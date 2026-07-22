/**
 * @file websocket_server.cpp
 * @brief WebSocket 服务器实现 — 基于 Winsock2 + SDL 事件系统
 * 
 * 实现 WebSocket 协议的服务端逻辑：
 * 1. HTTP 升级握手（RFC 6455）：解析 Sec-WebSocket-Key，计算 Sec-WebSocket-Accept
 *    accept_key = base64(sha1(key + GUID))
 *    GUID: 258EAFA5-E914-47DA-95CA-C5AB0DC85B11
 * 2. WebSocket 帧编码/解码：支持掩码、多字节长度编码
 * 3. 客户端事件 → SDL 事件：将浏览器 JSON 事件转换为 SDL_Event 并通过 SDL_PushEvent 注入
 * 4. 帧数据广播：将渲染帧以二进制帧形式发送给所有客户端
 * 
 * 完整的系统框架图、WebSocket 协议帧格式说明和线程模型详见 websocket_server.h。
 * 
 * 线程池模型概要：
 * - 主线程（UI）：运行 SDL 事件循环和渲染，调用 send_framebuffer() 更新帧缓冲区
 * - acceptor 线程（1个）：listen + accept，将新连接轮询分配给 worker
 * - worker 线程（POOL_SIZE=4个）：每个 worker 使用 select() 多路复用监听
 *   多个客户端 socket，处理握手、帧解析、事件注入和帧广播
 * 
 * 线程同步：
 * - framebuffer_mutex_：保护 framebuffer_（主线程写，worker 线程读）
 * - worker_mutex[i]：保护每个 worker 的客户端列表（acceptor 写，worker 读写）
 * - worker_cv[i]：通知 worker 有新客户端加入（acceptor 通知，worker 等待）
 * - SDL_PushEvent()：SDL 内部线程安全，可跨线程调用
 * - running_：atomic<bool>，所有线程检查此标志决定是否退出
 */

#include "websocket_server.h"
#include <SDL2/SDL.h>
extern "C" {
#include "utils/sha1.h"
#include "utils/base64.h"
}
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <algorithm>
#include <memory>
#include <chrono>

/**
 * @brief 线程池大小，与 websocket_server.h 中 POOL_SIZE 保持一致
 */
constexpr int POOL_SIZE = 4;

namespace {

/**
 * @brief 客户端连接状态
 * 
 * 每个 WebSocket 连接对应一个 WebSocketClient 实例，
 * 由 shared_ptr 管理生命周期，在 acceptor 线程和 worker 线程间共享。
 */
struct WebSocketClient {
    SOCKET sock;               ///< 客户端套接字描述符
    bool handshake_done;       ///< WebSocket 握手是否已完成
    bool connected;             ///< 连接是否活跃（false 时将被移除）
    std::string receive_buffer; ///< 接收缓冲区（累积分片数据，直到完整帧）
    
    WebSocketClient(SOCKET s) : sock(s), handshake_done(false), connected(true) {}
};

/**
 * @brief 每个 worker 线程的客户端列表
 * 
 * acceptor 线程将新客户端添加到某个 worker 的列表中（加锁），
 * worker 线程从自己的列表中取出客户端进行 select 多路复用。
 * 客户端断开时由 worker 线程从列表中移除。
 */
struct WorkerData {
    std::mutex mutex;                          ///< 保护 clients 列表
    std::condition_variable cv;                ///< 通知 worker 有新客户端
    std::vector<std::shared_ptr<WebSocketClient>> clients;  ///< 该 worker 负责的客户端
};

/**
 * @brief 全局 worker 数据数组（POOL_SIZE 个 worker）
 * 
 * acceptor 线程通过轮询将新连接分配给各 worker，
 * 每个 worker 独立使用 select() 多路复用监听自己的客户端。
 */
WorkerData g_workers[POOL_SIZE];

/**
 * @brief 下一个分配的 worker 编号（原子变量，轮询调度）
 */
std::atomic<int> g_next_worker{0};

// ===========================================================================
// 协议工具函数（与线程模型无关，纯函数）
// ===========================================================================

/**
 * @brief JS keyCode → SDL_Scancode 映射
 * 
 * 浏览器 KeyboardEvent.keyCode 对于字母键等于 ASCII 码值：
 *   'A'=65, 'W'=87, 'S'=83, ...
 * SDL_Scancode 对于字母键从 SDL_SCANCODE_A(4) 连续编号。
 * 因此映射公式为：SDL_SCANCODE_A + (keycode - 65)
 * 
 * @param keycode JS keyCode 值
 * @return 对应的 SDL_Scancode，未知键返回 SDL_SCANCODE_UNKNOWN
 */
SDL_Scancode jsKeyCodeToSDL(int keycode) {
    if (keycode >= 65 && keycode <= 90) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (keycode - 65));
    }
    if (keycode >= 48 && keycode <= 57) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_0 + (keycode - 48));
    }
    return SDL_SCANCODE_UNKNOWN;
}

