#===============================================================================
# ffmpeg/CMakeLists.txt — FFmpeg 视频解码 + WebSocket 推流示例
#===============================================================================
# 目标文件：ffmpeg_server.exe
# 源文件：
#   - main.cpp              主程序，FFmpeg 解码 + SDL 预览 + WebSocket 推流
#   - websocket_server.cpp  WebSocket 服务器实现
#   - ../utils/sha1.c      SHA1 哈希（WebSocket 握手）
#   - ../utils/base64.c    Base64 编码（WebSocket 握手）
#
# 依赖：
#   - FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample)
#   - SDL2
#   - ws2_32 (Windows 套接字)
#
# 编译选项：
#   Debug:  -g -O0 -Wall -Wextra
#   Release: -O2
#
# 后处理：编译后自动拷贝 ffmpeg DLL 和 client.html 到构建目录
#===============================================================================

cmake_minimum_required(VERSION 3.10)
project(ffmpeg_server)

# 使用 PkgConfig 查找 FFmpeg（MSYS2 UCRT64 环境）
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC    REQUIRED libavcodec)
pkg_check_modules(AVFORMAT   REQUIRED libavformat)
pkg_check_modules(AVUTIL     REQUIRED libavutil)
pkg_check_modules(SWSCALE    REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# 源文件列表
set(SOURCES
    main.cpp
    websocket_server.cpp
    websocket_server.h
    ../utils/sha1.c
    ../utils/base64.c
)

add_executable(ffmpeg_server ${SOURCES})

# 包含目录
target_include_directories(ffmpeg_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${SDL2_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(ffmpeg_server
    ${SDL2_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ws2_32
)

# 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(ffmpeg_server PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(ffmpeg_server PRIVATE -O2)
endif()

# FFmpeg DLL 文件列表
set(FFMPEG_DLLS
    avcodec-62.dll
    avformat-62.dll
    avutil-60.dll
    swscale-9.dll
    swresample-6.dll
)

# DLL 搜索路径
set(FFMPEG_BIN_DIR "I:/msys64/ucrt64/bin")

# 拷贝 FFmpeg DLL 到输出目录
foreach(dll_file ${FFMPEG_DLLS})
    add_custom_command(TARGET ffmpeg_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_BIN_DIR}/${dll_file}"
            "${CMAKE_CURRENT_BINARY_DIR}/${dll_file}"
        COMMENT "Copying ${dll_file} to build directory"
    )
endforeach()

# 拷贝 client.html 到输出目录
add_custom_command(TARGET ffmpeg_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/client.html
        ${CMAKE_CURRENT_BINARY_DIR}/client.html
    COMMENT "Copying client.html to build directory"
)