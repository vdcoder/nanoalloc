// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// nanoalloc::Pool: an arena-backed allocator with a stable, algorithm-neutral
// interface. The allocation engine behind this interface is an implementation
// detail and is expected to change between versions.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nanoalloc/config.hpp"

namespace nanoalloc {

/// Parameters for constructing a Pool.
struct PoolConfig {
    /// Size of the memory arena in bytes. Must be non-zero and no greater than
    /// Pool::max_capacity. It is rounded up to a multiple of
    /// Pool::granularity. The whole arena is reserved when the Pool is
    /// constructed; the pool never grows and never falls back to another
    /// allocator.
    std::size_t capacity_bytes = 0;

    /// Record the requested size of every live allocation so that PoolStats
    /// can report requested bytes (and therefore payload slack) exactly.
    /// deallocate() does not receive a size, so this needs a side table.
    ///
    /// Cost: one byte of bookkeeping per Pool::granularity bytes of arena
    /// capacity (reserved at construction, reported as
    /// PoolStats::tracking_bytes) and one extra memory access, often a cache
    /// miss, per allocation and deallocation. It is off by default because the
    /// benchmarks show this cost is significant relative to the allocation
    /// path itself. When disabled, the *_requested_bytes statistics are zero
    /// and PoolStats::requested_bytes_tracked is false; all other behavior is
    /// identical.
    bool track_requested_bytes = false;
};

/// A snapshot of pool statistics. All byte counts are in bytes.
///
/// Terminology used throughout nanoalloc:
///   requested: the `bytes` argument the caller passed to allocate().
///   usable:    the payload capacity actually granted, i.e. usable_size().
///              The caller may use all of it. usable >= requested.
///   consumed:  arena bytes made unavailable by a live allocation, including
///              any per-allocation overhead stored inside the arena.
///              consumed >= usable.
///   reserved:  memory obtained from the system for the pool: the arena plus
///              the bookkeeping structures (metadata_bytes, tracking_bytes).
///
/// "Payload slack" is usable - requested; "fragmentation" is free arena
/// memory that cannot satisfy a request because it is not contiguous.
struct PoolStats {
    // --- Reservation -------------------------------------------------------
    /// Size of the arena managed by the pool (PoolConfig::capacity_bytes
    /// rounded up to Pool::granularity).
    std::size_t capacity_bytes = 0;
    /// Bookkeeping memory required by the allocation engine, held outside the
    /// arena (allocated once at construction, never on the allocation path).
    std::size_t metadata_bytes = 0;
    /// Bookkeeping memory used only to track requested sizes (zero when
    /// PoolConfig::track_requested_bytes is false).
    std::size_t tracking_bytes = 0;

    // --- Live usage (current values) --------------------------------------
    /// Number of allocations that have been returned and not yet released.
    std::size_t live_allocations = 0;
    /// Sum of the requested sizes of live allocations (0 unless
    /// PoolConfig::track_requested_bytes is set).
    std::size_t live_requested_bytes = 0;
    /// Sum of usable_size() over live allocations.
    std::size_t live_usable_bytes = 0;
    /// Arena bytes consumed by live allocations (see terminology above).
    std::size_t live_consumed_bytes = 0;

    // --- High-water marks since construction -------------------------------
    // Each peak is tracked independently, so the peaks may have been reached
    // at different moments.
    std::size_t peak_requested_bytes = 0;
    std::size_t peak_usable_bytes = 0;
    std::size_t peak_consumed_bytes = 0;

    // --- Availability -------------------------------------------------------
    /// capacity_bytes - live_consumed_bytes. Not necessarily contiguous.
    std::size_t free_bytes = 0;
    /// The largest request that is guaranteed to succeed right now: any
    /// allocate(bytes, alignment) with bytes <= largest_free_block and
    /// alignment <= Pool::min_alignment succeeds. Zero when the pool is full.
    std::size_t largest_free_block = 0;

    // --- Event counters since construction ----------------------------------
    /// Successful allocations.
    std::uint64_t allocation_count = 0;
    /// Deallocations of non-null pointers.
    std::uint64_t deallocation_count = 0;
    /// allocate()/try_allocate() calls that did not return memory.
    std::uint64_t failed_allocation_count = 0;
    /// Times a free block was divided to serve a smaller request.
    std::uint64_t split_count = 0;
    /// Times adjacent free blocks were coalesced into a larger block.
    std::uint64_t merge_count = 0;

