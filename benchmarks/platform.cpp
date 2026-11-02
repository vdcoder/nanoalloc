// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
#include "platform.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "nanobench_build_flags.hpp"
#include "nanobench_build_type.hpp"

#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#include <malloc.h>
#endif
#if defined(_WIN32)
#include <malloc.h>
#endif
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define NANOBENCH_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace nanobench {

namespace {

std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown compiler";
#endif
}

/// Flags CMake adds for `build_type` (configuration names are case-insensitive).
std::string configuration_flags(const std::string& build_type) {
    struct Entry {
        const char* configuration;
        const char* flags;
    };
    static const Entry entries[] = {
        {"debug", NANOBENCH_CXX_FLAGS_DEBUG},
        {"release", NANOBENCH_CXX_FLAGS_RELEASE},
        {"relwithdebinfo", NANOBENCH_CXX_FLAGS_RELWITHDEBINFO},
        {"minsizerel", NANOBENCH_CXX_FLAGS_MINSIZEREL},
    };
    std::string lowered = build_type;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    for (const Entry& entry : entries) {
        if (lowered == entry.configuration) {
            return entry.flags;
        }
    }
    return {};
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, last - first + 1);
}

std::string cpu_name() {
#if defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                return trim(line.substr(colon + 1));
            }
        }
    }
#endif
#if defined(NANOBENCH_X86)
    unsigned int regs[12] = {};
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, static_cast<int>(0x80000000U));
    if (static_cast<unsigned int>(info[0]) >= 0x80000004U) {
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            __cpuid(info, static_cast<int>(0x80000002U + leaf));
            std::memcpy(regs + leaf * 4, info, sizeof(info));
        }
    }
#else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000U, &eax, &ebx, &ecx, &edx) != 0 && eax >= 0x80000004U) {
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            __get_cpuid(0x80000002U + leaf, &regs[leaf * 4], &regs[leaf * 4 + 1],
                        &regs[leaf * 4 + 2], &regs[leaf * 4 + 3]);
        }
    }
#endif
    char brand[sizeof(regs) + 1] = {};
    std::memcpy(brand, regs, sizeof(regs));
    const std::string name = trim(brand);
    if (!name.empty()) {
        return name;
    }
#endif
    return "unknown CPU";
}

std::string os_name() {
#if defined(__unix__) || defined(__APPLE__)
    utsname name{};
    if (uname(&name) == 0) {
        return std::string(name.sysname) + " " + name.release + " " + name.machine;
    }
    return "Unix";
#elif defined(_WIN32)
#if defined(_M_ARM64)
    return "Windows arm64";
#elif defined(_M_X64)
    return "Windows x64";
#else
    return "Windows";
#endif
#else
    return "unknown OS";
#endif
}

std::string system_allocator_name() {
#if defined(__GLIBC__)
    return std::string("glibc ") + gnu_get_libc_version() + " malloc/free";
#elif defined(_WIN32)
    return "MSVC CRT malloc/free";
#elif defined(__APPLE__)
    return "macOS libmalloc malloc/free";
#else
    return "C runtime malloc/free";
#endif
}

}  // namespace

PlatformInfo platform_info() {
    PlatformInfo info;
    info.compiler = compiler_name();
    info.build_type = NANOBENCH_BUILD_TYPE;
    info.cxx_flags =
        trim(std::string(NANOBENCH_CXX_FLAGS) + " " + configuration_flags(info.build_type));
    info.sanitizers = NANOBENCH_SANITIZE;
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && !defined(_DEBUG))
    info.optimized = true;
#endif
#if !defined(NDEBUG)
    info.assertions = true;
#endif
    info.os = os_name();
    info.cpu = cpu_name();
    info.logical_cpus = std::thread::hardware_concurrency();
    info.system_allocator = system_allocator_name();
    return info;
}

bool system_usable_size_available() {
#if defined(__GLIBC__) || defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::size_t system_usable_size(void* ptr) {
#if defined(__GLIBC__)
    return malloc_usable_size(ptr);
#elif defined(_WIN32)
    return _msize(ptr);
#elif defined(__APPLE__)
    return malloc_size(ptr);
#else
    (void)ptr;
    return 0;
#endif
}

}  // namespace nanobench
