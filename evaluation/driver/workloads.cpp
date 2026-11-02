// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "workloads.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <utility>

namespace nanobench {

std::uint32_t Rng::log_uniform(std::uint32_t low, std::uint32_t high) {
    // Sizes are drawn from [low, high): pick one of the power-of-two bands
    // [low << b, low << (b + 1)) uniformly, then a size uniformly within it.
    if (low == 0 || high <= low || !std::has_single_bit(high / low) || high % low != 0) {
        throw std::logic_error("log_uniform: high / low must be a power of two");
    }
    const auto bands = static_cast<std::uint64_t>(std::countr_zero(high / low));
    const std::uint64_t band_low = std::uint64_t{low} << below(bands);
    return static_cast<std::uint32_t>(between(band_low, 2 * band_low - 1));
}

namespace {

std::uint64_t scaled(std::uint64_t count, double scale) {
    const auto value = static_cast<std::uint64_t>(std::llround(static_cast<double>(count) * scale));
    return std::max<std::uint64_t>(value, 1);
}

/// Records operations, assigns slots and keeps the per-trace statistics.
class TraceBuilder {
public:
    TraceBuilder(const WorkloadInfo& info, std::uint64_t seed) : rng(seed) {
        trace_.name = info.name;
        trace_.description = info.description;
    }

    std::uint32_t allocate(std::uint32_t size) {
        std::uint32_t slot = 0;
        if (!free_slots_.empty()) {
            slot = free_slots_.back();
            free_slots_.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(slot_size_.size());
            slot_size_.push_back(0);
            position_.push_back(0);
        }
        slot_size_[slot] = size;
        position_[slot] = static_cast<std::uint32_t>(live_.size());
        live_.push_back(slot);
        append(Phase::Allocate, {slot, size});
        ++trace_.allocations;
        trace_.total_requested += size;
        live_requested_ += size;
        trace_.peak_live_requested = std::max(trace_.peak_live_requested, live_requested_);
        trace_.peak_live_count =
            std::max<std::uint64_t>(trace_.peak_live_count, live_.size());
        return slot;
    }

    void deallocate(std::uint32_t slot) {
        const std::uint32_t index = position_[slot];
        const std::uint32_t moved = live_.back();
        live_[index] = moved;
        position_[moved] = index;
        live_.pop_back();
        free_slots_.push_back(slot);
        append(Phase::Deallocate, {slot, slot_size_[slot]});
        ++trace_.deallocations;
        live_requested_ -= slot_size_[slot];
    }

    void deallocate_random() {
        deallocate(live_[static_cast<std::size_t>(rng.below(live_.size()))]);
    }

    void drain_random() {
        while (!live_.empty()) {
            deallocate_random();
        }
    }

    [[nodiscard]] std::size_t live() const { return live_.size(); }
    [[nodiscard]] std::uint64_t allocations() const { return trace_.allocations; }

    Trace finish() {
        if (!live_.empty()) {
            throw std::logic_error("trace does not release every allocation");
        }
        trace_.slot_count = static_cast<std::uint32_t>(slot_size_.size());
        return std::move(trace_);
    }

    Rng rng;

private:
    void append(Phase phase, Op op) {
        const auto index = static_cast<std::uint32_t>(trace_.ops.size());
        if (trace_.segments.empty() || trace_.segments.back().phase != phase) {
            trace_.segments.push_back({phase, index, index});
        }
        trace_.ops.push_back(op);
        trace_.segments.back().end = index + 1;
    }

