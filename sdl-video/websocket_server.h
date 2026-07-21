#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <thread>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <atomic>

struct Event {
    std::string type;
    int x, y;
    int button;
    int keycode;
    bool pressed;
};

class WebSocketServer {
public:
    WebSocketServer(int port = 8080);
    ~WebSocketServer();
    
    void start();
    void stop();
    
    void send_framebuffer(const uint8_t* data, int width, int height, int pitch);
    bool has_events();
    Event get_event();
    
private:
    void server_thread();
    
    int port_;
    std::thread thread_;
    std::atomic<bool> running_;
    
    std::mutex events_mutex_;
    std::queue<Event> events_;
    
    std::mutex framebuffer_mutex_;
    std::vector<uint8_t> framebuffer_;
    int frame_width_;
    int frame_height_;
    int frame_pitch_;
    std::atomic<bool> new_frame_;
};

#endif
