// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Public contract test: after construction, Pool operations never call the
// global allocation functions. This executable replaces every global
// operator new/delete with counting versions that forward to the C runtime.
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace {

std::atomic<std::size_t> g_new_calls{0};

void* counted_alloc(std::size_t size) {
    ++g_new_calls;
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void* counted_aligned_alloc(std::size_t size, std::align_val_t alignment) {
    ++g_new_calls;
    const auto align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1) / align * align;
#if defined(_MSC_VER)
    void* ptr = _aligned_malloc(rounded == 0 ? align : rounded, align);
#else
    void* ptr = std::aligned_alloc(align, rounded == 0 ? align : rounded);
#endif
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void aligned_free(void* ptr) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return counted_aligned_alloc(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return counted_aligned_alloc(size, alignment);
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { aligned_free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { aligned_free(ptr); }

namespace {

/// Runs a mixed workload through the public API and returns how many global
/// allocations happened during it.
std::size_t churn(nanoalloc::Pool& pool, std::vector<void*>& live) {
    nanotest::Rng rng(12345);
    const std::size_t before = g_new_calls.load();
    for (int step = 0; step < 100000; ++step) {
        if (live.size() < live.capacity() && (live.empty() || rng.chance(55))) {
            const std::size_t size = rng.chance(2) ? 0 : rng.log_uniform(1, 8192);
            const std::size_t alignment = std::size_t{1} << rng.below(13);
            void* ptr = pool.try_allocate(size, alignment);
            if (ptr != nullptr) {
                (void)pool.usable_size(ptr);
                live.push_back(ptr);  // capacity reserved up front: no reallocation
            }
        } else {
            const std::size_t victim = rng.below(live.size());
            pool.deallocate(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
        if (step % 1000 == 0) {
            (void)pool.stats();
            (void)pool.owns(live.empty() ? nullptr : live.front());
            (void)pool.try_allocate(pool.capacity() * 2);  // a failing request
            (void)pool.try_allocate(16, 3);                 // an invalid alignment
        }
    }
    for (void* ptr : live) {
        pool.deallocate(ptr);
    }
    live.clear();
    return g_new_calls.load() - before;
}

}  // namespace

NANO_TEST(replacement_operator_new_is_active) {
    const std::size_t before = g_new_calls.load();
    delete new int(1);
    CHECK_EQ(g_new_calls.load() - before, 1U);
}

NANO_TEST(pool_operations_do_not_allocate) {
    nanoalloc::Pool pool({4 * 1024 * 1024});  // construction may allocate

    std::vector<void*> live;
    live.reserve(4096);
    CHECK_EQ(churn(pool, live), 0U);
}

NANO_TEST(requested_size_tracking_does_not_allocate_per_operation) {
    nanoalloc::PoolConfig config;
    config.capacity_bytes = 4 * 1024 * 1024;
    config.track_requested_bytes = true;
    nanoalloc::Pool pool(config);
    std::vector<void*> live;
    live.reserve(4096);
    CHECK_EQ(churn(pool, live), 0U);
}

NANO_TEST(memory_resource_does_not_allocate_per_operation) {
    nanoalloc::Pool pool({1024 * 1024});
    nanoalloc::MemoryResource resource(pool);
    const std::size_t before = g_new_calls.load();
    for (int i = 0; i < 10000; ++i) {
        void* ptr = resource.allocate(static_cast<std::size_t>(16 + i % 500), 16);
        resource.deallocate(ptr, static_cast<std::size_t>(16 + i % 500), 16);
    }
    CHECK_EQ(g_new_calls.load() - before, 0U);
}

NANOTEST_MAIN()
