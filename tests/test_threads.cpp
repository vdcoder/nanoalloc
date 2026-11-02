// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Multithreaded contract tests: cross-thread deallocation through the owner's
// inbox, explicit and implicit draining, concurrent producers while the owner
// drains, thread exit under the documented lifetime contract, and concurrent
// statistics. Intended to be run under ThreadSanitizer as well.
//
// All test-harness synchronization (mutexes, joins) happens outside the pool;
// every block freed by another thread is handed over through it, which is
// what the thread lifetime contract requires.
#include <nanoalloc/pool.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"

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
            // Every thread must still be alive while others free its blocks:
            // wait until all threads have finished freeing, then drain.
            finished_phase.fetch_add(1);
            while (finished_phase.load() < threads) {
                std::this_thread::yield();
            }
            pool.drain_pending_frees();
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(intact.load());
    // Blocks still in the exchange belong to exited threads; freeing them now
    // would violate the lifetime contract, so they stay allocated until the
    // pool is destroyed. Everything else must be accounted for.
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.pending_remote_frees, 0U);
    CHECK_EQ(stats.live_allocations, exchange.size());
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
    CHECK_EQ(stats.live_allocations, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(thread_exit_with_live_blocks_keeps_them_valid) {
    Pool pool({16 * kMiB});
    void* survivor = nullptr;
    std::vector<void*> handed;
    std::mutex mutex;
    std::thread worker([&] {
        Rng rng(3);
        for (int i = 0; i < 100; ++i) {
            void* ptr = pool.allocate(200);
            nanotest::fill_pattern(ptr, 200, 42);
            std::lock_guard<std::mutex> lock(mutex);
            handed.push_back(ptr);
        }
        survivor = pool.allocate(64);
    });
    worker.join();
    // The worker exited with all blocks live. Blocks of an exited thread may
    // be read until the pool is destroyed, but not freed (see the contract).
    for (void* ptr : handed) {
        CHECK(nanotest::check_pattern(ptr, 200, 42));
    }
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_allocations, 101U);
    CHECK_EQ(stats.thread_heap_count, 1U);
    CHECK(pool.verify_integrity());
    CHECK(survivor != nullptr);
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

NANOTEST_MAIN()
