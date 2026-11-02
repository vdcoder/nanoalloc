// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "nanoalloc/pool.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

#include "block.hpp"
#include "diagnostics.hpp"
#include "heap.hpp"

namespace nanoalloc {

namespace {

using detail::Block;
using detail::kHeaderSize;
using detail::Span;
using detail::ThreadHeap;

static_assert(Pool::granularity == detail::kGranule);
static_assert(Pool::header_size == detail::kHeaderSize);
static_assert((Pool::max_alignment & (Pool::max_alignment - 1)) == 0);
static_assert(Pool::max_alignment < (std::size_t{1} << 16), "log2 must fit in 4 bits");

/// RAII owner of the arena. Aligned to max_alignment so that span offsets
/// are the only thing that determines block alignment.
class Arena {
public:
    explicit Arena(std::size_t bytes)
        : data_(static_cast<std::byte*>(
              ::operator new(bytes, std::align_val_t{Pool::max_alignment}))),
          size_(bytes) {}

    ~Arena() {
        detail::unpoison_memory(data_, size_);
        ::operator delete(data_, std::align_val_t{Pool::max_alignment});
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    [[nodiscard]] std::byte* data() const noexcept { return data_; }

private:
    std::byte* data_;
    std::size_t size_;
};

constexpr std::size_t round_up(std::size_t value, std::size_t multiple) noexcept {
    return (value + multiple - 1) & ~(multiple - 1);
}

/// True for non-zero powers of two not exceeding Pool::max_alignment.
constexpr bool is_supported_alignment(std::size_t alignment) noexcept {
    return alignment - 1 < Pool::max_alignment && (alignment & (alignment - 1)) == 0;
}

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr std::size_t kHeapObjectSize = round_up(sizeof(ThreadHeap), 64);

// ---- thread-local lookup -------------------------------------------------------
// A small direct-mapped cache from pool id to the calling thread's heap. It is
// trivially constructible, so accessing it needs no initialization guard.
// Pool ids are never reused, so an entry of a destroyed pool can never match.
constexpr unsigned kTlsSlots = 4;

struct TlsSlot {
    std::uint64_t pool_id;
    ThreadHeap* heap;  // nullptr: this thread has no heap in that pool
};

struct ThreadState {
    std::uint64_t thread_id;
    TlsSlot slots[kTlsSlots];
};

thread_local constinit ThreadState t_state{};

std::atomic<std::uint64_t> g_next_pool_id{1};
std::atomic<std::uint64_t> g_next_thread_id{1};

std::uint64_t current_thread_id() noexcept {
    if (t_state.thread_id == 0) {
        t_state.thread_id = g_next_thread_id.fetch_add(1, std::memory_order_relaxed);
    }
    return t_state.thread_id;
}

/// Bookkeeping in front of every oversized allocation:
/// [padding][ExternalLinks][header][payload...], payload aligned as requested.
struct ExternalLinks {
    ExternalLinks* prev;
    ExternalLinks* next;
};
static_assert(sizeof(ExternalLinks) == 16);

}  // namespace

// =============================================================================
// PoolCore: the implementation behind Pool.
// =============================================================================

class alignas(64) PoolCore : public detail::SpanProvider {
public:
    explicit PoolCore(const PoolConfig& config);
    virtual ~PoolCore();

    PoolCore(const PoolCore&) = delete;
    PoolCore& operator=(const PoolCore&) = delete;

