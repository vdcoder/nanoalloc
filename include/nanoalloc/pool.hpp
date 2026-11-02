// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// nanoalloc::Pool: a thread-capable, capacity-bounded memory pool.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nanoalloc/config.hpp"

namespace nanoalloc {

/// Parameters for constructing a Pool.
struct PoolConfig {
    /// Memory budget of the pool in bytes. Must be non-zero and no greater
    /// than Pool::max_capacity; it is rounded up to a multiple of
    /// Pool::granularity.
    ///
    /// The pool reserves an arena of this size at construction (the operating
    /// system typically commits its pages lazily) and carves 1 MiB spans and
    /// per-thread bookkeeping from it on demand. Requests too large for a span
    /// are served by the system allocator and are charged against the same
    /// budget. The pool never exceeds the budget and never falls back to
    /// another allocator: exhaustion is reported as an allocation failure.
    std::size_t capacity_bytes = 0;

    /// Report requested-byte statistics. Every block header records its
    /// payload slack, so tracking costs no memory and no time; set this to
    /// false to report the *_requested_bytes fields as zero, as earlier
    /// versions did when tracking was disabled.
    bool track_requested_bytes = true;
};

/// A snapshot of pool statistics. All byte counts are in bytes.
///
/// Terminology:
///   requested: the `bytes` argument the caller passed to allocate().
///   usable:    the payload capacity granted, i.e. usable_size(). For blocks
///              served from spans this is `bytes` rounded up to a multiple of
///              16, so usable - requested (the payload slack) is at most 15.
///   consumed:  memory made unavailable by a live allocation: usable bytes
///              plus the 16-byte block header (span blocks), or the whole
///              system allocation (oversized blocks).
///   reserved:  memory carved from the arena (spans and thread heaps) plus
///              the system memory of live oversized allocations.
///
/// Accuracy: every counter is maintained by the thread that owns it and read
/// with relaxed atomic loads. Values are exact when no other thread is using
/// the pool during the call; under concurrent use each value is a recent
/// value, but different fields may come from slightly different moments.
/// Peak values are the sum of per-thread peaks: exact when one thread uses the
/// pool, an upper bound of the true combined peak otherwise.
struct PoolStats {
    // --- Budget and reservation ----------------------------------------------
    /// The pool's memory budget (PoolConfig::capacity_bytes rounded up).
    std::size_t capacity_bytes = 0;
    /// Bookkeeping memory: the pool object, per-thread heaps, and span headers
    /// and fences. Excludes the per-allocation headers (header_bytes).
    std::size_t metadata_bytes = 0;
    /// Always 0: requested sizes are recorded in the block headers.
    std::size_t tracking_bytes = 0;
    /// Arena bytes carved so far (spans and thread heaps) plus the system
    /// memory of live oversized allocations.
    std::size_t reserved_bytes = 0;
    /// Number and total size of spans carved from the arena (owned by thread
    /// heaps or kept for reuse).
    std::size_t span_count = 0;
    std::size_t span_bytes = 0;
    /// Number of per-thread heaps created for this pool.
    std::size_t thread_heap_count = 0;

    // --- Live usage (current values) ------------------------------------------
    /// Allocations not yet returned to free memory, including blocks released
    /// by other threads that their owner has not drained yet.
    std::size_t live_allocations = 0;
    /// Sum of the requested sizes of live allocations (0 when
    /// PoolConfig::track_requested_bytes is false).
    std::size_t live_requested_bytes = 0;
    /// Sum of usable_size() over live allocations.
    std::size_t live_usable_bytes = 0;
    /// Memory consumed by live allocations (see terminology above).
    std::size_t live_consumed_bytes = 0;
    /// Per-allocation overhead of live allocations: 16-byte block headers, and
    /// header plus alignment padding of oversized allocations.
    std::size_t header_bytes = 0;
    /// Remote deallocations waiting in their owner's inbox.
    std::size_t pending_remote_frees = 0;
    /// Free 16-byte blocks: a header without payload, left between two
    /// allocations when an exact split leaves 16 bytes. They are merged back
    /// as soon as a neighbouring block is released.
    std::size_t fragment_bytes = 0;
    /// Live allocations served by the system allocator because they do not
    /// fit in a span, and their payload bytes.
    std::size_t oversized_live_allocations = 0;
    std::size_t oversized_live_bytes = 0;

