// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Public contract tests for nanoalloc::Pool.
//
// These tests exercise only the documented public contract. They must not
// depend on implementation details such as power-of-two block sizes, a
// particular usable_size(), or a particular fragmentation pattern.
#include <nanoalloc/nanoalloc.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"
#include "test_stats.hpp"

using nanoalloc::Pool;
using nanoalloc::PoolConfig;
using nanoalloc::PoolStats;
using nanotest::check_pattern;
using nanotest::fill_pattern;
using nanotest::is_aligned;
using nanotest::Rng;

namespace {

constexpr std::size_t kKiB = 1024;
constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

// AddressSanitizer and ThreadSanitizer report oversized system allocations as
// errors instead of failing them.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizedAllocator = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitizedAllocator = true;
#else
constexpr bool kSanitizedAllocator = false;
#endif
#else
constexpr bool kSanitizedAllocator = false;
#endif

struct Allocation {
    void* ptr;
    std::size_t size;
    std::uint64_t tag;
};

/// Writes the whole usable range: the pattern over the requested bytes and a
/// second pattern over the slack, then verifies both.
bool fill_and_verify_usable(Pool& pool, void* ptr, std::size_t size, std::uint64_t tag) {
    const std::size_t usable = pool.usable_size(ptr);
    if (usable < size) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(ptr);
    fill_pattern(bytes, size, tag);
    fill_pattern(bytes + size, usable - size, ~tag);
    return check_pattern(bytes, size, tag) && check_pattern(bytes + size, usable - size, ~tag);
}

bool same_counters(const PoolStats& a, const PoolStats& b) {
    return a.capacity_bytes == b.capacity_bytes && a.live_allocations == b.live_allocations &&
           a.live_requested_bytes == b.live_requested_bytes &&
           a.live_usable_bytes == b.live_usable_bytes &&
           a.live_consumed_bytes == b.live_consumed_bytes && a.free_bytes == b.free_bytes &&
           a.largest_free_block == b.largest_free_block &&
           a.allocation_count == b.allocation_count &&
           a.deallocation_count == b.deallocation_count &&
           a.peak_requested_bytes == b.peak_requested_bytes &&
           a.peak_usable_bytes == b.peak_usable_bytes;
}

PoolConfig tracked(std::size_t capacity) {
    PoolConfig config;
    config.capacity_bytes = capacity;
    config.track_requested_bytes = true;
    return config;
}

const std::vector<std::size_t>& boundary_sizes() {
    static const std::vector<std::size_t> sizes = [] {
        std::vector<std::size_t> result = {0, 1, 2, 7, 8, 9};
        for (std::size_t power = 16; power <= 64 * kKiB; power *= 2) {
            result.push_back(power - 1);
            result.push_back(power);
            result.push_back(power + 1);
        }
        result.push_back(48);
        result.push_back(80);
        result.push_back(100);
        result.push_back(1000);
        result.push_back(3000);
        result.push_back(5000);
        return result;
    }();
    return sizes;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction and configuration
// ---------------------------------------------------------------------------

NANO_TEST(fresh_pool_reports_consistent_statistics) {
    Pool pool({1 * kMiB});
    const PoolStats stats = pool.stats();
    CHECK(stats.capacity_bytes >= 1 * kMiB);
    CHECK_EQ(pool.capacity(), stats.capacity_bytes);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.live_requested_bytes, 0U);
    CHECK_EQ(stats.live_usable_bytes, 0U);
    CHECK_EQ(stats.live_consumed_bytes, 0U);
    CHECK_EQ(stats.peak_requested_bytes, 0U);
    CHECK_EQ(stats.peak_usable_bytes, 0U);
    CHECK_EQ(stats.peak_consumed_bytes, 0U);
    CHECK_EQ(stats.free_bytes, stats.capacity_bytes);
    CHECK(stats.largest_free_block > 0);
    CHECK(stats.largest_free_block <= stats.capacity_bytes);
    // Nothing is carved from the arena before the first allocation, but the
    // whole arena is already obtained from the system.
    CHECK_EQ(stats.reserved_bytes, 0U);
    CHECK_EQ(stats.metadata_bytes, 0U);
    CHECK_EQ(stats.span_count, 0U);
    CHECK_EQ(stats.thread_heap_count, 0U);
    CHECK(stats.system_reserved_bytes > stats.capacity_bytes);
    CHECK(stats.system_reserved_bytes < stats.capacity_bytes + 4096);
    // Requested sizes are recorded in the block headers, so tracking is on by
    // default and needs no side table.
    CHECK(stats.requested_bytes_tracked);
    CHECK_EQ(stats.tracking_bytes, 0U);
    CHECK_EQ(stats.allocation_count, 0U);
    CHECK_EQ(stats.deallocation_count, 0U);
    CHECK_EQ(stats.failed_allocation_count, 0U);
    CHECK(pool.verify_integrity());

    PoolConfig untracked_config;
    untracked_config.capacity_bytes = 1 * kMiB;
    untracked_config.track_requested_bytes = false;
    Pool untracked(untracked_config);
    CHECK(!untracked.stats().requested_bytes_tracked);
    CHECK_EQ(untracked.stats().tracking_bytes, 0U);
    CHECK_EQ(untracked.stats().metadata_bytes, stats.metadata_bytes);
}

NANO_TEST(invalid_configuration_is_rejected) {
    CHECK_THROWS_AS(Pool(PoolConfig{0}), std::invalid_argument);
    CHECK_THROWS_AS(Pool(PoolConfig{Pool::max_capacity + 1}), std::invalid_argument);
    CHECK_THROWS_AS(Pool(PoolConfig{kSizeMax}), std::invalid_argument);
    PoolConfig empty;  // default capacity is zero
    CHECK_THROWS_AS(Pool{empty}, std::invalid_argument);
}

NANO_TEST(unobtainable_arena_throws_bad_alloc) {
    if constexpr (kSanitizedAllocator) {
        // The sanitizer runtimes report oversized system allocations as
        // errors instead of failing them, so this case cannot be exercised.
        std::printf("  skipped under a sanitizer allocator\n");
    } else if constexpr (sizeof(void*) < 8) {
        // On 32-bit targets max_capacity (1 GiB) can often be reserved.
        std::printf("  skipped on 32-bit targets\n");
    } else {
        CHECK_THROWS_AS(Pool(PoolConfig{Pool::max_capacity}), std::bad_alloc);
    }
}

NANO_TEST(capacity_is_rounded_up_to_granularity) {
    for (std::size_t requested : {std::size_t{1}, std::size_t{15}, std::size_t{16},
                                  std::size_t{17}, std::size_t{100}, std::size_t{4095},
                                  std::size_t{100000}}) {
        Pool pool({requested});
        CHECK(pool.capacity() >= requested);
        CHECK(pool.capacity() < requested + Pool::granularity);
        CHECK_EQ(pool.capacity() % Pool::granularity, 0U);
        CHECK(pool.verify_integrity());
    }
}

NANO_TEST(tiny_pool_behaves_consistently) {
    Pool pool({Pool::granularity});
    const PoolStats initial = pool.stats();
    CHECK(initial.largest_free_block <= initial.capacity_bytes);
    CHECK_THROWS_AS(pool.allocate(pool.capacity() + 1), std::bad_alloc);
    if (initial.largest_free_block > 0) {
        for (int round = 0; round < 3; ++round) {
            void* ptr = pool.allocate(initial.largest_free_block);
            REQUIRE(ptr != nullptr);
            CHECK(pool.usable_size(ptr) >= initial.largest_free_block);
            pool.deallocate(ptr);
            CHECK_EQ(pool.stats().free_bytes, initial.free_bytes);
            CHECK_EQ(pool.stats().largest_free_block, initial.largest_free_block);
        }
    }
    CHECK(pool.verify_integrity());
}

// ---------------------------------------------------------------------------
// Alignment
// ---------------------------------------------------------------------------

NANO_TEST(default_alignment_is_at_least_16) {
    Pool pool({1 * kMiB});
    std::vector<void*> ptrs;
    for (std::size_t size : boundary_sizes()) {
        void* ptr = pool.allocate(size);
        CHECK(is_aligned(ptr, Pool::min_alignment));
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(requested_alignments_are_honored) {
    Pool pool({8 * kMiB});
    const std::size_t sizes[] = {0, 1, 15, 16, 17, 100, 1000, 4096, 5000, 20000};
    std::uint64_t tag = 1;
    for (std::size_t alignment = 1; alignment <= Pool::max_alignment; alignment *= 2) {
        std::vector<void*> ptrs;
        for (std::size_t size : sizes) {
            void* ptr = pool.allocate(size, alignment);
            REQUIRE(ptr != nullptr);
            CHECK(is_aligned(ptr, std::max(alignment, Pool::min_alignment)));
            CHECK(fill_and_verify_usable(pool, ptr, size, tag++));
            ptrs.push_back(ptr);

            void* other = pool.try_allocate(size, alignment);
            REQUIRE(other != nullptr);
            CHECK(is_aligned(other, std::max(alignment, Pool::min_alignment)));
            ptrs.push_back(other);
        }
        for (void* ptr : ptrs) {
            pool.deallocate(ptr);
        }
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(required_alignments_16_to_4096) {
    Pool pool({4 * kMiB});
    for (std::size_t alignment : {16, 32, 64, 256, 4096}) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 16; ++i) {
            void* ptr = pool.allocate(static_cast<std::size_t>(24 + i * 40), alignment);
            CHECK(is_aligned(ptr, alignment));
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs) {
            pool.deallocate(ptr);
        }
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(alignment_above_maximum_is_bad_alloc) {
    Pool pool({1 * kMiB});
    const PoolStats before = pool.stats();
    CHECK_THROWS_AS(pool.allocate(16, Pool::max_alignment * 2), std::bad_alloc);
    CHECK(pool.try_allocate(16, Pool::max_alignment * 2) == nullptr);
    CHECK(pool.try_allocate(16, Pool::max_alignment << 8) == nullptr);
    const PoolStats after = pool.stats();
    CHECK(same_counters(before, after));
    CHECK_EQ(after.failed_allocation_count, before.failed_allocation_count + 3);
}

NANO_TEST(invalid_alignment_is_invalid_argument) {
    Pool pool({1 * kMiB});
    const PoolStats before = pool.stats();
    for (std::size_t alignment : {std::size_t{0}, std::size_t{3}, std::size_t{24},
                                  std::size_t{48}, std::size_t{100}, std::size_t{4097},
                                  kSizeMax}) {
        CHECK_THROWS_AS(pool.allocate(32, alignment), std::invalid_argument);
        CHECK(pool.try_allocate(32, alignment) == nullptr);
    }
    CHECK(same_counters(before, pool.stats()));
    CHECK(pool.verify_integrity());
}

// ---------------------------------------------------------------------------
// Special requests
// ---------------------------------------------------------------------------

NANO_TEST(zero_byte_allocations_are_unique_and_releasable) {
    Pool pool(tracked(64 * kKiB));
    void* first = pool.allocate(0);
    void* second = pool.allocate(0);
    void* third = pool.try_allocate(0);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(third != nullptr);
    CHECK(first != second);
    CHECK(second != third);
    CHECK(first != third);
    CHECK(is_aligned(first, Pool::min_alignment));
    CHECK(pool.owns(first));
    CHECK(fill_and_verify_usable(pool, first, 0, 7));

    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 3U);
    CHECK_EQ(stats.live_requested_bytes, 0U);
    CHECK(stats.live_consumed_bytes > 0);  // each allocation is distinct

    pool.deallocate(first);
    pool.deallocate(second);
    pool.deallocate(third);
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK_EQ(pool.stats().free_bytes, pool.capacity());
    CHECK(pool.verify_integrity());
}

NANO_TEST(null_deallocation_is_a_no_op) {
    Pool pool({64 * kKiB});
    void* live = pool.allocate(40);
    const PoolStats before = pool.stats();
    pool.deallocate(nullptr);
    const PoolStats after = pool.stats();
    CHECK(same_counters(before, after));
    CHECK_EQ(after.deallocation_count, before.deallocation_count);
    pool.deallocate(live);
    CHECK(pool.verify_integrity());
}

NANO_TEST(usable_size_covers_request_and_is_writable) {
    Pool pool({4 * kMiB});
    CHECK_EQ(pool.usable_size(nullptr), 0U);
    std::uint64_t tag = 11;
    for (std::size_t size : boundary_sizes()) {
        void* ptr = pool.allocate(size);
        CHECK(pool.usable_size(ptr) >= size);
        CHECK(fill_and_verify_usable(pool, ptr, size, tag++));
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(oversized_requests_fail_cleanly) {
    Pool pool({1 * kMiB});
    const PoolStats before = pool.stats();
    CHECK_THROWS_AS(pool.allocate(pool.capacity() + 1), std::bad_alloc);
    CHECK_THROWS_AS(pool.allocate(kSizeMax), std::bad_alloc);
    CHECK_THROWS_AS(pool.allocate(kSizeMax - 15), std::bad_alloc);
    CHECK_THROWS_AS(pool.allocate(kSizeMax / 2 + 1), std::bad_alloc);
    CHECK_THROWS_AS(pool.allocate(kSizeMax, Pool::max_alignment), std::bad_alloc);
    CHECK(pool.try_allocate(kSizeMax) == nullptr);
    const PoolStats after = pool.stats();
    CHECK(same_counters(before, after));
    CHECK_EQ(after.failed_allocation_count, before.failed_allocation_count + 6);
    CHECK(pool.verify_integrity());
}

NANO_TEST(owns_distinguishes_arena_addresses) {
    Pool pool({64 * kKiB});
    Pool other({64 * kKiB});
    void* ptr = pool.allocate(100);
    void* foreign = other.allocate(100);
    int local = 0;
    CHECK(pool.owns(ptr));
    CHECK(pool.owns(static_cast<char*>(ptr) + 50));
    CHECK(!pool.owns(foreign));
    CHECK(!pool.owns(&local));
    CHECK(!pool.owns(nullptr));
    CHECK(other.owns(foreign));
    pool.deallocate(ptr);
    other.deallocate(foreign);
}

// ---------------------------------------------------------------------------
// Sizes, overlap and content integrity
// ---------------------------------------------------------------------------

NANO_TEST(boundary_sizes_are_readable_writable_and_disjoint) {
    Pool pool({8 * kMiB});
    nanotest::IntervalSet intervals;
    std::vector<Allocation> live;
    std::uint64_t tag = 100;
    for (std::size_t size : boundary_sizes()) {
        for (int copy = 0; copy < 3; ++copy) {
            void* ptr = pool.allocate(size);
            REQUIRE(ptr != nullptr);
            CHECK(is_aligned(ptr, Pool::min_alignment));
            CHECK(intervals.insert(ptr, pool.usable_size(ptr)));
            CHECK(fill_and_verify_usable(pool, ptr, size, tag));
            fill_pattern(ptr, size, tag);
            live.push_back({ptr, size, tag++});
        }
    }
    for (const Allocation& allocation : live) {
        CHECK(check_pattern(allocation.ptr, allocation.size, allocation.tag));
    }
    // Release in an interleaved order: odd indices first, then even ones.
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 1 - pass; i < live.size(); i += 2) {
            CHECK(check_pattern(live[i].ptr, live[i].size, live[i].tag));
            pool.deallocate(live[i].ptr);
        }
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(live_allocations_never_overlap) {
    Pool pool({16 * kMiB});
    Rng rng(42);
    nanotest::IntervalSet intervals;
    std::vector<void*> live;
    for (int i = 0; i < 5000; ++i) {
        const std::size_t size = rng.log_uniform(1, 16 * kKiB);
        const std::size_t alignment = std::size_t{1} << rng.below(13);
        void* ptr = pool.allocate(size, alignment);
        REQUIRE(ptr != nullptr);
        REQUIRE(pool.owns(ptr));
        REQUIRE(pool.owns(static_cast<char*>(ptr) + pool.usable_size(ptr) - 1));
        REQUIRE(intervals.insert(ptr, pool.usable_size(ptr)));
        live.push_back(ptr);
        if (rng.chance(30) && !live.empty()) {
            const std::size_t victim = rng.below(live.size());
            intervals.erase(live[victim]);
            pool.deallocate(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(contents_survive_unrelated_activity) {
    Pool pool({8 * kMiB});
    Rng rng(7);
    std::vector<Allocation> keep;
    for (std::uint64_t tag = 0; tag < 500; ++tag) {
        const std::size_t size = rng.log_uniform(1, 4 * kKiB);
        void* ptr = pool.allocate(size);
        fill_pattern(ptr, size, tag);
        keep.push_back({ptr, size, tag});
    }
    std::vector<Allocation> churn;
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 100; ++i) {
            const std::size_t size = rng.log_uniform(1, 8 * kKiB);
            void* ptr = pool.allocate(size, std::size_t{1} << rng.below(10));
            const std::uint64_t tag = 1000000 + static_cast<std::uint64_t>(round * 100 + i);
            fill_pattern(ptr, pool.usable_size(ptr), tag);
            churn.push_back({ptr, pool.usable_size(ptr), tag});
        }
        nanotest::shuffle(churn, rng);
        while (churn.size() > 50) {
            CHECK(check_pattern(churn.back().ptr, churn.back().size, churn.back().tag));
            pool.deallocate(churn.back().ptr);
            churn.pop_back();
        }
        for (const Allocation& allocation : keep) {
            REQUIRE(check_pattern(allocation.ptr, allocation.size, allocation.tag));
        }
    }
    for (const Allocation& allocation : churn) {
        pool.deallocate(allocation.ptr);
    }
    for (const Allocation& allocation : keep) {
        CHECK(check_pattern(allocation.ptr, allocation.size, allocation.tag));
        pool.deallocate(allocation.ptr);
    }
    CHECK(pool.verify_integrity());
}

// ---------------------------------------------------------------------------
// Exhaustion and recovery
// ---------------------------------------------------------------------------

NANO_TEST(exhaustion_throws_and_full_recovery_follows) {
    Pool pool({256 * kKiB});
    const std::size_t initial_largest = pool.stats().largest_free_block;
    Rng rng(3);
    for (int round = 0; round < 3; ++round) {
        std::vector<void*> ptrs;
        for (;;) {
            void* ptr = nullptr;
            try {
                ptr = pool.allocate(48);
            } catch (const std::bad_alloc&) {
                break;
            }
            fill_pattern(ptr, 48, ptrs.size());
            ptrs.push_back(ptr);
        }
        REQUIRE(!ptrs.empty());
        CHECK(ptrs.size() * 48 <= pool.capacity());

        // A failed request leaves the pool untouched.
        const PoolStats exhausted = pool.stats();
        CHECK(pool.try_allocate(48) == nullptr);
        CHECK(same_counters(exhausted, pool.stats()));
        CHECK(pool.verify_integrity());

        for (std::size_t i = 0; i < ptrs.size(); ++i) {
            CHECK(check_pattern(ptrs[i], 48, i));
        }
        nanotest::shuffle(ptrs, rng);
        for (void* ptr : ptrs) {
            pool.deallocate(ptr);
        }
        const PoolStats recovered = pool.stats();
        CHECK_EQ(recovered.live_allocations, 0U);
        CHECK_EQ(recovered.free_bytes, recovered.capacity_bytes);
        CHECK_EQ(recovered.largest_free_block, initial_largest);
    }
    void* whole = pool.allocate(initial_largest);
    CHECK(whole != nullptr);
    pool.deallocate(whole);
    CHECK(pool.verify_integrity());
}

NANO_TEST(exhaustion_with_mixed_sizes_and_recovery) {
    Pool pool({1 * kMiB});
    const std::size_t initial_largest = pool.stats().largest_free_block;
    Rng rng(99);
    for (int round = 0; round < 5; ++round) {
        std::vector<Allocation> live;
        std::size_t failures = 0;
        while (failures < 64) {
            const std::size_t size = rng.log_uniform(1, 32 * kKiB);
            void* ptr = pool.try_allocate(size);
            if (ptr == nullptr) {
                ++failures;
                continue;
            }
            const std::uint64_t tag = rng.next();
            fill_pattern(ptr, size, tag);
            live.push_back({ptr, size, tag});
            if (rng.chance(20)) {
                const std::size_t victim = rng.below(live.size());
                CHECK(check_pattern(live[victim].ptr, live[victim].size, live[victim].tag));
                pool.deallocate(live[victim].ptr);
                live[victim] = live.back();
                live.pop_back();
            }
        }
        CHECK(pool.verify_integrity());
        nanotest::shuffle(live, rng);
        for (const Allocation& allocation : live) {
            CHECK(check_pattern(allocation.ptr, allocation.size, allocation.tag));
            pool.deallocate(allocation.ptr);
        }
        CHECK_EQ(pool.stats().free_bytes, pool.capacity());
        CHECK_EQ(pool.stats().largest_free_block, initial_largest);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(largest_free_block_is_always_allocatable) {
    Pool pool({4 * kMiB});
    Rng rng(5);
    std::vector<void*> live;
    for (int step = 0; step < 200; ++step) {
        const PoolStats stats = pool.stats();
        if (stats.largest_free_block > 0) {
            // The documented guarantee: any request up to largest_free_block
            // at default alignment succeeds.
            const std::size_t request = rng.chance(50) ? stats.largest_free_block
                                                       : rng.between(1, stats.largest_free_block);
            void* probe = pool.try_allocate(request);
            REQUIRE(probe != nullptr);
            pool.deallocate(probe);
        }
        void* ptr = pool.try_allocate(rng.log_uniform(1, 64 * kKiB));
        if (ptr != nullptr) {
            live.push_back(ptr);
        }
        if (rng.chance(40) && !live.empty()) {
            const std::size_t victim = rng.below(live.size());
            pool.deallocate(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(full_recovery_after_fragmentation) {
    Pool pool({2 * kMiB});
    const PoolStats initial = pool.stats();
    std::vector<void*> small;
    for (;;) {
        void* ptr = pool.try_allocate(24);
        if (ptr == nullptr) {
            break;
        }
        small.push_back(ptr);
    }
    // Free every other block, leaving many small holes.
    std::vector<void*> remaining;
    for (std::size_t i = 0; i < small.size(); ++i) {
        if (i % 2 == 0) {
            pool.deallocate(small[i]);
        } else {
            remaining.push_back(small[i]);
        }
    }
    CHECK(pool.stats().free_bytes > 0);
    CHECK(pool.verify_integrity());
    // Refill the holes with small objects, then release everything.
    std::vector<void*> refill;
    for (;;) {
        void* ptr = pool.try_allocate(20);
        if (ptr == nullptr) {
            break;
        }
        refill.push_back(ptr);
    }
    CHECK(!refill.empty());
    for (void* ptr : remaining) {
        pool.deallocate(ptr);
    }
    for (void* ptr : refill) {
        pool.deallocate(ptr);
    }
    const PoolStats final_stats = pool.stats();
    CHECK_EQ(final_stats.free_bytes, initial.capacity_bytes);
    CHECK_EQ(final_stats.largest_free_block, initial.largest_free_block);
    void* whole = pool.allocate(initial.largest_free_block);
    CHECK(whole != nullptr);
    std::memset(whole, 0xAB, initial.largest_free_block);
    pool.deallocate(whole);
    CHECK(pool.verify_integrity());
}

NANO_TEST(non_power_of_two_capacities_work) {
    for (std::size_t capacity : {std::size_t{48}, std::size_t{4096 + 16}, 3 * kMiB + 48,
                                 5 * kMiB / 3, std::size_t{1000000}}) {
        Pool pool({capacity});
        Rng rng(capacity);
        std::vector<Allocation> live;
        for (int step = 0; step < 2000; ++step) {
            const std::size_t size = rng.log_uniform(1, 8 * kKiB);
            void* ptr = pool.try_allocate(size);
            if (ptr != nullptr) {
                REQUIRE(pool.owns(ptr));
                REQUIRE(pool.owns(static_cast<char*>(ptr) + pool.usable_size(ptr) - 1));
                const std::uint64_t tag = rng.next();
                fill_pattern(ptr, size, tag);
                live.push_back({ptr, size, tag});
            }
            if ((ptr == nullptr || rng.chance(45)) && !live.empty()) {
                const std::size_t victim = rng.below(live.size());
                CHECK(check_pattern(live[victim].ptr, live[victim].size, live[victim].tag));
                pool.deallocate(live[victim].ptr);
                live[victim] = live.back();
                live.pop_back();
            }
        }
        for (const Allocation& allocation : live) {
            pool.deallocate(allocation.ptr);
        }
        CHECK_EQ(pool.stats().free_bytes, pool.capacity());
        CHECK(pool.verify_integrity());
    }
}

// ---------------------------------------------------------------------------
// Large and mixed workloads, repeated cycles
// ---------------------------------------------------------------------------

NANO_TEST(large_allocations) {
    Pool pool({64 * kMiB});
    const std::size_t sizes[] = {1 * kMiB, 5 * kMiB / 2, 4 * kMiB + 16, 3 * kMiB - 1};
    std::vector<Allocation> live;
    std::uint64_t tag = 77;
    for (std::size_t size : sizes) {
        void* ptr = pool.allocate(size);
        REQUIRE(ptr != nullptr);
        CHECK(pool.usable_size(ptr) >= size);
        std::memset(ptr, 0x5A, size);
        // Stamp a pattern at both ends and at a few interior points.
        auto* bytes = static_cast<unsigned char*>(ptr);
        bytes[0] = static_cast<unsigned char>(tag);
        bytes[size / 2] = static_cast<unsigned char>(tag + 1);
        bytes[size - 1] = static_cast<unsigned char>(tag + 2);
        live.push_back({ptr, size, tag});
        tag += 3;
    }
    for (const Allocation& allocation : live) {
        const auto* bytes = static_cast<const unsigned char*>(allocation.ptr);
        CHECK_EQ(bytes[0], static_cast<unsigned char>(allocation.tag));
        CHECK_EQ(bytes[allocation.size / 2], static_cast<unsigned char>(allocation.tag + 1));
        CHECK_EQ(bytes[allocation.size - 1], static_cast<unsigned char>(allocation.tag + 2));
        CHECK_EQ(bytes[1], 0x5A);
        pool.deallocate(allocation.ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(mixed_small_and_large_objects) {
    Pool pool({32 * kMiB});
    Rng rng(2024);
    std::vector<Allocation> live;
    for (int step = 0; step < 4000; ++step) {
        const bool large = rng.chance(5);
        const std::size_t size =
            large ? rng.between(64 * kKiB, 1 * kMiB) : rng.log_uniform(1, 256);
        void* ptr = pool.try_allocate(size);
        if (ptr != nullptr) {
            const std::uint64_t tag = rng.next();
            fill_pattern(ptr, std::min<std::size_t>(size, 4096), tag);
            live.push_back({ptr, std::min<std::size_t>(size, 4096), tag});
        }
        if (rng.chance(45) && !live.empty()) {
            const std::size_t victim = rng.below(live.size());
            CHECK(check_pattern(live[victim].ptr, live[victim].size, live[victim].tag));
            pool.deallocate(live[victim].ptr);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (const Allocation& allocation : live) {
        CHECK(check_pattern(allocation.ptr, allocation.size, allocation.tag));
        pool.deallocate(allocation.ptr);
    }
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(repeated_allocate_free_cycles) {
    Pool pool({1 * kMiB});
    const PoolStats initial = pool.stats();
    for (int cycle = 0; cycle < 20000; ++cycle) {
        const auto size = static_cast<std::size_t>(1 + (cycle * 37) % 2000);
        void* ptr = pool.allocate(size);
        static_cast<unsigned char*>(ptr)[size - 1] = 1;
        pool.deallocate(ptr);
    }
    for (int cycle = 0; cycle < 200; ++cycle) {
        std::vector<void*> batch;
        for (int i = 0; i < 64; ++i) {
            batch.push_back(pool.allocate(static_cast<std::size_t>(16 + i * 8)));
        }
        while (!batch.empty()) {  // LIFO release
            pool.deallocate(batch.back());
            batch.pop_back();
        }
    }
    const PoolStats final_stats = pool.stats();
    CHECK_EQ(final_stats.live_allocations, 0U);
    CHECK_EQ(final_stats.free_bytes, initial.free_bytes);
    CHECK_EQ(final_stats.largest_free_block, initial.largest_free_block);
    CHECK_EQ(final_stats.allocation_count, 20000U + 200U * 64U);
    CHECK_EQ(final_stats.deallocation_count, final_stats.allocation_count);
    CHECK(pool.verify_integrity());
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

NANO_TEST(statistics_are_accurate) {
    Pool pool(tracked(4 * kMiB));
    Rng rng(31337);
    std::vector<Allocation> live;
    std::size_t live_requested = 0;
    std::size_t peak_requested = 0;
    std::size_t peak_usable = 0;
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;

    auto verify = [&] {
        std::size_t usable = 0;
        for (const Allocation& allocation : live) {
            usable += pool.usable_size(allocation.ptr);
        }
        peak_usable = std::max(peak_usable, usable);
        const PoolStats stats = pool.stats();
        CHECK_EQ(stats.live_allocations, live.size());
        CHECK_EQ(stats.live_requested_bytes, live_requested);
        CHECK_EQ(stats.live_usable_bytes, usable);
        CHECK(stats.live_consumed_bytes >= stats.live_usable_bytes);
        CHECK(stats.live_usable_bytes >= stats.live_requested_bytes);
        CHECK_EQ(stats.free_bytes, stats.capacity_bytes - stats.live_consumed_bytes);
        CHECK(stats.largest_free_block <= stats.free_bytes);
        CHECK_EQ(stats.peak_requested_bytes, peak_requested);
        CHECK(stats.peak_usable_bytes >= peak_usable);
        CHECK(stats.peak_consumed_bytes >= stats.peak_usable_bytes);
        CHECK(stats.peak_consumed_bytes >= stats.live_consumed_bytes);
        CHECK_EQ(stats.allocation_count, allocations);
        CHECK_EQ(stats.deallocation_count, deallocations);
        CHECK(nanotest::stats_are_consistent(stats));
    };

    verify();
    for (int step = 0; step < 3000; ++step) {
        if (live.empty() || rng.chance(60)) {
            const std::size_t size = rng.chance(5) ? 0 : rng.log_uniform(1, 16 * kKiB);
            const std::size_t alignment = rng.chance(10) ? std::size_t{1} << rng.below(13) : 16;
            void* ptr = pool.try_allocate(size, alignment);
            if (ptr != nullptr) {
                live.push_back({ptr, size, 0});
                live_requested += size;
                peak_requested = std::max(peak_requested, live_requested);
                ++allocations;
            }
        } else {
            const std::size_t victim = rng.below(live.size());
            pool.deallocate(live[victim].ptr);
            live_requested -= live[victim].size;
            ++deallocations;
            live[victim] = live.back();
            live.pop_back();
        }
        if (step % 50 == 0) {
            verify();
        }
    }
    verify();
    for (const Allocation& allocation : live) {
        pool.deallocate(allocation.ptr);
        ++deallocations;
    }
    live.clear();
    live_requested = 0;
    verify();
    CHECK(pool.verify_integrity());
}

NANO_TEST(statistics_without_requested_tracking) {
    PoolConfig config;
    config.capacity_bytes = 1 * kMiB;
    config.track_requested_bytes = false;  // also the default
    Pool pool(config);
    std::vector<void*> live;
    std::size_t usable = 0;
    for (std::size_t size : boundary_sizes()) {
        void* ptr = pool.allocate(size);
        usable += pool.usable_size(ptr);
        live.push_back(ptr);
    }
    PoolStats stats = pool.stats();
    CHECK(!stats.requested_bytes_tracked);
    CHECK_EQ(stats.tracking_bytes, 0U);
    CHECK_EQ(stats.live_requested_bytes, 0U);
    CHECK_EQ(stats.peak_requested_bytes, 0U);
    CHECK_EQ(stats.live_allocations, live.size());
    CHECK_EQ(stats.live_usable_bytes, usable);
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    stats = pool.stats();
    CHECK_EQ(stats.live_usable_bytes, 0U);
    CHECK_EQ(stats.free_bytes, stats.capacity_bytes);
    CHECK(pool.verify_integrity());
}

NANO_TEST(peaks_persist_after_release) {
    Pool pool(tracked(1 * kMiB));
    void* a = pool.allocate(1000);
    void* b = pool.allocate(3000);
    const PoolStats at_peak = pool.stats();
    pool.deallocate(a);
    pool.deallocate(b);
    void* c = pool.allocate(10);
    const PoolStats later = pool.stats();
    CHECK_EQ(later.peak_requested_bytes, 4000U);
    CHECK_EQ(later.peak_usable_bytes, at_peak.live_usable_bytes);
    CHECK_EQ(later.peak_consumed_bytes, at_peak.live_consumed_bytes);
    CHECK_EQ(later.live_requested_bytes, 10U);
    pool.deallocate(c);
}

NANO_TEST(failed_allocations_are_counted_without_side_effects) {
    Pool pool({64 * kKiB});
    void* keep = pool.allocate(100);
    const PoolStats before = pool.stats();
    CHECK(pool.try_allocate(pool.capacity() * 2) == nullptr);
    CHECK_THROWS_AS(pool.allocate(pool.capacity() * 2), std::bad_alloc);
    const PoolStats after = pool.stats();
    CHECK(same_counters(before, after));
    CHECK_EQ(after.failed_allocation_count, before.failed_allocation_count + 2);
    pool.deallocate(keep);
}

NANO_TEST(independent_pools_do_not_interact) {
    Pool first({256 * kKiB});
    Pool second({256 * kKiB});
    std::vector<void*> from_first;
    for (int i = 0; i < 100; ++i) {
        from_first.push_back(first.allocate(64));
    }
    CHECK_EQ(second.stats().live_allocations, 0U);
    CHECK_EQ(second.stats().free_bytes, second.capacity());
    void* ptr = second.allocate(64);
    CHECK(!first.owns(ptr));
    second.deallocate(ptr);
    for (void* p : from_first) {
        first.deallocate(p);
    }
    CHECK(first.verify_integrity());
    CHECK(second.verify_integrity());
}

// ---------------------------------------------------------------------------
// Budget and reservation accounting
// ---------------------------------------------------------------------------

NANO_TEST(budget_covers_heaps_spans_and_oversized_blocks) {
    Pool pool({8 * kMiB});
    const PoolStats fresh = pool.stats();
    const std::size_t pool_object = fresh.system_reserved_bytes - fresh.capacity_bytes;
    Rng rng(2024);
    std::vector<Allocation> live;
    std::uint64_t failures = 0;
    bool consistent = true;
    for (int step = 0; step < 4000; ++step) {
        if (live.empty() || rng.chance(55)) {
            // Mostly span blocks, some larger than a 1 MiB span can hold.
            const std::size_t size = rng.chance(3) ? rng.between(kMiB + kMiB / 8, 5 * kMiB / 2)
                                                   : rng.log_uniform(1, 256 * kKiB);
            const std::size_t alignment = rng.chance(10) ? std::size_t{1} << rng.below(13) : 16;
            void* ptr = pool.try_allocate(size, alignment);
            if (ptr == nullptr) {
                ++failures;
                continue;
            }
            fill_pattern(ptr, std::min<std::size_t>(size, 4096), step);
            live.push_back({ptr, size, static_cast<std::uint64_t>(step)});
        } else {
            const std::size_t victim = rng.below(live.size());
            consistent = consistent && check_pattern(live[victim].ptr,
                                                     std::min<std::size_t>(live[victim].size, 4096),
                                                     live[victim].tag);
            pool.deallocate(live[victim].ptr);
            live[victim] = live.back();
            live.pop_back();
        }
        if (step % 50 == 0) {
            const PoolStats stats = pool.stats();
            consistent = consistent && nanotest::stats_are_consistent(stats) &&
                         stats.reserved_bytes <= stats.capacity_bytes &&
                         stats.system_reserved_bytes ==
                             stats.capacity_bytes + stats.oversized_reserved_bytes + pool_object;
        }
    }
    CHECK(consistent);
    CHECK(failures > 0);  // the budget was reached
    CHECK_EQ(pool.stats().failed_allocation_count, failures);
    for (const Allocation& allocation : live) {
        pool.deallocate(allocation.ptr);
    }
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.oversized_reserved_bytes, 0U);
    CHECK_EQ(stats.system_reserved_bytes, fresh.system_reserved_bytes);
    CHECK_EQ(stats.reserved_bytes,
             stats.metadata_bytes + stats.thread_free_bytes + stats.free_span_bytes);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(oversized_blocks_return_their_budget_when_released) {
    Pool pool({16 * kMiB});
    void* small = pool.allocate(100);  // the thread's heap and first span exist
    const PoolStats before = pool.stats();
    void* a = pool.allocate(3 * kMiB);
    void* b = pool.allocate(2 * kMiB + 1, 4096);
    CHECK(is_aligned(b, 4096));
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.oversized_live_allocations, 2U);
    CHECK_EQ(stats.oversized_live_bytes, pool.usable_size(a) + pool.usable_size(b));
    CHECK(stats.oversized_reserved_bytes >= stats.oversized_live_bytes + 2 * Pool::header_size);
    CHECK_EQ(stats.reserved_bytes, before.reserved_bytes + stats.oversized_reserved_bytes);
    CHECK_EQ(stats.system_reserved_bytes,
             before.system_reserved_bytes + stats.oversized_reserved_bytes);
    CHECK_EQ(stats.live_consumed_bytes,
             before.live_consumed_bytes + stats.oversized_reserved_bytes);
    CHECK(nanotest::stats_are_consistent(stats));

    pool.deallocate(a);
    pool.deallocate(b);
    stats = pool.stats();
    CHECK_EQ(stats.oversized_live_allocations, 0U);
    CHECK_EQ(stats.oversized_reserved_bytes, 0U);
    CHECK_EQ(stats.reserved_bytes, before.reserved_bytes);
    CHECK_EQ(stats.system_reserved_bytes, before.system_reserved_bytes);
    CHECK_EQ(stats.oversized_allocation_count, 2U);
    CHECK_EQ(stats.deallocation_count, 2U);

    // Oversized blocks alone can use the whole unreserved budget, and no more.
    std::vector<void*> blocks;
    for (;;) {
        void* ptr = pool.try_allocate(3 * kMiB);
        if (ptr == nullptr) {
            break;
        }
        blocks.push_back(ptr);
        CHECK(pool.stats().reserved_bytes <= pool.capacity());
    }
    CHECK(blocks.size() >= 4);
    CHECK(blocks.size() * 3 * kMiB <= pool.capacity() - before.reserved_bytes);
    for (void* ptr : blocks) {
        pool.deallocate(ptr);
    }
    pool.deallocate(small);
    stats = pool.stats();
    CHECK_EQ(stats.reserved_bytes, before.reserved_bytes);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(carved_spans_stay_charged_against_the_budget) {
    // Span blocks fill the budget; once spans are carved they stay charged
    // (and owned by this thread) after their blocks are freed.
    Pool pool({4 * kMiB});
    std::vector<void*> blocks;
    for (;;) {
        void* ptr = pool.try_allocate(64 * kKiB);
        if (ptr == nullptr) {
            break;
        }
        blocks.push_back(ptr);
    }
    const PoolStats full = pool.stats();
    CHECK(blocks.size() >= 50);
    CHECK(full.reserved_bytes <= full.capacity_bytes);
    CHECK(nanotest::stats_are_consistent(full));
    CHECK(pool.try_allocate(2 * kMiB) == nullptr);  // more than the unreserved rest

    for (void* ptr : blocks) {
        pool.deallocate(ptr);
    }
    const PoolStats empty = pool.stats();
    CHECK_EQ(empty.live_allocations, 0U);
    CHECK_EQ(empty.reserved_bytes, full.reserved_bytes);
    CHECK_EQ(empty.span_count, full.span_count);
    CHECK_EQ(empty.free_span_count, 0U);  // this thread still owns them
    CHECK_EQ(empty.reserved_bytes, empty.metadata_bytes + empty.thread_free_bytes);
    CHECK(pool.try_allocate(2 * kMiB) == nullptr);
    CHECK(nanotest::stats_are_consistent(empty));

    // The thread's spans serve span requests again.
    for (void*& ptr : blocks) {
        ptr = pool.allocate(64 * kKiB);
    }
    CHECK_EQ(pool.stats().span_count, full.span_count);
    for (void* ptr : blocks) {
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(statistics_identities_hold_for_every_size_and_alignment) {
    Pool pool({16 * kMiB});
    std::vector<void*> live;
    for (std::size_t size : boundary_sizes()) {
        for (std::size_t alignment = 16; alignment <= Pool::max_alignment; alignment *= 4) {
            live.push_back(pool.allocate(size, alignment));
            if (!nanotest::stats_are_consistent(pool.stats())) {
                CHECK(false);
            }
        }
    }
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, live.size());
    for (std::size_t i = 0; i < live.size(); i += 2) {
        pool.deallocate(live[i]);
    }
    CHECK(nanotest::stats_are_consistent(pool.stats()));
    for (std::size_t i = 1; i < live.size(); i += 2) {
        pool.deallocate(live[i]);
    }
    stats = pool.stats();
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.fragment_bytes, 0U);  // everything coalesced again
    CHECK(pool.verify_integrity());
}

NANO_TEST(version_information_matches_build) {
    CHECK_EQ(std::string(nanoalloc::version_string), std::string(NANOALLOC_EXPECTED_VERSION));
}

NANOTEST_MAIN()
