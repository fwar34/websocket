/**
 * @file rtsp_server.h
 * @brief RTSP/RTP 服务器类定义 — 支持 MJPEG 视频流
 *
 * 本文件定义了 RtspServer 类，提供基于 Winsock2 的 RTSP 服务端功能。
 * 主要特性：
 * - 监听指定端口，接受 RTSP 客户端连接（如 VLC、ffplay）
 * - 支持 RTSP 协议：OPTIONS、DESCRIBE、SETUP、PLAY、PAUSE、TEARDOWN
 * - 支持 SDP 会话描述（MJPEG 视频）
 * - 支持两种传输模式：
 *   a) UDP 传输：RTP 包通过独立 UDP socket 发送
 *   b) TCP 交织传输：RTP 包通过 RTSP TCP 连接发送（interleaved）
 * - 使用线程池 + select 多路复用，每个 worker 监听多个客户端
 * - MJPEG 视频源：内置测试图案生成器 + JPEG 编码器
 *
 * ============================================================================
 * 一、系统框架图
 * ============================================================================
 *
 *   ┌──────────────┐                      ┌──────────────────────────┐
 *   │  RTSP 客户端  │                      │     C++ 进程              │
 *   │  (VLC/ffplay) │                      │                          │
 *   │              │   RTSP (TCP)         │  ┌────────────────────┐  │
 *   │              │ ───────────────────→ │  │ acceptor_thread    │  │
 *   │              │ ←───────────────────→ │  │  listen + accept   │  │
 *   │              │                      │  │  轮询分配给 worker  │  │
 *   │              │   RTP (UDP/TCP)       │  └────────┬───────────┘  │
 *   │              │ ←─────────────────── │     ↓ ↓ ↓ ↓             │
 *   │              │                      │  ┌─┬─┬─┬─┐             │
 *   │              │                      │  │W│W│W│W│ worker 线程池  │
 *   │              │                      │  │0│1│2│3│ (4个线程)   │
 *   │              │                      │  └─┴─┴─┴─┘             │
 *   │              │                      │  每个 worker:             │
 *   │              │                      │  select() 多路复用        │
 *   │              │                      │  监听多个 RTSP session    │
 *   │              │                      │  ↓                       │
 *   │              │                      │  处理 RTSP 请求           │
 *   │              │                      │  PLAY → 生成 RTP 包       │
 *   │              │                      │  → 发送 JPEG 帧           │
 *   └──────────────┘                      │                          │
 *                                         │  ┌────────────────────┐  │
 *                                         │  │ VideoSource        │  │
 *                                         │  │  生成测试图案       │  │
 *                                         │  │  JPEG 编码         │  │
 *                                         │  └────────────────────┘  │
 *                                         └──────────────────────────┘
 *
 * 数据流：
 *   客户端 RTSP 请求 → worker_thread 解析 → SETUP 创建 session →
 *   PLAY → worker 循环中生成 JPEG 帧 → RTP 分包 → UDP/TCP 发送 → 客户端解码
 *
 * ============================================================================
 * 二、RTSP 协议（RFC 2326）
 * ============================================================================
 *
 * RTSP 是类似 HTTP 的文本协议，用于控制流媒体会话。
 * 默认端口 554，使用 TCP 作为控制通道。
 *
 * 1. 请求格式：
 *
 *      METHOD rtsp://host:port/path RTSP/1.0\r\n
 *      CSeq: 1\r\n
 *      Header: Value\r\n
 *      \r\n
 *      [Body]
 *
 * 2. 响应格式：
 *
 *      RTSP/1.0 200 OK\r\n
 *      CSeq: 1\r\n
 *      Header: Value\r\n
 *      \r\n
 *      [Body]
 *
 * 3. 方法说明：
 *
 *      OPTIONS    → 查询服务器支持的方法
 *      DESCRIBE   → 获取 SDP 会话描述（媒体格式、编码等）
 *      SETUP      → 创建会话，协商传输参数（UDP/TCP、端口）
 *      PLAY       → 开始或恢复播放（启动 RTP 发送）
 *      PAUSE      → 暂停播放（停止 RTP 发送）
 *      TEARDOWN   → 结束会话（关闭连接和资源）
 *
 * 4. 典型交互流程：
 *
 *      客户端                                    服务器
 *        │                                         │
 *        │ ── OPTIONS rtsp://host/test RTSP/1.0 ──→ │
 *        │ ←──────── 200 OK (Public: ...) ─────────│
 *        │                                         │
 *        │ ── DESCRIBE rtsp://host/test ──────────→ │
 *        │ ←── 200 OK (SDP: m=video, JPEG/90000) ──│
 *        │                                         │
 *        │ ── SETUP rtsp://host/test/track0 ──────→ │
 *        │    Transport: RTP/AVP;unicast;            │
 *        │    client_port=4588-4589                 │
 *        │ ←── 200 OK (Session: 12345,              │
 *        │    Transport: ...;server_port=6256-6257) │
 *        │                                         │
 *        │ ── PLAY rtsp://host/test ───────────────→│
 *        │    Session: 12345                        │
 *        │ ←── 200 OK (Range: npt=0.000-)           │
 *        │                                         │
 *        │ ←── RTP/AVP (UDP port 4588) ────────────│
 *        │     [JPEG frame data]                    │
 *        │     [JPEG frame data]                    │
 *        │     ...                                  │
 *        │                                         │
 *        │ ── TEARDOWN rtsp://host/test ───────────→│
 *        │    Session: 12345                        │
 *        │ ←── 200 OK ──────────────────────────────│
 *        │                                         │
 *
 * 5. SDP 会话描述（RFC 4566）：
 *
 *      v=0                                          ← SDP 版本
 *      o=- 0 0 IN IP4 0.0.0.0                       ← 发起者信息
 *      s=RTSP Server                                ← 会话名称
 *      t=0 0                                        ← 时间（0=永久）
 *      m=video 0 RTP/AVP 26                         ← 媒体行（端口=0动态分配, 格式=26=JPEG）
 *      a=control:track0                             ← 控制 URI 后缀
 *      a=rtpmap:26 JPEG/90000                       ← RTP 映射（PT=26, 编码=JPEG, 时钟=90000Hz）
 *
 * ============================================================================
 * 三、RTP 协议（RFC 3550）— MJPEG 载荷（RFC 2435）
 * ============================================================================
 *
 * 1. RTP 头部格式（12 字节固定 + 可选扩展）：
 *
 *      0                   1                   2                   3
 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |                           timestamp                           |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |           synchronization source (SSRC) identifier            |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *     字段说明：
 *       V(2bit)    : RTP 版本，恒为 2
 *       P(1bit)    : 填充标志，0=无填充
 *       X(1bit)    : 扩展标志，0=无扩展
 *       CC(4bit)   : CSRC 计数，通常为 0
 *       M(1bit)    : 标记位，JPEG 最后一个包设为 1
 *       PT(7bit)   : 载荷类型，JPEG=26
 *       Seq(16bit) : 序列号，每个 RTP 包递增 1
 *       Timestamp  : 32bit 时间戳，视频 90000Hz 时钟
 *       SSRC(32bit): 同步源标识符，随机生成
 *
 * 2. RTP/JPEG 载荷头部（RFC 2435，8 字节，紧跟在 RTP 头之后）：
 *
 *      0                   1                   2                   3
 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |       Type-specific |              Fragment offset          |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |      Type     |       Q       |       Width     |  Height  |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *     字段说明：
 *       Type-specific(8bit) : 类型特定字段，通常为 0
 *       Fragment offset(24bit): 当前包在 JPEG 数据中的字节偏移
 *       Type(8bit)   : 色度子采样类型，1=4:2:2, 0=4:2:0
 *       Q(8bit)      : 质量因子（1-100），255=使用内联量化表
 *       Width(8bit)  : 图像宽度（以 8 像素为单位），如 640→80
 *       Height(8bit) : 图像高度（以 8 像素为单位），如 480→60
 *
 * 3. JPEG 分片：
 *
 *      当 JPEG 帧大于 MTU（约 1400 字节）时，需要分片：
 *      - 每片一个 RTP 包
 *      - Fragment offset 标明当前片的偏移
 *      - 最后一片的 M 标记位设为 1
 *      - 同一帧的所有包共享相同的时间戳
 *      - 序列号连续递增
 *
 * ============================================================================
 * 四、TCP 交织模式（Interleaved）
 * ============================================================================
 *
 * RTSP 支持在 TCP 连接上交织传输 RTP 和 RTCP 数据：
 *
 *   数据帧格式（4 字节头 + RTP 数据）：
 *
 *      0                   1                   2                   3
 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |  '$' (0x24)   |   Channel    |           Length              |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |                       RTP data ...                           |
 *
 *     Channel: SETUP 请求中 interleaved=0-1 指定
 *              0 = RTP, 1 = RTCP（本项目仅用 RTP）
 *     Length:  RTP 数据长度（不含这 4 字节头）
 *
 * ============================================================================
 * 五、线程模型（线程池 + select 多路复用）
 * ============================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    主线程 (main)                            │
 *   │                                                             │
 *   │  RtspServer server(8554);                                  │
 *   │  server.start();   ← 启动服务器                              │
 *   │  server.wait();    ← 等待退出信号                             │
 *   │  server.stop();                                           │
 *   └──────────────────────┬──────────────────────────────────────┘
 *                          │ start()
 *                          ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │              acceptor_thread (1个)                          │
 *   │                                                              │
 *   │  WSAStartup → socket → bind → listen                         │
 *   │  while (running_) {                                          │
 *   │    select(1s超时)                                             │
 *   │    accept() → 创建 RTSPSession                                │
 *   │    → 轮询分配给 worker[next_worker % POOL_SIZE]              │
 *   │    → notify worker 有新客户端                                  │
 *   │  }                                                           │
 *   └──────────────────────┬──────────────────────────────────────┘
 *     ┌────────────────────┼────────────────────┐
 *     ▼                    ▼                    ▼
 *   ┌────────────┐  ┌────────────┐  ┌────────────┐
 *   │ worker[0] │  │ worker[1]  │  │ worker[N]  │  (POOL_SIZE=4)
 *   │            │  │            │  │            │
 *   │ select()   │  │ select()   │  │ select()   │
 *   │ 多路复用    │  │ 多路复用    │  │ 多路复用    │
 *   │            │  │            │  │            │
 *   │ 处理 RTSP  │  │ 处理 RTSP  │  │ 处理 RTSP  │
 *   │ 请求       │  │ 请求       │  │ 请求       │
 *   │            │  │            │  │            │
 *   │ 对 PLAYING │  │ 对 PLAYING │  │ 对 PLAYING │
 *   │ 的 session │  │ 的 session │  │ 的 session │
 *   │ 发送 RTP   │  │ 发送 RTP   │  │ 发送 RTP   │
 *   │            │  │            │  │            │
 *   │ 退出时：   │  │            │  │            │
 *   │ 清理 session│ │            │  │            │
 *   └────────────┘  └────────────┘  └────────────┘
 *
 *   线程同步：
 *     session_mutex[i]   → 保护每个 worker 的 session 列表
 *     session_cv[i]      → 通知 worker 有新 session
 *     VideoSource        → 线程安全，每个 session 独立生成帧
 *     running_           → atomic<bool>，所有线程检查此标志
 *
 * ============================================================================
 */