    /// True when requested sizes are tracked (PoolConfig::track_requested_bytes).
    bool requested_bytes_tracked = false;
};

/// An arena-backed memory pool.
///
/// The pool reserves one contiguous arena at construction and serves every
/// allocation from it. It never calls the system allocator on the
/// allocate/deallocate path and never falls back to it when the arena is
/// exhausted: exhaustion is reported as std::bad_alloc (allocate) or nullptr
/// (try_allocate).
///
/// Guarantees:
///   * Every returned pointer is aligned to at least min_alignment (16) and to
///     the requested alignment (any power of two up to max_alignment).
///   * Live allocations never overlap, including their full usable_size().
///   * Deallocation does not need the allocation size.
///   * Once every allocation has been released, the pool's free capacity is
///     fully restored (free_bytes == capacity_bytes, and largest_free_block
///     returns to its initial value).
///
/// Thread safety: a Pool is NOT thread-safe. Concurrent calls on the same Pool
/// (including const member functions concurrently with mutating ones) are
/// data races. Distinct Pool objects may be used from different threads.
///
/// Lifetime: the Pool must outlive every allocation made from it and every
/// adapter (MemoryResource, Allocator) and container that refers to it.
/// Destroying the Pool releases the whole arena at once; it does not run
/// destructors of objects still living in it.
///
/// Misuse: passing deallocate() or usable_size() a pointer that this pool did
/// not return, or that was already released, is undefined behavior. The pool
/// detects common cases cheaply (foreign pointers, pointers into the middle of
/// a block, double frees of blocks that have not been reused) and terminates
/// the process with a diagnostic rather than corrupting its state.
class NANOALLOC_API Pool final {
public:
    /// Minimum alignment of every returned pointer.
    static constexpr std::size_t min_alignment = 16;
    /// Largest supported alignment. Larger alignments are rejected with
    /// std::bad_alloc (or nullptr from try_allocate).
    static constexpr std::size_t max_alignment = 4096;
    /// The arena capacity is rounded up to a multiple of this value.
    static constexpr std::size_t granularity = 16;
    /// Largest accepted PoolConfig::capacity_bytes.
    static constexpr std::size_t max_capacity = std::size_t{1}
                                                << (sizeof(std::size_t) * 8 - 2);

    /// Reserves the arena and the engine's bookkeeping.
    /// Throws std::invalid_argument if capacity_bytes is zero or larger than
    /// max_capacity, and std::bad_alloc if the memory cannot be obtained.
    explicit Pool(const PoolConfig& config);

    /// Releases the arena. Outstanding allocations become invalid.
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
    /// two, and std::bad_alloc if the request cannot be satisfied (arena
    /// exhausted or too fragmented, request larger than the arena, or
    /// alignment greater than max_alignment). On failure the pool is
    /// unchanged apart from PoolStats::failed_allocation_count.
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = min_alignment);

    /// Like allocate(), but returns nullptr instead of throwing for any
    /// failure, including an invalid alignment.
    [[nodiscard]] void* try_allocate(
        std::size_t bytes, std::size_t alignment = min_alignment) noexcept;

    /// Releases an allocation. `ptr` must be nullptr (no effect) or a live
    /// pointer returned by allocate()/try_allocate() of this pool.
    void deallocate(void* ptr) noexcept;

    /// Number of bytes usable at `ptr` (at least the requested size). The
    /// caller may read and write all of them. Returns 0 for nullptr.
    /// `ptr` must otherwise be a live allocation of this pool.
    [[nodiscard]] std::size_t usable_size(const void* ptr) const noexcept;

    /// True if `ptr` points into this pool's arena. This is an address-range
    /// check only; it does not tell whether `ptr` is a live allocation.
    [[nodiscard]] bool owns(const void* ptr) const noexcept;

    /// Arena capacity in bytes (same as stats().capacity_bytes).
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// Returns a snapshot of the pool statistics. O(1).
    [[nodiscard]] PoolStats stats() const noexcept;

    /// Checks the internal data structures for consistency. Runs in time
    /// proportional to the arena capacity; intended for tests and debugging.
    [[nodiscard]] bool verify_integrity() const noexcept;

private:
    struct Impl;
    // Owned. A plain pointer keeps standard-library types out of the class
    // layout of an exported type.
    Impl* impl_;
};

}  // namespace nanoalloc