/**
 * @brief 读取文件内容到字符串
 * @param filename 文件路径
 * @return 文件内容，失败返回空字符串
 */
std::string read_file(const std::string& filename) {
    FILE* file = fopen(filename.c_str(), "rb");
    if (!file) {
        return "";
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    std::vector<char> buffer(size);
    fread(buffer.data(), 1, size, file);
    fclose(file);
    
    return std::string(buffer.data(), size);
}

/**
 * @brief 使用项目内 Base64 库进行编码
 * 
 * 将二进制数据编码为 Base64 字符串，用于 WebSocket 握手响应。
 * 使用用户提供的 base64.h / base64.c 接口。
 * 
 * @param data 输入二进制数据
 * @return Base64 编码后的字符串
 */
std::string base64_encode(const std::vector<uint8_t>& data) {
    char buffer[1024];
    Base64Handle b64;
    Base64HandleInitialize(&b64);
    b64.setbuff(&b64, buffer, sizeof(buffer));
    b64.encodeData(&b64, data.data(), data.size(), 1);
    b64.addzero(&b64);
    
    printf("[Base64] Input: %zu bytes, Output: %s\n", data.size(), buffer);
    fflush(stdout);
    
    return std::string(buffer);
}

/**
 * @brief 使用项目内 SHA1 库计算哈希
 * 
 * 使用用户提供的 sha1.h / sha1.c 接口，
 * 采用 SHA-1 算法（RFC 3174）计算 20 字节哈希值。
 * 
 * @param input 输入字符串
 * @param output 输出哈希值（20 字节）
 */
void sha1_hash(const std::string& input, std::vector<uint8_t>& output) {
    output.resize(HASH_SHA1_FINALSIZE, 0);
    sha1_t* sha1 = (sha1_t*)malloc(sizeof(sha1_t));
    if (!sha1) return;
    HASH_SHA1_INITIALIZE(sha1);
    sha1->Update(sha1, input.data(), input.size());
    sha1->Final(sha1, output.data());
    free(sha1);
    
    printf("[SHA1] Input: '%s' (%zu bytes), Output: ", input.c_str(), input.size());
    for (int i = 0; i < HASH_SHA1_FINALSIZE; i++) {
        printf("%02X", output[i]);
    }
    printf("\n");
    fflush(stdout);
}

/**
 * @brief 计算 WebSocket Sec-WebSocket-Accept 值
 * 
 * 按照 RFC 6455 规范：
 * accept_key = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 * 
 * @param key 客户端 Sec-WebSocket-Key
 * @return Sec-WebSocket-Accept 值
 */
std::string compute_accept_key(const std::string& key) {
    std::string concat = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::vector<uint8_t> hash;
    sha1_hash(concat, hash);
    return base64_encode(hash);
}

/**
 * @brief 从 HTTP 请求头中解析指定字段的值
 * 
 * 在 "Header-Name: value\r\n" 格式中查找并提取 value。
 * 
 * @param request HTTP 请求头字符串（包含多行 \r\n 分隔的头部字段）
 * @param header_name 要查找的头部字段名（如 "Sec-WebSocket-Key"）
 * @return 字段值字符串，未找到返回空字符串
 */
std::string parse_header(const std::string& request, const std::string& header_name) {
    size_t pos = request.find(header_name + ": ");
    if (pos == std::string::npos) {
        return "";
    }
    
    size_t start = pos + header_name.size() + 2;
    size_t end = request.find("\r\n", start);
    if (end == std::string::npos) {
        end = request.size();
    }
    
    return request.substr(start, end - start);
}

/**
 * @brief 发送 HTTP 响应给客户端
 * 
 * 构建标准 HTTP/1.1 响应（状态行 + 头部 + 空行 + 正文），
 * 用于在握手前向浏览器返回 client.html 页面。
 * 
 * @param sock 客户端套接字
 * @param content 响应正文内容
 * @param status_code HTTP 状态码（如 200、404）
 * @param content_type Content-Type（如 "text/html"）
 */
void send_http_response(SOCKET sock, const std::string& content, int status_code, const std::string& content_type) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " OK\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << content.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << content;
    
    std::string response = oss.str();
    send(sock, response.c_str(), response.size(), 0);
}

