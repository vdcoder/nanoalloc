// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// nanoalloc benchmark: runtime and memory behavior of nanoalloc::Pool versus
// the system allocator on deterministic allocation traces.
//
// Method
// ------
// * Every workload is a precomputed trace (see workloads.hpp), generated from
//   a fixed seed before any timing starts. All allocators replay the same
//   trace through the same code path; only the allocate/deallocate calls
//   differ. The benchmark uses only the public Pool API.
// * Each allocation writes a stamp into its first 8 bytes and its last byte;
//   each deallocation reads them back into a checksum. The expected checksum
//   is computed from the trace, and every replay (warmup and measured) must
//   reproduce it exactly, otherwise the benchmark stops with an error. The
//   workload therefore touches real memory, cannot be optimized away, and a
//   replay that returned overlapping or corrupted blocks is detected.
// * Traces are grouped into segments of only allocations or only
//   deallocations. Each segment is timed with one steady_clock read at its
//   boundary, so allocation and deallocation throughput can be reported
//   separately without per-operation timer overhead.
// * Each (workload, allocator) pair runs `warmup` unmeasured iterations and
//   `iterations` measured ones; medians are reported with their spread (median
//   absolute deviation, minimum and maximum). The allocator order rotates
//   between iterations (iteration i starts with allocator i mod n), so every
//   allocator runs in every position equally often when the iteration count
//   is a multiple of the number of allocators.
// * All allocators replay the same trace with the same configuration: one
//   arena size per workload for every nanoalloc variant.
// * A separate, untimed replay measures memory: peak usable and consumed
//   bytes, payload slack and metadata.
// * The nanoalloc arena is sized from the trace (at least twice its peak live
//   requested bytes, doubled until the untimed replay succeeds), so a too
//   small arena can never make nanoalloc fail where the system allocator
//   succeeds. Any allocation failure during a timed run aborts the benchmark.
// * Only the public Pool API is used, and statistics fields that older
//   library versions lack are detected at compile time, so this harness can
//   be built unchanged against earlier nanoalloc releases for before/after
//   comparisons on identical traces.
#include <nanoalloc/pool.hpp>
#include <nanoalloc/version.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "platform.hpp"
#include "workloads.hpp"

namespace nanobench {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kMiB = 1024.0 * 1024.0;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    unsigned iterations = 9;
    unsigned warmup = 2;
    double scale = 1.0;
    std::uint64_t seed = 0x6E616E6FULL;
    std::vector<std::string> workloads;  // empty: all
    std::vector<std::string> allocators = {"system", "nanoalloc", "nanoalloc-tracked"};
    std::string csv_path;
    bool verbose = false;
};

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

void print_usage() {
    std::printf(
        "usage: nanoalloc_bench [options]\n"
        "  --iterations N     measured iterations per workload, 1-100000 (default 9)\n"
        "  --warmup N         unmeasured warmup iterations, 0-100000 (default 2)\n"
        "  --scale F          multiply operation counts by F, 0 < F <= 1000 (default 1.0)\n"
        "  --quick            shorthand for --scale 0.1 --iterations 3\n"
        "  --seed N           trace seed (default 0x6E616E6F)\n"
        "  --workload LIST    comma-separated workloads (default: all)\n"
        "  --allocators LIST  comma-separated subset of system,nanoalloc,nanoalloc-tracked\n"
        "  --csv PATH         also write all results as CSV\n"
        "  --verbose          print every measured iteration\n"
        "  --list             list workloads and exit\n");
}

// Bounds keep warmup + iterations far from overflow and runs finite.
constexpr unsigned long long kMaxIterations = 100000;
constexpr double kMaxScale = 1000.0;