    void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
        if (!is_supported_alignment(alignment)) [[unlikely]] {
            failed_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (bytes <= detail::kMaxSpanPayload) [[likely]] {
            const std::size_t payload = round_up(bytes, detail::kGranule);
            ThreadHeap* heap = current_heap(true);
            if (heap == nullptr) [[unlikely]] {
                failed_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            if (alignment <= detail::kGranule) [[likely]] {
                return heap->allocate(bytes, payload);
            }
            if (payload + alignment <= detail::kMaxBlockSize) {
                return heap->allocate_aligned(bytes, payload, alignment);
            }
        }
        return allocate_external(bytes, alignment);
    }

    void deallocate(void* ptr) noexcept {
        Block* block = checked_header(ptr);
        const std::uint64_t word0 = detail::load_word(block->word0);
        const std::uint64_t state = word0 & detail::kStateMask;
        if (state == detail::kStateAllocated) [[likely]] {
            const std::uint64_t word1 = detail::load_word(block->word1);
            if ((word1 & detail::kMagicMask) != detail::kMagic) [[unlikely]] {
                bad_pointer(ptr);
            }
            auto* owner = reinterpret_cast<ThreadHeap*>(word0 & detail::kOwnerMask);
            ThreadHeap* self = current_heap(false);
            if (owner == self) [[likely]] {
                self->deallocate_local(block, word1);
                return;
            }
            deallocate_remote(owner, self, block, word0, ptr);
            return;
        }
        if (state == detail::kStateExternal) {
            deallocate_external(block, word0, ptr);
            return;
        }
        if ((detail::load_word(block->word1) & detail::kMagicMask) != detail::kMagic) {
            bad_pointer(ptr);
        }
        detail::fatal_error(state == detail::kStateFree
                                ? "double free (block is already free)"
                                : "double free (block is already waiting in an inbox)",
                            ptr);
    }

    [[nodiscard]] std::size_t usable_size(const void* ptr) const noexcept;
    [[nodiscard]] bool owns(const void* ptr) const noexcept;
    [[nodiscard]] PoolStats stats() const noexcept;
    [[nodiscard]] bool verify_integrity() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    void drain_pending_frees() noexcept {
        if (ThreadHeap* heap = current_heap(false)) {
            heap->drain_pending();
        }
    }

    /// Called at thread exit (under the registry lock) for every live pool.
    void retire_thread(std::uint64_t thread_id) noexcept;

    Span* acquire_span(std::size_t min_block) noexcept override;

private:
    ThreadHeap* current_heap(bool create) noexcept {
        TlsSlot& slot = t_state.slots[id_ & (kTlsSlots - 1)];
        if (slot.pool_id == id_ && (slot.heap != nullptr || !create)) [[likely]] {
            return slot.heap;
        }
        return current_heap_slow(create);
    }
    [[nodiscard]] ThreadHeap* current_heap_if_cached() const noexcept;
    ThreadHeap* current_heap_slow(bool create) noexcept;
    ThreadHeap* create_heap_locked(std::uint64_t thread_id) noexcept;

    static Block* checked_header(const void* ptr) noexcept {
        if ((detail::address_of(ptr) & (detail::kGranule - 1)) != 0) [[unlikely]] {
            detail::fatal_error("pointer was not allocated by this pool (misaligned)", ptr);
        }
        return detail::header_of(ptr);
    }
    [[noreturn]] static void bad_pointer(const void* ptr) noexcept {
        detail::fatal_error(
            "pointer was not allocated by this pool or does not refer to the start of an "
            "allocated block",
            ptr);
    }

    void deallocate_remote(ThreadHeap* owner, ThreadHeap* self, Block* block,
                           std::uint64_t word0, const void* ptr) noexcept;
    void* allocate_external(std::size_t bytes, std::size_t alignment) noexcept;
    void deallocate_external(Block* block, std::uint64_t word0, const void* ptr) noexcept;
    /// Bytes still available in the budget (caller holds mutex_).
    [[nodiscard]] std::size_t available_locked() const noexcept {
        return capacity_ - arena_used_ - external_reserved_;
    }
    /// Payload of the largest span the calling thread could still obtain.
    [[nodiscard]] std::size_t span_payload_available_locked(bool has_heap) const noexcept;

    const std::uint64_t id_;
    const std::size_t capacity_;
    const bool track_requested_;
    Arena arena_;

    mutable std::mutex mutex_;
    std::size_t arena_used_ = 0;  // arena bytes carved for spans and heaps
    std::size_t carved_spans_ = 0;
    std::size_t carved_span_bytes_ = 0;
    Span* free_spans_ = nullptr;  // spans returned by exited threads
    ThreadHeap* heaps_ = nullptr;
    std::size_t heap_count_ = 0;

    // Oversized (system) allocations, guarded by mutex_.
    ExternalLinks* external_ = nullptr;
    std::size_t external_reserved_ = 0;  // system bytes of live allocations
    std::size_t external_live_ = 0;
    std::size_t external_payload_ = 0;
    std::size_t external_requested_ = 0;
    std::size_t external_peak_payload_ = 0;
    std::size_t external_peak_requested_ = 0;
    std::size_t external_peak_consumed_ = 0;
    std::uint64_t external_allocations_ = 0;
    std::uint64_t external_frees_ = 0;

    std::atomic<std::uint64_t> failed_{0};
    // Remote frees issued by threads that have no heap in this pool.
    std::atomic<std::uint64_t> heapless_remote_frees_{0};
};

struct Pool::Impl final : PoolCore {
    using PoolCore::PoolCore;
};

namespace {

/// Live pools, for thread-exit processing. Intentionally leaked so that it
/// outlives every thread_local destructor.
struct Registry {
    std::mutex mutex;
    std::vector<PoolCore*> pools;
};

Registry& registry() {
    static Registry* instance = new Registry();
    return *instance;
}

/// Its destructor runs when a thread that created a heap exits.
struct ThreadExitHook {
    bool armed = false;
    ThreadExitHook() = default;
    ThreadExitHook(const ThreadExitHook&) = delete;
    ThreadExitHook& operator=(const ThreadExitHook&) = delete;
    ~ThreadExitHook() {
        if (!armed) {
            return;
        }
        const std::uint64_t thread_id = t_state.thread_id;
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (PoolCore* pool : reg.pools) {
            pool->retire_thread(thread_id);
        }
        t_state = ThreadState{};
    }
};

thread_local ThreadExitHook t_exit_hook;

}  // namespace

// ---- construction --------------------------------------------------------------

PoolCore::PoolCore(const PoolConfig& config)
    : id_(g_next_pool_id.fetch_add(1, std::memory_order_relaxed)),
      capacity_(round_up(config.capacity_bytes, Pool::granularity)),
      track_requested_(config.track_requested_bytes),
      arena_(round_up(config.capacity_bytes, Pool::granularity)) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.pools.push_back(this);
}

PoolCore::~PoolCore() {
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.pools.erase(std::find(reg.pools.begin(), reg.pools.end(), this));
    }
    for (ExternalLinks* node = external_; node != nullptr;) {
        ExternalLinks* next = node->next;
        Block* header = reinterpret_cast<Block*>(node + 1);
        const std::uint64_t word0 = detail::load_word(header->word0);
        const std::size_t alignment = std::size_t{1}
                                      << ((word0 >> detail::kAlignShiftPos) & 15U);
        const std::size_t front = std::max<std::size_t>(alignment, 32);
        ::operator delete(reinterpret_cast<std::byte*>(node + 1) + kHeaderSize - front,
                          std::align_val_t{alignment});
        node = next;
    }
}

// ---- thread heaps --------------------------------------------------------------

ThreadHeap* PoolCore::current_heap_slow(bool create) noexcept {
    const std::uint64_t thread_id = current_thread_id();
    ThreadHeap* found = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
            if (heap->thread_id() == thread_id && heap->status() == ThreadHeap::kActive) {
                found = heap;
                break;
            }
        }
        if (found == nullptr && create) {
            found = create_heap_locked(thread_id);
        }
    }
    if (found != nullptr) {
        t_exit_hook.armed = true;  // registers the thread-exit cleanup
    }
    t_state.slots[id_ & (kTlsSlots - 1)] = TlsSlot{id_, found};
    return found;
}