    // --- High-water marks -------------------------------------------------------
    // Each peak is tracked independently (see Accuracy above).
    std::size_t peak_requested_bytes = 0;
    std::size_t peak_usable_bytes = 0;
    std::size_t peak_consumed_bytes = 0;

    // --- Availability -------------------------------------------------------------
    /// capacity_bytes - live_consumed_bytes. Not necessarily contiguous, and
    /// it includes bookkeeping memory and free memory held in other threads'
    /// spans, which the calling thread cannot use.
    std::size_t free_bytes = 0;
    /// The largest request that the calling thread is guaranteed to be able to
    /// allocate from a span right now (assuming no concurrent use): any
    /// allocate(bytes, alignment) with bytes <= largest_free_block and
    /// alignment <= Pool::min_alignment succeeds. Larger requests may still
    /// succeed as oversized allocations.
    std::size_t largest_free_block = 0;

    // --- Event counters since construction -----------------------------------------
    /// Successful allocations (span and oversized).
    std::uint64_t allocation_count = 0;
    /// deallocate() calls with a non-null pointer (local, remote and
    /// oversized), counted when the call is made.
    std::uint64_t deallocation_count = 0;
    /// Deallocations of blocks owned by the calling thread, and of blocks
    /// owned by another thread (sent to that thread's inbox).
    std::uint64_t local_deallocation_count = 0;
    std::uint64_t remote_deallocation_count = 0;
    /// Oversized allocations made since construction.
    std::uint64_t oversized_allocation_count = 0;
    /// allocate()/try_allocate() calls that did not return memory.
    std::uint64_t failed_allocation_count = 0;
    /// Times a free block was divided to serve a request.
    std::uint64_t split_count = 0;
    /// Times adjacent free blocks were coalesced.
    std::uint64_t merge_count = 0;

    /// True when the requested-byte fields are reported.
    bool requested_bytes_tracked = false;
};

/// A memory pool with per-thread allocation state.
///
/// Every thread that allocates from a pool gets its own heap: a set of 1 MiB
/// spans carved from the pool's arena, whose free blocks are indexed by size
/// buckets. A thread allocates and frees its own blocks without locks. A
/// block freed by another thread is queued, lock-free, in its owner's inbox
/// and released by the owner at its next allocation or deallocation in this
/// pool, or when the owner calls drain_pending_frees().
///
/// Guarantees:
///   * Every returned pointer is aligned to at least min_alignment (16) and to
///     the requested alignment (any power of two up to max_alignment).
///   * Blocks served from spans have exactly `bytes` rounded up to a multiple
///     of 16 usable bytes (at most 15 bytes of slack) plus a 16-byte header.
///   * Live allocations never overlap, including their full usable_size().
///   * Deallocation does not need the allocation size.
///   * Allocation and deallocation never call the system allocator, except
///     for oversized requests (see PoolConfig::capacity_bytes).
///   * In single-threaded use, once every allocation has been released and
///     drained, free_bytes == capacity_bytes and largest_free_block returns
///     to its initial value.
///
/// Thread safety: allocate(), try_allocate(), deallocate(), usable_size(),
/// owns(), capacity(), stats() and drain_pending_frees() may be called
/// concurrently from any threads. verify_integrity() requires that no other
/// thread uses the pool during the call.
///
/// Thread lifetime contract: memory allocated by a thread must not be accessed
/// or deallocated by another thread after the allocating thread has begun to
/// terminate. All cross-thread deallocations of its blocks must complete, with
/// appropriate synchronization (for example, joining the freeing threads),
/// before it exits. At exit a thread drains its inbox and returns its
/// completely free spans to the pool for reuse by other threads; spans that
/// still hold live blocks stay reserved until the pool is destroyed.
///
/// Lifetime: the Pool must outlive every allocation made from it and every
/// adapter (MemoryResource, Allocator) and container that refers to it.
/// Destroying the Pool releases all its memory at once, including oversized
/// allocations; it does not run destructors of objects still living in it.
///
/// Misuse: passing deallocate() or usable_size() a pointer that this pool did
/// not return, or that was already released, is undefined behavior. The pool
/// detects common cases (misaligned pointers, pointers that do not start a
/// block, blocks of another pool, double frees of blocks that have not been
/// reused, frees after the owning thread exited) and terminates the process
/// with a diagnostic rather than corrupting its state.
class NANOALLOC_API Pool final {
public:
    /// Minimum alignment of every returned pointer.
    static constexpr std::size_t min_alignment = 16;
    /// Largest supported alignment. Larger alignments are rejected with
    /// std::bad_alloc (or nullptr from try_allocate).
    static constexpr std::size_t max_alignment = 4096;
    /// Payload sizes and the capacity are rounded up to a multiple of this.
    static constexpr std::size_t granularity = 16;
    /// Size of the header stored before every span block.
    static constexpr std::size_t header_size = 16;
    /// Largest accepted PoolConfig::capacity_bytes.
    static constexpr std::size_t max_capacity = std::size_t{1}
                                                << (sizeof(std::size_t) * 8 - 2);