/**
 * @brief 发送一个 WebSocket 数据帧给客户端
 * 
 * 按 RFC 6455 帧格式组装并发送：
 *   - FIN=1（单帧完整消息）
 *   - 不带掩码（服务端→客户端帧不需要掩码）
 *   - 根据 payload 长度选择 7bit / 16bit / 64bit 长度编码
 * 
 * @param sock 客户端套接字
 * @param data 载荷数据指针
 * @param length 载荷长度（字节）
 * @param opcode 帧类型（0x01=文本, 0x02=二进制, 0x08=关闭, 0x09=Ping, 0x0A=Pong）
 */
void send_websocket_frame(SOCKET sock, const uint8_t* data, size_t length, uint8_t opcode) {
    std::vector<uint8_t> frame;
    
    frame.push_back(opcode | 0x80);
    
    if (length <= 125) {
        frame.push_back(static_cast<uint8_t>(length));
    } else if (length <= 65535) {
        frame.push_back(126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((length >> (i * 8)) & 0xFF);
        }
    }
    
    frame.insert(frame.end(), data, data + length);
    
    send(sock, reinterpret_cast<const char*>(frame.data()), frame.size(), 0);
}

/**
 * @brief 发送 Pong 帧给客户端
 * 
 * 作为对客户端 Ping 帧的响应（RFC 6455 要求）。
 * Pong 帧的 opcode=0x0A，载荷为 Ping 帧携带的回显数据。
 * 
 * @param sock 客户端套接字
 * @param data Pong 载荷（回显 Ping 的 payload）
 * @param length 载荷长度
 */
