// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Multithreaded benchmark: nanoalloc::Pool versus the system allocator with a
// configurable share of cross-thread deallocations.
//
// Scenarios
// ---------
// exchange     Every round, each thread allocates N objects (allocation
//              phase), then all threads deallocate (deallocation phase). A
//              precomputed share of the objects (0-100%) is deallocated by a
//              different thread than the one that allocated it. Blocks are
//              reused round after round; nanoalloc owners drain their inbox in
//              the next allocation phase, so draining is part of the measured
//              allocation work.
// many-to-one  Thread 0 allocates N objects for each other thread; all other
//              threads deallocate them concurrently into thread 0's inbox;
//              thread 0 then drains its inbox explicitly
//              (drain_pending_frees()), which is timed separately.
//
// Method
// ------
// * Sizes and the deallocating thread of every object are precomputed from a
//   fixed seed before any timing. All allocators run the same trace.
// * Phases are separated by a spinning barrier. Each thread times only its own
//   work in a phase; a phase's throughput is its total operations divided by
//   the longest per-thread time, so barrier wake-up skew is not counted.
// * Each allocation writes an 8-byte stamp; each deallocation reads it.
// * 2 warmup rounds, then the median of the measured rounds is reported.
// * Thread lifetime contract: worker threads live for the whole run of a
//   configuration and only exit after every cross-thread deallocation has
//   completed (final barrier).
#include <nanoalloc/pool.hpp>
#include <nanoalloc/version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "platform.hpp"
#include "workloads.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using nanobench::Rng;

struct Options {
    std::vector<unsigned> threads;
    std::vector<unsigned> remote_percent = {0, 10, 25, 50, 100};
    std::size_t objects = 20000;  // per thread per round
    unsigned rounds = 9;
    unsigned warmup = 2;
    std::uint64_t seed = 0x6D74626EULL;
    bool many_to_one = true;
    std::vector<std::string> allocators = {"system", "nanoalloc"};
};

std::vector<unsigned> parse_unsigned_list(const std::string& text) {
    std::vector<unsigned> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos ||
            item.size() > 6) {
            throw std::invalid_argument("expected a comma-separated list of integers: " + text);
        }
        values.push_back(static_cast<unsigned>(std::stoul(item)));
    }
    return values;
}

void print_usage() {
    std::printf(
        "usage: nanoalloc_mtbench [options]\n"
        "  --threads LIST     thread counts (default 1,2,4,8,16 up to the CPU count)\n"
        "  --remote LIST      cross-thread deallocation percentages (default 0,10,25,50,100)\n"
        "  --objects N        objects per thread per round (default 20000)\n"
        "  --rounds N         measured rounds (default 9)\n"
        "  --quick            --objects 5000 --rounds 3\n"
        "  --allocators LIST  subset of system,nanoalloc\n"
        "  --no-many-to-one   skip the many-to-one scenario\n");
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool threads_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--threads") {
            options.threads = parse_unsigned_list(value());
            threads_given = true;
        } else if (arg == "--remote") {
            options.remote_percent = parse_unsigned_list(value());
        } else if (arg == "--objects") {
            options.objects = parse_unsigned_list(value()).at(0);
        } else if (arg == "--rounds") {
            options.rounds = parse_unsigned_list(value()).at(0);
        } else if (arg == "--quick") {
            options.objects = 5000;
            options.rounds = 3;
        } else if (arg == "--allocators") {
            std::stringstream stream(value());
            std::string item;
            options.allocators.clear();
            while (std::getline(stream, item, ',')) {
                if (item != "system" && item != "nanoalloc") {
                    throw std::invalid_argument("unknown allocator " + item);
                }
                options.allocators.push_back(item);
            }
        } else if (arg == "--no-many-to-one") {
            options.many_to_one = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (!threads_given) {
        const unsigned cpus = std::max(1U, std::thread::hardware_concurrency());
        for (unsigned count : {1U, 2U, 4U, 8U, 16U}) {
            if (count <= cpus) {
                options.threads.push_back(count);
            }
        }
    }
    for (unsigned count : options.threads) {
        if (count == 0 || count > 256) {
            throw std::invalid_argument("thread counts must be in 1..256");
        }
    }
    for (unsigned percent : options.remote_percent) {
        if (percent > 100) {
            throw std::invalid_argument("remote percentages must be in 0..100");
        }
    }
    if (options.objects == 0 || options.objects > 10'000'000 || options.rounds == 0) {
        throw std::invalid_argument("objects and rounds must be positive");
    }
    return options;
}