/// Parses a decimal count and checks that it lies in [low, high]. Rejects
/// signs, spaces and trailing characters, which std::stoul would accept.
unsigned parse_count(const std::string& option, const std::string& text,
                     unsigned long long low, unsigned long long high) {
    const bool digits_only =
        !text.empty() && text.size() <= 12 &&
        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    const unsigned long long value = digits_only ? std::stoull(text) : 0;
    if (!digits_only || value < low || value > high) {
        throw std::invalid_argument(option + " expects an integer from " + std::to_string(low) +
                                    " to " + std::to_string(high) + ", got '" + text + "'");
    }
    return static_cast<unsigned>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--iterations") {
            options.iterations = parse_count(arg, value(), 1, kMaxIterations);
        } else if (arg == "--warmup") {
            options.warmup = parse_count(arg, value(), 0, kMaxIterations);
        } else if (arg == "--scale") {
            options.scale = std::stod(value());
        } else if (arg == "--quick") {
            options.scale = 0.1;
            options.iterations = 3;
        } else if (arg == "--seed") {
            options.seed = std::stoull(value(), nullptr, 0);
        } else if (arg == "--workload" || arg == "--workloads") {
            options.workloads = split_list(value());
        } else if (arg == "--allocators") {
            options.allocators = split_list(value());
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--csv") {
            options.csv_path = value();
        } else if (arg == "--list") {
            for (const WorkloadInfo& workload : workloads()) {
                std::printf("%-14s %s\n", workload.name, workload.description);
            }
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (!(options.scale > 0 && options.scale <= kMaxScale)) {  // also rejects NaN
        throw std::invalid_argument("--scale expects a value in (0, " +
                                    std::to_string(static_cast<int>(kMaxScale)) + "]");
    }
    for (const std::string& allocator : options.allocators) {
        if (allocator != "system" && allocator != "nanoalloc" && allocator != "nanoalloc-tracked") {
            throw std::invalid_argument("unknown allocator " + allocator);
        }
    }
    if (options.workloads.empty()) {
        for (const WorkloadInfo& workload : workloads()) {
            options.workloads.emplace_back(workload.name);
        }
    }
    return options;
}

// ---------------------------------------------------------------------------
// Statistics fields that not every library version has
// ---------------------------------------------------------------------------

template <class Stats>
std::uint64_t reserved_of(const Stats& stats) {
    if constexpr (requires { stats.reserved_bytes; }) {
        return stats.reserved_bytes;
    } else {
        return stats.capacity_bytes;  // the whole arena is the reservation
    }
}

// ---------------------------------------------------------------------------
// Memory access and optimization barriers
// ---------------------------------------------------------------------------

/// Forces `value` to be materialized, so the work producing it is kept.
template <class T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    static const void* volatile sink = nullptr;
    sink = &value;
#endif
}

/// Writes a stamp into the first (up to) 8 bytes and the last byte.
inline void touch(void* ptr, std::uint32_t size, std::uint32_t slot) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    const std::uint64_t stamp = (std::uint64_t{slot} << 32) | size;
    if (size >= sizeof(stamp)) {
        std::memcpy(bytes, &stamp, sizeof(stamp));
    }
    if (size > 0) {
        bytes[size - 1] = static_cast<unsigned char>(slot);
    }
}

/// Reads back what touch() wrote.
inline std::uint64_t consume(const void* ptr, std::uint32_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    std::uint64_t stamp = 0;
    if (size >= sizeof(stamp)) {
        std::memcpy(&stamp, bytes, sizeof(stamp));
    }
    return size > 0 ? stamp + bytes[size - 1] : stamp;
}

/// The value consume() returns for a block that touch() stamped.
std::uint64_t expected_consume(std::uint32_t size, std::uint32_t slot) {
    if (size > 8) {  // the stamp and the last byte do not overlap
        return ((std::uint64_t{slot} << 32) | size) + (slot & 0xFFU);
    }
    unsigned char buffer[8] = {};  // overlapping cases, in native byte order
    touch(buffer, size, slot);
    return consume(buffer, size);
}

