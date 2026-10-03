cmake_minimum_required(VERSION 3.16)
project(perflab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimised, with debug info so perf can map samples to source lines.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(perflab perflab.cpp)

# Frame pointers let `perf record -g` walk the stack (call graphs, flame graphs).
target_compile_options(perflab PRIVATE -Wall -fno-omit-frame-pointer)

# Put the binary next to the source so the tutorial's ./perflab commands work.
set_target_properties(perflab PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
