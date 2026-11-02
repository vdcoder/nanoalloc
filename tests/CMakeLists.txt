# nanoalloc test suite. Self-contained: no downloaded dependencies.

# --- Public contract tests ---------------------------------------------------
# Valid for any allocation engine behind the Pool API.
set(NANOALLOC_CONTRACT_TESTS
    test_pool
    test_stress
    test_pmr
    test_allocator
    test_no_heap)

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

# --- Buddy engine implementation tests -----------------------------------------
# Compiled directly against the library sources so they can reach the internal
# engine regardless of how the library itself is built (static or shared).
add_executable(test_buddy
    test_buddy.cpp
    ${PROJECT_SOURCE_DIR}/src/buddy_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/diagnostics.cpp
    ${PROJECT_SOURCE_DIR}/src/pool.cpp)
target_include_directories(test_buddy PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
target_compile_features(test_buddy PRIVATE cxx_std_20)
nanoalloc_apply_warnings(test_buddy)
add_test(NAME engine.test_buddy COMMAND test_buddy)
set_tests_properties(engine.test_buddy PROPERTIES LABELS "engine")

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
    "usable_size_after_free\;not allocated")
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
    foreach(scenario asan_use_after_free asan_overflow_into_free_block)
        add_test(NAME misuse.${scenario} COMMAND test_misuse ${scenario})
        set_tests_properties(misuse.${scenario} PROPERTIES
            LABELS "contract;misuse;sanitizer"
            PASS_REGULAR_EXPRESSION "AddressSanitizer: use-after-poison")
    endforeach()
endif()
