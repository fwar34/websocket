/**
 * @file rtsp_server.cpp
 * @brief RTSP/RTP 服务器实现 — 基于 Winsock2 + 线程池
 * 
 * 实现 RTSP 协议（RFC 2326）和 RTP 协议（RFC 3550）的服务端逻辑：
 * 1. RTSP 请求解析：OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / TEARDOWN
 * 2. SDP 会话描述生成（MJPEG 视频）
 * 3. RTP/JPEG 分包发送（RFC 2435）
 * 4. UDP 和 TCP 交织两种传输模式
 * 
 * 完整的协议说明、框架图和线程模型详见 rtsp_server.h。
 */

#include "rtsp_server.h"
#include "video_source.h"
#include "camera_source.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <random>
#include <cctype>

// ===========================================================================
// 辅助函数
// ===========================================================================

/**
 * @brief 生成随机 Session ID
 * @return 8 位十六进制字符串
 */
static std::string generate_session_id() {
    std::random_device rd;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(8) << (rd() & 0xFFFFFFFF);
    return oss.str();
}

/**
 * @brief 从 RTSP 请求中解析指定头部字段的值
 * @param request RTSP 请求字符串
 * @param header_name 头部字段名
 * @return 字段值，未找到返回空字符串
 */
static std::string parse_rtsp_header(const std::string& request, const std::string& header_name) {
    // RTSP 头部字段名大小写不敏感 (RFC 2326)
    // 将请求转为小写进行搜索
    std::string lower_request = request;
    for (auto& c : lower_request) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string lower_search = header_name;
    for (auto& c : lower_search) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    lower_search += ":";
    
    size_t pos = lower_request.find(lower_search);
    if (pos == std::string::npos) return "";
    size_t start = pos + lower_search.size();
    // 跳过可选的空白符（空格或制表符）
    while (start < request.size() && (request[start] == ' ' || request[start] == '\t')) {
        start++;
    }
    size_t end_cr = request.find("\r\n", start);
    size_t end_lf = request.find("\n", start);
    size_t end = std::min(end_cr, end_lf);
    if (end == std::string::npos) end = request.size();
    return request.substr(start, end - start);
}

/**
 * @brief 从 RTSP 请求第一行解析 URL 和方法
 * 
 * 请求行格式：METHOD rtsp://host:port/path RTSP/1.0
 * 
 * @param request RTSP 请求字符串
 * @param method 输出方法名
 * @param url 输出 URL
 */
static void parse_request_line(const std::string& request, std::string& method, std::string& url) {
    size_t end_cr = request.find("\r\n");
    size_t end_lf = request.find("\n");
    size_t end = std::min(end_cr, end_lf);
    std::string first_line = (end != std::string::npos) 
        ? request.substr(0, end) : request;
    
    size_t sp1 = first_line.find(' ');
    if (sp1 != std::string::npos) {
        method = first_line.substr(0, sp1);
        size_t sp2 = first_line.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) {
            url = first_line.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }
}

/**
 * @brief 从 RTSP URL 中提取基础 URL（用于 Content-Base）
 * 
 * 输入: rtsp://host:port/path
 * 输出: rtsp://host:port/path/
 * 
 * @param url 完整 RTSP URL
 * @return 基础 URL（带尾部斜杠）
 */
static std::string extract_base_url(const std::string& url) {
    // 输入: rtsp://host:port/path
    // 输出: rtsp://host:port/path/
    if (url.empty()) return "";
    if (url.back() == '/') return url;
    return url + "/";
}

// ===========================================================================
// RtspServer 类实现
// ===========================================================================

RtspServer::RtspServer(int port)
    : port_(port), running_(false), quit_(false) {
}

RtspServer::~RtspServer() {
    stop();
}

void RtspServer::start() {
    running_ = true;
    quit_ = false;

    // 尝试打开摄像头（单例，所有 session 共享）
    // 摄像头不可用时自动回退到测试图案
    CameraSource* cam = CameraSource::instance();
    if (cam->open("Integrated Webcam", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS)) {
        printf("[RTSP] Camera opened: %dx%d@%dfps\n", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    } else {
        printf("[RTSP] Camera unavailable, using test pattern\n");
    }
    fflush(stdout);

    // 创建 POOL_SIZE 个 worker 线程
    workers_.reserve(POOL_SIZE);
    for (int i = 0; i < POOL_SIZE; i++) {
        workers_.emplace_back(&RtspServer::worker_thread, this, i);
    }

    // 创建 acceptor 线程
    acceptor_thread_ = std::thread(&RtspServer::acceptor_thread, this);
}

void RtspServer::stop() {
    running_ = false;
    quit_ = true;
    
    // 唤醒所有 worker
    for (int i = 0; i < POOL_SIZE; i++) {
        std::lock_guard<std::mutex> lock(worker_sessions_[i].mutex);
        for (auto& s : worker_sessions_[i].sessions) {
            s->playing = false;
            closesocket(s->sock);
        }
        worker_sessions_[i].sessions.clear();
        worker_sessions_[i].cv.notify_all();
    }
    
    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }
    
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();

    // 关闭摄像头
    CameraSource::instance()->close();
}

