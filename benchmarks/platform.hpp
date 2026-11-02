// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Platform queries used by the benchmark: build/host description and
// optional introspection of the system allocator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nanobench {

struct PlatformInfo {
    std::string compiler;
    std::string build_type;
    std::string cxx_flags;
    std::string sanitizers;
    bool optimized = false;   // compiled with optimization enabled
    bool assertions = false;  // NDEBUG not defined
    std::string os;
    std::string cpu;
    unsigned logical_cpus = 0;
    std::string system_allocator;
};

PlatformInfo platform_info();

/// Usable size of a block returned by std::malloc, when the C runtime can
/// report it (glibc malloc_usable_size, MSVC _msize, macOS malloc_size).
bool system_usable_size_available();
std::size_t system_usable_size(void* ptr);

}  // namespace nanobench
