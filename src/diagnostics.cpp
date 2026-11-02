// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "diagnostics.hpp"

#include <cstdio>
#include <cstdlib>

namespace nanoalloc::detail {

void fatal_error(const char* message, const void* ptr) noexcept {
    std::fprintf(stderr, "nanoalloc: fatal error: %s (pointer %p)\n", message,
                 ptr);
    std::fflush(stderr);
    std::abort();
}

}  // namespace nanoalloc::detail