void RtspServer::wait() {
    while (!quit_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ===========================================================================
// acceptor 线程
// ===========================================================================

void RtspServer::acceptor_thread() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[RTSP] WSAStartup failed\n");
        return;
    }
    
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        fprintf(stderr, "[RTSP] Failed to create socket\n");
        WSACleanup();
        return;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(server_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[RTSP] Bind failed: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return;
    }
    
    if (listen(server_socket, 5) == SOCKET_ERROR) {
        fprintf(stderr, "[RTSP] Listen failed\n");
        closesocket(server_socket);
        WSACleanup();
        return;
    }
    
    printf("[RTSP] Server listening on port %d\n", port_);
    printf("[RTSP] Stream URL: rtsp://localhost:%d/test\n", port_);
    fflush(stdout);
    
    while (running_) {
        // select 超时等待新连接
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int ret = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret <= 0) continue;
        
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_sock = accept(server_socket, 
                                     reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == INVALID_SOCKET) continue;
        
        printf("[RTSP] New client connected\n");
        fflush(stdout);
        
        // 创建会话
        auto session = std::make_shared<RTSPSession>(client_sock);
        session->session_id = generate_session_id();
        
        // 随机生成 SSRC
        std::random_device rd;
        session->ssrc = rd();
        
        // 轮询分配给 worker
        int worker_id = next_worker_.fetch_add(1) % POOL_SIZE;
        {
            std::lock_guard<std::mutex> lock(worker_sessions_[worker_id].mutex);
            worker_sessions_[worker_id].sessions.push_back(session);
        }
        worker_sessions_[worker_id].cv.notify_one();
        
        printf("[RTSP] Assigned to worker[%d], session=%s\n", 
               worker_id, session->session_id.c_str());
        fflush(stdout);
    }
    
    closesocket(server_socket);
    WSACleanup();
}

// ===========================================================================
// worker 线程
// ===========================================================================

