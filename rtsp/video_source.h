/**
 * @file video_source.h
 * @brief 视频源接口 — 测试图案生成 + JPEG 编码
 *
 * 本文件定义了 VideoSource 类，用于生成 MJPEG 视频帧。
 * 主要特性：
 * - 生成动态测试图案（移动彩条 + 时间戳）
 * - 内置 JPEG 编码器（baseline JPEG, DCT + 量化 + Huffman）
 * - 每帧生成 RGB → 编码为 JPEG → 返回 JPEG 字节流
 * - 线程安全：每个 RTSP 会话拥有独立的 VideoSource 实例
 *
 * JPEG 编码流程：
 *   RGB 数据 → YCbCr 4:2:0 → 8x8 DCT → 量化 → Zig-zag → Huffman → JPEG 字节流
 *
 * 测试图案：
 *   ┌───────────────────────────────┐
 *   │ White │ Yellow │ Cyan │ Green │  ← SMPTE 彩条
 *   ├───────┼────────┼──────┼──────┤
 *   │ Magenta│ Red   │ Blue │ Black │
 *   ├───────┴────────┴──────┴──────┤
 *   │         Frame: 0001          │  ← 帧计数（动态变化）
 *   └───────────────────────────────┘
 */

#ifndef VIDEO_SOURCE_H
#define VIDEO_SOURCE_H

#include <cstdint>
#include <vector>
#include <string>

/**
 * @class VideoSource
 * @brief MJPEG 视频源（测试图案 + JPEG 编码器）
 * 
 * 使用示例：
 *   VideoSource vs(320, 240);
 *   auto jpeg = vs.get_frame();  // 返回 JPEG 编码后的字节
 */
class VideoSource {
public:
    VideoSource(int width, int height);
    ~VideoSource();
    
    /**
     * @brief 获取下一帧完整 JPEG 文件
     * 返回包含 SOI/DQT/SOF0/DHT/SOS/熵数据/EOI 的完整 JPEG 文件。
     * 用于直接通过 RTP 发送完整 JPEG 帧。
     */
    std::vector<uint8_t> get_frame_jpeg();
    std::vector<uint8_t> get_frame();
    std::vector<uint8_t> get_frame_entropy();
    
    int get_width() const { return width_; }
    int get_height() const { return height_; }
    
    void seek_to_frame(int frame) { frame_count_ = frame; }
    
    std::vector<uint8_t> encode_entropy_only(const uint8_t* rgb);
    
    static std::vector<uint8_t> build_jpeg_headers(int width, int height);
    
    static const uint8_t QUANT_LUM[64];
    static const uint8_t QUANT_CHROM[64];
    static const int ZIGZAG[64];
    
    static const uint8_t DC_LUM_BITS[16];
    static const uint8_t DC_LUM_VALS[12];
    static const uint8_t AC_LUM_BITS[16];
    static const uint8_t AC_LUM_VALS[162];
    static const uint8_t DC_CHROM_BITS[16];
    static const uint8_t DC_CHROM_VALS[12];
    static const uint8_t AC_CHROM_BITS[16];
    static const uint8_t AC_CHROM_VALS[162];
    
private:
    void generate_pattern(uint8_t* rgb);
    
    std::vector<uint8_t> encode_jpeg(const uint8_t* rgb);
    
    int width_;
    int height_;
    int frame_count_;
};

#endif
