# nanoalloc benchmark executable. No third-party dependencies.

add_executable(nanoalloc_bench
    benchmark.cpp
    platform.cpp
    platform.hpp
    workloads.cpp
    workloads.hpp)
target_link_libraries(nanoalloc_bench PRIVATE nanoalloc::nanoalloc)
nanoalloc_apply_warnings(nanoalloc_bench)

# Record the build configuration so that reports state how the binary was built.
set(_nanoalloc_config_flags "")
foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_config} _config_upper)
    string(APPEND _nanoalloc_config_flags
        "$<$<CONFIG:${_config}>:${CMAKE_CXX_FLAGS_${_config_upper}}>")
endforeach()
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/nanobench_build_info.hpp
    CONTENT "#pragma once
#define NANOBENCH_BUILD_TYPE \"$<CONFIG>\"
#define NANOBENCH_CXX_FLAGS \"${CMAKE_CXX_FLAGS} ${_nanoalloc_config_flags}\"
#define NANOBENCH_SANITIZE \"${NANOALLOC_SANITIZE}\"
")
target_include_directories(nanoalloc_bench PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>)

# A short smoke run so CI verifies that the benchmark builds and executes.
if(BUILD_TESTING)
    add_test(NAME benchmark.smoke
        COMMAND nanoalloc_bench --scale 0.02 --iterations 1 --warmup 0)
    set_tests_properties(benchmark.smoke PROPERTIES LABELS "benchmark" TIMEOUT 600)
endif()
