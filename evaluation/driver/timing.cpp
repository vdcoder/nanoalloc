// nanoalloc evaluation driver: workload timing and memory footprint.
//
// Trusted harness of the evaluation package. It is compiled together with one
// implementation's sources and uses only the public API of the reference
// (buddy) baseline: nanoalloc::Pool, PoolConfig, allocate, try_allocate,
// deallocate and stats.
//
// Timing mode (default), for each of the nine original workloads:
//   1. Generate the trace (workloads.cpp, unchanged from the baseline's
//      benchmark) and the checksum a correct replay produces. Untimed.
//   2. Choose a capacity: twice the trace's peak live requested bytes rounded
//      up to a power of two, at least 1 MiB, doubled until an untimed replay
//      with try_allocate succeeds. Each implementation gets the capacity it
//      needs; the value is reported.
//   3. Construct the pool (untimed), run --warmup replays, then --iterations
//      measured replays. A measured replay is timed as a whole: every
//      allocation and deallocation of the trace, plus writing a stamp into
//      each new block and reading it back before its deallocation. Every
//      replay, warmup included, must reproduce the expected checksum.
//
// Footprint mode (--footprint): the smallest pool capacity with which the
// implementation completes each trace, found by bisection between the trace's
// peak live requested bytes and the capacity of step 2 (assumes that a larger
// capacity never fails where a smaller one succeeds). This measures actual
// memory consumption without relying on usable_size() or the statistics.
//
// Output: one JSON document on stdout. Exit status 0 on success, 2 if an
// implementation produced a wrong result or failed an allocation, 1 on usage
// errors.
#include <nanoalloc/pool.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.hpp"
#include "workloads.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using nanobench::Op;
using nanobench::Phase;
using nanobench::Segment;
using nanobench::Trace;

struct Options {
    std::vector<std::string> workloads;
    unsigned warmup = 2;
    unsigned iterations = 9;
    double scale = 1.0;
    std::uint64_t seed = 0x6E616E6FULL;  // the original benchmark's seed
    bool footprint = false;
};

struct ReplayError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- stamps and checksum ------------------------------------------------------

/// Writes the slot number and size into the first (up to) 8 bytes and the
/// slot's low byte into the last byte of a new block.
inline void stamp(void* ptr, std::uint32_t size, std::uint32_t slot) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    const std::uint64_t value = (std::uint64_t{slot} << 32) | size;
    if (size >= sizeof(value)) {
        std::memcpy(bytes, &value, sizeof(value));
    }
    if (size > 0) {
        bytes[size - 1] = static_cast<unsigned char>(slot);
    }
}

/// Reads back what stamp() wrote.
inline std::uint64_t read_stamp(const void* ptr, std::uint32_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    std::uint64_t value = 0;
    if (size >= sizeof(value)) {
        std::memcpy(&value, bytes, sizeof(value));
    }
    return size > 0 ? value + bytes[size - 1] : value;
}

std::uint64_t expected_checksum(const Trace& trace) {
    std::uint64_t sum = 0;
    for (const Segment& segment : trace.segments) {
        if (segment.phase != Phase::Deallocate) {
            continue;
        }
        for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
            const Op op = trace.ops[i];
            unsigned char buffer[16] = {};
            const std::uint32_t size = std::min<std::uint32_t>(op.size, 16);
            if (op.size <= 16) {  // stamp and last byte may overlap: simulate
                stamp(buffer, size, op.slot);
                sum += read_stamp(buffer, size);
            } else {
                sum += ((std::uint64_t{op.slot} << 32) | op.size) + (op.slot & 0xFFU);
            }
        }
    }
    return sum;
}

// ---- replays ------------------------------------------------------------------------

nanoalloc::PoolConfig config_for(std::size_t capacity) {
    nanoalloc::PoolConfig config;
    config.capacity_bytes = capacity;
    return config;
}

