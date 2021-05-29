cmake_minimum_required(VERSION 3.19)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project ("ConcurrentQueue")

add_executable(ConcurrentQueue "source.cpp")

install(TARGETS ConcurrentQueue RUNTIME DESTINATION ${INSTALL_BIN})
