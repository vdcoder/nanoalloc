// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Deterministic randomized stress tests for nanoalloc::Pool (public contract
// only; no dependence on implementation details).
//
// Every scenario keeps a record of each live allocation, fills its whole
// usable range with a per-allocation pattern, and continuously checks:
//   * alignment, ownership and usable_size() >= requested size;
//   * no overlap between the usable ranges of live allocations;
//   * pattern integrity of the allocation being released, and of *all* live
//     allocations at regular intervals during the run;
//   * pool statistics against an independent model;
//   * the pool's internal consistency (verify_integrity) at intervals;
//   * full recovery of the arena at the end.
//
// Environment overrides (useful to reproduce or extend a run):
//   NANOALLOC_STRESS_SEED=<n>   run every scenario with this seed only
//   NANOALLOC_STRESS_SCALE=<f>  multiply operation counts by <f>
#include <nanoalloc/pool.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"

using nanoalloc::Pool;
using nanoalloc::PoolConfig;
using nanoalloc::PoolStats;
using nanotest::Rng;

namespace {

constexpr std::size_t kKiB = 1024;
constexpr std::size_t kMiB = 1024 * 1024;

enum class SizeProfile { Mixed, Small, LargeAndSmall, ExactPowers };

struct Scenario {
    const char* name;
    std::size_t capacity;
    std::size_t operations;
    SizeProfile profile;
    unsigned aligned_percent;   // share of requests with non-default alignment
    std::size_t target_live;    // live-set size around which the run oscillates
    bool track_requested;
    bool use_throwing_api;      // allocate() instead of try_allocate()
};

struct LiveRecord {
    unsigned char* ptr;
    std::size_t size;
    std::size_t usable;
    std::uint64_t tag;
};

double stress_scale() {
    if (const char* text = std::getenv("NANOALLOC_STRESS_SCALE")) {
        const double value = std::strtod(text, nullptr);
        if (value > 0) {
            return value;
        }
    }
    return 1.0;
}

std::vector<std::uint64_t> stress_seeds() {
    if (const char* text = std::getenv("NANOALLOC_STRESS_SEED")) {
        return {std::strtoull(text, nullptr, 0)};
    }
    return {0x1234ULL, 0xC0FFEEULL, 0xDEADBEEFULL, 20240101ULL};
}

std::size_t draw_size(SizeProfile profile, Rng& rng) {
    switch (profile) {
        case SizeProfile::Small:
            return rng.chance(2) ? 0 : rng.between(1, 256);
        case SizeProfile::LargeAndSmall:
            return rng.chance(8) ? rng.between(16 * kKiB, 256 * kKiB) : rng.log_uniform(1, 512);
        case SizeProfile::ExactPowers: {
            // power - 1, power or power + 1 for powers of two from 2 to 16 KiB.
            const std::size_t power = std::size_t{1} << rng.between(1, 14);
            return power - 1 + rng.below(3);
        }
        case SizeProfile::Mixed:
        default:
            if (rng.chance(1)) {
                return 0;
            }
            return rng.log_uniform(1, 64 * kKiB);
    }
}

std::size_t draw_alignment(unsigned aligned_percent, Rng& rng) {
    if (!rng.chance(aligned_percent)) {
        return Pool::min_alignment;
    }
    return std::size_t{1} << rng.between(0, 12);  // 1 .. 4096
}

/// Fills or checks the full usable range: requested bytes with `tag`, the
/// remainder with `~tag`.
void fill_record(const LiveRecord& record) {
    nanotest::fill_pattern(record.ptr, record.size, record.tag);
    nanotest::fill_pattern(record.ptr + record.size, record.usable - record.size, ~record.tag);
}

bool check_record(const LiveRecord& record) {
    return nanotest::check_pattern(record.ptr, record.size, record.tag) &&
           nanotest::check_pattern(record.ptr + record.size, record.usable - record.size,
                                   ~record.tag);
}

void run_scenario(const Scenario& scenario, std::uint64_t seed) {
    std::printf("  scenario=%s seed=0x%" PRIx64 "\n", scenario.name, seed);
    std::fflush(stdout);

    PoolConfig config;
    config.capacity_bytes = scenario.capacity;
    config.track_requested_bytes = scenario.track_requested;
    Pool pool(config);
    const PoolStats initial = pool.stats();

    Rng rng(seed);
    std::vector<LiveRecord> live;
    nanotest::IntervalSet intervals;
    std::size_t model_requested = 0;
    std::size_t model_usable = 0;
    std::size_t model_peak_requested = 0;
    std::uint64_t model_allocations = 0;
    std::uint64_t model_deallocations = 0;
    std::uint64_t failures = 0;

    const auto operations =
        static_cast<std::size_t>(static_cast<double>(scenario.operations) * stress_scale());
    const std::size_t check_interval = std::max<std::size_t>(operations / 16, 64);

    auto release = [&](std::size_t index) {
        LiveRecord record = live[index];
        REQUIRE(check_record(record));
        REQUIRE_EQ(pool.usable_size(record.ptr), record.usable);
        intervals.erase(record.ptr);
        pool.deallocate(record.ptr);
        model_requested -= record.size;
        model_usable -= record.usable;
        ++model_deallocations;
        live[index] = live.back();
        live.pop_back();
    };

    auto verify_model = [&] {
        const PoolStats stats = pool.stats();
        REQUIRE_EQ(stats.live_allocations, live.size());
        REQUIRE_EQ(stats.live_usable_bytes, model_usable);
        if (scenario.track_requested) {
            REQUIRE_EQ(stats.live_requested_bytes, model_requested);
            REQUIRE_EQ(stats.peak_requested_bytes, model_peak_requested);
        } else {
            REQUIRE_EQ(stats.live_requested_bytes, 0U);
        }
        REQUIRE(stats.live_consumed_bytes >= stats.live_usable_bytes);
        REQUIRE_EQ(stats.free_bytes, stats.capacity_bytes - stats.live_consumed_bytes);
        REQUIRE_EQ(stats.allocation_count, model_allocations);
        REQUIRE_EQ(stats.deallocation_count, model_deallocations);
        REQUIRE_EQ(stats.failed_allocation_count, failures);
    };

    for (std::size_t op = 0; op < operations; ++op) {
        // Bias towards allocation while below the target live-set size.
        const unsigned allocate_percent = live.size() < scenario.target_live ? 65U : 35U;
        if (live.empty() || rng.chance(allocate_percent)) {
            const std::size_t size = draw_size(scenario.profile, rng);
            const std::size_t alignment = draw_alignment(scenario.aligned_percent, rng);
            void* raw = nullptr;
            if (scenario.use_throwing_api) {
                try {
                    raw = pool.allocate(size, alignment);
                } catch (const std::bad_alloc&) {
                    raw = nullptr;
                }
            } else {
                raw = pool.try_allocate(size, alignment);
            }
            if (raw == nullptr) {
                ++failures;
                // Failure must not have changed anything; release some memory.
                verify_model();
                for (int i = 0; i < 8 && !live.empty(); ++i) {
                    release(rng.below(live.size()));
                }
                continue;
            }
            auto* ptr = static_cast<unsigned char*>(raw);
            REQUIRE(nanotest::is_aligned(ptr, std::max(alignment, Pool::min_alignment)));
            REQUIRE(pool.owns(ptr));
            const std::size_t usable = pool.usable_size(ptr);
            REQUIRE(usable >= size);
            REQUIRE(pool.owns(ptr + (usable == 0 ? 0 : usable - 1)));
            REQUIRE(intervals.insert(ptr, usable));
            LiveRecord record{ptr, size, usable, rng.next()};
            fill_record(record);
            live.push_back(record);
            model_requested += size;
            model_usable += usable;
            model_peak_requested = std::max(model_peak_requested, model_requested);
            ++model_allocations;
        } else {
            release(rng.below(live.size()));
        }

        if (op % check_interval == 0) {
            for (const LiveRecord& record : live) {
                REQUIRE(check_record(record));
            }
            verify_model();
            REQUIRE(pool.verify_integrity());
        }
    }

    verify_model();
    REQUIRE(pool.verify_integrity());
    nanotest::shuffle(live, rng);
    while (!live.empty()) {
        release(live.size() - 1);
    }
    const PoolStats final_stats = pool.stats();
    CHECK_EQ(final_stats.live_allocations, 0U);
    CHECK_EQ(final_stats.live_requested_bytes, 0U);
    CHECK_EQ(final_stats.live_usable_bytes, 0U);
    CHECK_EQ(final_stats.free_bytes, initial.free_bytes);
    CHECK_EQ(final_stats.largest_free_block, initial.largest_free_block);
    CHECK(pool.verify_integrity());
    std::printf("    %" PRIu64 " allocations, %" PRIu64 " failures, peak requested %zu bytes\n",
                model_allocations, failures, model_peak_requested);
}

void run_all_seeds(const Scenario& scenario) {
    for (std::uint64_t seed : stress_seeds()) {
        run_scenario(scenario, seed);
    }
}

}  // namespace

