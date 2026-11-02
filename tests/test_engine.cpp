// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Implementation tests for the span/bucket engine (src/heap.hpp, src/block.hpp)
// and for the guarantees specific to it: exact 16-byte payload rounding,
// 16-byte headers, splitting, coalescing, size buckets and bitmaps, spans and
// oversized allocations. These tests reach into internal headers.
#include <nanoalloc/pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "block.hpp"
#include "heap.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using nanoalloc::Pool;
using nanoalloc::PoolConfig;
using nanoalloc::PoolStats;
using namespace nanoalloc::detail;
using nanotest::Rng;

namespace {

constexpr std::size_t kKiB = 1024;
constexpr std::size_t kMiB = 1024 * 1024;

/// Hands out spans carved from a private arena, like the pool does.
class TestProvider final : public SpanProvider {
public:
    explicit TestProvider(std::size_t bytes, std::size_t span_size = kSpanSize)
        : data_(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{4096}))),
          size_(bytes),
          span_size_(span_size) {}
    ~TestProvider() {
        unpoison_memory(data_, size_);
        ::operator delete(data_, std::align_val_t{4096});
    }
    TestProvider(const TestProvider&) = delete;
    TestProvider& operator=(const TestProvider&) = delete;

    Span* acquire_span(std::size_t min_block) noexcept override {
        const std::size_t size = std::min(span_size_, size_ - used_) & ~std::size_t{63};
        if (size < min_block + kSpanOverhead || size < kMinSpanSize) {
            return nullptr;
        }
        auto* span = reinterpret_cast<Span*>(data_ + used_);
        span->size = size;
        used_ += size;
        ++spans_;
        return span;
    }
    [[nodiscard]] std::size_t spans() const { return spans_; }

private:
    std::byte* data_;
    std::size_t size_;
    std::size_t span_size_;
    std::size_t used_ = 0;
    std::size_t spans_ = 0;
};

struct HeapDeleter {
    void operator()(ThreadHeap* heap) const { delete heap; }
};
using HeapPtr = std::unique_ptr<ThreadHeap, HeapDeleter>;

HeapPtr make_heap(TestProvider& provider) {
    return HeapPtr(new ThreadHeap(provider, 1, 1));
}

constexpr std::size_t payload_for(std::size_t bytes) { return (bytes + 15) & ~std::size_t{15}; }

void* heap_alloc(ThreadHeap& heap, std::size_t bytes) {
    return heap.allocate(bytes, payload_for(bytes));
}

void heap_free(ThreadHeap& heap, void* ptr) {
    Block* block = header_of(ptr);
    heap.deallocate_local(block, load_word(block->word1));
}

std::size_t block_size_of(const void* ptr) {
    return size_field(load_word(header_of(ptr)->word1));
}

#define REQUIRE_HEAP_VALID(heap)                                                      \
    do {                                                                              \
        const char* nanotest_error_ = (heap).validate();                              \
        if (nanotest_error_ != nullptr) {                                             \
            ::nanotest::report_failure(__FILE__, __LINE__,                            \
                                       std::string("validate(): ") + nanotest_error_); \
            throw ::nanotest::AbortTest{};                                            \
        }                                                                             \
    } while (false)

bool bit_set(const ThreadHeap& heap, unsigned bucket) {
    return ((heap.bitmap_word(bucket >> 6) >> (bucket & 63U)) & 1U) != 0;
}

/// The single free block that fills a fresh full-size span.
constexpr std::size_t kFreshBlock = kSpanSize - kSpanOverhead;

}  // namespace

// ---------------------------------------------------------------------------
// Size buckets
// ---------------------------------------------------------------------------

