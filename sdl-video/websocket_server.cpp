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
 * 线程模型：
 * - 主线程（UI）：运行 SDL 事件循环和渲染
 * - 服务器线程：监听端口，接受新连接
 * - 客户端线程：每个客户端独立处理握手、收发数据
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
#include <vector>
#include <atomic>
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace {

/**
 * @brief 客户端连接状态
 */
struct WebSocketClient {
    SOCKET sock;
    bool handshake_done;
    bool connected;
    
    WebSocketClient(SOCKET s) : sock(s), handshake_done(false), connected(true) {}
};

struct ServerState {
    std::mutex framebuffer_mutex;
    std::vector<uint8_t> framebuffer;
    int frame_width;
    int frame_height;
    int frame_pitch;
    bool new_frame;
    
    std::mutex clients_mutex;
    std::vector<std::shared_ptr<WebSocketClient>> clients;
    
    std::atomic<bool> running;
};

/**
 * @brief 全局服务器状态（进程内唯一）
 * 
 * 管理所有客户端连接、帧缓冲区和运行状态。
 * 所有客户端线程通过此共享状态与主线程通信。
 */
ServerState g_server_state;

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
 * 主线程的 SDL_PollEvent 会拾取这些事件，
 * SDL_GetKeyboardState() 也会正确反映按键状态。
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

/**
 * @brief 客户端处理线程
 * 
 * 每个 WebSocket 连接在独立线程中处理：
 * 1. 使用 select() 实现非阻塞/超时读取
 * 2. 首次收到数据时判断是 HTTP 请求还是 WebSocket 升级请求
 *    - HTTP GET "/" → 返回 client.html 页面
 *    - WebSocket 升级握手 → 计算 accept_key 并返回 101 响应
 * 3. 握手完成后，持续解析 WebSocket 数据帧
 *    - 数据帧可能被分片，需要在 receive_buffer 中累积
 *    - 完整帧交给 process_websocket_frame 处理
 * 
 * @param client 客户端连接的智能指针
 */
void client_thread(std::shared_ptr<WebSocketClient> client) {
    char buffer[4096];
    std::string receive_buffer;
    
    printf("[WS] Client thread started\n");
    fflush(stdout);
    
    while (client->connected && g_server_state.running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->sock, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(client->sock + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result == 0) {
            continue;
        }
        
        if (select_result < 0) {
            break;
        }
        
        int bytes_received = recv(client->sock, buffer, sizeof(buffer), 0);
        
        if (bytes_received <= 0) {
            break;
        }
        
        receive_buffer.insert(receive_buffer.end(), buffer, buffer + bytes_received);
        
        if (!client->handshake_done) {
            size_t header_end = receive_buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string request(receive_buffer.begin(), receive_buffer.begin() + header_end);
                receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + header_end + 4);
                
                std::string upgrade = parse_header(request, "Upgrade");
                std::string connection = parse_header(request, "Connection");
                
                if (upgrade == "websocket" && connection.find("Upgrade") != std::string::npos) {
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
                    
                    client->connected = false;
                    break;
                }
            }
        } else {
            while (receive_buffer.size() >= 2) {
                bool masked = (receive_buffer[1] & 0x80) != 0;
                size_t payload_len = receive_buffer[1] & 0x7F;
                
                size_t frame_size = 2;
                
                if (payload_len == 126) {
                    if (receive_buffer.size() < 4) break;
                    payload_len = (static_cast<uint32_t>(receive_buffer[2]) << 8) | receive_buffer[3];
                    frame_size = 4;
                } else if (payload_len == 127) {
                    if (receive_buffer.size() < 10) break;
                    payload_len = 0;
                    for (int i = 0; i < 8; i++) {
                        payload_len = (payload_len << 8) | receive_buffer[2 + i];
                    }
                    frame_size = 10;
                }
                
                if (masked) {
                    frame_size += 4;
                }
                
                frame_size += payload_len;
                
                if (receive_buffer.size() < frame_size) {
                    break;
                }
                
                std::vector<uint8_t> frame(receive_buffer.begin(), receive_buffer.begin() + frame_size);
                receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + frame_size);
                
                process_websocket_frame(client->sock, frame);
            }
        }
    }
    
    client->connected = false;
    
    std::lock_guard<std::mutex> lock(g_server_state.clients_mutex);
    auto it = std::find(g_server_state.clients.begin(), g_server_state.clients.end(), client);
    if (it != g_server_state.clients.end()) {
        g_server_state.clients.erase(it);
    }
    
    closesocket(client->sock);
    printf("[WS] Client disconnected\n");
    fflush(stdout);
}

