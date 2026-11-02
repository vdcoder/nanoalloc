// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "buddy_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace nanoalloc::detail {

BuddyEngine::BuddyEngine(std::byte* base, std::size_t capacity_bytes)
    : base_(base), granules_(capacity_bytes >> kGranuleShift) {
    if (base == nullptr || (reinterpret_cast<std::uintptr_t>(base) & (kGranule - 1)) != 0) {
        throw std::invalid_argument("BuddyEngine: arena must be 16-byte aligned");
    }
    if (capacity_bytes == 0 || (capacity_bytes & (kGranule - 1)) != 0 ||
        capacity_bytes > (std::size_t{1} << (sizeof(std::size_t) * 8 - 2))) {
        throw std::invalid_argument(
            "BuddyEngine: capacity must be a non-zero multiple of 16 bytes");
    }

    top_order_ = granules_ == 1 ? 0U : static_cast<unsigned>(std::bit_width(granules_ - 1));
    largest_root_order_ = static_cast<unsigned>(std::bit_width(granules_)) - 1;

    // Lay out every bitmap in one array: for each order, its free bitmap
    // followed (for orders >= 1) by its split bitmap.
    std::size_t words = 0;
    for (unsigned order = 0; order <= top_order_; ++order) {
        free_map_[order] = words;
        words += map_words(order);
        if (order >= 1) {
            split_map_[order] = words;
            words += map_words(order);
        }
    }
    bits_ = std::make_unique<std::uint64_t[]>(words);  // zero-initialized
    bitmap_words_ = words;

    // Everything is free; under ASan the whole arena starts poisoned.
    poison_memory(base_, capacity());

    // Decompose the arena into maximal power-of-two root blocks, largest
    // first, one per set bit of the granule count. Every strict ancestor of a
    // root in the virtual tree straddles the end of the arena and is split.
    std::size_t offset = 0;
    for (unsigned order = largest_root_order_ + 1; order-- > 0;) {
        if ((granules_ >> order) & 1U) {
            for (unsigned ancestor = order + 1; ancestor <= top_order_; ++ancestor) {
                set_bit(split_map_[ancestor], offset >> ancestor);
            }
            push_free(order, offset >> order);
            offset += std::size_t{1} << order;
        }
    }
    free_granules_ = granules_;
}

BuddyEngine::~BuddyEngine() {
    // Hand the arena back to its owner in a clean (unpoisoned) state.
    unpoison_memory(base_, capacity());
}

std::size_t BuddyEngine::free_block_count(unsigned order) const noexcept {
    if (order >= kOrderSlots) {
        return 0;
    }
    std::size_t count = 0;
    for (const FreeNode* node = heads_[order]; node != nullptr; node = load_next(node)) {
        ++count;
    }
    return count;
}

bool BuddyEngine::is_free(unsigned order, std::size_t index) const noexcept {
    return order <= top_order_ && index <= node_count(order) &&
           test_bit(free_map_[order], index);
}

bool BuddyEngine::is_split(unsigned order, std::size_t index) const noexcept {
    return order >= 1 && order <= top_order_ && index <= node_count(order) &&
           test_bit(split_map_[order], index);
}

bool BuddyEngine::range_is_clear(std::size_t map, std::size_t begin,
                                 std::size_t end) const noexcept {
    std::size_t index = begin;
    while (index < end) {
        const unsigned bit = static_cast<unsigned>(index & 63U);
        const std::size_t span = std::min<std::size_t>(64U - bit, end - index);
        const std::uint64_t mask =
            (span == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << span) - 1)) << bit;
        if ((bits_[map + (index >> 6)] & mask) != 0) {
            return false;
        }
        index += span;
    }
    return true;
}

