/**
 * @file video_source.cpp
 * @brief 视频源实现 — 测试图案生成 + JPEG 编码器
 * 
 * 实现两部分功能：
 * 1. generate_pattern(): 生成 SMPTE 彩条测试图案 + 帧号
 * 2. encode_jpeg(): 将 RGB 数据编码为 baseline JPEG
 * 
 * JPEG 编码流程：
 *   RGB → YCbCr 4:2:0 → 8x8 DCT → 量化 → Zig-zag → DC差分 + AC游程 → Huffman → JPEG
 * 
 * 不依赖第三方库，所有 JPEG 标准表（量化表、Huffman 表）均为内置常量。
 */

#include "video_source.h"
#include "camera_source.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===========================================================================
// JPEG 标准常量表 (公开为 VideoSource 静态成员)
// ===========================================================================

const int VideoSource::ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

const uint8_t VideoSource::QUANT_LUM[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};

const uint8_t VideoSource::QUANT_CHROM[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// ===========================================================================
// 标准 Huffman 表（JPEG Annex K, Tables K.3~K.6）
// ===========================================================================

/**
 * @brief Huffman 表条目：值 → (编码, 码长)
 */
struct HuffCode {
    uint32_t code;  ///< Huffman 编码值
    int length;      ///< 编码位数
};

/**
 * @brief 从 BITS + HUFFVAL 生成 Huffman 编码表
 * 
 * 算法（JPEG标准 Section C.2 / Annex C）：
 * 1. 对每个码长 len (1~16):
 *    a. 分配 BITS[len] 个值，每个值对应一个编码
 *    b. 编码递增，码长结束时左移一位
 * 
 * @param bits 各码长的码字数量（16 个元素，bits[0]对应码长 1）
 * @param huffval 值数组（按码长顺序排列）
 * @param table 输出编码表（256 个条目，索引为值）
 */
static void build_huff_table(const uint8_t bits[16], const uint8_t* huffval,
                              HuffCode table[256]) {
    // 初始化所有条目为无效
    for (int i = 0; i < 256; i++) {
        table[i].code = 0;
        table[i].length = 0;
    }
    
    uint32_t code = 0;
    int val_idx = 0;
    
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            table[huffval[val_idx]].code = code;
            table[huffval[val_idx]].length = len;
            code++;
            val_idx++;
        }
        code <<= 1;  // 码长增加一位，编码左移
    }
}

