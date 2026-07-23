/**
 * @file camera_source.cpp
 * @brief 摄像头视频源实现 — FFmpeg avdevice (DirectShow)
 *
 * 使用 FFmpeg avdevice 模块打开系统摄像头，采集线程持续读取帧，
 * 转换为 RGB24 格式存储在内部缓冲区中，供 VideoSource 的 JPEG 编码器使用。
 *
 * Windows 下使用 DirectShow (dshow) 作为输入格式：
 *   输入 URL 格式: video="Integrated Webcam"
 *   可指定分辨率和帧率: -video_size 640x480 -framerate 30
 */

#include "camera_source.h"
#include <iostream>

// ============================================================================
// 单例
// ============================================================================

CameraSource* CameraSource::instance() {
    static CameraSource inst;
    return &inst;
}

CameraSource::CameraSource() {
    // 注册所有 FFmpeg 设备（dshow, vfwcap 等）
    avdevice_register_all();
}

CameraSource::~CameraSource() {
    close();
}

// ============================================================================
// 打开 / 关闭摄像头
// ============================================================================

bool CameraSource::open(const std::string& device_name, int width, int height, int fps) {
    if (opened_) {
        // 已打开，直接返回
        return true;
    }

    // 1. 查找 dshow 输入格式
    const AVInputFormat* input_fmt = av_find_input_format("dshow");
    if (!input_fmt) {
        std::cerr << "[Camera] 未找到 dshow 输入格式，请确保 FFmpeg 编译时启用了 avdevice\n";
        return false;
    }

    // 2. 设置采集参数（分辨率、帧率）
    AVDictionary* opts = nullptr;
    if (width > 0 && height > 0) {
        std::string size = std::to_string(width) + "x" + std::to_string(height);
        av_dict_set(&opts, "video_size", size.c_str(), 0);
    }
    if (fps > 0) {
        av_dict_set(&opts, "framerate", std::to_string(fps).c_str(), 0);
    }
    // 优先使用 MJPEG 编码（摄像头硬件压缩，CPU 负载低）
    // 如果指定分辨率不支持 mjpeg，FFmpeg 会自动回退到 yuyv422
    av_dict_set(&opts, "vcodec", "mjpeg", 0);

    // 3. 构建 dshow 输入 URL
    // Windows dshow 格式: video="设备名"
    std::string url = "video=" + device_name;

    std::cout << "[Camera] Opening: " << url
              << " (" << width << "x" << height << "@" << fps << "fps)\n";

    // 4. 打开设备
    int ret = avformat_open_input(&fmt_ctx_, url.c_str(), input_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[Camera] avformat_open_input 失败: " << errbuf << "\n";
        return false;
    }

    // 5. 获取流信息
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        std::cerr << "[Camera] avformat_find_stream_info 失败\n";
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // 6. 查找视频流
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx_ = i;
            break;
        }
    }
    if (video_idx_ < 0) {
        std::cerr << "[Camera] 未找到视频流\n";
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // 7. 打开解码器
    AVStream* vstream = fmt_ctx_->streams[video_idx_];
    const AVCodec* codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "[Camera] 不支持的编码: "
                  << avcodec_get_name(vstream->codecpar->codec_id) << "\n";
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, vstream->codecpar);
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        std::cerr << "[Camera] 解码器打开失败\n";
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // 8. 获取实际分辨率
    width_  = codec_ctx_->width;
    height_ = codec_ctx_->height;
    std::cout << "[Camera] Opened: " << width_ << "x" << height_
              << ", codec: " << avcodec_get_name(codec->id) << "\n";

    // 9. 创建 sws 上下文（转换为 RGB24）
    sws_ctx_ = sws_getContext(
        width_, height_, codec_ctx_->pix_fmt,
        width_, height_, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx_) {
        std::cerr << "[Camera] sws_getContext 失败\n";
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // 10. 分配帧
    frame_     = av_frame_alloc();
    rgb_frame_ = av_frame_alloc();
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width_, height_, 1);
    uint8_t* buf = (uint8_t*)av_malloc(buf_size);
    av_image_fill_arrays(rgb_frame_->data, rgb_frame_->linesize,
                         buf, AV_PIX_FMT_RGB24, width_, height_, 1);

    // 11. 预分配 RGB 缓冲区
    rgb_buffer_.resize(width_ * height_ * 3);

    // 12. 启动采集线程
    opened_ = true;
    capturing_ = true;
    capture_thread_ = std::thread(&CameraSource::capture_thread_func, this);

    return true;
}

void CameraSource::close() {
    capturing_ = false;
    opened_ = false;

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (rgb_frame_) {
        if (rgb_frame_->data[0]) {
            av_freep(&rgb_frame_->data[0]);
        }
        av_frame_free(&rgb_frame_);
    }
    av_frame_free(&frame_);
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
    avcodec_free_context(&codec_ctx_);
    avformat_close_input(&fmt_ctx_);

    std::cout << "[Camera] Closed\n";
}

// ============================================================================
// 采集线程
// ============================================================================

void CameraSource::capture_thread_func() {
    AVPacket* packet = av_packet_alloc();

    while (capturing_) {
        // 读取一帧压缩数据
        int ret = av_read_frame(fmt_ctx_, packet);
        if (ret < 0) {
            // 读取失败，等待后重试
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (packet->stream_index != video_idx_) {
            av_packet_unref(packet);
            continue;
        }

        // 发送到解码器
        avcodec_send_packet(codec_ctx_, packet);
        av_packet_unref(packet);

        // 接收解码帧
        while (avcodec_receive_frame(codec_ctx_, frame_) >= 0) {
            // 转换为 RGB24
            sws_scale(sws_ctx_,
                     frame_->data, frame_->linesize,
                     0, codec_ctx_->height,
                     rgb_frame_->data, rgb_frame_->linesize);

            // 拷贝到共享缓冲区（加锁）
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                int pitch = rgb_frame_->linesize[0];
                if (pitch == width_ * 3) {
                    memcpy(rgb_buffer_.data(), rgb_frame_->data[0], rgb_buffer_.size());
                } else {
                    // linesize 有对齐，需要逐行拷贝
                    uint8_t* src = rgb_frame_->data[0];
                    uint8_t* dst = rgb_buffer_.data();
                    for (int y = 0; y < height_; y++) {
                        memcpy(dst, src, width_ * 3);
                        src += pitch;
                        dst += width_ * 3;
                    }
                }
            }
        }
    }

    av_packet_free(&packet);
}

// ============================================================================
// 获取帧
// ============================================================================

std::vector<uint8_t> CameraSource::get_frame() {
    if (!opened_) {
        return {};
    }

    std::lock_guard<std::mutex> lock(frame_mutex_);
    return rgb_buffer_;
}
