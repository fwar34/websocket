#===============================================================================
# rtsp/CMakeLists.txt — RTSP MJPEG 服务器（支持摄像头采集）
#===============================================================================
# 目标文件：rtsp_server.exe
# 源文件：
#   - main.cpp              主程序入口
#   - rtsp_server.cpp        RTSP + RTP 协议实现
#   - video_source.cpp       JPEG 编码器（内置测试图案 + 摄像头数据编码）
#   - camera_source.cpp      摄像头采集（FFmpeg avdevice / DirectShow）
#
# 依赖：
#   - ws2_32（Windows 套接字库）
#   - FFmpeg (libavformat, libavcodec, libavutil, libswscale, libavdevice)
#
# 编译选项：
#   Debug:  -g -O0 -Wall -Wextra（调试信息 + 零优化 + 全警告）
#   Release: -O2（优化）
#===============================================================================

cmake_minimum_required(VERSION 3.10)
project(rtsp_server)

# 可执行文件输出到构建目录下各自子目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rtsp)

# 使用 PkgConfig 查找 FFmpeg（MSYS2 UCRT64 环境）
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT   REQUIRED libavformat)
pkg_check_modules(AVCODEC    REQUIRED libavcodec)
pkg_check_modules(AVUTIL     REQUIRED libavutil)
pkg_check_modules(SWSCALE    REQUIRED libswscale)
pkg_check_modules(AVDEVICE   REQUIRED libavdevice)

# 头文件搜索路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVDEVICE_INCLUDE_DIRS}
)

add_executable(rtsp_server
    main.cpp
    rtsp_server.cpp
    rtsp_server.h
    video_source.cpp
    video_source.h
    camera_source.cpp
    camera_source.h
)

# 链接库
target_link_libraries(rtsp_server
    ws2_32
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVDEVICE_LIBRARIES}
)

# 按构建类型设置编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(rtsp_server PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(rtsp_server PRIVATE -O2)
endif()

# FFmpeg DLL 文件列表
set(FFMPEG_DLLS
    avcodec-62.dll
    avformat-62.dll
    avutil-60.dll
    swscale-9.dll
    avdevice-62.dll
)

# DLL 搜索路径
set(FFMPEG_BIN_DIR "I:/msys64/ucrt64/bin")

# 拷贝 FFmpeg DLL 到输出目录
foreach(dll_file ${FFMPEG_DLLS})
    add_custom_command(TARGET rtsp_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_BIN_DIR}/${dll_file}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${dll_file}"
        COMMENT "Copying ${dll_file} to build directory"
    )
endforeach()
