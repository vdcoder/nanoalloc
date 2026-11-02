// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Internal: binary buddy allocation engine used by nanoalloc::Pool. Not part
// of the public API.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "diagnostics.hpp"

namespace nanoalloc::detail {

/// Binary buddy allocator over a caller-provided arena.
///
/// Layout
/// ------
/// The arena is divided into 16-byte granules. A block of order k spans 2^k
/// granules (16 << k bytes) and starts at a granule index that is a multiple
/// of 2^k, so block (k, i) covers granules [i << k, (i + 1) << k). Its buddy
/// is (k, i ^ 1), and the two together form the parent block (k + 1, i >> 1).
///
/// An arena whose granule count is not a power of two is treated as the
/// prefix of a virtual power-of-two tree of order top_order(). Nodes that lie
/// entirely beyond the arena are never free, so nothing ever merges with
/// them; nodes that straddle the end of the arena stay permanently split. The
/// arena is therefore a forest of power-of-two root blocks, one per set bit
/// of the granule count.
///
/// Metadata (held outside the arena, allocated once at construction)
/// -------------------------------------------------------------------
/// * Free bitmap: one bit per node at every order, set iff node (k, i) is a
///   free block on free list k.
/// * Split bitmap: one bit per node at orders >= 1, set iff the node is
///   currently divided into two children, i.e. it is a strict ancestor of a
///   block. Blocks themselves and nodes inside a block have the bit clear.
/// * One doubly linked free list per order. List nodes are stored inside the
///   free blocks (two pointers fit in the 16-byte minimum block), so lists
///   cost no extra memory and any block can be unlinked in O(1). The `prev`
///   link of the head node is not maintained (heads_[k] identifies the head),
///   so popping the head never has to touch the next node's memory.
///
/// The bitmaps cost about 3 bits per 16-byte granule (~2.3% of the arena).
/// Allocated blocks carry no header: the order of an allocated block is
/// recovered from the split bitmap by walking up from order 0 until a split
/// parent is found, which costs one bit test per level (O(order)).
///
/// The engine is single-threaded and performs no system allocation outside
/// its constructor.
class BuddyEngine {
public:
    static constexpr unsigned kGranuleShift = 4;
    static constexpr std::size_t kGranule = std::size_t{1} << kGranuleShift;
    /// Size of the per-order tables. Orders range over [0, top_order()],
    /// and top_order() < kOrderSlots for every supported capacity.
    static constexpr unsigned kOrderSlots = 64;

    /// `base` must be aligned to at least kGranule and `capacity_bytes` must
    /// be a non-zero multiple of kGranule no larger than 2^62. A block of order
    /// k is aligned to min(16 << k, alignment of base). Throws
    /// std::invalid_argument on violated preconditions and std::bad_alloc if
    /// the metadata cannot be allocated.
    BuddyEngine(std::byte* base, std::size_t capacity_bytes);
    ~BuddyEngine();

    BuddyEngine(const BuddyEngine&) = delete;
    BuddyEngine& operator=(const BuddyEngine&) = delete;
    BuddyEngine(BuddyEngine&&) = delete;
    BuddyEngine& operator=(BuddyEngine&&) = delete;

    /// Smallest order whose blocks hold `bytes` bytes. Defined for any
    /// `bytes` up to SIZE_MAX / 2; callers compare against max_block_size()
    /// first.
    [[nodiscard]] static unsigned order_for_size(std::size_t bytes) noexcept {
        if (bytes <= kGranule) {
            return 0;
        }
        return static_cast<unsigned>(std::bit_width(bytes - 1)) - kGranuleShift;
    }

    [[nodiscard]] static constexpr std::size_t block_size(unsigned order) noexcept {
        return kGranule << order;
    }

    /// Returns a block of exactly `order`, splitting the smallest larger free
    /// block if necessary, or nullptr if no free block of that order or
    /// larger exists.
    [[nodiscard]] void* allocate(unsigned order) noexcept;