const char* BuddyEngine::validate_subtree(unsigned order, std::size_t index,
                                          std::size_t& free_granules) const noexcept {
    const std::size_t first = index << order;
    const bool is_free_node = test_bit(free_map_[order], index);
    const bool is_split_node = order >= 1 && test_bit(split_map_[order], index);

    if (first >= granules_) {
        // Entirely beyond the arena: must never carry state.
        if (is_free_node) {
            return "a node beyond the end of the arena is marked free";
        }
        if (is_split_node) {
            return "a node beyond the end of the arena is marked split";
        }
        return nullptr;
    }

    if (is_split_node) {
        if (is_free_node) {
            return "a node is marked both split and free";
        }
        const std::size_t left = index << 1;
        if (const char* error = validate_subtree(order - 1, left, free_granules)) {
            return error;
        }
        if (const char* error = validate_subtree(order - 1, left + 1, free_granules)) {
            return error;
        }
        if (test_bit(free_map_[order - 1], left) && test_bit(free_map_[order - 1], left + 1)) {
            return "two free buddies were not coalesced";
        }
        return nullptr;
    }

    if (first + (std::size_t{1} << order) > granules_) {
        return "a node straddling the end of the arena is not split";
    }

    // A whole block (free or allocated): nothing below it may carry state.
    for (unsigned below = 0; below < order; ++below) {
        const unsigned shift = order - below;
        const std::size_t begin = index << shift;
        const std::size_t end = (index + 1) << shift;
        if (!range_is_clear(free_map_[below], begin, end)) {
            return "a node inside a block is marked free";
        }
        if (below >= 1 && !range_is_clear(split_map_[below], begin, end)) {
            return "a node inside a block is marked split";
        }
    }
    if (is_free_node) {
        free_granules += std::size_t{1} << order;
    }
    return nullptr;
}

const char* BuddyEngine::validate() const noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(base_);
    std::size_t listed_granules = 0;

    for (unsigned order = 0; order < kOrderSlots; ++order) {
        const bool marked_nonempty = ((nonempty_ >> order) & 1U) != 0;
        if (order > top_order_) {
            if (heads_[order] != nullptr || marked_nonempty) {
                return "a free list above the top order is not empty";
            }
            continue;
        }
        if ((heads_[order] != nullptr) != marked_nonempty) {
            return "the non-empty mask disagrees with a free list";
        }

        std::size_t count = 0;
        const FreeNode* expected_prev = nullptr;
        for (const FreeNode* node = heads_[order]; node != nullptr;
             node = load_next(node)) {
            if (!owns(node)) {
                return "a free-list node lies outside the arena";
            }
            const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(node) - base;
            if ((offset & (kGranule - 1)) != 0) {
                return "a free-list node is not granule aligned";
            }
            const std::size_t granule = static_cast<std::size_t>(offset >> kGranuleShift);
            if ((granule & ((std::size_t{1} << order) - 1)) != 0) {
                return "a free block is not aligned to its order";
            }
            if (granule + (std::size_t{1} << order) > granules_) {
                return "a free block extends past the end of the arena";
            }
            if (!test_bit(free_map_[order], granule >> order)) {
                return "a listed free block is not marked free";
            }
            // The head's back link is intentionally not maintained.
            if (expected_prev != nullptr && load_prev(node) != expected_prev) {
                return "a free list has a broken back link";
            }
            if (++count > node_count(order)) {
                return "a free list is cyclic";
            }
            expected_prev = node;
            listed_granules += std::size_t{1} << order;
        }

        std::size_t marked = 0;
        for (std::size_t word = 0; word < map_words(order); ++word) {
            marked += static_cast<std::size_t>(std::popcount(bits_[free_map_[order] + word]));
        }
        if (marked != count) {
            return "the free bitmap population differs from the free list length";
        }
    }

    if (listed_granules != free_granules_) {
        return "the free byte counter disagrees with the free lists";
    }

    std::size_t walked_free = 0;
    if (const char* error = validate_subtree(top_order_, 0, walked_free)) {
        return error;
    }
    if (walked_free != free_granules_) {
        return "the block structure disagrees with the free byte counter";
    }
    return nullptr;
}

}  // namespace nanoalloc::detail
