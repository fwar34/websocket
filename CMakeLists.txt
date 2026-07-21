cmake_minimum_required(VERSION 3.10)
project(sdl_websocket_renderer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MINGW)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_C_COMPILER gcc)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/src)

add_executable(sdl_websocket_renderer
    src/main.cpp
    src/websocket_server.cpp
    src/websocket_server.h
    src/utils/sha1.c
    src/utils/base64.c
)

target_link_libraries(sdl_websocket_renderer
    ${SDL2_LIBRARIES}
    ws2_32
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(sdl_websocket_renderer PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(sdl_websocket_renderer PRIVATE -O2)
endif()

add_custom_command(TARGET sdl_websocket_renderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/client.html
        ${CMAKE_BINARY_DIR}/client.html
    COMMENT "Copying client.html to build directory"
)
