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

        const Uint8* keystate = SDL_GetKeyboardState(nullptr);

        if (keystate[SDL_SCANCODE_W]) posY -= 5.0f;
        if (keystate[SDL_SCANCODE_S]) posY += 5.0f;
        if (keystate[SDL_SCANCODE_A]) posX -= 5.0f;
        if (keystate[SDL_SCANCODE_D]) posX += 5.0f;
        if (keystate[SDL_SCANCODE_Q]) rotation -= 0.05f;
        if (keystate[SDL_SCANCODE_E]) rotation += 0.05f;
        if (keystate[SDL_SCANCODE_R]) scale += 0.02f;
        if (keystate[SDL_SCANCODE_F]) scale = fmax(0.1f, scale - 0.02f);

        while (ws_server.has_events()) {
            Event event = ws_server.get_event();
            if (event.type == "keydown") {
                switch (event.keycode) {
                    case 87: posY -= 5.0f; break;
                    case 83: posY += 5.0f; break;
                    case 65: posX -= 5.0f; break;
                    case 68: posX += 5.0f; break;
                    case 81: rotation -= 0.05f; break;
                    case 69: rotation += 0.05f; break;
                    case 82: scale += 0.02f; break;
                    case 70: scale = fmax(0.1f, scale - 0.02f); break;
                }
            }
        }

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

        void* pixels;
        int pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels, pitch);
            ws_server.send_framebuffer((const uint8_t*)pixels, SCREEN_WIDTH, SCREEN_HEIGHT, pitch);
            SDL_UnlockTexture(texture);
        }

        SDL_Delay(16);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}