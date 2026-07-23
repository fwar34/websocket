/**
 * @file camera_source.h
 * @brief 摄像头视频源 — 基于 FFmpeg avdevice (DirectShow)
 *
 * 使用 FFmpeg 的 avdevice 模块捕获系统摄像头画面，将每一帧转换为 RGB24
 * 供 VideoSource 的 JPEG 编码器使用。
 *
 * 设计要点：
 * - 摄像头是独占资源，同一时刻只能有一个进程打开
 * - 因此 CameraSource 设计为单例（Singleton），所有 VideoSource 实例共享
 * - 采集线程持续读取摄像头帧到内部缓冲区
 * - get_frame() 返回最新一帧的 RGB24 数据
 *
 * 数据流：
 *   摄像头 → av_read_frame() → avcodec_receive_frame() → sws_scale(RGB24)
 *         → 内部缓冲区 → get_frame() → VideoSource::encode_jpeg()
 *
 * 线程模型：
 *   ┌─────────────────────────────────────────────┐
 *   │  CameraSource (单例)                         │
 *   │                                             │
 *   │  capture_thread()  ← 独立采集线程            │
 *   │    循环读取摄像头帧 → sws_scale → rgb_buffer  │
 *   │                                             │
 *   │  get_frame()      ← RTP 发送线程调用          │
 *   │    拷贝 rgb_buffer 快照返回                   │
 *   │                                             │
 *   │  mutex 保护 rgb_buffer 的读写                 │
 *   └─────────────────────────────────────────────┘
 */

#ifndef CAMERA_SOURCE_H
#define CAMERA_SOURCE_H

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

// FFmpeg C 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
}

/**
 * @class CameraSource
 * @brief 摄像头采集源（单例模式）
 *
 * 使用 FFmpeg avdevice 打开系统摄像头（Windows 下为 DirectShow），
 * 持续采集帧并转换为 RGB24 格式供 JPEG 编码使用。
 *
 * 使用方式：
 *   auto cam = CameraSource::instance();
 *   cam->open("Integrated Webcam", 640, 480, 30);
 *   auto rgb = cam->get_frame();  // 返回 RGB24 数据
 */
class CameraSource {
public:
    /**
     * @brief 获取单例实例
     * @return CameraSource 单例指针
     */
    static CameraSource* instance();

    ~CameraSource();

    /**
     * @brief 打开摄像头
     * @param device_name 摄像头设备名（如 "Integrated Webcam"）
     * @param width 目标宽度（0=使用摄像头默认分辨率）
     * @param height 目标高度
     * @param fps 目标帧率
     * @return true=成功, false=失败（摄像头不可用或已打开）
     */
    bool open(const std::string& device_name, int width = 0, int height = 0, int fps = 30);

    /**
     * @brief 关闭摄像头，停止采集线程
     */
    void close();

    /**
     * @brief 获取最新一帧 RGB24 数据
     * @return RGB24 字节向量，size = width * height * 3；摄像头未打开时返回空
     */
    std::vector<uint8_t> get_frame();

    /**
     * @brief 摄像头是否已打开
     */
    bool is_open() const { return opened_; }

    /**
     * @brief 获取实际宽度
     */
    int width() const { return width_; }

    /**
     * @brief 获取实际高度
     */
    int height() const { return height_; }

private:
    CameraSource();  // 私有构造（单例）

    // 禁止拷贝
    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    /**
     * @brief 采集线程主函数
     * 循环读取摄像头帧 → 解码 → sws_scale 转 RGB24 → 写入 rgb_buffer_
     */
    void capture_thread_func();

    // ---- FFmpeg 上下文 ----
    AVFormatContext* fmt_ctx_      = nullptr;  ///< 格式上下文
    AVCodecContext*  codec_ctx_    = nullptr;  ///< 解码上下文
    AVFrame*         frame_        = nullptr;  ///< 解码帧
    AVFrame*         rgb_frame_    = nullptr;  ///< RGB 转换帧
    SwsContext*      sws_ctx_      = nullptr;  ///< 格式转换上下文
    int              video_idx_    = -1;      ///< 视频流索引

    // ---- 采集状态 ----
    std::atomic<bool> opened_{false};        ///< 摄像头是否已打开
    std::atomic<bool> capturing_{false};    ///< 采集线程是否运行
    std::thread       capture_thread_;       ///< 采集线程

    // ---- 帧缓冲 ----
    std::mutex        frame_mutex_;          ///< 帧缓冲互斥锁
    std::vector<uint8_t> rgb_buffer_;        ///< 最新一帧 RGB24 数据
    int               width_  = 0;           ///< 实际宽度
    int               height_ = 0;           ///< 实际高度
};

#endif // CAMERA_SOURCE_H
