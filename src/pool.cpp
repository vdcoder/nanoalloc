// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "nanoalloc/pool.hpp"

#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#include "buddy_engine.hpp"

namespace nanoalloc {

namespace {

using detail::BuddyEngine;

static_assert(Pool::granularity == BuddyEngine::kGranule);
static_assert(Pool::min_alignment == BuddyEngine::kGranule);
static_assert((Pool::max_alignment & (Pool::max_alignment - 1)) == 0);

/// RAII owner of the arena memory. The arena is aligned to
/// Pool::max_alignment so that any block of at least that size is suitably
/// aligned for every supported alignment.
class Arena {
public:
    explicit Arena(std::size_t bytes)
        : data_(static_cast<std::byte*>(
              ::operator new(bytes, std::align_val_t{Pool::max_alignment}))) {}

    ~Arena() { ::operator delete(data_, std::align_val_t{Pool::max_alignment}); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    [[nodiscard]] std::byte* data() const noexcept { return data_; }

private:
    std::byte* data_;
};

/// Optional side table recording the requested size of each live allocation,
/// so that requested-byte statistics stay exact even though deallocate() is
/// not given a size.
///
/// The table has one byte per arena granule. Allocations start on distinct
/// granules and an allocation of `usable` bytes owns usable / 16 consecutive
/// entries, which always suffice to encode its requested size (<= usable):
///   usable  <  32: 1 entry, 1-byte value
///   usable  < 128: >= 2 entries, 2-byte value
///   otherwise:     >= 8 entries, 8-byte value
/// The table is not initialized: an entry is always written by allocate()
/// before deallocate() reads it, so untouched pages are never committed.
class RequestedSizeTable {
public:
    explicit RequestedSizeTable(std::size_t granules)
        : entries_(granules == 0 ? nullptr : new std::uint8_t[granules]), size_(granules) {}

    [[nodiscard]] bool enabled() const noexcept { return entries_ != nullptr; }
    [[nodiscard]] std::size_t bytes() const noexcept { return size_; }

    void store(std::size_t granule, std::size_t usable, std::size_t requested) noexcept {
        std::uint8_t* entry = entries_.get() + granule;
        if (usable < 2 * Pool::granularity) {
            *entry = static_cast<std::uint8_t>(requested);
        } else if (usable < 8 * Pool::granularity) {
            const auto value = static_cast<std::uint16_t>(requested);
            std::memcpy(entry, &value, sizeof(value));
        } else {
            const auto value = static_cast<std::uint64_t>(requested);
            std::memcpy(entry, &value, sizeof(value));
        }
    }

    [[nodiscard]] std::size_t load(std::size_t granule, std::size_t usable) const noexcept {
        const std::uint8_t* entry = entries_.get() + granule;
        if (usable < 2 * Pool::granularity) {
            return *entry;
        }
        if (usable < 8 * Pool::granularity) {
            std::uint16_t value = 0;
            std::memcpy(&value, entry, sizeof(value));
            return value;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, entry, sizeof(value));
        return static_cast<std::size_t>(value);
    }

private:
    std::unique_ptr<std::uint8_t[]> entries_;
    std::size_t size_;
};

constexpr std::size_t round_up_to_granularity(std::size_t bytes) noexcept {
    return (bytes + Pool::granularity - 1) & ~(Pool::granularity - 1);
}

/// True for non-zero powers of two not exceeding Pool::max_alignment.
constexpr bool is_supported_alignment(std::size_t alignment) noexcept {
    return alignment - 1 < Pool::max_alignment && (alignment & (alignment - 1)) == 0;
}

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

struct Pool::Impl {
    explicit Impl(const PoolConfig& config)
        : arena(round_up_to_granularity(config.capacity_bytes)),
          engine(arena.data(), round_up_to_granularity(config.capacity_bytes)),
          requested(config.track_requested_bytes ? engine.capacity() / granularity : 0),
          max_request(engine.max_block_size()) {}

    void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
        if (!is_supported_alignment(alignment)) [[unlikely]] {
            ++failed_allocations;
            return nullptr;
        }
        // A block at least as large as the alignment is aligned to it, because
        // blocks are naturally aligned within an arena aligned to max_alignment.
        const std::size_t need = bytes > alignment ? bytes : alignment;
        if (need > max_request) [[unlikely]] {
            ++failed_allocations;
            return nullptr;
        }
        const unsigned order = BuddyEngine::order_for_size(need);
        void* ptr = engine.allocate(order);
        if (ptr == nullptr) [[unlikely]] {
            ++failed_allocations;
            return nullptr;
        }
        const std::size_t usable = BuddyEngine::block_size(order);
        ++live_allocations;
        ++allocations;
        live_usable += usable;
        if (live_usable > peak_usable) {
            peak_usable = live_usable;
        }
        if (requested.enabled()) {
            requested.store(engine.granule_of(ptr), usable, bytes);
            live_requested += bytes;
            if (live_requested > peak_requested) {
                peak_requested = live_requested;
            }
        }
        return ptr;
    }

