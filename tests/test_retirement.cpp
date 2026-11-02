// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Allocations that outlive their thread. When a thread exits while its heap
// still holds allocated blocks, the heap is retired: handed to the pool's
// reclamation worker, which processes frees of its blocks (from any thread,
// through the heap's inbox), returns spans that become empty to the pool and
// makes the heap reusable once it holds nothing. The exiting thread may not
// allocate any more.
//
// Reclamation is asynchronous, so the tests wait for it through the public
// statistics (retired heaps and pending frees reaching zero). Only the public
// API is used. Intended to be run under ASan and ThreadSanitizer as well.
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"
#include "test_stats.hpp"

using nanoalloc::Pool;
using nanoalloc::PoolStats;
using nanotest::Rng;

namespace {

constexpr std::size_t kMiB = 1024 * 1024;

struct Item {
    void* ptr;
    std::size_t size;
    std::uint64_t tag;
};

Item make_item(Pool& pool, Rng& rng, std::size_t max_size) {
    const std::size_t size = rng.log_uniform(1, max_size);
    void* ptr = pool.allocate(size);
    const std::uint64_t tag = rng.next();
    nanotest::fill_pattern(ptr, size, tag);
    return {ptr, size, tag};
}

bool check_item(const Item& item) { return nanotest::check_pattern(item.ptr, item.size, item.tag); }

/// Runs `body` in a new thread and waits for it to exit.
template <class Body>
void in_thread(Body&& body) {
    std::thread thread(std::forward<Body>(body));
    thread.join();
}

/// Allocates `count` items in a thread that then exits.
std::vector<Item> allocate_in_exited_thread(Pool& pool, std::size_t count, std::size_t max_size,
                                            std::uint64_t seed) {
    std::vector<Item> items;
    in_thread([&] {
        Rng rng(seed);
        for (std::size_t i = 0; i < count; ++i) {
            items.push_back(make_item(pool, rng, max_size));
        }
    });
    return items;
}

void busy_wait_us(unsigned microseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
    while (std::chrono::steady_clock::now() < end) {
    }
}

// ---- pool use from thread_local destructors ------------------------------------

/// Used from a thread_local destructor. Touched before the thread's first
/// allocation, the object is destroyed after the pool processed the exit.
struct ExitTimeUser {
    Pool* pool = nullptr;
    Pool* other_pool = nullptr;  // a pool the thread never used
    std::vector<Item> blocks;    // freed by the destructor
    bool try_to_allocate = false;

    ExitTimeUser() = default;
    ExitTimeUser(const ExitTimeUser&) = delete;
    ExitTimeUser& operator=(const ExitTimeUser&) = delete;
    ~ExitTimeUser();
};

struct ExitTimeResults {
    std::atomic<bool> intact{true};
    std::atomic<unsigned> destructors{0};
    std::atomic<unsigned> refused{0};     // allocation attempts that failed as required
    std::atomic<unsigned> succeeded{0};   // allocation attempts that wrongly succeeded

    void reset() {
        intact = true;
        destructors = 0;
        refused = 0;
        succeeded = 0;
    }
};

ExitTimeResults g_exit;
thread_local ExitTimeUser t_exit_user;

ExitTimeUser::~ExitTimeUser() {
    if (pool == nullptr) {
        return;
    }
    for (const Item& item : blocks) {
        if (!check_item(item)) {
            g_exit.intact = false;
        }
        pool->deallocate(item.ptr);
    }
    if (try_to_allocate) {
        const auto expect_null = [](void* ptr, Pool& owner) {
            if (ptr == nullptr) {
                g_exit.refused.fetch_add(1);
            } else {
                g_exit.succeeded.fetch_add(1);
                owner.deallocate(ptr);
            }
        };
        try {
            void* ptr = pool->allocate(64);
            g_exit.succeeded.fetch_add(1);
            pool->deallocate(ptr);
        } catch (const std::bad_alloc&) {
            g_exit.refused.fetch_add(1);
        }
        expect_null(pool->try_allocate(64), *pool);
        expect_null(pool->try_allocate(64, 256), *pool);       // aligned
        expect_null(pool->try_allocate(2 * kMiB), *pool);      // oversized
        nanoalloc::MemoryResource resource(*pool);
        try {
            void* ptr = resource.allocate(100);
            g_exit.succeeded.fetch_add(1);
            resource.deallocate(ptr, 100);
        } catch (const std::bad_alloc&) {
            g_exit.refused.fetch_add(1);
        }
        expect_null(other_pool->try_allocate(64), *other_pool);
    }
    g_exit.destructors.fetch_add(1);
}

}  // namespace

