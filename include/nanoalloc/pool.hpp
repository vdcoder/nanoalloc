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
    /// The pool allocates an arena of this size from the system at
    /// construction (most operating systems commit its pages only when they
    /// are first touched) and carves per-thread heaps and 1 MiB spans from it
    /// on demand. Requests too large for a span ("oversized") are served by
    /// separate system allocations.
    ///
    /// Everything the pool hands out is charged against the budget: per-thread
    /// heaps, spans and oversized allocations together never exceed it
    /// (PoolStats::reserved_bytes <= capacity_bytes), and a request that would
    /// exceed it fails instead of falling back to another allocator. The
    /// budget is not a bound on the memory obtained from the system: the arena
    /// is allocated in full up front and oversized allocations come on top of
    /// it, so PoolStats::system_reserved_bytes can approach twice the budget
    /// when oversized allocations use the part of the budget that spans have
    /// not taken.
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
///   reserved:  charged against the budget (PoolConfig::capacity_bytes).
///
/// Every reserved byte is in exactly one place. When no other thread uses the
/// pool during the call:
///
///   reserved_bytes == metadata_bytes + live_consumed_bytes
///                     + thread_free_bytes + free_span_bytes
///   reserved_bytes <= capacity_bytes
///   allocation_count == deallocation_count + live_allocations
///                       - pending_remote_frees
///
/// Accuracy: every counter is maintained by the thread that owns it and read
/// with relaxed atomic loads. Values are exact when no other thread is using
/// the pool during the call; under concurrent use each value is a recent
/// value, but different fields may come from slightly different moments.
///
/// History: counters and peaks cover the whole life of the pool. A per-thread
/// heap left behind by an exited thread is reused by the next new thread
/// together with its counters, so no statistic is lost when threads come and
/// go.
struct PoolStats {
    // --- Budget and reservation ----------------------------------------------
    /// The pool's memory budget (PoolConfig::capacity_bytes rounded up).
    std::size_t capacity_bytes = 0;
    /// Budget charged so far: arena bytes carved for per-thread heaps and
    /// spans, plus the system memory of live oversized allocations. Never
    /// exceeds capacity_bytes. Carved spans stay charged (owned by a thread or
    /// kept in the pool's free-span list) until the pool is destroyed.
    std::size_t reserved_bytes = 0;
    /// Memory currently obtained from the system for this pool: the whole
    /// arena (capacity_bytes, allocated at construction), the pool object and
    /// the system allocations of live oversized blocks. Excludes the system
    /// allocator's own overhead and does not tell how much is committed.
    std::size_t system_reserved_bytes = 0;
    /// Bookkeeping memory carved from the arena: per-thread heap objects and
    /// the headers and fences of the spans that thread heaps own. Excludes the
    /// per-allocation headers (header_bytes) and the pool object.
    std::size_t metadata_bytes = 0;
    /// Always 0: requested sizes are recorded in the block headers.
    std::size_t tracking_bytes = 0;
    /// Number and total size of spans carved from the arena, whether owned by
    /// a thread heap or kept for reuse.
    std::size_t span_count = 0;
    std::size_t span_bytes = 0;
    /// Spans that no thread owns: returned, completely free, by exited
    /// threads. Any thread that needs a span takes one of these before a new
    /// span is carved.
    std::size_t free_span_count = 0;
    std::size_t free_span_bytes = 0;
    /// Per-thread heaps ever carved for this pool; those owned by a running
    /// thread; and retired heaps: heaps of exited threads that still hold
    /// allocated blocks (or frees not processed yet), managed by the
    /// reclamation worker. Heaps that hold no memory are reused by new threads.
    std::size_t thread_heap_count = 0;
    std::size_t active_thread_heap_count = 0;
    std::size_t retired_thread_heap_count = 0;

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
    /// Blocks released by other threads that are waiting in their owner's
    /// inbox: sent and not yet drained, counted by the owning heap.
    std::size_t pending_remote_frees = 0;
    /// Free 16-byte blocks: a header without payload, left between two
    /// allocations when an exact split leaves 16 bytes. They are merged back
    /// as soon as a neighbouring block is released. Part of thread_free_bytes.
    std::size_t fragment_bytes = 0;
    /// Live allocations served by separate system allocations because they
    /// do not fit in a span: their number, payload bytes, and system bytes
    /// (payload, header and alignment padding; charged against the budget).
    std::size_t oversized_live_allocations = 0;
    std::size_t oversized_live_bytes = 0;
    std::size_t oversized_reserved_bytes = 0;

