/**
 * @file websocket_server.h
 * @brief WebSocket 服务器类定义
 * 
 * 本文件定义了 WebSocketServer 类，提供基于 Winsock2 的 WebSocket 服务端功能。
 * 主要特性：
 * - 监听指定端口，接受浏览器 WebSocket 连接
 * - 支持 HTTP 升级握手（Sec-WebSocket-Key / Sec-WebSocket-Accept）
 * - 将浏览器键盘/鼠标事件转换为 SDL 事件（SDL_PushEvent）
 * - 支持多客户端并发连接，每个客户端独立线程处理
 * - 将 SDL 渲染帧通过 WebSocket 二进制帧发送给所有已连接客户端
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

/**
 * @class WebSocketServer
 * @brief WebSocket 服务端封装类
 * 
 * 使用 Winsock2 实现，在独立线程中运行主服务器循环。
 * 通过 SDL_PushEvent 将浏览器输入事件注入 SDL 事件队列，
 * 使得浏览器端的键鼠操作与本地 SDL 键鼠操作统一处理。
 * 
 * 使用示例：
 *   WebSocketServer ws(8080);
 *   ws.start();   // 启动服务（非阻塞）
 *   // ... 主循环 ...
 *   ws.stop();    // 停止服务
 */
class WebSocketServer {
public:
    /**
     * @brief 构造函数
     * @param port 监听端口号，默认 8080
     */
    WebSocketServer(int port = 8080);
    
    /**
     * @brief 析构函数，自动调用 stop()
     */
    ~WebSocketServer();
    
    /**
     * @brief 启动 WebSocket 服务器
     * 
     * 在新线程中启动主监听循环，接受客户端连接。
     * 非阻塞调用。
     */
    void start();
    
    /**
     * @brief 停止 WebSocket 服务器
     * 
     * 关闭所有客户端连接，等待服务器线程退出。
     * 必须在 SDL 事件循环退出后调用，以确保 SDL_PushEvent 不再被使用。
     */
    void stop();
    
    /**
     * @brief 发送当前帧缓冲区到所有已连接的 WebSocket 客户端
     * 
     * 将 RGBA 像素数据保存并通过 WebSocket 二进制帧（opcode 0x02）
     * 广播给所有完成握手的客户端。
     * 
     * @param data 像素数据（ARGB8888 格式）
     * @param width 画面宽度（像素）
     * @param height 画面高度（像素）
     * @param pitch 每行字节数
     */
    void send_framebuffer(const uint8_t* data, int width, int height, int pitch);
    
private:
    /**
     * @brief 服务器主循环线程函数
     * 
     * 使用 select() 多路复用监听端口，接受新连接并分发给独立客户端线程。
     */
    void server_thread();
    
    int port_;                          ///< 监听端口
    std::thread thread_;                ///< 服务器主线程
    std::atomic<bool> running_;        ///< 运行状态标志
    
    std::mutex framebuffer_mutex_;      ///< 帧缓冲区互斥锁
    std::vector<uint8_t> framebuffer_;  ///< 当前帧数据
    int frame_width_;                   ///< 帧宽度
    int frame_height_;                  ///< 帧高度
    int frame_pitch_;                   ///< 帧行距
    std::atomic<bool> new_frame_;       ///< 新帧到达标志
};

#endif
