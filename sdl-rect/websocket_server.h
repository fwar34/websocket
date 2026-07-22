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
 *
 * ============================================================================
 * 一、系统框架图
 * ============================================================================
 *
 *   ┌──────────────┐                      ┌──────────────────────────┐
 *   │   浏览器      │                      │     C++ 进程              │
 *   │  (client.html)│                      │                          │
 *   │              │   HTTP GET /         │  ┌────────────────────┐  │
 *   │  Canvas 渲染  │ ───────────────────→ │  │ WebSocketServer    │  │
 *   │  键鼠事件捕获  │   WebSocket 连接      │  │  (server_thread)   │  │
 *   │              │ ←───────────────────→ │  │  listen + accept   │  │
 *   │              │                      │  └────────┬───────────┘  │
 *   │              │  WS 二进制帧 (帧数据)  │           │ 新连接        │
 *   │              │ ←─────────────────── │  ┌────────▼───────────┐  │
 *   │              │                      │  │ client_thread (N个) │  │
 *   │              │  WS 文本帧 (JSON事件)  │  │  握手 + 帧解析      │  │
 *   │              │ ───────────────────→ │  │  ↓                  │  │
 *   │              │                      │  │  SDL_PushEvent()    │  │
 *   └──────────────┘                      │  └────────┬───────────┘  │
 *                                         │           │ SDL 事件      │
 *                                         │  ┌────────▼───────────┐  │
 *                                         │  │ main() 主线程       │  │
 *                                         │  │  SDL_PollEvent()   │  │
 *                                         │  │  SDL_GetKeyboardState│ │
 *                                         │  │  SDL 渲染           │  │
 *                                         │  │  send_framebuffer() │  │
 *                                         │  └────────────────────┘  │
 *                                         └──────────────────────────┘
 *
 * 数据流：
 *   浏览器键鼠 → WebSocket JSON → client_thread → SDL_PushEvent → SDL 事件队列
 *   → SDL_PollEvent → ws_key_state[] / keystate → 渲染变换 → RenderReadPixels
 *   → send_framebuffer → WebSocket 二进制帧 → 浏览器 Canvas
 *
 * ============================================================================
 * 二、WebSocket 协议（RFC 6455）
 * ============================================================================
 *
 * 1. 握手阶段（HTTP 升级）：
 *
 *    浏览器请求：
 *      GET / HTTP/1.1
 *      Upgrade: websocket
 *      Connection: Upgrade
 *      Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==     ← 浏览器生成
 *      Sec-WebSocket-Version: 13
 *
 *    服务器响应：
 *      HTTP/1.1 101 Switching Protocols
 *      Upgrade: websocket
 *      Connection: Upgrade
 *      Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=  ← 服务端计算
 *
 *    计算公式：
 *      accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 *
 * 2. 数据帧格式（RFC 6455 第 5.2 节）：
 *
 *      0                   1                   2                   3
 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     +-+-+-+-+-------+-+-------------+-------------------------------+
 *     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 *     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 *     |N|V|V|V|       |S|             |   (if payload len==126/127)    |
 *     | |1|2|3|       |K|             |                               |
 *     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 *     |     Extended payload length continued, if payload len == 127  |
 *     + - - - - - - - - - - - - - - - +-------------------------------+
 *     |                               |Masking-key, if MASK set to 1  |
 *     +-------------------------------+-------------------------------+
 *     | Masking-key (continued)       |          Payload Data         |
 *     +-------------------------------- - - - - - - - - - - - - - - - +
 *
 *     字段说明：
 *       FIN(1bit)   : 1=消息的最后一帧（本项目不支持分片，恒为1）
 *       RSV(3bit)   : 保留位，必须为0
 *       opcode(4bit): 帧类型
 *                       0x01 = 文本帧（浏览器→服务器的 JSON 事件）
 *                       0x02 = 二进制帧（服务器→浏览器的帧数据）
 *                       0x08 = 关闭帧
 *                       0x09 = Ping
 *                       0x0A = Pong
 *       MASK(1bit)  : 1=载荷被掩码（客户端→服务端必须掩码，服务端→客户端不掩码）
 *       Payload len : 0-125=实际长度; 126=后跟2字节16位长度; 127=后跟8字节64位长度
 *       Masking-key : 4字节掩码密钥（当 MASK=1 时存在）
 *       Payload Data : 实际数据，被掩码时需用 mask[i%4] 异或解密
 *
 * 3. 事件 JSON 格式：
 *
 *      键盘：    {"type":"keydown","keycode":87}   (keyCode: A=65..Z=90)
 *      鼠标移动： {"type":"mousemove","x":100,"y":200}
 *      鼠标按键： {"type":"mousedown","x":100,"y":200,"button":0}
 *                              (button: 0=左键, 1=中键, 2=右键)
 *
 * ============================================================================
 * 三、线程模型
 * ============================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    主线程 (main / UI)                        │
 *   │                                                             │
 *   │  while (running) {                                          │
 *   │    SDL_PollEvent()  ←── 拾取 SDL_PushEvent 注入的事件        │
 *   │      ├─ SDL_QUIT     → 退出                                  │
 *   │      ├─ SDL_KEYDOWN  → ws_key_state[scancode] = true         │
 *   │      └─ SDL_KEYUP    → ws_key_state[scancode] = false        │
 *   │    keystate = SDL_GetKeyboardState()  ← 本地硬件键盘          │
 *   │    if (keystate[W] || ws_key_state[W]) posY -= 5  ← 逻辑或  │
 *   │    SDL_Render*()     ← 渲染                                  │
 *   │    send_framebuffer() ──→ 拷贝到全局 framebuffer (加锁)      │
 *   │    SDL_Delay(8)      ← 控制帧率                              │
 *   │  }                                                          │
 *   └──────────────────────┬──────────────────────────────────────┘
 *                          │ send_framebuffer()
 *                          ▼
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │              WebSocketServer::server_thread                   │
 *   │                                                              │
 *   │  WSAStartup → socket → bind → listen                         │
 *   │  while (running_) {                                          │
 *   │    select(1s超时)                                             │
 *   │    accept() → 创建 WebSocketClient → push 到 clients 列表    │
 *   │    thread(client_thread, client).detach()  ──┐               │
 *   │  }                                           │ 每个连接       │
 *   └──────────────────────────────────────────────┼──────────────┘
 *                                                  │
 *   ┌──────────────────────────────────────────────▼──────────────┐
 *   │            client_thread (每客户端一个，detach)                 │
 *   │                                                              │
 *   │  Phase 1: 握手前                                              │
 *   │    recv() → 检测 "\r\n\r\n" 分隔头部                          │
 *   │    ├─ Upgrade: websocket → 握手（101 响应）                   │
 *   │    └─ 普通 HTTP GET → 返回 client.html → 关闭连接             │
 *   │                                                              │
 *   │  Phase 2: 握手后                                              │
 *   │    recv() → 累积到 receive_buffer                              │
 *   │    while (buffer >= 完整帧大小) {                              │
 *   │      提取一帧 → process_websocket_frame()                      │
 *   │        ├─ 0x01 文本 → parse_events() → SDL_PushEvent()  ←──┐  │
 *   │        ├─ 0x08 关闭 → send_close()                          │  │
 *   │        ├─ 0x09 Ping  → send_pong()                         │  │
 *   │        └─ 0x0A Pong  → 打印日志                              │  │
 *   │    }                                                        │  │
 *   │                                                             │  │
 *   │    trigger_writeable()                                      │  │
 *   │      → 遍历 clients → send_websocket_frame(0x02)            │  │
 *   │      → 从全局 framebuffer 读取帧数据（加锁）                   │  │
 *   │                                                             │  │
 *   │  退出时：从 clients 列表移除自身（加锁），关闭 socket         │  │
 *   └─────────────────────────────────────────────────────────────┘  │
 *                                                                    │
 *   线程同步：                                                       │
 *     g_server_state.framebuffer_mutex → 保护 framebuffer            │
 *     g_server_state.clients_mutex    → 保护 clients 列表             │
 *     SDL_PushEvent()                 → SDL 内部线程安全              │
 *     g_server_state.running          → atomic<bool>，所有线程检查    │
 *
 * ============================================================================
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

/**
 * @class WebSocketServer
 * @brief WebSocket 服务端封装类
 * 
 * 使用 Winsock2 实现，在独立线程中运行主服务器循环。
 * 通过 SDL_PushEvent 将浏览器输入事件注入 SDL 事件队列，
 * 使得浏览器端的键鼠操作能被主线程的 SDL_PollEvent 拾取。
 * 
 * 注意：SDL_PushEvent 注入的键盘事件不会更新 SDL_GetKeyboardState()
 * 返回的键盘状态表，因此调用方需在 SDL_PollEvent 循环中手动维护
 * ws_key_state[] 数组来追踪浏览器按键的按下/释放状态。
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
