// nanoalloc evaluation driver: shared helpers (JSON output, compiler info).
// Part of the evaluation package; independent of any allocator implementation.
#pragma once

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace nanoeval {

/// Escapes a string for a JSON string literal.
inline std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

inline std::string hex64(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

/// The compiler that built this driver (and, in the same build, the
/// implementation under test).
inline std::string compiler_description() {
    std::ostringstream out;
#if defined(__clang__)
    out << "Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#elif defined(__GNUC__)
    out << "GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
    out << "MSVC " << _MSC_FULL_VER;
#else
    out << "unknown compiler";
#endif
    return out.str();
}

/// Keeps `value` (and the work producing it) from being optimized away.
template <class T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    static const void* volatile sink = nullptr;
    sink = &value;
#endif
}

}  // namespace nanoeval
