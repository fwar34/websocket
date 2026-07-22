/**
 * @file sha1.h
 * @brief SHA-1 哈希算法接口（RFC 3174）
 * 
 * 本文件定义了 SHA-1 哈希的结构体和接口函数。
 * 在本项目中用于 WebSocket 握手的 Sec-WebSocket-Accept 计算。
 * 
 * 使用方法：
 *   sha1_t* sha1 = (sha1_t*)malloc(sizeof(sha1_t));
 *   HASH_SHA1_INITIALIZE(sha1);              // 初始化
 *   sha1->Update(sha1, data, len);          // 增量输入
 *   sha1->Final(sha1, hash);                // 输出 20 字节哈希
 *   free(sha1);
 */

#ifndef __SHA1_H

#ifndef __FUN_BYTE2HEX
    #define byte2hex(_input, _output1, _output2, _dump) \
        _dump = _input / 16; _output1 = _dump + (_dump < 10 ? 48 : 87); \
        _dump = _input % 16; _output2 = _dump + (_dump < 10 ? 48 : 87);
    #define byte2Hex(_input, _output1, _output2, _dump) \
        _dump = _input / 16; _output1 = _dump + (_dump < 10 ? 48 : 55); \
        _dump = _input % 16; _output2 = _dump + (_dump < 10 ? 48 : 55);
#endif // __FUN_BYTE2HEX

#define HASH_SHA1_FINALSIZE 20
struct HASH_SHA1_STRUCT {
    const char seat[104];
    void (*Format)(struct HASH_SHA1_STRUCT*);
    void (*Update)(struct HASH_SHA1_STRUCT*, const void*, unsigned int);
    void (*Final)(struct HASH_SHA1_STRUCT*, unsigned char[HASH_SHA1_FINALSIZE]);
};
extern void HASH_SHA1_INITIALIZE(struct HASH_SHA1_STRUCT*);
typedef struct HASH_SHA1_STRUCT sha1_t;

#ifdef __debug
    extern void HASH_SHA1Test();
#endif // __debug

#endif // __SHA1_H
