/**
 * @file main.cpp
 * @brief FFmpeg 视频解码 + WebSocket 推流示例
 * 
 * 使用 FFmpeg 库解码视频文件/RTSP 流，将解码后的 ARGB8888 帧
 * 通过 WebSocket 推送给浏览器客户端，同时保留 SDL 本地预览窗口。
 * 
 * 系统架构：
 * 
 *   ┌─ 解码线程 ─────────────────────────────────────────────────┐
 *   │                                                            │
 *   │  avformat_open_input(url)  ← 打开视频源                   │
 *   │  avformat_find_stream_info() ← 获取流信息                  │
 *   │  avcodec_alloc_context3()  ← 创建解码上下文                │
 *   │  avcodec_open2()          ← 打开解码器                     │
 *   │                                                            │
 *   │  while (running) {                                         │
 *   │    av_read_frame()        ← 读取压缩包                    │
 *   │    avcodec_send_packet()  ← 发送压缩包到解码器              │
 *   │    avcodec_receive_frame()← 获取解压帧                     │
 *   │    sws_scale()            ← 转码为 ARGB8888                │
 *   │    ws_server.send_framebuffer() ← 推送给浏览器              │
 *   │    SDL 渲染 → 本地预览                                     │
 *   │  }                                                        │
 *   │                                                            │
 *   └────────────────────────────────────────────────────────────┘
 *   主线程 (SDL) ← 浏览器键鼠事件 → WebSocket → SDL_PushEvent
 *
 * 用法：
 *   ffmpeg_server <视频文件路径或 RTSP URL>
 *   例如：ffmpeg_server ./test.mp4
 *         ffmpeg_server rtsp://localhost:8554/test
 */

#include <SDL2/SDL.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cmath>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "websocket_server.h"

// ============================================================================
// 配置常量
// ============================================================================

const int DEFAULT_WIDTH  = 640;   ///< 输出画面宽度
const int DEFAULT_HEIGHT = 480;   ///< 输出画面高度
const int TARGET_FPS     = 30;    ///< 目标帧率
const int WEBSOCKET_PORT = 8080;  ///< WebSocket 服务端口

// ============================================================================
// 全局状态
// ============================================================================

struct DecoderContext {
    AVFormatContext* fmt_ctx    = nullptr;  ///< 格式上下文
    AVCodecContext*  video_ctx  = nullptr;  ///< 视频解码上下文
    AVCodecContext*  audio_ctx  = nullptr;  ///< 音频解码上下文
    SwsContext*      sws_ctx    = nullptr;  ///< 视频格式转换上下文
    SwrContext*      swr_ctx    = nullptr;  ///< 音频重采样上下文
    AVFrame*         video_frame = nullptr; ///< 视频解码帧
    AVFrame*         audio_frame = nullptr; ///< 音频解码帧
    AVFrame*         rgb_frame  = nullptr;  ///< ARGB8888 转换帧
    int              video_stream_idx = -1; ///< 视频流索引
    int              audio_stream_idx = -1; ///< 音频流索引
    int              output_width  = DEFAULT_WIDTH;
    int              output_height = DEFAULT_HEIGHT;
    std::string      input_url;             ///< 输入源地址
};

static DecoderContext g_decoder;
static WebSocketServer* g_ws_server = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_paused{false};
static std::atomic<bool> g_no_audio{false}; ///< 是否禁用音频
static std::atomic<int64_t> g_seek_target{-1}; ///< seek 目标时间（ms），-1=无效

// ============================================================================
// 音频播放（SDL Audio）
// ============================================================================

static std::mutex g_audio_mutex;             ///< 音频缓冲区互斥锁
static std::vector<uint8_t> g_audio_buffer;  ///< PCM 音频缓冲区（S16 stereo 44100Hz）
static constexpr size_t AUDIO_BUFFER_MAX = 44100 * 4 * 2; ///< 最大缓冲 2 秒音频

/**
 * @brief SDL 音频回调函数
 * 
 * SDL 音频设备需要数据时调用此函数。
 * 从全局音频缓冲区中读取 PCM 数据填充到 stream 中。
 * 如果缓冲区数据不足，用静音填充。
 * 
 * @param userdata 用户数据（未使用）
 * @param stream 输出音频流
 * @param len 需要的字节数
 */
