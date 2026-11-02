// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Standard Allocator adapter for nanoalloc::Pool.
#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

#include "nanoalloc/pool.hpp"

namespace nanoalloc {

/// A C++20 Allocator that draws memory from a nanoalloc::Pool.
///
///     nanoalloc::Pool pool({1 << 20});
///     std::vector<int, nanoalloc::Allocator<int>> values{
///         nanoalloc::Allocator<int>(pool)};
///
/// Semantics:
///   * The allocator is a lightweight, non-owning handle (one pointer) to a
///     Pool. The pool must outlive every container using the allocator.
///   * It is not default-constructible: a pool must always be named.
///   * Two allocators compare equal iff they refer to the same Pool, across
///     all value types (rebinding preserves the pool).
///   * Containers propagate the allocator on copy assignment, move assignment
///     and swap, so a container's storage always comes from the pool its
///     allocator names, and move assignment never has to copy elements.
///   * Storage is aligned to max(alignof(T), Pool::min_alignment). Types with
///     alignof(T) > Pool::max_alignment are rejected at compile time.
///   * allocate() throws std::bad_array_new_length if n * sizeof(T)
///     overflows, and std::bad_alloc if the pool cannot satisfy the request.
///   * Not thread-safe, like the Pool itself.
template <class T>
class Allocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    explicit Allocator(Pool& pool) noexcept : pool_(&pool) {}

    template <class U>
    Allocator(const Allocator<U>& other) noexcept  // NOLINT: implicit by design
        : pool_(&other.pool()) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        static_assert(alignof(T) <= Pool::max_alignment,
                      "nanoalloc::Allocator: alignof(T) exceeds "
                      "Pool::max_alignment");
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        constexpr std::size_t alignment = alignof(T) > Pool::min_alignment
                                              ? alignof(T)
                                              : Pool::min_alignment;
        return static_cast<T*>(pool_->allocate(n * sizeof(T), alignment));
    }

    void deallocate(T* ptr, std::size_t /*n*/) noexcept {
        // The pool recovers the block size itself; `n` is not needed.
        pool_->deallocate(ptr);
    }

    /// The pool this allocator draws from.
    [[nodiscard]] Pool& pool() const noexcept { return *pool_; }

private:
    Pool* pool_;
};

template <class T, class U>
[[nodiscard]] bool operator==(const Allocator<T>& lhs,
                              const Allocator<U>& rhs) noexcept {
    return &lhs.pool() == &rhs.pool();
}

}  // namespace nanoalloc