#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <memory>
#include <chrono>
#include <winsock2.h>

#include "video_source.h"

/**
 * @brief RTSP 会话状态
 * 
 * 每个客户端连接对应一个 RTSPSession，记录会话的全生命周期状态。
 * 由 shared_ptr 管理，在 acceptor 和 worker 间共享。
 */
struct RTSPSession {
    SOCKET sock;                    ///< TCP 套接字（RTSP 控制通道）
    std::string session_id;         ///< 会话 ID（SETUP 时生成）
    int cseq;                       ///< 当前请求的 CSeq 序号
    std::string receive_buffer;     ///< 接收缓冲区（累积分片数据）
    
    // 传输参数
    bool interleaved;               ///< true=TCP 交织模式, false=UDP 模式
    int rtp_channel;                ///< 交织模式下的 RTP 通道号（通常 0）
    
    // UDP 传输参数
    struct sockaddr_in rtp_dest;    ///< RTP 目标地址（客户端 IP + 端口）
    struct sockaddr_in rtcp_dest;   ///< RTCP 目标地址（客户端 IP + 端口）
    SOCKET rtp_socket;              ///< UDP RTP 发送 socket
    SOCKET rtcp_socket;             ///< UDP RTCP 发送 socket
    
    // 会话状态
    bool setup_done;                ///< SETUP 是否已完成
    bool playing;                   ///< PLAY 是否已启动
    
