#===============================================================================
# 顶层 CMakeLists.txt — WebSocket SDL 示例项目
#===============================================================================
# 项目结构：
#   websocket/
#   ├── CMakeLists.txt          ← 本文件
#   ├── utils/                  ← 公共工具库（SHA1、Base64）
#   │   ├── sha1.h / sha1.c
#   │   └── base64.h / base64.c
#   ├── sdl-rect/               ← 示例1：SDL 红色正方形
#   │   ├── CMakeLists.txt
#   │   ├── main.cpp
#   │   ├── websocket_server.cpp / .h
#   │   └── client.html
#   └── sdl-video/              ← 示例2：SDL 视频渲染（结构同上）
#
# 构建方式：
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build
#
# 依赖：
#   - SDL2（通过 find_package 查找）
#   - ws2_32（Windows 套接字库）
#===============================================================================

cmake_minimum_required(VERSION 3.10)
project(sdl_examples)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json，供 IDE 智能索引使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)

# 两个子项目
add_subdirectory(sdl-rect)
add_subdirectory(sdl-video)
