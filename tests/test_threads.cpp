// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Multithreaded contract tests: cross-thread deallocation through the owner's
// inbox, explicit and implicit draining, concurrent producers while the owner
// drains, thread exit under the documented lifetime contract, reuse of the
// heaps of exited threads, statistics across thread churn, and pool use from
// thread_local destructors. Intended to be run under ThreadSanitizer as well.
//
// All test-harness synchronization (mutexes, atomics, joins) happens outside
// the pool. Every cross-thread access or free of a block completes, through
// that synchronization, before the block's owner begins to terminate, as the
// thread lifetime contract requires; blocks left by an exited thread are
// never touched again.
#include <nanoalloc/pool.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"
#include "test_stats.hpp"

using nanoalloc::Pool;
using nanoalloc::PoolConfig;
using nanoalloc::PoolStats;
using nanotest::Rng;

namespace {

constexpr std::size_t kMiB = 1024 * 1024;

struct Item {
    void* ptr;
    std::size_t size;
    std::uint64_t tag;
};

Item make_item(Pool& pool, Rng& rng, std::size_t max_size = 1024) {
    const std::size_t size = rng.log_uniform(1, max_size);
    void* ptr = pool.allocate(size);
    const std::uint64_t tag = rng.next();
    nanotest::fill_pattern(ptr, size, tag);
    return {ptr, size, tag};
}

bool check_item(const Item& item) { return nanotest::check_pattern(item.ptr, item.size, item.tag); }

unsigned worker_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware < 2 ? 2 : (hardware > 8 ? 8 : hardware);
}

/// Waits until `counter` reaches `target` (a simple spinning barrier).
void wait_for(const std::atomic<unsigned>& counter, unsigned target) {
    while (counter.load() < target) {
        std::this_thread::yield();
    }
}

/// Uses a pool from a thread_local destructor. The object is constructed when
/// a thread first touches it; touched before the thread's first allocation, its
/// destructor runs after the pool has processed the thread's exit.
struct ExitTimeUser {
    Pool* pool = nullptr;
    std::vector<Item> blocks;   // freed by the destructor
    bool allocate_at_exit = false;

    ExitTimeUser() = default;
    ExitTimeUser(const ExitTimeUser&) = delete;
    ExitTimeUser& operator=(const ExitTimeUser&) = delete;
    ~ExitTimeUser();
};

std::atomic<bool> g_exit_time_intact{true};
std::atomic<unsigned> g_exit_time_destructors{0};
thread_local ExitTimeUser t_exit_time_user;

ExitTimeUser::~ExitTimeUser() {
    if (pool == nullptr) {
        return;
    }
    for (const Item& item : blocks) {
        if (!check_item(item)) {
            g_exit_time_intact = false;
        }
        pool->deallocate(item.ptr);
    }
    if (allocate_at_exit) {
        // A temporary allocation, and one that stays live after the thread.
        void* temporary = pool->allocate(5000);
        nanotest::fill_pattern(temporary, 5000, 5);
        if (!nanotest::check_pattern(temporary, 5000, 5)) {
            g_exit_time_intact = false;
        }
        pool->deallocate(temporary);
        void* kept = pool->allocate(64);
        nanotest::fill_pattern(kept, 64, 6);
    }
    g_exit_time_destructors.fetch_add(1);
}

}  // namespace

NANO_TEST(remote_frees_wait_in_the_inbox_until_drained) {
    Pool pool({16 * kMiB});
    Rng rng(1);
    std::vector<Item> items;
    for (int i = 0; i < 2000; ++i) {
        items.push_back(make_item(pool, rng));
    }
    const PoolStats before = pool.stats();
    std::atomic<bool> intact{true};
    std::thread remote([&] {
        for (const Item& item : items) {
            if (!check_item(item)) {
                intact = false;
            }
            pool.deallocate(item.ptr);
        }
    });
    remote.join();
    CHECK(intact.load());

    PoolStats stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, items.size());
    CHECK_EQ(stats.remote_deallocation_count, items.size());
    CHECK_EQ(stats.deallocation_count, items.size());
    // Pending blocks still occupy memory until their owner drains them.
    CHECK_EQ(stats.live_allocations, items.size());
    CHECK_EQ(stats.live_usable_bytes, before.live_usable_bytes);

    pool.drain_pending_frees();
    stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.live_usable_bytes, 0U);
    CHECK_EQ(stats.free_bytes, stats.capacity_bytes);
    CHECK(pool.verify_integrity());
}

