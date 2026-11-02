// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Using a nanoalloc::Pool with standard containers, through std::pmr and
// through the nanoalloc::Allocator<T> template.
#include <nanoalloc/allocator.hpp>
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <cstdio>
#include <map>
#include <memory_resource>
#include <string>
#include <vector>

int main() {
    // The pool must outlive every container that allocates from it.
    nanoalloc::Pool pool({64 * 1024 * 1024});

    // --- std::pmr ---------------------------------------------------------
    {
        nanoalloc::MemoryResource resource(pool);

        std::pmr::vector<int> values(&resource);
        for (int i = 0; i < 1000; ++i) {
            values.push_back(i * i);
        }

        // Nested pmr containers pass the resource on to their elements.
        std::pmr::map<std::pmr::string, std::pmr::vector<int>> index(&resource);
        index["squares"].assign(values.begin(), values.begin() + 10);

        std::printf("pmr: %zu values, %zu live allocations in the pool\n", values.size(),
                    pool.stats().live_allocations);
    }

    // --- Standard allocator -------------------------------------------------
    {
        using IntAllocator = nanoalloc::Allocator<int>;
        std::vector<int, IntAllocator> values{IntAllocator(pool)};
        values.assign({3, 1, 4, 1, 5, 9, 2, 6});

        // Rebinding happens automatically for node-based containers.
        using Entry = std::pair<const int, double>;
        std::map<int, double, std::less<>, nanoalloc::Allocator<Entry>> weights{
            nanoalloc::Allocator<Entry>(pool)};
        weights[1] = 0.5;
        weights[2] = 0.25;

        std::printf("allocator: %zu values, %zu weights, %zu live allocations\n", values.size(),
                    weights.size(), pool.stats().live_allocations);
    }

    // Every container released its memory back to the pool.
    std::printf("after scope: %zu live allocations\n", pool.stats().live_allocations);
    return pool.stats().live_allocations == 0 ? 0 : 1;
}