    /// Reserves the arena.
    /// Throws std::invalid_argument if capacity_bytes is zero or larger than
    /// max_capacity, and std::bad_alloc if the memory cannot be obtained.
    explicit Pool(const PoolConfig& config);

    /// Releases all memory of the pool. Outstanding allocations become invalid.
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    // Adapters store a pointer to the Pool, so it is pinned in memory. Use
    // std::unique_ptr<Pool> if a movable handle is needed.
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    /// Allocates at least `bytes` bytes aligned to `alignment`.
    ///
    /// * `bytes == 0` is valid and returns a unique, non-null pointer that
    ///   must be released with deallocate() like any other allocation.
    /// * `alignment` must be a power of two; values below min_alignment are
    ///   raised to min_alignment.
    ///
    /// Throws std::invalid_argument if `alignment` is zero or not a power of
    /// two, and std::bad_alloc if the request cannot be satisfied (budget
    /// exhausted, request larger than the budget, or alignment greater than
    /// max_alignment). On failure the pool is unchanged apart from
    /// PoolStats::failed_allocation_count and possibly a newly created,
    /// empty per-thread heap.
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = min_alignment);

    /// Like allocate(), but returns nullptr instead of throwing for any
    /// failure, including an invalid alignment.
    [[nodiscard]] void* try_allocate(
        std::size_t bytes, std::size_t alignment = min_alignment) noexcept;

    /// Releases an allocation. `ptr` must be nullptr (no effect) or a live
    /// pointer returned by allocate()/try_allocate() of this pool. Any thread
    /// may release any block (subject to the thread lifetime contract).
    void deallocate(void* ptr) noexcept;

    /// Releases the blocks that other threads have deallocated on behalf of
    /// the calling thread. Allocation and deallocation do this automatically;
    /// call it to return memory promptly when the thread is otherwise idle.
    void drain_pending_frees() noexcept;

    /// Number of bytes usable at `ptr` (at least the requested size). The
    /// caller may read and write all of them. Returns 0 for nullptr.
    /// `ptr` must otherwise be a live allocation of this pool.
    [[nodiscard]] std::size_t usable_size(const void* ptr) const noexcept;

    /// True if `ptr` points into this pool's arena or into a live oversized
    /// allocation of this pool. It does not tell whether `ptr` is a live
    /// allocation.
    [[nodiscard]] bool owns(const void* ptr) const noexcept;

    /// The pool's memory budget in bytes (same as stats().capacity_bytes).
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// Returns a snapshot of the pool statistics. Takes the pool's internal
    /// lock and visits every thread heap; not intended for hot paths.
    [[nodiscard]] PoolStats stats() const noexcept;

    /// Checks the internal data structures for consistency. Runs in time
    /// proportional to the reserved memory; intended for tests and debugging.
    /// No other thread may use the pool during the call.
    [[nodiscard]] bool verify_integrity() const noexcept;

private:
    struct Impl;
    // Owned. A plain pointer keeps standard-library types out of the class
    // layout of an exported type.
    Impl* impl_;
};

}  // namespace nanoalloc