/**
 * @brief 触发帧数据广播
 * 
 * 遍历所有已连接且完成握手的客户端，
 * 将最新帧数据以 WebSocket 二进制帧（opcode 0x02）形式发送。
 * 发送完毕后清除 new_frame 标志。
 */
void trigger_writeable() {
    std::lock_guard<std::mutex> lock(g_server_state.clients_mutex);
    for (auto& client : g_server_state.clients) {
        if (client->connected && client->handshake_done) {
            std::lock_guard<std::mutex> fb_lock(g_server_state.framebuffer_mutex);
            if (g_server_state.new_frame && !g_server_state.framebuffer.empty()) {
                send_websocket_frame(client->sock, g_server_state.framebuffer.data(), 
                                     g_server_state.framebuffer.size(), 0x02);
            }
        }
    }
    
    std::lock_guard<std::mutex> fb_lock(g_server_state.framebuffer_mutex);
    g_server_state.new_frame = false;
}

}

WebSocketServer::WebSocketServer(int port) 
    : port_(port), running_(false),
      frame_width_(0), frame_height_(0), frame_pitch_(0), new_frame_(false) {
}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::start() {
    running_ = true;
    g_server_state.running = true;
    thread_ = std::thread(&WebSocketServer::server_thread, this);
}

void WebSocketServer::stop() {
    running_ = false;
    g_server_state.running = false;
    
    std::lock_guard<std::mutex> lock(g_server_state.clients_mutex);
    for (auto& client : g_server_state.clients) {
        client->connected = false;
        closesocket(client->sock);
    }
    g_server_state.clients.clear();
    
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WebSocketServer::send_framebuffer(const uint8_t* data, int width, int height, int pitch) {
    {
        std::lock_guard<std::mutex> lock(g_server_state.framebuffer_mutex);
        g_server_state.framebuffer.resize(static_cast<size_t>(height) * static_cast<size_t>(pitch));
        std::memcpy(g_server_state.framebuffer.data(), data, g_server_state.framebuffer.size());
        g_server_state.frame_width = width;
        g_server_state.frame_height = height;
        g_server_state.frame_pitch = pitch;
        g_server_state.new_frame = true;
    }
    
    trigger_writeable();
}

/**
 * @brief 服务器主线程
 * 
 * 初始化 Winsock，创建监听 socket，进入 accept 循环：
 * 1. WSAStartup 初始化 Winsock 库
 * 2. 创建 socket 并绑定到所有网卡的指定端口
 * 3. listen 开始监听，最多 5 个等待连接
 * 4. 使用 select() 超时机制（1秒）等待新连接
 * 5. 接受新连接后创建对应的 WebSocketClient 和客户端线程
 */
void WebSocketServer::server_thread() {
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
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result == 0) {
            continue;
        }
        
        if (select_result < 0) {
            break;
        }
        
        if (FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr;
            int client_addr_len = sizeof(client_addr);
            
            SOCKET client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_addr), 
                                          reinterpret_cast<int*>(&client_addr_len));
            
            if (client_socket == INVALID_SOCKET) {
                continue;
            }
            
            printf("[WS] New client connected\n");
            fflush(stdout);
            
            auto client = std::make_shared<WebSocketClient>(client_socket);
            
            std::lock_guard<std::mutex> lock(g_server_state.clients_mutex);
            g_server_state.clients.push_back(client);
            
            std::thread t(client_thread, client);
            t.detach();
        }
    }
    
    closesocket(server_socket);
    WSACleanup();
}
