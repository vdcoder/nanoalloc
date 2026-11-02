cmake_minimum_required(VERSION 3.20)

project(nanoalloc
    VERSION 0.1.0
    DESCRIPTION "Memory-efficient, high-performance C++20 allocator"
    HOMEPAGE_URL "https://github.com/vdcoder/nanoalloc"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NANOALLOC_IS_TOP_LEVEL ON)
else()
    set(NANOALLOC_IS_TOP_LEVEL OFF)
endif()

option(NANOALLOC_BUILD_TESTS "Build the nanoalloc test suite" ${NANOALLOC_IS_TOP_LEVEL})
option(NANOALLOC_BUILD_BENCHMARKS "Build the nanoalloc benchmark executable" ${NANOALLOC_IS_TOP_LEVEL})
option(NANOALLOC_BUILD_EXAMPLES "Build the nanoalloc examples" ${NANOALLOC_IS_TOP_LEVEL})
option(NANOALLOC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(NANOALLOC_INSTALL "Generate install rules" ${NANOALLOC_IS_TOP_LEVEL})
set(NANOALLOC_SANITIZE "" CACHE STRING
    "Sanitizers to enable for all nanoalloc targets, e.g. 'address;undefined'")

# Default to an optimized build when nothing was requested (single-config generators).
get_property(NANOALLOC_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NANOALLOC_IS_TOP_LEVEL AND NOT NANOALLOC_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "nanoalloc: CMAKE_BUILD_TYPE not set, defaulting to Release")
endif()

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NanoallocOptions)

# Sanitizers apply to every target in this project (library, tests,
# benchmarks, examples) so that instrumented code is linked consistently.
nanoalloc_enable_sanitizers()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(nanoalloc
    src/buddy_engine.cpp
    src/buddy_engine.hpp
    src/diagnostics.cpp
    src/diagnostics.hpp
    src/pool.cpp)
add_library(nanoalloc::nanoalloc ALIAS nanoalloc)

target_compile_features(nanoalloc PUBLIC cxx_std_20)
target_include_directories(nanoalloc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(nanoalloc PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
if(BUILD_SHARED_LIBS)
    target_compile_definitions(nanoalloc PUBLIC NANOALLOC_SHARED PRIVATE NANOALLOC_BUILDING)
endif()
nanoalloc_apply_strict_warnings(nanoalloc)
# An instrumented library can only be linked together with the sanitizer
# runtimes. The directory-wide options above do not reach parent projects
# (add_subdirectory) or installed-package users, so publish them on the target.
if(NANOALLOC_SANITIZER_LINK_OPTIONS)
    target_link_options(nanoalloc INTERFACE ${NANOALLOC_SANITIZER_LINK_OPTIONS})
endif()

# ---------------------------------------------------------------------------
# Tests, benchmarks, examples
# ---------------------------------------------------------------------------
if(NANOALLOC_BUILD_TESTS OR NANOALLOC_BUILD_EXAMPLES)
    include(CTest)
endif()

if(NANOALLOC_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NANOALLOC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(NANOALLOC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Installation and package export (find_package(nanoalloc))
# ---------------------------------------------------------------------------
if(NANOALLOC_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS nanoalloc
        EXPORT nanoallocTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/nanoalloc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT nanoallocTargets
        NAMESPACE nanoalloc::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanoalloc)

    configure_package_config_file(
        cmake/nanoallocConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/nanoallocConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanoalloc)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/nanoallocConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/nanoallocConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/nanoallocConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanoalloc)
endif()