    // RTP 状态
    uint16_t rtp_seq;               ///< RTP 序列号（递增）
    uint32_t rtp_timestamp;         ///< RTP 时间戳（90000Hz）
    uint32_t ssrc;                  ///< 同步源标识符（随机）
    uint32_t sender_packet_count;   ///< 已发送 RTP 包数量（RTCP SR 用）
    uint32_t sender_octet_count;    ///< 已发送 RTP 载荷字节数（RTCP SR 用）
    
    // 播放位置（用于 PAUSE/SEEK/进度显示）
    double current_npt;             ///< 当前播放位置（秒，Normal Play Time）
    double duration;                ///< 媒体总时长（秒）
    std::chrono::steady_clock::time_point play_start_time; ///< 本次播放开始的墙钟时间
    uint32_t play_start_timestamp;  ///< 本次播放开始时的 RTP 时间戳
    
    // 视频源（每个会话独立）
    std::unique_ptr<VideoSource> video;

    // 帧率控制
    std::chrono::steady_clock::time_point last_frame_time;  ///< 上次发帧时间
    std::chrono::steady_clock::time_point last_rtcp_time;   ///< 上次发送 RTCP SR 时间
    
    RTSPSession(SOCKET s) 
        : sock(s), cseq(0), interleaved(false), rtp_channel(0),
          rtp_socket(INVALID_SOCKET), rtcp_socket(INVALID_SOCKET),
          setup_done(false), playing(false),
          rtp_seq(0), rtp_timestamp(0), ssrc(0),
          sender_packet_count(0), sender_octet_count(0),
          current_npt(0.0), duration(60.0) {}
    
