# CMakeList.txt : CMake project for concurrent_vector, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.19)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project ("concurrent_impl_tests")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../header/" "${CMAKE_CURRENT_SOURCE_DIR}/common/")

link_libraries(-pthread)

set(${INSTALL_BIN} ${CMAKE_CURRENT_BINARY_DIR}/../bin/)

add_subdirectory(concurrent_vector)
