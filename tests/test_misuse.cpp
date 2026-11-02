// nanoalloc - memory-efficient, high-performance C++ allocation
// SPDX-License-Identifier: MIT
//
// Misuse detection. Each scenario performs one invalid operation that the
// pool is documented to detect; the process must terminate with a
// "nanoalloc: fatal error" diagnostic. CTest checks the diagnostic text, so a
// scenario that returns normally fails.
#include <nanoalloc/pool.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// fatal_error() ends with std::abort(). Exit normally from the SIGABRT handler
// so that the test outcome is decided by the diagnostic text alone (CTest
// reports a process killed by a signal as failed regardless of its output).
extern "C" void exit_after_abort(int) { std::_Exit(0); }

int run(const char* scenario) {
    nanoalloc::Pool pool({64 * 1024});
    if (std::strcmp(scenario, "double_free") == 0) {
        void* ptr = pool.allocate(64);
        pool.deallocate(ptr);
        pool.deallocate(ptr);
    } else if (std::strcmp(scenario, "double_free_unmerged") == 0) {
        void* keep = pool.allocate(64);
        void* ptr = pool.allocate(64);
        void* neighbor = pool.allocate(64);
        pool.deallocate(ptr);
        pool.deallocate(ptr);
        pool.deallocate(keep);
        pool.deallocate(neighbor);
    } else if (std::strcmp(scenario, "foreign_pointer") == 0) {
        int local = 0;
        pool.deallocate(&local);
    } else if (std::strcmp(scenario, "pool_address_before_pointer") == 0) {
        // The 16 bytes in front of the pointer hold a copy of the Pool object
        // (its implementation pointer), which must not pass for a header.
        alignas(16) unsigned char buffer[64] = {};
        static_assert(sizeof(pool) <= 16);
        std::memcpy(buffer, &pool, sizeof(pool));
        pool.deallocate(buffer + 16);
    } else if (std::strcmp(scenario, "other_pool") == 0) {
        nanoalloc::Pool other({64 * 1024});
        void* ptr = other.allocate(32);
        pool.deallocate(ptr);
    } else if (std::strcmp(scenario, "interior_pointer") == 0) {
        void* ptr = pool.allocate(256);
        pool.deallocate(static_cast<char*>(ptr) + 64);
    } else if (std::strcmp(scenario, "misaligned_pointer") == 0) {
        void* ptr = pool.allocate(256);
        pool.deallocate(static_cast<char*>(ptr) + 1);
    } else if (std::strcmp(scenario, "asan_use_after_free") == 0) {
        // Only meaningful under AddressSanitizer: released blocks are poisoned.
        auto* keep = static_cast<char*>(pool.allocate(256));
        auto* ptr = static_cast<char*>(pool.allocate(256));
        pool.deallocate(ptr);
        ptr[100] = 1;
        pool.deallocate(keep);
    } else if (std::strcmp(scenario, "asan_overflow_into_free_block") == 0) {
        // Blocks are carved downwards: low_block is followed by victim's
        // 16-byte header and then victim's (freed, poisoned) payload.
        auto* keep = static_cast<char*>(pool.allocate(64));
        auto* victim = static_cast<char*>(pool.allocate(64));
        auto* low_block = static_cast<char*>(pool.allocate(64));
        pool.deallocate(victim);
        low_block[pool.usable_size(low_block) + 16] = 1;
        pool.deallocate(low_block);
        pool.deallocate(keep);
    } else if (std::strcmp(scenario, "usable_size_after_free") == 0) {
        void* ptr = pool.allocate(100);
        pool.deallocate(ptr);
        std::printf("%zu\n", pool.usable_size(ptr));
    } else {
        std::fprintf(stderr, "unknown scenario '%s'\n", scenario);
        return 2;
    }
    std::fprintf(stderr, "scenario '%s' was not detected\n", scenario);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
    // Report abort() on stderr only, without interactive dialogs.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::signal(SIGABRT, exit_after_abort);
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_misuse <scenario>\n");
        return 2;
    }
    return run(argv[1]);
}