NANO_TEST(owner_allocation_drains_the_inbox_and_reuses_blocks) {
    Pool pool({8 * kMiB});
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        blocks.push_back(pool.allocate(48));
    }
    const std::size_t spans = pool.stats().span_count;
    std::thread remote([&] {
        for (void* ptr : blocks) {
            pool.deallocate(ptr);
        }
    });
    remote.join();
    // The next allocation by the owner processes the inbox first.
    void* again = pool.allocate(48);
    CHECK_EQ(pool.stats().pending_remote_frees, 0U);
    CHECK_EQ(pool.stats().live_allocations, 1U);
    CHECK_EQ(pool.stats().span_count, spans);
    pool.deallocate(again);
    CHECK(pool.verify_integrity());
}

NANO_TEST(owner_deallocation_drains_the_inbox) {
    Pool pool({8 * kMiB});
    void* local = pool.allocate(64);
    void* shared = pool.allocate(64);
    std::thread remote([&] { pool.deallocate(shared); });
    remote.join();
    CHECK_EQ(pool.stats().pending_remote_frees, 1U);
    pool.deallocate(local);
    CHECK_EQ(pool.stats().pending_remote_frees, 0U);
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(many_threads_free_into_one_owner) {
    Pool pool({64 * kMiB});
    const unsigned threads = worker_count();
    Rng rng(7);
    std::vector<std::vector<Item>> per_thread(threads);
    for (unsigned t = 0; t < threads; ++t) {
        for (int i = 0; i < 3000; ++i) {
            per_thread[t].push_back(make_item(pool, rng));
        }
    }
    std::atomic<bool> intact{true};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (const Item& item : per_thread[t]) {
                if (!check_item(item)) {
                    intact = false;
                }
                pool.deallocate(item.ptr);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(intact.load());
    CHECK_EQ(pool.stats().pending_remote_frees, std::size_t{threads} * 3000);
    pool.drain_pending_frees();
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.free_bytes, stats.capacity_bytes);
    CHECK(pool.verify_integrity());
}

NANO_TEST(remote_frees_arrive_while_the_owner_allocates_and_drains) {
    Pool pool({64 * kMiB});
    const unsigned consumers = worker_count() - 1;
    std::mutex mutex;
    std::deque<Item> handoff;
    std::atomic<bool> producing{true};
    std::atomic<bool> intact{true};
    std::atomic<std::uint64_t> remote_freed{0};

    std::vector<std::thread> workers;
    for (unsigned c = 0; c < consumers; ++c) {
        workers.emplace_back([&] {
            for (;;) {
                Item item{};
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (handoff.empty()) {
                        if (!producing.load()) {
                            return;
                        }
                    } else {
                        item = handoff.front();
                        handoff.pop_front();
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
                remote_freed.fetch_add(1);
            }
        });
    }

    // The owner keeps allocating (which drains), freeing some blocks itself
    // and handing the rest to the consumers.
    Rng rng(99);
    std::vector<Item> local;
    std::uint64_t handed = 0;
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 100; ++i) {
            Item item = make_item(pool, rng, 600);
            if (rng.chance(70)) {
                std::lock_guard<std::mutex> lock(mutex);
                handoff.push_back(item);
                ++handed;
            } else {
                local.push_back(item);
            }
        }
        while (local.size() > 50) {
            if (!check_item(local.back())) {
                intact = false;
            }
            pool.deallocate(local.back().ptr);
            local.pop_back();
        }
        if (round % 10 == 0) {
            pool.drain_pending_frees();
        }
    }
    producing = false;
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const Item& item : local) {
        pool.deallocate(item.ptr);
    }
    pool.drain_pending_frees();
    CHECK(intact.load());
    CHECK_EQ(remote_freed.load(), handed);
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.remote_deallocation_count, handed);
    CHECK(pool.verify_integrity());
}

