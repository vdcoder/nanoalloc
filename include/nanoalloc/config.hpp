// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Build configuration shared by all public headers.
#pragma once

// NANOALLOC_API marks symbols that are part of the library's binary interface.
// It only has an effect when nanoalloc is built as a shared library
// (BUILD_SHARED_LIBS=ON), in which case the CMake target defines
// NANOALLOC_SHARED for consumers and NANOALLOC_BUILDING while compiling the
// library itself.
#if defined(NANOALLOC_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(NANOALLOC_BUILDING)
#define NANOALLOC_API __declspec(dllexport)
#else
#define NANOALLOC_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define NANOALLOC_API __attribute__((visibility("default")))
#else
#define NANOALLOC_API
#endif
#else
#define NANOALLOC_API
#endif