void send_pong(SOCKET sock, const uint8_t* data, size_t length) {
    std::vector<uint8_t> frame;
    frame.push_back(0x8A);
    
    if (length <= 125) {
        frame.push_back(static_cast<uint8_t>(length));
    } else if (length <= 65535) {
        frame.push_back(126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    }
    
    frame.insert(frame.end(), data, data + length);
    send(sock, reinterpret_cast<const char*>(frame.data()), frame.size(), 0);
}

/**
 * @brief 发送 Close 帧给客户端
 * 
 * 用于正常关闭 WebSocket 连接（RFC 6455 第 5.5.1 节）。
 * Close 帧的 opcode=0x08，载荷前 2 字节为状态码，后面为可选关闭原因。
 * 
 * @param sock 客户端套接字
 * @param code 关闭状态码（如 1000=正常关闭）
 * @param reason 关闭原因文本（可为空）
 */
void send_close(SOCKET sock, uint16_t code, const std::string& reason) {
    std::vector<uint8_t> frame;
    frame.push_back(0x88);
    
    size_t payload_len = 2 + reason.size();
    if (payload_len <= 125) {
        frame.push_back(static_cast<uint8_t>(payload_len));
    } else if (payload_len <= 65535) {
        frame.push_back(126);
        frame.push_back((payload_len >> 8) & 0xFF);
        frame.push_back(payload_len & 0xFF);
    }
    
    frame.push_back((code >> 8) & 0xFF);
    frame.push_back(code & 0xFF);
    frame.insert(frame.end(), reason.begin(), reason.end());
    
    send(sock, reinterpret_cast<const char*>(frame.data()), frame.size(), 0);
}

/**
 * @brief 将浏览器键盘事件转换为 SDL 事件并推入 SDL 事件队列
 * 
 * 通过 SDL_PushEvent（线程安全）将键盘事件注入 SDL 事件循环，
 * 主线程的 SDL_PollEvent 会拾取这些事件。
 * 
 * 注意：SDL_PushEvent 注入的事件不会更新 SDL_GetKeyboardState() 返回的键盘状态表，
 * 因此 main.cpp 中需要额外维护 ws_key_state[] 数组来追踪浏览器按键状态。
 * 
 * @param type 事件类型："keydown" 或 "keyup"
 * @param keycode JS keyCode 值（如 87=W, 65=A）
 */
void push_sdl_key_event(const std::string& type, int keycode) {
    SDL_Event event;
    SDL_zero(event);
    
    SDL_Scancode scancode = jsKeyCodeToSDL(keycode);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    
    if (type == "keydown") {
        event.type = SDL_KEYDOWN;
        event.key.keysym.scancode = scancode;
        event.key.keysym.sym = static_cast<SDL_KeyCode>(keycode);
    } else if (type == "keyup") {
        event.type = SDL_KEYUP;
        event.key.keysym.scancode = scancode;
        event.key.keysym.sym = static_cast<SDL_KeyCode>(keycode);
    } else {
        return;
    }
    
    printf("[SDL Event] type=%s, keycode=%d, scancode=%d\n",
           type.c_str(), keycode, scancode);
    fflush(stdout);
    
    SDL_PushEvent(&event);
}

/**
 * @brief 将浏览器鼠标事件转换为 SDL 事件并推入 SDL 事件队列
 * @param type 事件类型："mousemove"/"mousedown"/"mouseup"
 * @param x 鼠标 X 坐标
 * @param y 鼠标 Y 坐标
 * @param button 按键编号（0=左键, 1=中键, 2=右键），映射到 SDL_BUTTON_LEFT(1) 等
 */
void push_sdl_mouse_event(const std::string& type, int x, int y, int button) {
    SDL_Event event;
    SDL_zero(event);
    
    if (type == "mousemove") {
        event.type = SDL_MOUSEMOTION;
        event.motion.x = x;
        event.motion.y = y;
    } else if (type == "mousedown") {
        event.type = SDL_MOUSEBUTTONDOWN;
        event.button.x = x;
        event.button.y = y;
        event.button.button = button + 1;
    } else if (type == "mouseup") {
        event.type = SDL_MOUSEBUTTONUP;
        event.button.x = x;
        event.button.y = y;
        event.button.button = button + 1;
    } else {
        return;
    }
    
    printf("[SDL Event] type=%s, x=%d, y=%d, button=%d\n",
           type.c_str(), x, y, button);
    fflush(stdout);
    
    SDL_PushEvent(&event);
}

/**
 * @brief 解析浏览器发来的 JSON 事件字符串，转换为 SDL 事件
 * 
 * 浏览器通过 WebSocket 发送 JSON 格式的事件：
 * - 键盘：{"type":"keydown","keycode":87}
 * - 鼠标移动：{"type":"mousemove","x":100,"y":200}
 * - 鼠标按键：{"type":"mousedown","x":100,"y":200,"button":0}
 * 
 * 每个 JSON 对象被解析后，通过 push_sdl_key_event / push_sdl_mouse_event
 * 转换为 SDL 事件并推送到 SDL 事件队列中。
 * 
 * @param json 一个或多个 JSON 对象组成的字符串
 */
void parse_events(const std::string& json) {
    size_t pos = 0;
    
    while (pos < json.size()) {
        size_t start = json.find('{', pos);
        size_t end = json.find('}', start);
        if (start == std::string::npos || end == std::string::npos) {
            break;
        }
        
        std::string obj = json.substr(start, end - start + 1);
        pos = end + 1;
        
        size_t type_start = obj.find("\"type\":\"");
        if (type_start == std::string::npos) continue;
        type_start += 8;
        size_t type_end = obj.find("\"", type_start);
        if (type_end == std::string::npos) continue;
        std::string event_type = obj.substr(type_start, type_end - type_start);
        
        if (event_type == "keydown" || event_type == "keyup") {
            size_t key_start = obj.find("\"keycode\":");
            if (key_start != std::string::npos) {
                key_start += 10;
                size_t key_end = obj.find("}", key_start);
                if (key_end != std::string::npos) {
                    int keycode = std::stoi(obj.substr(key_start, key_end - key_start));
                    push_sdl_key_event(event_type, keycode);
                }
            }
        } else if (event_type == "mousemove") {
            size_t x_start = obj.find("\"x\":");
            size_t y_start = obj.find("\"y\":");
            if (x_start != std::string::npos && y_start != std::string::npos) {
                x_start += 4;
                y_start += 4;
                size_t x_end = obj.find(",", x_start);
                size_t y_end = obj.find("}", y_start);
                if (x_end != std::string::npos && y_end != std::string::npos) {
                    int x = std::stoi(obj.substr(x_start, x_end - x_start));
                    int y = std::stoi(obj.substr(y_start, y_end - y_start));
                    push_sdl_mouse_event(event_type, x, y, 0);
                }
            }
        } else if (event_type == "mousedown" || event_type == "mouseup") {
            size_t x_start = obj.find("\"x\":");
            size_t y_start = obj.find("\"y\":");
            size_t btn_start = obj.find("\"button\":");
            if (x_start != std::string::npos && y_start != std::string::npos && btn_start != std::string::npos) {
                x_start += 4;
                y_start += 4;
                btn_start += 9;
                size_t x_end = obj.find(",", x_start);
                size_t y_end = obj.find(",", y_start);
                size_t btn_end = obj.find("}", btn_start);
                if (x_end != std::string::npos && y_end != std::string::npos && btn_end != std::string::npos) {
                    int x = std::stoi(obj.substr(x_start, x_end - x_start));
                    int y = std::stoi(obj.substr(y_start, y_end - y_start));
                    int button = std::stoi(obj.substr(btn_start, btn_end - btn_start));
                    push_sdl_mouse_event(event_type, x, y, button);
                }
            }
        }
    }
}

/**
 * @brief 解析单个 WebSocket 数据帧
 * 
 * 按照 RFC 6455 帧格式解析：
 *   字节0: FIN(1bit) + RSV(3bit) + opcode(4bit)
 *   字节1: MASK(1bit) + payload_len(7bit)
 *   字节2-9: 扩展长度（可选）
 *   字节N: 掩码键（可选，客户端发送必须掩码）
 *   剩余: 载荷数据
 * 
 * opcode 类型：
 *   0x01 文本帧 → 解析为 JSON 事件
 *   0x02 二进制帧 → 调试信息
 *   0x08 关闭帧 → 回复关闭确认
 *   0x09 Ping帧 → 回复 Pong
 *   0x0A Pong帧 → 调试信息
 */
void process_websocket_frame(SOCKET sock, const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) {
        return;
    }
    
    uint8_t opcode = frame[0] & 0x0F;
    bool masked = (frame[1] & 0x80) != 0;
    size_t payload_len = frame[1] & 0x7F;
    
    size_t offset = 2;
    
    if (payload_len == 126) {
        if (frame.size() < 4) return;
        payload_len = (static_cast<uint32_t>(frame[2]) << 8) | frame[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (frame.size() < 10) return;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | frame[offset + i];
        }
        offset = 10;
    }
    
    uint8_t mask[4] = {0};
    if (masked) {
        if (frame.size() < offset + 4) return;
        memcpy(mask, frame.data() + offset, 4);
        offset += 4;
    }
    
    if (frame.size() < offset + payload_len) {
        return;
    }
    
    std::vector<uint8_t> payload(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        if (masked) {
            payload[i] = frame[offset + i] ^ mask[i % 4];
        } else {
            payload[i] = frame[offset + i];
        }
    }
    
    switch (opcode) {
        case 0x01: {
            std::string text(payload.begin(), payload.end());
            printf("[WS] Received text: %s\n", text.c_str());
            fflush(stdout);
            parse_events(text);
            break;
        }
        case 0x02: {
            printf("[WS] Received binary: %zu bytes\n", payload.size());
            fflush(stdout);
            break;
        }
        case 0x08: {
            printf("[WS] Received close frame\n");
            fflush(stdout);
            send_close(sock, 1000, "");
            break;
        }
        case 0x09: {
            printf("[WS] Received ping\n");
            fflush(stdout);
            send_pong(sock, payload.data(), payload.size());
            break;
        }
        case 0x0A: {
            printf("[WS] Received pong\n");
            fflush(stdout);
            break;
        }
        default:
            printf("[WS] Unknown opcode: %d\n", opcode);
            fflush(stdout);
            break;
    }
}

