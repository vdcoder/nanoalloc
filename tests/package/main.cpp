// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Uses an installed nanoalloc package: pmr containers, a cross-thread free
// and the statistics. Returns non-zero on failure.
#include <nanoalloc/nanoalloc.hpp>

#include <cstdio>
#include <memory_resource>
#include <thread>
#include <vector>

int main() {
    nanoalloc::Pool pool({4 * 1024 * 1024});
    nanoalloc::MemoryResource resource(pool);
    std::pmr::vector<int> values(&resource);
    for (int i = 0; i < 1000; ++i) {
        values.push_back(i);
    }
    void* shared = pool.allocate(100);
    std::thread other([&] { pool.deallocate(shared); });
    other.join();
    pool.drain_pending_frees();
    const nanoalloc::PoolStats stats = pool.stats();
    const bool ok = stats.live_allocations == 1 && stats.remote_deallocation_count == 1 &&
                    stats.reserved_bytes <= stats.capacity_bytes && pool.verify_integrity();
    std::printf("nanoalloc %s package test: %s\n", nanoalloc::version_string,
                ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
