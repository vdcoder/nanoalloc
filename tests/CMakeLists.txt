# nanoalloc test suite. Self-contained: no downloaded dependencies.

# --- Public contract tests ---------------------------------------------------
# Use only the public API; no dependence on implementation details.
set(NANOALLOC_CONTRACT_TESTS
    test_pool
    test_stress
    test_pmr
    test_allocator
    test_threads)
# test_no_heap replaces the global operator new, which ThreadSanitizer's
# runtime already defines; it cannot be linked in a TSan build.
string(REPLACE "," ";" _nanoalloc_test_sanitizers "${NANOALLOC_SANITIZE}")
if(NOT "thread" IN_LIST _nanoalloc_test_sanitizers)
    list(APPEND NANOALLOC_CONTRACT_TESTS test_no_heap)
endif()

foreach(test_name IN LISTS NANOALLOC_CONTRACT_TESTS)
    add_executable(${test_name} ${test_name}.cpp test_framework.hpp test_helpers.hpp)
    target_link_libraries(${test_name} PRIVATE nanoalloc::nanoalloc)
    target_compile_definitions(${test_name} PRIVATE
        NANOALLOC_EXPECTED_VERSION="${PROJECT_VERSION}")
    nanoalloc_apply_warnings(${test_name})
    add_test(NAME contract.${test_name} COMMAND ${test_name})
    set_tests_properties(contract.${test_name} PROPERTIES LABELS "contract")
endforeach()
set_tests_properties(contract.test_stress PROPERTIES LABELS "contract;stress" TIMEOUT 1500)
set_tests_properties(contract.test_threads PROPERTIES LABELS "contract;threads" TIMEOUT 1500)

# --- Engine implementation tests ------------------------------------------------
# Compiled directly against the library sources so they can reach the internal
# engine regardless of how the library itself is built (static or shared).
add_executable(test_engine test_engine.cpp ${NANOALLOC_SOURCES_ABSOLUTE})
target_include_directories(test_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
target_compile_features(test_engine PRIVATE cxx_std_20)
target_link_libraries(test_engine PRIVATE Threads::Threads)
nanoalloc_apply_warnings(test_engine)
add_test(NAME engine.test_engine COMMAND test_engine)
set_tests_properties(engine.test_engine PROPERTIES LABELS "engine")

# --- Misuse detection ------------------------------------------------------------
# Each scenario must terminate with nanoalloc's fatal-error diagnostic.
add_executable(test_misuse test_misuse.cpp)
target_link_libraries(test_misuse PRIVATE nanoalloc::nanoalloc)
nanoalloc_apply_warnings(test_misuse)

set(NANOALLOC_MISUSE_SCENARIOS
    "double_free\;double free"
    "double_free_unmerged\;double free"
    "foreign_pointer\;not allocated by this pool"
    "other_pool\;not allocated by this pool"
    "interior_pointer\;does not refer to the start of an allocated block"
    "misaligned_pointer\;not allocated by this pool"
    "usable_size_after_free\;not allocated"
    "free_after_owner_exit\;owning thread has exited")
foreach(entry IN LISTS NANOALLOC_MISUSE_SCENARIOS)
    list(GET entry 0 scenario)
    list(GET entry 1 message)
    add_test(NAME misuse.${scenario} COMMAND test_misuse ${scenario})
    set_tests_properties(misuse.${scenario} PROPERTIES
        LABELS "contract;misuse"
        PASS_REGULAR_EXPRESSION "nanoalloc: fatal error: [^\n]*${message}")
endforeach()

# With AddressSanitizer, memory in the arena that is not part of a live
# allocation is poisoned; ASan itself must report these accesses.
string(REPLACE "," ";" _nanoalloc_sanitizer_list "${NANOALLOC_SANITIZE}")
if("address" IN_LIST _nanoalloc_sanitizer_list)
    # Deallocation reads the 16-byte header in front of the pointer. For a
    # pointer to a stack variable that read is out of bounds, and ASan reports
    # the misuse before the pool's own check does.
    set_tests_properties(misuse.foreign_pointer PROPERTIES
        PASS_REGULAR_EXPRESSION
        "nanoalloc: fatal error: [^\n]*not allocated by this pool|AddressSanitizer: stack-buffer-(overflow|underflow)")
    foreach(scenario asan_use_after_free asan_overflow_into_free_block)
        add_test(NAME misuse.${scenario} COMMAND test_misuse ${scenario})
        set_tests_properties(misuse.${scenario} PROPERTIES
            LABELS "contract;misuse;sanitizer"
            PASS_REGULAR_EXPRESSION "AddressSanitizer: use-after-poison")
    endforeach()
endif()
