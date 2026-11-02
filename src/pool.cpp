// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "nanoalloc/pool.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
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

// Oversized blocks carry the same 16-byte header as span blocks:
//   word0 = (pool address | log2(alignment) << 2 | EXTERNAL) ^ kExternalTag
//   word1 = magic | payload size | slack
// The tag sets high address bits, so a stray copy of the pool's address in
// front of a foreign pointer is not mistaken for an oversized block header.
constexpr std::uint64_t kExternalTag = 0xA55A'0000'0000'0000ULL;
constexpr std::uint64_t kExternalSizeMask = 0x0000'FFFF'FFFF'FFF0ULL;

[[nodiscard]] bool is_external_of(std::uint64_t word0, std::uint64_t word1,
                                  const void* pool) noexcept {
    return ((word0 ^ kExternalTag) & detail::kOwnerMask) == detail::address_of(pool) &&
           (word1 & detail::kMagicMask) == detail::kMagic;
}

[[nodiscard]] std::size_t external_alignment(std::uint64_t word0) noexcept {
    return std::size_t{1} << (((word0 ^ kExternalTag) >> detail::kAlignShiftPos) & 15U);
}

[[nodiscard]] std::size_t external_payload(std::uint64_t word1) noexcept {
    return static_cast<std::size_t>(word1 & kExternalSizeMask);
}

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
    // Set when the thread-exit hook has run: the thread's heaps are retired
    // and its later calls (from thread_local destructors) may not allocate
    // and free through the owners' inboxes.
    bool exiting;
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

    /// `Policy` decides whether failure returns nullptr or throws; failures
    /// are handled in the out-of-line paths, so the fast path is the same.
    template <detail::OnFailure Policy>
    void* allocate(std::size_t bytes,
                   std::size_t alignment) noexcept(Policy == detail::OnFailure::kReturnNull) {
        if (alignment == detail::kGranule && bytes <= detail::kMaxSpanPayload) [[likely]] {
            const TlsSlot& slot = t_state.slots[id_ & (kTlsSlots - 1)];
            if (slot.pool_id == id_ && slot.heap != nullptr) [[likely]] {
                return slot.heap->allocate<Policy>(bytes, round_up(bytes, detail::kGranule));
            }
        }
        if constexpr (Policy == detail::OnFailure::kThrow) {
            return allocate_general_or_throw(bytes, alignment);
        } else {
            return allocate_general(bytes, alignment);
        }
    }

    NANOALLOC_NOINLINE void* allocate_general_or_throw(std::size_t bytes, std::size_t alignment) {
        void* ptr = allocate_general(bytes, alignment);
        if (ptr == nullptr) [[unlikely]] {
            if (!is_power_of_two(alignment)) {
                throw std::invalid_argument(
                    "nanoalloc::Pool::allocate: alignment must be a power of two");
            }
            throw std::bad_alloc();
        }
        return ptr;
    }

    NANOALLOC_NOINLINE void* allocate_general(std::size_t bytes, std::size_t alignment) noexcept {
        if (!is_supported_alignment(alignment) || t_state.exiting) [[unlikely]] {
            // A thread whose heaps have been retired (thread_local destructors
            // after the exit hook) may not allocate: its heaps belong to the
            // reclamation worker, and it gets no new one.
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
        // Fast path: an aligned, valid, allocated block owned by the calling
        // thread whose heap is in the thread-local cache. Everything else is
        // handled out of line.
        if ((detail::address_of(ptr) & (detail::kGranule - 1)) == 0) [[likely]] {
            Block* block = detail::header_of(ptr);
            const std::uint64_t word0 = detail::load_word(block->word0);
            const std::uint64_t word1 = detail::load_word(block->word1);
            const TlsSlot& slot = t_state.slots[id_ & (kTlsSlots - 1)];
            if ((word0 & detail::kStateMask) == detail::kStateAllocated &&
                (word1 & detail::kMagicMask) == detail::kMagic && slot.pool_id == id_ &&
                (word0 & detail::kOwnerMask) == detail::address_of(slot.heap)) [[likely]] {
                slot.heap->deallocate_local(block, word1);
                return;
            }
        }
        deallocate_slow(ptr);
    }

    /// Every deallocation that is not a plain local free, including the
    /// diagnosis of invalid pointers.
    NANOALLOC_NOINLINE void deallocate_slow(void* ptr) noexcept {
        Block* block = checked_header(ptr);
        const std::uint64_t word0 = detail::load_word(block->word0);
        const std::uint64_t state = word0 & detail::kStateMask;
        if (state == detail::kStateAllocated) {
            const std::uint64_t word1 = detail::load_word(block->word1);
            if ((word1 & detail::kMagicMask) != detail::kMagic) {
                bad_pointer(ptr);
            }
            auto* owner = reinterpret_cast<ThreadHeap*>(word0 & detail::kOwnerMask);
            ThreadHeap* self = current_heap(false);
            if (owner == self) {
                self->deallocate_local(block, word1);
                return;
            }
            deallocate_remote(owner, block, word0, ptr);
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

    /// Called at thread exit (under the registry lock) for every live pool:
    /// hands the thread's heap to the reclamation worker, or marks it
    /// recyclable if it holds no block.
    void retire_thread(std::uint64_t thread_id) noexcept;

    enum class Reclaim { kIdle, kProgress, kRecycled };
    /// One reclamation step for a retired heap, run by the reclamation worker
    /// (the only thread that touches the heap's free lists): releases the
    /// frees waiting in its inbox and returns spans that became empty to the
    /// pool. kRecycled: the heap holds nothing any more and is recyclable.
    Reclaim reclaim_retired(ThreadHeap* heap) noexcept;

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
    /// Moves the heap's completely free spans to the free-span list and marks
    /// a heap without spans recyclable. Caller holds mutex_ and is the heap's
    /// exiting owner or the reclamation worker.
    void release_empty_spans_locked(ThreadHeap* heap) noexcept;

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

    void deallocate_remote(ThreadHeap* owner, Block* block, std::uint64_t word0,
                           const void* ptr) noexcept;
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

/// The process-wide reclamation worker for heaps of exited threads.
///
/// A thread that exits while its heap still has allocated blocks hands the
/// heap over: from then on the worker is the only thread that touches the
/// heap's free lists. Any thread may still free the heap's blocks, through
/// its lock-free inbox as for any remote free. The worker periodically drains
/// the inboxes of the heaps it holds (releasing and coalescing the blocks)
/// and returns spans that became empty to their pool. A heap without blocks
/// becomes recyclable and leaves the collection.
///
/// mutex_ guards the collection and is held for a whole pass, so registering
/// a heap, dropping the heaps of a pool being destroyed and
/// verify_integrity() never overlap with the worker touching a heap.
/// Lock order: the registry mutex, then mutex_, then a pool's mutex.
///
/// Intentionally leaked, like the registry; its thread is detached and is
/// started by the first hand-over.
class Releaser {
public:
    std::mutex& mutex() noexcept { return mutex_; }

    /// Takes over a retired heap. Its exiting owner has finished every
    /// operation on the heap: the owner's release of mutex_ here orders them
    /// before the worker's acquisition of mutex_ for its next pass.
    void adopt(ThreadHeap* heap) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        heap->next_retired = retired_;
        retired_ = heap;
        if (!started_) {
            try {
                std::thread([this] { run(); }).detach();
                started_ = true;
            } catch (...) {
                // No worker: the heap keeps its memory reserved, and the next
                // hand-over tries again.
            }
        }
        wake_.notify_one();
    }

    /// Drops the heaps of a pool that is being destroyed. While mutex_ is
    /// held the worker is not inside a pass, and afterwards it cannot reach
    /// the pool any more.
    void forget(const detail::SpanProvider* pool) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ThreadHeap** link = &retired_; *link != nullptr;) {
            ThreadHeap* heap = *link;
            if (heap->provider() == pool) {
                *link = heap->next_retired;
                heap->next_retired = nullptr;
            } else {
                link = &heap->next_retired;
            }
        }
    }

private:
    // Poll quickly while there is work, then back off: a retired heap whose
    // blocks are never freed costs one inbox check per kMaxInterval.
    static constexpr std::chrono::milliseconds kMinInterval{1};
    static constexpr std::chrono::milliseconds kMaxInterval{50};

    void run() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        std::chrono::milliseconds interval = kMinInterval;
        for (;;) {
            if (retired_ == nullptr) {
                wake_.wait(lock, [this] { return retired_ != nullptr; });
                interval = kMinInterval;
            } else {
                wake_.wait_for(lock, interval);
            }
            bool progress = false;
            for (ThreadHeap** link = &retired_; *link != nullptr;) {
                ThreadHeap* heap = *link;
                const PoolCore::Reclaim result =
                    static_cast<PoolCore*>(heap->provider())->reclaim_retired(heap);
                progress = progress || result != PoolCore::Reclaim::kIdle;
                if (result == PoolCore::Reclaim::kRecycled) {
                    // Another thread may adopt the heap as soon as the pool's
                    // lock is released; next_retired is only used here.
                    *link = heap->next_retired;
                    heap->next_retired = nullptr;
                } else {
                    link = &heap->next_retired;
                }
            }
            interval = progress ? kMinInterval : std::min(interval * 2, kMaxInterval);
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    ThreadHeap* retired_ = nullptr;  // linked through ThreadHeap::next_retired
    bool started_ = false;
};

Releaser& releaser() {
    static Releaser* instance = new Releaser();
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
        // Forget every cached heap. thread_local destructors that run after
        // this one find no heap: their allocations fail and their frees go
        // through the owners' inboxes, like frees from any other thread.
        t_state = ThreadState{};
        t_state.thread_id = thread_id;
        t_state.exiting = true;
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
    // After this the reclamation worker no longer touches the arena.
    releaser().forget(this);
    for (ExternalLinks* node = external_; node != nullptr;) {
        ExternalLinks* next = node->next;
        Block* header = reinterpret_cast<Block*>(node + 1);
        const std::size_t alignment = external_alignment(detail::load_word(header->word0));
        const std::size_t front = std::max<std::size_t>(alignment, 32);
        ::operator delete(reinterpret_cast<std::byte*>(node + 1) + kHeaderSize - front,
                          std::align_val_t{alignment});
        node = next;
    }
}

// ---- thread heaps --------------------------------------------------------------

ThreadHeap* PoolCore::current_heap_slow(bool create) noexcept {
    if (t_state.exiting) [[unlikely]] {
        // An exiting thread has no heap and gets none (see allocate_general).
        t_state.slots[id_ & (kTlsSlots - 1)] = TlsSlot{id_, nullptr};
        return nullptr;
    }
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
    // Reuse the heap object of an exited thread that left no span behind. It
    // keeps its counters, which belong to the pool's history.
    for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        if (heap->status() == ThreadHeap::kRecyclable) {
            heap->adopt(thread_id);
            return heap;
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

void PoolCore::release_empty_spans_locked(ThreadHeap* heap) noexcept {
    Span* empty = heap->take_empty_spans();
    while (empty != nullptr) {
        Span* next = empty->next;
        empty->owner = nullptr;
        empty->next = free_spans_;
        free_spans_ = empty;
        empty = next;
    }
    if (heap->spans() == nullptr) {
        heap->set_status(ThreadHeap::kRecyclable);
    }
}

void PoolCore::retire_thread(std::uint64_t thread_id) noexcept {
    ThreadHeap* retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
            if (heap->thread_id() == thread_id && heap->status() == ThreadHeap::kActive) {
                // The owner's last operations: release the frees waiting in
                // the inbox and give completely free spans back to the pool.
                // A heap left without spans holds no block and no pending
                // free, so no other thread can still refer to it.
                heap->set_status(ThreadHeap::kRetired);
                release_empty_spans_locked(heap);
                if (heap->status() == ThreadHeap::kRetired) {
                    retired = heap;
                }
                break;
            }
        }
    }
    // Hand the heap over. Frees that other threads push to its inbox in the
    // meantime simply wait there for the worker.
    if (retired != nullptr) {
        releaser().adopt(retired);
    }
}

PoolCore::Reclaim PoolCore::reclaim_retired(ThreadHeap* heap) noexcept {
    if (!heap->has_pending_frees()) {
        return Reclaim::kIdle;
    }
    heap->drain_pending();
    std::lock_guard<std::mutex> lock(mutex_);
    release_empty_spans_locked(heap);
    return heap->status() == ThreadHeap::kRecyclable ? Reclaim::kRecycled : Reclaim::kProgress;
}

// ---- deallocation paths ------------------------------------------------------------

void PoolCore::deallocate_remote(ThreadHeap* owner, Block* block, std::uint64_t word0,
                                 const void* ptr) noexcept {
    if (owner->pool_id() != id_) {
        detail::fatal_error("pointer was not allocated by this pool", ptr);
    }
    // Whether the owner is running or has exited (its heap then belongs to
    // the reclamation worker), the block goes to the owner's inbox.
    owner->enqueue_remote(block, word0);
}

void* PoolCore::allocate_external(std::size_t bytes, std::size_t alignment) noexcept {
    const std::size_t align = std::max(alignment, detail::kGranule);
    const std::size_t front = std::max<std::size_t>(align, 32);
    if (bytes > capacity_ || bytes >= kExternalSizeMask) {
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
    detail::store_word(header->word0, (detail::address_of(this) |
                                       (log2_align << detail::kAlignShiftPos) |
                                       detail::kStateExternal) ^
                                          kExternalTag);
    detail::store_word(header->word1, detail::kMagic | static_cast<std::uint64_t>(payload) |
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
    const std::uint64_t word1 = detail::load_word(block->word1);
    if (!is_external_of(word0, word1, this)) {
        bad_pointer(ptr);
    }
    const std::size_t align = external_alignment(word0);
    const std::size_t front = std::max<std::size_t>(align, 32);
    const std::size_t payload = external_payload(word1);
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
        const std::uint64_t word1 = detail::load_word(block->word1);
        if (!is_external_of(word0, word1, this)) {
            bad_pointer(ptr);
        }
        return external_payload(word1);
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
        if (address >= payload && address - payload < external_payload(word1)) {
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
    std::uint64_t live_consumed = 0;
    std::uint64_t live_payload = 0;
    std::uint64_t live_requested = 0;
    std::uint64_t allocations = 0;
    std::uint64_t local_frees = 0;
    std::uint64_t remote_received = 0;
    std::uint64_t pending = 0;
    std::uint64_t failed = failed_.load(std::memory_order_relaxed);
    std::uint64_t peak_payload = 0;
    std::uint64_t peak_requested = 0;
    std::uint64_t peak_consumed = 0;
    std::uint64_t owned_span_count = 0;
    std::uint64_t thread_free = 0;
    std::size_t active = 0;
    std::size_t retired = 0;
    for (const ThreadHeap* heap = heaps_; heap != nullptr; heap = heap->next_in_pool) {
        const detail::HeapCounters& c = heap->counters();
        // Under concurrent use the received and drained counts may come from
        // different moments; pending_remote_frees() never goes below zero.
        const std::uint64_t heap_pending = heap->pending_remote_frees();
        const std::uint64_t heap_live = c.live_count();
        const std::uint64_t heap_consumed = c.live_consumed.get();
        const std::uint64_t heap_span_count = c.span_count.get();
        const std::uint64_t heap_span_bytes = c.span_bytes.get();
        live_count += heap_live;
        live_consumed += heap_consumed;
        live_payload += c.live_payload.get();
        live_requested += c.live_requested.get();
        allocations += c.allocations();
        local_frees += c.local_frees.get();
        remote_received += heap->remote_frees_received();
        pending += heap_pending;
        failed += c.failed.get();
        peak_payload += c.peak_payload.get();
        peak_requested += c.peak_requested.get();
        peak_consumed += c.peak_consumed.get();
        stats.split_count += c.splits.get();
        stats.merge_count += c.merges.get();
        stats.fragment_bytes += c.fragment_bytes.get();
        owned_span_count += heap_span_count;
        const std::uint64_t used = heap_span_count * detail::kSpanOverhead + heap_consumed;
        thread_free += heap_span_bytes > used ? heap_span_bytes - used : 0;
        active += heap->status() == ThreadHeap::kActive ? std::size_t{1} : std::size_t{0};
        retired += heap->status() == ThreadHeap::kRetired ? std::size_t{1} : std::size_t{0};
    }
    for (const Span* span = free_spans_; span != nullptr; span = span->next) {
        ++stats.free_span_count;
        stats.free_span_bytes += span->size;
    }

    stats.thread_heap_count = heap_count_;
    stats.active_thread_heap_count = active;
    stats.retired_thread_heap_count = retired;
    stats.span_count = carved_spans_;
    stats.span_bytes = carved_span_bytes_;
    stats.metadata_bytes = heap_count_ * kHeapObjectSize + owned_span_count * detail::kSpanOverhead;
    stats.reserved_bytes = arena_used_ + external_reserved_;
    stats.system_reserved_bytes = sizeof(PoolCore) + capacity_ + external_reserved_;

    stats.live_allocations = live_count + external_live_;
    stats.live_usable_bytes = live_payload + external_payload_;
    stats.live_consumed_bytes = live_consumed + external_reserved_;
    stats.header_bytes = live_count * kHeaderSize + (external_reserved_ - external_payload_);
    stats.pending_remote_frees = pending;
    stats.oversized_live_allocations = external_live_;
    stats.oversized_live_bytes = external_payload_;
    stats.oversized_reserved_bytes = external_reserved_;

    stats.peak_usable_bytes = peak_payload + external_peak_payload_;
    stats.peak_consumed_bytes = peak_consumed + external_peak_consumed_;
    if (track_requested_) {
        stats.live_requested_bytes = live_requested + external_requested_;
        stats.peak_requested_bytes = peak_requested + external_peak_requested_;
    }

    stats.free_bytes =
        capacity_ > stats.live_consumed_bytes ? capacity_ - stats.live_consumed_bytes : 0;
    stats.thread_free_bytes = thread_free;
    ThreadHeap* own = current_heap_if_cached();
    stats.largest_free_block = std::max(own != nullptr ? own->largest_free_payload() : 0,
                                        span_payload_available_locked(own != nullptr));

    stats.allocation_count = allocations + external_allocations_;
    stats.local_deallocation_count = local_frees;
    stats.remote_deallocation_count = remote_received;
    stats.deallocation_count = local_frees + remote_received + external_frees_;
    stats.oversized_allocation_count = external_allocations_;
    stats.failed_allocation_count = failed;
    return stats;
}

bool PoolCore::verify_integrity() const noexcept {
    // Keep the reclamation worker out of the heaps while they are checked.
    std::lock_guard<std::mutex> worker(releaser().mutex());
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
           arena_used_ == heap_count_ * kHeapObjectSize + carved_span_bytes_ &&
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
    return impl_->allocate<detail::OnFailure::kThrow>(bytes, alignment);
}

void* Pool::try_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    return impl_->allocate<detail::OnFailure::kReturnNull>(bytes, alignment);
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