    void deallocate(void* ptr) noexcept {
        const unsigned order = engine.deallocate(ptr);  // validates ptr
        const std::size_t usable = BuddyEngine::block_size(order);
        --live_allocations;
        ++deallocations;
        live_usable -= usable;
        if (requested.enabled()) {
            live_requested -= requested.load(engine.granule_of(ptr), usable);
        }
    }

    Arena arena;
    BuddyEngine engine;
    RequestedSizeTable requested;
    std::size_t max_request;

    std::size_t live_allocations = 0;
    std::size_t live_requested = 0;
    std::size_t live_usable = 0;
    std::size_t peak_requested = 0;
    std::size_t peak_usable = 0;
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t failed_allocations = 0;
};

Pool::Pool(const PoolConfig& config) : impl_(nullptr) {
    if (config.capacity_bytes == 0) {
        throw std::invalid_argument("nanoalloc::Pool: capacity_bytes must be non-zero");
    }
    if (config.capacity_bytes > max_capacity) {
        throw std::invalid_argument(
            "nanoalloc::Pool: capacity_bytes exceeds Pool::max_capacity");
    }
    impl_ = new Impl(config);
}

Pool::~Pool() { delete impl_; }

void* Pool::allocate(std::size_t bytes, std::size_t alignment) {
    void* ptr = impl_->allocate(bytes, alignment);
    if (ptr == nullptr) [[unlikely]] {
        if (!is_power_of_two(alignment)) {
            throw std::invalid_argument(
                "nanoalloc::Pool::allocate: alignment must be a power of two");
        }
        throw std::bad_alloc();
    }
    return ptr;
}

void* Pool::try_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    return impl_->allocate(bytes, alignment);
}

void Pool::deallocate(void* ptr) noexcept {
    if (ptr != nullptr) {
        impl_->deallocate(ptr);
    }
}

std::size_t Pool::usable_size(const void* ptr) const noexcept {
    if (ptr == nullptr) {
        return 0;
    }
    return BuddyEngine::block_size(impl_->engine.order_of(ptr));
}

bool Pool::owns(const void* ptr) const noexcept { return impl_->engine.owns(ptr); }

std::size_t Pool::capacity() const noexcept { return impl_->engine.capacity(); }

PoolStats Pool::stats() const noexcept {
    const Impl& impl = *impl_;
    PoolStats stats;
    stats.capacity_bytes = impl.engine.capacity();
    stats.metadata_bytes = sizeof(Impl) + impl.engine.metadata_bytes();
    stats.tracking_bytes = impl.requested.bytes();

    stats.live_allocations = impl.live_allocations;
    stats.live_requested_bytes = impl.live_requested;
    stats.live_usable_bytes = impl.live_usable;
    // Buddy blocks carry no in-arena header: a block consumes exactly its
    // usable size.
    stats.live_consumed_bytes = impl.live_usable;

    stats.peak_requested_bytes = impl.peak_requested;
    stats.peak_usable_bytes = impl.peak_usable;
    stats.peak_consumed_bytes = impl.peak_usable;

    stats.free_bytes = stats.capacity_bytes - stats.live_consumed_bytes;
    stats.largest_free_block = impl.engine.largest_free_block();

    stats.allocation_count = impl.allocations;
    stats.deallocation_count = impl.deallocations;
    stats.failed_allocation_count = impl.failed_allocations;
    stats.split_count = impl.engine.split_count();
    stats.merge_count = impl.engine.merge_count();
    stats.requested_bytes_tracked = impl.requested.enabled();
    return stats;
}

bool Pool::verify_integrity() const noexcept {
    const Impl& impl = *impl_;
    if (impl.engine.validate() != nullptr) {
        return false;
    }
    if (impl.engine.free_bytes() != impl.engine.capacity() - impl.live_usable) {
        return false;
    }
    if (impl.live_allocations == 0 && (impl.live_usable != 0 || impl.live_requested != 0)) {
        return false;
    }
    return impl.live_requested <= impl.live_usable && impl.peak_usable >= impl.live_usable &&
           impl.peak_requested >= impl.live_requested;
}

}  // namespace nanoalloc
