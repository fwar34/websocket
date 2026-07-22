/**
 * @file main.cpp
 * @brief SDL2 红色正方形渲染程序 — 支持本地键鼠和浏览器远程控制
 * 
 * 主循环流程：
 * 1. 初始化 SDL 视频子系统，创建窗口、渲染器、纹理
 * 2. 启动 WebSocket 服务器（独立线程，处理浏览器连接）
 * 3. 主循环（~120 FPS）：
 *    a. SDL_PollEvent 处理事件（SDL_QUIT 退出）
 *       注：浏览器键鼠事件通过 SDL_PushEvent 注入，
 *          被 SDL_PollEvent 处理后会更新 SDL_GetKeyboardState()
 *    b. 读取键盘状态 SDL_GetKeyboardState()，同时响应本地和浏览器按键
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

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }

        // SDL_GetKeyboardState 返回所有按键的当前状态。
        // 当浏览器端按键时，WebSocket 线程通过 SDL_PushEvent 注入
        // SDL_KEYDOWN/SDL_KEYUP 事件，这些事件被 SDL_PollEvent 处理后，
        // SDL 内部会更新键盘状态表，因此此处的 keystate 同时反映
        // 本地键盘和浏览器远程键盘的状态，实现统一的输入处理。
        const Uint8* keystate = SDL_GetKeyboardState(nullptr);

        if (keystate[SDL_SCANCODE_W]) posY -= 5.0f;
        if (keystate[SDL_SCANCODE_S]) posY += 5.0f;
        if (keystate[SDL_SCANCODE_A]) posX -= 5.0f;
        if (keystate[SDL_SCANCODE_D]) posX += 5.0f;
        if (keystate[SDL_SCANCODE_Q]) rotation -= 0.05f;
        if (keystate[SDL_SCANCODE_E]) rotation += 0.05f;
        if (keystate[SDL_SCANCODE_R]) scale += 0.02f;
        if (keystate[SDL_SCANCODE_F]) scale = fmax(0.1f, scale - 0.02f);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);

        int size = (int)(SQUARE_SIZE * scale);

        float cosRot = cos(rotation);
        float sinRot = sin(rotation);

        SDL_Point points[4];
        points[0] = { (int)(posX + (-size / 2) * cosRot - (-size / 2) * sinRot),
                      (int)(posY + (-size / 2) * sinRot + (-size / 2) * cosRot) };
        points[1] = { (int)(posX + (size / 2) * cosRot - (-size / 2) * sinRot),
                      (int)(posY + (size / 2) * sinRot + (-size / 2) * cosRot) };
        points[2] = { (int)(posX + (size / 2) * cosRot - (size / 2) * sinRot),
                      (int)(posY + (size / 2) * sinRot + (size / 2) * cosRot) };
        points[3] = { (int)(posX + (-size / 2) * cosRot - (size / 2) * sinRot),
                      (int)(posY + (-size / 2) * sinRot + (size / 2) * cosRot) };

        SDL_RenderDrawLine(renderer, points[0].x, points[0].y, points[1].x, points[1].y);
        SDL_RenderDrawLine(renderer, points[1].x, points[1].y, points[2].x, points[2].y);
        SDL_RenderDrawLine(renderer, points[2].x, points[2].y, points[3].x, points[3].y);
        SDL_RenderDrawLine(renderer, points[3].x, points[3].y, points[0].x, points[0].y);

        SDL_RenderPresent(renderer);

        // 读取当前渲染像素并发送给浏览器
        void* pixels;
        int pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels, pitch);
            ws_server.send_framebuffer((const uint8_t*)pixels, SCREEN_WIDTH, SCREEN_HEIGHT, pitch);
            SDL_UnlockTexture(texture);
        }

        SDL_Delay(8);
    }

    ws_server.stop();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
