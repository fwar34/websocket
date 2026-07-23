/**
 * @file main.cpp
 * @brief RTSP 服务器入口程序
 * 
 * 启动 RTSP 服务器，监听 8554 端口，提供 MJPEG 视频流。
 * 
 * 测试方式：
 *   1. 启动本程序
 *   2. 使用 VLC 打开：rtsp://localhost:8554/test
 *   3. 或使用 ffplay：ffplay rtsp://localhost:8554/test
 * 
 * 完整的协议说明、框架图和线程模型详见 rtsp_server.h。
 */

#include "rtsp_server.h"
#include <csignal>
#include <iostream>

static std::atomic<bool> g_should_quit{false};

/**
 * @brief 信号处理函数（Ctrl+C 退出）
 */
void signal_handler(int) {
    g_should_quit = true;
}

int main() {
    // 注册 Ctrl+C 信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 创建并启动 RTSP 服务器（端口 8554）
    RtspServer server(8554);
    server.start();

    printf("[RTSP] Server running on port 8554\n");
    printf("[RTSP] Open rtsp://localhost:8554/test in VLC or ffplay\n");
    printf("[RTSP] Press Ctrl+C to stop\n");
    fflush(stdout);

    // 等待退出信号
    while (!g_should_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    printf("[RTSP] Shutting down...\n");
    fflush(stdout);

    server.stop();

    printf("[RTSP] Server stopped\n");
    return 0;
}