// 1. A worker allocates and exits; another thread frees its blocks.
NANO_TEST(blocks_outlive_their_thread_and_are_freed_by_another) {
    Pool pool({32 * kMiB});
    std::vector<Item> items = allocate_in_exited_thread(pool, 2000, 100000, 11);
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.retired_thread_heap_count, 1U);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    CHECK_EQ(stats.live_allocations, items.size());
    CHECK(stats.span_count >= 3);

    // The blocks stay valid after their owner exited.
    bool intact = true;
    for (std::size_t i = 0; i < items.size(); i += 2) {
        intact = intact && check_item(items[i]);
        pool.deallocate(items[i].ptr);
    }
    CHECK(nanotest::wait_for_no_pending(pool, stats));
    CHECK_EQ(stats.live_allocations, items.size() / 2);
    CHECK_EQ(stats.retired_thread_heap_count, 1U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    for (std::size_t i = 1; i < items.size(); i += 2) {
        intact = intact && check_item(items[i]);
        pool.deallocate(items[i].ptr);
    }
    CHECK(intact);
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.remote_deallocation_count, items.size());
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK_EQ(stats.free_span_count, stats.span_count);  // every span went back
    CHECK_EQ(stats.thread_free_bytes, 0U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    // The heap is reusable: the next thread adopts it.
    in_thread([&] { pool.deallocate(pool.allocate(64)); });
    CHECK_EQ(pool.stats().thread_heap_count, 1U);
}

// 2. Several retired heaps receive concurrent remote frees.
NANO_TEST(several_retired_heaps_receive_concurrent_remote_frees) {
    Pool pool({128 * kMiB});
    constexpr unsigned kHeaps = 6;
    constexpr unsigned kFreers = 4;
    std::vector<Item> all;
    for (unsigned h = 0; h < kHeaps; ++h) {
        std::vector<Item> items = allocate_in_exited_thread(pool, 3000, 4096, 100 + h);
        all.insert(all.end(), items.begin(), items.end());
    }
    Rng rng(5);
    nanotest::shuffle(all, rng);  // every freer frees into every heap
    CHECK_EQ(pool.stats().retired_thread_heap_count, kHeaps);

    std::atomic<bool> intact{true};
    std::vector<std::thread> freers;
    for (unsigned f = 0; f < kFreers; ++f) {
        freers.emplace_back([&, f] {
            for (std::size_t i = f; i < all.size(); i += kFreers) {
                if (!check_item(all[i])) {
                    intact = false;
                }
                pool.deallocate(all[i].ptr);
            }
        });
    }
    for (std::thread& freer : freers) {
        freer.join();
    }
    CHECK(intact.load());
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.thread_heap_count, kHeaps);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    CHECK_EQ(stats.allocation_count, all.size());
    CHECK_EQ(stats.remote_deallocation_count, all.size());
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

