// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Paired comparison of this nanoalloc with another version of it (the
// "reference"), built into the same executable.
//
// Shared machines often vary by tens of percent between runs of the same
// program, which hides the differences an optimization makes. This program
// links both versions (the reference compiled with its namespace renamed to
// nanoalloc_reference) and alternates them iteration by iteration on the
// same trace: iteration i runs current then reference when i is even, and the
// other way round when i is odd. Each iteration yields one ratio
// current / reference per phase, so drift in machine speed affects both
// sides of a ratio alike. Reported: the median ratio and its interquartile
// range, and the median throughput of each version.
//
// Both versions replay the nanoalloc_bench traces with the arena size
// nanoalloc_bench would choose, write and read a stamp in every block, and
// must produce the same checksum.
//
// Within one process, memory and code placement (address-space layout
// randomization, physical pages) can favour one side consistently: comparing
// a build with an identical copy of itself has shown single-process medians
// up to 20% away from 1 on the smallest workload (reuse). Run several
// processes and compare their medians; such a self-comparison measures the
// noise floor.
//
// Build: configure with -DNANOALLOC_REFERENCE_SOURCE_DIR=<another nanoalloc
// source tree> (for example a worktree of an earlier commit); the target is
// nanoalloc_paired.
#include <nanoalloc/pool.hpp>
#include <nanoalloc/version.hpp>

#define nanoalloc nanoalloc_reference
#include NANOALLOC_REFERENCE_POOL_HEADER
#undef nanoalloc

#include <algorithm>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "platform.hpp"
#include "workloads.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace nanobench;

template <class T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    static const void* volatile sink = nullptr;
    sink = &value;
#endif
}

struct Sample {
    double alloc_ns = 0;
    double free_ns = 0;
    std::uint64_t checksum = 0;
};

template <class PoolT>
Sample replay(const Trace& trace, PoolT& pool, std::vector<void*>& slots) {
    Sample sample;
    std::uint64_t sum = 0;
    Clock::time_point last = Clock::now();
    for (const Segment& segment : trace.segments) {
        if (segment.phase == Phase::Allocate) {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = trace.ops[i];
                void* ptr = pool.allocate(op.size);
                const std::uint64_t stamp = (std::uint64_t{op.slot} << 32) | op.size;
                std::memcpy(ptr, &stamp, sizeof(stamp));  // trace sizes are >= 16
                static_cast<unsigned char*>(ptr)[op.size - 1] = static_cast<unsigned char>(op.slot);
                slots[op.slot] = ptr;
            }
        } else {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = trace.ops[i];
                void* ptr = slots[op.slot];
                std::uint64_t stamp = 0;
                std::memcpy(&stamp, ptr, sizeof(stamp));
                sum += stamp + static_cast<const unsigned char*>(ptr)[op.size - 1];
                pool.deallocate(ptr);
            }
        }
        do_not_optimize(sum);
        const Clock::time_point now = Clock::now();
        const double elapsed = std::chrono::duration<double, std::nano>(now - last).count();
        (segment.phase == Phase::Allocate ? sample.alloc_ns : sample.free_ns) += elapsed;
        last = now;
    }
    sample.checksum = sum;
    return sample;
}

template <class PoolT>
bool replay_fits(const Trace& trace, std::size_t capacity) {
    PoolT pool({capacity});
    std::vector<void*> slots(trace.slot_count, nullptr);
    for (const Segment& segment : trace.segments) {
        for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
            const Op op = trace.ops[i];
            if (segment.phase == Phase::Allocate) {
                slots[op.slot] = pool.try_allocate(op.size);
                if (slots[op.slot] == nullptr) {
                    for (void* live : slots) {
                        pool.deallocate(live);
                    }
                    return false;
                }
            } else {
                pool.deallocate(slots[op.slot]);
                slots[op.slot] = nullptr;
            }
        }
    }
    return true;
}

/// nanoalloc_bench's rule (2 x peak live requested, power of two, at least
/// 1 MiB, doubled until the replay fits), applied to both versions.
std::size_t choose_capacity(const Trace& trace) {
    std::size_t capacity = std::max<std::size_t>(
        std::bit_ceil(static_cast<std::size_t>(trace.peak_live_requested) * 2), 1U << 20);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (replay_fits<nanoalloc::Pool>(trace, capacity) &&
            replay_fits<nanoalloc_reference::Pool>(trace, capacity)) {
            return capacity;
        }
        capacity *= 2;
    }
    throw std::runtime_error("no arena capacity fits workload " + trace.name);
}