    /// Releases a block returned by allocate() and coalesces it with free
    /// buddies as far as possible. Returns the order of the released block.
    /// Terminates the process on detected misuse (pointer outside the arena,
    /// pointer into the middle of a block, double free).
    unsigned deallocate(void* ptr) noexcept;

    /// Order of the allocated block at `ptr`. Same validation as deallocate().
    [[nodiscard]] unsigned order_of(const void* ptr) const noexcept;

    [[nodiscard]] bool owns(const void* ptr) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        const auto base = reinterpret_cast<std::uintptr_t>(base_);
        return address - base < capacity();  // wraps for address < base
    }

    [[nodiscard]] std::byte* base() const noexcept { return base_; }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return granules_ << kGranuleShift;
    }

    /// Granule index of an address inside the arena.
    [[nodiscard]] std::size_t granule_of(const void* ptr) const noexcept {
        return static_cast<std::size_t>(static_cast<const std::byte*>(ptr) - base_) >>
               kGranuleShift;
    }

    /// Order of the (possibly virtual) root that covers the whole arena.
    [[nodiscard]] unsigned top_order() const noexcept { return top_order_; }

    /// Largest block the arena can ever provide (its largest root block).
    [[nodiscard]] std::size_t max_block_size() const noexcept {
        return block_size(largest_root_order_);
    }

    /// Size of the largest block currently on a free list (0 if none).
    [[nodiscard]] std::size_t largest_free_block() const noexcept {
        if (nonempty_ == 0) {
            return 0;
        }
        return block_size(static_cast<unsigned>(std::bit_width(nonempty_)) - 1);
    }

    [[nodiscard]] std::size_t free_bytes() const noexcept {
        return free_granules_ << kGranuleShift;
    }

    /// Heap bytes used by the bitmaps (the only engine metadata outside the
    /// engine object itself).
    [[nodiscard]] std::size_t metadata_bytes() const noexcept {
        return bitmap_words_ * sizeof(std::uint64_t);
    }

    [[nodiscard]] std::uint64_t split_count() const noexcept { return splits_; }
    [[nodiscard]] std::uint64_t merge_count() const noexcept { return merges_; }

    // --- Introspection for tests and debugging ------------------------------

    /// Number of blocks on free list `order` (walks the list).
    [[nodiscard]] std::size_t free_block_count(unsigned order) const noexcept;

    /// Whether node (order, index) is a free block.
    [[nodiscard]] bool is_free(unsigned order, std::size_t index) const noexcept;

    /// Whether node (order, index) is split. Always false for order 0.
    [[nodiscard]] bool is_split(unsigned order, std::size_t index) const noexcept;

    /// Full consistency check of free lists, bitmaps, counters and block
    /// structure. Returns nullptr if consistent, otherwise a description of
    /// the first problem found. Runs in O(capacity / 16).
    [[nodiscard]] const char* validate() const noexcept;