// 3. The last allocation is freed while the worker drains the others.
NANO_TEST(final_allocation_freed_while_the_collector_drains) {
    Pool pool({32 * kMiB});
    constexpr unsigned kRounds = 30;
    constexpr std::size_t kItems = 600;
    Rng timing(9);
    bool intact = true;
    bool recycled = true;
    for (unsigned round = 0; round < kRounds; ++round) {
        std::vector<Item> items = allocate_in_exited_thread(pool, kItems, 2048, round);
        for (std::size_t i = 0; i + 1 < items.size(); ++i) {
            intact = intact && check_item(items[i]);
            pool.deallocate(items[i].ptr);
        }
        busy_wait_us(static_cast<unsigned>(timing.below(3000)));  // 0-3 ms
        intact = intact && check_item(items.back());
        pool.deallocate(items.back().ptr);
        PoolStats stats{};
        recycled = recycled && nanotest::wait_for_reclamation(pool, stats) &&
                   stats.live_allocations == 0 && stats.thread_heap_count == 1;
    }
    CHECK(intact);
    CHECK(recycled);
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.thread_heap_count, 1U);  // every round reused the heap
    CHECK_EQ(stats.remote_deallocation_count, std::uint64_t{kRounds} * kItems);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

// 4. A thread_local destructor frees memory after the retirement.
NANO_TEST(thread_local_destructor_frees_after_retirement) {
    Pool pool({16 * kMiB});
    g_exit.reset();
    in_thread([&] {
        t_exit_user.pool = &pool;  // constructed before the first allocation
        Rng rng(77);
        for (int i = 0; i < 200; ++i) {
            t_exit_user.blocks.push_back(make_item(pool, rng, 100000));
        }
    });
    CHECK_EQ(g_exit.destructors.load(), 1U);
    CHECK(g_exit.intact.load());
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, 200U);
    // After the retirement the thread frees through its heap's inbox.
    CHECK_EQ(stats.remote_deallocation_count, 200U);
    CHECK_EQ(stats.local_deallocation_count, 0U);
    CHECK(stats.span_count >= 2);
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    in_thread([&] { pool.deallocate(pool.allocate(64)); });
    CHECK_EQ(pool.stats().thread_heap_count, 1U);
}

// 5. Allocation attempts after the retirement fail.
NANO_TEST(allocation_after_retirement_fails) {
    Pool pool({16 * kMiB});
    Pool other({16 * kMiB});
    g_exit.reset();
    in_thread([&] {
        t_exit_user.pool = &pool;
        t_exit_user.other_pool = &other;
        t_exit_user.try_to_allocate = true;
        Rng rng(78);
        t_exit_user.blocks.push_back(make_item(pool, rng, 512));
    });
    CHECK_EQ(g_exit.destructors.load(), 1U);
    CHECK(g_exit.intact.load());
    CHECK_EQ(g_exit.refused.load(), 6U);
    CHECK_EQ(g_exit.succeeded.load(), 0U);
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.failed_allocation_count, 5U);
    CHECK_EQ(stats.allocation_count, 1U);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.oversized_allocation_count, 0U);
    CHECK_EQ(stats.thread_heap_count, 1U);  // no replacement heap
    const PoolStats other_stats = other.stats();
    CHECK_EQ(other_stats.failed_allocation_count, 1U);
    CHECK_EQ(other_stats.thread_heap_count, 0U);
    CHECK(pool.verify_integrity());
}

// 6. Empty spans return to the pool and are reused by other threads.
NANO_TEST(empty_spans_of_retired_heaps_are_reused) {
    Pool pool({64 * kMiB});
    std::vector<void*> blocks;
    in_thread([&] {
        for (int i = 0; i < 60; ++i) {
            blocks.push_back(pool.allocate(100000));
        }
    });
    const PoolStats retired = pool.stats();
    CHECK(retired.span_count >= 5);
    CHECK_EQ(retired.free_span_count, 0U);
    for (void* ptr : blocks) {
        pool.deallocate(ptr);
    }
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.free_span_count, retired.span_count);
    CHECK_EQ(stats.free_span_bytes, retired.span_bytes);
    CHECK_EQ(stats.reserved_bytes, retired.reserved_bytes);

    // A new thread (on a new heap) takes the returned spans; nothing new is
    // carved from the arena.
    std::atomic<bool> hold{true};
    std::atomic<bool> allocated{false};
    std::thread user([&] {
        std::vector<void*> mine;
        for (int i = 0; i < 60; ++i) {
            mine.push_back(pool.allocate(100000));
        }
        allocated = true;
        while (hold.load()) {
            std::this_thread::yield();
        }
        for (void* ptr : mine) {
            pool.deallocate(ptr);
        }
    });
    while (!allocated.load()) {
        std::this_thread::yield();
    }
    stats = pool.stats();
    CHECK_EQ(stats.span_count, retired.span_count);
    CHECK_EQ(stats.reserved_bytes, retired.reserved_bytes);
    CHECK_EQ(stats.free_span_count, 0U);
    hold = false;
    user.join();
    CHECK(pool.verify_integrity());
}

