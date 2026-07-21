#include "websocket_server.h"
#include <libwebsockets.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <iostream>
#include <malloc.h>
#include <vector>
#include <algorithm>

namespace {

struct PerSessionData {
    bool connected;
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
    
    std::mutex connections_mutex;
    std::vector<struct lws*> connections;
    
    std::atomic<bool> running;
};

ServerState g_server_state;

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
        
        size_t type_start = obj.find("\"type\":\"") + 7;
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
            std::lock_guard<std::mutex> lock(g_server_state.events_mutex);
            g_server_state.events.push(event);
        }
    }
}

int callback_websocket(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    PerSessionData *pss = (PerSessionData *)user;
    
    switch (reason) {
        case LWS_CALLBACK_HTTP: {
            char path[256];
            lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI);
            printf("[HTTP] Request for: %s\n", path);
            fflush(stdout);
            
            if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
                if (lws_serve_http_file(wsi, "client.html", "text/html", NULL, 0)) {
                    return -1;
                }
            } else {
                return -1;
            }
            break;
        }
            
        case LWS_CALLBACK_ESTABLISHED: {
            pss->connected = true;
            printf("[WS] Client connected\n");
            fflush(stdout);
            
            std::lock_guard<std::mutex> lock(g_server_state.connections_mutex);
            g_server_state.connections.push_back(wsi);
            
            lws_callback_on_writable(wsi);
            break;
        }
            
        case LWS_CALLBACK_CLOSED: {
            pss->connected = false;
            printf("[WS] Client disconnected\n");
            fflush(stdout);
            
            std::lock_guard<std::mutex> lock(g_server_state.connections_mutex);
            auto it = std::find(g_server_state.connections.begin(), 
                                g_server_state.connections.end(), wsi);
            if (it != g_server_state.connections.end()) {
                g_server_state.connections.erase(it);
            }
            break;
        }
            
        case LWS_CALLBACK_SERVER_WRITEABLE: {
            bool has_frame = false;
            std::vector<uint8_t> frame_data;
            
            {
                std::lock_guard<std::mutex> lock(g_server_state.framebuffer_mutex);
                if (g_server_state.new_frame && !g_server_state.framebuffer.empty()) {
                    has_frame = true;
                    frame_data = g_server_state.framebuffer;
                    g_server_state.new_frame = false;
                }
            }
            
            if (has_frame) {
                printf("[WS] Sending frame: %zu bytes\n", frame_data.size());
                fflush(stdout);
                
                unsigned char *buf = (unsigned char *)malloc(LWS_PRE + frame_data.size());
                memcpy(buf + LWS_PRE, frame_data.data(), frame_data.size());
                
                lws_write(wsi, buf + LWS_PRE, frame_data.size(), LWS_WRITE_BINARY);
                free(buf);
            }
            
            lws_callback_on_writable(wsi);
            break;
        }
            
        case LWS_CALLBACK_RECEIVE: {
            std::string json(reinterpret_cast<char*>(in), len);
            printf("[WS] Received event: %s\n", json.c_str());
            fflush(stdout);
            parse_events(json);
            break;
        }
            
        default:
            break;
    }
    return 0;
}

struct lws_protocols protocols[] = {
    {
        "ws-protocol",
        callback_websocket,
        sizeof(PerSessionData),
        0,
    },
    { NULL, NULL, 0, 0 }
};

void trigger_writeable() {
    std::lock_guard<std::mutex> lock(g_server_state.connections_mutex);
    for (auto wsi : g_server_state.connections) {
        lws_callback_on_writable(wsi);
    }
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
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WebSocketServer::send_framebuffer(const uint8_t* data, int width, int height, int pitch) {
    std::lock_guard<std::mutex> lock(g_server_state.framebuffer_mutex);
    g_server_state.framebuffer.resize(static_cast<size_t>(height) * static_cast<size_t>(pitch));
    std::memcpy(g_server_state.framebuffer.data(), data, g_server_state.framebuffer.size());
    g_server_state.frame_width = width;
    g_server_state.frame_height = height;
    g_server_state.frame_pitch = pitch;
    g_server_state.new_frame = true;
    
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
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = port_;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
    
    info.protocols = protocols;
    
    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "[WS] Failed to create context\n");
        return;
    }
    
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
    
    printf("[Server] WebSocket server listening on port %d\n", port_);
    printf("[Server] Open http://localhost:%d in your browser\n", port_);
    fflush(stdout);
    
    while (running_) {
        lws_service(context, 100);
    }
    
    lws_context_destroy(context);
}