/// Checksum that a correct replay of `trace` produces.
std::uint64_t expected_checksum(const Trace& trace) {
    std::uint64_t sum = 0;
    for (const Segment& segment : trace.segments) {
        if (segment.phase == Phase::Deallocate) {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                sum += expected_consume(trace.ops[i].size, trace.ops[i].slot);
            }
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Allocator backends
// ---------------------------------------------------------------------------

class SystemBackend {
public:
    void* allocate(std::size_t size) {
        void* ptr = std::malloc(size);
        if (ptr == nullptr) {
            throw std::bad_alloc();
        }
        return ptr;
    }
    void deallocate(void* ptr, std::size_t /*size*/) { std::free(ptr); }
};

class PoolBackend {
public:
    explicit PoolBackend(nanoalloc::Pool& pool) : pool_(&pool) {}
    void* allocate(std::size_t size) { return pool_->allocate(size); }
    void deallocate(void* ptr, std::size_t /*size*/) { pool_->deallocate(ptr); }

private:
    nanoalloc::Pool* pool_;
};

// ---------------------------------------------------------------------------
// Timed replay
// ---------------------------------------------------------------------------

struct Sample {
    double alloc_ns = 0;
    double free_ns = 0;
    double total_ns = 0;
};

template <class Backend>
Sample replay_timed(const Trace& trace, Backend& backend, std::vector<void*>& slots,
                    std::uint64_t& checksum) {
    // `checksum` receives this replay's checksum; the caller validates it.
    Sample sample;
    const Op* ops = trace.ops.data();
    void** slot_ptrs = slots.data();
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    Clock::time_point last = start;
    for (const Segment& segment : trace.segments) {
        if (segment.phase == Phase::Allocate) {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = ops[i];
                void* ptr = backend.allocate(op.size);
                touch(ptr, op.size, op.slot);
                slot_ptrs[op.slot] = ptr;
            }
        } else {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = ops[i];
                void* ptr = slot_ptrs[op.slot];
                sum += consume(ptr, op.size);
                backend.deallocate(ptr, op.size);
            }
        }
        do_not_optimize(sum);
        const Clock::time_point now = Clock::now();
        const double elapsed = std::chrono::duration<double, std::nano>(now - last).count();
        (segment.phase == Phase::Allocate ? sample.alloc_ns : sample.free_ns) += elapsed;
        last = now;
    }
    sample.total_ns = std::chrono::duration<double, std::nano>(last - start).count();
    checksum = sum;
    return sample;
}

class Runner {
public:
    virtual ~Runner() = default;
    virtual Sample run(const Trace& trace, std::vector<void*>& slots, std::uint64_t& checksum) = 0;
    [[nodiscard]] virtual const char* name() const = 0;
};

class SystemRunner final : public Runner {
public:
    Sample run(const Trace& trace, std::vector<void*>& slots, std::uint64_t& checksum) override {
        return replay_timed(trace, backend_, slots, checksum);
    }
    [[nodiscard]] const char* name() const override { return "system"; }

private:
    SystemBackend backend_;
};

class PoolRunner final : public Runner {
public:
    PoolRunner(const char* name, const nanoalloc::PoolConfig& config)
        : name_(name), pool_(config), backend_(pool_) {}

    Sample run(const Trace& trace, std::vector<void*>& slots, std::uint64_t& checksum) override {
        const Sample sample = replay_timed(trace, backend_, slots, checksum);
        if (pool_.stats().live_allocations != 0) {
            throw std::runtime_error("trace replay left live allocations in the pool");
        }
        return sample;
    }
    [[nodiscard]] const char* name() const override { return name_; }

private:
    const char* name_;
    nanoalloc::Pool pool_;
    PoolBackend backend_;
};

// ---------------------------------------------------------------------------
// Untimed memory replay
// ---------------------------------------------------------------------------

