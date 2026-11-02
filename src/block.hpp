// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Internal: block header encoding, size classes and free-list links.
//
// Every allocation inside a span is a variable-sized block made of a 16-byte
// header followed by its payload. Block sizes are multiples of 16 and include
// the header. The header is two 64-bit words:
//
//   word0  bits 0-1   state: 0 EXTERNAL, 1 ALLOCATED, 2 PENDING_REMOTE_FREE,
//                     3 FREE
//          ALLOCATED / FREE: bits 6-63 are the owning ThreadHeap (64-aligned)
//          PENDING:          bits 4-63 link to the next block in the owner's
//                            remote-free inbox (headers are 16-aligned)
//          EXTERNAL:         bits 6-63 are the owning Pool::Impl, bits 2-5
//                            hold log2 of the allocation's alignment
//
//   word1  bits 0-3   payload slack: payload - requested bytes (0-15)
//          bits 4-23  block size in bytes (low 4 bits implied zero)
//          bits 28-47 previous physical block size (0 for the first block)
//          bits 48-63 magic value, used to reject pointers that do not refer
//                     to a block header
//
// The state lives in word0 because remote threads change it with a CAS while
// the owner may concurrently rewrite word1 (the previous-size field) of the
// same block when a neighbour is split or merged. Keeping the two in separate
// words means neither side ever needs to read-modify-write the other's word.
// All accesses to header words are atomic (relaxed unless stated otherwise),
// which compiles to plain loads and stores on x86-64 and ARM64.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "diagnostics.hpp"