NANO_TEST(bucket_index_matches_the_specified_layout) {
    CHECK_EQ(bucket_index(16), 0U);
    CHECK_EQ(bucket_index(32), 1U);
    CHECK_EQ(bucket_index(4096), 255U);
    CHECK_EQ(bucket_index(4096 + 16), 256U);      // > 4 KiB: 256 B steps
    CHECK_EQ(bucket_index(4096 + 256), 256U);
    CHECK_EQ(bucket_index(4096 + 272), 257U);
    CHECK_EQ(bucket_index(8192), 271U);
    CHECK_EQ(bucket_index(8192 + 16), 272U);      // > 8 KiB: 512 B steps
    CHECK_EQ(bucket_index(16 * kKiB), 287U);
    CHECK_EQ(bucket_index(32 * kKiB), 303U);
    CHECK_EQ(bucket_index(64 * kKiB), 319U);
    CHECK_EQ(bucket_index(128 * kKiB), 335U);
    CHECK_EQ(bucket_index(256 * kKiB), 351U);
    CHECK_EQ(bucket_index(512 * kKiB), 367U);
    CHECK_EQ(bucket_index(512 * kKiB + 16), 368U);  // > 512 KiB: 32 KiB steps
    CHECK_EQ(bucket_index(kMiB), 383U);
    CHECK_EQ(kBucketCount, 384U);

    unsigned previous = 0;
    for (std::size_t size = 16; size <= kMiB; size += 16) {
        const unsigned bucket = bucket_index(size);
        REQUIRE(bucket < kBucketCount);
        REQUIRE(bucket >= previous);  // monotonic
        REQUIRE(size >= bucket_min_size(bucket));
        REQUIRE(size <= bucket_max_size(bucket));
        previous = bucket;
    }
    for (unsigned bucket = 0; bucket < kBucketCount; ++bucket) {
        CHECK_EQ(bucket_index(bucket_min_size(bucket)), bucket);
        CHECK_EQ(bucket_index(bucket_max_size(bucket)), bucket);
        if (bucket + 1 < kBucketCount) {
            CHECK_EQ(bucket_max_size(bucket) + 16, bucket_min_size(bucket + 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Payload rounding, headers and alignment (through the public API)
// ---------------------------------------------------------------------------

NANO_TEST(payload_is_exactly_rounded_to_16_bytes) {
    Pool pool({64 * kMiB});
    for (std::size_t bytes = 0; bytes <= 20000; bytes += (bytes < 300 ? 1 : 7)) {
        void* ptr = pool.allocate(bytes);
        const std::size_t usable = pool.usable_size(ptr);
        REQUIRE_EQ(usable, payload_for(bytes));
        REQUIRE(usable - bytes <= 15);
        REQUIRE(nanotest::is_aligned(ptr, 16));
        REQUIRE_EQ(block_size_of(ptr), usable + kHeaderSize);
        pool.deallocate(ptr);
    }
    for (std::size_t bytes : {kMaxSpanPayload - 1, kMaxSpanPayload}) {
        void* ptr = pool.allocate(bytes);
        CHECK_EQ(pool.usable_size(ptr), payload_for(bytes));
        CHECK_EQ(pool.stats().oversized_live_allocations, 0U);
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(aligned_requests_keep_exact_payload_rounding) {
    Pool pool({16 * kMiB});
    for (std::size_t alignment = 32; alignment <= Pool::max_alignment; alignment *= 2) {
        std::vector<void*> live;
        for (std::size_t bytes : {std::size_t{0}, std::size_t{1}, std::size_t{17},
                                  std::size_t{100}, std::size_t{4095}, std::size_t{5000}}) {
            void* ptr = pool.allocate(bytes, alignment);
            CHECK(nanotest::is_aligned(ptr, alignment));
            CHECK_EQ(pool.usable_size(ptr), payload_for(bytes));
            live.push_back(ptr);
        }
        CHECK(pool.verify_integrity());
        for (void* ptr : live) {
            pool.deallocate(ptr);
        }
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(header_accounting) {
    Pool pool({8 * kMiB});
    std::vector<void*> live;
    std::size_t requested = 0;
    std::size_t usable = 0;
    for (std::size_t bytes = 0; bytes < 400; bytes += 3) {
        live.push_back(pool.allocate(bytes));
        requested += bytes;
        usable += payload_for(bytes);
    }
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.live_requested_bytes, requested);
    CHECK_EQ(stats.live_usable_bytes, usable);
    CHECK_EQ(stats.header_bytes, live.size() * kHeaderSize);
    CHECK_EQ(stats.live_consumed_bytes, usable + live.size() * kHeaderSize);
    CHECK_EQ(stats.tracking_bytes, 0U);
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    stats = pool.stats();
    CHECK_EQ(stats.header_bytes, 0U);
    CHECK_EQ(stats.live_consumed_bytes, 0U);
}

// ---------------------------------------------------------------------------
// Splitting and coalescing
// ---------------------------------------------------------------------------

NANO_TEST(first_allocation_carves_from_the_end_of_a_fresh_span) {
    TestProvider provider(4 * kMiB);
    HeapPtr heap = make_heap(provider);
    void* ptr = heap_alloc(*heap, 100);  // block 128 bytes
    REQUIRE(ptr != nullptr);
    CHECK_EQ(provider.spans(), 1U);
    CHECK_EQ(block_size_of(ptr), 128U);
    const unsigned rest_bucket = bucket_index(kFreshBlock - 128);
    CHECK_EQ(rest_bucket, bucket_index(kFreshBlock));  // same coarse bucket
    CHECK_EQ(heap->free_block_count(rest_bucket), 1U);
    CHECK_EQ(heap->counters().splits.get(), 1U);
    // The allocated block ends at the span fence.
    Span* span = heap->spans();
    CHECK_EQ(address_of(ptr) + 112 + kHeaderSize, address_of(span) + span->size);
    REQUIRE_HEAP_VALID(*heap);
    heap_free(*heap, ptr);
    CHECK_EQ(heap->free_block_count(bucket_index(kFreshBlock)), 1U);
    CHECK_EQ(heap->counters().merges.get(), 1U);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(splitting_at_every_small_remainder) {
    // For each remainder r, carve a free block of exactly need + r bytes and
    // allocate `need` from it.
    for (std::size_t rest = 0; rest <= 96; rest += 16) {
        TestProvider provider(2 * kMiB);
        HeapPtr heap = make_heap(provider);
        // Build: [big free][guard A][hole of 64 + rest][guard B][fence]
        void* guard_b = heap_alloc(*heap, 16);
        void* hole = heap_alloc(*heap, 64 - 16 + rest);  // block = 64 + rest
        void* guard_a = heap_alloc(*heap, 16);
        REQUIRE_EQ(block_size_of(hole), 64 + rest);
        heap_free(*heap, hole);  // neighbours allocated: no merge
        CHECK_EQ(heap->counters().merges.get(), 0U);
        REQUIRE_HEAP_VALID(*heap);

        const std::uint64_t fragments_before = heap->counters().fragment_bytes.get();
        void* ptr = heap_alloc(*heap, 48);  // need = 64: exact bucket picks the hole
        REQUIRE(ptr != nullptr);
        CHECK_EQ(block_size_of(ptr), 64U);
        const std::uintptr_t hole_start = address_of(hole) - kHeaderSize;
        // Carved from the end of the hole.
        CHECK_EQ(address_of(ptr) - kHeaderSize, hole_start + rest);
        if (rest == 16) {
            CHECK_EQ(heap->counters().fragment_bytes.get(), fragments_before + 16);
        } else {
            CHECK_EQ(heap->counters().fragment_bytes.get(), fragments_before);
        }
        if (rest >= kMinFreeBlock) {
            CHECK_EQ(heap->free_block_count(bucket_index(rest)), 1U);
        }
        REQUIRE_HEAP_VALID(*heap);

        // Releasing everything merges every piece back, fragment included.
        heap_free(*heap, ptr);
        heap_free(*heap, guard_a);
        heap_free(*heap, guard_b);
        CHECK_EQ(heap->counters().fragment_bytes.get(), 0U);
        CHECK_EQ(heap->free_block_count(bucket_index(kFreshBlock)), 1U);
        REQUIRE_HEAP_VALID(*heap);
    }
}

NANO_TEST(sixteen_byte_remainders_never_inflate_the_payload) {
    // A request whose only candidate leaves 16 bytes must still get exactly
    // its rounded payload; the 16 bytes become a header-only free fragment.
    Pool pool({4 * kMiB});
    void* a = pool.allocate(16);
    void* hole = pool.allocate(32);  // block 48
    void* b = pool.allocate(16);
    pool.deallocate(hole);
    void* ptr = pool.allocate(16);  // block 32 from the 48-byte hole
    CHECK_EQ(pool.usable_size(ptr), 16U);
    CHECK_EQ(pool.stats().fragment_bytes, 16U);
    CHECK(pool.verify_integrity());
    pool.deallocate(ptr);  // merges with the fragment
    CHECK_EQ(pool.stats().fragment_bytes, 0U);
    pool.deallocate(a);
    pool.deallocate(b);
    CHECK(pool.verify_integrity());
}

NANO_TEST(zero_byte_blocks_are_header_only) {
    TestProvider provider(2 * kMiB);
    HeapPtr heap = make_heap(provider);
    void* left = heap_alloc(*heap, 32);
    void* zero = heap_alloc(*heap, 0);
    void* right = heap_alloc(*heap, 32);
    CHECK_EQ(block_size_of(zero), kHeaderSize);
    CHECK(zero != left);
    CHECK(zero != right);
    heap_free(*heap, zero);  // isolated 16-byte free block: a fragment
    CHECK_EQ(heap->counters().fragment_bytes.get(), 16U);
    REQUIRE_HEAP_VALID(*heap);
    heap_free(*heap, left);
    heap_free(*heap, right);
    CHECK_EQ(heap->counters().fragment_bytes.get(), 0U);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(immediate_merging_with_both_neighbours) {
    TestProvider provider(2 * kMiB);
    HeapPtr heap = make_heap(provider);
    void* guard = heap_alloc(*heap, 64);
    void* c = heap_alloc(*heap, 64);
    void* b = heap_alloc(*heap, 64);
    void* a = heap_alloc(*heap, 64);  // lowest address: [rest][a][b][c][guard]
    REQUIRE(address_of(a) < address_of(b));
    REQUIRE(address_of(b) < address_of(c));
    heap_free(*heap, b);
    CHECK_EQ(heap->free_block_count(bucket_index(80)), 1U);
    heap_free(*heap, c);  // merges with b (previous neighbour)
    CHECK_EQ(heap->free_block_count(bucket_index(80)), 0U);
    CHECK_EQ(heap->free_block_count(bucket_index(160)), 1U);
    heap_free(*heap, a);  // merges with the free rest below and b+c above
    CHECK_EQ(heap->free_block_count(bucket_index(160)), 0U);
    REQUIRE_HEAP_VALID(*heap);
    heap_free(*heap, guard);
    CHECK_EQ(heap->free_block_count(bucket_index(kFreshBlock)), 1U);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(multilevel_coalescing_restores_the_span) {
    TestProvider provider(2 * kMiB);
    HeapPtr heap = make_heap(provider);
    Rng rng(11);
    std::vector<void*> blocks;
    for (int i = 0; i < 5000; ++i) {
        blocks.push_back(heap_alloc(*heap, rng.between(0, 150)));
    }
    REQUIRE_EQ(provider.spans(), 1U);
    nanotest::shuffle(blocks, rng);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        heap_free(*heap, blocks[i]);
        if (i % 500 == 0) {
            REQUIRE_HEAP_VALID(*heap);
        }
    }
    CHECK_EQ(heap->free_block_count(bucket_index(kFreshBlock)), 1U);
    CHECK_EQ(heap->counters().fragment_bytes.get(), 0U);
    CHECK_EQ(heap->counters().live_count(), 0U);
    REQUIRE_HEAP_VALID(*heap);
}

// ---------------------------------------------------------------------------
// Buckets and bitmaps
// ---------------------------------------------------------------------------

NANO_TEST(bitmap_bits_follow_bucket_occupancy) {
    TestProvider provider(2 * kMiB);
    HeapPtr heap = make_heap(provider);
    void* guard1 = heap_alloc(*heap, 16);
    void* hole = heap_alloc(*heap, 200);  // block 224, bucket 13
    void* guard2 = heap_alloc(*heap, 16);
    const unsigned bucket = bucket_index(224);
    CHECK(!bit_set(*heap, bucket));
    heap_free(*heap, hole);
    CHECK(bit_set(*heap, bucket));
    CHECK(((heap->summary_bits() >> (bucket >> 6)) & 1U) != 0);
    void* again = heap_alloc(*heap, 200);  // exact fit empties the bucket
    CHECK(again == hole);
    CHECK(!bit_set(*heap, bucket));
    CHECK_EQ(heap->bitmap_word(0), 0U);
    CHECK_EQ(heap->summary_bits() & 1U, 0U);  // word 0 has no free bucket left
    REQUIRE_HEAP_VALID(*heap);
    heap_free(*heap, again);
    heap_free(*heap, guard1);
    heap_free(*heap, guard2);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(smallest_sufficient_exact_bucket_is_used) {
    TestProvider provider(2 * kMiB);
    HeapPtr heap = make_heap(provider);
    std::vector<void*> guards;
    std::vector<void*> holes;
    for (std::size_t payload : {64U, 96U, 48U}) {
        guards.push_back(heap_alloc(*heap, 16));
        holes.push_back(heap_alloc(*heap, payload));
    }
    guards.push_back(heap_alloc(*heap, 16));
    for (void* hole : holes) {
        heap_free(*heap, hole);
    }
    // Free holes have blocks of 80, 112 and 64 bytes. A 70-byte request
    // (block 96) must take the 112-byte block, the smallest that fits.
    void* ptr = heap_alloc(*heap, 70);
    CHECK_EQ(block_size_of(ptr), 96U);
    CHECK(address_of(ptr) - kHeaderSize == address_of(holes[1]) - kHeaderSize + 16);
    CHECK_EQ(heap->free_block_count(bucket_index(80)), 1U);
    CHECK_EQ(heap->free_block_count(bucket_index(64)), 1U);
    CHECK_EQ(heap->counters().fragment_bytes.get(), 16U);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(coarse_bucket_search_never_returns_an_undersized_block) {
    TestProvider provider(4 * kMiB);
    HeapPtr heap = make_heap(provider);
    // Two holes in coarse bucket 256 (block sizes 4097..4352): 4112 and 4352.
    void* g1 = heap_alloc(*heap, 16);
    void* small = heap_alloc(*heap, 4112 - 16);
    void* g2 = heap_alloc(*heap, 16);
    void* large = heap_alloc(*heap, 4352 - 16);
    void* g3 = heap_alloc(*heap, 16);
    // Exhaust everything else in the span so that only the holes remain.
    std::vector<void*> filler;
    while (heap->spans() != nullptr && provider.spans() == 1) {
        void* ptr = heap_alloc(*heap, 64 * kKiB);
        if (provider.spans() != 1) {
            heap_free(*heap, ptr);
            break;
        }
        filler.push_back(ptr);
    }
    heap_free(*heap, small);  // pushed first
    heap_free(*heap, large);  // head of bucket 256
    const unsigned bucket = bucket_index(4352);
    REQUIRE_EQ(bucket, bucket_index(4112));
    REQUIRE(heap->free_block_count(bucket) >= 2);

    // Request a 4352-byte block: the head (4352) fits.
    void* first = heap_alloc(*heap, 4352 - 16);
    CHECK(first == large);
    // Request 4352 again: only the 4112-byte hole is in the bucket; it must
    // not be returned. A fit has to come from elsewhere.
    void* second = heap_alloc(*heap, 4352 - 16);
    REQUIRE(second != nullptr);
    CHECK(second != small);
    CHECK(block_size_of(second) == 4352);
    // A 4112-byte request can use the hole even though the head check of
    // coarse buckets is not exact: the bucket is searched.
    REQUIRE_HEAP_VALID(*heap);
    heap_free(*heap, first);
    heap_free(*heap, second);
    for (void* ptr : filler) {
        heap_free(*heap, ptr);
    }
    heap_free(*heap, g1);
    heap_free(*heap, g2);
    heap_free(*heap, g3);
    REQUIRE_HEAP_VALID(*heap);
}

NANO_TEST(coarse_bucket_is_searched_before_acquiring_a_span) {
    TestProvider provider(kSpanSize);  // room for exactly one span
    HeapPtr heap = make_heap(provider);
    // Two free blocks in coarse bucket 274 (block sizes 9217..9728): 9728 and
    // 9232, with the smaller one at the head of the list.
    void* g0 = heap_alloc(*heap, 0);
    void* fits = heap_alloc(*heap, 9728 - 16);
    void* g1 = heap_alloc(*heap, 0);
    void* too_small = heap_alloc(*heap, 9232 - 16);
    void* g2 = heap_alloc(*heap, 0);
    // Use up the rest of the span so no other free block remains.
    std::vector<void*> filler;
    for (void* ptr = heap_alloc(*heap, 0); ptr != nullptr; ptr = heap_alloc(*heap, 0)) {
        filler.push_back(ptr);
    }
    CHECK_EQ(heap->largest_free_payload(), 0U);
    heap_free(*heap, fits);
    heap_free(*heap, too_small);  // now the head of the bucket
    const unsigned bucket = bucket_index(9600);
    REQUIRE_EQ(bucket, bucket_index(9232));
    REQUIRE_EQ(bucket, bucket_index(9728));
    REQUIRE_EQ(heap->free_block_count(bucket), 2U);

    // need = 9600: the head (9232) is too small and no higher bucket has a
    // block, so the bucket is searched and the 9728-byte block is used.
    void* ptr = heap_alloc(*heap, 9600 - 16);
    REQUIRE(ptr != nullptr);
    CHECK_EQ(block_size_of(ptr), 9600U);
    CHECK(address_of(ptr) > address_of(fits));
    CHECK(address_of(ptr) < address_of(fits) + 9728);
    CHECK_EQ(provider.spans(), 1U);
    // Nothing else fits a second 9600-byte block.
    CHECK(heap_alloc(*heap, 9600 - 16) == nullptr);
    REQUIRE_HEAP_VALID(*heap);

    heap_free(*heap, ptr);
    for (void* block : filler) {
        heap_free(*heap, block);
    }
    heap_free(*heap, g0);
    heap_free(*heap, g1);
    heap_free(*heap, g2);
    REQUIRE_HEAP_VALID(*heap);
}

// ---------------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------------

NANO_TEST(additional_spans_are_acquired_when_needed) {
    TestProvider provider(3 * kMiB);
    HeapPtr heap = make_heap(provider);
    std::vector<void*> blocks;
    for (;;) {
        void* ptr = heap_alloc(*heap, 300 * kKiB);
        if (ptr == nullptr) {
            break;
        }
        blocks.push_back(ptr);
    }
    // 3 spans of 1 MiB hold 3 blocks of ~300 KiB each.
    CHECK_EQ(provider.spans(), 3U);
    CHECK_EQ(blocks.size(), 9U);
    CHECK_EQ(heap->counters().span_count.get(), 3U);
    CHECK_EQ(heap->counters().failed.get(), 1U);
    REQUIRE_HEAP_VALID(*heap);
    for (void* ptr : blocks) {
        heap_free(*heap, ptr);
    }
    REQUIRE_HEAP_VALID(*heap);
    Span* empty = heap->take_empty_spans();
    std::size_t returned = 0;
    for (; empty != nullptr; empty = empty->next) {
        ++returned;
    }
    CHECK_EQ(returned, 3U);
    CHECK(heap->spans() == nullptr);
    CHECK_EQ(heap->counters().span_count.get(), 0U);
}

NANO_TEST(blocks_never_cross_span_boundaries) {
    Pool pool({8 * kMiB});
    Rng rng(5);
    std::vector<void*> live;
    for (int i = 0; i < 3000; ++i) {
        void* ptr = pool.try_allocate(rng.log_uniform(1, 200 * kKiB));
        if (ptr == nullptr) {
            break;
        }
        live.push_back(ptr);
        if (rng.chance(30)) {
            const std::size_t victim = rng.below(live.size());
            pool.deallocate(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    // verify_integrity walks every span block by block and checks that the
    // blocks end exactly at the span fence.
    CHECK(pool.verify_integrity());
    CHECK(pool.stats().span_count >= 2);
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    CHECK(pool.verify_integrity());
}

NANO_TEST(small_pools_use_smaller_spans) {
    Pool pool({256 * kKiB});
    void* ptr = pool.allocate(1000);
    const PoolStats stats = pool.stats();
    CHECK_EQ(stats.span_count, 1U);
    CHECK(stats.span_bytes <= 256 * kKiB);
    CHECK(stats.reserved_bytes <= stats.capacity_bytes);
    pool.deallocate(ptr);
    CHECK(pool.try_allocate(300 * kKiB) == nullptr);  // larger than the budget
}

// ---------------------------------------------------------------------------
// Oversized allocations
// ---------------------------------------------------------------------------

NANO_TEST(oversized_allocations_use_the_system_allocator) {
    Pool pool({64 * kMiB});
    const std::size_t sizes[] = {kMaxSpanPayload + 1, 2 * kMiB, 5 * kMiB + 3};
    std::vector<void*> live;
    for (std::size_t bytes : sizes) {
        void* ptr = pool.allocate(bytes);
        CHECK(nanotest::is_aligned(ptr, 16));
        CHECK_EQ(pool.usable_size(ptr), payload_for(bytes));
        CHECK(pool.owns(ptr));
        CHECK(pool.owns(static_cast<char*>(ptr) + bytes - 1));
        static_cast<unsigned char*>(ptr)[bytes - 1] = 7;
        live.push_back(ptr);
    }
    void* aligned = pool.allocate(3 * kMiB, 4096);
    CHECK(nanotest::is_aligned(aligned, 4096));
    live.push_back(aligned);
    PoolStats stats = pool.stats();
    CHECK_EQ(stats.oversized_live_allocations, 4U);
    CHECK_EQ(stats.oversized_allocation_count, 4U);
    CHECK_EQ(stats.span_count, 0U);
    CHECK(stats.reserved_bytes >= stats.oversized_live_bytes);
    CHECK(stats.live_consumed_bytes >= stats.live_usable_bytes);
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    stats = pool.stats();
    CHECK_EQ(stats.oversized_live_allocations, 0U);
    CHECK_EQ(stats.reserved_bytes, 0U);
    CHECK_EQ(stats.live_consumed_bytes, 0U);
    CHECK(pool.verify_integrity());
}

NANO_TEST(oversized_allocations_are_charged_against_the_budget) {
    Pool pool({4 * kMiB});
    void* a = pool.allocate(2 * kMiB);
    CHECK(pool.try_allocate(2 * kMiB) == nullptr);  // 2 MiB + bookkeeping > rest
    void* b = pool.allocate(kMiB);
    pool.deallocate(a);
    void* c = pool.allocate(2 * kMiB);
    pool.deallocate(b);
    pool.deallocate(c);
    CHECK_EQ(pool.stats().reserved_bytes, 0U);
}

NANO_TEST(pool_destruction_releases_live_oversized_allocations) {
    // Run under LeakSanitizer/ASan: nothing may leak.
    Pool pool({16 * kMiB});
    (void)pool.allocate(3 * kMiB);
    (void)pool.allocate(2 * kMiB, 1024);
    CHECK_EQ(pool.stats().oversized_live_allocations, 2U);
}

// ---------------------------------------------------------------------------
// Several pools in one thread
// ---------------------------------------------------------------------------

NANO_TEST(many_pools_interleaved_in_one_thread) {
    // More pools than thread-local cache slots, so lookups collide.
    std::vector<std::unique_ptr<Pool>> pools;
    for (int i = 0; i < 9; ++i) {
        pools.push_back(std::make_unique<Pool>(PoolConfig{2 * kMiB}));
    }
    Rng rng(77);
    struct Live {
        std::size_t pool;
        void* ptr;
        std::uint64_t tag;
        std::size_t size;
    };
    std::vector<Live> live;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || rng.chance(55)) {
            const std::size_t index = rng.below(pools.size());
            const std::size_t size = rng.log_uniform(1, 2000);
            void* ptr = pools[index]->try_allocate(size);
            if (ptr != nullptr) {
                const std::uint64_t tag = rng.next();
                nanotest::fill_pattern(ptr, size, tag);
                live.push_back({index, ptr, tag, size});
                REQUIRE(pools[index]->owns(ptr));
            }
        } else {
            const std::size_t victim = rng.below(live.size());
            REQUIRE(nanotest::check_pattern(live[victim].ptr, live[victim].size, live[victim].tag));
            pools[live[victim].pool]->deallocate(live[victim].ptr);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (const Live& entry : live) {
        pools[entry.pool]->deallocate(entry.ptr);
    }
    for (const auto& pool : pools) {
        CHECK(pool->verify_integrity());
        CHECK_EQ(pool->stats().live_allocations, 0U);
        CHECK_EQ(pool->stats().thread_heap_count, 1U);
    }
}

// ---------------------------------------------------------------------------
// Randomized fragmentation with full validation
// ---------------------------------------------------------------------------

NANO_TEST(randomized_fragmentation_keeps_structures_consistent) {
    TestProvider provider(6 * kMiB);
    HeapPtr heap = make_heap(provider);
    Rng rng(2026);
    struct Live {
        void* ptr;
        std::size_t size;
        std::uint64_t tag;
    };
    std::vector<Live> live;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || rng.chance(52)) {
            const std::size_t size =
                rng.chance(3) ? rng.between(4000, 70000) : rng.log_uniform(1, 600);
            void* ptr = rng.chance(5)
                            ? heap->allocate_aligned(size, payload_for(size),
                                                     std::size_t{32} << rng.below(8))
                            : heap_alloc(*heap, size);
            if (ptr != nullptr) {
                const std::uint64_t tag = rng.next();
                nanotest::fill_pattern(ptr, size, tag);
                live.push_back({ptr, size, tag});
            }
        } else {
            const std::size_t victim = rng.below(live.size());
            REQUIRE(nanotest::check_pattern(live[victim].ptr, live[victim].size, live[victim].tag));
            heap_free(*heap, live[victim].ptr);
            live[victim] = live.back();
            live.pop_back();
        }
        if (step % 250 == 0) {
            REQUIRE_HEAP_VALID(*heap);
        }
    }
    for (const Live& entry : live) {
        REQUIRE(nanotest::check_pattern(entry.ptr, entry.size, entry.tag));
        heap_free(*heap, entry.ptr);
    }
    REQUIRE_HEAP_VALID(*heap);
    CHECK_EQ(heap->counters().live_count(), 0U);
    CHECK_EQ(heap->counters().fragment_bytes.get(), 0U);
}

NANOTEST_MAIN()
