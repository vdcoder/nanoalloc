// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Checks the relations between PoolStats fields that the public header
// documents. They are exact only when no other thread uses the pool while
// the snapshot is taken.
#pragma once

#include <nanoalloc/pool.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace nanotest {

/// Returns true if `s` satisfies every documented identity; prints each
/// violated identity to stderr.
inline bool stats_are_consistent(const nanoalloc::PoolStats& s) {
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "  inconsistent PoolStats: %s\n", what);
            ok = false;
        }
    };
    // Budget and where every charged byte is.
    expect(s.reserved_bytes <= s.capacity_bytes, "reserved_bytes <= capacity_bytes");
    expect(s.reserved_bytes == s.metadata_bytes + s.live_consumed_bytes + s.thread_free_bytes +
                                   s.free_span_bytes,
           "reserved == metadata + live consumed + thread free + free spans");
    expect(s.system_reserved_bytes >= s.capacity_bytes + s.oversized_reserved_bytes,
           "system_reserved_bytes covers the arena and oversized blocks");
    expect(s.free_bytes == s.capacity_bytes - s.live_consumed_bytes,
           "free_bytes == capacity - live consumed");
    expect(s.free_span_count <= s.span_count && s.free_span_bytes <= s.span_bytes,
           "free spans are a subset of the carved spans");
    expect(s.active_thread_heap_count + s.retired_thread_heap_count <= s.thread_heap_count,
           "active + retired heaps <= heaps");
    expect(s.fragment_bytes <= s.thread_free_bytes, "fragments are part of thread free bytes");

    // Live usage.
    expect(s.live_consumed_bytes == s.live_usable_bytes + s.header_bytes,
           "live consumed == live usable + header bytes");
    expect(s.oversized_live_bytes <= s.oversized_reserved_bytes &&
               s.oversized_live_allocations <= s.live_allocations,
           "oversized blocks are part of the live blocks");
    expect(s.pending_remote_frees <= s.live_allocations, "pending frees are live until drained");
    if (s.requested_bytes_tracked) {
        expect(s.live_requested_bytes <= s.live_usable_bytes &&
                   s.live_usable_bytes - s.live_requested_bytes <= 15 * s.live_allocations,
               "at most 15 bytes of slack per live allocation");
        expect(s.peak_requested_bytes >= s.live_requested_bytes, "peak requested >= live");
    } else {
        expect(s.live_requested_bytes == 0 && s.peak_requested_bytes == 0,
               "requested bytes are zero when not tracked");
    }
    expect(s.peak_usable_bytes >= s.live_usable_bytes, "peak usable >= live");
    expect(s.peak_consumed_bytes >= s.live_consumed_bytes, "peak consumed >= live");

    // Event counters.
    expect(s.oversized_allocation_count >= s.oversized_live_allocations,
           "oversized allocations >= live oversized blocks");
    expect(s.deallocation_count == s.local_deallocation_count + s.remote_deallocation_count +
                                       (s.oversized_allocation_count -
                                        s.oversized_live_allocations),
           "deallocations == local + remote + oversized frees");
    expect(s.allocation_count ==
               s.deallocation_count + s.live_allocations - s.pending_remote_frees,
           "allocations == deallocations + live - pending");
    return ok;
}

/// Waits until the reclamation worker has processed every retired heap of
/// `pool` (none left, no pending frees) and returns the final snapshot in
/// `out`. Returns false after `timeout`.
inline bool wait_for_reclamation(const nanoalloc::Pool& pool, nanoalloc::PoolStats& out,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        out = pool.stats();
        if (out.retired_thread_heap_count == 0 && out.pending_remote_frees == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "  reclamation timed out: %zu retired heaps, %zu pending frees\n",
                         out.retired_thread_heap_count, out.pending_remote_frees);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// Waits until `pool` has no pending remote frees (the reclamation worker
/// has processed the frees of retired heaps) without requiring the retired
/// heaps themselves to be gone.
inline bool wait_for_no_pending(const nanoalloc::Pool& pool, nanoalloc::PoolStats& out,
                                std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        out = pool.stats();
        if (out.pending_remote_frees == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// True if no counter or peak of `later` is below its value in `earlier`.
inline bool history_is_monotonic(const nanoalloc::PoolStats& earlier,
                                 const nanoalloc::PoolStats& later) {
    return later.allocation_count >= earlier.allocation_count &&
           later.deallocation_count >= earlier.deallocation_count &&
           later.local_deallocation_count >= earlier.local_deallocation_count &&
           later.remote_deallocation_count >= earlier.remote_deallocation_count &&
           later.oversized_allocation_count >= earlier.oversized_allocation_count &&
           later.failed_allocation_count >= earlier.failed_allocation_count &&
           later.split_count >= earlier.split_count && later.merge_count >= earlier.merge_count &&
           later.peak_requested_bytes >= earlier.peak_requested_bytes &&
           later.peak_usable_bytes >= earlier.peak_usable_bytes &&
           later.peak_consumed_bytes >= earlier.peak_consumed_bytes &&
           later.thread_heap_count >= earlier.thread_heap_count &&
           later.span_count >= earlier.span_count;
}

}  // namespace nanotest
