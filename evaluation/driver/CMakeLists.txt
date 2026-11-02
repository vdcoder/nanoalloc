# nanoalloc evaluation driver.
#
# Builds ONE implementation tree together with the trusted drivers of the
# evaluation package. The implementation is compiled from its sources with the
# flags of this project, so that every implementation evaluated with the same
# toolchain is built the same way:
#   NANOEVAL_IMPL_DIR/include   public headers (nanoalloc/pool.hpp, ...)
#   NANOEVAL_IMPL_DIR/src       implementation: every *.cpp below it is compiled
# The implementation's own CMakeLists.txt is not used here (the evaluator
# builds it separately to run the implementation's own tests).
cmake_minimum_required(VERSION 3.20)
project(nanoeval_driver LANGUAGES CXX)

set(NANOEVAL_IMPL_DIR "" CACHE PATH "Implementation source tree (include/ and src/)")
if(NOT IS_DIRECTORY "${NANOEVAL_IMPL_DIR}/include/nanoalloc" OR
   NOT IS_DIRECTORY "${NANOEVAL_IMPL_DIR}/src")
    message(FATAL_ERROR "NANOEVAL_IMPL_DIR must contain include/nanoalloc/ and src/: "
                        "'${NANOEVAL_IMPL_DIR}'")
endif()
file(GLOB_RECURSE NANOEVAL_IMPL_SOURCES CONFIGURE_DEPENDS "${NANOEVAL_IMPL_DIR}/src/*.cpp")
if(NOT NANOEVAL_IMPL_SOURCES)
    message(FATAL_ERROR "no .cpp files under ${NANOEVAL_IMPL_DIR}/src")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)

add_library(nanoeval_impl STATIC ${NANOEVAL_IMPL_SOURCES})
target_include_directories(nanoeval_impl
    PUBLIC "${NANOEVAL_IMPL_DIR}/include"
    PRIVATE "${NANOEVAL_IMPL_DIR}/src")
target_link_libraries(nanoeval_impl PUBLIC Threads::Threads)

add_executable(nanoeval_timing timing.cpp workloads.cpp workloads.hpp common.hpp)
target_link_libraries(nanoeval_timing PRIVATE nanoeval_impl)

add_executable(nanoeval_conformance conformance.cpp common.hpp)
target_link_libraries(nanoeval_conformance PRIVATE nanoeval_impl)