double quantile(std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    const double position = q * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(position);
    const std::size_t high = std::min(low + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(low);
    return values[low] * (1.0 - fraction) + values[high] * fraction;
}

struct Options {
    double scale = 1.0;
    unsigned iterations = 30;
    unsigned warmup = 2;
    std::uint64_t seed = 0x6E616E6FULL;  // nanoalloc_bench's default
    std::vector<std::string> workloads;
};

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
        if (arg == "--scale") {
            options.scale = std::stod(value());
        } else if (arg == "--iterations") {
            options.iterations = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--warmup") {
            options.warmup = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--seed") {
            options.seed = std::stoull(value(), nullptr, 0);
        } else if (arg == "--workload" || arg == "--workloads") {
            std::stringstream stream(value());
            std::string item;
            while (std::getline(stream, item, ',')) {
                options.workloads.push_back(item);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: nanoalloc_paired [--scale F] [--iterations N] [--warmup N] [--seed N]\n"
                "                        [--workload LIST]\n");
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (!(options.scale > 0 && options.scale <= 1000) || options.iterations < 2 ||
        options.iterations > 100000 || options.warmup > 100000) {
        throw std::invalid_argument("invalid --scale, --iterations or --warmup");
    }
    if (options.workloads.empty()) {
        for (const WorkloadInfo& workload : workloads()) {
            options.workloads.emplace_back(workload.name);
        }
    }
    return options;
}

int run(const Options& options) {
    const PlatformInfo platform = platform_info();
    std::printf("nanoalloc paired comparison: current (%s) vs reference\n",
                nanoalloc::version_string);
    std::printf("  compiler   %s\n", platform.compiler.c_str());
    std::printf("  build      %s, flags \"%s\"%s\n", platform.build_type.c_str(),
                platform.cxx_flags.c_str(), platform.optimized ? "" : " [NOT OPTIMIZED]");
    std::printf("  platform   %s\n", platform.os.c_str());
    std::printf("  cpu        %s (%u logical CPUs)\n", platform.cpu.c_str(), platform.logical_cpus);
    std::printf("  method     %u warmup + %u alternating iterations per workload; scale %.3g; "
                "seed 0x%" PRIx64 "\n\n",
                options.warmup, options.iterations, options.scale, options.seed);
    std::printf("Time ratio current / reference: median [interquartile range]; below 1 means "
                "the current version is faster\n");
    std::printf("%-14s %21s %21s %21s %11s %11s\n", "workload", "allocation", "deallocation",
                "combined", "cur Mops/s", "ref Mops/s");

    for (const std::string& name : options.workloads) {
        const Trace trace = make_trace(name, options.scale, options.seed);
        const std::size_t capacity = choose_capacity(trace);
        nanoalloc::Pool current({capacity});
        nanoalloc_reference::Pool reference({capacity});
        std::vector<void*> slots(trace.slot_count, nullptr);
        std::vector<double> alloc_ratio;
        std::vector<double> free_ratio;
        std::vector<double> total_ratio;
        std::vector<double> current_mops;
        std::vector<double> reference_mops;
        const double operations = static_cast<double>(trace.allocations + trace.deallocations);
        for (unsigned iteration = 0; iteration < options.warmup + options.iterations; ++iteration) {
            Sample c;
            Sample r;
            if (iteration % 2 == 0) {
                c = replay(trace, current, slots);
                r = replay(trace, reference, slots);
            } else {
                r = replay(trace, reference, slots);
                c = replay(trace, current, slots);
            }
            if (c.checksum != r.checksum) {
                throw std::runtime_error("checksum mismatch in workload " + name);
            }
            if (iteration < options.warmup) {
                continue;
            }
            alloc_ratio.push_back(c.alloc_ns / r.alloc_ns);
            free_ratio.push_back(c.free_ns / r.free_ns);
            total_ratio.push_back((c.alloc_ns + c.free_ns) / (r.alloc_ns + r.free_ns));
            current_mops.push_back(operations / (c.alloc_ns + c.free_ns) * 1e3);
            reference_mops.push_back(operations / (r.alloc_ns + r.free_ns) * 1e3);
        }
        auto cell = [](const std::vector<double>& values) {
            char text[48];
            std::snprintf(text, sizeof(text), "%.3f [%.3f-%.3f]", quantile(values, 0.5),
                          quantile(values, 0.25), quantile(values, 0.75));
            return std::string(text);
        };
        std::printf("%-14s %21s %21s %21s %11.2f %11.2f\n", name.c_str(),
                    cell(alloc_ratio).c_str(), cell(free_ratio).c_str(),
                    cell(total_ratio).c_str(), quantile(current_mops, 0.5),
                    quantile(reference_mops, 0.5));
        std::fflush(stdout);
    }
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