// ===========================================================================
// 客户端数据处理（握手 + 帧解析）
// ===========================================================================

/**
 * @brief 处理客户端接收到的数据
 * 
 * 根据 handshake_done 状态分发到不同处理阶段：
 * 
 * Phase 1 — 握手前：
 *   在 receive_buffer 中查找 "\r\n\r\n" 分隔头部，解析后判断：
 *   - WebSocket 升级请求 → 计算 accept_key，返回 101 响应
 *   - 普通 HTTP GET → 返回 client.html 页面，然后关闭连接
 * 
 * Phase 2 — 握手后：
 *   循环从 receive_buffer 中提取完整 WebSocket 帧：
 *   - 先读取帧头（2~10 字节）确定帧大小
 *   - 若缓冲区中数据不足一帧，等待下次 recv
 *   - 完整帧交给 process_websocket_frame 处理
 * 
 * @param client 客户端连接（包含 receive_buffer 累积的数据）
 */
void process_client_data(std::shared_ptr<WebSocketClient> client) {
    if (!client->handshake_done) {
        // Phase 1: 握手前 — 查找 HTTP 头部结束标记
        size_t header_end = client->receive_buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return;  // 头部不完整，等待更多数据
        }
        
        std::string request(client->receive_buffer.begin(),
                             client->receive_buffer.begin() + header_end);
        client->receive_buffer.erase(0, header_end + 4);
        
        std::string upgrade = parse_header(request, "Upgrade");
        std::string connection = parse_header(request, "Connection");
        
        if (upgrade == "websocket" && connection.find("Upgrade") != std::string::npos) {
            // WebSocket 升级握手
            std::string key = parse_header(request, "Sec-WebSocket-Key");
            std::string accept_key = compute_accept_key(key);
            
            std::ostringstream oss;
            oss << "HTTP/1.1 101 Switching Protocols\r\n";
            oss << "Upgrade: websocket\r\n";
            oss << "Connection: Upgrade\r\n";
            oss << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
            oss << "\r\n";
            
            send(client->sock, oss.str().c_str(), oss.str().size(), 0);
            client->handshake_done = true;
            
            printf("[WS] Handshake completed\n");
            fflush(stdout);
        } else {
            // 普通 HTTP 请求 — 返回 client.html
            std::string path;
            size_t get_pos = request.find("GET ");
            if (get_pos != std::string::npos) {
                size_t path_start = get_pos + 4;
                size_t path_end = request.find(" ", path_start);
                if (path_end != std::string::npos) {
                    path = request.substr(path_start, path_end - path_start);
                }
            }
            
            printf("[HTTP] Request for: %s\n", path.c_str());
            fflush(stdout);
            
            if (path == "/" || path == "/index.html") {
                std::string html_content = read_file("client.html");
                if (html_content.empty()) {
                    send_http_response(client->sock, "<html><body>client.html not found</body></html>", 404, "text/html");
                } else {
                    send_http_response(client->sock, html_content, 200, "text/html");
                }
            } else {
                send_http_response(client->sock, "", 404, "text/plain");
            }
            
            client->connected = false;  // HTTP 请求处理完毕，关闭连接
        }
    } else {
        // Phase 2: 握手后 — 循环提取完整 WebSocket 帧
        while (client->receive_buffer.size() >= 2) {
            // 解析帧头，计算帧总大小
            bool masked = (client->receive_buffer[1] & 0x80) != 0;
            size_t payload_len = client->receive_buffer[1] & 0x7F;
            
            size_t frame_size = 2;
            
            if (payload_len == 126) {
                if (client->receive_buffer.size() < 4) break;  // 数据不足，等待
                payload_len = (static_cast<uint32_t>(client->receive_buffer[2]) << 8) | client->receive_buffer[3];
                frame_size = 4;
            } else if (payload_len == 127) {
                if (client->receive_buffer.size() < 10) break;
                payload_len = 0;
                for (int i = 0; i < 8; i++) {
                    payload_len = (payload_len << 8) | client->receive_buffer[2 + i];
                }
                frame_size = 10;
            }
            
            if (masked) {
                frame_size += 4;
            }
            
            frame_size += payload_len;
            
            // 缓冲区数据不足一帧，等待更多数据
            if (client->receive_buffer.size() < frame_size) {
                break;
            }
            
            // 提取完整帧并处理
            std::vector<uint8_t> frame(client->receive_buffer.begin(),
                                        client->receive_buffer.begin() + frame_size);
            client->receive_buffer.erase(0, frame_size);
            
            process_websocket_frame(client->sock, frame);
        }
    }
}

} // anonymous namespace

