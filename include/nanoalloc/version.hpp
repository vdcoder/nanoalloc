// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#pragma once

#define NANOALLOC_VERSION_MAJOR 0
#define NANOALLOC_VERSION_MINOR 1
#define NANOALLOC_VERSION_PATCH 0
#define NANOALLOC_VERSION_STRING "0.1.0"

namespace nanoalloc {

inline constexpr int version_major = NANOALLOC_VERSION_MAJOR;
inline constexpr int version_minor = NANOALLOC_VERSION_MINOR;
inline constexpr int version_patch = NANOALLOC_VERSION_PATCH;
inline constexpr const char* version_string = NANOALLOC_VERSION_STRING;

}  // namespace nanoalloc
