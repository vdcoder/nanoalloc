// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Implementation tests for the binary buddy engine (src/buddy_engine.hpp).
//
// Unlike the public contract tests, these tests exercise the internal engine
// directly and rely on buddy-specific behavior (power-of-two rounding,
// split/merge structure, LIFO free lists).
#include <nanoalloc/pool.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "buddy_engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using nanoalloc::Pool;
using nanoalloc::detail::BuddyEngine;
using nanotest::Rng;

namespace {

constexpr std::size_t kKiB = 1024;
constexpr std::size_t kArenaAlignment = 4096;

/// Owns a 4096-aligned buffer for an engine under test. Declare it before the
/// engine so that the engine is destroyed first.
class TestArena {
public:
    explicit TestArena(std::size_t bytes)
        : data_(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{kArenaAlignment}))) {}
    ~TestArena() { ::operator delete(data_, std::align_val_t{kArenaAlignment}); }
    TestArena(const TestArena&) = delete;
    TestArena& operator=(const TestArena&) = delete;
    [[nodiscard]] std::byte* data() const { return data_; }

private:
    std::byte* data_;
};

std::size_t offset_of(const BuddyEngine& engine, const void* ptr) {
    return static_cast<std::size_t>(static_cast<const std::byte*>(ptr) - engine.base());
}

/// Asserts the engine's internal consistency.
#define REQUIRE_VALID(engine)                                                       \
    do {                                                                            \
        const char* nanotest_error_ = (engine).validate();                          \
        if (nanotest_error_ != nullptr) {                                           \
            ::nanotest::report_failure(__FILE__, __LINE__,                          \
                                       std::string("validate(): ") + nanotest_error_); \
            throw ::nanotest::AbortTest{};                                          \
        }                                                                           \
    } while (false)

}  // namespace

// ---------------------------------------------------------------------------
// Size classes
// ---------------------------------------------------------------------------

NANO_TEST(order_for_size_rounds_up_to_powers_of_two) {
    struct Case {
        std::size_t bytes;
        unsigned order;
    };
    const Case cases[] = {{0, 0},     {1, 0},     {15, 0},    {16, 0},    {17, 1},
                          {31, 1},    {32, 1},    {33, 2},    {63, 2},    {64, 2},
                          {65, 3},    {127, 3},   {128, 3},   {129, 4},   {255, 4},
                          {256, 4},   {257, 5},   {4095, 8},  {4096, 8},  {4097, 9},
                          {1 << 20, 16}, {(1 << 20) + 1, 17}};
    for (const Case& c : cases) {
        CHECK_EQ(BuddyEngine::order_for_size(c.bytes), c.order);
    }
    for (std::size_t bytes = 1; bytes <= 70000; bytes += 7) {
        const std::size_t block = BuddyEngine::block_size(BuddyEngine::order_for_size(bytes));
        CHECK_EQ(block, std::max<std::size_t>(16, std::bit_ceil(bytes)));
    }
}

NANO_TEST(pool_usable_size_is_power_of_two_block) {
    Pool pool({4 * 1024 * kKiB});
    for (std::size_t bytes : {0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255,
                              256, 257, 1000, 4096, 4097, 100000}) {
        for (std::size_t alignment : {1, 16, 64, 1024, 4096}) {
            void* ptr = pool.allocate(bytes, alignment);
            const std::size_t expected =
                std::max({std::size_t{16}, std::bit_ceil(bytes), alignment});
            CHECK_EQ(pool.usable_size(ptr), expected);
            pool.deallocate(ptr);
        }
    }
}

// ---------------------------------------------------------------------------
// Initial state, splitting and buddies
// ---------------------------------------------------------------------------

