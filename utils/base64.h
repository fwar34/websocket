/**
 * @file base64.h
 * @brief Base64 编解码接口
 * 
 * 本文件定义了 Base64 编码/解码的句柄和函数指针接口。
 * 在本项目中用于 WebSocket 握手的 Sec-WebSocket-Accept 编码。
 * 
 * 使用方法：
 *   Base64Handle b64;
 *   Base64HandleInitialize(&b64);           // 初始化
 *   b64.setbuff(&b64, buffer, size);        // 设置输出缓冲区
 *   b64.encodeData(&b64, data, len, 1);     // 编码（isFinal=1 表示最后一批）
 *   b64.addzero(&b64);                      // 添加字符串结尾
 */

#ifndef __BASE64_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BASE64_Finished = 0,            ///< 编码完成
    BASE64_InsufficientCaching      ///< 输出缓冲区不足
} Base64OnReturn;

typedef struct {
    char seat[5];                   ///< 内部残留数据缓冲区（不足 3 字节的部分）
    const void* data[4];            ///< 内部指针（输入位置、输出位置等）
    void(*setbuff)(void* struct_this, void*, size_t);                          ///< 设置输出缓冲区
    void(*setdata)(void* struct_this, const void*, size_t, int isFinal);       ///< 设置输入数据
    int (*encode)(void* struct_this);                                         ///< 执行编码
    int (*addzero)(void* struct_this);                                        ///< 添加 '\0' 结尾
    int (*encodeData)(void* struct_this, const void*, size_t, int isFinal);    ///< 设置数据并编码（便捷接口）
    int (*decode)(void* struct_this);                                         ///< 执行解码
} Base64Handle;

#define Base64Size(_b64, _buf)  ( (const char*)_b64.data[2] - (const char*)_buf )
void Base64HandleInitialize(Base64Handle*);

#ifdef __debug
    extern void Base64Test();
#endif // __debug

#endif // __BASE64_H