NANO_TEST(stress_mixed_sizes) {
    run_all_seeds({"mixed", 16 * kMiB, 40000, SizeProfile::Mixed, 5, 600, true, false});
}

NANO_TEST(stress_small_objects) {
    run_all_seeds({"small", 4 * kMiB, 120000, SizeProfile::Small, 3, 5000, true, true});
}

NANO_TEST(stress_large_and_small_objects) {
    run_all_seeds(
        {"large+small", 32 * kMiB, 25000, SizeProfile::LargeAndSmall, 2, 400, true, false});
}

NANO_TEST(stress_alignment_heavy) {
    run_all_seeds({"aligned", 16 * kMiB, 30000, SizeProfile::Mixed, 60, 400, true, true});
}

NANO_TEST(stress_power_of_two_boundaries) {
    run_all_seeds({"boundaries", 16 * kMiB, 40000, SizeProfile::ExactPowers, 5, 800, true, false});
}

NANO_TEST(stress_near_exhaustion) {
    // A small arena and a large target live set keep the pool full, so the
    // run constantly alternates between exhaustion and recovery.
    run_all_seeds({"exhaustion", 512 * kKiB, 40000, SizeProfile::Mixed, 5, 100000, true, true});
}

NANO_TEST(stress_non_power_of_two_capacity) {
    run_all_seeds(
        {"odd-capacity", 3 * kMiB + 7 * kKiB + 48, 40000, SizeProfile::Mixed, 5, 300, true, false});
}

NANO_TEST(stress_without_requested_tracking) {
    run_all_seeds({"untracked", 8 * kMiB, 30000, SizeProfile::Mixed, 5, 400, false, false});
}

NANOTEST_MAIN()
