// nanoalloc evaluation driver: correctness gate.
//
// Trusted checks of the evaluation package, compiled together with one
// implementation's sources and using only the public API of the reference
// (buddy) baseline. Single-threaded, as the baseline's Pool is.
//
// Categories:
//   api          the baseline's documented contract: construction, zero-size
//                requests, alignment, invalid requests, owns(), statistics
//                counts, full recovery after releasing everything, the
//                std::pmr and Allocator adapters
//   integrity    random allocation and release with every byte of every
//                reported usable_size() written and checked, no overlap of
//                live [ptr, ptr + usable_size) ranges, verify_integrity()
//   reuse        filling a pool to exhaustion, releasing everything and
//                filling it again must succeed at least as often
//   budget       a pool may not hand out more payload than its capacity
//   packing      positive-size requests get exactly round_up(n, 16) usable
//                bytes (at most 15 bytes of slack), also with alignment
//
// Actual consumption (pool capacity divided by the number of allocations of
// one size that fit in it) is reported per size, independent of
// usable_size() and of the statistics.
//
// Output: one JSON document on stdout. Exit status 0 when the driver ran to
// completion (whatever the check results), 1 on usage errors. A crash or
// abort means the implementation failed.
#include <nanoalloc/allocator.hpp>
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <memory_resource>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.hpp"

namespace {

constexpr std::size_t kKiB = 1024;
constexpr std::size_t kMiB = 1024 * 1024;

constexpr std::size_t round16(std::size_t n) { return (n + 15) & ~std::size_t{15}; }

/// SplitMix64: deterministic on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }
    bool chance(unsigned percent) { return below(100) < percent; }
    /// Roughly log-uniform in [1, high].
    std::size_t log_size(std::size_t high) {
        const unsigned bits =
            1 + static_cast<unsigned>(below(static_cast<std::size_t>(std::bit_width(high))));
        const std::size_t top = std::min<std::size_t>(high, (std::size_t{1} << bits) - 1);
        const std::size_t bottom = std::size_t{1} << (bits - 1);
        return bottom + below(top - bottom + 1);
    }

private:
    std::uint64_t state_;
};

// ---- patterns over whole blocks -------------------------------------------------

inline std::uint64_t pattern_word(std::uint64_t tag, std::size_t index) {
    std::uint64_t z = tag + index * 0x9E3779B97F4A7C15ULL;
    z ^= z >> 29;
    return z * 0xBF58476D1CE4E5B9ULL;
}

void fill(void* ptr, std::size_t bytes, std::uint64_t tag) {
    auto* data = static_cast<unsigned char*>(ptr);
    std::size_t offset = 0;
    for (; offset + 8 <= bytes; offset += 8) {
        const std::uint64_t word = pattern_word(tag, offset);
        std::memcpy(data + offset, &word, 8);
    }
    for (; offset < bytes; ++offset) {
        data[offset] = static_cast<unsigned char>(pattern_word(tag, offset) >> 56);
    }
}

bool intact(const void* ptr, std::size_t bytes, std::uint64_t tag) {
    const auto* data = static_cast<const unsigned char*>(ptr);
    std::size_t offset = 0;
    for (; offset + 8 <= bytes; offset += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, data + offset, 8);
        if (word != pattern_word(tag, offset)) {
            return false;
        }
    }
    for (; offset < bytes; ++offset) {
        if (data[offset] != static_cast<unsigned char>(pattern_word(tag, offset) >> 56)) {
            return false;
        }
    }
    return true;
}

bool aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1)) == 0;
}

nanoalloc::PoolConfig config_for(std::size_t capacity) {
    nanoalloc::PoolConfig config;
    config.capacity_bytes = capacity;
    return config;
}

// ---- check results ----------------------------------------------------------------

struct Check {
    std::string name;
    std::string category;
    bool passed = true;
    std::uint64_t cases = 0;
    std::uint64_t violations = 0;
    std::vector<std::string> examples;  // first few violations