void RtspServer::worker_thread(int worker_id) {
    auto& wl = worker_sessions_[worker_id];
    
    printf("[RTSP Worker %d] Started\n", worker_id);
    fflush(stdout);
    
    while (running_) {
        // 等待有会话或超时（15ms，保证高帧率 RTP 发送）
        std::vector<std::shared_ptr<RTSPSession>> my_sessions;
        {
            std::unique_lock<std::mutex> lock(wl.mutex);
            wl.cv.wait_for(lock, std::chrono::milliseconds(15), [&] {
                return !running_ || !wl.sessions.empty();
            });
            if (!running_) break;
            my_sessions = wl.sessions;
        }
        
        if (my_sessions.empty()) continue;
        
        // 构建 fd_set 进行 select 多路复用
        fd_set read_fds;
        FD_ZERO(&read_fds);
        SOCKET max_fd = 0;
        
        for (auto& s : my_sessions) {
            if (s->sock != INVALID_SOCKET) {
                FD_SET(s->sock, &read_fds);
                if (s->sock > max_fd) max_fd = s->sock;
            }
        }
        
        // select 超时 15ms（保证 ~60fps 帧率检查）
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 15000;
        
        int ret = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        
        // 对 PLAYING 状态的会话发送视频帧（无论 select 结果如何）
        // 帧率节流：每个会话独立控制，避免 66fps 灌满
        for (auto& s : my_sessions) {
            printf("[RTSP Worker %d] Checking session: playing=%d, video=%p\n",
                   worker_id, s->playing, (void*)s->video.get());
            fflush(stdout);
            if (s->playing) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - s->last_frame_time).count();
                printf("[RTSP Worker %d] Session playing: elapsed=%lldms, fps_interval=%dms\n",
                       worker_id, (long long)elapsed, 1000 / VIDEO_FPS);
                fflush(stdout);
                // VIDEO_FPS=15 → 每帧间隔 66ms
                if (elapsed >= 1000 / VIDEO_FPS) {
                    s->last_frame_time = now;
                    printf("[RTSP Worker %d] Sending video frame...\n", worker_id);
                    fflush(stdout);
                    send_video_frame(s);
                    printf("[RTSP Worker %d] Video frame sent\n", worker_id);
                    fflush(stdout);
                }
                
                // 每 1 秒发送一次 RTCP Sender Report（让 VLC 尽快获取进度）
                auto rtcp_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - s->last_rtcp_time).count();
                if (rtcp_elapsed >= 1000) {
                    s->last_rtcp_time = now;
                    send_rtcp_sender_report(s);
                }
            }
        }
        
        if (ret <= 0) continue;
        
        // 处理可读的 RTSP 控制连接
        std::vector<std::shared_ptr<RTSPSession>> to_remove;
        
        for (auto& s : my_sessions) {
            if (s->sock == INVALID_SOCKET) {
                to_remove.push_back(s);
                continue;
            }
            
            if (!FD_ISSET(s->sock, &read_fds)) continue;
            
            char buffer[4096];
            int bytes = recv(s->sock, buffer, sizeof(buffer), 0);
            
            if (bytes <= 0) {
                // 连接关闭
                printf("[RTSP] Client disconnected (worker %d)\n", worker_id);
                fflush(stdout);
                s->playing = false;
                to_remove.push_back(s);
                continue;
            }
            
            // 累积接收数据
            s->receive_buffer.append(buffer, bytes);
            
            // 处理所有完整的 RTSP 请求（以 \r\n\r\n 或 \n\n 结尾）
            size_t pos;
            while ((pos = s->receive_buffer.find("\r\n\r\n")) != std::string::npos ||
                   (pos = s->receive_buffer.find("\n\n")) != std::string::npos) {
                // 检查是否使用了 \n\n 还是 \r\n\r\n
                bool is_crlf = (pos + 3 < s->receive_buffer.size() && 
                               s->receive_buffer[pos] == '\r' && s->receive_buffer[pos+1] == '\n' &&
                               s->receive_buffer[pos+2] == '\r' && s->receive_buffer[pos+3] == '\n');
                size_t delim_len = is_crlf ? 4 : 2;
                
                // 如果在 \n\n 之前已经处理过 \r\n\r\n，跳过
                if (!is_crlf) {
                    // 检查前两个字符是否是 \r\n，如果是则应该使用 \r\n\r\n
                    if (pos >= 1 && s->receive_buffer[pos-1] == '\r') {
                        // 这是 \r\n 后面跟 \n\n，实际分隔符是 \r\n\r\n
                        is_crlf = true;
                        delim_len = 4;
                        pos -= 1;
                    }
                }
                
                std::string request = s->receive_buffer.substr(0, pos + delim_len);
                s->receive_buffer.erase(0, pos + delim_len);
                
                // 解析 Content-Length 检查是否有 body
                std::string cl = parse_rtsp_header(request, "Content-Length");
                if (!cl.empty()) {
                    size_t body_len = std::stoul(cl);
                    if (s->receive_buffer.size() < body_len) {
                        // body 不完整，放回请求等待更多数据
                        s->receive_buffer = request + s->receive_buffer;
                        break;
                    }
                    request += s->receive_buffer.substr(0, body_len);
                    s->receive_buffer.erase(0, body_len);
                }
                
                handle_request(s, request);
            }
        }
        
        // 移除断开的会话
        if (!to_remove.empty()) {
            std::lock_guard<std::mutex> lock(wl.mutex);
            for (auto& s : to_remove) {
                auto it = std::find(wl.sessions.begin(), wl.sessions.end(), s);
                if (it != wl.sessions.end()) {
                    wl.sessions.erase(it);
                }
            }
        }
    }
    
    printf("[RTSP Worker %d] Stopped\n", worker_id);
    fflush(stdout);
}

// ===========================================================================
// RTSP 请求处理
// ===========================================================================