/// Spinning barrier: threads are released within nanoseconds of each other.
class SpinBarrier {
public:
    explicit SpinBarrier(unsigned participants) : participants_(participants) {}
    void arrive_and_wait() {
        const unsigned generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == participants_) {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

private:
    const unsigned participants_;
    std::atomic<unsigned> arrived_{0};
    std::atomic<unsigned> generation_{0};
};

/// Allocator interface used by the worker threads (one virtual call per op is
/// the same for both allocators).
class Backend {
public:
    virtual ~Backend() = default;
    virtual void* allocate(std::size_t bytes) = 0;
    virtual void deallocate(void* ptr) = 0;
    virtual void drain() {}
    [[nodiscard]] virtual std::size_t reserved_bytes() const { return 0; }
};

class SystemBackend final : public Backend {
public:
    void* allocate(std::size_t bytes) override {
        void* ptr = std::malloc(bytes);
        if (ptr == nullptr) {
            std::fprintf(stderr, "error: malloc failed\n");
            std::abort();
        }
        return ptr;
    }
    void deallocate(void* ptr) override { std::free(ptr); }
};

class PoolBackend final : public Backend {
public:
    explicit PoolBackend(std::size_t capacity) : pool_(nanoalloc::PoolConfig{capacity}) {}
    void* allocate(std::size_t bytes) override {
        void* ptr = pool_.try_allocate(bytes);
        if (ptr == nullptr) {
            std::fprintf(stderr, "error: nanoalloc pool exhausted\n");
            std::abort();
        }
        return ptr;
    }
    void deallocate(void* ptr) override { pool_.deallocate(ptr); }
    void drain() override { pool_.drain_pending_frees(); }
    [[nodiscard]] std::size_t reserved_bytes() const override {
        return pool_.stats().reserved_bytes;
    }

private:
    nanoalloc::Pool pool_;
};

std::unique_ptr<Backend> make_backend(const std::string& name, unsigned threads,
                                      std::size_t objects) {
    if (name == "system") {
        return std::make_unique<SystemBackend>();
    }
    // Generous budget (reserved lazily): live data is at most
    // threads * objects * 512 B, plus spans retained per thread.
    const std::size_t capacity =
        std::max<std::size_t>(std::size_t{256} << 20,
                              std::size_t{threads} * (objects * 1024 + (std::size_t{8} << 20)));
    return std::make_unique<PoolBackend>(capacity);
}

inline void stamp(void* ptr, std::uint32_t value) { std::memcpy(ptr, &value, sizeof(value)); }

inline std::uint32_t read_stamp(const void* ptr) {
    std::uint32_t value = 0;
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

std::atomic<std::uint64_t> g_checksum{0};

// ---- exchange scenario ------------------------------------------------------

struct ExchangeResult {
    double alloc_mops = 0;
    double free_mops = 0;
    double combined_mops = 0;
    std::size_t reserved = 0;
};

ExchangeResult run_exchange(Backend& backend, unsigned threads, unsigned remote_percent,
                            const Options& options) {
    const std::size_t objects = options.objects;
    // Precomputed trace: sizes per producer and the list of (producer, index)
    // each thread deallocates.
    std::vector<std::vector<std::uint32_t>> sizes(threads, std::vector<std::uint32_t>(objects));
    std::vector<std::vector<std::pair<unsigned, std::uint32_t>>> frees(threads);
    Rng rng(options.seed + threads * 1000 + remote_percent);
    for (unsigned t = 0; t < threads; ++t) {
        for (std::size_t i = 0; i < objects; ++i) {
            sizes[t][i] = rng.log_uniform(16, 512);
            unsigned freer = t;
            if (threads > 1 && rng.below(100) < remote_percent) {
                freer = static_cast<unsigned>((t + 1 + rng.below(threads - 1)) % threads);
            }
            frees[freer].emplace_back(t, static_cast<std::uint32_t>(i));
        }
    }
    for (auto& list : frees) {  // interleave producers within each freer's list
        for (std::size_t i = list.size(); i > 1; --i) {
            std::swap(list[i - 1], list[static_cast<std::size_t>(rng.below(i))]);
        }
    }
    std::vector<std::vector<void*>> slots(threads, std::vector<void*>(objects, nullptr));

    const unsigned total_rounds = options.warmup + options.rounds;
    std::vector<std::vector<double>> alloc_ns(total_rounds, std::vector<double>(threads));
    std::vector<std::vector<double>> free_ns(total_rounds, std::vector<double>(threads));
    SpinBarrier barrier(threads);

    auto worker = [&](unsigned t) {
        std::uint64_t sum = 0;
        for (unsigned round = 0; round < total_rounds; ++round) {
            barrier.arrive_and_wait();
            const Clock::time_point a0 = Clock::now();
            for (std::size_t i = 0; i < objects; ++i) {
                void* ptr = backend.allocate(sizes[t][i]);
                stamp(ptr, static_cast<std::uint32_t>(i));
                slots[t][i] = ptr;
            }
            const Clock::time_point a1 = Clock::now();
            barrier.arrive_and_wait();
            const Clock::time_point f0 = Clock::now();
            for (const auto& [producer, index] : frees[t]) {
                void* ptr = slots[producer][index];
                sum += read_stamp(ptr);
                backend.deallocate(ptr);
            }
            const Clock::time_point f1 = Clock::now();
            alloc_ns[round][t] = elapsed_ns(a0, a1);
            free_ns[round][t] = elapsed_ns(f0, f1);
        }
        // All cross-thread deallocations are complete once everyone is here.
        barrier.arrive_and_wait();
        backend.drain();
        g_checksum.fetch_add(sum, std::memory_order_relaxed);
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back(worker, t);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }

    const double ops_per_phase = static_cast<double>(objects) * threads;
    std::vector<double> alloc_rates;
    std::vector<double> free_rates;
    std::vector<double> combined_rates;
    for (unsigned round = options.warmup; round < total_rounds; ++round) {
        const double alloc_time = *std::max_element(alloc_ns[round].begin(), alloc_ns[round].end());
        const double free_time = *std::max_element(free_ns[round].begin(), free_ns[round].end());
        alloc_rates.push_back(ops_per_phase / alloc_time * 1e3);
        free_rates.push_back(ops_per_phase / free_time * 1e3);
        combined_rates.push_back(2 * ops_per_phase / (alloc_time + free_time) * 1e3);
    }
    ExchangeResult result;
    result.alloc_mops = median(alloc_rates);
    result.free_mops = median(free_rates);
    result.combined_mops = median(combined_rates);
    result.reserved = backend.reserved_bytes();
    return result;
}

// ---- many-to-one scenario ------------------------------------------------------

struct ManyToOneResult {
    double owner_alloc_mops = 0;
    double remote_free_mops = 0;
    double drain_mops = 0;
};

ManyToOneResult run_many_to_one(Backend& backend, unsigned threads, const Options& options) {
    const unsigned freers = threads - 1;
    const std::size_t per_freer = options.objects;
    const std::size_t total = per_freer * freers;
    std::vector<std::uint32_t> sizes(total);
    Rng rng(options.seed ^ 0x51ULL);
    for (std::uint32_t& size : sizes) {
        size = rng.log_uniform(16, 512);
    }
    std::vector<void*> slots(total, nullptr);
    const unsigned total_rounds = options.warmup + options.rounds;
    std::vector<double> owner_ns(total_rounds);
    std::vector<double> drain_ns(total_rounds);
    std::vector<std::vector<double>> free_ns(total_rounds, std::vector<double>(threads, 0.0));
    SpinBarrier barrier(threads);

    auto worker = [&](unsigned t) {
        std::uint64_t sum = 0;
        for (unsigned round = 0; round < total_rounds; ++round) {
            barrier.arrive_and_wait();
            if (t == 0) {
                const Clock::time_point start = Clock::now();
                for (std::size_t i = 0; i < total; ++i) {
                    void* ptr = backend.allocate(sizes[i]);
                    stamp(ptr, static_cast<std::uint32_t>(i));
                    slots[i] = ptr;
                }
                owner_ns[round] = elapsed_ns(start, Clock::now());
            }
            barrier.arrive_and_wait();
            if (t != 0) {
                const Clock::time_point start = Clock::now();
                const std::size_t begin = (t - 1) * per_freer;
                for (std::size_t i = begin; i < begin + per_freer; ++i) {
                    sum += read_stamp(slots[i]);
                    backend.deallocate(slots[i]);
                }
                free_ns[round][t] = elapsed_ns(start, Clock::now());
            }
            barrier.arrive_and_wait();
            if (t == 0) {
                const Clock::time_point start = Clock::now();
                backend.drain();
                drain_ns[round] = elapsed_ns(start, Clock::now());
            }
        }
        barrier.arrive_and_wait();
        g_checksum.fetch_add(sum, std::memory_order_relaxed);
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back(worker, t);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }

    std::vector<double> owner_rates;
    std::vector<double> free_rates;
    std::vector<double> drain_rates;
    for (unsigned round = options.warmup; round < total_rounds; ++round) {
        owner_rates.push_back(static_cast<double>(total) / owner_ns[round] * 1e3);
        const double free_time = *std::max_element(free_ns[round].begin(), free_ns[round].end());
        free_rates.push_back(static_cast<double>(total) / free_time * 1e3);
        drain_rates.push_back(static_cast<double>(total) / std::max(drain_ns[round], 1.0) * 1e3);
    }
    return {median(owner_rates), median(free_rates), median(drain_rates)};
}

int run(const Options& options) {
    const nanobench::PlatformInfo platform = nanobench::platform_info();
    std::printf("nanoalloc multithreaded benchmark %s\n", nanoalloc::version_string);
    std::printf("  compiler   %s\n", platform.compiler.c_str());
    std::printf("  build      %s, flags \"%s\"%s\n", platform.build_type.c_str(),
                platform.cxx_flags.c_str(), platform.optimized ? "" : " [NOT OPTIMIZED]");
    if (!platform.sanitizers.empty()) {
        std::printf("  WARNING    built with sanitizers (%s)\n", platform.sanitizers.c_str());
    }
    std::printf("  platform   %s\n", platform.os.c_str());
    std::printf("  cpu        %s (%u logical CPUs)\n", platform.cpu.c_str(), platform.logical_cpus);
    std::printf("  system     %s\n", platform.system_allocator.c_str());
    std::printf("  method     %zu objects of 16-511 B per thread per round, %u warmup + %u "
                "measured rounds, medians\n\n",
                options.objects, options.warmup, options.rounds);

    std::printf("Exchange: allocate N per thread, then deallocate; remote%% are freed by "
                "another thread\n");
    std::printf("%7s %7s %-10s %12s %12s %12s %12s\n", "threads", "remote", "allocator",
                "alloc Mops/s", "free Mops/s", "comb Mops/s", "reserved MiB");
    for (unsigned threads : options.threads) {
        for (unsigned percent : options.remote_percent) {
            if (threads == 1 && percent != 0) {
                continue;
            }
            for (const std::string& name : options.allocators) {
                std::unique_ptr<Backend> backend = make_backend(name, threads, options.objects);
                const ExchangeResult r = run_exchange(*backend, threads, percent, options);
                char reserved[32] = "n/a";
                if (name == "nanoalloc") {
                    std::snprintf(reserved, sizeof(reserved), "%.1f",
                                  static_cast<double>(r.reserved) / (1024.0 * 1024.0));
                }
                std::printf("%7u %6u%% %-10s %12.2f %12.2f %12.2f %12s\n", threads, percent,
                            name.c_str(), r.alloc_mops, r.free_mops, r.combined_mops, reserved);
                std::fflush(stdout);
            }
        }
    }

    if (options.many_to_one) {
        std::printf("\nMany-to-one: thread 0 allocates, all other threads free into its inbox, "
                    "thread 0 drains\n");
        std::printf("%7s %-10s %15s %15s %15s\n", "threads", "allocator", "owner alloc",
                    "remote free", "drain");
        std::printf("%7s %-10s %15s %15s %15s\n", "", "", "Mops/s", "Mops/s", "Mblocks/s");
        for (unsigned threads : options.threads) {
            if (threads < 2) {
                continue;
            }
            for (const std::string& name : options.allocators) {
                std::unique_ptr<Backend> backend = make_backend(name, threads, options.objects);
                const ManyToOneResult r = run_many_to_one(*backend, threads, options);
                char drain[32] = "n/a";
                if (name == "nanoalloc") {
                    std::snprintf(drain, sizeof(drain), "%.2f", r.drain_mops);
                }
                std::printf("%7u %-10s %15.2f %15.2f %15s\n", threads, name.c_str(),
                            r.owner_alloc_mops, r.remote_free_mops, drain);
                std::fflush(stdout);
            }
        }
    }
    std::printf("\nchecksum 0x%" PRIx64 "\n", g_checksum.load());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
