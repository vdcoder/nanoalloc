// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Deterministic allocation traces for the benchmark.
//
// A trace is a precomputed list of operations grouped into segments. Each
// segment contains only allocations or only deallocations, so the runner can
// time segments with one clock read per segment instead of one per
// operation. Traces depend only on the workload definition, the scale and the
// seed; they are generated before any timing starts and are independent of
// the allocator being measured.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nanobench {

/// SplitMix64: fast, deterministic and identical on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    /// Uniform in [0, bound).
    std::uint64_t below(std::uint64_t bound) { return next() % bound; }
    /// Uniform in [low, high].
    std::uint64_t between(std::uint64_t low, std::uint64_t high) {
        return low + below(high - low + 1);
    }
    /// Log-uniform in [low, high] (low >= 1): every power-of-two band is
    /// equally likely, and sizes are uniform within a band.
    std::uint32_t log_uniform(std::uint32_t low, std::uint32_t high);
    bool chance(unsigned percent) { return below(100) < percent; }

private:
    std::uint64_t state_;
};

enum class Phase : std::uint8_t { Allocate, Deallocate };

/// One operation. For allocations `size` is the request; for deallocations it
/// repeats the size of the allocation being released (used for sized APIs and
/// to re-read the touched bytes).
struct Op {
    std::uint32_t slot;
    std::uint32_t size;
};

struct Segment {
    Phase phase;
    std::uint32_t begin;  // index of the first op
    std::uint32_t end;    // one past the last op
};

struct Trace {
    std::string name;
    std::string description;
    std::vector<Op> ops;
    std::vector<Segment> segments;
    std::uint32_t slot_count = 0;           // distinct live slots needed
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t total_requested = 0;      // sum of all allocation sizes
    std::uint64_t peak_live_requested = 0;  // max over time of live requested bytes
    std::uint64_t peak_live_count = 0;      // max over time of live allocations
};

struct WorkloadInfo {
    const char* name;
    const char* description;
};

/// All workloads, in report order.
const std::vector<WorkloadInfo>& workloads();

/// Builds the trace for workload `name`. `scale` multiplies operation counts.
/// Throws std::invalid_argument for an unknown name.
Trace make_trace(const std::string& name, double scale, std::uint64_t seed);

}  // namespace nanobench