    ~RTSPSession() {
        if (rtcp_socket != INVALID_SOCKET) closesocket(rtcp_socket);
        if (rtp_socket != INVALID_SOCKET) closesocket(rtp_socket);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }
};

/**
 * @class RtspServer
 * @brief RTSP 服务器（线程池 + select 多路复用）
 * 
 * 使用 Winsock2 实现，采用线程池模型：
 * - 1 个 acceptor 线程负责接受新连接并轮询分配给 worker
 * - POOL_SIZE 个 worker 线程，每个 worker 使用 select() 多路复用
 *   监听多个客户端 socket，处理 RTSP 请求和 RTP 发送
 * 
 * 使用示例：
 *   RtspServer server(8554);
 *   server.start();   // 启动服务
 *   server.wait();    // 等待退出
 *   server.stop();    // 停止服务
 */
class RtspServer {
public:
    /**
     * @brief 构造函数
     * @param port 监听端口号，默认 8554
     */
    RtspServer(int port = 8554);
    
    /**
     * @brief 析构函数，自动调用 stop()
     */
    ~RtspServer();
    
    /**
     * @brief 启动 RTSP 服务器
     * 创建 acceptor 和 worker 线程。非阻塞调用。
     */
    void start();
    
    /**
     * @brief 停止 RTSP 服务器
     * 关闭所有会话，等待所有线程退出。
     */
    void stop();
    
    /**
     * @brief 等待服务器退出（阻塞）
     * 内部等待一个原子标志被外部设置。
     */
    void wait();
    
private:
    /**
     * @brief acceptor 线程函数
     * 监听端口，接受新连接，轮询分配给 worker。
     */
    void acceptor_thread();
    
    /**
     * @brief worker 线程函数
     * 使用 select() 多路复用处理多个 RTSP 会话：
     * - 接收 RTSP 请求并处理（OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN）
     * - 对 PLAYING 状态的会话，生成 JPEG 帧并通过 RTP 发送
     * 
     * @param worker_id worker 编号（0 ~ POOL_SIZE-1）
     */
    void worker_thread(int worker_id);
    