    Trace trace_;
    std::vector<std::uint32_t> live_;
    std::vector<std::uint32_t> position_;   // slot -> index in live_
    std::vector<std::uint32_t> slot_size_;  // slot -> size of its allocation
    std::vector<std::uint32_t> free_slots_;
    std::uint64_t live_requested_ = 0;
};

const WorkloadInfo kWorkloads[] = {
    {"small", "16-128 B objects, 20k live, random replacement in batches of 32-128"},
    {"medium", "128-4096 B objects, 4k live, random replacement in batches of 32-128"},
    {"mixed", "80% 16-256 B, 18% 256 B-4 KiB, 2% 4-64 KiB; 8k live, random replacement"},
    {"lifo", "50k objects of 16-1024 B allocated in sequence, freed in reverse order"},
    {"random", "16-2048 B, random alloc/free batches of 16-256, random victims, ~10k live"},
    {"bursts", "bursts of 64-1024 short-lived 16-256 B objects freed FIFO, 1k background"},
    {"long_short", "short-lived 16-512 B batches interleaved with up to 25k long-lived 16 B-4 KiB"},
    {"fragmentation", "fill 20k x 32-64 B, free every other, add 2.5k x 512-2048 B, refill holes"},
    {"reuse", "batches of 256 objects of 40-56 B, allocated and freed repeatedly"},
};

const WorkloadInfo& info(const char* name) {
    for (const WorkloadInfo& workload : kWorkloads) {
        if (std::string(workload.name) == name) {
            return workload;
        }
    }
    throw std::invalid_argument(std::string("unknown workload: ") + name);
}

/// Steady-state random replacement: fill to `live_target`, then repeatedly
/// free a random batch of victims and allocate an equal batch.
template <class SizeFn>
Trace steady_replacement(const char* name, std::uint64_t live_target, std::uint64_t total,
                         std::uint64_t seed, SizeFn size) {
    TraceBuilder builder(info(name), seed);
    while (builder.live() < live_target) {
        builder.allocate(size(builder.rng));
    }
    while (builder.allocations() < total) {
        const auto batch = static_cast<std::size_t>(builder.rng.between(32, 128));
        for (std::size_t i = 0; i < batch; ++i) {
            builder.deallocate_random();
        }
        for (std::size_t i = 0; i < batch; ++i) {
            builder.allocate(size(builder.rng));
        }
    }
    builder.drain_random();
    return builder.finish();
}

Trace small_trace(double scale, std::uint64_t seed) {
    return steady_replacement("small", 20000, scaled(1000000, scale), seed,
                              [](Rng& rng) { return rng.log_uniform(16, 128); });
}

Trace medium_trace(double scale, std::uint64_t seed) {
    return steady_replacement("medium", 4000, scaled(400000, scale), seed,
                              [](Rng& rng) { return rng.log_uniform(128, 4096); });
}

Trace mixed_trace(double scale, std::uint64_t seed) {
    return steady_replacement("mixed", 8000, scaled(600000, scale), seed, [](Rng& rng) {
        const std::uint64_t pick = rng.below(100);
        if (pick < 80) {
            return rng.log_uniform(16, 256);
        }
        if (pick < 98) {
            return rng.log_uniform(256, 4096);
        }
        return rng.log_uniform(4096, 65536);
    });
}

Trace lifo_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("lifo"), seed);
    const std::uint64_t rounds = scaled(20, scale);
    std::vector<std::uint32_t> slots;
    for (std::uint64_t round = 0; round < rounds; ++round) {
        slots.clear();
        for (int i = 0; i < 50000; ++i) {
            slots.push_back(builder.allocate(builder.rng.log_uniform(16, 1024)));
        }
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            builder.deallocate(*it);
        }
    }
    return builder.finish();
}

Trace random_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("random"), seed);
    const std::uint64_t total = scaled(600000, scale);
    constexpr std::size_t kTarget = 10000;
    while (builder.allocations() < total) {
        const auto batch = static_cast<std::size_t>(builder.rng.between(16, 256));
        const bool allocate = builder.live() < batch ||
                              builder.rng.chance(builder.live() < kTarget ? 60U : 40U);
        for (std::size_t i = 0; i < batch; ++i) {
            if (allocate) {
                builder.allocate(builder.rng.log_uniform(16, 2048));
            } else {
                builder.deallocate_random();
            }
        }
    }
    builder.drain_random();
    return builder.finish();
}

