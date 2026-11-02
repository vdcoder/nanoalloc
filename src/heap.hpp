// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Internal: per-thread allocation state (ThreadHeap) and spans.
//
// A ThreadHeap belongs to one thread and one Pool. It owns the spans it has
// acquired and keeps every free block of those spans in 384 size buckets
// (doubly linked intrusive lists) indexed by a two-level bitmap. Only the
// owning thread touches its buckets, so allocation and local deallocation use
// no locks and no atomic read-modify-write operations.
//
// Other threads release blocks they do not own by pushing them onto the
// owner's inbox, a lock-free multi-producer stack linked through header word0.
// The owner detaches the whole stack (one atomic exchange) at its next
// allocation or deallocation, or on an explicit drain, and frees the blocks
// with the normal coalescing path.
//
// Publication protocol for remote frees (thread B frees a block owned by A):
//   1. B changes the block's word0 from `A | ALLOCATED` to
//      `inbox_head | PENDING` with a CAS. A concurrent or repeated free of
//      the same block makes the CAS fail, which is reported as a double free.
//   2. B pushes the block with a release CAS on A's inbox head (on retry B
//      rewrites the link in word0; the block is still private to B).
//   3. A detaches the stack with an acquire exchange. Every write B made to
//      the block, including its payload and the link, happens-before A's
//      subsequent reuse of the block.
// A only reads the state of a PENDING neighbour (it is not FREE, so no merge
// happens) and never writes word0 of a block it does not own the state of.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "block.hpp"
#include "diagnostics.hpp"

