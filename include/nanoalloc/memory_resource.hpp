// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// std::pmr::memory_resource adapter for nanoalloc::Pool.
#pragma once

#include <cstddef>
#include <memory_resource>

#include "nanoalloc/pool.hpp"

namespace nanoalloc {

/// A std::pmr::memory_resource that allocates from a nanoalloc::Pool.
///
///     nanoalloc::Pool pool({64 * 1024 * 1024});
///     nanoalloc::MemoryResource resource(pool);
///     std::pmr::vector<int> values(&resource);
///
/// The resource does not own the pool. The pool must outlive the resource,
/// and the resource must outlive every container or object using it.
///
/// Errors follow the std::pmr conventions: allocation failure throws
/// std::bad_alloc.
///
/// Threads: the resource is as thread-safe as the Pool. Any thread may
/// allocate through it and deallocate memory obtained through it, also after
/// the allocating thread has exited: a container filled by one thread may be
/// destroyed by another at any time while the pool lives. A thread may not
/// allocate from thread_local destructors that run after the pool has
/// processed its exit (see Pool, Thread lifetime). (The containers themselves
/// still need their usual synchronization.)
///
/// Two MemoryResource objects compare equal when they refer to the same Pool,
/// because memory allocated through one can be released through the other.
class MemoryResource final : public std::pmr::memory_resource {
public:
    explicit MemoryResource(Pool& pool) noexcept : pool_(&pool) {}

    /// The pool this resource allocates from.
    [[nodiscard]] Pool& pool() const noexcept { return *pool_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return pool_->allocate(bytes, alignment);
    }

    void do_deallocate(void* ptr, std::size_t /*bytes*/,
                       std::size_t /*alignment*/) override {
        // The pool recovers the block size itself; the size hint is unused.
        pool_->deallocate(ptr);
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        if (this == &other) {
            return true;
        }
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
        const auto* resource = dynamic_cast<const MemoryResource*>(&other);
        return resource != nullptr && resource->pool_ == pool_;
#else
        // Without RTTI we cannot identify another MemoryResource safely;
        // reporting "not equal" is always correct (containers then copy
        // instead of stealing storage).
        return false;
#endif
    }

    Pool* pool_;
};

}  // namespace nanoalloc
