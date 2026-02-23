cmake_minimum_required(VERSION 3.20)
project("llama.cpp-agent" C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER ON CACHE BOOL "" FORCE)
set(LLAMA_HTTPLIB ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/llama.cpp)
add_subdirectory(agent)
