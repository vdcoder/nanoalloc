// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// A minimal, dependency-free unit test harness.
//
//   NANO_TEST(name) { CHECK(...); REQUIRE_EQ(a, b); }
//
// CHECK* records a failure and continues; REQUIRE* records a failure and
// aborts the current test case. Each test executable runs all registered
// cases; pass a substring as the first argument to run a subset, or --list to
// print the case names.
#pragma once

#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace nanotest {

struct TestCase {
    const char* name;
    void (*function)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back({name, function}); }
};

/// Thrown by REQUIRE* to abandon the current test case.
struct AbortTest {};

inline int& current_failures() {
    static int failures = 0;
    return failures;
}

inline void report_failure(const char* file, int line, const std::string& message) {
    ++current_failures();
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
}

template <class T, class = void>
struct is_streamable : std::false_type {};
template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, std::nullptr_t>) {
        return "nullptr";
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (is_streamable<T>::value) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<unprintable>";
    }
}

/// Integer types accepted by std::cmp_equal (excludes bool and character types).
template <class T>
inline constexpr bool is_comparable_integer_v =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, signed char> && !std::is_same_v<T, unsigned char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
    !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;

template <class A, class B>
bool check_equal(const A& lhs, const B& rhs, const char* lhs_text, const char* rhs_text,
                 const char* file, int line) {
    if constexpr (is_comparable_integer_v<A> && is_comparable_integer_v<B>) {
        if (std::cmp_equal(lhs, rhs)) {
            return true;
        }
    } else {
        if (lhs == rhs) {
            return true;
        }
    }
    report_failure(file, line,
                   std::string("expected ") + lhs_text + " == " + rhs_text + ", got " +
                       describe(lhs) + " vs " + describe(rhs));
    return false;
}

inline int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (filter != nullptr && std::strcmp(filter, "--list") == 0) {
        for (const TestCase& test : registry()) {
            std::printf("%s\n", test.name);
        }
        return 0;
    }
    int failed_cases = 0;
    int run_cases = 0;
    for (const TestCase& test : registry()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }
        ++run_cases;
        current_failures() = 0;
        std::printf("[ RUN      ] %s\n", test.name);
        std::fflush(stdout);
        try {
            test.function();
        } catch (const AbortTest&) {
            // Failure already reported.
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__,
                           std::string("unexpected exception: ") + error.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        if (current_failures() == 0) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            std::printf("[  FAILED  ] %s (%d failure(s))\n", test.name, current_failures());
            ++failed_cases;
        }
        std::fflush(stdout);
    }
    std::printf("%d test case(s) run, %d failed\n", run_cases, failed_cases);
    if (run_cases == 0) {
        std::fprintf(stderr, "no test case matched the filter\n");
        return 1;
    }
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace nanotest

#define NANOTEST_CONCAT_INNER(a, b) a##b
#define NANOTEST_CONCAT(a, b) NANOTEST_CONCAT_INNER(a, b)

#define NANO_TEST(name)                                                              \
    static void name();                                                              \
    static const ::nanotest::Registrar NANOTEST_CONCAT(nanotest_registrar_, name){#name, \
                                                                                &name}; \
    static void name()

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ::nanotest::report_failure(__FILE__, __LINE__, "CHECK(" #condition ") failed"); \
        }                                                                             \
    } while (false)

#define REQUIRE(condition)                                                            \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ::nanotest::report_failure(__FILE__, __LINE__,                            \
                                       "REQUIRE(" #condition ") failed");             \
            throw ::nanotest::AbortTest{};                                            \
        }                                                                             \
    } while (false)

#define CHECK_EQ(lhs, rhs) \
    ((void)::nanotest::check_equal((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__))

#define REQUIRE_EQ(lhs, rhs)                                                          \
    do {                                                                              \
        if (!::nanotest::check_equal((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__)) { \
            throw ::nanotest::AbortTest{};                                            \
        }                                                                             \
    } while (false)

#define CHECK_THROWS_AS(expression, exception_type)                                   \
    do {                                                                              \
        bool nanotest_caught_ = false;                                                \
        try {                                                                         \
            (void)(expression);                                                       \
        } catch (const exception_type&) {                                             \
            nanotest_caught_ = true;                                                  \
        } catch (...) {                                                               \
        }                                                                             \
        if (!nanotest_caught_) {                                                      \
            ::nanotest::report_failure(__FILE__, __LINE__,                            \
                                       "expected " #expression " to throw " #exception_type); \
        }                                                                             \
    } while (false)

#define CHECK_NOTHROW(expression)                                                     \
    do {                                                                              \
        try {                                                                         \
            (void)(expression);                                                       \
        } catch (...) {                                                               \
            ::nanotest::report_failure(__FILE__, __LINE__,                            \
                                       "expected " #expression " not to throw");     \
        }                                                                             \
    } while (false)

#define NANOTEST_MAIN()                                                              \
    int main(int argc, char** argv) { return ::nanotest::run_all(argc, argv); }