NANO_TEST(all_threads_exchange_and_free_each_others_blocks) {
    Pool pool({128 * kMiB});
    const unsigned threads = worker_count();
    std::mutex mutex;
    std::vector<Item> exchange;
    std::atomic<bool> intact{true};
    std::atomic<unsigned> finished_phase{0};

    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            Rng rng(1000 + t);
            std::vector<Item> mine;
            for (int step = 0; step < 20000; ++step) {
                const unsigned action = static_cast<unsigned>(rng.below(100));
                if (action < 45 || mine.empty()) {
                    mine.push_back(make_item(pool, rng, rng.chance(2) ? 100000 : 512));
                } else if (action < 70) {
                    // Hand a block to whichever thread picks it up.
                    std::lock_guard<std::mutex> lock(mutex);
                    exchange.push_back(mine.back());
                    mine.pop_back();
                } else if (action < 85) {
                    Item item{};
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!exchange.empty()) {
                            const std::size_t index = rng.below(exchange.size());
                            item = exchange[index];
                            exchange[index] = exchange.back();
                            exchange.pop_back();
                        }
                    }
                    if (item.ptr != nullptr) {
                        if (!check_item(item)) {
                            intact = false;
                        }
                        pool.deallocate(item.ptr);
                    }
                } else {
                    const std::size_t index = rng.below(mine.size());
                    if (!check_item(mine[index])) {
                        intact = false;
                    }
                    pool.deallocate(mine[index].ptr);
                    mine[index] = mine.back();
                    mine.pop_back();
                }
            }
            for (const Item& item : mine) {
                if (!check_item(item)) {
                    intact = false;
                }
                pool.deallocate(item.ptr);
            }
            // Empty the exchange while every owner is still running: no thread
            // proceeds to exit until all blocks have been freed.
            finished_phase.fetch_add(1);
            wait_for(finished_phase, threads);
            for (;;) {
                Item item{};
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (exchange.empty()) {
                        break;
                    }
                    item = exchange.back();
                    exchange.pop_back();
                }
                if (!check_item(item)) {
                    intact = false;
                }
                pool.deallocate(item.ptr);
            }
            finished_phase.fetch_add(1);
            wait_for(finished_phase, 2 * threads);
            pool.drain_pending_frees();
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(intact.load());
    // Every block was freed while its owner ran, and each owner released its
    // inbox and its spans when it exited.
    const PoolStats stats = pool.stats();
    CHECK(exchange.empty());
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK(stats.remote_deallocation_count > 0);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(thread_exit_returns_empty_spans_for_reuse) {
    Pool pool({16 * kMiB});
    std::thread first([&] {
        std::vector<void*> blocks;
        for (int i = 0; i < 40; ++i) {
            blocks.push_back(pool.allocate(100000));  // several spans
        }
        for (void* ptr : blocks) {
            pool.deallocate(ptr);
        }
    });
    first.join();
    PoolStats stats = pool.stats();
    const std::size_t spans = stats.span_count;
    CHECK(spans >= 4);
    CHECK_EQ(stats.thread_heap_count, 1U);
    // All spans went back to the pool; no thread owns free memory any more.
    CHECK_EQ(stats.free_span_count, spans);
    CHECK_EQ(stats.free_span_bytes, stats.span_bytes);
    CHECK_EQ(stats.thread_free_bytes, 0U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    std::thread second([&] {
        std::vector<void*> blocks;
        for (int i = 0; i < 40; ++i) {
            blocks.push_back(pool.allocate(100000));
        }
        for (void* ptr : blocks) {
            pool.deallocate(ptr);
        }
    });
    second.join();
    stats = pool.stats();
    CHECK_EQ(stats.span_count, spans);        // spans were reused, not carved
    CHECK_EQ(stats.thread_heap_count, 1U);    // the heap object was recycled
    CHECK_EQ(stats.free_span_count, spans);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, 80U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(blocks_are_shared_while_their_owner_runs) {
    Pool pool({16 * kMiB});
    std::vector<void*> handed;
    std::atomic<unsigned> phase{0};
    std::thread owner([&] {
        for (int i = 0; i < 100; ++i) {
            void* ptr = pool.allocate(200);
            nanotest::fill_pattern(ptr, 200, 42);
            handed.push_back(ptr);
        }
        phase = 1;          // blocks are ready
        wait_for(phase, 2); // the other thread is done with them
    });
    wait_for(phase, 1);
    // The owner is running: other threads may read and free its blocks.
    bool intact = true;
    for (std::size_t i = 0; i < handed.size(); ++i) {
        intact = intact && nanotest::check_pattern(handed[i], 200, 42);
        if (i % 2 == 0) {
            pool.deallocate(handed[i]);
        }
    }
    phase = 2;
    owner.join();
    CHECK(intact);
    // The owner drained the 50 frees when it exited. The other 50 blocks
    // stay allocated, untouched, until the pool is destroyed, and so does the
    // span that holds them.
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 50U);
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.remote_deallocation_count, 50U);
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    CHECK_EQ(stats.span_count, 1U);
    CHECK_EQ(stats.free_span_count, 0U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    // A new thread cannot reuse the retained heap; it gets its own.
    std::thread next([&] { pool.deallocate(pool.allocate(64)); });
    next.join();
    CHECK_EQ(pool.stats().thread_heap_count, 2U);
    CHECK_EQ(pool.stats().live_allocations, 50U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(statistics_and_queries_are_safe_during_concurrent_use) {
    Pool pool({64 * kMiB});
    std::atomic<bool> running{true};
    std::atomic<bool> sizes_ok{true};
    std::thread reader([&] {
        std::uint64_t observed = 0;
        while (running.load()) {
            const PoolStats stats = pool.stats();
            observed += stats.allocation_count;
            (void)pool.owns(&observed);
        }
        (void)observed;
    });
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 2; ++t) {
        workers.emplace_back([&, t] {
            Rng rng(t + 50);
            std::vector<void*> live;
            for (int i = 0; i < 20000; ++i) {
                if (live.size() < 200 && rng.chance(60)) {
                    const std::size_t size = rng.log_uniform(1, 2048);
                    void* ptr = pool.allocate(size);
                    if (pool.usable_size(ptr) < size) {
                        sizes_ok = false;
                    }
                    live.push_back(ptr);
                } else if (!live.empty()) {
                    pool.deallocate(live.back());
                    live.pop_back();
                }
            }
            for (void* ptr : live) {
                pool.deallocate(ptr);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    running = false;
    reader.join();
    CHECK(sizes_ok.load());
    CHECK_EQ(pool.stats().live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(pools_are_independent_across_threads) {
    Pool first({16 * kMiB});
    Pool second({16 * kMiB});
    std::vector<void*> from_first;
    std::vector<void*> from_second;
    std::thread worker([&] {
        for (int i = 0; i < 500; ++i) {
            from_first.push_back(first.allocate(32));
            from_second.push_back(second.allocate(32));
        }
    });
    worker.join();
    // NOTE: freeing these here would violate the lifetime contract (the
    // worker has exited), so the main thread allocates its own instead.
    std::vector<void*> local;
    for (int i = 0; i < 500; ++i) {
        local.push_back(first.allocate(32));
    }
    CHECK_EQ(first.stats().thread_heap_count, 2U);
    CHECK_EQ(second.stats().thread_heap_count, 1U);
    for (void* ptr : local) {
        CHECK(first.owns(ptr));
        CHECK(!second.owns(ptr));
        first.deallocate(ptr);
    }
    CHECK_EQ(first.stats().live_allocations, 500U);
    CHECK_EQ(second.stats().live_allocations, 500U);
    CHECK(first.verify_integrity());
    CHECK(second.verify_integrity());
}


// ---------------------------------------------------------------------------
// Reuse of exited threads' heaps: statistics cover the whole pool lifetime
// ---------------------------------------------------------------------------

NANO_TEST(recycled_heap_keeps_allocation_totals_and_peaks) {
    Pool pool({16 * kMiB});
    std::thread first([&] { pool.deallocate(pool.allocate(1000)); });
    first.join();
    const PoolStats after_first = pool.stats();
    CHECK_EQ(after_first.allocation_count, 1U);
    CHECK_EQ(after_first.deallocation_count, 1U);
    CHECK_EQ(after_first.thread_heap_count, 1U);
    CHECK_EQ(after_first.active_thread_heap_count, 0U);

    // The second thread adopts the first thread's heap.
    std::thread second([&] { pool.deallocate(pool.allocate(100)); });
    second.join();
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK_EQ(stats.allocation_count, 2U);
    CHECK_EQ(stats.deallocation_count, 2U);
    CHECK_EQ(stats.local_deallocation_count, 2U);
    CHECK_EQ(stats.remote_deallocation_count, 0U);
    // One thread at a time: the peaks are exact and remember the first thread.
    CHECK_EQ(stats.peak_requested_bytes, 1000U);
    CHECK_EQ(stats.peak_usable_bytes, 1008U);
    CHECK_EQ(stats.peak_consumed_bytes, 1024U);
    CHECK(nanotest::history_is_monotonic(after_first, stats));
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(recycled_heap_keeps_pending_remote_frees) {
    Pool pool({16 * kMiB});
    void* x = pool.allocate(256);  // owned by this thread
    std::thread first([&] {
        pool.deallocate(pool.allocate(128));  // local
        pool.deallocate(x);                   // remote, waits in our inbox
    });
    first.join();
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, 1U);
    CHECK_EQ(stats.remote_deallocation_count, 1U);
    CHECK_EQ(stats.local_deallocation_count, 1U);
    CHECK_EQ(stats.allocation_count, 2U);
    CHECK_EQ(stats.deallocation_count, 2U);
    CHECK_EQ(stats.live_allocations, 1U);
    CHECK_EQ(stats.thread_heap_count, 2U);
    CHECK_EQ(stats.active_thread_heap_count, 1U);
    CHECK(nanotest::stats_are_consistent(stats));

    // The second thread adopts the first thread's heap. The free the first
    // thread sent is still pending and must still be reported.
    std::thread second([&] { pool.deallocate(pool.allocate(128)); });
    second.join();
    const PoolStats recycled = pool.stats();
    CHECK_EQ(recycled.thread_heap_count, 2U);
    CHECK_EQ(recycled.pending_remote_frees, 1U);
    CHECK_EQ(recycled.remote_deallocation_count, 1U);
    CHECK_EQ(recycled.local_deallocation_count, 2U);
    CHECK_EQ(recycled.allocation_count, 3U);
    CHECK_EQ(recycled.deallocation_count, 3U);
    CHECK_EQ(recycled.live_allocations, 1U);
    CHECK(nanotest::history_is_monotonic(stats, recycled));
    CHECK(nanotest::stats_are_consistent(recycled));

    pool.drain_pending_frees();
    stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.remote_deallocation_count, 1U);
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(sequential_thread_churn_reuses_one_heap_and_keeps_totals) {
    Pool pool({16 * kMiB});
    constexpr int kThreads = 200;
    std::size_t expected_peak = 0;
    std::uint64_t expected_allocations = 0;
    PoolStats previous = pool.stats();
    bool monotonic = true;
    for (int t = 0; t < kThreads; ++t) {
        // Thread t holds blocks of 16, 32, ... (t % 13 + 1) * 16 bytes at once.
        const int blocks = t % 13 + 1;
        std::size_t live = 0;
        for (int i = 1; i <= blocks; ++i) {
            live += static_cast<std::size_t>(i) * 16;
        }
        expected_peak = live > expected_peak ? live : expected_peak;
        expected_allocations += static_cast<std::uint64_t>(blocks);
        std::thread worker([&pool, blocks] {
            std::vector<void*> held;
            for (int i = 1; i <= blocks; ++i) {
                held.push_back(pool.allocate(static_cast<std::size_t>(i) * 16));
            }
            for (void* ptr : held) {
                pool.deallocate(ptr);
            }
        });
        worker.join();
        const PoolStats stats = pool.stats();
        monotonic = monotonic && nanotest::history_is_monotonic(previous, stats);
        previous = stats;
    }
    CHECK(monotonic);
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK_EQ(stats.span_count, 1U);
    CHECK_EQ(stats.free_span_count, 1U);
    CHECK_EQ(stats.allocation_count, expected_allocations);
    CHECK_EQ(stats.deallocation_count, expected_allocations);
    CHECK_EQ(stats.peak_usable_bytes, expected_peak);
    CHECK_EQ(stats.peak_requested_bytes, expected_peak);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

NANO_TEST(thread_churn_with_concurrent_remote_frees_keeps_exact_totals) {
    Pool pool({64 * kMiB});
    const unsigned per_round = worker_count();
    constexpr unsigned kRounds = 25;
    constexpr unsigned kOwnedPerWorker = 150;
    constexpr unsigned kLocalPerWorker = 40;

    // Blocks owned by this thread, freed by short-lived workers.
    Rng rng(4242);
    std::vector<Item> owned;
    for (unsigned i = 0; i < kRounds * per_round * kOwnedPerWorker; ++i) {
        owned.push_back(make_item(pool, rng, 256));
    }
    const std::uint64_t owner_allocations = owned.size();

    std::atomic<bool> intact{true};
    bool monotonic = true;
    bool exact = true;
    PoolStats previous = pool.stats();
    for (unsigned round = 0; round < kRounds; ++round) {
        std::atomic<unsigned> done{0};
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < per_round; ++w) {
            workers.emplace_back([&, round, w] {
                Rng local_rng(round * 100 + w);
                std::vector<Item> mine;
                for (unsigned i = 0; i < kLocalPerWorker; ++i) {
                    mine.push_back(make_item(pool, local_rng, 2048));
                }
                const std::size_t first = (round * per_round + w) * kOwnedPerWorker;
                for (std::size_t i = first; i < first + kOwnedPerWorker; ++i) {
                    if (!check_item(owned[i])) {
                        intact = false;
                    }
                    pool.deallocate(owned[i].ptr);  // remote
                }
                for (const Item& item : mine) {
                    if (!check_item(item)) {
                        intact = false;
                    }
                    pool.deallocate(item.ptr);  // local
                }
                done.fetch_add(1);
            });
        }
        // Drain concurrently with the remote frees.
        while (done.load() < per_round) {
            pool.drain_pending_frees();
            std::this_thread::yield();
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        const PoolStats stats = pool.stats();
        monotonic = monotonic && nanotest::history_is_monotonic(previous, stats);
        const std::uint64_t sent = std::uint64_t{round + 1} * per_round * kOwnedPerWorker;
        const std::uint64_t worker_allocations =
            std::uint64_t{round + 1} * per_round * kLocalPerWorker;
        exact = exact && stats.remote_deallocation_count == sent &&
                stats.allocation_count == owner_allocations + worker_allocations &&
                stats.local_deallocation_count == worker_allocations &&
                stats.live_allocations == owner_allocations - sent + stats.pending_remote_frees &&
                stats.thread_heap_count <= per_round + 1 &&
                nanotest::stats_are_consistent(stats);
        previous = stats;
    }
    CHECK(intact.load());
    CHECK(monotonic);
    CHECK(exact);
    pool.drain_pending_frees();
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, stats.deallocation_count);
    CHECK_EQ(stats.remote_deallocation_count, owner_allocations);
    CHECK(stats.thread_heap_count <= per_round + 1);
    CHECK_EQ(stats.active_thread_heap_count, 1U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());
}

// ---------------------------------------------------------------------------
// thread_local destructors that run after the pool processed the thread exit
// ---------------------------------------------------------------------------

NANO_TEST(thread_local_destructors_may_free_the_threads_own_blocks) {
    Pool pool({16 * kMiB});
    g_exit_time_destructors = 0;
    g_exit_time_intact = true;
    std::thread worker([&] {
        t_exit_time_user.pool = &pool;  // constructed before the first allocation
        Rng rng(77);
        for (int i = 0; i < 200; ++i) {
            t_exit_time_user.blocks.push_back(make_item(pool, rng, 100000));
        }
    });
    worker.join();
    CHECK_EQ(g_exit_time_destructors.load(), 1U);
    CHECK(g_exit_time_intact.load());
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK_EQ(stats.allocation_count, 200U);
    CHECK_EQ(stats.local_deallocation_count, 200U);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    // Spans emptied by the destructor went back to the pool.
    CHECK(stats.span_count >= 2);
    CHECK_EQ(stats.free_span_count, stats.span_count);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    // The heap holds nothing, so the next thread adopts it.
    std::thread next([&] { pool.deallocate(pool.allocate(64)); });
    next.join();
    stats = pool.stats();
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK_EQ(stats.allocation_count, 201U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(thread_local_destructors_may_allocate_after_exit_processing) {
    Pool pool({16 * kMiB});
    g_exit_time_destructors = 0;
    g_exit_time_intact = true;
    std::thread worker([&] {
        t_exit_time_user.pool = &pool;
        t_exit_time_user.allocate_at_exit = true;
        Rng rng(78);
        t_exit_time_user.blocks.push_back(make_item(pool, rng, 512));
    });
    worker.join();
    CHECK_EQ(g_exit_time_destructors.load(), 1U);
    CHECK(g_exit_time_intact.load());
    // The block allocated last by the destructor stays live, with its span,
    // owned by the exited thread's heap.
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.allocation_count, 3U);
    CHECK_EQ(stats.deallocation_count, 2U);
    CHECK_EQ(stats.live_allocations, 1U);
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK_EQ(stats.active_thread_heap_count, 0U);
    CHECK_EQ(stats.span_count - stats.free_span_count, 1U);
    CHECK(nanotest::stats_are_consistent(stats));
    CHECK(pool.verify_integrity());

    std::thread next([&] { pool.deallocate(pool.allocate(64)); });
    next.join();
    CHECK_EQ(pool.stats().thread_heap_count, 2U);
    CHECK(pool.verify_integrity());
}

NANOTEST_MAIN()
