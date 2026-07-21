cmake_minimum_required(VERSION 3.10)
project(sdl_video)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include_directories(${SDL2_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(sdl_video
    main.cpp
    websocket_server.cpp
    websocket_server.h
    ../utils/sha1.c
    ../utils/base64.c
)

target_link_libraries(sdl_video
    ${SDL2_LIBRARIES}
    ws2_32
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(sdl_video PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(sdl_video PRIVATE -O2)
endif()

add_custom_command(TARGET sdl_video POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/client.html
        ${CMAKE_BINARY_DIR}/client.html
    COMMENT "Copying client.html to build directory"
)
