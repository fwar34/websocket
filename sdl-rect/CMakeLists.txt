#===============================================================================
# sdl-rect/CMakeLists.txt — SDL 红色正方形示例
#===============================================================================
# 目标文件：sdl_rect.exe
# 源文件：
#   - main.cpp              主程序，SDL 渲染 + 键盘交互
#   - websocket_server.cpp  WebSocket 服务器实现
#   - ../utils/sha1.c      SHA1 哈希（WebSocket 握手）
#   - ../utils/base64.c    Base64 编码（WebSocket 握手）
#
# 编译选项：
#   Debug:  -g -O0 -Wall -Wextra（调试信息 + 零优化 + 全警告）
#   Release: -O2（优化）
#
# 后处理：编译后自动拷贝 client.html 到构建目录
#===============================================================================

cmake_minimum_required(VERSION 3.10)
project(sdl_rect)

# SDL2 头文件 + 上级目录（用于 #include "utils/sha1.h"）
include_directories(${SDL2_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(sdl_rect
    main.cpp
    websocket_server.cpp
    websocket_server.h
    ../utils/sha1.c
    ../utils/base64.c
)

# 链接 SDL2 和 Windows 套接字库
target_link_libraries(sdl_rect
    ${SDL2_LIBRARIES}
    ws2_32
)

# 按构建类型设置编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(sdl_rect PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(sdl_rect PRIVATE -O2)
endif()

# 编译后拷贝 client.html 到 exe 所在目录，浏览器通过 HTTP 访问
add_custom_command(TARGET sdl_rect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/client.html
        ${CMAKE_CURRENT_BINARY_DIR}/client.html
    COMMENT "Copying client.html to build directory"
)