// ===========================================================================
// WebSocketServer 类实现
// ===========================================================================

/**
 * @brief 构造函数，初始化服务器参数
 * @param port 监听端口号
 */
WebSocketServer::WebSocketServer(int port) 
    : port_(port), running_(false),
      frame_width_(0), frame_height_(0), frame_pitch_(0), frame_version_(0) {
}

/**
 * @brief 析构函数，自动停止服务器并释放资源
 */
WebSocketServer::~WebSocketServer() {
    stop();
}

/**
 * @brief 启动 WebSocket 服务器
 * 
 * 设置运行标志，创建 1 个 acceptor 线程和 POOL_SIZE 个 worker 线程。
 * 非阻塞调用。
 */
void WebSocketServer::start() {
    running_ = true;
    frame_version_ = 0;
    
    // 创建 POOL_SIZE 个 worker 线程
    workers_.reserve(POOL_SIZE);
    for (int i = 0; i < POOL_SIZE; i++) {
        workers_.emplace_back(&WebSocketServer::worker_thread, this, i);
    }
    
    // 创建 acceptor 线程
    acceptor_thread_ = std::thread(&WebSocketServer::acceptor_thread, this);
}

/**
 * @brief 停止 WebSocket 服务器
 * 
 * 1. 清除运行标志，使所有线程的循环退出
 * 2. 唤醒所有等待中的 worker（notify_all）
 * 3. 关闭所有客户端连接
 * 4. 等待所有线程结束（join）
 * 
 * 必须在 SDL 事件循环退出后调用，以确保 SDL_PushEvent 不再被使用。
 */
