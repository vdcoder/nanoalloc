// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Basic use of nanoalloc::Pool: raw allocation, alignment, sizes and
// statistics.
#include <nanoalloc/pool.hpp>

#include <cstdio>
#include <cstring>
#include <new>

int main() {
    // Reserve a 1 MiB arena. All allocations below are served from it.
    // Requested-size tracking is optional; it makes stats() report the bytes
    // callers asked for, not only the bytes they were given.
    nanoalloc::PoolConfig config;
    config.capacity_bytes = 1024 * 1024;
    config.track_requested_bytes = true;
    nanoalloc::Pool pool(config);

    // Allocate 100 bytes (16-byte aligned by default) and use them.
    void* message = pool.allocate(100);
    const char text[] = "hello from nanoalloc";
    std::memcpy(message, text, sizeof(text));

    // Request a stronger alignment, e.g. for SIMD or cache-line ownership.
    void* line = pool.allocate(64, 64);

    // The pool knows each block's size: deallocation takes only the pointer.
    std::printf("%s\n", static_cast<const char*>(message));
    std::printf("requested 100 bytes, usable %zu bytes\n", pool.usable_size(message));

    const nanoalloc::PoolStats stats = pool.stats();
    std::printf("live allocations: %zu\n", stats.live_allocations);
    std::printf("requested bytes:  %zu\n", stats.live_requested_bytes);
    std::printf("usable bytes:     %zu\n", stats.live_usable_bytes);
    std::printf("free bytes:       %zu of %zu\n", stats.free_bytes, stats.capacity_bytes);
    std::printf("metadata bytes:   %zu\n", stats.metadata_bytes);

    pool.deallocate(line);
    pool.deallocate(message);

    // Exhaustion is reported, never silently redirected to another allocator.
    try {
        void* too_big = pool.allocate(2 * 1024 * 1024);
        pool.deallocate(too_big);
    } catch (const std::bad_alloc&) {
        std::printf("2 MiB request: std::bad_alloc (arena is 1 MiB)\n");
    }

    // try_allocate() reports failure with nullptr instead of an exception.
    if (pool.try_allocate(2 * 1024 * 1024) == nullptr) {
        std::printf("try_allocate: nullptr\n");
    }

    return pool.stats().live_allocations == 0 ? 0 : 1;
}
