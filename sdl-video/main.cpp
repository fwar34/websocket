/**
 * @file main.cpp
 * @brief SDL2 红色正方形渲染程序 — 支持本地键鼠和浏览器远程控制
 * 
 * 本文件是 UI 主线程的入口，负责 SDL 渲染和事件处理。
 * 完整的系统框架图、WebSocket 协议说明和线程模型详见 websocket_server.h。
 *
 * 主线程与 WebSocket 服务器的交互方式：
 *
 *   ┌─ main.cpp (主线程) ──────────────────────────────────────┐
 *   │                                                          │
 *   │   WebSocketServer ws(8080);  ws.start();  ← 启动服务器    │
 *   │                                                          │
 *   │   while (running) {                                      │
 *   │     SDL_PollEvent()  ← 拾取 client_thread 注入的 SDL 事件  │
 *   │     keystate = SDL_GetKeyboardState()  ← 本地键盘状态      │
 *   │     渲染 → RenderReadPixels → ws.send_framebuffer()       │
 *   │   }                                                      │
 *   │                                                          │
 *   │   ws.stop();  ← 停止服务器，等待所有线程退出               │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 主循环流程：
 * 1. 初始化 SDL 视频子系统，创建窗口、渲染器、纹理
 * 2. 启动 WebSocket 服务器（独立线程，处理浏览器连接）
 * 3. 主循环（~120 FPS）：
 *    a. SDL_PollEvent 处理事件：
 *       - SDL_QUIT 退出
 *       - SDL_KEYDOWN/SDL_KEYUP 更新 ws_key_state（浏览器按键状态）
 *         注：SDL_PushEvent 注入的事件不会更新 SDL_GetKeyboardState()，
 *            因此需手动维护浏览器按键状态表
 *    b. 读取键盘状态：keystate（本地）|| ws_key_state（浏览器）逻辑或
 *    c. 更新变换参数（平移、旋转、缩放）
 *    d. 绘制红色正方形（使用 SDL 多边形连线方式）
 *    e. 读取帧像素并通过 WebSocket 发送给浏览器
 *    f. 延时 8ms 控制帧率
 * 
 * 控制方式：
 * - W/S: 上下平移
 * - A/D: 左右平移
 * - Q/E: 逆时针/顺时针旋转
 * - R/F: 放大/缩小
 * - 以上按键本地键盘和浏览器远程均可使用
 */

#include <SDL2/SDL.h>
#include <cmath>
#include "websocket_server.h"

const int SCREEN_WIDTH = 640;
const int SCREEN_HEIGHT = 480;
const int SQUARE_SIZE = 100;

int main(int, char*[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "SDL WebSocket Renderer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH, SCREEN_HEIGHT
    );

    WebSocketServer ws_server(8080);
    ws_server.start();

    float posX = SCREEN_WIDTH / 2.0f;
    float posY = SCREEN_HEIGHT / 2.0f;
    float rotation = 0.0f;
    float scale = 1.0f;

    bool running = true;
    SDL_Event e;

    // 浏览器按键状态表。
    // SDL_PushEvent 注入的 SDL_KEYDOWN/SDL_KEYUP 不会更新 SDL_GetKeyboardState()，
    // 因此需要在此手动维护浏览器端按键的按下/释放状态。
    // 本地键盘状态由 SDL_GetKeyboardState() 提供，两者逻辑或后统一判断。
    bool ws_key_state[SDL_NUM_SCANCODES] = {};

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN) {
                ws_key_state[e.key.keysym.scancode] = true;
            } else if (e.type == SDL_KEYUP) {
                ws_key_state[e.key.keysym.scancode] = false;
            }
        }

        // keystate 反映本地硬件键盘状态；
        // ws_key_state 反映浏览器远程键盘状态（由 SDL_PushEvent 注入的事件维护）。
        // 两者逻辑或，使本地和远程按键都能控制矩形。
        const Uint8* keystate = SDL_GetKeyboardState(nullptr);

        if (keystate[SDL_SCANCODE_W] || ws_key_state[SDL_SCANCODE_W]) posY -= 5.0f;
        if (keystate[SDL_SCANCODE_S] || ws_key_state[SDL_SCANCODE_S]) posY += 5.0f;
        if (keystate[SDL_SCANCODE_A] || ws_key_state[SDL_SCANCODE_A]) posX -= 5.0f;
        if (keystate[SDL_SCANCODE_D] || ws_key_state[SDL_SCANCODE_D]) posX += 5.0f;
        if (keystate[SDL_SCANCODE_Q] || ws_key_state[SDL_SCANCODE_Q]) rotation -= 0.05f;
        if (keystate[SDL_SCANCODE_E] || ws_key_state[SDL_SCANCODE_E]) rotation += 0.05f;
        if (keystate[SDL_SCANCODE_R] || ws_key_state[SDL_SCANCODE_R]) scale += 0.02f;
        if (keystate[SDL_SCANCODE_F] || ws_key_state[SDL_SCANCODE_F]) scale = fmax(0.1f, scale - 0.02f);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);

        int size = (int)(SQUARE_SIZE * scale);

        // 旋转矩阵：将正方形四个顶点绕中心 (posX, posY) 旋转
        // [x'] = [cos -sin] [dx]     [y'] = [sin  cos] [dy]
        float cosRot = cos(rotation);
        float sinRot = sin(rotation);

        // 计算旋转后的四个顶点（左上、右上、右下、左下）
        SDL_Point points[4];
        points[0] = { (int)(posX + (-size / 2) * cosRot - (-size / 2) * sinRot),
                      (int)(posY + (-size / 2) * sinRot + (-size / 2) * cosRot) };
        points[1] = { (int)(posX + (size / 2) * cosRot - (-size / 2) * sinRot),
                      (int)(posY + (size / 2) * sinRot + (-size / 2) * cosRot) };
        points[2] = { (int)(posX + (size / 2) * cosRot - (size / 2) * sinRot),
                      (int)(posY + (size / 2) * sinRot + (size / 2) * cosRot) };
        points[3] = { (int)(posX + (-size / 2) * cosRot - (size / 2) * sinRot),
                      (int)(posY + (-size / 2) * sinRot + (size / 2) * cosRot) };

        // 依次连接四个顶点绘制正方形边框
        SDL_RenderDrawLine(renderer, points[0].x, points[0].y, points[1].x, points[1].y);
        SDL_RenderDrawLine(renderer, points[1].x, points[1].y, points[2].x, points[2].y);
        SDL_RenderDrawLine(renderer, points[2].x, points[2].y, points[3].x, points[3].y);
        SDL_RenderDrawLine(renderer, points[3].x, points[3].y, points[0].x, points[0].y);

        SDL_RenderPresent(renderer);

        // 读取渲染器像素并通过 WebSocket 发送给浏览器
        // 使用 STREAMING 纹理作为中间缓冲，RenderReadPixels 读取后拷贝到纹理
        void* pixels;
        int pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels, pitch);
            ws_server.send_framebuffer((const uint8_t*)pixels, SCREEN_WIDTH, SCREEN_HEIGHT, pitch);
            SDL_UnlockTexture(texture);
        }

        // 延时控制帧率（~120 FPS）
        SDL_Delay(8);
    }

    ws_server.stop();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