void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    
    if (g_audio_buffer.empty()) {
        memset(stream, 0, len);
        return;
    }
    
    if (g_audio_buffer.size() >= static_cast<size_t>(len)) {
        memcpy(stream, g_audio_buffer.data(), len);
        g_audio_buffer.erase(g_audio_buffer.begin(), g_audio_buffer.begin() + len);
    } else {
        size_t available = g_audio_buffer.size();
        memcpy(stream, g_audio_buffer.data(), available);
        memset(stream + available, 0, len - available);
        g_audio_buffer.clear();
    }
}

// ============================================================================
// FFmpeg 初始化与清理
// ============================================================================

/**
 * @brief 初始化 FFmpeg 解码器
 * @param url 视频文件路径或 RTSP URL
 * @return true=成功, false=失败
 */
bool init_decoder(const std::string& url) {
    g_decoder.input_url = url;

    // 1. 打开输入
    AVDictionary* opts = nullptr;
    // 对于 RTSP 流，设置一些超时参数
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "timeout", "5000000", 0); // 5秒超时（微秒）
    
    int ret = avformat_open_input(&g_decoder.fmt_ctx, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        std::cerr << "[FFmpeg] 无法打开输入: " << url << std::endl;
        return false;
    }

    // 2. 获取流信息
    ret = avformat_find_stream_info(g_decoder.fmt_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "[FFmpeg] avformat_find_stream_info 失败" << std::endl;
        avformat_close_input(&g_decoder.fmt_ctx);
        return false;
    }

    // 3. 查找视频流和音频流
    for (unsigned i = 0; i < g_decoder.fmt_ctx->nb_streams; i++) {
        AVStream* stream = g_decoder.fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_decoder.video_stream_idx = i;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            g_decoder.audio_stream_idx = i;
        }
    }

    if (g_decoder.video_stream_idx < 0) {
        std::cerr << "[FFmpeg] 未找到视频流" << std::endl;
        avformat_close_input(&g_decoder.fmt_ctx);
        return false;
    }

    AVStream* video_stream = g_decoder.fmt_ctx->streams[g_decoder.video_stream_idx];
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "[FFmpeg] 不支持的视频编码: " 
                  << avcodec_get_name(video_stream->codecpar->codec_id) << std::endl;
        avformat_close_input(&g_decoder.fmt_ctx);
        return false;
    }

    // 4. 打开视频解码器
    g_decoder.video_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_decoder.video_ctx, video_stream->codecpar);
    
    // 设置低延迟选项（通过 AVDictionary 传递给 avcodec_open2）
    AVDictionary* decoder_opts = nullptr;
    av_dict_set(&decoder_opts, "threads", "auto", 0);
    
    ret = avcodec_open2(g_decoder.video_ctx, codec, &decoder_opts);
    av_dict_free(&decoder_opts);
    if (ret < 0) {
        std::cerr << "[FFmpeg] 视频解码器打开失败" << std::endl;
        avcodec_free_context(&g_decoder.video_ctx);
        avformat_close_input(&g_decoder.fmt_ctx);
        return false;
    }

    // 5. 视频格式转换上下文
    g_decoder.output_width = video_stream->codecpar->width;
    g_decoder.output_height = video_stream->codecpar->height;
    
    // 如果分辨率太大，缩放到合适尺寸
    if (g_decoder.output_width > 1280) {
        double ratio = 1280.0 / g_decoder.output_width;
        g_decoder.output_height = static_cast<int>(g_decoder.output_height * ratio);
        g_decoder.output_width = 1280;
    }

    g_decoder.sws_ctx = sws_getContext(
        video_stream->codecpar->width,
        video_stream->codecpar->height,
        static_cast<AVPixelFormat>(video_stream->codecpar->format),
        g_decoder.output_width,
        g_decoder.output_height,
        AV_PIX_FMT_BGRA,  // ARGB8888 for SDL/Canvas
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    // 6. 分配帧
    g_decoder.video_frame = av_frame_alloc();
    g_decoder.rgb_frame = av_frame_alloc();
    g_decoder.audio_frame = av_frame_alloc();

    int buffer_size = av_image_get_buffer_size(
        AV_PIX_FMT_BGRA, g_decoder.output_width, g_decoder.output_height, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(buffer_size);
    av_image_fill_arrays(g_decoder.rgb_frame->data, g_decoder.rgb_frame->linesize,
                         buffer, AV_PIX_FMT_BGRA, g_decoder.output_width,
                         g_decoder.output_height, 1);

    // 7. 初始化音频（如果有）
    if (g_decoder.audio_stream_idx >= 0) {
        AVStream* audio_stream = g_decoder.fmt_ctx->streams[g_decoder.audio_stream_idx];
        const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (audio_codec) {
            g_decoder.audio_ctx = avcodec_alloc_context3(audio_codec);
            avcodec_parameters_to_context(g_decoder.audio_ctx, audio_stream->codecpar);
            avcodec_open2(g_decoder.audio_ctx, audio_codec, nullptr);
            
            // 音频重采样：转为 stereo 44100Hz 16-bit（FFmpeg 7.x API）
            g_decoder.swr_ctx = swr_alloc();
            
            // 使用 swr_alloc_set_opts2 设置重采样参数
            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            AVChannelLayout in_layout;
            av_channel_layout_copy(&in_layout, &audio_stream->codecpar->ch_layout);
            
            ret = swr_alloc_set_opts2(&g_decoder.swr_ctx,
                &out_layout, AV_SAMPLE_FMT_S16, 44100,
                &in_layout,
                static_cast<AVSampleFormat>(audio_stream->codecpar->format),
                audio_stream->codecpar->sample_rate,
                0, nullptr);
            
            if (ret < 0) {
                std::cerr << "[FFmpeg] swr_alloc_set_opts2 失败: " << ret << std::endl;
                swr_free(&g_decoder.swr_ctx);
            } else {
                ret = swr_init(g_decoder.swr_ctx);
                if (ret < 0) {
                    std::cerr << "[FFmpeg] swr_init 失败: " << ret << std::endl;
                    swr_free(&g_decoder.swr_ctx);
                }
            }
        }
    }

    std::cout << "[FFmpeg] 初始化成功: " 
              << g_decoder.output_width << "x" << g_decoder.output_height
              << ", 视频编码: " << avcodec_get_name(codec->id) << std::endl;
    if (g_decoder.audio_ctx) {
        std::cout << "[FFmpeg] 音频流已找到" << std::endl;
    }
    return true;
}

/**
 * @brief 清理 FFmpeg 解码器
 */
void cleanup_decoder() {
    if (g_decoder.rgb_frame) {
        if (g_decoder.rgb_frame->data[0]) {
            av_freep(&g_decoder.rgb_frame->data[0]);
        }
        av_frame_free(&g_decoder.rgb_frame);
    }
    av_frame_free(&g_decoder.video_frame);
    av_frame_free(&g_decoder.audio_frame);
    
    sws_freeContext(g_decoder.sws_ctx);
    swr_free(&g_decoder.swr_ctx);
    avcodec_free_context(&g_decoder.video_ctx);
    avcodec_free_context(&g_decoder.audio_ctx);
    avformat_close_input(&g_decoder.fmt_ctx);
}

// ============================================================================
// 解码线程
// ============================================================================

/**
 * @brief 解码线程主函数
 * 
 * 循环读取压缩包 → 解码 → 转码 → 推送到 WebSocket
 * 支持：pause/resume 和 seek
 */
void decoder_thread_func() {
    AVPacket* packet = av_packet_alloc();
    if (!packet) return;

    // 基于 PTS 的播放时间控制（替代固定帧率）
    AVStream* vstream = g_decoder.fmt_ctx->streams[g_decoder.video_stream_idx];
    double video_time_base = av_q2d(vstream->time_base);
    auto play_start_clock = std::chrono::steady_clock::now();
    double play_start_pts = 0.0;
    bool first_frame = true;

    while (g_running) {
        // 处理 seek 请求
        int64_t seek_target = g_seek_target.load();
        if (seek_target >= 0) {
            // seek_target 是毫秒，转换到流时间基
            g_seek_target.store(-1);
            
            AVRational us_timebase = {1, 1000000};
            int64_t seek_pos = av_rescale_q(seek_target * 1000, us_timebase, 
                              vstream->time_base);
            
            av_seek_frame(g_decoder.fmt_ctx, g_decoder.video_stream_idx, 
                         seek_pos, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(g_decoder.video_ctx);
            if (g_decoder.audio_ctx) {
                avcodec_flush_buffers(g_decoder.audio_ctx);
            }
            av_packet_unref(packet);
            // seek 后重置时间基准
            first_frame = true;
            // 清空音频缓冲区避免旧的音频残留
            {
                std::lock_guard<std::mutex> lock(g_audio_mutex);
                g_audio_buffer.clear();
            }
            std::cout << "[FFmpeg] Seek 到 " << seek_target << "ms" << std::endl;
            continue;
        }

        // pause 等待（记录暂停时长，恢复时补偿时钟）
        if (g_paused) {
            auto pause_start = std::chrono::steady_clock::now();
            while (g_paused && g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // 恢复时将暂停时长加到 play_start_clock，保持音视频同步
            play_start_clock += std::chrono::steady_clock::now() - pause_start;
            continue;
        }

        // 读取压缩包
        int ret = av_read_frame(g_decoder.fmt_ctx, packet);
        if (ret < 0) {
            // 到文件末尾或流结束
            if (g_decoder.input_url.find("rtsp://") == 0 || 
                g_decoder.input_url.find("rtmp://") == 0) {
                // 实时流：重试
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                // 文件：循环播放
                av_seek_frame(g_decoder.fmt_ctx, g_decoder.video_stream_idx, 
                             0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(g_decoder.video_ctx);
                if (g_decoder.audio_ctx) {
                    avcodec_flush_buffers(g_decoder.audio_ctx);
                }
                first_frame = true;
                {
                    std::lock_guard<std::mutex> lock(g_audio_mutex);
                    g_audio_buffer.clear();
                }
                std::cout << "[FFmpeg] 文件结束，重新开始播放" << std::endl;
                continue;
            }
        }

        // 判断流类型
        bool is_video = (packet->stream_index == g_decoder.video_stream_idx);
        bool is_audio = (packet->stream_index == g_decoder.audio_stream_idx && 
                         g_decoder.audio_ctx && g_decoder.swr_ctx && !g_no_audio.load());

        if (is_video) {
            // 发送压缩包到视频解码器
            avcodec_send_packet(g_decoder.video_ctx, packet);
            
            // 接收解码帧
            while (avcodec_receive_frame(g_decoder.video_ctx, g_decoder.video_frame) >= 0) {
                // 格式转换
                sws_scale(g_decoder.sws_ctx,
                         g_decoder.video_frame->data, g_decoder.video_frame->linesize,
                         0, g_decoder.video_frame->height,
                         g_decoder.rgb_frame->data, g_decoder.rgb_frame->linesize);

                // 推送给浏览器
                int pitch = g_decoder.rgb_frame->linesize[0];
                int expected_pitch = g_decoder.output_width * 4;
                
                // 基于 PTS 的播放速度控制
                double pts_sec = 0.0;
                if (g_decoder.video_frame->pts != AV_NOPTS_VALUE) {
                    pts_sec = g_decoder.video_frame->pts * video_time_base;
                }
                
                if (first_frame) {
                    play_start_pts = pts_sec;
                    play_start_clock = std::chrono::steady_clock::now();
                    first_frame = false;
                }
                
                // 计算这帧应该在什么时间显示，并等待
                double elapsed_video = pts_sec - play_start_pts;
                auto target_time = play_start_clock + 
                    std::chrono::microseconds(static_cast<int64_t>(elapsed_video * 1000000));
                std::this_thread::sleep_until(target_time);
                
                if (pitch != expected_pitch) {
                    // linesize 有对齐填充，逐行拷贝到紧凑缓冲区
                    std::vector<uint8_t> contiguous(static_cast<size_t>(expected_pitch) * g_decoder.output_height);
                    uint8_t* src = g_decoder.rgb_frame->data[0];
                    uint8_t* dst = contiguous.data();
                    for (int y = 0; y < g_decoder.output_height; y++) {
                        memcpy(dst, src, expected_pitch);
                        src += pitch;
                        dst += expected_pitch;
                    }
                    g_ws_server->send_framebuffer(contiguous.data(), 
                                                  g_decoder.output_width,
                                                  g_decoder.output_height,
                                                  expected_pitch);
                } else {
                    g_ws_server->send_framebuffer(g_decoder.rgb_frame->data[0],
                                                  g_decoder.output_width,
                                                  g_decoder.output_height,
                                                  pitch);
                }
            }
        } else if (is_audio) {
            // 音频解码 + 重采样 → 写入音频缓冲区供 SDL 播放
            avcodec_send_packet(g_decoder.audio_ctx, packet);
            while (avcodec_receive_frame(g_decoder.audio_ctx, g_decoder.audio_frame) >= 0) {
                if (!g_decoder.swr_ctx || g_decoder.audio_frame->nb_samples <= 0) break;
                
                // 计算输出样本数（加 256 余量防止重采样延迟导致截断）
                int out_samples = swr_get_out_samples(g_decoder.swr_ctx, 
                    g_decoder.audio_frame->nb_samples);
                if (out_samples <= 0) continue;
                
                // 自己分配输出缓冲区（stereo S16 = 4 字节/样本）
                size_t buf_size = static_cast<size_t>(out_samples) * 4;
                uint8_t* out_buf = (uint8_t*)av_malloc(buf_size);
                if (!out_buf) continue;
                
                int actual = swr_convert(g_decoder.swr_ctx, 
                    &out_buf, out_samples,
                    (const uint8_t* const*)g_decoder.audio_frame->data,
                    g_decoder.audio_frame->nb_samples);
                
                if (actual > 0) {
                    size_t data_size = static_cast<size_t>(actual) * 4;
                    
                    std::lock_guard<std::mutex> lock(g_audio_mutex);
                    if (g_audio_buffer.size() + data_size > AUDIO_BUFFER_MAX) {
                        g_audio_buffer.erase(g_audio_buffer.begin(),
                            g_audio_buffer.begin() + 
                            (g_audio_buffer.size() + data_size - AUDIO_BUFFER_MAX));
                    }
                    g_audio_buffer.insert(g_audio_buffer.end(), 
                        out_buf, out_buf + data_size);
                }
                av_free(out_buf);
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

// ============================================================================
// SDL 渲染辅助（可选的本地预览）
// ============================================================================

/**
 * @brief 在 SDL 窗口中本地预览当前帧
 * 
 * 当浏览器连接时可直接通过 WebSocket 看，SDL 预览作为备用显示
 * 
 * @param renderer SDL 渲染器
 * @param texture  SDL 纹理
 * @param data     ARGB8888 数据
 * @param width    宽度
 * @param height   高度
 */
void render_local_preview(SDL_Renderer* renderer, SDL_Texture* texture,
                          const uint8_t* data, int width, int height) {
    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
        // 将 ARGB8888 数据拷贝到纹理
        // 注意：SDL_PIXELFORMAT_ARGB8888 数据布局与我们的输出一致
        uint8_t* dst = static_cast<uint8_t*>(pixels);
        int copy_width = std::min(width, 1920); // 防止溢出
        int copy_height = std::min(height, 1080);
        
        for (int y = 0; y < copy_height; y++) {
            memcpy(dst + y * pitch, 
                   data + y * width * 4,
                   copy_width * 4);
        }
        SDL_UnlockTexture(texture);
    }
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    // 1. 解析命令行参数
    std::string input_url;
    bool headless = false;
    bool no_audio = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--no-audio") {
            no_audio = true;
        } else if (input_url.empty()) {
            input_url = arg;
        }
    }
    if (input_url.empty()) {
        input_url = "test.mp4";
        std::cout << "[提示] 未指定输入文件，尝试使用默认的 test.mp4" << std::endl;
        std::cout << "       用法: " << argv[0] << " <视频文件|RTSP URL> [--headless]" << std::endl;
    }

    // 2. 初始化 SDL（音频必选，视频可选）
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "[SDL] 音频初始化失败: " << SDL_GetError() << std::endl;
        return 1;
    }
    // 尝试初始化视频子系统（失败不致命）
    if (!headless) {
        SDL_InitSubSystem(SDL_INIT_VIDEO);
    }

    // 3. 初始化 FFmpeg 解码器（先初始化以获取视频分辨率）
    if (!init_decoder(input_url)) {
        std::cerr << "[错误] 解码器初始化失败" << std::endl;
        SDL_Quit();
        return 1;
    }

    // 4. 创建 SDL 窗口和纹理（使用实际视频分辨率，可选）
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    if (!headless) {
        window = SDL_CreateWindow(
            "FFmpeg WebSocket Streamer",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_decoder.output_width, g_decoder.output_height,
            SDL_WINDOW_SHOWN
        );
        if (window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
            if (renderer) {
                texture = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING,
                    g_decoder.output_width, g_decoder.output_height
                );
            }
        }
        if (!window || !renderer || !texture) {
            std::cerr << "[SDL] 窗口创建失败，将以无窗口模式运行: " << SDL_GetError() << std::endl;
            if (texture) SDL_DestroyTexture(texture);
            if (renderer) SDL_DestroyRenderer(renderer);
            if (window) SDL_DestroyWindow(window);
            window = nullptr;
            renderer = nullptr;
            texture = nullptr;
        }
    }

    // 5. 初始化 SDL 音频（如果有音频流且未禁用）
    SDL_AudioDeviceID audio_dev = 0;
    if (no_audio) {
        g_no_audio.store(true);
    }
    if (g_decoder.audio_ctx && !no_audio) {
        SDL_AudioSpec wanted, obtained;
        SDL_zero(wanted);
        wanted.freq = 44100;
        wanted.format = AUDIO_S16LSB;
        wanted.channels = 2;
        wanted.samples = 1024;
        wanted.callback = sdl_audio_callback;
        wanted.userdata = nullptr;
        
        audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (audio_dev > 0) {
            SDL_PauseAudioDevice(audio_dev, 0); // 开始播放
            std::cout << "[SDL] 音频设备已打开: " << obtained.freq << "Hz" << std::endl;
        } else {
            std::cerr << "[SDL] 音频设备打开失败: " << SDL_GetError() << std::endl;
        }
    }

    // 6. 启动 WebSocket 服务器
    WebSocketServer ws_server(WEBSOCKET_PORT);
    g_ws_server = &ws_server;
    ws_server.set_video_info(g_decoder.output_width, g_decoder.output_height);
    ws_server.start();

    std::cout << "[信息] WebSocket 服务器运行于 ws://localhost:" << WEBSOCKET_PORT << std::endl;
    std::cout << "[信息] 浏览器访问: http://localhost:" << WEBSOCKET_PORT << std::endl;
    if (window) {
        std::cout << "[信息] 本地预览窗口已打开" << std::endl;
    } else {
        std::cout << "[信息] 无窗口模式（仅 WebSocket 推流）" << std::endl;
    }

    // 5. 启动解码线程
    std::thread decoder_thread(decoder_thread_func);

    // 6. 主循环：SDL 事件处理（如果有窗口）
    bool running = true;
    
    while (running) {
        if (window) {
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0) {
                if (event.type == SDL_QUIT) {
                    running = false;
                } else if (event.type == SDL_KEYDOWN) {
                    SDL_Keycode key = event.key.keysym.sym;
                    if (key == SDLK_SPACE) {
                        g_paused = !g_paused;
                        std::cout << (g_paused ? "[FFmpeg] 已暂停" : "[FFmpeg] 已恢复") << std::endl;
                    } else if (key == SDLK_ESCAPE) {
                        running = false;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 7. 清理
    g_running = false;
    ws_server.stop();
    decoder_thread.join();

    if (audio_dev > 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    cleanup_decoder();

    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "[信息] 程序已退出" << std::endl;
    return 0;
}