namespace nanoalloc::detail {

class ThreadHeap;

/// Header at the start of every span (48 bytes). The first block header
/// follows immediately; a permanently allocated fence header ends the span.
struct Span {
    std::uint64_t magic;
    ThreadHeap* owner;
    Span* next;  // owner's span list, or the pool's free-span list
    std::size_t size;
    std::uint64_t reserved[2];
};
static_assert(sizeof(Span) == kSpanHeaderSize);
inline constexpr std::uint64_t kSpanMagic = 0x4E414E4F5350414EULL;

/// Supplies spans to thread heaps (implemented by Pool::Impl).
class SpanProvider {
public:
    /// Returns an uninitialized span able to hold a block of `min_block` bytes,
    /// or nullptr if the pool's capacity does not allow one.
    virtual Span* acquire_span(std::size_t min_block) noexcept = 0;

protected:
    SpanProvider() = default;
    ~SpanProvider() = default;
    SpanProvider(const SpanProvider&) = default;
    SpanProvider& operator=(const SpanProvider&) = default;
};

/// A counter written by one thread and read by any thread. Relaxed atomic
/// loads and stores compile to plain moves; there is no read-modify-write.
class OwnerCounter {
public:
    void add(std::uint64_t delta) noexcept {
        value_.store(value_.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
    }
    void sub(std::uint64_t delta) noexcept {
        value_.store(value_.load(std::memory_order_relaxed) - delta, std::memory_order_relaxed);
    }
    void set(std::uint64_t value) noexcept { value_.store(value, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

/// Statistics of one heap. Written only by the owning thread.
struct HeapCounters {
    OwnerCounter allocations;          // successful in-span allocations
    OwnerCounter local_frees;          // deallocations of own blocks
    OwnerCounter remote_frees_sent;    // deallocations of other threads' blocks
    OwnerCounter remote_frees_drained; // own blocks released through the inbox
    OwnerCounter failed;               // failed allocations
    OwnerCounter splits;
    OwnerCounter merges;
    OwnerCounter live_count;           // allocated blocks, including pending
    OwnerCounter live_payload;         // sum of payload sizes of live blocks
    OwnerCounter live_requested;       // sum of requested sizes of live blocks
    OwnerCounter peak_payload;
    OwnerCounter peak_requested;
    OwnerCounter peak_consumed;        // payload + header bytes
    OwnerCounter fragment_bytes;       // free 16-byte (header-only) blocks
    OwnerCounter span_count;
    OwnerCounter span_bytes;
};

class alignas(64) ThreadHeap {
public:
    enum Status : std::uint32_t { kActive = 0, kRetired = 1, kRecyclable = 2 };

    ThreadHeap(SpanProvider& provider, std::uint64_t pool_id, std::uint64_t thread_id) noexcept
        : provider_(&provider), pool_id_(pool_id), thread_id_(thread_id) {}

    ThreadHeap(const ThreadHeap&) = delete;
    ThreadHeap& operator=(const ThreadHeap&) = delete;

    // ---- owner thread ------------------------------------------------------

    /// Allocates `payload` bytes (a multiple of 16, at most kMaxSpanPayload)
    /// for a request of `requested` bytes, 16-byte aligned.
    [[nodiscard]] void* allocate(std::size_t requested, std::size_t payload) noexcept;

    /// As allocate(), aligned to `alignment` (a power of two above 16).
    [[nodiscard]] void* allocate_aligned(std::size_t requested, std::size_t payload,
                                         std::size_t alignment) noexcept;

    /// Releases an ALLOCATED block owned by this heap (`word1` is its header).
    void deallocate_local(Block* block, std::uint64_t word1) noexcept {
        if (inbox_.load(std::memory_order_relaxed) != 0) [[unlikely]] {
            drain_inbox();
            // Draining may have merged a neighbour and rewritten this block's
            // previous-size field.
            word1 = load_word(block->word1);
        }
        account_release(word1);
        counters_.local_frees.add(1);
        release(block, word1);
    }

    /// Processes the remote-free inbox if it is not empty.
    void drain_pending() noexcept {
        if (inbox_.load(std::memory_order_relaxed) != 0) [[unlikely]] {
            drain_inbox();
        }
    }

    /// Largest payload a request can get from this heap's free blocks.
    [[nodiscard]] std::size_t largest_free_payload() const noexcept;

    /// Detaches spans that contain no allocated block and returns them as a
    /// linked list (through Span::next). Drains the inbox first.
    [[nodiscard]] Span* take_empty_spans() noexcept;

    /// Checks spans, blocks, free lists, bitmaps and counters. Returns
    /// nullptr if consistent. The heap must not be in use concurrently.
    [[nodiscard]] const char* validate() const noexcept;

    // ---- any thread ----------------------------------------------------------

    /// Hands a block owned by this heap over from another thread.
    /// `expected_word0` must be `this | ALLOCATED`.
    void enqueue_remote(Block* block, std::uint64_t expected_word0) noexcept;

    [[nodiscard]] std::uint64_t pool_id() const noexcept { return pool_id_; }
    [[nodiscard]] std::uint64_t thread_id() const noexcept { return thread_id_; }
    [[nodiscard]] Status status() const noexcept {
        return static_cast<Status>(status_.load(std::memory_order_acquire));
    }
    void set_status(Status status) noexcept {
        status_.store(status, std::memory_order_release);
    }
    [[nodiscard]] const HeapCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] HeapCounters& counters() noexcept { return counters_; }

    // ---- introspection (tests) ------------------------------------------------
    [[nodiscard]] std::uint64_t summary_bits() const noexcept { return summary_; }
    [[nodiscard]] std::uint64_t bitmap_word(unsigned index) const noexcept {
        return nonempty_[index];
    }
    [[nodiscard]] std::size_t free_block_count(unsigned bucket) const noexcept;
    [[nodiscard]] Span* spans() const noexcept { return spans_; }

    ThreadHeap* next_in_pool = nullptr;  // Pool's heap list (guarded by its mutex)

private:
    struct Fit {
        Block* block;
        unsigned bucket;
    };

    [[nodiscard]] std::uint64_t owner_word() const noexcept {
        return static_cast<std::uint64_t>(address_of(this));
    }

    // bitmap-indexed buckets
    void mark_nonempty(unsigned bucket) noexcept {
        const unsigned word = bucket >> 6;
        nonempty_[word] |= std::uint64_t{1} << (bucket & 63U);
        summary_ |= std::uint64_t{1} << word;
    }
    void mark_empty(unsigned bucket) noexcept {
        const unsigned word = bucket >> 6;
        nonempty_[word] &= ~(std::uint64_t{1} << (bucket & 63U));
        if (nonempty_[word] == 0) {
            summary_ &= ~(std::uint64_t{1} << word);
        }
    }
    /// First non-empty bucket >= start, or kBucketCount.
    [[nodiscard]] unsigned find_nonempty(unsigned start) const noexcept;

    void push_free(Block* block, unsigned bucket) noexcept;
    void unlink_free(Block* block, unsigned bucket) noexcept;
    [[nodiscard]] Fit find_fit(std::size_t need) const noexcept;

    /// Sets the previous-size field of the block following `block`.
    static void set_following_prev(Block* block, std::size_t size) noexcept {
        Block* next = block_after(block, size);
        store_word(next->word1, (load_word(next->word1) & ~kPrevMask) |
                                    (static_cast<std::uint64_t>(size) << kPrevShift));
    }

    void* finish(Block* block, std::size_t need, std::size_t prev, std::size_t requested,
                 std::size_t payload) noexcept;
    bool grow(std::size_t need) noexcept;
    void add_span(Span* span) noexcept;
    void release(Block* block, std::uint64_t word1) noexcept;
    void account_release(std::uint64_t word1) noexcept {
        const std::size_t payload = size_field(word1) - kHeaderSize;
        counters_.live_count.sub(1);
        counters_.live_payload.sub(payload);
        counters_.live_requested.sub(payload - (word1 & kSlackMask));
    }
    void drain_inbox() noexcept;

    // ---- identity: written once, read by any thread ------------------------
    SpanProvider* provider_;
    std::uint64_t pool_id_;
    std::uint64_t thread_id_;
    std::atomic<std::uint32_t> status_{kActive};
    Span* spans_ = nullptr;  // owner's spans (owner thread, or under pool lock)

    // ---- remote side: written by other threads -------------------------------
    alignas(64) std::atomic<std::uintptr_t> inbox_{0};

    // ---- owner side ------------------------------------------------------------
    alignas(64) std::uint64_t summary_ = 0;
    std::uint64_t nonempty_[kBitmapWords] = {};
    HeapCounters counters_;
    Block* heads_[kBucketCount] = {};
};

// =============================================================================
// Inline hot paths
// =============================================================================

inline unsigned ThreadHeap::find_nonempty(unsigned start) const noexcept {
    if (start >= kBucketCount) {
        return kBucketCount;
    }
    const unsigned word = start >> 6;
    const std::uint64_t here = nonempty_[word] & (~std::uint64_t{0} << (start & 63U));
    if (here != 0) {
        return (word << 6) + static_cast<unsigned>(std::countr_zero(here));
    }
    // Words after `word` that contain a non-empty bucket. word + 1 <= 6.
    const std::uint64_t later = summary_ & (~std::uint64_t{0} << (word + 1));
    if (later == 0) {
        return kBucketCount;
    }
    const auto next_word = static_cast<unsigned>(std::countr_zero(later));
    return (next_word << 6) + static_cast<unsigned>(std::countr_zero(nonempty_[next_word]));
}

inline void ThreadHeap::push_free(Block* block, unsigned bucket) noexcept {
    Block* head = heads_[bucket];
    set_link_next(block, head);
    if (head != nullptr) {
        set_link_prev(head, block);
    } else {
        mark_nonempty(bucket);
    }
    heads_[bucket] = block;
}

// The head's `prev` link is not maintained: heads_[] identifies the head, so
// removing the head never touches the following node.
inline void ThreadHeap::unlink_free(Block* block, unsigned bucket) noexcept {
    Block* next = link_next(block);
    if (heads_[bucket] == block) {
        heads_[bucket] = next;
        if (next == nullptr) {
            mark_empty(bucket);
        }
        return;
    }
    Block* prev = link_prev(block);
    set_link_next(prev, next);
    if (next != nullptr) {
        set_link_prev(next, prev);
    }
}

inline ThreadHeap::Fit ThreadHeap::find_fit(std::size_t need) const noexcept {
    const unsigned bucket = bucket_index(need);
    if (need <= kExactLimit) [[likely]] {
        // Exact buckets hold one size each, so every block found from here on
        // is large enough.
        const unsigned found = find_nonempty(bucket);
        if (found < kBucketCount) {
            return {heads_[found], found};
        }
        return {nullptr, 0};
    }
    // A coarse bucket mixes sizes: its blocks may be smaller than `need`.
    // Take its head if it fits, otherwise any block of a higher bucket (all
    // large enough), and only then search the rest of this bucket.
    Block* head = heads_[bucket];
    if (head != nullptr && size_field(load_word(head->word1)) >= need) {
        return {head, bucket};
    }
    const unsigned found = find_nonempty(bucket + 1);
    if (found < kBucketCount) {
        return {heads_[found], found};
    }
    for (Block* block = head; block != nullptr; block = link_next(block)) {
        if (size_field(load_word(block->word1)) >= need) {
            return {block, bucket};
        }
    }
    return {nullptr, 0};
}

inline void* ThreadHeap::finish(Block* block, std::size_t need, std::size_t prev,
                                std::size_t requested, std::size_t payload) noexcept {
    store_word(block->word1, make_word1(need, prev, payload - requested));
    store_word(block->word0, owner_word() | kStateAllocated);
    unpoison_memory(payload_of(block), payload);
    HeapCounters& c = counters_;
    c.allocations.add(1);
    c.live_count.add(1);
    c.live_payload.add(payload);
    c.live_requested.add(requested);
    const std::uint64_t live_payload = c.live_payload.get();
    if (live_payload > c.peak_payload.get()) {
        c.peak_payload.set(live_payload);
    }
    const std::uint64_t live_requested = c.live_requested.get();
    if (live_requested > c.peak_requested.get()) {
        c.peak_requested.set(live_requested);
    }
    const std::uint64_t consumed = live_payload + c.live_count.get() * kHeaderSize;
    if (consumed > c.peak_consumed.get()) {
        c.peak_consumed.set(consumed);
    }
    return payload_of(block);
}

inline void* ThreadHeap::allocate(std::size_t requested, std::size_t payload) noexcept {
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
        const unsigned rest_bucket = bucket_index(rest);
        if (rest_bucket != fit.bucket) {
            unlink_free(free_block, fit.bucket);
            push_free(free_block, rest_bucket);
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

inline void ThreadHeap::release(Block* block, std::uint64_t word1) noexcept {
    const std::size_t size = size_field(word1);
    poison_memory(payload_of(block), size - kHeaderSize);
    Block* start = block;
    std::size_t total = size;
    std::uint64_t start_prev = word1 & kPrevMask;

    Block* next = block_after(block, size);
    if ((load_word(next->word0) & kStateMask) == kStateFree) {
        const std::size_t next_size = size_field(load_word(next->word1));
        if (next_size >= kMinFreeBlock) {
            unlink_free(next, bucket_index(next_size));
        } else {
            counters_.fragment_bytes.sub(kHeaderSize);
        }
        total += next_size;
        counters_.merges.add(1);
    }

    const std::size_t prev_size = prev_field(word1);
    if (prev_size != 0) {
        Block* prev = block_before(block, prev_size);
        if ((load_word(prev->word0) & kStateMask) == kStateFree) {
            const std::uint64_t prev_word1 = load_word(prev->word1);
            const std::size_t merged = prev_size + total;
            counters_.merges.add(1);
            // Leave the absorbed header marked FREE so that a repeated
            // deallocation of this pointer is still recognized.
            store_word(block->word0, owner_word() | kStateFree);
            if (prev_size >= kMinFreeBlock) {
                const unsigned prev_bucket = bucket_index(prev_size);
                if (bucket_index(merged) == prev_bucket) {
                    // Grows within its bucket: update in place, no list work.
                    store_word(prev->word1, (prev_word1 & ~kSizeMask) | merged);
                    set_following_prev(prev, merged);
                    return;
                }
                unlink_free(prev, prev_bucket);
            } else {
                counters_.fragment_bytes.sub(kHeaderSize);
            }
            start = prev;
            total = merged;
            start_prev = prev_word1 & kPrevMask;
        }
    }

    store_word(start->word1, kMagic | start_prev | total);
    if (start == block) {
        store_word(block->word0, owner_word() | kStateFree);
    }
    set_following_prev(start, total);
    if (total >= kMinFreeBlock) {
        push_free(start, bucket_index(total));
    } else {
        counters_.fragment_bytes.add(kHeaderSize);
    }
}

inline void ThreadHeap::enqueue_remote(Block* block, std::uint64_t expected_word0) noexcept {
    std::uintptr_t head = inbox_.load(std::memory_order_relaxed);
    if (!std::atomic_ref<std::uint64_t>(block->word0)
             .compare_exchange_strong(expected_word0, head | kStatePending,
                                      std::memory_order_relaxed, std::memory_order_relaxed)) {
        fatal_error("double free (block was released concurrently or already freed)",
                    payload_of(block));
    }
    while (!inbox_.compare_exchange_weak(head, address_of(block), std::memory_order_release,
                                         std::memory_order_relaxed)) {
        store_word(block->word0, head | kStatePending);
    }
}

}  // namespace nanoalloc::detail