struct MemoryResult {
    bool usable_known = false;
    bool consumed_known = false;
    bool arena_known = false;
    std::uint64_t peak_usable = 0;
    std::uint64_t peak_consumed = 0;
    std::uint64_t total_usable = 0;  // sum of usable sizes over all allocations
    std::uint64_t max_slack = 0;     // largest usable - requested of any allocation
    std::uint64_t capacity = 0;
    std::uint64_t reserved = 0;  // pool memory reserved at the end of the replay
    std::uint64_t metadata = 0;
    std::uint64_t tracking = 0;
    std::uint64_t splits = 0;
    std::uint64_t merges = 0;
};

/// Replays `trace` on a fresh pool. Returns false if an allocation failed.
bool measure_pool(const Trace& trace, const nanoalloc::PoolConfig& config, MemoryResult& result) {
    nanoalloc::Pool pool(config);
    std::vector<void*> slots(trace.slot_count, nullptr);
    result = MemoryResult{};
    for (const Segment& segment : trace.segments) {
        for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
            const Op op = trace.ops[i];
            if (segment.phase == Phase::Allocate) {
                void* ptr = pool.try_allocate(op.size);
                if (ptr == nullptr) {
                    for (void* live : slots) {
                        pool.deallocate(live);
                    }
                    return false;
                }
                const std::size_t usable = pool.usable_size(ptr);
                result.total_usable += usable;
                result.max_slack = std::max<std::uint64_t>(result.max_slack, usable - op.size);
                slots[op.slot] = ptr;
            } else {
                pool.deallocate(slots[op.slot]);
                slots[op.slot] = nullptr;
            }
        }
    }
    const nanoalloc::PoolStats stats = pool.stats();
    result.usable_known = true;
    result.consumed_known = true;
    result.arena_known = true;
    result.peak_usable = stats.peak_usable_bytes;
    result.peak_consumed = stats.peak_consumed_bytes;
    result.capacity = stats.capacity_bytes;
    result.reserved = reserved_of(stats);
    result.metadata = stats.metadata_bytes;
    result.tracking = stats.tracking_bytes;
    result.splits = stats.split_count;
    result.merges = stats.merge_count;
    return true;
}