    // --- High-water marks -------------------------------------------------------
    // Each peak is tracked per thread heap, plus one for oversized blocks, and
    // the reported value is their sum: exact when a single thread allocates
    // from spans only, otherwise an upper bound of the true combined peak
    // (heaps may have peaked at different moments). Peaks never decrease.
    std::size_t peak_requested_bytes = 0;
    std::size_t peak_usable_bytes = 0;
    std::size_t peak_consumed_bytes = 0;

    // --- Free memory ----------------------------------------------------------------
    /// capacity_bytes - live_consumed_bytes: the part of the budget that live
    /// allocations do not consume. A utilization figure, not an amount the
    /// caller can allocate: it includes bookkeeping memory, free memory in
    /// other threads' spans and fragmentation. Use largest_free_block for a
    /// guarantee.
    std::size_t free_bytes = 0;
    /// Bytes of free blocks (headers included) inside spans owned by thread
    /// heaps. Only the owning thread allocates from them; memory freed into a
    /// thread's spans is not available to other threads while it runs. Spans
    /// that become completely free return to the pool when the thread exits.
    std::size_t thread_free_bytes = 0;
    /// The largest request that the calling thread is guaranteed to be able to
    /// allocate from a span right now (assuming no concurrent use): any
    /// allocate(bytes, alignment) with bytes <= largest_free_block and
    /// alignment <= Pool::min_alignment succeeds. Larger requests may still
    /// succeed as oversized allocations.
    std::size_t largest_free_block = 0;

    // --- Event counters since construction -----------------------------------------
    /// Successful allocations (span and oversized).
    std::uint64_t allocation_count = 0;
    /// deallocate() calls with a non-null pointer: local, remote and
    /// oversized, counted when the call is made.
    std::uint64_t deallocation_count = 0;
    /// Deallocations of span blocks by the thread that allocated them.
    std::uint64_t local_deallocation_count = 0;
    /// Deallocations of span blocks by another thread (sent to the owner's
    /// inbox). Counted once, by the receiving heap.
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
///     for oversized requests, which are still charged against the budget
///     (see PoolConfig::capacity_bytes).
///   * In single-threaded use, once every allocation has been released and
///     drained, free_bytes == capacity_bytes and largest_free_block returns
///     to its initial value.
///
/// Thread safety: allocate(), try_allocate(), deallocate(), usable_size(),
/// owns(), capacity(), stats() and drain_pending_frees() may be called
/// concurrently from any threads. verify_integrity() requires that no other
/// thread uses the pool during the call.
///
/// Thread lifetime: allocations may outlive the thread that made them. A
/// block stays valid until it is deallocated or the pool is destroyed, and
/// any thread may access or deallocate it, before or after the allocating
/// thread has exited (with the synchronization any shared object needs).
///
/// When a thread that used the pool terminates (when its thread_local objects
/// are destroyed), the pool releases the frees pending in its per-thread heap
/// and returns the heap's completely free spans to the pool. Then:
///   * A heap that still holds allocated blocks is retired: it is handed to a
///     process-wide reclamation worker. Frees of its blocks by any thread go
///     to its inbox, as other remote frees do; the worker processes them
///     asynchronously (typically within milliseconds), returns spans that
///     become empty to the pool for other threads, and makes the heap
///     reusable once it holds no block.
///   * The terminating thread may not allocate any more: in thread_local
///     destructors that run after this point, allocate() throws
///     std::bad_alloc and try_allocate() returns nullptr, for every pool.
///     Its deallocations are accepted and go through the owners' inboxes.
///   * A heap that holds no block is reused by the next thread that uses the
///     pool, together with its statistics.
/// Until the worker has processed them, frees of a retired heap's blocks
/// count as pending (PoolStats::pending_remote_frees) and their memory as
/// live.
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
/// reused) and
/// terminates the process with a diagnostic rather than corrupting its state.
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
    /// may release any block, also after the thread that allocated it has
    /// exited (see Thread lifetime above).
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
    /// No other thread may use the pool during the call; the call waits until
    /// the reclamation worker is between passes and keeps it waiting.
    [[nodiscard]] bool verify_integrity() const noexcept;

private:
    struct Impl;
    // Owned. A plain pointer keeps standard-library types out of the class
    // layout of an exported type.
    Impl* impl_;
};

}  // namespace nanoalloc