    void expect(bool condition, const std::string& what) {
        ++cases;
        if (!condition) {
            passed = false;
            ++violations;
            if (examples.size() < 5) {
                examples.push_back(what);
            }
        }
    }
};

std::vector<Check> g_checks;

Check& begin(const std::string& name, const std::string& category) {
    // Progress on stderr: if the implementation crashes, the evaluator
    // reports the check that was running.
    std::fprintf(stderr, "nanoeval_conformance: running %s\n", name.c_str());
    std::fflush(stderr);
    g_checks.push_back(Check{name, category});
    return g_checks.back();
}

// Runs a check body, turning an unexpected exception into a failure.
template <class Body>
void run_check(const std::string& name, const std::string& category, Body&& body) {
    Check& check = begin(name, category);
    try {
        body(check);
    } catch (const std::exception& error) {
        check.expect(false, std::string("unexpected exception: ") + error.what());
    }
}

std::string describe(std::size_t n, std::size_t alignment, std::size_t usable) {
    std::ostringstream out;
    out << "size " << n << " alignment " << alignment << ": usable_size " << usable;
    return out.str();
}

// ---- api: the baseline's documented contract -----------------------------------

void check_construction(Check& check) {
    bool threw = false;
    try {
        nanoalloc::Pool pool(config_for(0));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check.expect(threw, "capacity 0 must throw std::invalid_argument");
    nanoalloc::Pool pool(config_for(1000));
    check.expect(pool.capacity() == 1008, "capacity 1000 must be rounded up to 1008, got " +
                                              std::to_string(pool.capacity()));
    check.expect(pool.stats().capacity_bytes == pool.capacity(),
                 "stats().capacity_bytes must equal capacity()");
}

void check_zero_size(Check& check) {
    nanoalloc::Pool pool(config_for(1 * kMiB));
    std::vector<void*> ptrs;
    for (int i = 0; i < 64; ++i) {
        void* ptr = pool.allocate(0);
        check.expect(ptr != nullptr, "allocate(0) must return a non-null pointer");
        check.expect(aligned(ptr, 16), "allocate(0) must be 16-byte aligned");
        check.expect(std::find(ptrs.begin(), ptrs.end(), ptr) == ptrs.end(),
                     "allocate(0) must return unique pointers");
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
    pool.deallocate(nullptr);
    check.expect(pool.usable_size(nullptr) == 0, "usable_size(nullptr) must be 0");
    check.expect(pool.stats().live_allocations == 0, "live_allocations must be 0 after release");
}

void check_alignments(Check& check) {
    nanoalloc::Pool pool(config_for(32 * kMiB));
    for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        for (std::size_t n : {std::size_t{1}, std::size_t{15}, std::size_t{100}, std::size_t{1000},
                              std::size_t{5000}, std::size_t{70000}}) {
            void* ptr = pool.allocate(n, alignment);
            const std::size_t usable = pool.usable_size(ptr);
            check.expect(aligned(ptr, std::max<std::size_t>(alignment, 16)),
                         describe(n, alignment, usable) + ": misaligned pointer");
            check.expect(usable >= n, describe(n, alignment, usable) + ": usable < requested");
            fill(ptr, usable, n * 7 + alignment);
            check.expect(intact(ptr, usable, n * 7 + alignment),
                         describe(n, alignment, usable) + ": usable range not writable");
            pool.deallocate(ptr);
        }
    }
}

void check_invalid_requests(Check& check) {
    nanoalloc::Pool pool(config_for(1 * kMiB));
    const std::uint64_t failed_before = pool.stats().failed_allocation_count;
    auto throws_invalid = [&](std::size_t alignment) {
        try {
            void* ptr = pool.allocate(64, alignment);
            pool.deallocate(ptr);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    auto throws_bad_alloc = [&](std::size_t bytes, std::size_t alignment) {
        try {
            void* ptr = pool.allocate(bytes, alignment);
            pool.deallocate(ptr);
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    };
    check.expect(throws_invalid(0), "allocate(64, 0) must throw std::invalid_argument");
    check.expect(throws_invalid(3), "allocate(64, 3) must throw std::invalid_argument");
    check.expect(pool.try_allocate(64, 3) == nullptr, "try_allocate(64, 3) must return nullptr");
    check.expect(throws_bad_alloc(64, 8192), "alignment 8192 must throw std::bad_alloc");
    check.expect(pool.try_allocate(64, 8192) == nullptr,
                 "try_allocate with alignment 8192 must return nullptr");
    check.expect(throws_bad_alloc(pool.capacity() + 1, 16),
                 "a request larger than the capacity must throw std::bad_alloc");
    check.expect(pool.try_allocate(pool.capacity() + 1) == nullptr,
                 "try_allocate larger than the capacity must return nullptr");
    const nanoalloc::PoolStats stats = pool.stats();
    check.expect(stats.live_allocations == 0, "failed requests must not leave allocations");
    check.expect(stats.failed_allocation_count >= failed_before + 4,
                 "failed allocations must be counted");
}

void check_owns(Check& check) {
    nanoalloc::Pool pool(config_for(1 * kMiB));
    int local = 0;
    check.expect(!pool.owns(&local), "owns() must be false for a stack address");
    std::vector<void*> ptrs;
    for (std::size_t n : {1, 100, 1000, 10000}) {
        ptrs.push_back(pool.allocate(n));
        check.expect(pool.owns(ptrs.back()), "owns() must be true for an allocation of size " +
                                                 std::to_string(n));
    }
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
}

void check_statistics_counts(Check& check) {
    nanoalloc::Pool pool(config_for(8 * kMiB));
    Rng rng(3);
    std::vector<void*> ptrs;
    for (int i = 0; i < 1000; ++i) {
        ptrs.push_back(pool.allocate(rng.log_size(4000)));
    }
    nanoalloc::PoolStats stats = pool.stats();
    check.expect(stats.live_allocations == 1000, "live_allocations must count live blocks");
    check.expect(stats.allocation_count == 1000, "allocation_count must count allocations");
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
    stats = pool.stats();
    check.expect(stats.live_allocations == 0, "live_allocations must be 0 after release");
    check.expect(stats.deallocation_count == 1000, "deallocation_count must count releases");
}

void check_full_recovery(Check& check) {
    nanoalloc::Pool pool(config_for(4 * kMiB));
    const nanoalloc::PoolStats initial = pool.stats();
    Rng rng(4);
    for (int round = 0; round < 3; ++round) {
        std::vector<void*> ptrs;
        for (;;) {
            void* ptr = pool.try_allocate(rng.log_size(rng.chance(5) ? 100000 : 2000));
            if (ptr == nullptr) {
                break;
            }
            ptrs.push_back(ptr);
        }
        for (std::size_t i = ptrs.size(); i > 1; --i) {  // release in random order
            std::swap(ptrs[i - 1], ptrs[rng.below(i)]);
        }
        for (void* ptr : ptrs) {
            pool.deallocate(ptr);
        }
        const nanoalloc::PoolStats stats = pool.stats();
        check.expect(stats.live_allocations == 0, "everything released");
        check.expect(stats.free_bytes == stats.capacity_bytes,
                     "free_bytes must equal capacity_bytes after releasing everything");
        check.expect(stats.largest_free_block == initial.largest_free_block,
                     "largest_free_block must return to its initial value");
    }
}

void check_adapters(Check& check) {
    nanoalloc::Pool pool(config_for(16 * kMiB));
    {
        nanoalloc::MemoryResource resource(pool);
        std::pmr::vector<std::uint64_t> values(&resource);
        for (std::uint64_t i = 0; i < 20000; ++i) {
            values.push_back(i * 3);
        }
        std::pmr::map<int, std::pmr::string> map(&resource);
        for (int i = 0; i < 2000; ++i) {
            map.emplace(i, std::pmr::string(std::to_string(i) + " a string long enough to allocate",
                                            &resource));
        }
        bool values_ok = true;
        for (std::uint64_t i = 0; i < values.size(); ++i) {
            values_ok = values_ok && values[i] == i * 3;
        }
        check.expect(values_ok, "std::pmr::vector contents");
        check.expect(map.size() == 2000 && map.at(1999).rfind("1999 ", 0) == 0,
                     "std::pmr::map contents");
        nanoalloc::MemoryResource other(pool);
        check.expect(resource.is_equal(other), "resources of the same pool compare equal");
    }
    {
        nanoalloc::Allocator<std::uint32_t> alloc(pool);
        std::vector<std::uint32_t, nanoalloc::Allocator<std::uint32_t>> vector(alloc);
        std::list<std::uint64_t, nanoalloc::Allocator<std::uint64_t>> list{
            nanoalloc::Allocator<std::uint64_t>(pool)};
        for (std::uint32_t i = 0; i < 10000; ++i) {
            vector.push_back(i);
            list.push_back(i);
        }
        check.expect(vector.size() == 10000 && vector[9999] == 9999 && list.back() == 9999,
                     "Allocator<T> with standard containers");
        check.expect(nanoalloc::Allocator<char>(pool) == nanoalloc::Allocator<int>(pool),
                     "allocators of the same pool compare equal");
    }
    check.expect(pool.stats().live_allocations == 0, "containers released their memory");
}

// ---- integrity: random use with every usable byte written ---------------------------

void check_random_integrity(Check& check) {
    nanoalloc::Pool pool(config_for(64 * kMiB));
    Rng rng(20260101);
    struct Live {
        void* ptr;
        std::size_t usable;
        std::uint64_t tag;
    };
    std::vector<Live> live;
    std::map<std::uintptr_t, std::uintptr_t> ranges;  // begin -> end of live usable ranges
    std::uint64_t next_tag = 1;

    auto add_range = [&](const void* ptr, std::size_t usable) {
        const auto begin_address = reinterpret_cast<std::uintptr_t>(ptr);
        const std::uintptr_t end_address = begin_address + std::max<std::size_t>(usable, 1);
        auto after = ranges.lower_bound(begin_address);
        bool overlap = after != ranges.end() && after->first < end_address;
        if (after != ranges.begin() && std::prev(after)->second > begin_address) {
            overlap = true;
        }
        ranges.emplace(begin_address, end_address);
        return !overlap;
    };
    auto verify_all = [&] {
        std::size_t corrupted = 0;
        for (const Live& block : live) {
            corrupted += intact(block.ptr, block.usable, block.tag) ? 0 : 1;
        }
        check.expect(corrupted == 0, std::to_string(corrupted) + " live blocks were corrupted");
        check.expect(pool.verify_integrity(), "verify_integrity() returned false");
    };

    for (int step = 0; step < 200000; ++step) {
        if (live.empty() || (live.size() < 6000 && rng.chance(55))) {
            std::size_t n = rng.log_size(16384);
            if (rng.chance(2)) {
                n = 16384 + rng.below(250000);
            }
            if (rng.chance(3)) {
                n = 0;
            }
            const std::size_t alignment = rng.chance(5) ? std::size_t{32} << rng.below(8) : 16;
            void* ptr = pool.try_allocate(n, alignment);
            if (ptr == nullptr) {
                continue;  // budget reached; release something next
            }
            const std::size_t usable = pool.usable_size(ptr);
            check.expect(usable >= n, describe(n, alignment, usable) + ": usable < requested");
            check.expect(aligned(ptr, alignment), describe(n, alignment, usable) + ": misaligned");
            check.expect(add_range(ptr, usable),
                         describe(n, alignment, usable) + ": overlaps a live allocation");
            fill(ptr, usable, next_tag);
            live.push_back({ptr, usable, next_tag++});
        } else {
            const std::size_t index = rng.below(live.size());
            const Live block = live[index];
            check.expect(intact(block.ptr, block.usable, block.tag),
                         "a block was corrupted before its release");
            ranges.erase(reinterpret_cast<std::uintptr_t>(block.ptr));
            pool.deallocate(block.ptr);
            live[index] = live.back();
            live.pop_back();
        }
        if (step % 25000 == 0) {
            verify_all();
        }
    }
    verify_all();
    for (const Live& block : live) {
        pool.deallocate(block.ptr);
    }
    check.expect(pool.stats().live_allocations == 0, "everything released");
    check.expect(pool.verify_integrity(), "verify_integrity() after releasing everything");
}

// ---- reuse, budget and actual consumption ---------------------------------------------

struct Consumption {
    std::size_t size;
    std::size_t capacity;
    std::size_t first_fill;
    std::size_t second_fill;
    bool contents_intact;
};

std::vector<Consumption> g_consumption;

void check_fill_release_refill(Check& reuse, Check& budget) {
    const std::size_t capacity = 4 * kMiB;
    for (std::size_t size : {std::size_t{1}, std::size_t{8}, std::size_t{16}, std::size_t{24},
                             std::size_t{32}, std::size_t{48}, std::size_t{64}, std::size_t{100},
                             std::size_t{128}, std::size_t{200}, std::size_t{256},
                             std::size_t{500}, std::size_t{1000}, std::size_t{2000},
                             std::size_t{4000}, std::size_t{10000}}) {
        nanoalloc::Pool pool(config_for(capacity));
        Rng rng(size);
        Consumption result{size, capacity, 0, 0, true};
        std::vector<void*> ptrs;
        for (int round = 0; round < 2; ++round) {
            for (;;) {
                void* ptr = pool.try_allocate(size);
                if (ptr == nullptr) {
                    break;
                }
                fill(ptr, size, ptrs.size() + 1);
                ptrs.push_back(ptr);
                if (ptrs.size() * round16(size) > 4 * capacity) {
                    break;  // far beyond the budget: stop counting
                }
            }
            for (std::size_t i = 0; i < ptrs.size(); ++i) {
                result.contents_intact = result.contents_intact && intact(ptrs[i], size, i + 1);
            }
            (round == 0 ? result.first_fill : result.second_fill) = ptrs.size();
            for (std::size_t i = ptrs.size(); i > 1; --i) {
                std::swap(ptrs[i - 1], ptrs[rng.below(i)]);
            }
            for (void* ptr : ptrs) {
                pool.deallocate(ptr);
            }
            ptrs.clear();
        }
        const std::string label = "size " + std::to_string(size) + ": ";
        reuse.expect(result.first_fill > 0, label + "nothing could be allocated");
        reuse.expect(result.second_fill >= result.first_fill,
                     label + "after releasing everything only " +
                         std::to_string(result.second_fill) + " of " +
                         std::to_string(result.first_fill) + " allocations fit again");
        reuse.expect(result.contents_intact, label + "contents corrupted while the pool was full");
        budget.expect(result.first_fill * std::max<std::size_t>(round16(size), 16) <= capacity,
                      label + std::to_string(result.first_fill) +
                          " allocations exceed the pool capacity of " + std::to_string(capacity) +
                          " bytes");
        g_consumption.push_back(result);
    }
}

// ---- packing: exact 16-byte rounding ------------------------------------------------------

void check_packing(Check& check, Check& with_alignment, std::size_t& max_slack) {
    nanoalloc::Pool pool(config_for(64 * kMiB));
    std::vector<std::size_t> sizes;
    for (std::size_t n = 1; n <= 4096; ++n) {
        sizes.push_back(n);
    }
    for (std::size_t n = 4097; n <= 65536; n += 61) {
        sizes.push_back(n);
    }
    for (std::size_t n : {std::size_t{100000}, std::size_t{262144}, std::size_t{500001},
                          std::size_t{1000000}, std::size_t{1048576}, std::size_t{2000000},
                          std::size_t{5000000}}) {
        sizes.push_back(n);
    }
    for (std::size_t n : sizes) {
        void* ptr = pool.allocate(n);
        const std::size_t usable = pool.usable_size(ptr);
        if (usable >= n) {
            max_slack = std::max(max_slack, usable - n);
        }
        check.expect(usable == round16(n),
                     describe(n, 16, usable) + " (expected " + std::to_string(round16(n)) + ")");
        fill(ptr, usable, n);
        check.expect(intact(ptr, usable, n), describe(n, 16, usable) + ": not writable");
        pool.deallocate(ptr);
    }
    for (std::size_t alignment = 32; alignment <= 4096; alignment *= 2) {
        for (std::size_t n : {std::size_t{1}, std::size_t{17}, std::size_t{100}, std::size_t{1000},
                              std::size_t{4000}, std::size_t{30000}}) {
            void* ptr = pool.allocate(n, alignment);
            const std::size_t usable = pool.usable_size(ptr);
            with_alignment.expect(usable == round16(n), describe(n, alignment, usable) +
                                                            " (expected " +
                                                            std::to_string(round16(n)) + ")");
            pool.deallocate(ptr);
        }
    }
}

std::string check_json(const Check& check) {
    std::ostringstream out;
    out << "{\"name\": " << nanoeval::json_string(check.name)
        << ", \"category\": " << nanoeval::json_string(check.category)
        << ", \"passed\": " << (check.passed ? "true" : "false") << ", \"cases\": " << check.cases
        << ", \"violations\": " << check.violations << ", \"examples\": [";
    for (std::size_t i = 0; i < check.examples.size(); ++i) {
        out << (i ? ", " : "") << nanoeval::json_string(check.examples[i]);
    }
    out << "]}";
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s (no options)\n", argv[0]);
        return 1;
    }
    run_check("construction", "api", check_construction);
    run_check("zero_size_requests", "api", check_zero_size);
    run_check("requested_alignments", "api", check_alignments);
    run_check("invalid_requests", "api", check_invalid_requests);
    run_check("owns", "api", check_owns);
    run_check("statistics_counts", "api", check_statistics_counts);
    run_check("full_recovery_after_release", "api", check_full_recovery);
    run_check("pmr_and_allocator_adapters", "api", check_adapters);
    run_check("random_use_full_usable_range", "integrity", check_random_integrity);
    {
        begin("fill_release_refill", "reuse");
        begin("payload_within_capacity", "budget");
        Check& reuse = g_checks[g_checks.size() - 2];
        Check& budget = g_checks[g_checks.size() - 1];
        try {
            check_fill_release_refill(reuse, budget);
        } catch (const std::exception& error) {
            reuse.expect(false, std::string("unexpected exception: ") + error.what());
        }
    }
    std::size_t max_slack = 0;
    {
        begin("exact_16_byte_rounding", "packing");
        begin("exact_16_byte_rounding_with_alignment", "packing");
        Check& plain = g_checks[g_checks.size() - 2];
        Check& with_alignment = g_checks[g_checks.size() - 1];
        try {
            check_packing(plain, with_alignment, max_slack);
        } catch (const std::exception& error) {
            plain.expect(false, std::string("unexpected exception: ") + error.what());
        }
    }

    std::ostringstream out;
    out << "{\"driver\": \"nanoeval_conformance\", \"compiler\": "
        << nanoeval::json_string(nanoeval::compiler_description()) << ", \"checks\": [";
    for (std::size_t i = 0; i < g_checks.size(); ++i) {
        out << (i ? ", " : "") << check_json(g_checks[i]);
    }
    out << "], \"max_payload_slack\": " << max_slack << ", \"consumption\": [";
    for (std::size_t i = 0; i < g_consumption.size(); ++i) {
        const Consumption& c = g_consumption[i];
        out << (i ? ", " : "") << "{\"size\": " << c.size << ", \"capacity\": " << c.capacity
            << ", \"allocations\": " << c.first_fill << ", \"allocations_after_refill\": "
            << c.second_fill << ", \"bytes_per_allocation\": "
            << (c.first_fill ? static_cast<double>(c.capacity) / static_cast<double>(c.first_fill) : 0.0)
            << "}";
    }
    out << "]}";
    std::printf("%s\n", out.str().c_str());
    return 0;
}