/// Replays `trace` with malloc/free. The C runtime exposes usable sizes on
/// common platforms, but not an exact figure for per-chunk overhead (glibc's
/// mallinfo2 counts chunks cached in tcache as in use), so consumed bytes are
/// not reported for the system allocator.
MemoryResult measure_system(const Trace& trace) {
    MemoryResult result;
    result.usable_known = system_usable_size_available();
    std::vector<void*> slots(trace.slot_count, nullptr);
    std::vector<std::size_t> usable_of(trace.slot_count, 0);
    std::uint64_t live_usable = 0;
    for (const Segment& segment : trace.segments) {
        for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
            const Op op = trace.ops[i];
            if (segment.phase == Phase::Allocate) {
                void* ptr = std::malloc(op.size);
                if (ptr == nullptr) {
                    throw std::bad_alloc();
                }
                slots[op.slot] = ptr;
                if (result.usable_known) {
                    const std::size_t usable = system_usable_size(ptr);
                    usable_of[op.slot] = usable;
                    live_usable += usable;
                    result.total_usable += usable;
                    result.max_slack = std::max<std::uint64_t>(result.max_slack, usable - op.size);
                    result.peak_usable = std::max(result.peak_usable, live_usable);
                }
            } else {
                live_usable -= usable_of[op.slot];
                std::free(slots[op.slot]);
                slots[op.slot] = nullptr;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

struct Result {
    std::string workload;
    std::string allocator;
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t peak_requested = 0;
    std::uint64_t total_requested = 0;
    std::vector<Sample> samples;
    MemoryResult memory;

    double median_alloc_ns = 0;
    double median_free_ns = 0;
    double median_total_ns = 0;
    double noise = 0;  // median absolute deviation / median of total time
    double min_combined_mops = 0;  // slowest and fastest measured iteration
    double max_combined_mops = 0;
};

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

double mops(std::uint64_t operations, double ns) {
    return ns > 0 ? static_cast<double>(operations) / ns * 1e3 : 0.0;
}

void summarize(Result& result) {
    std::vector<double> alloc;
    std::vector<double> free;
    std::vector<double> total;
    std::vector<double> combined;
    for (const Sample& sample : result.samples) {
        alloc.push_back(sample.alloc_ns);
        free.push_back(sample.free_ns);
        total.push_back(sample.total_ns);
        combined.push_back(
            mops(result.allocations + result.deallocations, sample.alloc_ns + sample.free_ns));
    }
    if (!combined.empty()) {
        result.min_combined_mops = *std::min_element(combined.begin(), combined.end());
        result.max_combined_mops = *std::max_element(combined.begin(), combined.end());
    }
    result.median_alloc_ns = median(alloc);
    result.median_free_ns = median(free);
    result.median_total_ns = median(total);
    std::vector<double> deviations;
    for (double value : total) {
        deviations.push_back(value > result.median_total_ns ? value - result.median_total_ns
                                                            : result.median_total_ns - value);
    }
    result.noise = result.median_total_ns > 0 ? median(deviations) / result.median_total_ns : 0;
}

/// Picks an arena large enough for the trace: at least twice the peak live
/// requested bytes (rounded up to a power of two), doubled until an untimed
/// replay succeeds.
std::size_t choose_capacity(const Trace& trace) {
    std::size_t capacity = std::max<std::size_t>(
        std::bit_ceil(static_cast<std::size_t>(trace.peak_live_requested) * 2), 1U << 20);
    for (int attempt = 0; attempt < 8; ++attempt) {
        nanoalloc::PoolConfig config;
        config.capacity_bytes = capacity;
        config.track_requested_bytes = false;
        MemoryResult probe;
        if (measure_pool(trace, config, probe)) {
            return capacity;
        }
        capacity *= 2;
    }
    throw std::runtime_error("could not find an arena capacity for workload " + trace.name);
}

std::string format_mib(std::uint64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(bytes) / kMiB);
    return buffer;
}

void print_header(const Options& options, const PlatformInfo& platform) {
    std::printf("nanoalloc benchmark %s\n", nanoalloc::version_string);
    std::printf("  compiler   %s\n", platform.compiler.c_str());
    std::printf("  build      %s, flags \"%s\"%s\n",
                platform.build_type.empty() ? "(no build type)" : platform.build_type.c_str(),
                platform.cxx_flags.c_str(), platform.optimized ? "" : " [NOT OPTIMIZED]");
    if (!platform.sanitizers.empty()) {
        std::printf("  WARNING    built with sanitizers (%s); timings are not representative\n",
                    platform.sanitizers.c_str());
    }
    if (!platform.optimized || platform.assertions) {
        std::printf("  WARNING    unoptimized or assertion-enabled build; use Release for "
                    "meaningful timings\n");
    }
    std::printf("  platform   %s\n", platform.os.c_str());
    std::printf("  cpu        %s (%u logical CPUs)\n", platform.cpu.c_str(), platform.logical_cpus);
    std::printf("  system     %s\n", platform.system_allocator.c_str());
    std::printf("  method     %u warmup + %u measured iterations, medians; scale %.3g; "
                "seed 0x%" PRIx64 "\n\n",
                options.warmup, options.iterations, options.scale, options.seed);
}

void print_runtime_table(const std::vector<Result>& results) {
    std::printf("Runtime (median of measured iterations)\n");
    std::printf("%-14s %-17s %10s %10s %10s %9s %9s %10s %8s %15s\n", "workload", "allocator",
                "alloc", "free", "combined", "alloc", "free", "total", "noise", "combined");
    std::printf("%-14s %-17s %10s %10s %10s %9s %9s %10s %8s %15s\n", "", "", "Mops/s", "Mops/s",
                "Mops/s", "ns/op", "ns/op", "ms", "%", "min-max Mops/s");
    std::string previous;
    for (const Result& r : results) {
        const bool first = r.workload != previous;
        previous = r.workload;
        const double timed = r.median_alloc_ns + r.median_free_ns;
        char range[32];
        std::snprintf(range, sizeof(range), "%.1f-%.1f", r.min_combined_mops,
                      r.max_combined_mops);
        std::printf("%-14s %-17s %10.2f %10.2f %10.2f %9.2f %9.2f %10.2f %8.1f %15s\n",
                    first ? r.workload.c_str() : "", r.allocator.c_str(),
                    mops(r.allocations, r.median_alloc_ns), mops(r.deallocations, r.median_free_ns),
                    mops(r.allocations + r.deallocations, timed),
                    r.median_alloc_ns / static_cast<double>(r.allocations),
                    r.median_free_ns / static_cast<double>(r.deallocations),
                    r.median_total_ns / 1e6, r.noise * 100.0, range);
    }
    std::printf("\n");
}

void print_memory_table(const std::vector<Result>& results) {
    std::printf("Memory (untimed replay of the same trace)\n");
    std::printf("%-14s %-17s %9s %9s %9s %8s %7s %8s %9s %7s %9s %9s %6s\n", "workload",
                "allocator", "peak req", "peak use", "peak con", "slack", "slack", "max", "arena",
                "arena", "reserved", "metadata", "meta");
    std::printf("%-14s %-17s %9s %9s %9s %8s %7s %8s %9s %7s %9s %9s %6s\n", "", "", "MiB",
                "MiB", "MiB", "B/alloc", "%", "slack B", "MiB", "util %", "MiB", "KiB", "%");
    std::string previous;
    for (const Result& r : results) {
        const bool first = r.workload != previous;
        previous = r.workload;
        const MemoryResult& m = r.memory;
        const auto slack_total = static_cast<double>(m.total_usable - r.total_requested);
        char usable[16] = "n/a";
        char consumed[16] = "n/a";
        char slack_alloc[16] = "n/a";
        char slack_pct[16] = "n/a";
        char max_slack[16] = "n/a";
        char arena[16] = "-";
        char util[16] = "-";
        char reserved[16] = "-";
        char meta[16] = "-";
        char meta_pct[16] = "-";
        if (m.usable_known) {
            std::snprintf(usable, sizeof(usable), "%.2f", static_cast<double>(m.peak_usable) / kMiB);
            std::snprintf(slack_alloc, sizeof(slack_alloc), "%.1f",
                          slack_total / static_cast<double>(r.allocations));
            std::snprintf(slack_pct, sizeof(slack_pct), "%.1f",
                          100.0 * slack_total / static_cast<double>(r.total_requested));
            std::snprintf(max_slack, sizeof(max_slack), "%" PRIu64, m.max_slack);
        }
        if (m.consumed_known) {
            std::snprintf(consumed, sizeof(consumed), "%.2f",
                          static_cast<double>(m.peak_consumed) / kMiB);
        }
        if (m.arena_known) {
            std::snprintf(arena, sizeof(arena), "%.0f", static_cast<double>(m.capacity) / kMiB);
            std::snprintf(reserved, sizeof(reserved), "%.2f",
                          static_cast<double>(m.reserved) / kMiB);
            std::snprintf(util, sizeof(util), "%.1f",
                          100.0 * static_cast<double>(m.peak_consumed) /
                              static_cast<double>(m.capacity));
            const std::uint64_t bookkeeping = m.metadata + m.tracking;
            std::snprintf(meta, sizeof(meta), "%.0f", static_cast<double>(bookkeeping) / 1024.0);
            std::snprintf(meta_pct, sizeof(meta_pct), "%.2f",
                          100.0 * static_cast<double>(bookkeeping) /
                              static_cast<double>(m.capacity));
        }
        std::printf("%-14s %-17s %9.2f %9s %9s %8s %7s %8s %9s %7s %9s %9s %6s\n",
                    first ? r.workload.c_str() : "", r.allocator.c_str(),
                    static_cast<double>(r.peak_requested) / kMiB, usable, consumed, slack_alloc,
                    slack_pct, max_slack, arena, util, reserved, meta, meta_pct);
    }
    std::printf("\n");
}

void print_definitions() {
    std::printf(
        "Definitions\n"
        "  alloc/free Mops/s  operations per second within allocation / deallocation segments\n"
        "  combined Mops/s    all operations / (allocation time + deallocation time)\n"
        "  total ms           wall time of one full trace replay, including timer reads\n"
        "  noise              median absolute deviation of the total time across measured\n"
        "                     iterations, relative to the median\n"
        "  min-max            slowest and fastest measured iteration (combined Mops/s)\n"
        "  peak req           max over time of live requested bytes (from the trace)\n"
        "  peak use           max over time of live usable bytes (usable_size / malloc_usable_size)\n"
        "  peak con           max over time of arena bytes consumed by live allocations,\n"
        "                     including per-allocation overhead (nanoalloc only: the C runtime\n"
        "                     does not report the system allocator's per-chunk overhead exactly)\n"
        "  slack              usable - requested, averaged over all allocations (B/alloc) and\n"
        "                     relative to all requested bytes (%%); max = worst single allocation\n"
        "  arena / util       nanoalloc arena capacity / peak consumed as a share of it\n"
        "  reserved           PoolStats::reserved_bytes at the end of the replay: the budget\n"
        "                     charged (versions without that field: the whole arena)\n"
        "  metadata           PoolStats::metadata_bytes + tracking_bytes (bookkeeping), and\n"
        "                     its share of the arena\n"
        "  nanoalloc          PoolConfig::track_requested_bytes = false\n"
        "  nanoalloc-tracked  PoolConfig::track_requested_bytes = true\n"
        "  checksum           every replay reproduced the checksum computed from the trace\n\n");
}

void write_csv(const std::string& path, const std::vector<Result>& results,
               const PlatformInfo& platform) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot open " + path);
    }
    out << "workload,allocator,allocations,deallocations,alloc_ns_median,free_ns_median,"
           "total_ns_median,alloc_mops,free_mops,combined_mops,noise,peak_requested,"
           "peak_usable,peak_consumed,total_requested,total_usable,max_slack,capacity,"
           "metadata,tracking,splits,merges,reserved,combined_mops_min,combined_mops_max,"
           "samples,library_version,compiler,build_type,cpu\n";
    for (const Result& r : results) {
        const MemoryResult& m = r.memory;
        out << r.workload << ',' << r.allocator << ',' << r.allocations << ',' << r.deallocations
            << ',' << r.median_alloc_ns << ',' << r.median_free_ns << ',' << r.median_total_ns
            << ',' << mops(r.allocations, r.median_alloc_ns) << ','
            << mops(r.deallocations, r.median_free_ns) << ','
            << mops(r.allocations + r.deallocations, r.median_alloc_ns + r.median_free_ns) << ','
            << r.noise << ',' << r.peak_requested << ','
            << (m.usable_known ? std::to_string(m.peak_usable) : "") << ','
            << (m.consumed_known ? std::to_string(m.peak_consumed) : "") << ','
            << r.total_requested << ','
            << (m.usable_known ? std::to_string(m.total_usable) : "") << ','
            << (m.usable_known ? std::to_string(m.max_slack) : "") << ','
            << (m.arena_known ? std::to_string(m.capacity) : "") << ','
            << (m.arena_known ? std::to_string(m.metadata) : "") << ','
            << (m.arena_known ? std::to_string(m.tracking) : "") << ','
            << (m.arena_known ? std::to_string(m.splits) : "") << ','
            << (m.arena_known ? std::to_string(m.merges) : "") << ','
            << (m.arena_known ? std::to_string(m.reserved) : "") << ',' << r.min_combined_mops
            << ',' << r.max_combined_mops << ',' << r.samples.size() << ','
            << nanoalloc::version_string << ",\"" << platform.compiler << "\","
            << platform.build_type << ",\"" << platform.cpu << "\"\n";
    }
}