ThreadHeap* PoolCore::current_heap_if_cached() const noexcept {
    const TlsSlot& slot = t_state.slots[id_ & (kTlsSlots - 1)];
    if (slot.pool_id == id_) {
        return slot.heap;
    }
    const std::uint64_t thread_id = t_state.thread_id;
    if (thread_id == 0) {
        return nullptr;
    }
    // Caller holds mutex_.
    for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        if (heap->thread_id() == thread_id && heap->status() == ThreadHeap::kActive) {
            return heap;
        }
    }
    return nullptr;
}

ThreadHeap* PoolCore::create_heap_locked(std::uint64_t thread_id) noexcept {
    // Reuse the heap object of an exited thread that left no blocks behind.
    for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        if (heap->status() == ThreadHeap::kRecyclable) {
            ThreadHeap* next = heap->next_in_pool;
            heap->~ThreadHeap();
            auto* fresh = ::new (static_cast<void*>(heap)) ThreadHeap(*this, id_, thread_id);
            fresh->next_in_pool = next;
            return fresh;
        }
    }
    if (available_locked() < kHeapObjectSize) {
        return nullptr;
    }
    void* memory = arena_.data() + arena_used_;
    arena_used_ += kHeapObjectSize;
    auto* heap = ::new (memory) ThreadHeap(*this, id_, thread_id);
    heap->next_in_pool = heaps_;
    heaps_ = heap;
    ++heap_count_;
    return heap;
}