Trace bursts_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("bursts"), seed);
    const std::uint64_t total = scaled(1000000, scale);
    std::vector<std::uint32_t> background;
    for (int i = 0; i < 1000; ++i) {
        background.push_back(builder.allocate(builder.rng.log_uniform(16, 256)));
    }
    std::vector<std::uint32_t> burst;
    while (builder.allocations() < total) {
        burst.clear();
        const auto count = builder.rng.between(64, 1024);
        for (std::uint64_t i = 0; i < count; ++i) {
            burst.push_back(builder.allocate(builder.rng.log_uniform(16, 256)));
        }
        for (std::uint32_t slot : burst) {  // FIFO: oldest first
            builder.deallocate(slot);
        }
    }
    for (std::uint32_t slot : background) {
        builder.deallocate(slot);
    }
    return builder.finish();
}

Trace long_short_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("long_short"), seed);
    const std::uint64_t total = scaled(800000, scale);
    constexpr std::size_t kLongCap = 25000;
    std::size_t long_lived = 0;
    std::deque<std::vector<std::uint32_t>> pending;  // short-lived batches, oldest first
    for (std::uint64_t round = 0; builder.allocations() < total; ++round) {
        std::vector<std::uint32_t> batch;
        for (int i = 0; i < 64; ++i) {
            batch.push_back(builder.allocate(builder.rng.log_uniform(16, 512)));
            // Long-lived objects are interleaved with the short-lived ones, so
            // they end up scattered across the address space.
            if (round % 4 == 0 && i % 4 == 0 && long_lived < kLongCap) {
                builder.allocate(builder.rng.log_uniform(16, 4096));
                ++long_lived;
            }
        }
        pending.push_back(std::move(batch));
        if (pending.size() > 2) {
            for (std::uint32_t slot : pending.front()) {
                builder.deallocate(slot);
            }
            pending.pop_front();
        }
    }
    for (const auto& batch : pending) {
        for (std::uint32_t slot : batch) {
            builder.deallocate(slot);
        }
    }
    builder.drain_random();  // the long-lived objects
    return builder.finish();
}

Trace fragmentation_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("fragmentation"), seed);
    const std::uint64_t cycles = scaled(25, scale);
    std::vector<std::uint32_t> small;
    for (std::uint64_t cycle = 0; cycle < cycles; ++cycle) {
        small.clear();
        for (int i = 0; i < 20000; ++i) {
            small.push_back(builder.allocate(builder.rng.log_uniform(32, 64)));
        }
        for (std::size_t i = 0; i < small.size(); i += 2) {  // punch holes
            builder.deallocate(small[i]);
        }
        for (int i = 0; i < 2500; ++i) {  // larger objects cannot use the holes
            builder.allocate(builder.rng.log_uniform(512, 2048));
        }
        for (int i = 0; i < 10000; ++i) {  // small objects can
            builder.allocate(builder.rng.log_uniform(32, 64));
        }
        builder.drain_random();
    }
    return builder.finish();
}

Trace reuse_trace(double scale, std::uint64_t seed) {
    TraceBuilder builder(info("reuse"), seed);
    const std::uint64_t rounds = scaled(4000, scale);
    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (int i = 0; i < 256; ++i) {
            builder.allocate(static_cast<std::uint32_t>(builder.rng.between(40, 56)));
        }
        builder.drain_random();
    }
    return builder.finish();
}

}  // namespace

const std::vector<WorkloadInfo>& workloads() {
    static const std::vector<WorkloadInfo> list(std::begin(kWorkloads), std::end(kWorkloads));
    return list;
}

Trace make_trace(const std::string& name, double scale, std::uint64_t seed) {
    // Each workload gets its own stream derived from the global seed.
    std::uint64_t stream = seed;
    for (char c : name) {
        stream = stream * 1099511628211ULL + static_cast<unsigned char>(c);
    }
    if (name == "small") return small_trace(scale, stream);
    if (name == "medium") return medium_trace(scale, stream);
    if (name == "mixed") return mixed_trace(scale, stream);
    if (name == "lifo") return lifo_trace(scale, stream);
    if (name == "random") return random_trace(scale, stream);
    if (name == "bursts") return bursts_trace(scale, stream);
    if (name == "long_short") return long_short_trace(scale, stream);
    if (name == "fragmentation") return fragmentation_trace(scale, stream);
    if (name == "reuse") return reuse_trace(scale, stream);
    throw std::invalid_argument("unknown workload: " + name);
}

}  // namespace nanobench