void RtspServer::handle_request(std::shared_ptr<RTSPSession> session,
                                 const std::string& request) {
    std::string method, url;
    parse_request_line(request, method, url);
    
    // 解析 CSeq
    std::string cseq_str = parse_rtsp_header(request, "CSeq");
    if (!cseq_str.empty()) {
        session->cseq = std::stoi(cseq_str);
    }
    
    printf("[RTSP] %s %s (CSeq: %d)\n", method.c_str(), url.c_str(), session->cseq);
    fflush(stdout);
    
    if (method == "OPTIONS") {
        // 返回支持的方法列表
        send_response(session, 200, "OK",
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n", "");
    }
    else if (method == "DESCRIBE") {
        // 从请求 URL 提取基础 URL（Content-Base）
        std::string base_url = extract_base_url(url);
        
        // 生成 SDP 会话描述
        std::ostringstream sdp;
        sdp << "v=0\r\n";
        sdp << "o=- 0 0 IN IP4 0.0.0.0\r\n";
        sdp << "s=RTSP Server\r\n";
        sdp << "c=IN IP4 0.0.0.0\r\n";
        sdp << "t=0 0\r\n";
        sdp << "a=range:npt=0-60.000\r\n";
        sdp << "a=control:*\r\n";
        sdp << "m=video 0 RTP/AVP 96\r\n";
        sdp << "a=control:track0\r\n";
        sdp << "a=rtpmap:96 JPEG/90000\r\n";
        
        std::string sdp_str = sdp.str();
        std::ostringstream headers;
        headers << "Content-Type: application/sdp\r\n";
        headers << "Content-Length: " << sdp_str.size() << "\r\n";
        headers << "Content-Base: " << base_url << "\r\n";
        
        send_response(session, 200, "OK", headers.str(), sdp_str);
    }
    else if (method == "SETUP") {
        // 解析 Transport 头部
        std::string transport = parse_rtsp_header(request, "Transport");
        
        if (transport.find("interleaved=") != std::string::npos) {
            // TCP 交织模式
            session->interleaved = true;
            // 解析通道号，如 interleaved=0-1
            size_t pos = transport.find("interleaved=");
            pos += 12;
            session->rtp_channel = transport[pos] - '0';
            
            printf("[RTSP] SETUP: TCP interleaved, channel=%d\n", session->rtp_channel);
            fflush(stdout);
            
            std::ostringstream hdr;
            hdr << "Session: " << session->session_id << "\r\n";
            hdr << "Transport: RTP/AVP/TCP;unicast;interleaved="
                << session->rtp_channel << "-" << (session->rtp_channel + 1) << "\r\n";
            send_response(session, 200, "OK", hdr.str(), "");
        } else {
            // UDP 模式
            session->interleaved = false;
            
            // 解析 client_port
            int client_rtp_port = 0, client_rtcp_port = 0;
            size_t cp = transport.find("client_port=");
            if (cp != std::string::npos) {
                cp += 12;
                sscanf(transport.c_str() + cp, "%d-%d", &client_rtp_port, &client_rtcp_port);
            }
            
            printf("[RTSP] SETUP: UDP, client_port=%d-%d\n", client_rtp_port, client_rtcp_port);
            fflush(stdout);
            
            // 创建 UDP RTP socket 用于发送 RTP
            session->rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
            
            // 设置发送缓冲区大小（容纳完整 JPEG 帧 ~6KB）
            int sndbuf = 65536;
            setsockopt(session->rtp_socket, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
            
            // 绑定服务器 RTP 端口
            sockaddr_in server_rtp;
            server_rtp.sin_family = AF_INET;
            server_rtp.sin_addr.s_addr = INADDR_ANY;
            server_rtp.sin_port = 0; // 系统分配端口
            bind(session->rtp_socket, reinterpret_cast<sockaddr*>(&server_rtp), sizeof(server_rtp));
            
            // 获取实际分配的 RTP 端口
            int len = sizeof(server_rtp);
            getsockname(session->rtp_socket, reinterpret_cast<sockaddr*>(&server_rtp), &len);
            int server_rtp_port = ntohs(server_rtp.sin_port);
            int server_rtcp_port = server_rtp_port + 1;
            
            // 创建 UDP RTCP socket
            session->rtcp_socket = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in server_rtcp;
            server_rtcp.sin_family = AF_INET;
            server_rtcp.sin_addr.s_addr = INADDR_ANY;
            server_rtcp.sin_port = htons(server_rtcp_port);
            bind(session->rtcp_socket, reinterpret_cast<sockaddr*>(&server_rtcp), sizeof(server_rtcp));
            
            // 设置目标地址
            memset(&session->rtp_dest, 0, sizeof(session->rtp_dest));
            session->rtp_dest.sin_family = AF_INET;
            session->rtp_dest.sin_port = htons(client_rtp_port);
            
            memset(&session->rtcp_dest, 0, sizeof(session->rtcp_dest));
            session->rtcp_dest.sin_family = AF_INET;
            session->rtcp_dest.sin_port = htons(client_rtcp_port);
            
            // 从 TCP 连接获取客户端 IP
            sockaddr_in peer;
            int peer_len = sizeof(peer);
            getpeername(session->sock, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            session->rtp_dest.sin_addr = peer.sin_addr;
            session->rtcp_dest.sin_addr = peer.sin_addr;
            
            session->setup_done = true;
            
            std::ostringstream hdr;
            hdr << "Session: " << session->session_id << "\r\n";
            hdr << "Transport: RTP/AVP;unicast;client_port="
                << client_rtp_port << "-" << client_rtcp_port
                << ";server_port=" << server_rtp_port << "-" << server_rtcp_port << "\r\n";
            send_response(session, 200, "OK", hdr.str(), "");
        }
    }
    else if (method == "PLAY") {
        // 解析 Range 头部（支持 seek 拖动）
        std::string range_header = parse_rtsp_header(request, "Range");
        double seek_npt = session->current_npt;
        
        if (!range_header.empty()) {
            // 格式: Range: npt=10.5-  或  Range: npt=10.5-30.0
            size_t npt_pos = range_header.find("npt=");
            if (npt_pos != std::string::npos) {
                size_t start = npt_pos + 4;
                size_t dash = range_header.find('-', start);
                if (dash != std::string::npos) {
                    std::string start_str = range_header.substr(start, dash - start);
                    if (!start_str.empty() && start_str != "now") {
                        seek_npt = std::stod(start_str);
                    }
                }
            }
            // 限制在有效范围内
            if (seek_npt < 0) seek_npt = 0;
            if (seek_npt > session->duration) seek_npt = session->duration;
        }
        
        // 启动/恢复播放
        session->playing = true;
        
        printf("[RTSP] PLAY: session=%s, playing=true\n", session->session_id.c_str());
        fflush(stdout);
        
        // 根据 seek 位置设置 RTP 时间戳和视频源帧号
        session->current_npt = seek_npt;
        session->rtp_timestamp = static_cast<uint32_t>(seek_npt * RTP_CLOCK);
        session->play_start_time = std::chrono::steady_clock::now();
        session->play_start_timestamp = session->rtp_timestamp;
        
        // 创建视频源（如果不存在）
        if (!session->video) {
            session->video = std::make_unique<VideoSource>(VIDEO_WIDTH, VIDEO_HEIGHT);
        }
        
        // 根据 seek 位置跳转到对应帧
        int target_frame = static_cast<int>(seek_npt * VIDEO_FPS);
        session->video->seek_to_frame(target_frame);
        
        // 初始化帧计时器（设为过去时间，确保第一帧立即发送）
        // 使用 1 秒前的时间作为初始值，避免 time_point::min() 导致整数溢出
        auto init_time = std::chrono::steady_clock::now();
        session->last_frame_time = init_time - std::chrono::seconds(1);
        session->last_rtcp_time = init_time - std::chrono::seconds(1);
        
        // 重置 RTCP 统计
        session->sender_packet_count = 0;
        session->sender_octet_count = 0;
        
        printf("[RTSP] PLAY: session=%s, npt=%.3f\n", session->session_id.c_str(), seek_npt);
        fflush(stdout);
        
        // 从请求 URL 提取基础 URL 用于 RTP-Info
        std::string base_url = extract_base_url(url);
        
        std::ostringstream hdr;
        hdr << "Session: " << session->session_id << "\r\n";
        hdr << "Range: npt=" << std::fixed << std::setprecision(3) << seek_npt
            << "-" << std::fixed << std::setprecision(3) << session->duration << "\r\n";
        hdr << "RTP-Info: url=" << base_url << "track0;"
            << "seq=" << session->rtp_seq << ";"
            << "rtptime=" << session->rtp_timestamp << "\r\n";
        send_response(session, 200, "OK", hdr.str(), "");
    }
    else if (method == "PAUSE") {
        session->playing = false;
        
        // 基于实际已发送的 RTP 时间戳计算当前播放位置
        // 更准确：用 rtp_timestamp / RTP_CLOCK
        session->current_npt = static_cast<double>(session->rtp_timestamp) / RTP_CLOCK;
        if (session->current_npt > session->duration) {
            session->current_npt = session->duration;
        }
        
        printf("[RTSP] PAUSE: session=%s, npt=%.3f\n", 
               session->session_id.c_str(), session->current_npt);
        fflush(stdout);
        
        std::ostringstream hdr;
        hdr << "Session: " << session->session_id << "\r\n";
        send_response(session, 200, "OK", hdr.str(), "");
    }
    else if (method == "GET_PARAMETER") {
        // VLC 可能用 GET_PARAMETER 来查询当前播放位置
        // 计算当前 NPT（基于实际 RTP 时间戳）
        double current_npt = static_cast<double>(session->rtp_timestamp) / RTP_CLOCK;
        if (current_npt > session->duration) current_npt = session->duration;
        
        std::ostringstream body;
        body << "npt=" << std::fixed << std::setprecision(3) << current_npt << "\r\n";
        
        std::string body_str = body.str();
        std::ostringstream hdr;
        hdr << "Session: " << session->session_id << "\r\n";
        hdr << "Content-Type: text/parameters\r\n";
        hdr << "Content-Length: " << body_str.size() << "\r\n";
        
        send_response(session, 200, "OK", hdr.str(), body_str);
    }
    else if (method == "TEARDOWN") {
        session->playing = false;
        
        std::ostringstream hdr;
        hdr << "Session: " << session->session_id << "\r\n";
        send_response(session, 200, "OK", hdr.str(), "");
        
        // 先关闭再标记 INVALID_SOCKET，避免在 INVALID_SOCKET 上调用 shutdown
        shutdown(session->sock, SD_BOTH);
        closesocket(session->sock);
        session->sock = INVALID_SOCKET;
    }
    else {
        // 不支持的方法
        send_response(session, 501, "Not Implemented", "", "");
    }
}

void RtspServer::send_response(std::shared_ptr<RTSPSession> session,
                                int status_code, const std::string& status_text,
                                const std::string& headers, const std::string& body) {
    // 构建响应，自动添加 Content-Length 头部
    std::ostringstream oss;
    oss << "RTSP/1.0 " << status_code << " " << status_text << "\r\n";
    oss << "CSeq: " << session->cseq << "\r\n";
    // 检查 headers 中是否已包含 Content-Length
    if (headers.find("Content-Length") == std::string::npos) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }
    oss << headers;
    oss << "\r\n";
    oss << body;
    
    std::string response = oss.str();
    send(session->sock, response.c_str(), static_cast<int>(response.size()), 0);
    
    printf("[RTSP] Response: %d %s (%zu bytes)\n", status_code, status_text.c_str(), response.size());
    fflush(stdout);
}

// ===========================================================================
// RTP 视频帧发送
// ===========================================================================

void RtspServer::send_video_frame(std::shared_ptr<RTSPSession> session) {
    if (!session->video) {
        printf("[RTSP] send_video_frame: no video source!\n");
        fflush(stdout);
        return;
    }
    
    // 获取熵编码数据（不含 JPEG 文件头，仅 Huffman 编码后的压缩数据）
    // RFC 2435 规定 RTP JPEG 负载 = JPEG 头 + 量化表(可选) + 熵编码数据
    std::vector<uint8_t> entropy = session->video->get_frame_entropy();
    if (entropy.empty()) {
        printf("[RTSP] send_video_frame: entropy data is empty!\n");
        fflush(stdout);
        return;
    }
    
    printf("[RTSP] send_video_frame: entropy size=%zu bytes, interleaved=%d\n",
           entropy.size(), session->interleaved);
    fflush(stdout);
    
    // RFC 2435 JPEG 头参数：
    //   Type = 1: 8位精度，最大 2040x2040，4:2:0/4:2:2/4:4:4
    //   Q = 50: 使用标准 K=50 量化表（Q 在 1-127 范围内）
    //            此时 JPEG 头只有 8 字节，不附带量化表数据
    //   Width/Height: 以 8 像素为单位
    uint8_t jtype = 1;
    uint8_t jq = 50;
    uint8_t jwidth = (VIDEO_WIDTH + 7) / 8;
    uint8_t jheight = (VIDEO_HEIGHT + 7) / 8;
    
    // 基础 JPEG 头 = 8 字节
    const size_t JPEG_HEADER_SIZE = 8;
    
    // 当 Q <= 127 时，不附带量化表数据（使用标准 K 因子表）
    const size_t QUANT_TABLE_SIZE = 0;
    
    // MTU 限制：每片最大 RTP payload = 1400 字节
    // 所有包结构：12(RTP头) + 8(JPEG头) + 数据 = 1400
    //   → 数据最多 1400 - 8 = 1392 字节
    const size_t MAX_RTP_PAYLOAD = 1400;
    
    size_t offset = 0;
    uint32_t frag_count = 0;
    uint32_t total_octets = 0;
    
    while (offset < entropy.size()) {
        bool is_first = (offset == 0);
        bool is_last = false;
        
        size_t header_total = JPEG_HEADER_SIZE + (is_first ? QUANT_TABLE_SIZE : 0);
        size_t max_data = MAX_RTP_PAYLOAD - header_total;
        size_t chunk = std::min(max_data, entropy.size() - offset);
        
        if (chunk == 0 && offset < entropy.size()) {
            chunk = entropy.size() - offset;
        }
        
        is_last = (offset + chunk >= entropy.size());
        
        std::vector<uint8_t> packet;
        packet.reserve(12 + header_total + chunk);
        
        // ---- RTP 头 (12 字节) ----
        packet.push_back(0x80);                              // V=2, P=0, X=0, CC=0
        packet.push_back((is_last ? 0x80 : 0x00) | 96);     // M=marker, PT=96
        uint16_t seq = session->rtp_seq++;
        packet.push_back((seq >> 8) & 0xFF);
        packet.push_back(seq & 0xFF);
        uint32_t ts = session->rtp_timestamp;
        packet.push_back((ts >> 24) & 0xFF);
        packet.push_back((ts >> 16) & 0xFF);
        packet.push_back((ts >> 8) & 0xFF);
        packet.push_back(ts & 0xFF);
        uint32_t ssrc = session->ssrc;
        packet.push_back((ssrc >> 24) & 0xFF);
        packet.push_back((ssrc >> 16) & 0xFF);
        packet.push_back((ssrc >> 8) & 0xFF);
        packet.push_back(ssrc & 0xFF);
        
        // ---- RFC 2435 JPEG 头 (8 字节) ----
        // Byte 0: Type-specific (对于 Type 1，设为 0)
        packet.push_back(0x00);
        // Byte 1-3: Fragment Offset (24 位，大端)
        packet.push_back((offset >> 16) & 0xFF);
        packet.push_back((offset >> 8) & 0xFF);
        packet.push_back(offset & 0xFF);
        // Byte 4: Type (1 = 8-bit baseline JPEG)
        packet.push_back(jtype);
        // Byte 5: Q (50 = 标准 K=50 量化表)
        packet.push_back(jq);
        // Byte 6: Width (单位: 8 像素)
        packet.push_back(jwidth);
        // Byte 7: Height (单位: 8 像素)
        packet.push_back(jheight);
        
        // ---- 熵编码数据分片 ----
        packet.insert(packet.end(), entropy.begin() + offset,
                     entropy.begin() + offset + chunk);
        
        // 发送
        if (session->interleaved) {
            send_rtp_tcp(session, packet.data(), packet.size());
        } else {
            send_rtp_udp(session, packet.data(), packet.size());
        }
        
        offset += chunk;
        frag_count++;
        total_octets += static_cast<uint32_t>(chunk);
    }
    
    // 更新 RTCP 发送端统计
    session->sender_packet_count += frag_count;
    session->sender_octet_count += total_octets;
    
    // 更新 RTP 时间戳（90kHz 时钟 / 帧率）
    session->rtp_timestamp += RTP_CLOCK / VIDEO_FPS;
}

/**
 * @brief 发送 MJPEG RTP 包（完整 JPEG 文件）
 * 
 * 将完整 JPEG 文件作为单个 RTP 包的 payload 发送。
 * 这是最简单可靠的方式，VLC 直接将 RTP payload 解码为 JPEG。
 */
void RtspServer::send_rtp_mjpeg(std::shared_ptr<RTSPSession> session,
                                 const std::vector<uint8_t>& jpeg, bool marker) {
    // 构建 RTP 包：RTP 头(12) + JPEG 数据
    std::vector<uint8_t> packet;
    packet.reserve(12 + jpeg.size());
    
    // RTP 头
    packet.push_back(0x80);  // V=2, P=0, X=0, CC=0
    packet.push_back((marker ? 0x80 : 0x00) | 96);  // M=marker, PT=96 (动态)
    uint16_t seq = session->rtp_seq++;
    packet.push_back((seq >> 8) & 0xFF);
    packet.push_back(seq & 0xFF);
    uint32_t ts = session->rtp_timestamp;
    packet.push_back((ts >> 24) & 0xFF);
    packet.push_back((ts >> 16) & 0xFF);
    packet.push_back((ts >> 8) & 0xFF);
    packet.push_back(ts & 0xFF);
    uint32_t ssrc = session->ssrc;
    packet.push_back((ssrc >> 24) & 0xFF);
    packet.push_back((ssrc >> 16) & 0xFF);
    packet.push_back((ssrc >> 8) & 0xFF);
    packet.push_back(ssrc & 0xFF);
    
    // JPEG 文件直接作为 payload
    packet.insert(packet.end(), jpeg.begin(), jpeg.end());
    
    // 发送
    if (session->interleaved) {
        send_rtp_tcp(session, packet.data(), packet.size());
    } else {
        send_rtp_udp(session, packet.data(), packet.size());
    }
}

void RtspServer::send_rtp_udp(std::shared_ptr<RTSPSession> session,
                                const uint8_t* data, size_t size) {
    if (session->rtp_socket == INVALID_SOCKET) return;
    sendto(session->rtp_socket, reinterpret_cast<const char*>(data),
           static_cast<int>(size), 0,
           reinterpret_cast<sockaddr*>(&session->rtp_dest), sizeof(session->rtp_dest));
}

void RtspServer::send_rtp_tcp(std::shared_ptr<RTSPSession> session,
                                const uint8_t* data, size_t size) {
    // TCP 交织模式：4 字节头 + RTP 数据
    uint8_t header[4];
    header[0] = 0x24;                             // '$' 标识
    header[1] = static_cast<uint8_t>(session->rtp_channel); // 通道号
    header[2] = (size >> 8) & 0xFF;               // 长度高
    header[3] = size & 0xFF;                      // 长度低
    
    // 先发头再发数据（合并为一次发送更高效）
    std::vector<uint8_t> frame;
    frame.reserve(4 + size);
    frame.insert(frame.end(), header, header + 4);
    frame.insert(frame.end(), data, data + size);
    
    int sent = send(session->sock, reinterpret_cast<const char*>(frame.data()),
         static_cast<int>(frame.size()), 0);
    if (sent == SOCKET_ERROR) {
        printf("[RTSP] send_rtp_tcp failed: error=%d, size=%zu\n", WSAGetLastError(), frame.size());
        fflush(stdout);
    } else if (sent != (int)frame.size()) {
        printf("[RTSP] send_rtp_tcp partial: sent=%d, expected=%zu\n", sent, frame.size());
        fflush(stdout);
    }
}

// ===========================================================================
// RTCP Sender Report (RFC 3550)
// ===========================================================================

void RtspServer::send_rtcp_sender_report(std::shared_ptr<RTSPSession> session) {
    // RTCP SR 包结构（28 字节固定头部）：
    //
    //   0                   1                   2                   3
    //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |V=2|P|    RC   |   PT=SR=200   |             length            |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |                         SSRC of sender                        |  (4)
    //  +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
    //  |              NTP timestamp, most significant word             |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |             NTP timestamp, least significant word             |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |                         RTP timestamp                         |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |                     sender's packet count                     |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |                      sender's octet count                     |  (4)
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // V=2 (版本), P=0 (填充), RC=0 (报告计数), PT=200 (SR)
    
    std::vector<uint8_t> sr_packet;
    sr_packet.reserve(28);
    
    // 计算当前 NTP 时间戳（Unix 纪元 → NTP 纪元）
    // NTP 纪元 = 1900-01-01, Unix 纪元 = 1970-01-01
    // 差值 = 2208988800 秒
    auto now = std::chrono::system_clock::now();
    auto duration_since_epoch = now.time_since_epoch();
    uint64_t unix_us = std::chrono::duration_cast<std::chrono::microseconds>(
        duration_since_epoch).count();
    uint64_t ntp_sec = (unix_us / 1000000) + 2208988800ULL;
    uint64_t ntp_frac = ((unix_us % 1000000) * 0x100000000ULL) / 1000000ULL;
    
    // 当前 RTP 时间戳 — 使用实际发送的时间戳（更准确）
    uint32_t current_rtp_ts = session->rtp_timestamp;
    
    // Byte 0: V=2, P=0, RC=0 → 0x80
    sr_packet.push_back(0x80);
    // Byte 1: PT=200 (SR)
    sr_packet.push_back(200);
    // Byte 2-3: length = (28/4) - 1 = 6 (以 32-bit 字为单位，减 1)
    sr_packet.push_back(0x00);
    sr_packet.push_back(0x06);
    // Byte 4-7: SSRC
    uint32_t ssrc = session->ssrc;
    sr_packet.push_back((ssrc >> 24) & 0xFF);
    sr_packet.push_back((ssrc >> 16) & 0xFF);
    sr_packet.push_back((ssrc >> 8) & 0xFF);
    sr_packet.push_back(ssrc & 0xFF);
    // Byte 8-11: NTP timestamp (MSW)
    sr_packet.push_back((ntp_sec >> 24) & 0xFF);
    sr_packet.push_back((ntp_sec >> 16) & 0xFF);
    sr_packet.push_back((ntp_sec >> 8) & 0xFF);
    sr_packet.push_back(ntp_sec & 0xFF);
    // Byte 12-15: NTP timestamp (LSW)
    sr_packet.push_back((ntp_frac >> 24) & 0xFF);
    sr_packet.push_back((ntp_frac >> 16) & 0xFF);
    sr_packet.push_back((ntp_frac >> 8) & 0xFF);
    sr_packet.push_back(ntp_frac & 0xFF);
    // Byte 16-19: RTP timestamp
    sr_packet.push_back((current_rtp_ts >> 24) & 0xFF);
    sr_packet.push_back((current_rtp_ts >> 16) & 0xFF);
    sr_packet.push_back((current_rtp_ts >> 8) & 0xFF);
    sr_packet.push_back(current_rtp_ts & 0xFF);
    // Byte 20-23: sender's packet count
    uint32_t spc = session->sender_packet_count;
    sr_packet.push_back((spc >> 24) & 0xFF);
    sr_packet.push_back((spc >> 16) & 0xFF);
    sr_packet.push_back((spc >> 8) & 0xFF);
    sr_packet.push_back(spc & 0xFF);
    // Byte 24-27: sender's octet count
    uint32_t soc = session->sender_octet_count;
    sr_packet.push_back((soc >> 24) & 0xFF);
    sr_packet.push_back((soc >> 16) & 0xFF);
    sr_packet.push_back((soc >> 8) & 0xFF);
    sr_packet.push_back(soc & 0xFF);
    
    // 发送
    if (session->interleaved) {
        send_rtcp_tcp(session, sr_packet.data(), sr_packet.size());
    } else {
        send_rtcp_udp(session, sr_packet.data(), sr_packet.size());
    }
    
    printf("[RTCP] Sender Report sent: packets=%u, octets=%u, rtp_ts=%u\n",
           spc, soc, current_rtp_ts);
    fflush(stdout);
}

void RtspServer::send_rtcp_udp(std::shared_ptr<RTSPSession> session,
                                 const uint8_t* data, size_t size) {
    if (session->rtcp_socket == INVALID_SOCKET) return;
    sendto(session->rtcp_socket, reinterpret_cast<const char*>(data),
           static_cast<int>(size), 0,
           reinterpret_cast<sockaddr*>(&session->rtcp_dest), sizeof(session->rtcp_dest));
}

void RtspServer::send_rtcp_tcp(std::shared_ptr<RTSPSession> session,
                                 const uint8_t* data, size_t size) {
    // TCP 交织模式：4 字节头 + RTCP 数据
    // RTCP 通道号 = RTP 通道号 + 1
    uint8_t header[4];
    header[0] = 0x24;                             // '$' 标识
    header[1] = static_cast<uint8_t>(session->rtp_channel + 1); // RTCP 通道号
    header[2] = (size >> 8) & 0xFF;               // 长度高
    header[3] = size & 0xFF;                      // 长度低
    
    std::vector<uint8_t> frame;
    frame.reserve(4 + size);
    frame.insert(frame.end(), header, header + 4);
    frame.insert(frame.end(), data, data + size);
    
    send(session->sock, reinterpret_cast<const char*>(frame.data()),
         static_cast<int>(frame.size()), 0);
}