Span* PoolCore::acquire_span(std::size_t min_block) noexcept {
    const std::size_t min_span = min_block + detail::kSpanOverhead;
    std::lock_guard<std::mutex> lock(mutex_);
    for (Span** link = &free_spans_; *link != nullptr; link = &(*link)->next) {
        if ((*link)->size >= min_span) {
            Span* span = *link;
            *link = span->next;
            return span;
        }
    }
    const std::size_t size = std::min(detail::kSpanSize, available_locked()) & ~std::size_t{63};
    if (size < min_span || size < detail::kMinSpanSize) {
        return nullptr;
    }
    auto* span = reinterpret_cast<Span*>(arena_.data() + arena_used_);
    span->size = size;
    arena_used_ += size;
    ++carved_spans_;
    carved_span_bytes_ += size;
    return span;
}

void PoolCore::retire_thread(std::uint64_t thread_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        if (heap->thread_id() != thread_id || heap->status() != ThreadHeap::kActive) {
            continue;
        }
        Span* empty = heap->take_empty_spans();
        while (empty != nullptr) {
            Span* next = empty->next;
            empty->owner = nullptr;
            empty->next = free_spans_;
            free_spans_ = empty;
            empty = next;
        }
        heap->set_status(heap->spans() == nullptr ? ThreadHeap::kRecyclable
                                                  : ThreadHeap::kRetired);
    }
}

// ---- deallocation paths ------------------------------------------------------------

void PoolCore::deallocate_remote(ThreadHeap* owner, ThreadHeap* self, Block* block,
                                 std::uint64_t word0, const void* ptr) noexcept {
    if (owner->pool_id() != id_) {
        detail::fatal_error("pointer was not allocated by this pool", ptr);
    }
    if (owner->status() != ThreadHeap::kActive) {
        detail::fatal_error(
            "deallocation of a block whose owning thread has exited (thread lifetime "
            "contract violated)",
            ptr);
    }
    if (self != nullptr) {
        self->counters().remote_frees_sent.add(1);
    } else {
        heapless_remote_frees_.fetch_add(1, std::memory_order_relaxed);
    }
    owner->enqueue_remote(block, word0);
}

void* PoolCore::allocate_external(std::size_t bytes, std::size_t alignment) noexcept {
    const std::size_t align = std::max(alignment, detail::kGranule);
    const std::size_t front = std::max<std::size_t>(align, 32);
    if (bytes > capacity_) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const std::size_t payload = round_up(bytes, detail::kGranule);
    const std::size_t total = front + payload;
    std::lock_guard<std::mutex> lock(mutex_);
    if (total > available_locked()) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    void* base = ::operator new(total, std::align_val_t{align}, std::nothrow);
    if (base == nullptr) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    std::byte* user = static_cast<std::byte*>(base) + front;
    auto* node = reinterpret_cast<ExternalLinks*>(user - kHeaderSize - sizeof(ExternalLinks));
    Block* header = detail::header_of(user);
    const auto log2_align = static_cast<std::uint64_t>(std::countr_zero(align));
    detail::store_word(header->word0, detail::address_of(this) |
                                          (log2_align << detail::kAlignShiftPos) |
                                          detail::kStateExternal);
    detail::store_word(header->word1, static_cast<std::uint64_t>(payload) |
                                          static_cast<std::uint64_t>(payload - bytes));
    node->prev = nullptr;
    node->next = external_;
    if (external_ != nullptr) {
        external_->prev = node;
    }
    external_ = node;

    external_reserved_ += total;
    ++external_live_;
    external_payload_ += payload;
    external_requested_ += bytes;
    ++external_allocations_;
    external_peak_payload_ = std::max(external_peak_payload_, external_payload_);
    external_peak_requested_ = std::max(external_peak_requested_, external_requested_);
    external_peak_consumed_ = std::max(external_peak_consumed_, external_reserved_);
    return user;
}

