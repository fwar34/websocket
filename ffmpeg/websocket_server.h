/**
 * @file websocket_server.h
 * @brief WebSocket 服务器类定义
 * 
 * 本文件定义了 WebSocketServer 类，提供基于 Winsock2 的 WebSocket 服务端功能。
 * 主要特性：
 * - 监听指定端口，接受浏览器 WebSocket 连接
 * - 支持 HTTP 升级握手（Sec-WebSocket-Key / Sec-WebSocket-Accept）
 * - 将浏览器键盘/鼠标事件转换为 SDL 事件（SDL_PushEvent）
 * - 使用线程池 + select 多路复用，每个 worker 线程监听多个客户端
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
 *   │  Canvas 渲染  │ ───────────────────→ │  │ acceptor_thread    │  │
 *   │  键鼠事件捕获  │   WebSocket 连接      │  │  listen + accept   │  │
 *   │              │ ←───────────────────→ │  │  轮询分配给 worker  │  │
 *   │              │                      │  └────────┬───────────┘  │
 *   │              │  WS 二进制帧 (帧数据)  │     ↓ ↓ ↓ ↓             │
 *   │              │ ←─────────────────── │  ┌─┬─┬─┬─┐             │
 *   │              │                      │  │W│W│W│W│ worker 线程池  │
 *   │              │  WS 文本帧 (JSON事件)  │  │0│1│2│3│ (4个线程)   │
 *   │              │ ───────────────────→ │  └─┴─┴─┴─┘             │
 *   │              │                      │  每个 worker:             │
 *   │              │                      │  select() 多路复用        │
 *   │              │                      │  监听多个 client socket   │
 *   │              │                      │  ↓                       │
 *   │              │                      │  SDL_PushEvent()         │
 *   │              │                      │  → SDL 事件队列           │
 *   └──────────────┘                      │  ┌────────────────────┐  │
 *                                         │  │ main() 主线程       │  │
 *                                         │  │  SDL_PollEvent()   │  │
 *                                         │  │  SDL_GetKeyboardState│ │
 *                                         │  │  SDL 渲染           │  │
 *                                         │  │  send_framebuffer() │  │
 *                                         │  └────────────────────┘  │
 *                                         └──────────────────────────┘
 *
 * 数据流：
 *   浏览器键鼠 → WebSocket JSON → worker_thread → SDL_PushEvent → SDL 事件队列
 *   → SDL_PollEvent → ws_key_state[] / keystate → 渲染变换 → RenderReadPixels
 *   → send_framebuffer → worker_thread 检测新帧 → WebSocket 二进制帧 → 浏览器 Canvas
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
 * 三、线程模型（线程池 + select 多路复用）
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
 *   │    send_framebuffer() ──→ 更新全局 framebuffer (加锁)       │
 *   │    SDL_Delay(8)      ← 控制帧率                              │
 *   │  }                                                          │
 *   └──────────────────────┬──────────────────────────────────────┘
 *                          │ send_framebuffer()
 *                          ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │              acceptor_thread (1个)                          │
 *   │                                                              │
 *   │  WSAStartup → socket → bind → listen                         │
 *   │  while (running_) {                                          │
 *   │    select(1s超时)                                             │
 *   │    accept() → 创建 WebSocketClient                            │
 *   │    → 轮询分配给 worker[next_worker % POOL_SIZE]              │
 *   │    → 通知 worker 有新客户端 (condition_variable)              │
 *   │  }                                                           │
 *   └──────────────────────┬──────────────────────────────────────┘
 *                          │ 分配客户端
 *     ┌────────────────────┼────────────────────┐
 *     ▼                    ▼                    ▼
 *   ┌────────────┐  ┌────────────┐  ┌────────────┐
 *   │ worker[0]  │  │ worker[1]  │  │ worker[N]  │  (POOL_SIZE=4)
 *   │            │  │            │  │            │
 *   │ select()   │  │ select()   │  │ select()   │
 *   │ 多路复用    │  │ 多路复用    │  │ 多路复用    │
 *   │ client 1,2 │  │ client 3,4 │  │ client 5,6 │
 *   │            │  │            │  │            │
 *   │ Phase 1: 握手前                        │
 *   │   recv → 检测 HTTP/WS                  │
 *   │   ├─ WS 升级 → 101 响应                │
 *   │   └─ HTTP GET → 返回 client.html       │
 *   │                                        │
 *   │ Phase 2: 握手后                        │
 *   │   recv → 累积 buffer                    │
 *   │   while (完整帧) {                      │
 *   │     process_websocket_frame()           │
 *   │       ├─ 0x01 文本 → parse_events()     │
 *   │       │             → SDL_PushEvent()   │
 *   │       ├─ 0x08 关闭 → send_close()      │
 *   │       ├─ 0x09 Ping  → send_pong()      │
 *   │       └─ 0x0A Pong  → 打印日志          │
 *   │   }                                    │
 *   │                                        │
 *   │   if (new_frame)                       │
 *   │     → send_websocket_frame(0x02)       │
 *   │     → 广播帧数据给自己的客户端            │
 *   │                                        │
 *   │   退出时：移除客户端，关闭 socket        │
 *   └────────────────────────────────────────┘
 *
 *   线程同步：
 *     framebuffer_mutex_   → 保护 framebuffer（主线程写，worker 线程读）
 *     worker_mutex[i]      → 保护每个 worker 的客户端列表
 *     worker_cv[i]         → 通知 worker 有新客户端加入
 *     SDL_PushEvent()      → SDL 内部线程安全，可跨线程调用
 *     running_             → atomic<bool>，所有线程检查此标志决定是否退出
 *
 * ============================================================================
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

/**
 * @class WebSocketServer
 * @brief WebSocket 服务端封装类（线程池 + select 多路复用）
 * 
 * 使用 Winsock2 实现，采用线程池模型：
 * - 1 个 acceptor 线程负责接受新连接并轮询分配给 worker
 * - POOL_SIZE 个 worker 线程，每个 worker 使用 select() 多路复用
 *   监听多个客户端 socket，处理握手、帧解析和帧广播
 * 
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
     * 创建 1 个 acceptor 线程和 POOL_SIZE 个 worker 线程。
     * 非阻塞调用。
     */
    void start();
    
    /**
     * @brief 停止 WebSocket 服务器
     * 
     * 1. 清除运行标志，使所有线程的循环退出
     * 2. 关闭所有客户端连接
     * 3. 唤醒所有 worker（notify_all）
     * 4. 等待所有线程结束（join）
     * 
     * 必须在 SDL 事件循环退出后调用，以确保 SDL_PushEvent 不再被使用。
     */
    void stop();
    
    /**
     * @brief 发送当前帧缓冲区到所有已连接的 WebSocket 客户端
     * 
     * 将 ARGB8888 像素数据保存到全局帧缓冲区（加锁），
     * 设置 new_frame 标志。各 worker 线程在 select 循环中检测到新帧后，
     * 以 WebSocket 二进制帧（opcode 0x02）广播给自己负责的客户端。
     * 
     * @param data 像素数据（ARGB8888 格式）
     * @param width 画面宽度（像素）
     * @param height 画面高度（像素）
     * @param pitch 每行字节数
     */
    void send_framebuffer(const uint8_t* data, int width, int height, int pitch);
    
    /**
     * @brief 设置视频分辨率信息
     * 
     * 在客户端连接后，worker 线程会自动发送一条包含宽高的文本帧，
     * 让浏览器动态设置 Canvas 尺寸。
     * 
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     */
    void set_video_info(int width, int height);
    