private:
    struct FreeNode {
        FreeNode* prev;
        FreeNode* next;
    };
    static_assert(sizeof(FreeNode) <= kGranule,
                  "a free-list node must fit in the minimum block");

    // ---- bitmap primitives -------------------------------------------------
    [[nodiscard]] bool test_bit(std::size_t map, std::size_t index) const noexcept {
        return ((bits_[map + (index >> 6)] >> (index & 63U)) & 1U) != 0;
    }
    void set_bit(std::size_t map, std::size_t index) noexcept {
        bits_[map + (index >> 6)] |= std::uint64_t{1} << (index & 63U);
    }
    void clear_bit(std::size_t map, std::size_t index) noexcept {
        bits_[map + (index >> 6)] &= ~(std::uint64_t{1} << (index & 63U));
    }

    /// Number of nodes of `order` that contain at least one arena granule.
    [[nodiscard]] std::size_t node_count(unsigned order) const noexcept {
        return ((granules_ - 1) >> order) + 1;
    }
    /// Words per bitmap of `order`. One spare bit is always available so the
    /// buddy (index ^ 1) of the last node can be tested without a bounds check.
    [[nodiscard]] std::size_t map_words(unsigned order) const noexcept {
        return (node_count(order) >> 6) + 1;
    }

    [[nodiscard]] std::byte* block_address(unsigned order, std::size_t index) const noexcept {
        return base_ + ((index << order) << kGranuleShift);
    }
    [[nodiscard]] static FreeNode* node_at(std::byte* address) noexcept {
        return std::launder(reinterpret_cast<FreeNode*>(address));
    }

    // ---- free-list primitives ----------------------------------------------
    // Under AddressSanitizer free blocks are poisoned; these helpers unpoison a
    // list node only for the duration of each access.
    static FreeNode* load_next(const FreeNode* node) noexcept;
    static FreeNode* load_prev(const FreeNode* node) noexcept;
    static void store_next(FreeNode* node, FreeNode* value) noexcept;
    static void store_prev(FreeNode* node, FreeNode* value) noexcept;

    void push_free(unsigned order, std::size_t index) noexcept;
    FreeNode* pop_free(unsigned order) noexcept;
    void unlink_free(unsigned order, FreeNode* node) noexcept;

    // ---- pointer validation ------------------------------------------------
    [[nodiscard]] std::size_t checked_granule(const void* ptr) const noexcept;
    [[nodiscard]] unsigned find_block_order(std::size_t granule,
                                            const void* ptr) const noexcept;

    // ---- consistency checking ----------------------------------------------
    [[nodiscard]] bool range_is_clear(std::size_t map, std::size_t begin,
                                      std::size_t end) const noexcept;
    [[nodiscard]] const char* validate_subtree(unsigned order, std::size_t index,
                                               std::size_t& free_granules) const noexcept;

    std::byte* base_;
    std::size_t granules_;
    unsigned top_order_ = 0;
    unsigned largest_root_order_ = 0;
    std::uint64_t nonempty_ = 0;  // bit k set iff free list k is non-empty
    std::size_t free_granules_ = 0;
    std::uint64_t splits_ = 0;
    std::uint64_t merges_ = 0;
    FreeNode* heads_[kOrderSlots] = {};
    std::size_t free_map_[kOrderSlots] = {};   // word offset of each free bitmap
    std::size_t split_map_[kOrderSlots] = {};  // word offset of each split bitmap
    std::size_t bitmap_words_ = 0;
    std::unique_ptr<std::uint64_t[]> bits_;
};

// ===========================================================================
// Hot paths, defined inline so that the Pool can inline them.
// ===========================================================================

inline BuddyEngine::FreeNode* BuddyEngine::load_next(const FreeNode* node) noexcept {
    unpoison_memory(node, sizeof(FreeNode));
    FreeNode* value = node->next;
    poison_memory(node, sizeof(FreeNode));
    return value;
}

inline BuddyEngine::FreeNode* BuddyEngine::load_prev(const FreeNode* node) noexcept {
    unpoison_memory(node, sizeof(FreeNode));
    FreeNode* value = node->prev;
    poison_memory(node, sizeof(FreeNode));
    return value;
}

inline void BuddyEngine::store_next(FreeNode* node, FreeNode* value) noexcept {
    unpoison_memory(node, sizeof(FreeNode));
    node->next = value;
    poison_memory(node, sizeof(FreeNode));
}

inline void BuddyEngine::store_prev(FreeNode* node, FreeNode* value) noexcept {
    unpoison_memory(node, sizeof(FreeNode));
    node->prev = value;
    poison_memory(node, sizeof(FreeNode));
}

inline void BuddyEngine::push_free(unsigned order, std::size_t index) noexcept {
    std::byte* address = block_address(order, index);
    FreeNode* head = heads_[order];
    unpoison_memory(address, sizeof(FreeNode));
    FreeNode* node = ::new (static_cast<void*>(address)) FreeNode{nullptr, head};
    poison_memory(address, sizeof(FreeNode));
    if (head != nullptr) {
        store_prev(head, node);
    }
    heads_[order] = node;
    nonempty_ |= std::uint64_t{1} << order;
    set_bit(free_map_[order], index);
}

inline BuddyEngine::FreeNode* BuddyEngine::pop_free(unsigned order) noexcept {
    FreeNode* node = heads_[order];
    FreeNode* next = load_next(node);
    heads_[order] = next;  // next->prev becomes stale; heads never use it
    if (next == nullptr) {
        nonempty_ &= ~(std::uint64_t{1} << order);
    }
    return node;
}