void PoolCore::deallocate_external(Block* block, std::uint64_t word0, const void* ptr) noexcept {
    if ((word0 & detail::kOwnerMask) != detail::address_of(this)) {
        bad_pointer(ptr);
    }
    const std::size_t align = std::size_t{1} << ((word0 >> detail::kAlignShiftPos) & 15U);
    const std::size_t front = std::max<std::size_t>(align, 32);
    const std::uint64_t word1 = detail::load_word(block->word1);
    const std::size_t payload = static_cast<std::size_t>(word1 & ~detail::kSlackMask);
    auto* node = reinterpret_cast<ExternalLinks*>(block) - 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            external_ = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        }
        external_reserved_ -= front + payload;
        --external_live_;
        external_payload_ -= payload;
        external_requested_ -= payload - static_cast<std::size_t>(word1 & detail::kSlackMask);
        ++external_frees_;
    }
    // Mark the header so that a repeated free is not mistaken for a live block.
    detail::store_word(block->word0, detail::kStateFree);
    ::operator delete(reinterpret_cast<std::byte*>(block) + kHeaderSize - front,
                      std::align_val_t{align});
}

// ---- queries ---------------------------------------------------------------------

std::size_t PoolCore::usable_size(const void* ptr) const noexcept {
    Block* block = checked_header(ptr);
    const std::uint64_t word0 = detail::load_word(block->word0);
    const std::uint64_t state = word0 & detail::kStateMask;
    if (state == detail::kStateAllocated) {
        const std::uint64_t word1 = detail::load_word(block->word1);
        if ((word1 & detail::kMagicMask) != detail::kMagic) {
            bad_pointer(ptr);
        }
        if (reinterpret_cast<ThreadHeap*>(word0 & detail::kOwnerMask)->pool_id() != id_) {
            detail::fatal_error("pointer was not allocated by this pool", ptr);
        }
        return detail::size_field(word1) - kHeaderSize;
    }
    if (state == detail::kStateExternal) {
        if ((word0 & detail::kOwnerMask) != detail::address_of(this)) {
            bad_pointer(ptr);
        }
        return static_cast<std::size_t>(detail::load_word(block->word1) & ~detail::kSlackMask);
    }
    if ((detail::load_word(block->word1) & detail::kMagicMask) != detail::kMagic) {
        bad_pointer(ptr);
    }
    detail::fatal_error("pointer refers to a block that is not allocated", ptr);
}

bool PoolCore::owns(const void* ptr) const noexcept {
    const std::uintptr_t address = detail::address_of(ptr);
    if (address - detail::address_of(arena_.data()) < capacity_) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ExternalLinks* node = external_; node != nullptr; node = node->next) {
        const auto* header = reinterpret_cast<const Block*>(node + 1);
        const std::uintptr_t payload = detail::address_of(header + 1);
        const std::uint64_t word1 =
            detail::load_word(const_cast<std::uint64_t&>(header->word1));
        if (address >= payload && address - payload < (word1 & ~detail::kSlackMask)) {
            return true;
        }
    }
    return false;
}

std::size_t PoolCore::span_payload_available_locked(bool has_heap) const noexcept {
    std::size_t best = 0;
    for (const Span* span = free_spans_; span != nullptr; span = span->next) {
        best = std::max(best, span->size);
    }
    std::size_t available = available_locked();
    if (!has_heap) {
        available = available >= kHeapObjectSize ? available - kHeapObjectSize : 0;
    }
    const std::size_t carve = std::min(detail::kSpanSize, available) & ~std::size_t{63};
    best = std::max(best, carve);
    if (best < detail::kMinSpanSize) {
        return 0;
    }
    return best - detail::kSpanOverhead - kHeaderSize;
}

