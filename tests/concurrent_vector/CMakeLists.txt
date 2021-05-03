cmake_minimum_required(VERSION 3.19)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project ("ConcurrentVector")

add_executable(ConcurrentVector "source.cpp")

install(TARGETS ConcurrentVector RUNTIME DESTINATION ${INSTALL_BIN})