inline void BuddyEngine::unlink_free(unsigned order, FreeNode* node) noexcept {
    FreeNode* next = load_next(node);
    if (heads_[order] == node) {
        heads_[order] = next;
        if (next == nullptr) {
            nonempty_ &= ~(std::uint64_t{1} << order);
        }
        return;
    }
    // Not the head, so `prev` is valid; `next`, if any, is not the head either.
    FreeNode* prev = load_prev(node);
    store_next(prev, next);
    if (next != nullptr) {
        store_prev(next, prev);
    }
}

inline std::size_t BuddyEngine::checked_granule(const void* ptr) const noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto base = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t offset = address - base;  // wraps for address < base
    if (offset >= capacity() || (offset & (kGranule - 1)) != 0) [[unlikely]] {
        fatal_error("pointer was not allocated by this pool", ptr);
    }
    return static_cast<std::size_t>(offset >> kGranuleShift);
}

inline unsigned BuddyEngine::find_block_order(std::size_t granule,
                                              const void* ptr) const noexcept {
    // Walk up from order 0. Nodes inside a block have a clear split bit, so the
    // first level whose parent is split is the block's order. A valid block
    // start is aligned to its order, so meeting a set granule bit before
    // finding a split parent means `ptr` points into the middle of a block.
    unsigned order = 0;
    while (order < top_order_) {
        if (test_bit(split_map_[order + 1], granule >> (order + 1))) {
            return order;
        }
        if (((granule >> order) & 1U) != 0) [[unlikely]] {
            fatal_error("pointer does not refer to the start of an allocated block",
                        ptr);
        }
        ++order;
    }
    return order;  // the whole arena is a single block
}

inline void* BuddyEngine::allocate(unsigned order) noexcept {
    if (order > top_order_) [[unlikely]] {
        return nullptr;
    }
    const std::uint64_t candidates = nonempty_ >> order;
    if (candidates == 0) [[unlikely]] {
        return nullptr;
    }
    unsigned current = order + static_cast<unsigned>(std::countr_zero(candidates));
    FreeNode* node = pop_free(current);
    std::size_t index = granule_of(node) >> current;
    clear_bit(free_map_[current], index);
    // Split down to the requested order, keeping the lower half each time and
    // putting the upper half (its buddy) on the next free list.
    while (current > order) {
        set_bit(split_map_[current], index);
        --current;
        index <<= 1;
        push_free(current, index + 1);
        ++splits_;
    }
    free_granules_ -= std::size_t{1} << order;
    unpoison_memory(node, block_size(order));
    return node;
}

inline unsigned BuddyEngine::deallocate(void* ptr) noexcept {
    const std::size_t granule = checked_granule(ptr);
    const unsigned order = find_block_order(granule, ptr);
    std::size_t index = granule >> order;
    if (test_bit(free_map_[order], index)) [[unlikely]] {
        fatal_error("double free (block is already free)", ptr);
    }
    poison_memory(ptr, block_size(order));
    free_granules_ += std::size_t{1} << order;
    // Coalesce with free buddies as far as possible.
    unsigned current = order;
    while (current < top_order_) {
        const std::size_t buddy = index ^ 1U;
        if (!test_bit(free_map_[current], buddy)) {
            break;
        }
        clear_bit(free_map_[current], buddy);
        unlink_free(current, node_at(block_address(current, buddy)));
        index >>= 1;
        ++current;
        clear_bit(split_map_[current], index);
        ++merges_;
    }
    push_free(current, index);
    return order;
}

inline unsigned BuddyEngine::order_of(const void* ptr) const noexcept {
    const std::size_t granule = checked_granule(ptr);
    const unsigned order = find_block_order(granule, ptr);
    if (test_bit(free_map_[order], granule >> order)) [[unlikely]] {
        fatal_error("pointer refers to a block that is not allocated", ptr);
    }
    return order;
}

}  // namespace nanoalloc::detail
