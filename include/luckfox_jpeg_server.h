#ifndef LUCKFOX_JPEG_SERVER_H
#define LUCKFOX_JPEG_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" { 
#endif

// JPEG服务器配置
typedef struct {
    int port;          // 服务器端口
    int width;         // 捕获宽度
    int height;        // 捕获高度
    int jpegQuality;   // JPEG质量 (1-100)
    const char *iqFilesDir;  // IQ文件目录
} JpegServerConfig;

// 默认配置
#define DEFAULT_SERVER_CONFIG { \
    .port = 8888, \
    .width = 640, \
    .height = 480, \
    .jpegQuality = 75, \
    .iqFilesDir = "/etc/iqfiles" \
}

// 初始化并启动JPEG服务器
// 返回0表示成功，非0表示失败
int initJpegServer(const JpegServerConfig *config);

// 停止和清理JPEG服务器
void stopJpegServer();

// 获取服务器是否在运行
int isJpegServerRunning();

#ifdef __cplusplus
}
#endif

#endif // LUCKFOX_JPEG_SERVER_H