// 7. A heap is not reused while frees of its blocks are still in flight.
NANO_TEST(heaps_are_not_recycled_while_frees_are_in_flight) {
    Pool pool({128 * kMiB});
    std::atomic<bool> stop{false};
    std::atomic<bool> churn_intact{true};
    // New threads keep adopting heaps as soon as they become reusable. A heap
    // reused too early would receive a late free from a producer, which
    // corrupts its lists and is caught by the pattern checks or the final
    // integrity check.
    std::thread churn([&] {
        std::uint64_t seed = 1;
        while (!stop.load()) {
            in_thread([&] {
                Rng rng(seed);
                std::vector<Item> mine;
                for (int i = 0; i < 50; ++i) {
                    mine.push_back(make_item(pool, rng, 512));
                }
                for (const Item& item : mine) {
                    if (!check_item(item)) {
                        churn_intact = false;
                    }
                    pool.deallocate(item.ptr);
                }
            });
            ++seed;
        }
    });

    constexpr unsigned kRounds = 20;
    constexpr unsigned kProducers = 4;
    std::atomic<bool> intact{true};
    for (unsigned round = 0; round < kRounds; ++round) {
        std::vector<Item> items = allocate_in_exited_thread(pool, 400, 1024, 500 + round);
        std::vector<std::thread> producers;
        for (unsigned p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                Rng rng(round * 10 + p);
                for (std::size_t i = p; i < items.size(); i += kProducers) {
                    if (!check_item(items[i])) {
                        intact = false;
                    }
                    pool.deallocate(items[i].ptr);
                    if (rng.chance(5)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread& producer : producers) {
            producer.join();
        }
    }
    stop = true;
    churn.join();
    CHECK(intact.load());
    CHECK(churn_intact.load());
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

// 8. Thousands of short-lived threads do not accumulate heap metadata.
NANO_TEST(thousands_of_short_lived_threads_keep_metadata_bounded) {
    Pool pool({128 * kMiB});
    constexpr unsigned kBatches = 50;
    constexpr unsigned kBatchSize = 40;  // 2000 threads in total
    std::mutex mutex;
    std::vector<Item> handed;
    std::size_t first_spans = 0;
    std::size_t first_reserved = 0;
    std::size_t first_heaps = 0;
    bool intact = true;
    bool reclaimed = true;
    for (unsigned batch = 0; batch < kBatches; ++batch) {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < kBatchSize; ++t) {
            threads.emplace_back([&, batch, t] {
                Rng rng(batch * 1000 + t);
                Item items[3] = {make_item(pool, rng, 64), make_item(pool, rng, 3000),
                                 make_item(pool, rng, 50000)};
                std::lock_guard<std::mutex> lock(mutex);
                handed.insert(handed.end(), items, items + 3);
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        // Every allocating thread has exited; its blocks are freed now.
        for (const Item& item : handed) {
            intact = intact && check_item(item);
            pool.deallocate(item.ptr);
        }
        handed.clear();
        PoolStats stats{};
        reclaimed = reclaimed && nanotest::wait_for_reclamation(pool, stats) &&
                    stats.live_allocations == 0;
        if (batch == 0) {
            first_spans = stats.span_count;
            first_reserved = stats.reserved_bytes;
            first_heaps = stats.thread_heap_count;
        }
    }
    CHECK(intact);
    CHECK(reclaimed);
    const PoolStats stats = pool.stats();
    CHECK(first_heaps <= kBatchSize);
    CHECK_EQ(stats.thread_heap_count, first_heaps);  // heaps are reused
    CHECK_EQ(stats.span_count, first_spans);          // spans are reused
    CHECK_EQ(stats.reserved_bytes, first_reserved);
    CHECK_EQ(stats.allocation_count, std::uint64_t{kBatches} * kBatchSize * 3);
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

// 9. Destroying a pool while the worker holds or drains its heaps.
NANO_TEST(pool_destruction_coordinates_with_the_worker) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto pool = std::make_unique<Pool>(nanoalloc::PoolConfig{8 * kMiB});
        std::vector<Item> items;
        for (int t = 0; t < 3; ++t) {
            std::vector<Item> mine = allocate_in_exited_thread(*pool, 200, 4096, iteration * 3 + t);
            items.insert(items.end(), mine.begin(), mine.end());
        }
        // Free some blocks so that the worker has inboxes to drain, and
        // destroy the pool right away (the rest stay allocated).
        const std::size_t freed = static_cast<std::size_t>(iteration % 4) * items.size() / 4;
        in_thread([&] {
            for (std::size_t i = 0; i < freed; ++i) {
                pool->deallocate(items[i].ptr);
            }
        });
        if (iteration % 7 == 0) {
            busy_wait_us(500);
        }
        pool.reset();
    }
    // The worker keeps serving other pools.
    Pool pool({8 * kMiB});
    std::vector<Item> items = allocate_in_exited_thread(pool, 100, 512, 4242);
    for (const Item& item : items) {
        pool.deallocate(item.ptr);
    }
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

// 10. Statistics across retirement, draining and heap reuse.
NANO_TEST(statistics_across_retirement_draining_and_recycling) {
    Pool pool({16 * kMiB});
    std::vector<void*> blocks;
    in_thread([&] {
        for (int i = 0; i < 100; ++i) {
            blocks.push_back(pool.allocate(1000));  // 100,000 requested bytes
        }
    });
    PoolStats retired = pool.stats();
    CHECK_EQ(retired.retired_thread_heap_count, 1U);
    CHECK_EQ(retired.allocation_count, 100U);
    CHECK_EQ(retired.live_allocations, 100U);
    CHECK_EQ(retired.live_requested_bytes, 100000U);
    CHECK_EQ(retired.peak_requested_bytes, 100000U);
    CHECK(nanotest::stats_are_consistent(retired));

    for (int i = 0; i < 40; ++i) {
        pool.deallocate(blocks[static_cast<std::size_t>(i)]);
    }
    PoolStats drained{};
    CHECK(nanotest::wait_for_no_pending(pool, drained));
    CHECK_EQ(drained.live_allocations, 60U);
    CHECK_EQ(drained.remote_deallocation_count, 40U);
    CHECK_EQ(drained.deallocation_count, 40U);
    CHECK_EQ(drained.retired_thread_heap_count, 1U);
    CHECK(nanotest::history_is_monotonic(retired, drained));
    CHECK(nanotest::stats_are_consistent(drained));

    for (int i = 40; i < 100; ++i) {
        pool.deallocate(blocks[static_cast<std::size_t>(i)]);
    }
    PoolStats reclaimed{};
    CHECK(nanotest::wait_for_reclamation(pool, reclaimed));
    CHECK_EQ(reclaimed.live_allocations, 0U);
    CHECK_EQ(reclaimed.allocation_count, 100U);
    CHECK_EQ(reclaimed.deallocation_count, 100U);
    CHECK_EQ(reclaimed.thread_heap_count, 1U);
    CHECK_EQ(reclaimed.retired_thread_heap_count, 0U);
    CHECK(nanotest::history_is_monotonic(drained, reclaimed));
    CHECK(nanotest::stats_are_consistent(reclaimed));

    // A new thread adopts the heap; the history continues.
    in_thread([&] {
        std::vector<void*> mine;
        for (int i = 0; i < 30; ++i) {
            mine.push_back(pool.allocate(500));
        }
        for (void* ptr : mine) {
            pool.deallocate(ptr);
        }
    });
    const PoolStats reused = pool.stats();
    CHECK_EQ(reused.thread_heap_count, 1U);
    CHECK_EQ(reused.allocation_count, 130U);
    CHECK_EQ(reused.deallocation_count, 130U);
    CHECK_EQ(reused.local_deallocation_count, 30U);
    CHECK_EQ(reused.remote_deallocation_count, 100U);
    CHECK_EQ(reused.peak_requested_bytes, 100000U);  // the first thread's peak
    CHECK(nanotest::history_is_monotonic(reclaimed, reused));
    CHECK(nanotest::stats_are_consistent(reused));
    CHECK(pool.verify_integrity());
}

// Randomized thread exits and remote-free timing.
NANO_TEST(randomized_retirement_stress) {
    Pool pool({256 * kMiB});
    constexpr unsigned kThreads = 300;
    constexpr unsigned kConcurrent = 6;
    constexpr unsigned kFreers = 2;
    std::mutex mutex;
    std::vector<Item> orphans;  // blocks handed over by threads that may have exited
    std::atomic<bool> producing{true};
    std::atomic<bool> intact{true};
    std::atomic<std::uint64_t> allocated{0};

    std::vector<std::thread> freers;
    for (unsigned f = 0; f < kFreers; ++f) {
        freers.emplace_back([&, f] {
            Rng rng(9000 + f);
            for (;;) {
                Item item{};
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (orphans.empty()) {
                        if (!producing.load()) {
                            return;
                        }
                    } else {
                        const std::size_t index = rng.below(orphans.size());
                        item = orphans[index];
                        orphans[index] = orphans.back();
                        orphans.pop_back();
                    }
                }
                if (item.ptr == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                if (!check_item(item)) {
                    intact = false;
                }
                pool.deallocate(item.ptr);
                if (rng.chance(3)) {
                    busy_wait_us(static_cast<unsigned>(rng.below(200)));
                }
            }
        });
    }

    Rng spawner(77);
    std::vector<std::thread> running;
    for (unsigned t = 0; t < kThreads; ++t) {
        if (running.size() == kConcurrent) {
            const std::size_t index = spawner.below(running.size());
            running[index].join();
            running[index] = std::move(running.back());
            running.pop_back();
        }
        running.emplace_back([&, t] {
            Rng rng(t);
            std::vector<Item> mine;
            const std::size_t count = rng.between(10, 300);
            for (std::size_t i = 0; i < count; ++i) {
                mine.push_back(make_item(pool, rng, rng.chance(10) ? 60000 : 700));
                allocated.fetch_add(1);
                if (rng.chance(20) && !mine.empty()) {
                    const std::size_t index = rng.below(mine.size());
                    if (!check_item(mine[index])) {
                        intact = false;
                    }
                    pool.deallocate(mine[index].ptr);  // local
                    mine[index] = mine.back();
                    mine.pop_back();
                }
            }
            {
                // Hand the rest over; they are freed whenever a freer gets to
                // them, before or after this thread exits.
                std::lock_guard<std::mutex> lock(mutex);
                orphans.insert(orphans.end(), mine.begin(), mine.end());
            }
            if (rng.chance(30)) {
                busy_wait_us(static_cast<unsigned>(rng.below(2000)));
            }
        });
    }
    for (std::thread& thread : running) {
        thread.join();
    }
    producing = false;
    for (std::thread& freer : freers) {
        freer.join();
    }
    CHECK(intact.load());
    PoolStats stats{};
    CHECK(nanotest::wait_for_reclamation(pool, stats));
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, allocated.load());
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK(stats.thread_heap_count <= kThreads);
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANOTEST_MAIN()