void WebSocketServer::stop() {
    running_ = false;
    
    // 唤醒所有 worker 线程
    for (int i = 0; i < POOL_SIZE; i++) {
        std::lock_guard<std::mutex> lock(g_workers[i].mutex);
        for (auto& c : g_workers[i].clients) {
            c->connected = false;
            closesocket(c->sock);
        }
        g_workers[i].clients.clear();
        g_workers[i].cv.notify_all();
    }
    
    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }
    
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
}

/**
 * @brief 发送当前帧缓冲区到所有已连接客户端
 * 
 * 将像素数据封装到 shared_ptr 中（仅一次拷贝），递增 frame_version_。
 * 各 worker 线程通过比较 frame_version_ 与本地 last_sent_version 判断是否有新帧，
 * 若有则通过 shared_ptr 共享同一份帧数据（零拷贝）发送给客户端。
 * 
 * @param data 像素数据（ARGB8888 格式）
 * @param width 画面宽度（像素）
 * @param height 画面高度（像素）
 * @param pitch 每行字节数
 */
void WebSocketServer::send_framebuffer(const uint8_t* data, int width, int height, int pitch) {
    auto new_fb = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(height) * static_cast<size_t>(pitch));
    std::memcpy(new_fb->data(), data, new_fb->size());
    
    {
        std::lock_guard<std::mutex> lock(framebuffer_mutex_);
        framebuffer_ = new_fb;
        frame_width_ = width;
        frame_height_ = height;
        frame_pitch_ = pitch;
    }
    frame_version_.fetch_add(1, std::memory_order_release);
}

/**
 * @brief acceptor 线程
 * 
 * 初始化 Winsock，创建监听 socket，进入 accept 循环：
 * 1. WSAStartup 初始化 Winsock 库
 * 2. 创建 socket 并绑定到所有网卡的指定端口
 * 3. listen 开始监听
 * 4. select() 超时等待新连接（1秒，便于检查 running_）
 * 5. accept() 新连接后创建 WebSocketClient，
 *    轮询分配给 worker[next_worker % POOL_SIZE]
 * 6. notify 该 worker 有新客户端
 */
void WebSocketServer::acceptor_thread() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[WS] WSAStartup failed\n");
        return;
    }
    
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        fprintf(stderr, "[WS] Failed to create socket\n");
        WSACleanup();
        return;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(server_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[WS] Bind failed\n");
        closesocket(server_socket);
        WSACleanup();
        return;
    }
    
    if (listen(server_socket, 5) == SOCKET_ERROR) {
        fprintf(stderr, "[WS] Listen failed\n");
        closesocket(server_socket);
        WSACleanup();
        return;
    }
    
    printf("[Server] WebSocket server listening on port %d\n", port_);
    printf("[Server] Open http://localhost:%d in your browser\n", port_);
    fflush(stdout);
    
    while (running_) {
        // 使用 select 超时等待新连接，便于定期检查 running_
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result == 0) {
            continue;  // 超时，检查 running_ 后继续
        }
        
        if (select_result < 0) {
            break;
        }
        
        if (FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr;
            int client_addr_len = sizeof(client_addr);
            
            SOCKET client_socket = accept(server_socket,
                                          reinterpret_cast<sockaddr*>(&client_addr),
                                          reinterpret_cast<int*>(&client_addr_len));
            
            if (client_socket == INVALID_SOCKET) {
                continue;
            }
            
            printf("[WS] New client connected\n");
            fflush(stdout);
            
            auto client = std::make_shared<WebSocketClient>(client_socket);
            
            // 轮询分配给 worker
            int worker_id = g_next_worker.fetch_add(1) % POOL_SIZE;
            
            {
                std::lock_guard<std::mutex> lock(g_workers[worker_id].mutex);
                g_workers[worker_id].clients.push_back(client);
            }
            g_workers[worker_id].cv.notify_one();
            
            printf("[WS] Assigned to worker[%d]\n", worker_id);
            fflush(stdout);
        }
    }
    
    closesocket(server_socket);
    WSACleanup();
}

