// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Internal: fatal error reporting and sanitizer integration.
#pragma once

#include <cstddef>

#if defined(__SANITIZE_ADDRESS__)
#define NANOALLOC_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NANOALLOC_HAS_ASAN 1
#endif
#endif

#if defined(NANOALLOC_HAS_ASAN)
#if defined(__has_include)
#if __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#define NANOALLOC_HAS_ASAN_INTERFACE_HEADER 1
#endif
#endif
#if !defined(NANOALLOC_HAS_ASAN_INTERFACE_HEADER)
// Stable public ASan entry points, declared directly when the header is absent.
extern "C" void __asan_poison_memory_region(void const volatile* addr, std::size_t size);
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, std::size_t size);
#define ASAN_POISON_MEMORY_REGION(addr, size) __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region((addr), (size))
#endif
#endif

namespace nanoalloc::detail {

/// Prints "nanoalloc: fatal error: <message> (pointer <ptr>)" to stderr and
/// aborts. Used for detected misuse that cannot be reported by exception
/// (deallocate() is noexcept) and must not be allowed to corrupt the pool.
[[noreturn]] void fatal_error(const char* message, const void* ptr) noexcept;

// When AddressSanitizer is active, memory inside the arena that is not part of
// a live allocation is poisoned, so use-after-free and overflows into free
// blocks are reported by ASan even though the arena is a single system
// allocation. In other builds these functions compile to nothing.
inline void poison_memory(const void* ptr, std::size_t bytes) noexcept {
#if defined(NANOALLOC_HAS_ASAN)
    ASAN_POISON_MEMORY_REGION(ptr, bytes);
#else
    (void)ptr;
    (void)bytes;
#endif
}

inline void unpoison_memory(const void* ptr, std::size_t bytes) noexcept {
#if defined(NANOALLOC_HAS_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(ptr, bytes);
#else
    (void)ptr;
    (void)bytes;
#endif
}

}  // namespace nanoalloc::detail
