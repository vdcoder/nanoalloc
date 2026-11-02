// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Public contract tests for nanoalloc::MemoryResource (std::pmr integration).
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory_resource>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"

using nanoalloc::MemoryResource;
using nanoalloc::Pool;

namespace {

constexpr std::size_t kMiB = 1024 * 1024;

/// A non-trivial element type that counts constructions and destructions.
struct Tracked {
    static inline int alive = 0;
    explicit Tracked(int v) : value(v), text("tracked value " + std::to_string(v)) { ++alive; }
    Tracked(const Tracked& other) : value(other.value), text(other.text) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(other.value), text(std::move(other.text)) {
        ++alive;
    }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --alive; }
    int value;
    std::string text;
};

}  // namespace

NANO_TEST(pmr_vector_of_int) {
    Pool pool({64 * kMiB});
    MemoryResource resource(pool);
    {
        std::pmr::vector<int> values(&resource);
        for (int i = 0; i < 200000; ++i) {
            values.push_back(i);
        }
        CHECK(pool.owns(values.data()));
        CHECK(pool.stats().live_allocations >= 1);
        CHECK(pool.stats().live_usable_bytes >= values.size() * sizeof(int));
        for (int i = 0; i < 200000; ++i) {
            if (values[static_cast<std::size_t>(i)] != i) {
                REQUIRE(values[static_cast<std::size_t>(i)] == i);
            }
        }
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(pmr_strings_and_nested_containers_use_the_resource) {
    Pool pool({16 * kMiB});
    MemoryResource resource(pool);
    {
        std::pmr::vector<std::pmr::string> words(&resource);
        for (int i = 0; i < 1000; ++i) {
            words.emplace_back("a string that is long enough to need heap storage #" +
                               std::to_string(i));
        }
        // Uses-allocator construction propagates the resource to elements.
        for (const auto& word : words) {
            CHECK(word.get_allocator().resource() == &resource);
            CHECK(pool.owns(word.data()));
        }
        CHECK(pool.stats().live_allocations >= 1001);
        CHECK_EQ(words[999], std::pmr::string("a string that is long enough to need heap storage #999"));
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(pmr_node_based_containers) {
    Pool pool({16 * kMiB});
    MemoryResource resource(pool);
    {
        std::pmr::map<int, std::pmr::string> map(&resource);
        std::pmr::unordered_map<int, int> hash(&resource);
        std::pmr::list<double> list(&resource);
        std::pmr::deque<int> deque(&resource);
        for (int i = 0; i < 5000; ++i) {
            map.emplace(i, std::pmr::string(static_cast<std::size_t>(i % 100), 'x'));
            hash[i] = i * 2;
            list.push_back(i * 0.5);
            deque.push_front(i);
        }
        for (int i = 0; i < 5000; i += 2) {
            map.erase(i);
            hash.erase(i);
        }
        CHECK_EQ(map.size(), 2500U);
        CHECK_EQ(hash.size(), 2500U);
        CHECK_EQ(hash[4999], 9998);
        CHECK_EQ(map.at(4999).size(), 99U);
        CHECK_EQ(list.size(), 5000U);
        CHECK_EQ(deque.front(), 4999);
        CHECK(pool.verify_integrity());
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(pmr_nontrivial_objects_are_constructed_and_destroyed) {
    Pool pool({8 * kMiB});
    MemoryResource resource(pool);
    Tracked::alive = 0;
    {
        std::pmr::vector<Tracked> objects(&resource);
        for (int i = 0; i < 2000; ++i) {
            objects.emplace_back(i);
        }
        CHECK_EQ(Tracked::alive, 2000);
        objects.erase(objects.begin(), objects.begin() + 500);
        CHECK_EQ(Tracked::alive, 1500);
        CHECK_EQ(objects.front().value, 500);
        CHECK_EQ(objects.back().text, std::string("tracked value 1999"));
    }
    CHECK_EQ(Tracked::alive, 0);
    CHECK_EQ(pool.stats().live_allocations, 0U);

    std::pmr::polymorphic_allocator<> allocator(&resource);
    Tracked* single = allocator.new_object<Tracked>(42);
    CHECK(pool.owns(single));
    CHECK_EQ(single->value, 42);
    allocator.delete_object(single);
    CHECK_EQ(Tracked::alive, 0);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(pmr_direct_allocation_honors_alignment) {
    Pool pool({4 * kMiB});
    MemoryResource resource(pool);
    for (std::size_t alignment = 1; alignment <= Pool::max_alignment; alignment *= 2) {
        void* ptr = resource.allocate(100, alignment);
        CHECK(nanotest::is_aligned(ptr, alignment));
        CHECK(nanotest::is_aligned(ptr, Pool::min_alignment));
        resource.deallocate(ptr, 100, alignment);
    }
    void* zero = resource.allocate(0, 16);
    CHECK(zero != nullptr);
    resource.deallocate(zero, 0, 16);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(pmr_exhaustion_throws_bad_alloc) {
    Pool pool({64 * 1024});
    MemoryResource resource(pool);
    std::pmr::vector<char> buffer(&resource);
    CHECK_THROWS_AS(buffer.reserve(pool.capacity() + 1), std::bad_alloc);
    CHECK_THROWS_AS(resource.allocate(pool.capacity() * 4), std::bad_alloc);
    CHECK_THROWS_AS(resource.allocate(16, Pool::max_alignment * 2), std::bad_alloc);
    // The container is still usable after a failed growth.
    buffer.push_back('x');
    CHECK_EQ(buffer.size(), 1U);
    std::size_t pushed = 1;
    try {
        for (;;) {
            buffer.push_back('y');
            ++pushed;
        }
    } catch (const std::bad_alloc&) {
    }
    CHECK_EQ(buffer.size(), pushed);
    CHECK_EQ(buffer.front(), 'x');
    buffer.clear();
    buffer.shrink_to_fit();
    CHECK(pool.verify_integrity());
}

NANO_TEST(pmr_is_equal_reflects_the_underlying_pool) {
    Pool pool({1 * kMiB});
    Pool other_pool({1 * kMiB});
    MemoryResource first(pool);
    MemoryResource second(pool);
    MemoryResource third(other_pool);
    CHECK(first.is_equal(first));
    CHECK(first == second);
    CHECK(second.is_equal(first));
    CHECK(!(first == third));
    CHECK(!first.is_equal(*std::pmr::new_delete_resource()));
    CHECK(!std::pmr::new_delete_resource()->is_equal(first));
    CHECK(&first.pool() == &pool);

    // Memory allocated through one resource may be released through an equal one.
    void* ptr = first.allocate(128);
    second.deallocate(ptr, 128);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(pmr_move_between_equal_resources_steals_storage) {
    Pool pool({4 * kMiB});
    MemoryResource first(pool);
    MemoryResource second(pool);
    std::pmr::vector<int> source({1, 2, 3, 4, 5}, &first);
    const int* storage = source.data();
    std::pmr::vector<int> target(std::move(source), &second);
    CHECK(target.data() == storage);  // resources compare equal: no copy
    CHECK_EQ(target.size(), 5U);
}

NANO_TEST(pmr_upstream_for_standard_pool_resources) {
    Pool pool({16 * kMiB});
    MemoryResource resource(pool);
    {
        std::pmr::monotonic_buffer_resource arena(&resource);
        std::pmr::vector<std::pmr::string> words(&arena);
        for (int i = 0; i < 2000; ++i) {
            words.emplace_back(std::to_string(i) + " monotonic upstream string payload");
        }
        CHECK(pool.stats().live_allocations > 0);
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    {
        std::pmr::unsynchronized_pool_resource pools(&resource);
        std::pmr::list<int> values(&pools);
        for (int i = 0; i < 10000; ++i) {
            values.push_back(i);
        }
        CHECK(pool.stats().live_allocations > 0);
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANOTEST_MAIN()