/// Untimed replay with try_allocate. Returns false if an allocation failed
/// (everything allocated so far is released).
bool replay_fits(const Trace& trace, std::size_t capacity) {
    nanoalloc::Pool pool(config_for(capacity));
    std::vector<void*> slots(trace.slot_count, nullptr);
    for (const Segment& segment : trace.segments) {
        for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
            const Op op = trace.ops[i];
            if (segment.phase == Phase::Allocate) {
                void* ptr = pool.try_allocate(op.size);
                if (ptr == nullptr) {
                    for (void*& live : slots) {
                        if (live != nullptr) {
                            pool.deallocate(live);
                            live = nullptr;
                        }
                    }
                    return false;
                }
                slots[op.slot] = ptr;
            } else {
                pool.deallocate(slots[op.slot]);
                slots[op.slot] = nullptr;
            }
        }
    }
    return true;
}

std::size_t choose_capacity(const Trace& trace, unsigned& attempts) {
    std::size_t capacity = std::max<std::size_t>(
        std::bit_ceil(static_cast<std::size_t>(trace.peak_live_requested) * 2), 1U << 20);
    for (attempts = 1; attempts <= 16; ++attempts) {
        if (replay_fits(trace, capacity)) {
            return capacity;
        }
        capacity *= 2;
    }
    throw ReplayError("no capacity up to " + std::to_string(capacity / 2) +
                      " bytes completes workload " + trace.name);
}

/// One measured replay: returns elapsed nanoseconds; `checksum` receives the
/// stamps read back.
double timed_replay(nanoalloc::Pool& pool, const Trace& trace, std::vector<void*>& slots,
                    std::uint64_t& checksum) {
    const Op* ops = trace.ops.data();
    void** slot_ptrs = slots.data();
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (const Segment& segment : trace.segments) {
        if (segment.phase == Phase::Allocate) {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = ops[i];
                void* ptr = pool.allocate(op.size);
                stamp(ptr, op.size, op.slot);
                slot_ptrs[op.slot] = ptr;
            }
        } else {
            for (std::uint32_t i = segment.begin; i < segment.end; ++i) {
                const Op op = ops[i];
                void* ptr = slot_ptrs[op.slot];
                sum += read_stamp(ptr, op.size);
                pool.deallocate(ptr);
            }
        }
    }
    nanoeval::do_not_optimize(sum);
    const Clock::time_point end = Clock::now();
    checksum = sum;
    return std::chrono::duration<double, std::nano>(end - start).count();
}

std::string number_list(const std::vector<double>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << (i ? ", " : "") << static_cast<long long>(values[i]);
    }
    out << "]";
    return out.str();
}

std::string run_timing(const Trace& trace, const Options& options) {
    unsigned attempts = 0;
    const std::size_t capacity = choose_capacity(trace, attempts);
    const std::uint64_t expected = expected_checksum(trace);
    nanoalloc::Pool pool(config_for(capacity));
    std::vector<void*> slots(trace.slot_count, nullptr);
    std::vector<double> warmup;
    std::vector<double> measured;
    for (unsigned i = 0; i < options.warmup + options.iterations; ++i) {
        std::uint64_t checksum = 0;
        double ns = 0;
        try {
            ns = timed_replay(pool, trace, slots, checksum);
        } catch (const std::bad_alloc&) {
            throw ReplayError("allocation failed while replaying " + trace.name +
                              " with the capacity that completed it before");
        }
        if (checksum != expected) {
            throw ReplayError("checksum mismatch in " + trace.name + ": expected " +
                              nanoeval::hex64(expected) + ", got " + nanoeval::hex64(checksum) +
                              " (blocks were overwritten or overlapped)");
        }
        (i < options.warmup ? warmup : measured).push_back(ns);
    }
    const nanoalloc::PoolStats stats = pool.stats();
    std::ostringstream out;
    out << "{\"name\": " << nanoeval::json_string(trace.name)
        << ", \"allocations\": " << trace.allocations << ", \"deallocations\": " << trace.deallocations
        << ", \"peak_live_requested\": " << trace.peak_live_requested
        << ", \"peak_live_count\": " << trace.peak_live_count << ", \"capacity\": " << capacity
        << ", \"capacity_attempts\": " << attempts
        << ", \"checksum\": " << nanoeval::json_string(nanoeval::hex64(expected))
        << ", \"warmup_ns\": " << number_list(warmup)
        << ", \"iterations_ns\": " << number_list(measured)
        << ", \"self_reported\": {\"capacity_bytes\": " << stats.capacity_bytes
        << ", \"metadata_bytes\": " << stats.metadata_bytes
        << ", \"peak_usable_bytes\": " << stats.peak_usable_bytes
        << ", \"peak_consumed_bytes\": " << stats.peak_consumed_bytes
        << ", \"live_allocations_after\": " << stats.live_allocations << "}}";
    return out.str();
}

