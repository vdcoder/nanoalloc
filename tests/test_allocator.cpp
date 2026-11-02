// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Public contract tests for nanoalloc::Allocator<T> (standard Allocator
// requirements and use with standard containers).
#include <nanoalloc/allocator.hpp>
#include <nanoalloc/pool.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"

using nanoalloc::Allocator;
using nanoalloc::Pool;

namespace {

constexpr std::size_t kMiB = 1024 * 1024;

using IntAllocator = Allocator<int>;
using Traits = std::allocator_traits<IntAllocator>;

// --- Compile-time checks of the Allocator requirements ---------------------
static_assert(std::is_same_v<Traits::value_type, int>);
static_assert(std::is_same_v<Traits::pointer, int*>);
static_assert(std::is_same_v<Traits::size_type, std::size_t>);
static_assert(std::is_same_v<Traits::rebind_alloc<double>, Allocator<double>>);
static_assert(Traits::propagate_on_container_copy_assignment::value);
static_assert(Traits::propagate_on_container_move_assignment::value);
static_assert(Traits::propagate_on_container_swap::value);
static_assert(!Traits::is_always_equal::value);
static_assert(std::is_nothrow_copy_constructible_v<IntAllocator>);
static_assert(std::is_nothrow_move_constructible_v<IntAllocator>);
static_assert(std::is_nothrow_constructible_v<Allocator<double>, const IntAllocator&>);
static_assert(!std::is_default_constructible_v<IntAllocator>);
static_assert(sizeof(IntAllocator) == sizeof(void*));

template <class T>
using Vector = std::vector<T, Allocator<T>>;

using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

struct Counted {
    static inline int alive = 0;
    static inline int constructed = 0;
    explicit Counted(int v) : value(v), label(std::to_string(v) + " counted label text") {
        ++alive;
        ++constructed;
    }
    Counted(const Counted& other) : value(other.value), label(other.label) {
        ++alive;
        ++constructed;
    }
    Counted(Counted&& other) noexcept : value(other.value), label(std::move(other.label)) {
        ++alive;
        ++constructed;
    }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --alive; }
    int value;
    std::string label;
};

struct alignas(64) CacheLine {
    std::uint64_t words[8];
};

struct alignas(256) Wide {
    unsigned char bytes[256];
};

struct alignas(4096) Page {
    unsigned char bytes[4096];
};

}  // namespace