namespace nanoalloc::detail {

inline constexpr std::size_t kGranule = 16;
inline constexpr std::size_t kHeaderSize = 16;
/// A free block needs room for its header and two list links.
inline constexpr std::size_t kMinFreeBlock = 32;

inline constexpr std::size_t kSpanSize = std::size_t{1} << 20;  // 1 MiB
/// Span header; the first block header follows it, so the first payload is
/// 64-byte aligned.
inline constexpr std::size_t kSpanHeaderSize = 48;
/// Span header plus the permanently allocated fence header at the span end.
inline constexpr std::size_t kSpanOverhead = kSpanHeaderSize + kHeaderSize;
inline constexpr std::size_t kMinSpanSize = kSpanOverhead + kMinFreeBlock;
/// Largest block (header included) a full-size span can hold.
inline constexpr std::size_t kMaxBlockSize = kSpanSize - kSpanOverhead;
/// Largest payload served from a span; larger requests are external.
inline constexpr std::size_t kMaxSpanPayload = kMaxBlockSize - kHeaderSize;

// ---- size classes -----------------------------------------------------------
// 256 exact buckets of 16 bytes for block sizes 16 B - 4 KiB, then 16 buckets
// per power of two up to 1 MiB: 384 buckets in total.
inline constexpr unsigned kExactBuckets = 256;
inline constexpr std::size_t kExactLimit = kExactBuckets * kGranule;  // 4096
inline constexpr unsigned kBucketCount = 384;
inline constexpr unsigned kBitmapWords = kBucketCount / 64;
static_assert(kBitmapWords <= 64, "the summary word has one bit per bitmap word");

/// Bucket of a free block of `size` bytes (multiple of 16, 16 <= size <= 1 MiB).
[[nodiscard]] constexpr unsigned bucket_index(std::size_t size) noexcept {
    if (size <= kExactLimit) {
        return static_cast<unsigned>(size >> 4) - 1;
    }
    const auto k = static_cast<unsigned>(std::bit_width(size - 1)) - 1;  // 12..19
    return kExactBuckets + (k - 12) * 16 +
           static_cast<unsigned>((size - 1 - (std::size_t{1} << k)) >> (k - 4));
}

/// Smallest block size that maps to `bucket`.
[[nodiscard]] constexpr std::size_t bucket_min_size(unsigned bucket) noexcept {
    if (bucket < kExactBuckets) {
        return (std::size_t{bucket} + 1) * kGranule;
    }
    const unsigned group = (bucket - kExactBuckets) / 16;
    const unsigned sub = (bucket - kExactBuckets) % 16;
    const unsigned k = 12 + group;
    return (std::size_t{1} << k) + std::size_t{sub} * (std::size_t{1} << (k - 4)) + kGranule;
}

/// Largest block size that maps to `bucket`.
[[nodiscard]] constexpr std::size_t bucket_max_size(unsigned bucket) noexcept {
    return bucket + 1 < kBucketCount ? bucket_min_size(bucket + 1) - kGranule
                                     : std::size_t{1} << 20;
}

// ---- header encoding ----------------------------------------------------------
inline constexpr std::uint64_t kStateMask = 3;
inline constexpr std::uint64_t kStateExternal = 0;
inline constexpr std::uint64_t kStateAllocated = 1;
inline constexpr std::uint64_t kStatePending = 2;
inline constexpr std::uint64_t kStateFree = 3;
inline constexpr std::uint64_t kOwnerMask = ~std::uint64_t{63};
inline constexpr std::uint64_t kLinkMask = ~std::uint64_t{15};
inline constexpr unsigned kAlignShiftPos = 2;

inline constexpr std::uint64_t kSlackMask = 0xF;
inline constexpr std::uint64_t kSizeMask = 0x00FF'FFF0;
inline constexpr unsigned kPrevShift = 24;
inline constexpr std::uint64_t kPrevMask = kSizeMask << kPrevShift;
inline constexpr std::uint64_t kMagic = std::uint64_t{0x4E41} << 48;
inline constexpr std::uint64_t kMagicMask = std::uint64_t{0xFFFF} << 48;

struct Block {
    std::uint64_t word0;
    std::uint64_t word1;
};
static_assert(sizeof(Block) == kHeaderSize);

[[nodiscard]] inline std::uint64_t load_word(std::uint64_t& word) noexcept {
    return std::atomic_ref<std::uint64_t>(word).load(std::memory_order_relaxed);
}

inline void store_word(std::uint64_t& word, std::uint64_t value) noexcept {
    std::atomic_ref<std::uint64_t>(word).store(value, std::memory_order_relaxed);
}

[[nodiscard]] inline Block* block_at(std::uintptr_t address) noexcept {
    return reinterpret_cast<Block*>(address);
}

[[nodiscard]] inline std::uintptr_t address_of(const void* ptr) noexcept {
    return reinterpret_cast<std::uintptr_t>(ptr);
}

[[nodiscard]] inline Block* block_after(Block* block, std::size_t bytes) noexcept {
    return block_at(address_of(block) + bytes);
}

[[nodiscard]] inline Block* block_before(Block* block, std::size_t bytes) noexcept {
    return block_at(address_of(block) - bytes);
}

[[nodiscard]] inline void* payload_of(Block* block) noexcept { return block + 1; }

[[nodiscard]] inline Block* header_of(const void* payload) noexcept {
    return block_at(address_of(payload) - kHeaderSize);
}

[[nodiscard]] constexpr std::size_t size_field(std::uint64_t word1) noexcept {
    return static_cast<std::size_t>(word1 & kSizeMask);
}

[[nodiscard]] constexpr std::size_t prev_field(std::uint64_t word1) noexcept {
    return static_cast<std::size_t>((word1 >> kPrevShift) & kSizeMask);
}

[[nodiscard]] constexpr std::uint64_t make_word1(std::size_t size, std::size_t prev,
                                                 std::size_t slack) noexcept {
    return kMagic | (static_cast<std::uint64_t>(prev) << kPrevShift) |
           static_cast<std::uint64_t>(size) | static_cast<std::uint64_t>(slack);
}

// ---- free-list links ------------------------------------------------------------
// A free block of at least kMinFreeBlock bytes stores {prev, next} in the first
// 16 bytes of its payload. Only the owning thread touches them. Under
// AddressSanitizer free payloads are poisoned, so the links are unpoisoned for
// the duration of each access.

[[nodiscard]] inline Block* link_next(Block* block) noexcept {
    std::byte* slot = reinterpret_cast<std::byte*>(block + 1) + sizeof(Block*);
    unpoison_memory(slot, sizeof(Block*));
    Block* value = nullptr;
    std::memcpy(&value, slot, sizeof(value));
    poison_memory(slot, sizeof(Block*));
    return value;
}

[[nodiscard]] inline Block* link_prev(Block* block) noexcept {
    std::byte* slot = reinterpret_cast<std::byte*>(block + 1);
    unpoison_memory(slot, sizeof(Block*));
    Block* value = nullptr;
    std::memcpy(&value, slot, sizeof(value));
    poison_memory(slot, sizeof(Block*));
    return value;
}

inline void set_link_next(Block* block, Block* value) noexcept {
    std::byte* slot = reinterpret_cast<std::byte*>(block + 1) + sizeof(Block*);
    unpoison_memory(slot, sizeof(Block*));
    std::memcpy(slot, &value, sizeof(value));
    poison_memory(slot, sizeof(Block*));
}

inline void set_link_prev(Block* block, Block* value) noexcept {
    std::byte* slot = reinterpret_cast<std::byte*>(block + 1);
    unpoison_memory(slot, sizeof(Block*));
    std::memcpy(slot, &value, sizeof(value));
    poison_memory(slot, sizeof(Block*));
}

}  // namespace nanoalloc::detail
