cmake_minimum_required(VERSION 3.10)
project(sdl_examples)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)

add_subdirectory(sdl-rect)
add_subdirectory(sdl-video)