/**
 * @brief worker 线程（线程池中的工作线程）
 * 
 * 每个 worker 使用 select() 多路复用监听分配给自己的客户端 socket：
 * 
 * 循环流程：
 * 1. 检查帧版本号，若有新帧则广播（shared_ptr 零拷贝）
 * 2. 等待条件变量（有新客户端或超时 15ms）
 * 3. 拷贝客户端列表（加锁后快速释放）
 * 4. 构建 fd_set，select() 等待可读事件（15ms 超时）
 * 5. 对每个可读的客户端：
 *    a. recv 读取数据，追加到 receive_buffer
 *    b. 调用 process_client_data 处理握手或帧解析
 * 6. 移除断开连接的客户端
 * 
 * select 超时设为 15ms（~67 FPS），确保低延迟帧发送。
 * 每个 worker 独立追踪 last_sent_version，互不干扰，避免竞态丢帧。
 * 
 * @param worker_id worker 编号（0 ~ POOL_SIZE-1）
 */
void WebSocketServer::worker_thread(int worker_id) {
    auto& wd = g_workers[worker_id];
    uint64_t last_sent_version = 0;  ///< 本 worker 上次发送的帧版本号
    
    printf("[Worker %d] Started\n", worker_id);
    fflush(stdout);
    
    while (running_) {
        // 1. 先检查是否有新帧需要广播（在 select 前发送，降低延迟）
        uint64_t current_version = frame_version_.load(std::memory_order_acquire);
        if (current_version > last_sent_version) {
            std::shared_ptr<const std::vector<uint8_t>> fb;
            {
                std::lock_guard<std::mutex> fb_lock(framebuffer_mutex_);
                fb = framebuffer_;  // shared_ptr 拷贝（8字节），数据零拷贝
            }
            if (fb && !fb->empty()) {
                // 拷贝客户端列表（避免在发送时持锁）
                std::vector<std::shared_ptr<WebSocketClient>> clients_copy;
                {
                    std::lock_guard<std::mutex> lock(wd.mutex);
                    clients_copy = wd.clients;
                }
                for (auto& c : clients_copy) {
                    if (c->connected && c->handshake_done) {
                        send_websocket_frame(c->sock, fb->data(), fb->size(), 0x02);
                    }
                }
            }
            last_sent_version = current_version;
        }
        
        // 2. 等待有客户端或超时（15ms，保证高帧率）
        std::vector<std::shared_ptr<WebSocketClient>> my_clients;
        {
            std::unique_lock<std::mutex> lock(wd.mutex);
            wd.cv.wait_for(lock, std::chrono::milliseconds(15), [&] {
                return !running_ || !wd.clients.empty();
            });
            if (!running_) break;
            my_clients = wd.clients;
        }
        
        if (my_clients.empty()) {
            continue;
        }
        
        // 3. 构建 fd_set 进行 select 多路复用
        fd_set read_fds;
        FD_ZERO(&read_fds);
        SOCKET max_fd = 0;
        int active_count = 0;
        
        for (auto& c : my_clients) {
            if (c->connected) {
                FD_SET(c->sock, &read_fds);
                if (c->sock > max_fd) max_fd = c->sock;
                active_count++;
            }
        }
        
        if (active_count == 0) {
            continue;
        }
        
        // 4. select 超时 15ms，保证高频检查新帧
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 15000;  // 15ms
        
        int select_result = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result <= 0) {
            continue;  // 超时或被信号中断，回到循环开头检查新帧
        }
        
        // 5. 处理可读的客户端 socket
        std::vector<std::shared_ptr<WebSocketClient>> to_remove;
        
        for (auto& c : my_clients) {
            if (c->connected && FD_ISSET(c->sock, &read_fds)) {
                char buffer[4096];
                int bytes_received = recv(c->sock, buffer, sizeof(buffer), 0);
                
                if (bytes_received <= 0) {
                    // 连接关闭或出错
                    c->connected = false;
                    to_remove.push_back(c);
                    printf("[WS] Client disconnected (worker %d)\n", worker_id);
                    fflush(stdout);
                    continue;
                }
                
                // 追加数据到接收缓冲区并处理
                c->receive_buffer.append(buffer, bytes_received);
                process_client_data(c);
                
                // HTTP 请求处理完毕后 connected 被设为 false
                if (!c->connected) {
                    to_remove.push_back(c);
                }
            }
        }
        
        // 6. 从 worker 列表中移除断开的客户端
        if (!to_remove.empty()) {
            std::lock_guard<std::mutex> lock(wd.mutex);
            for (auto& c : to_remove) {
                auto it = std::find(wd.clients.begin(), wd.clients.end(), c);
                if (it != wd.clients.end()) {
                    closesocket(c->sock);
                    wd.clients.erase(it);
                }
            }
        }
    }
    
    printf("[Worker %d] Stopped\n", worker_id);
    fflush(stdout);
}