PoolStats PoolCore::stats() const noexcept {
    PoolStats stats;
    stats.capacity_bytes = capacity_;
    stats.tracking_bytes = 0;
    stats.requested_bytes_tracked = track_requested_;

    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t live_count = 0;
    std::uint64_t live_payload = 0;
    std::uint64_t live_requested = 0;
    std::uint64_t allocations = 0;
    std::uint64_t local_frees = 0;
    std::uint64_t remote_sent = heapless_remote_frees_.load(std::memory_order_relaxed);
    std::uint64_t remote_drained = 0;
    std::uint64_t failed = failed_.load(std::memory_order_relaxed);
    std::uint64_t peak_payload = 0;
    std::uint64_t peak_requested = 0;
    std::uint64_t peak_consumed = 0;
    for (const ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        const detail::HeapCounters& c = heap->counters();
        live_count += c.live_count.get();
        live_payload += c.live_payload.get();
        live_requested += c.live_requested.get();
        allocations += c.allocations.get();
        local_frees += c.local_frees.get();
        remote_sent += c.remote_frees_sent.get();
        remote_drained += c.remote_frees_drained.get();
        failed += c.failed.get();
        peak_payload += c.peak_payload.get();
        peak_requested += c.peak_requested.get();
        peak_consumed += c.peak_consumed.get();
        stats.split_count += c.splits.get();
        stats.merge_count += c.merges.get();
        stats.fragment_bytes += c.fragment_bytes.get();
    }

    stats.thread_heap_count = heap_count_;
    stats.span_count = carved_spans_;
    stats.span_bytes = carved_span_bytes_;
    stats.metadata_bytes =
        sizeof(PoolCore) + heap_count_ * kHeapObjectSize + carved_spans_ * detail::kSpanOverhead;
    stats.reserved_bytes = arena_used_ + external_reserved_;

    stats.live_allocations = live_count + external_live_;
    stats.live_usable_bytes = live_payload + external_payload_;
    stats.live_consumed_bytes = live_payload + live_count * kHeaderSize + external_reserved_;
    stats.header_bytes = live_count * kHeaderSize + (external_reserved_ - external_payload_);
    stats.pending_remote_frees = remote_sent > remote_drained ? remote_sent - remote_drained : 0;
    stats.oversized_live_allocations = external_live_;
    stats.oversized_live_bytes = external_payload_;

    stats.peak_usable_bytes = peak_payload + external_peak_payload_;
    stats.peak_consumed_bytes = peak_consumed + external_peak_consumed_;
    if (track_requested_) {
        stats.live_requested_bytes = live_requested + external_requested_;
        stats.peak_requested_bytes = peak_requested + external_peak_requested_;
    }

    stats.free_bytes = capacity_ - stats.live_consumed_bytes;
    ThreadHeap* own = current_heap_if_cached();
    stats.largest_free_block = std::max(own != nullptr ? own->largest_free_payload() : 0,
                                        span_payload_available_locked(own != nullptr));

    stats.allocation_count = allocations + external_allocations_;
    stats.local_deallocation_count = local_frees;
    stats.remote_deallocation_count = remote_sent;
    stats.deallocation_count = local_frees + remote_sent + external_frees_;
    stats.oversized_allocation_count = external_allocations_;
    stats.failed_allocation_count = failed;
    return stats;
}

bool PoolCore::verify_integrity() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t heaps = 0;
    std::size_t owned_spans = 0;
    std::size_t owned_span_bytes = 0;
    for (const ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        ++heaps;
        if (heap->pool_id() != id_) {
            return false;
        }
        if (heap->validate() != nullptr) {
            return false;
        }
        owned_spans += heap->counters().span_count.get();
        owned_span_bytes += heap->counters().span_bytes.get();
    }
    std::size_t free_span_count = 0;
    std::size_t free_span_bytes = 0;
    for (const Span* span = free_spans_; span != nullptr; span = span->next) {
        ++free_span_count;
        free_span_bytes += span->size;
    }
    std::size_t external = 0;
    for (const ExternalLinks* node = external_; node != nullptr; node = node->next) {
        ++external;
        if (node->next != nullptr && node->next->prev != node) {
            return false;
        }
    }
    return heaps == heap_count_ && owned_spans + free_span_count == carved_spans_ &&
           owned_span_bytes + free_span_bytes == carved_span_bytes_ &&
           external == external_live_ && arena_used_ + external_reserved_ <= capacity_;
}

// =============================================================================
// Pool
// =============================================================================

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

void Pool::drain_pending_frees() noexcept { impl_->drain_pending_frees(); }

std::size_t Pool::usable_size(const void* ptr) const noexcept {
    return ptr == nullptr ? 0 : impl_->usable_size(ptr);
}

bool Pool::owns(const void* ptr) const noexcept { return impl_->owns(ptr); }

std::size_t Pool::capacity() const noexcept { return impl_->capacity(); }

PoolStats Pool::stats() const noexcept { return impl_->stats(); }

bool Pool::verify_integrity() const noexcept { return impl_->verify_integrity(); }

}  // namespace nanoalloc