// DC 亮度 Huffman 表（BITS + HUFFVAL）
const uint8_t VideoSource::DC_LUM_BITS[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
const uint8_t VideoSource::DC_LUM_VALS[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

// DC 色度 Huffman 表
const uint8_t VideoSource::DC_CHROM_BITS[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
const uint8_t VideoSource::DC_CHROM_VALS[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

// AC 亮度 Huffman 表
const uint8_t VideoSource::AC_LUM_BITS[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
const uint8_t VideoSource::AC_LUM_VALS[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

// AC 色度 Huffman 表
const uint8_t VideoSource::AC_CHROM_BITS[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
const uint8_t VideoSource::AC_CHROM_VALS[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

// ===========================================================================
// 位写入器 — 逐位写入字节流
// ===========================================================================

/**
 * @brief 位写入器，用于 Huffman 熵编码
 * 
 * JPEG 的熵编码以位为单位输出，需要一个位缓冲器：
 * - 逐位写入，凑满 8 位后输出一个字节
 * - 若输出 0xFF，需在其后插入 0x00（JPEG 字节填充规则）
 */
struct BitWriter {
    std::vector<uint8_t>& output;  ///< 输出字节流
    uint8_t byte;                  ///< 当前正在填充的字节
    int bits_used;                 ///< 已使用的位数（0~7）
    
    BitWriter(std::vector<uint8_t>& out) : output(out), byte(0), bits_used(0) {}
    
    /**
     * @brief 写入若干位
     * @param value 要写入的值（低位有效）
     * @param num_bits 位数
     */
    void write_bits(uint32_t value, int num_bits) {
        for (int i = num_bits - 1; i >= 0; i--) {
            byte = (byte << 1) | ((value >> i) & 1);
            bits_used++;
            if (bits_used == 8) {
                output.push_back(byte);
                if (byte == 0xFF) {
                    output.push_back(0x00);  // 字节填充
                }
                byte = 0;
                bits_used = 0;
            }
        }
    }
    
    /**
     * @brief 刷新剩余的位（用 1 填充）
     */
    void flush() {
        if (bits_used > 0) {
            byte <<= (8 - bits_used);
            byte |= (0xFF >> bits_used);  // 剩余位填 1
            output.push_back(byte);
            if (byte == 0xFF) {
                output.push_back(0x00);
            }
            byte = 0;
            bits_used = 0;
        }
    }
};

// ===========================================================================
// 前向 DCT（离散余弦变换）
// ===========================================================================

/**
 * @brief 预计算的 DCT 系数矩阵
 * 
 * dct_coef[k][n] = alpha(k) * cos(pi*(2n+1)*k / 16)
 * 其中 alpha(0) = sqrt(1/8), alpha(k) = sqrt(2/8) (k>0)
 * 
 * 2D DCT 通过可分离的两次 1D DCT 实现（先行后列）。
 */
static double dct_coef[8][8];
static bool dct_initialized = false;

/**
 * @brief 初始化 DCT 系数表（首次调用时执行）
 */
static void init_dct() {
    if (dct_initialized) return;
    for (int k = 0; k < 8; k++) {
        double alpha = (k == 0) ? sqrt(1.0 / 8) : sqrt(2.0 / 8);
        for (int n = 0; n < 8; n++) {
            dct_coef[k][n] = alpha * cos(M_PI * (2 * n + 1) * k / 16.0);
        }
    }
    dct_initialized = true;
}

/**
 * @brief 对 8x8 块执行 2D 前向 DCT
 * 
 * 可分离实现：先对每行做 1D DCT，再对每列做 1D DCT。
 * 输入为 level-shifted 数据（已减去 128）。
 * 
 * @param block 8x8 数据块（原地变换，输入 double，输出 double）
 */
static void forward_dct(double block[8][8]) {
    // 行 DCT
    for (int i = 0; i < 8; i++) {
        double tmp[8];
        for (int k = 0; k < 8; k++) {
            tmp[k] = 0;
            for (int n = 0; n < 8; n++) {
                tmp[k] += block[i][n] * dct_coef[k][n];
            }
        }
        memcpy(block[i], tmp, sizeof(tmp));
    }
    // 列 DCT
    for (int j = 0; j < 8; j++) {
        double tmp[8];
        for (int k = 0; k < 8; k++) {
            tmp[k] = 0;
            for (int n = 0; n < 8; n++) {
                tmp[k] += block[n][j] * dct_coef[k][n];
            }
        }
        for (int i = 0; i < 8; i++) block[i][j] = tmp[i];
    }
}

// ===========================================================================
// 值编码辅助函数
// ===========================================================================

/**
 * @brief 计算值所需的位数（category / VLI size）
 * 
 * JPEG 中 DC/AC 系数的附加位编码：
 * - 先编码 category（值所需的位数），用 Huffman 表
 * - 再编码值本身的二进制表示
 * 
 * 例: 5 → category=3 (101)
 *     -3 → category=2 (00, 即 -3+2^2-1=0)
 *      0 → category=0（无附加位）
 * 
 * @param value 系数值（已量化）
 * @return 位数（0~11）
 */
static int get_category(int value) {
    int abs_val = (value < 0) ? -value : value;
    int cat = 0;
    while (abs_val) { cat++; abs_val >>= 1; }
    return cat;
}

/**
 * @brief 计算值的附加位编码
 * 
 * 正值：直接返回 value
 * 负值：返回 value + (1 << category) - 1
 * 
 * @param value 系数值
 * @param category 位数（由 get_category 计算）
 * @return 附加位编码值
 */
static int get_additional_bits(int value, int category) {
    if (value >= 0) return value;
    return value + (1 << category) - 1;
}

// ===========================================================================
// VideoSource 类实现
// ===========================================================================

VideoSource::VideoSource(int width, int height)
    : width_(width), height_(height), frame_count_(0) {
    init_dct();  // 初始化 DCT 系数表
}

VideoSource::~VideoSource() {
    // CameraSource 是单例，由其自己管理生命周期
}

std::vector<uint8_t> VideoSource::get_frame() {
    std::vector<uint8_t> rgb(width_ * height_ * 3);
    generate_pattern(rgb.data());
    frame_count_++;
    return encode_jpeg(rgb.data());
}

std::vector<uint8_t> VideoSource::get_frame_jpeg() {
    std::vector<uint8_t> rgb(width_ * height_ * 3);
    generate_pattern(rgb.data());
    frame_count_++;
    
    std::vector<uint8_t> result;
    
    // SOI
    result.push_back(0xFF); result.push_back(0xD8);
    
    // DQT × 2
    for (int t = 0; t < 2; t++) {
        const uint8_t* quant = (t == 0) ? QUANT_LUM : QUANT_CHROM;
        result.push_back(0xFF); result.push_back(0xDB);
        result.push_back(0x00); result.push_back(67);
        result.push_back(static_cast<uint8_t>(t));
        for (int i = 0; i < 64; i++) {
            result.push_back(quant[ZIGZAG[i]]);
        }
    }
    
    // SOF0
    result.push_back(0xFF); result.push_back(0xC0);
    result.push_back(0x00); result.push_back(17);
    result.push_back(8);
    result.push_back((height_ >> 8) & 0xFF);
    result.push_back(height_ & 0xFF);
    result.push_back((width_ >> 8) & 0xFF);
    result.push_back(width_ & 0xFF);
    result.push_back(3);
    result.push_back(1); result.push_back(0x22); result.push_back(0);
    result.push_back(2); result.push_back(0x11); result.push_back(1);
    result.push_back(3); result.push_back(0x11); result.push_back(1);
    
    // DHT × 4
    struct HT { const uint8_t* bits; const uint8_t* vals; int val_count; uint8_t class_id; };
    HT tables[4] = {
        {DC_LUM_BITS,   DC_LUM_VALS,   12, 0x00},
        {AC_LUM_BITS,   AC_LUM_VALS,  162, 0x10},
        {DC_CHROM_BITS, DC_CHROM_VALS, 12, 0x01},
        {AC_CHROM_BITS, AC_CHROM_VALS,162, 0x11},
    };
    for (int t = 0; t < 4; t++) {
        int length = 2 + 1 + 16 + tables[t].val_count;
        result.push_back(0xFF); result.push_back(0xC4);
        result.push_back((length >> 8) & 0xFF);
        result.push_back(length & 0xFF);
        result.push_back(tables[t].class_id);
        for (int i = 0; i < 16; i++) result.push_back(tables[t].bits[i]);
        for (int i = 0; i < tables[t].val_count; i++) result.push_back(tables[t].vals[i]);
    }
    
    // SOS
    result.push_back(0xFF); result.push_back(0xDA);
    result.push_back(0x00); result.push_back(12);
    result.push_back(3);
    result.push_back(1); result.push_back(0x00);
    result.push_back(2); result.push_back(0x11);
    result.push_back(3); result.push_back(0x11);
    result.push_back(0); result.push_back(63); result.push_back(0);
    
    // 熵编码数据
    std::vector<uint8_t> entropy = encode_jpeg(rgb.data());
    result.insert(result.end(), entropy.begin(), entropy.end());
    
    // EOI
    result.push_back(0xFF); result.push_back(0xD9);
    
    return result;
}

std::vector<uint8_t> VideoSource::encode_entropy_only(const uint8_t* rgb) {
    return encode_jpeg(rgb);
}

std::vector<uint8_t> VideoSource::get_frame_entropy() {
    // 优先使用摄像头数据
    CameraSource* cam = CameraSource::instance();
    if (cam && cam->is_open()) {
        // 从摄像头获取最新一帧 RGB24 数据
        std::vector<uint8_t> rgb = cam->get_frame();
        if (!rgb.empty() && (int)rgb.size() == width_ * height_ * 3) {
            frame_count_++;
            return encode_jpeg(rgb.data());
        }
        // 摄像头帧尺寸不匹配或为空，回退到测试图案
    }

    // 回退：生成测试图案
    std::vector<uint8_t> rgb(width_ * height_ * 3);
    generate_pattern(rgb.data());
    frame_count_++;
    return encode_jpeg(rgb.data());
}

// ===========================================================================
// 测试图案生成
// ===========================================================================

void VideoSource::generate_pattern(uint8_t* rgb) {
    // SMPTE 彩条颜色（上 2/3 区域）
    // White, Yellow, Cyan, Green, Magenta, Red, Blue
    static const uint8_t COLORS[7][3] = {
        {255, 255, 255},  // White
        {255, 255,   0},  // Yellow
        {  0, 255, 255},  // Cyan
        {  0, 255,   0},  // Green
        {255,   0, 255},  // Magenta
        {255,   0,   0},  // Red
        {  0,   0, 255},  // Blue
    };
    
    int bar_height = height_ * 2 / 3;   // 彩条占上方 2/3
    int bar_width = width_ / 7;          // 7 条彩条
    
    // 绘制彩条（上 2/3）
    for (int y = 0; y < bar_height; y++) {
        for (int x = 0; x < width_; x++) {
            int bar = x / bar_width;
            if (bar > 6) bar = 6;
            
            // 根据帧号动态偏移彩条位置（每帧移动 1 像素）
            int dynamic_bar = (bar + frame_count_ / 3) % 7;
            int idx = (y * width_ + x) * 3;
            rgb[idx]     = COLORS[dynamic_bar][0];  // R
            rgb[idx + 1] = COLORS[dynamic_bar][1];  // G
            rgb[idx + 2] = COLORS[dynamic_bar][2];  // B
        }
    }
    
    // 下 1/3 区域：黑色背景 + 帧号文字
    for (int y = bar_height; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            int idx = (y * width_ + x) * 3;
            rgb[idx] = 0;
            rgb[idx + 1] = 0;
            rgb[idx + 2] = 0;
        }
    }
    
    // 简单显示帧号（用大字体画数字）
    // 使用 5x7 点阵字体，每位数字占 5x7 像素
    static const uint8_t FONT[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},  // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 1
        {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},  // 2
        {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},  // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},  // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},  // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},  // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},  // 9
    };
    
    // 将帧号转换为字符串
    char num_str[16];
    snprintf(num_str, sizeof(num_str), "Frame:%05d", frame_count_);
    
    // 绘制文字（放大 3 倍）
    int start_x = 10;
    int start_y = bar_height + 10;
    int scale = 3;
    
    for (int ch = 0; num_str[ch] != '\0'; ch++) {
        char c = num_str[ch];
        const uint8_t* glyph = nullptr;
        
        if (c >= '0' && c <= '9') {
            glyph = FONT[c - '0'];
        } else if (c == 'F') {
            static const uint8_t F_glyph[7] = {0x1F,0x10,0x1E,0x10,0x10,0x10,0x10};
            glyph = F_glyph;
        } else if (c == 'r') {
            static const uint8_t r_glyph[7] = {0x00,0x00,0x1E,0x11,0x10,0x10,0x10};
            glyph = r_glyph;
        } else if (c == 'a') {
            static const uint8_t a_glyph[7] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F};
            glyph = a_glyph;
        } else if (c == 'm') {
            static const uint8_t m_glyph[7] = {0x00,0x00,0x1B,0x15,0x15,0x11,0x11};
            glyph = m_glyph;
        } else if (c == 'e') {
            static const uint8_t e_glyph[7] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E};
            glyph = e_glyph;
        } else if (c == ':') {
            static const uint8_t colon[7] = {0x00,0x06,0x06,0x00,0x06,0x06,0x00};
            glyph = colon;
        }
        
        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (glyph[row] & (0x10 >> col)) {
                        // 绘制白色像素（放大 scale 倍）
                        for (int dy = 0; dy < scale; dy++) {
                            for (int dx = 0; dx < scale; dx++) {
                                int px = start_x + ch * 6 * scale + col * scale + dx;
                                int py = start_y + row * scale + dy;
                                if (px >= 0 && px < width_ && py >= 0 && py < height_) {
                                    int idx = (py * width_ + px) * 3;
                                    rgb[idx] = 255;
                                    rgb[idx + 1] = 255;
                                    rgb[idx + 2] = 255;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// ===========================================================================
// JPEG 编码器
// ===========================================================================

std::vector<uint8_t> VideoSource::build_jpeg_headers(int width, int height) {
    std::vector<uint8_t> jpeg;
    
    jpeg.push_back(0xFF); jpeg.push_back(0xD8);
    
    for (int t = 0; t < 2; t++) {
        const uint8_t* quant = (t == 0) ? QUANT_LUM : QUANT_CHROM;
        jpeg.push_back(0xFF); jpeg.push_back(0xDB);
        jpeg.push_back(0x00); jpeg.push_back(67);
        jpeg.push_back(static_cast<uint8_t>(t));
        for (int i = 0; i < 64; i++) {
            jpeg.push_back(quant[ZIGZAG[i]]);
        }
    }
    
    jpeg.push_back(0xFF); jpeg.push_back(0xC0);
    jpeg.push_back(0x00); jpeg.push_back(17);
    jpeg.push_back(8);
    jpeg.push_back((height >> 8) & 0xFF);
    jpeg.push_back(height & 0xFF);
    jpeg.push_back((width >> 8) & 0xFF);
    jpeg.push_back(width & 0xFF);
    jpeg.push_back(3);
    jpeg.push_back(1); jpeg.push_back(0x22); jpeg.push_back(0);
    jpeg.push_back(2); jpeg.push_back(0x11); jpeg.push_back(1);
    jpeg.push_back(3); jpeg.push_back(0x11); jpeg.push_back(1);
    
    struct HuffTableDef {
        const uint8_t* bits;
        const uint8_t* vals;
        int val_count;
        uint8_t class_id;
    };
    
    HuffTableDef tables[4] = {
        {DC_LUM_BITS,   DC_LUM_VALS,   12, 0x00},
        {AC_LUM_BITS,   AC_LUM_VALS,  162, 0x10},
        {DC_CHROM_BITS, DC_CHROM_VALS, 12, 0x01},
        {AC_CHROM_BITS, AC_CHROM_VALS,162, 0x11},
    };
    
    for (int t = 0; t < 4; t++) {
        const auto& td = tables[t];
        int length = 2 + 1 + 16 + td.val_count;
        
        jpeg.push_back(0xFF); jpeg.push_back(0xC4);
        jpeg.push_back((length >> 8) & 0xFF);
        jpeg.push_back(length & 0xFF);
        jpeg.push_back(td.class_id);
        for (int i = 0; i < 16; i++) jpeg.push_back(td.bits[i]);
        for (int i = 0; i < td.val_count; i++) jpeg.push_back(td.vals[i]);
    }
    
    jpeg.push_back(0xFF); jpeg.push_back(0xDA);
    jpeg.push_back(0x00); jpeg.push_back(12);
    jpeg.push_back(3);
    jpeg.push_back(1); jpeg.push_back(0x00);
    jpeg.push_back(2); jpeg.push_back(0x11);
    jpeg.push_back(3); jpeg.push_back(0x11);
    jpeg.push_back(0);
    jpeg.push_back(63);
    jpeg.push_back(0);
    
    return jpeg;
}


std::vector<uint8_t> VideoSource::encode_jpeg(const uint8_t* rgb) {
    static HuffCode dc_lum_table[256];
    static HuffCode ac_lum_table[256];
    static HuffCode dc_chrom_table[256];
    static HuffCode ac_chrom_table[256];
    static bool tables_initialized = false;
    
    if (!tables_initialized) {
        build_huff_table(DC_LUM_BITS, DC_LUM_VALS, dc_lum_table);
        build_huff_table(AC_LUM_BITS, AC_LUM_VALS, ac_lum_table);
        build_huff_table(DC_CHROM_BITS, DC_CHROM_VALS, dc_chrom_table);
        build_huff_table(AC_CHROM_BITS, AC_CHROM_VALS, ac_chrom_table);
        tables_initialized = true;
    }
    
    std::vector<uint8_t> jpeg = build_jpeg_headers(width_, height_);
    size_t entropy_start = jpeg.size();
    
    // === 6. 熵编码数据 ===
    // 将 RGB 转换为 YCbCr，4:2:0 子采样，DCT + 量化 + Huffman 编码
    
    BitWriter bw(jpeg);
    int prev_dc[3] = {0, 0, 0};  // 前一个 DC 系数（差分编码）
    
    // 按 MCU 遍历图像
    // 每个 MCU = 2x2 个 Y 块 + 1 个 Cb 块 + 1 个 Cr 块（4:2:0）
    int mcu_w = (width_ + 15) / 16;   // MCU 列数（每个 MCU 16x16）
    int mcu_h = (height_ + 15) / 16; // MCU 行数
    
    for (int my = 0; my < mcu_h; my++) {
        for (int mx = 0; mx < mcu_w; mx++) {
            // 提取 4 个 Y 块 + 1 个 Cb 块 + 1 个 Cr 块
            double y_blocks[4][8][8];
            double cb_block[8][8];
            double cr_block[8][8];
            
            // 4 个 Y 块（2x2）
            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    for (int i = 0; i < 8; i++) {
                        for (int j = 0; j < 8; j++) {
                            int px = mx * 16 + bx * 8 + j;
                            int py = my * 16 + by * 8 + i;
                            // 边界处理：超出图像范围用最后一行/列填充
                            if (px >= width_) px = width_ - 1;
                            if (py >= height_) py = height_ - 1;
                            int idx = (py * width_ + px) * 3;
                            int r = rgb[idx], g = rgb[idx + 1], b = rgb[idx + 2];
                            // RGB → YCbCr (ITU-R BT.601)
                            double y =  0.299 * r + 0.587 * g + 0.114 * b;
                            y_blocks[by * 2 + bx][i][j] = y - 128.0;  // Level shift
                        }
                    }
                }
            }
            
            // Cb, Cr 块（4:2:0 子采样：取 2x2 平均）
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 8; j++) {
                    double cb_sum = 0, cr_sum = 0;
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int px = mx * 16 + j * 2 + dx;
                            int py = my * 16 + i * 2 + dy;
                            if (px >= width_) px = width_ - 1;
                            if (py >= height_) py = height_ - 1;
                            int idx = (py * width_ + px) * 3;
                            int r = rgb[idx], g = rgb[idx + 1], b = rgb[idx + 2];
                            cb_sum += -0.168736 * r - 0.331264 * g + 0.5 * b;
                            cr_sum +=  0.5 * r - 0.418688 * g - 0.081312 * b;
                        }
                    }
                    cb_block[i][j] = cb_sum / 4.0;
                    cr_block[i][j] = cr_sum / 4.0;
                }
            }
            
            // 对每个块执行 DCT + 量化 + Huffman 编码
            // Y: 4 个块，Cb: 1 个块，Cr: 1 个块
            double* blocks[6] = {
                &y_blocks[0][0][0], &y_blocks[1][0][0],
                &y_blocks[2][0][0], &y_blocks[3][0][0],
                &cb_block[0][0], &cr_block[0][0]
            };
            int table_id[6] = {0, 0, 0, 0, 1, 1};  // 0=luma, 1=chroma
            
            for (int blk = 0; blk < 6; blk++) {
                double (*block)[8] = (double(*)[8])blocks[blk];
                int tid = table_id[blk];
                const uint8_t* quant = (tid == 0) ? QUANT_LUM : QUANT_CHROM;
                const HuffCode* dc_table = (tid == 0) ? dc_lum_table : dc_chrom_table;
                const HuffCode* ac_table = (tid == 0) ? ac_lum_table : ac_chrom_table;
                
                // 前向 DCT
                forward_dct(block);
                
                // 量化 + Zig-zag 扫描
                int quantized[64];
                for (int i = 0; i < 64; i++) {
                    int row = ZIGZAG[i] / 8;
                    int col = ZIGZAG[i] % 8;
                    quantized[i] = (int)round(block[row][col] / quant[ZIGZAG[i]]);
                }
                
                // DC 差分编码
                int dc_diff = quantized[0] - prev_dc[tid];
                prev_dc[tid] = quantized[0];
                
                int dc_cat = get_category(dc_diff);
                bw.write_bits(dc_table[dc_cat].code, dc_table[dc_cat].length);
                if (dc_cat > 0) {
                    bw.write_bits(get_additional_bits(dc_diff, dc_cat), dc_cat);
                }
                
                // AC 游程编码
                int zero_count = 0;
                for (int i = 1; i < 64; i++) {
                    if (quantized[i] == 0) {
                        zero_count++;
                    } else {
                        // 处理连续 0（最多 16 个一组）
                        while (zero_count > 15) {
                            // 编码 ZRL (16 zeros, 0x00 value → run=15, size=0 → byte 0xF0)
                            bw.write_bits(ac_table[0xF0].code, ac_table[0xF0].length);
                            zero_count -= 16;
                        }
                        // 编码 (run_length, category)
                        int ac_cat = get_category(quantized[i]);
                        uint8_t ac_symbol = (zero_count << 4) | ac_cat;
                        bw.write_bits(ac_table[ac_symbol].code, ac_table[ac_symbol].length);
                        bw.write_bits(get_additional_bits(quantized[i], ac_cat), ac_cat);
                        zero_count = 0;
                    }
                }
                
                // 如果最后还有未编码的 0，输出 EOB (End Of Block, 0x00)
                if (zero_count > 0) {
                    bw.write_bits(ac_table[0x00].code, ac_table[0x00].length);
                }
            }
        }
    }
    
    bw.flush();
    
    // 只返回熵编码数据（不含 JPEG 文件头和 EOI）
    // RTP/JPEG 接收方根据 RTP 头中的 Q/Type/Width/Height 重建完整 JPEG
    return std::vector<uint8_t>(jpeg.begin() + entropy_start, jpeg.end());
}
