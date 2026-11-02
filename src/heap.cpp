// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "heap.hpp"

#include <algorithm>

namespace nanoalloc::detail {

void* ThreadHeap::allocate_split(std::size_t requested, std::size_t payload) noexcept {
    drain_pending();
    const std::size_t need = payload + kHeaderSize;
    Fit fit = find_fit(need);
    if (fit.block == nullptr) [[unlikely]] {
        if (!grow(need)) {
            counters_.failed.add(1);
            return nullptr;
        }
        fit = find_fit(need);
    }
    // Carve the allocation from the end of the free block, so the remaining
    // free block keeps its header (and often its bucket).
    Block* free_block = fit.block;
    const std::uint64_t free_word1 = load_word(free_block->word1);
    const std::size_t size = size_field(free_word1);
    const std::size_t rest = size - need;
    if (rest == 0) {
        unlink_free(free_block, fit.bucket);
        return finish(free_block, need, prev_field(free_word1), requested, payload);
    }
    if (rest >= kMinFreeBlock) {
        // A remainder that stays within the bucket's bounds keeps its place.
        if (rest < kBucketBounds.min[fit.bucket]) {
            unlink_free(free_block, fit.bucket);
            push_free(free_block, bucket_index(rest));
        }
    } else {
        // A 16-byte remainder cannot hold list links. It stays a header-only
        // FREE block and is merged when a neighbour is released.
        unlink_free(free_block, fit.bucket);
        counters_.fragment_bytes.add(kHeaderSize);
    }
    store_word(free_block->word1, (free_word1 & ~kSizeMask) | rest);
    Block* block = block_after(free_block, rest);
    unpoison_memory(block, kHeaderSize);
    set_following_prev(block, need);
    counters_.splits.add(1);
    return finish(block, need, rest, requested, payload);
}

void* ThreadHeap::allocate_aligned(std::size_t requested, std::size_t payload,
                                   std::size_t alignment) noexcept {
    drain_pending();
    const std::size_t need = payload + kHeaderSize;
    // Enough room for the block at any alignment offset within the free block.
    const std::size_t search = need + alignment - kGranule;
    Fit fit = find_fit(search);
    if (fit.block == nullptr) {
        if (!grow(search)) {
            counters_.failed.add(1);
            return nullptr;
        }
        fit = find_fit(search);
    }

    Block* free_block = fit.block;
    const std::uintptr_t base = address_of(free_block);
    const std::uint64_t free_word1 = load_word(free_block->word1);
    const std::size_t size = size_field(free_word1);
    // Place the block as high as possible with its payload aligned: the part
    // before it stays free (lead) and so does the part after it (tail).
    const std::uintptr_t payload_address = (base + size - need + kHeaderSize) & ~(alignment - 1);
    Block* block = block_at(payload_address - kHeaderSize);
    const std::size_t lead = static_cast<std::size_t>(address_of(block) - base);
    const std::size_t tail = size - lead - need;

    std::size_t block_prev = 0;
    if (lead == 0) {
        unlink_free(free_block, fit.bucket);
        block_prev = prev_field(free_word1);
    } else {
        if (lead >= kMinFreeBlock) {
            const unsigned lead_bucket = bucket_index(lead);
            if (lead_bucket != fit.bucket) {
                unlink_free(free_block, fit.bucket);
                push_free(free_block, lead_bucket);
            }
        } else {
            unlink_free(free_block, fit.bucket);
            counters_.fragment_bytes.add(kHeaderSize);
        }
        store_word(free_block->word1, (free_word1 & ~kSizeMask) | lead);
        unpoison_memory(block, kHeaderSize);
        block_prev = lead;
        counters_.splits.add(1);
    }

    if (tail == 0) {
        set_following_prev(block, need);
    } else {
        Block* rest = block_after(block, need);
        unpoison_memory(rest, kHeaderSize);
        store_word(rest->word1, make_word1(tail, need, 0));
        store_word(rest->word0, owner_word() | kStateFree);
        set_following_prev(rest, tail);
        if (tail >= kMinFreeBlock) {
            push_free(rest, bucket_index(tail));
        } else {
            counters_.fragment_bytes.add(kHeaderSize);
        }
        counters_.splits.add(1);
    }
    return finish(block, need, block_prev, requested, payload);
}

bool ThreadHeap::grow(std::size_t need) noexcept {
    Span* span = provider_->acquire_span(need);
    if (span == nullptr) {
        return false;
    }
    add_span(span);
    return true;
}

void ThreadHeap::add_span(Span* span) noexcept {
    const std::size_t span_size = span->size;
    unpoison_memory(span, span_size);
    span->magic = kSpanMagic;
    span->owner = this;
    span->next = spans_;
    spans_ = span;

    const std::size_t block_size = span_size - kSpanOverhead;
    Block* first = block_at(address_of(span) + kSpanHeaderSize);
    Block* fence = block_after(first, block_size);
    store_word(first->word1, make_word1(block_size, 0, 0));
    store_word(first->word0, owner_word() | kStateFree);
    store_word(fence->word1, make_word1(0, block_size, 0));
    store_word(fence->word0, owner_word() | kStateAllocated);
    poison_memory(payload_of(first), block_size - kHeaderSize);
    push_free(first, bucket_index(block_size));

    counters_.span_count.add(1);
    counters_.span_bytes.add(span_size);
}

void ThreadHeap::drain_inbox() noexcept {
    std::uintptr_t node = inbox_.exchange(0, std::memory_order_acquire);
    std::uint64_t drained = 0;
    while (node != 0) {
        Block* block = block_at(node);
        const std::uint64_t word0 = load_word(block->word0);
        if ((word0 & kStateMask) != kStatePending) [[unlikely]] {
            fatal_error("corrupted remote-free inbox", payload_of(block));
        }
        node = static_cast<std::uintptr_t>(word0 & kLinkMask);
        const std::uint64_t word1 = load_word(block->word1);
        account_release(word1);
        release(block, word1);
        ++drained;
    }
    counters_.remote_frees_drained.add(drained);
}

std::size_t ThreadHeap::largest_free_payload() const noexcept {
    if (summary_ == 0) {
        return 0;
    }
    const auto word = static_cast<unsigned>(63 - std::countl_zero(summary_));
    const unsigned bucket =
        (word << 6) + static_cast<unsigned>(63 - std::countl_zero(nonempty_[word]));
    std::size_t largest = 0;
    if (bucket < kExactBuckets) {
        largest = bucket_min_size(bucket);
    } else {
        for (Block* block = heads_[bucket]; block != nullptr; block = link_next(block)) {
            largest = std::max(largest, size_field(load_word(block->word1)));
        }
    }
    return largest - kHeaderSize;
}

Span* ThreadHeap::take_empty_spans() noexcept {
    drain_pending();
    Span* empty = nullptr;
    Span** link = &spans_;
    while (*link != nullptr) {
        Span* span = *link;
        Block* first = block_at(address_of(span) + kSpanHeaderSize);
        const std::size_t block_size = span->size - kSpanOverhead;
        const std::uint64_t word1 = load_word(first->word1);
        if ((load_word(first->word0) & kStateMask) == kStateFree &&
            size_field(word1) == block_size) {
            unlink_free(first, bucket_index(block_size));
            *link = span->next;
            span->next = empty;
            empty = span;
            counters_.span_count.sub(1);
            counters_.span_bytes.sub(span->size);
        } else {
            link = &span->next;
        }
    }
    return empty;
}

std::size_t ThreadHeap::free_block_count(unsigned bucket) const noexcept {
    std::size_t count = 0;
    for (Block* block = heads_[bucket]; block != nullptr; block = link_next(block)) {
        ++count;
    }
    return count;
}

const char* ThreadHeap::validate() const noexcept {
    const std::uint64_t owner = owner_word();
    std::size_t free_listable = 0;
    std::size_t total_blocks = 0;
    std::uint64_t live_count = 0;
    std::uint64_t live_consumed = 0;
    std::uint64_t live_payload = 0;
    std::uint64_t live_requested = 0;
    std::uint64_t fragments = 0;
    std::uint64_t span_count = 0;
    std::uint64_t span_bytes = 0;

    for (Span* span = spans_; span != nullptr; span = span->next) {
        if (span->magic != kSpanMagic || span->owner != this) {
            return "span header is corrupted or owned by another heap";
        }
        ++span_count;
        span_bytes += span->size;
        Block* block = block_at(address_of(span) + kSpanHeaderSize);
        Block* fence = block_at(address_of(span) + span->size - kHeaderSize);
        std::size_t previous = 0;
        bool previous_free = false;
        while (block < fence) {
            const std::uint64_t word0 = load_word(block->word0);
            const std::uint64_t word1 = load_word(block->word1);
            if ((word1 & kMagicMask) != kMagic) {
                return "block header has no magic value";
            }
            const std::size_t size = size_field(word1);
            if (size < kHeaderSize || size > address_of(fence) - address_of(block)) {
                return "block size is out of range";
            }
            if (prev_field(word1) != previous) {
                return "previous-size field does not match the previous block";
            }
            const std::uint64_t state = word0 & kStateMask;
            if (state == kStateFree) {
                if (previous_free) {
                    return "two adjacent free blocks were not coalesced";
                }
                if ((word0 & kOwnerMask) != owner) {
                    return "free block has a foreign owner";
                }
                if (size >= kMinFreeBlock) {
                    ++free_listable;
                } else {
                    fragments += kHeaderSize;
                }
            } else if (state == kStateAllocated || state == kStatePending) {
                if (state == kStateAllocated && (word0 & kOwnerMask) != owner) {
                    return "allocated block has a foreign owner";
                }
                const std::size_t payload = size - kHeaderSize;
                ++live_count;
                live_consumed += size;
                live_payload += payload;
                live_requested += payload - (word1 & kSlackMask);
            } else {
                return "span contains a block in the external state";
            }
            previous_free = state == kStateFree;
            previous = size;
            ++total_blocks;
            block = block_after(block, size);
        }
        if (block != fence) {
            return "blocks do not end exactly at the span fence";
        }
        const std::uint64_t fence_word1 = load_word(fence->word1);
        if (load_word(fence->word0) != (owner | kStateAllocated) ||
            (fence_word1 & kMagicMask) != kMagic || prev_field(fence_word1) != previous) {
            return "span fence is corrupted";
        }
    }

    std::size_t listed = 0;
    for (unsigned bucket = 0; bucket < kBucketCount; ++bucket) {
        Block* previous = nullptr;
        std::size_t count = 0;
        for (Block* node = heads_[bucket]; node != nullptr; node = link_next(node)) {
            if (++count > total_blocks) {
                return "a free list is cyclic";
            }
            const std::uint64_t word0 = load_word(node->word0);
            if (word0 != (owner | kStateFree)) {
                return "a listed block is not free";
            }
            const std::size_t size = size_field(load_word(node->word1));
            if (size < kMinFreeBlock || bucket_index(size) != bucket) {
                return "a free block is in the wrong bucket";
            }
            bool inside = false;
            for (Span* span = spans_; span != nullptr && !inside; span = span->next) {
                inside = address_of(node) > address_of(span) &&
                         address_of(node) < address_of(span) + span->size;
            }
            if (!inside) {
                return "a listed block lies outside the heap's spans";
            }
            if (previous != nullptr && link_prev(node) != previous) {
                return "a free list has a broken back link";
            }
            previous = node;
        }
        const bool marked = ((nonempty_[bucket >> 6] >> (bucket & 63U)) & 1U) != 0;
        if (marked != (count > 0)) {
            return "the bucket bitmap disagrees with a free list";
        }
        listed += count;
    }
    for (unsigned word = 0; word < kBitmapWords; ++word) {
        if ((((summary_ >> word) & 1U) != 0) != (nonempty_[word] != 0)) {
            return "the summary bitmap disagrees with a bitmap word";
        }
    }
    if ((summary_ >> kBitmapWords) != 0) {
        return "the summary bitmap has bits beyond the last word";
    }
    if (listed != free_listable) {
        return "free lists do not contain exactly the free blocks";
    }
    const HeapCounters& c = counters_;
    if (c.live_count() != live_count || c.live_consumed.get() != live_consumed ||
        c.live_payload.get() != live_payload ||
        c.live_requested.get() != live_requested) {
        return "live-allocation counters disagree with the blocks";
    }
    if (c.fragment_bytes.get() != fragments) {
        return "fragment counter disagrees with the blocks";
    }
    if (c.span_count.get() != span_count || c.span_bytes.get() != span_bytes) {
        return "span counters disagree with the span list";
    }
    return nullptr;
}

}  // namespace nanoalloc::detail
