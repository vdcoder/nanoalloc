// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Algorithm-independent helpers shared by the test suites.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace nanotest {

/// Small, fast, deterministic PRNG (SplitMix64). Identical sequences on every
/// platform, unlike the std:: distributions.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// Uniform in [0, bound). `bound` must be non-zero.
    std::size_t below(std::size_t bound) {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(bound));
    }

    /// Uniform in [low, high].
    std::size_t between(std::size_t low, std::size_t high) { return low + below(high - low + 1); }

    /// Approximately log-uniform in [low, high]; low must be >= 1.
    std::size_t log_uniform(std::size_t low, std::size_t high) {
        const int low_bits = std::bit_width(low);
        const int high_bits = std::bit_width(high);
        const auto bits = static_cast<int>(between(static_cast<std::size_t>(low_bits),
                                                   static_cast<std::size_t>(high_bits)));
        const std::size_t range_low = std::max(low, std::size_t{1} << (bits - 1));
        const std::size_t range_high =
            std::min(high, bits >= 63 ? high : (std::size_t{1} << bits) - 1);
        return range_low >= range_high ? range_low : between(range_low, range_high);
    }

    bool chance(unsigned percent) { return below(100) < percent; }

private:
    std::uint64_t state_;
};

inline bool is_aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1)) == 0;
}

/// Pattern word for the 8-byte chunk starting at `offset` (a multiple of 8)
/// of a buffer tagged `tag`; it is stored in native byte order.
inline std::uint64_t pattern_word(std::uint64_t tag, std::size_t offset) {
    std::uint64_t z = tag + static_cast<std::uint64_t>(offset) * 0x9E3779B97F4A7C15ULL;
    z ^= z >> 29;
    return z * 0xBF58476D1CE4E5B9ULL;
}

/// Pattern byte for the trailing (< 8 byte) part of a buffer.
inline unsigned char pattern_byte(std::uint64_t tag, std::size_t offset) {
    return static_cast<unsigned char>(pattern_word(tag, offset) >> 56);
}

/// Fills [ptr, ptr + bytes) with a pattern derived from `tag`.
inline void fill_pattern(void* ptr, std::size_t bytes, std::uint64_t tag) {
    auto* data = static_cast<unsigned char*>(ptr);
    std::size_t offset = 0;
    for (; offset + 8 <= bytes; offset += 8) {
        const std::uint64_t word = pattern_word(tag, offset);
        std::memcpy(data + offset, &word, sizeof(word));
    }
    for (; offset < bytes; ++offset) {
        data[offset] = pattern_byte(tag, offset);
    }
}

/// Returns true if [ptr, ptr + bytes) still holds the pattern for `tag`.
inline bool check_pattern(const void* ptr, std::size_t bytes, std::uint64_t tag) {
    const auto* data = static_cast<const unsigned char*>(ptr);
    std::size_t offset = 0;
    for (; offset + 8 <= bytes; offset += 8) {
        const std::uint64_t word = pattern_word(tag, offset);
        std::uint64_t stored = 0;
        std::memcpy(&stored, data + offset, sizeof(stored));
        if (stored != word) {
            return false;
        }
    }
    for (; offset < bytes; ++offset) {
        if (data[offset] != pattern_byte(tag, offset)) {
            return false;
        }
    }
    return true;
}

/// Tracks live address ranges and detects overlaps on insertion.
class IntervalSet {
public:
    /// Inserts [ptr, ptr + bytes); returns false if it overlaps a live range.
    /// Zero-length ranges are treated as one byte long so that two zero-size
    /// allocations at the same address are reported as overlapping.
    bool insert(const void* ptr, std::size_t bytes) {
        const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
        const std::uintptr_t end = begin + std::max<std::size_t>(bytes, 1);
        auto next = ranges_.lower_bound(begin);
        if (next != ranges_.end() && next->first < end) {
            return false;
        }
        if (next != ranges_.begin()) {
            auto previous = std::prev(next);
            if (previous->second > begin) {
                return false;
            }
        }
        ranges_.emplace(begin, end);
        return true;
    }

    void erase(const void* ptr) { ranges_.erase(reinterpret_cast<std::uintptr_t>(ptr)); }
    [[nodiscard]] std::size_t size() const { return ranges_.size(); }

private:
    std::map<std::uintptr_t, std::uintptr_t> ranges_;
};

/// Deterministic Fisher-Yates shuffle.
template <class T>
void shuffle(std::vector<T>& values, Rng& rng) {
    for (std::size_t i = values.size(); i > 1; --i) {
        std::swap(values[i - 1], values[rng.below(i)]);
    }
}

}  // namespace nanotest