NANO_TEST(allocator_vector_of_int) {
    Pool pool({16 * kMiB});
    {
        Vector<int> values{IntAllocator(pool)};
        for (int i = 0; i < 100000; ++i) {
            values.push_back(i * 3);
        }
        CHECK(pool.owns(values.data()));
        CHECK_EQ(values[99999], 299997);
        CHECK(values.get_allocator() == IntAllocator(pool));
        values.shrink_to_fit();
        CHECK(pool.owns(values.data()));
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(allocator_equality_and_rebinding) {
    Pool pool({1 * kMiB});
    Pool other({1 * kMiB});
    IntAllocator a(pool);
    IntAllocator b(pool);
    IntAllocator c(other);
    Allocator<double> rebound(a);
    CHECK(a == b);
    CHECK(!(a != b));
    CHECK(a != c);
    CHECK(rebound == a);
    CHECK(a == rebound);
    CHECK(IntAllocator(rebound) == a);
    CHECK(&rebound.pool() == &pool);

    // Memory from one allocator can be released through an equal one,
    // including a rebound copy.
    int* ints = a.allocate(10);
    b.deallocate(ints, 10);
    Traits::rebind_alloc<long> as_long(a);
    long* longs = as_long.allocate(3);
    Allocator<long>(b).deallocate(longs, 3);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_direct_use) {
    Pool pool({1 * kMiB});
    IntAllocator allocator(pool);
    int* zero = allocator.allocate(0);
    CHECK(zero != nullptr);
    allocator.deallocate(zero, 0);

    int* data = Traits::allocate(allocator, 1000);
    CHECK(nanotest::is_aligned(data, Pool::min_alignment));
    CHECK(pool.usable_size(data) >= 1000 * sizeof(int));
    for (int i = 0; i < 1000; ++i) {
        Traits::construct(allocator, data + i, i);
    }
    CHECK_EQ(data[999], 999);
    Traits::deallocate(allocator, data, 1000);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_overflow_and_exhaustion) {
    Pool pool({64 * 1024});
    IntAllocator allocator(pool);
    CHECK_THROWS_AS(allocator.allocate(std::numeric_limits<std::size_t>::max() / 2),
                    std::bad_array_new_length);
    CHECK_THROWS_AS(allocator.allocate(pool.capacity()), std::bad_alloc);
    Vector<int> values{allocator};
    CHECK_THROWS_AS(values.reserve(pool.capacity()), std::bad_alloc);
    values.push_back(1);  // still usable after the failure
    CHECK_EQ(values.size(), 1U);
}

NANO_TEST(allocator_standard_containers) {
    Pool pool({32 * kMiB});
    {
        std::list<int, Allocator<int>> list{Allocator<int>(pool)};
        std::deque<int, Allocator<int>> deque{Allocator<int>(pool)};
        std::map<int, int, std::less<>, Allocator<std::pair<const int, int>>> map{
            Allocator<std::pair<const int, int>>(pool)};
        std::set<int, std::less<>, Allocator<int>> set{Allocator<int>(pool)};
        std::unordered_map<int, int, std::hash<int>, std::equal_to<>,
                           Allocator<std::pair<const int, int>>>
            hash{Allocator<std::pair<const int, int>>(pool)};
        String text{Allocator<char>(pool)};
        for (int i = 0; i < 20000; ++i) {
            list.push_back(i);
            deque.push_back(i);
            map[i] = -i;
            set.insert(i * 7);
            hash.emplace(i, i + 1);
            text.push_back(static_cast<char>('a' + i % 26));
        }
        for (int i = 0; i < 20000; i += 3) {
            map.erase(i);
            hash.erase(i);
        }
        CHECK_EQ(list.back(), 19999);
        CHECK_EQ(deque[12345], 12345);
        CHECK_EQ(map.at(19999), -19999);
        CHECK_EQ(hash.at(19999), 20000);
        CHECK(set.count(7 * 19999) == 1);
        CHECK_EQ(text.size(), 20000U);
        CHECK(pool.owns(text.data()));
        CHECK(pool.verify_integrity());
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(allocator_nontrivial_elements) {
    Pool pool({16 * kMiB});
    Counted::alive = 0;
    {
        Vector<Counted> objects{Allocator<Counted>(pool)};
        for (int i = 0; i < 5000; ++i) {
            objects.emplace_back(i);
        }
        CHECK_EQ(Counted::alive, 5000);
        objects.erase(objects.begin() + 100, objects.begin() + 200);
        CHECK_EQ(Counted::alive, 4900);
        CHECK_EQ(objects[100].value, 200);
        CHECK_EQ(objects.back().label, std::string("4999 counted label text"));

        std::list<Counted, Allocator<Counted>> list{Allocator<Counted>(pool)};
        for (int i = 0; i < 1000; ++i) {
            list.emplace_front(i);
        }
        list.remove_if([](const Counted& c) { return c.value % 2 == 0; });
        CHECK_EQ(list.size(), 500U);
    }
    CHECK_EQ(Counted::alive, 0);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_nested_containers) {
    Pool pool({16 * kMiB});
    {
        using Inner = Vector<int>;
        std::vector<Inner, Allocator<Inner>> outer{Allocator<Inner>(pool)};
        for (int i = 0; i < 200; ++i) {
            Inner inner{Allocator<int>(pool)};
            for (int j = 0; j < i; ++j) {
                inner.push_back(j);
            }
            outer.push_back(std::move(inner));
        }
        CHECK_EQ(outer[199].size(), 199U);
        CHECK(pool.owns(outer[199].data()));
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_shared_ptr) {
    Pool pool({1 * kMiB});
    Counted::alive = 0;
    {
        auto shared = std::allocate_shared<Counted>(Allocator<Counted>(pool), 7);
        CHECK(pool.owns(shared.get()));
        CHECK_EQ(shared->value, 7);
        CHECK_EQ(pool.stats().live_allocations, 1U);
        auto copy = shared;
        CHECK_EQ(copy.use_count(), 2);
    }
    CHECK_EQ(Counted::alive, 0);
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_over_aligned_types) {
    Pool pool({8 * kMiB});
    {
        Vector<CacheLine> lines{Allocator<CacheLine>(pool)};
        Vector<Wide> wide{Allocator<Wide>(pool)};
        Vector<Page> pages{Allocator<Page>(pool)};
        for (int i = 0; i < 100; ++i) {
            lines.push_back(CacheLine{});
            wide.push_back(Wide{});
            if (i < 20) {
                pages.push_back(Page{});
            }
            CHECK(nanotest::is_aligned(lines.data(), alignof(CacheLine)));
            CHECK(nanotest::is_aligned(wide.data(), alignof(Wide)));
            if (!pages.empty()) {
                CHECK(nanotest::is_aligned(pages.data(), alignof(Page)));
            }
        }
        Allocator<Page> page_allocator(pool);
        Page* page = page_allocator.allocate(1);
        CHECK(nanotest::is_aligned(page, 4096));
        page_allocator.deallocate(page, 1);
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
}

NANO_TEST(allocator_propagation_semantics) {
    Pool first_pool({4 * kMiB});
    Pool second_pool({4 * kMiB});
    Vector<int> a({1, 2, 3}, Allocator<int>(first_pool));
    Vector<int> b({4, 5, 6, 7}, Allocator<int>(second_pool));

    // Copy construction keeps the source's pool.
    Vector<int> copy(a);
    CHECK(copy.get_allocator() == a.get_allocator());
    CHECK(first_pool.owns(copy.data()));

    // Copy assignment propagates the allocator.
    copy = b;
    CHECK(copy.get_allocator() == b.get_allocator());
    CHECK(second_pool.owns(copy.data()));
    CHECK_EQ(copy.size(), 4U);

    // Move assignment propagates the allocator and steals the buffer.
    Vector<int> target({9}, Allocator<int>(first_pool));
    const int* storage = b.data();
    target = std::move(b);
    CHECK(target.data() == storage);
    CHECK(target.get_allocator() == Allocator<int>(second_pool));

    // Swap exchanges allocators together with the storage.
    swap(a, target);
    CHECK(second_pool.owns(a.data()));
    CHECK(first_pool.owns(target.data()));
    CHECK_EQ(a.size(), 4U);
    CHECK_EQ(target.size(), 3U);
}

NANO_TEST(allocator_container_built_by_one_thread_and_destroyed_by_another) {
    // Within the thread lifetime contract: the building thread stays alive
    // until the other thread has destroyed the container.
    Pool pool({16 * kMiB});
    using Map = std::map<int, std::uint64_t, std::less<>,
                         Allocator<std::pair<const int, std::uint64_t>>>;
    std::atomic<int> phase{0};
    Map* built = nullptr;
    std::thread builder([&] {
        built = new Map(Allocator<std::pair<const int, std::uint64_t>>(pool));
        for (int i = 0; i < 2000; ++i) {
            built->emplace(i, static_cast<std::uint64_t>(i) * 3);
        }
        phase = 1;
        while (phase.load() != 2) {
            std::this_thread::yield();
        }
    });
    while (phase.load() != 1) {
        std::this_thread::yield();
    }
    // Every node, and any bookkeeping node the standard library allocates
    // through the allocator (MSVC's std::map has one), came from the builder.
    const std::uint64_t built_blocks = pool.stats().allocation_count;
    bool intact = built->size() == 2000 && built_blocks >= 2000;
    for (const auto& [key, value] : *built) {
        intact = intact && value == static_cast<std::uint64_t>(key) * 3;
    }
    delete built;  // every node is released by this thread, remotely
    phase = 2;
    builder.join();
    CHECK(intact);
    const nanoalloc::PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.remote_deallocation_count, built_blocks);
    CHECK(pool.verify_integrity());
}

NANOTEST_MAIN()