int run(const Options& options) {
    const PlatformInfo platform = platform_info();
    print_header(options, platform);

    std::vector<Result> results;
    std::uint64_t checksum = 0;  // sum of the validated per-trace checksums

    std::printf("Workloads\n");
    for (const std::string& name : options.workloads) {
        const Trace trace = make_trace(name, options.scale, options.seed);
        const std::size_t capacity = choose_capacity(trace);
        std::printf("  %-14s %s\n", trace.name.c_str(), trace.description.c_str());
        std::printf("  %-14s %" PRIu64 " allocations, %zu segments, peak %" PRIu64
                    " live objects / %s MiB requested, nanoalloc arena %s MiB\n",
                    "", trace.allocations, trace.segments.size(), trace.peak_live_count,
                    format_mib(trace.peak_live_requested).c_str(), format_mib(capacity).c_str());
        std::fflush(stdout);

        // Runners, in the user's allocator order.
        std::vector<std::unique_ptr<Runner>> runners;
        std::vector<Result> trace_results;
        for (const std::string& allocator : options.allocators) {
            Result result;
            result.workload = trace.name;
            result.allocator = allocator;
            result.allocations = trace.allocations;
            result.deallocations = trace.deallocations;
            result.peak_requested = trace.peak_live_requested;
            result.total_requested = trace.total_requested;
            if (allocator == "system") {
                result.memory = measure_system(trace);
                runners.push_back(std::make_unique<SystemRunner>());
            } else {
                nanoalloc::PoolConfig config;
                config.capacity_bytes = capacity;
                config.track_requested_bytes = allocator == "nanoalloc-tracked";
                if (!measure_pool(trace, config, result.memory)) {
                    throw std::runtime_error("memory replay failed for " + allocator);
                }
                runners.push_back(std::make_unique<PoolRunner>(
                    allocator == "nanoalloc" ? "nanoalloc" : "nanoalloc-tracked", config));
            }
            trace_results.push_back(std::move(result));
        }

        std::vector<void*> slots(trace.slot_count, nullptr);
        const std::uint64_t expected = expected_checksum(trace);
        const unsigned total_iterations = options.warmup + options.iterations;
        for (unsigned iteration = 0; iteration < total_iterations; ++iteration) {
            for (std::size_t k = 0; k < runners.size(); ++k) {
                // Rotate the allocator order: each allocator runs in each
                // position equally often.
                const std::size_t index = (k + iteration) % runners.size();
                std::uint64_t replay_checksum = 0;
                const Sample sample = runners[index]->run(trace, slots, replay_checksum);
                if (replay_checksum != expected) {
                    throw std::runtime_error("checksum mismatch replaying " + trace.name +
                                             " with " + runners[index]->name() +
                                             ": blocks were overwritten or overlapped");
                }
                if (iteration >= options.warmup) {
                    trace_results[index].samples.push_back(sample);
                }
            }
        }
        checksum += expected;
        for (Result& result : trace_results) {
            summarize(result);
            if (options.verbose) {
                std::printf("  %-14s %-17s total ms:", result.workload.c_str(),
                            result.allocator.c_str());
                for (const Sample& sample : result.samples) {
                    std::printf(" %.2f", sample.total_ns / 1e6);
                }
                std::printf("\n");
            }
            results.push_back(std::move(result));
        }
    }
    std::printf("\n");

    print_runtime_table(results);
    print_memory_table(results);
    print_definitions();
    std::printf("checksum 0x%" PRIx64 " (validated for every replay)\n", checksum);

    if (!options.csv_path.empty()) {
        write_csv(options.csv_path, results, platform);
        std::printf("results written to %s\n", options.csv_path.c_str());
    }
    return 0;
}

}  // namespace
}  // namespace nanobench

int main(int argc, char** argv) {
    try {
        return nanobench::run(nanobench::parse_options(argc, argv));
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "error: allocation failure during the benchmark\n");
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