NANO_TEST(initial_state_is_one_free_root) {
    TestArena arena(1024);
    BuddyEngine engine(arena.data(), 1024);
    CHECK_EQ(engine.top_order(), 6U);
    CHECK_EQ(engine.max_block_size(), 1024U);
    CHECK_EQ(engine.largest_free_block(), 1024U);
    CHECK_EQ(engine.free_bytes(), 1024U);
    CHECK_EQ(engine.free_block_count(6), 1U);
    for (unsigned order = 0; order < 6; ++order) {
        CHECK_EQ(engine.free_block_count(order), 0U);
    }
    CHECK(engine.is_free(6, 0));
    REQUIRE_VALID(engine);
}

NANO_TEST(recursive_splitting_produces_one_free_buddy_per_level) {
    TestArena arena(1024);
    BuddyEngine engine(arena.data(), 1024);
    void* ptr = engine.allocate(0);
    REQUIRE(ptr != nullptr);
    CHECK_EQ(offset_of(engine, ptr), 0U);
    CHECK_EQ(engine.split_count(), 6U);
    CHECK_EQ(engine.merge_count(), 0U);
    for (unsigned order = 0; order < 6; ++order) {
        CHECK_EQ(engine.free_block_count(order), 1U);
        // The free block at each level is the upper buddy of the path taken.
        CHECK(engine.is_free(order, 1));
        CHECK(!engine.is_free(order, 0));
    }
    CHECK_EQ(engine.free_block_count(6), 0U);
    for (unsigned order = 1; order <= 6; ++order) {
        CHECK(engine.is_split(order, 0));
    }
    CHECK_EQ(engine.free_bytes(), 1024U - 16U);
    CHECK_EQ(engine.largest_free_block(), 512U);
    REQUIRE_VALID(engine);

    // A request for an order that is already available performs no split.
    void* mid = engine.allocate(4);
    CHECK_EQ(offset_of(engine, mid), 256U);
    CHECK_EQ(engine.split_count(), 6U);
    REQUIRE_VALID(engine);

    engine.deallocate(mid);
    engine.deallocate(ptr);
    CHECK_EQ(engine.merge_count(), 6U);
    CHECK_EQ(engine.free_block_count(6), 1U);
    REQUIRE_VALID(engine);
}

NANO_TEST(buddies_are_identified_by_address_xor_block_size) {
    TestArena arena(64 * kKiB);
    BuddyEngine engine(arena.data(), 64 * kKiB);
    for (unsigned order = 0; order <= 8; ++order) {
        void* first = engine.allocate(order);
        void* second = engine.allocate(order);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        // Splitting a parent yields the two buddies back to back.
        CHECK_EQ(offset_of(engine, first) ^ BuddyEngine::block_size(order),
                 offset_of(engine, second));
        CHECK_EQ(engine.order_of(first), order);
        CHECK_EQ(engine.order_of(second), order);
        // Freeing one buddy cannot merge while the other is live.
        const std::uint64_t merges = engine.merge_count();
        engine.deallocate(first);
        CHECK_EQ(engine.merge_count(), merges);
        CHECK(engine.is_free(order, engine.granule_of(first) >> order));
        engine.deallocate(second);
        CHECK(engine.merge_count() > merges);
        REQUIRE_VALID(engine);
    }
    CHECK_EQ(engine.free_bytes(), 64 * kKiB);
    CHECK_EQ(engine.free_block_count(engine.top_order()), 1U);
}

