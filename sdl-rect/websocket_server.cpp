#include "websocket_server.h"
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
#include <queue>
#include <vector>
#include <atomic>
#include <algorithm>
#include <memory>

namespace {

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
    
    std::mutex events_mutex;
    std::queue<Event> events;
    
    std::mutex clients_mutex;
    std::vector<std::shared_ptr<WebSocketClient>> clients;
    
    std::atomic<bool> running;
};

ServerState g_server_state;

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

void send_ping(SOCKET sock) {
    uint8_t ping_data[] = {0x89, 0x00};
    send(sock, reinterpret_cast<const char*>(ping_data), 2, 0);
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
        
        Event event{};
        
        size_t type_start = obj.find("\"type\":\"") + 8;
        size_t type_end = obj.find("\"", type_start);
        if (type_start != std::string::npos && type_end != std::string::npos) {
            event.type = obj.substr(type_start, type_end - type_start);
        }
        
        if (event.type == "mousemove") {
            size_t x_start = obj.find("\"x\":") + 4;
            size_t x_end = obj.find(",", x_start);
            size_t y_start = obj.find("\"y\":") + 4;
            size_t y_end = obj.find("}", y_start);
            
            if (x_start != std::string::npos && x_end != std::string::npos && 
                y_start != std::string::npos && y_end != std::string::npos) {
                event.x = std::stoi(obj.substr(x_start, x_end - x_start));
                event.y = std::stoi(obj.substr(y_start, y_end - y_start));
            }
        } else if (event.type == "mousedown" || event.type == "mouseup") {
            size_t x_start = obj.find("\"x\":") + 4;
            size_t x_end = obj.find(",", x_start);
            size_t y_start = obj.find("\"y\":") + 4;
            size_t y_end = obj.find(",", y_start);
            size_t btn_start = obj.find("\"button\":") + 9;
            size_t btn_end = obj.find("}", btn_start);
            
            if (x_start != std::string::npos && x_end != std::string::npos && 
                y_start != std::string::npos && y_end != std::string::npos &&
                btn_start != std::string::npos && btn_end != std::string::npos) {
                event.x = std::stoi(obj.substr(x_start, x_end - x_start));
                event.y = std::stoi(obj.substr(y_start, y_end - y_start));
                event.button = std::stoi(obj.substr(btn_start, btn_end - btn_start));
                event.pressed = (event.type == "mousedown");
            }
        } else if (event.type == "keydown" || event.type == "keyup") {
            size_t key_start = obj.find("\"keycode\":") + 10;
            size_t key_end = obj.find("}", key_start);
            
            if (key_start != std::string::npos && key_end != std::string::npos) {
                event.keycode = std::stoi(obj.substr(key_start, key_end - key_start));
                event.pressed = (event.type == "keydown");
            }
        }
        
        if (!event.type.empty()) {
            printf("[Event] type=%s, keycode=%d, x=%d, y=%d, button=%d\n",
                   event.type.c_str(), event.keycode, event.x, event.y, event.button);
            fflush(stdout);
            std::lock_guard<std::mutex> lock(g_server_state.events_mutex);
            g_server_state.events.push(event);
        }
    }
}

void process_websocket_frame(SOCKET sock, const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) {
        return;
    }
    
    uint8_t opcode = frame[0] & 0x0F;
    bool fin = (frame[0] & 0x80) != 0;
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
                uint8_t opcode = receive_buffer[0] & 0x0F;
                bool fin = (receive_buffer[0] & 0x80) != 0;
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

bool WebSocketServer::has_events() {
    std::lock_guard<std::mutex> lock(g_server_state.events_mutex);
    return !g_server_state.events.empty();
}

Event WebSocketServer::get_event() {
    std::lock_guard<std::mutex> lock(g_server_state.events_mutex);
    if (g_server_state.events.empty()) {
        return {"", 0, 0, 0, 0, false};
    }
    Event e = g_server_state.events.front();
    g_server_state.events.pop();
    return e;
}

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
