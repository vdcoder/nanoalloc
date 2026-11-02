# A separate project that uses an installed nanoalloc through find_package.
# CI builds it against the install tree to check the exported package.
cmake_minimum_required(VERSION 3.20)
project(nanoalloc_package_test LANGUAGES CXX)

find_package(nanoalloc REQUIRED)
add_executable(nanoalloc_package_test main.cpp)
target_link_libraries(nanoalloc_package_test PRIVATE nanoalloc::nanoalloc)