NANO_TEST(blocks_are_naturally_aligned) {
    TestArena arena(256 * kKiB);
    BuddyEngine engine(arena.data(), 256 * kKiB);
    Rng rng(17);
    std::vector<void*> live;
    for (int i = 0; i < 2000; ++i) {
        const auto order = static_cast<unsigned>(rng.below(11));
        void* ptr = engine.allocate(order);
        if (ptr == nullptr) {
            continue;
        }
        const std::size_t natural = std::min<std::size_t>(BuddyEngine::block_size(order), 4096);
        CHECK(nanotest::is_aligned(ptr, natural));
        CHECK_EQ(offset_of(engine, ptr) % BuddyEngine::block_size(order), 0U);
        live.push_back(ptr);
        if (rng.chance(50)) {
            const std::size_t victim = rng.below(live.size());
            engine.deallocate(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (void* ptr : live) {
        engine.deallocate(ptr);
    }
    REQUIRE_VALID(engine);
}

// ---------------------------------------------------------------------------
// Merging
// ---------------------------------------------------------------------------

NANO_TEST(adjacent_non_buddies_do_not_merge) {
    TestArena arena(1024);
    BuddyEngine engine(arena.data(), 1024);
    void* a = engine.allocate(0);  // granule 0
    void* b = engine.allocate(0);  // granule 1
    void* c = engine.allocate(0);  // granule 2
    void* d = engine.allocate(0);  // granule 3
    CHECK_EQ(offset_of(engine, a), 0U);
    CHECK_EQ(offset_of(engine, b), 16U);
    CHECK_EQ(offset_of(engine, c), 32U);
    CHECK_EQ(offset_of(engine, d), 48U);

    // b and c are adjacent and both free, but they are not buddies.
    engine.deallocate(b);
    engine.deallocate(c);
    CHECK_EQ(engine.merge_count(), 0U);
    CHECK_EQ(engine.free_block_count(0), 2U);
    REQUIRE_VALID(engine);

    // a merges with its buddy b only; the (c, d) pair is still split.
    engine.deallocate(a);
    CHECK_EQ(engine.merge_count(), 1U);
    CHECK(engine.is_free(1, 0));
    CHECK(engine.is_free(0, 2));
    CHECK(!engine.is_free(1, 1));
    REQUIRE_VALID(engine);

    // d completes every level up to the root.
    engine.deallocate(d);
    CHECK_EQ(engine.merge_count(), 7U);
    CHECK_EQ(engine.free_block_count(6), 1U);
    for (unsigned order = 0; order < 6; ++order) {
        CHECK_EQ(engine.free_block_count(order), 0U);
    }
    REQUIRE_VALID(engine);
}

NANO_TEST(free_buddy_of_a_different_order_is_not_merged) {
    TestArena arena(64);
    BuddyEngine engine(arena.data(), 64);  // 4 granules, top order 2
    void* g0 = engine.allocate(0);
    void* g1 = engine.allocate(0);
    void* g2 = engine.allocate(0);
    void* g3 = engine.allocate(0);
    CHECK(engine.allocate(0) == nullptr);
    engine.deallocate(g2);
    engine.deallocate(g0);
    CHECK_EQ(engine.merge_count(), 0U);
    // (1,0) becomes free; its buddy (1,1) is split, with only (0,2) free.
    engine.deallocate(g1);
    CHECK_EQ(engine.merge_count(), 1U);
    CHECK(engine.is_free(1, 0));
    CHECK(engine.is_free(0, 2));
    CHECK(engine.is_split(1, 1));
    CHECK_EQ(engine.largest_free_block(), 32U);
    REQUIRE_VALID(engine);
    engine.deallocate(g3);
    CHECK_EQ(engine.merge_count(), 3U);
    CHECK(engine.is_free(2, 0));
    CHECK_EQ(engine.largest_free_block(), 64U);
    REQUIRE_VALID(engine);
}

NANO_TEST(multilevel_coalescing_recovers_the_whole_arena) {
    constexpr std::size_t kCapacity = 64 * kKiB;  // 4096 granules, order 12
    TestArena arena(kCapacity);
    BuddyEngine engine(arena.data(), kCapacity);
    std::vector<void*> blocks;
    for (;;) {
        void* ptr = engine.allocate(0);
        if (ptr == nullptr) {
            break;
        }
        blocks.push_back(ptr);
    }
    CHECK_EQ(blocks.size(), kCapacity / 16);
    CHECK_EQ(engine.free_bytes(), 0U);
    CHECK_EQ(engine.largest_free_block(), 0U);
    CHECK_EQ(engine.split_count(), kCapacity / 16 - 1);
    REQUIRE_VALID(engine);

    Rng rng(4242);
    nanotest::shuffle(blocks, rng);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        engine.deallocate(blocks[i]);
        if (i % 512 == 0) {
            REQUIRE_VALID(engine);
        }
    }
    // Every split has been undone exactly once.
    CHECK_EQ(engine.merge_count(), kCapacity / 16 - 1);
    CHECK_EQ(engine.free_block_count(12), 1U);
    CHECK_EQ(engine.largest_free_block(), kCapacity);
    REQUIRE_VALID(engine);
    void* whole = engine.allocate(12);
    CHECK(whole != nullptr);
    CHECK_EQ(offset_of(engine, whole), 0U);
    engine.deallocate(whole);
    REQUIRE_VALID(engine);
}

NANO_TEST(recovery_after_fragmentation_with_mixed_orders) {
    constexpr std::size_t kCapacity = 128 * kKiB;
    TestArena arena(kCapacity);
    BuddyEngine engine(arena.data(), kCapacity);
    Rng rng(777);
    std::vector<void*> live;
    // Fill with random orders until exhaustion at the small end.
    for (int i = 0; i < 20000; ++i) {
        void* ptr = engine.allocate(static_cast<unsigned>(rng.below(6)));
        if (ptr != nullptr) {
            live.push_back(ptr);
        }
    }
    // Free every other block to fragment the arena.
    std::vector<void*> kept;
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (i % 2 == 0) {
            engine.deallocate(live[i]);
        } else {
            kept.push_back(live[i]);
        }
    }
    REQUIRE_VALID(engine);
    CHECK(engine.largest_free_block() < kCapacity);
    for (void* ptr : kept) {
        engine.deallocate(ptr);
    }
    CHECK_EQ(engine.free_bytes(), kCapacity);
    CHECK_EQ(engine.largest_free_block(), kCapacity);
    CHECK_EQ(engine.merge_count(), engine.split_count());
    REQUIRE_VALID(engine);
}

// ---------------------------------------------------------------------------
// Free-list consistency under random operation sequences
// ---------------------------------------------------------------------------

NANO_TEST(free_lists_stay_consistent_under_random_operations) {
    constexpr std::size_t kCapacity = 32 * kKiB;
    TestArena arena(kCapacity);
    BuddyEngine engine(arena.data(), kCapacity);
    Rng rng(99);
    struct Block {
        void* ptr;
        unsigned order;
    };
    std::vector<Block> live;
    std::size_t model_free = kCapacity;
    for (int step = 0; step < 6000; ++step) {
        if (live.empty() || rng.chance(55)) {
            const auto order = static_cast<unsigned>(rng.log_uniform(1, 10) - 1);
            void* ptr = engine.allocate(order);
            if (ptr != nullptr) {
                live.push_back({ptr, order});
                model_free -= BuddyEngine::block_size(order);
                CHECK_EQ(engine.order_of(ptr), order);
            }
        } else {
            const std::size_t victim = rng.below(live.size());
            CHECK_EQ(engine.deallocate(live[victim].ptr), live[victim].order);
            model_free += BuddyEngine::block_size(live[victim].order);
            live[victim] = live.back();
            live.pop_back();
        }
        CHECK_EQ(engine.free_bytes(), model_free);
        std::size_t listed = 0;
        for (unsigned order = 0; order <= engine.top_order(); ++order) {
            listed += engine.free_block_count(order) * BuddyEngine::block_size(order);
        }
        CHECK_EQ(listed, model_free);
        if (step % 10 == 0) {
            REQUIRE_VALID(engine);
        }
    }
    for (const Block& block : live) {
        engine.deallocate(block.ptr);
    }
    CHECK_EQ(engine.free_block_count(engine.top_order()), 1U);
    REQUIRE_VALID(engine);
}

NANO_TEST(free_lists_are_lifo) {
    TestArena arena(4 * kKiB);
    BuddyEngine engine(arena.data(), 4 * kKiB);
    void* keep = engine.allocate(0);  // granule 0; leaves (2,1) free
    void* a = engine.allocate(2);     // granules 4-7, the free (2,1)
    void* b = engine.allocate(2);     // granules 8-11, split from (3,1)
    void* c = engine.allocate(2);     // granules 12-15, b's buddy
    CHECK_EQ(offset_of(engine, a), 64U);
    CHECK_EQ(offset_of(engine, b), 128U);
    CHECK_EQ(offset_of(engine, c), 192U);
    engine.deallocate(a);  // cannot merge: (2,0) is split around `keep`
    engine.deallocate(c);  // cannot merge: its buddy b is live
    CHECK_EQ(engine.free_block_count(2), 2U);
    // The most recently freed block is reused first, repeatedly.
    for (int i = 0; i < 3; ++i) {
        void* again = engine.allocate(2);
        CHECK(again == c);
        engine.deallocate(again);
    }
    engine.deallocate(b);
    engine.deallocate(keep);
    CHECK_EQ(engine.free_bytes(), 4 * kKiB);
    REQUIRE_VALID(engine);
}

// ---------------------------------------------------------------------------
// Non-power-of-two arenas (forest of roots)
// ---------------------------------------------------------------------------

NANO_TEST(non_power_of_two_arena_is_a_forest_of_roots) {
    // 13 granules = 8 + 4 + 1: roots of order 3, 2 and 0.
    TestArena arena(13 * 16);
    BuddyEngine engine(arena.data(), 13 * 16);
    CHECK_EQ(engine.top_order(), 4U);
    CHECK_EQ(engine.max_block_size(), 128U);
    CHECK_EQ(engine.largest_free_block(), 128U);
    CHECK_EQ(engine.free_block_count(3), 1U);
    CHECK_EQ(engine.free_block_count(2), 1U);
    CHECK_EQ(engine.free_block_count(1), 0U);
    CHECK_EQ(engine.free_block_count(0), 1U);
    CHECK(engine.is_split(4, 0));
    CHECK(engine.is_split(3, 1));
    CHECK(engine.is_split(2, 3));
    CHECK(engine.is_split(1, 6));
    REQUIRE_VALID(engine);

    CHECK(engine.allocate(4) == nullptr);  // only the virtual root has order 4
    void* big = engine.allocate(3);
    void* mid = engine.allocate(2);
    void* tail = engine.allocate(0);
    CHECK_EQ(offset_of(engine, big), 0U);
    CHECK_EQ(offset_of(engine, mid), 128U);
    CHECK_EQ(offset_of(engine, tail), 192U);
    CHECK(engine.allocate(0) == nullptr);
    CHECK_EQ(engine.free_bytes(), 0U);
    REQUIRE_VALID(engine);

    // Roots never merge with each other or with the space past the end.
    engine.deallocate(tail);
    engine.deallocate(mid);
    engine.deallocate(big);
    CHECK_EQ(engine.merge_count(), 0U);
    CHECK_EQ(engine.free_block_count(3), 1U);
    CHECK_EQ(engine.free_block_count(2), 1U);
    CHECK_EQ(engine.free_block_count(0), 1U);
    REQUIRE_VALID(engine);
}

NANO_TEST(non_power_of_two_arena_random_operations) {
    for (std::size_t granules : {3U, 5U, 6U, 7U, 100U, 1023U, 1025U, 4097U, 12345U}) {
        const std::size_t capacity = granules * 16;
        TestArena arena(capacity);
        BuddyEngine engine(arena.data(), capacity);
        CHECK_EQ(engine.max_block_size(), std::bit_floor(granules) * 16);
        Rng rng(granules);
        std::vector<void*> live;
        for (int step = 0; step < 3000; ++step) {
            if (live.empty() || rng.chance(55)) {
                void* ptr = engine.allocate(static_cast<unsigned>(rng.below(8)));
                if (ptr != nullptr) {
                    CHECK(engine.owns(ptr));
                    live.push_back(ptr);
                }
            } else {
                const std::size_t victim = rng.below(live.size());
                engine.deallocate(live[victim]);
                live[victim] = live.back();
                live.pop_back();
            }
            if (step % 100 == 0) {
                REQUIRE_VALID(engine);
            }
        }
        for (void* ptr : live) {
            engine.deallocate(ptr);
        }
        CHECK_EQ(engine.free_bytes(), capacity);
        CHECK_EQ(engine.largest_free_block(), std::bit_floor(granules) * 16);
        REQUIRE_VALID(engine);
    }
}

NANO_TEST(pool_with_odd_capacity_exposes_largest_root) {
    Pool pool({3 * 1024 * kKiB});  // 2 MiB + 1 MiB roots
    CHECK_EQ(pool.stats().largest_free_block, 2 * 1024 * kKiB);
    void* two = pool.allocate(2 * 1024 * kKiB);
    void* one = pool.allocate(1024 * kKiB);
    CHECK(pool.try_allocate(1) == nullptr);
    pool.deallocate(two);
    pool.deallocate(one);
    CHECK_EQ(pool.stats().merge_count, 0U);
    CHECK(pool.try_allocate(3 * 1024 * kKiB) == nullptr);  // spans two roots
}

// ---------------------------------------------------------------------------
// Exhaustion, statistics and metadata
// ---------------------------------------------------------------------------

NANO_TEST(exhaustion_at_every_order) {
    TestArena arena(4096);
    BuddyEngine engine(arena.data(), 4096);  // order 8
    for (unsigned order = 0; order <= 8; ++order) {
        std::vector<void*> blocks;
        for (;;) {
            void* ptr = engine.allocate(order);
            if (ptr == nullptr) {
                break;
            }
            blocks.push_back(ptr);
        }
        CHECK_EQ(blocks.size(), std::size_t{1} << (8 - order));
        for (unsigned other = 0; other <= 8; ++other) {
            CHECK(engine.allocate(other) == nullptr);
        }
        CHECK(engine.allocate(9) == nullptr);
        for (void* ptr : blocks) {
            engine.deallocate(ptr);
        }
        CHECK_EQ(engine.free_block_count(8), 1U);
        REQUIRE_VALID(engine);
    }
}

NANO_TEST(pool_reports_split_and_merge_counts) {
    nanoalloc::PoolConfig config;
    config.capacity_bytes = 1024;
    config.track_requested_bytes = true;
    Pool pool(config);
    void* ptr = pool.allocate(1);
    CHECK_EQ(pool.stats().split_count, 6U);
    CHECK_EQ(pool.stats().merge_count, 0U);
    CHECK_EQ(pool.stats().largest_free_block, 512U);
    pool.deallocate(ptr);
    CHECK_EQ(pool.stats().merge_count, 6U);
    CHECK_EQ(pool.stats().largest_free_block, 1024U);
    void* big = pool.allocate(1000);
    CHECK_EQ(pool.usable_size(big), 1024U);
    CHECK_EQ(pool.stats().split_count, 6U);
    CHECK_EQ(pool.stats().live_usable_bytes, 1024U);
    CHECK_EQ(pool.stats().live_requested_bytes, 1000U);
    pool.deallocate(big);
}

NANO_TEST(metadata_costs_about_three_bits_per_granule) {
    for (std::size_t capacity : {std::size_t{1} << 16, std::size_t{1} << 20, std::size_t{1} << 24,
                                 (std::size_t{3} << 20) + 48}) {
        TestArena arena(capacity);
        BuddyEngine engine(arena.data(), capacity);
        const std::size_t granules = capacity / 16;
        const std::size_t ideal_bits = 3 * granules;
        // Per-order rounding to whole words adds at most two words per order.
        const std::size_t slack_bits = 2 * 64 * (engine.top_order() + 1);
        CHECK(engine.metadata_bytes() * 8 >= ideal_bits - 2 * (engine.top_order() + 1));
        CHECK(engine.metadata_bytes() * 8 <= ideal_bits + slack_bits);
    }
}

NANOTEST_MAIN()