private:
    /**
     * @brief acceptor 线程函数
     * 
     * 初始化 Winsock，创建监听 socket，进入 accept 循环。
     * 新连接通过轮询分配给 worker 线程。
     */
    void acceptor_thread();
    
    /**
     * @brief worker 线程函数
     * 
     * 每个 worker 使用 select() 多路复用监听分配给自己的客户端 socket：
     * - 握手前：检测 HTTP GET 或 WebSocket 升级请求
     * - 握手后：解析 WebSocket 帧，转换为 SDL 事件
     * - 检测到新帧时：广播 framebuffer 给自己的客户端
     * 
     * @param worker_id worker 编号（0 ~ POOL_SIZE-1）
     */
    void worker_thread(int worker_id);
    
    static constexpr int POOL_SIZE = 4;   ///< 线程池大小
    
    int port_;                            ///< 监听端口
    std::thread acceptor_thread_;         ///< acceptor 线程
    std::vector<std::thread> workers_;    ///< worker 线程池
    std::atomic<bool> running_;           ///< 运行状态标志
    
    // 帧缓冲区（主线程写，worker 线程读）
    // 使用 shared_ptr 避免每个 worker 拷贝 1.2MB 帧数据，
    // worker 只需拷贝 shared_ptr（8 字节），数据共享同一份。
    std::mutex framebuffer_mutex_;        ///< 帧缓冲区互斥锁
    std::shared_ptr<const std::vector<uint8_t>> framebuffer_;  ///< 当前帧数据（ARGB8888）
    int frame_width_;                      ///< 帧宽度
    int frame_height_;                    ///< 帧高度
    int frame_pitch_;                     ///< 帧行距
    std::atomic<uint64_t> frame_version_; ///< 帧版本号（每次 send_framebuffer 递增）
    
    // 视频分辨率信息（用于发送 init 消息给浏览器）
    int video_info_width_ = 0;            ///< 视频宽度
    int video_info_height_ = 0;           ///< 视频高度
};

#endif