std::string run_footprint(const Trace& trace) {
    unsigned attempts = 0;
    std::size_t high = choose_capacity(trace, attempts);  // completes the trace
    std::size_t low = std::max<std::size_t>(16, (trace.peak_live_requested + 15) & ~std::size_t{15});
    unsigned probes = attempts;
    bool at_lower_bound = false;
    if (replay_fits(trace, low)) {  // only possible if payload is not really reserved
        high = low;
        at_lower_bound = true;
    } else {
        // Invariant: low fails, high succeeds.
        while (high - low > std::max<std::size_t>(high / 512, 16)) {
            const std::size_t middle = (low + (high - low) / 2) & ~std::size_t{15};
            if (middle <= low) {
                break;
            }
            ++probes;
            (replay_fits(trace, middle) ? high : low) = middle;
        }
    }
    std::ostringstream out;
    out << "{\"name\": " << nanoeval::json_string(trace.name)
        << ", \"peak_live_requested\": " << trace.peak_live_requested
        << ", \"minimum_capacity\": " << high << ", \"probes\": " << probes
        << ", \"at_lower_bound\": " << (at_lower_bound ? "true" : "false") << "}";
    return out.str();
}

unsigned parse_unsigned(const std::string& option, const std::string& text, unsigned low,
                        unsigned high) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || value < low || value > high) {
        throw std::invalid_argument(option + " expects an integer in [" + std::to_string(low) +
                                    ", " + std::to_string(high) + "]");
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
        if (arg == "--warmup") {
            options.warmup = parse_unsigned(arg, value(), 1, 1000);
        } else if (arg == "--iterations") {
            options.iterations = parse_unsigned(arg, value(), 1, 100000);
        } else if (arg == "--scale") {
            options.scale = std::stod(value());
        } else if (arg == "--seed") {
            options.seed = std::stoull(value(), nullptr, 0);
        } else if (arg == "--workloads") {
            std::stringstream list(value());
            std::string item;
            while (std::getline(list, item, ',')) {
                options.workloads.push_back(item);
            }
        } else if (arg == "--footprint") {
            options.footprint = true;
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (!(options.scale > 0 && options.scale <= 100)) {
        throw std::invalid_argument("--scale expects a value in (0, 100]");
    }
    if (options.workloads.empty()) {
        for (const nanobench::WorkloadInfo& workload : nanobench::workloads()) {
            options.workloads.emplace_back(workload.name);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nanoeval_timing: %s\n", error.what());
        return 1;
    }
    std::ostringstream out;
    out << "{\"driver\": \"nanoeval_timing\", \"mode\": "
        << (options.footprint ? "\"footprint\"" : "\"timing\"")
        << ", \"compiler\": " << nanoeval::json_string(nanoeval::compiler_description())
        << ", \"warmup\": " << options.warmup << ", \"iterations\": " << options.iterations
        << ", \"scale\": " << options.scale << ", \"seed\": " << options.seed << ", \"workloads\": [";
    try {
        for (std::size_t w = 0; w < options.workloads.size(); ++w) {
            const Trace trace = nanobench::make_trace(options.workloads[w], options.scale, options.seed);
            out << (w ? ", " : "")
                << (options.footprint ? run_footprint(trace) : run_timing(trace, options));
            std::fprintf(stderr, "nanoeval_timing: %s done\n", trace.name.c_str());
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nanoeval_timing: FAILED: %s\n", error.what());
        std::printf("{\"driver\": \"nanoeval_timing\", \"error\": %s}\n",
                    nanoeval::json_string(error.what()).c_str());
        return 2;
    }
    out << "]}";
    std::printf("%s\n", out.str().c_str());
    return 0;
}