    /**
     * @brief 处理一个 RTSP 请求
     * @param session 会话
     * @param request RTSP 请求字符串
     */
    void handle_request(std::shared_ptr<RTSPSession> session, 
                        const std::string& request);
    
    /**
     * @brief 发送 RTSP 响应
     * @param session 会话
     * @param status_code 状态码（如 200）
     * @param status_text 状态文本（如 "OK"）
     * @param headers 额外头部（可为空）
     * @param body 响应体（可为空）
     */
    void send_response(std::shared_ptr<RTSPSession> session,
                       int status_code, const std::string& status_text,
                       const std::string& headers, const std::string& body);
    
    /**
     * @brief 为 PLAYING 状态的会话发送一帧视频
     * 生成 JPEG 帧 → RTP 分包 → 通过 UDP/TCP 发送
     * @param session 会话
     */
    void send_video_frame(std::shared_ptr<RTSPSession> session);
    
    /**
     * @brief 通过 UDP 发送 RTP 包
     */
    void send_rtp_udp(std::shared_ptr<RTSPSession> session,
                      const uint8_t* data, size_t size);
    
    /**
     * @brief 通过 TCP 交织模式发送 RTP 包
     * 在 RTP 数据前添加 4 字节交织头（$ + channel + length）
     */
    void send_rtp_tcp(std::shared_ptr<RTSPSession> session,
                      const uint8_t* data, size_t size);
    
    /**
     * @brief 发送 MJPEG RTP 包
     * 将完整 JPEG 文件作为单个 RTP 包的 payload 发送
     */
    void send_rtp_mjpeg(std::shared_ptr<RTSPSession> session,
                        const std::vector<uint8_t>& jpeg, bool marker);
    
    /**
     * @brief 发送 RTCP Sender Report（发送端报告）
     * 
     * RTCP SR 包含墙钟时间戳与 RTP 时间戳的对应关系，
     * 客户端（VLC）据此计算当前播放进度（NPT）。
     * 每 5 秒发送一次。
     * 
     * @param session 会话
     */
    void send_rtcp_sender_report(std::shared_ptr<RTSPSession> session);
    
    /**
     * @brief 通过 UDP 发送 RTCP 包
     */
    void send_rtcp_udp(std::shared_ptr<RTSPSession> session,
                       const uint8_t* data, size_t size);
    
    /**
     * @brief 通过 TCP 交织模式发送 RTCP 包
     */
    void send_rtcp_tcp(std::shared_ptr<RTSPSession> session,
                       const uint8_t* data, size_t size);
    
    static constexpr int POOL_SIZE = 4;   ///< 线程池大小
    static constexpr int RTP_MTU = 1400;  ///< RTP 最大传输单元（字节）
    static constexpr int VIDEO_WIDTH = 640;   ///< 视频宽度（匹配摄像头 mjpeg 模式）
    static constexpr int VIDEO_HEIGHT = 480;  ///< 视频高度
    static constexpr int VIDEO_FPS = 30;       ///< 视频帧率（摄像头原生帧率）
    static constexpr uint32_t RTP_CLOCK = 90000; ///< RTP 时钟频率（Hz）
    
    int port_;                        ///< 监听端口
    std::thread acceptor_thread_;     ///< acceptor 线程
    std::vector<std::thread> workers_;///< worker 线程池
    std::atomic<bool> running_;       ///< 运行状态标志
    std::atomic<bool> quit_;          ///< 退出标志（wait() 等待）
    
    /**
     * @brief 每个 worker 的会话列表
     * acceptor 将新会话添加到某个 worker 的列表中（加锁），
     * worker 从自己的列表中取出会话进行 select 多路复用。
     */
    struct SessionList {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::shared_ptr<RTSPSession>> sessions;
    };
    SessionList worker_sessions_[POOL_SIZE];  ///< 各 worker 的会话列表
    std::atomic<int> next_worker_{0};          ///< 轮询计数器
};

#endif
