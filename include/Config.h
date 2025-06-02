#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <map>
#include "Common.h"

struct VideoConfig {
    int width = 640;
    int height = 480;
    int bitrate = 2000000;  // 2Mbps
    int fps = 30;
    int quality = 75;       // JPEG质量
};

struct RTSPConfig {
    int port = 8554;
    std::string path = "/live/0";
};

struct PLCConfig {
    bool enabled = true;
    std::string ip = "192.168.1.10";
    int port = 502;
    int slaveId = 1;
    int pollingIntervalMs = 100;
};

struct RegisterMap {
    int statusRegister = 3000;
    int commandRegister = 3001;
    int heartbeatRegister = 3010;
    int streamStatusRegister = 3011;
    int clientCountRegister = 3012;
    int fpsRegister = 3013;
    int bitrateRegister = 3014;
    
    // 命令寄存器映射
    std::map<CommandType, int> commandToRegister = {
        {CommandType::START_STREAM, 3001},
        {CommandType::STOP_STREAM, 3002},
        {CommandType::ADJUST_QUALITY, 3003},
        {CommandType::SET_BITRATE, 3004},
        {CommandType::SET_RESOLUTION, 3005}
    };
};

struct AppConfig {
    VideoConfig video;
    RTSPConfig rtsp;
    PLCConfig plc;
    RegisterMap registerMap;
};

// 获取默认配置
inline AppConfig getDefaultConfig() {
    AppConfig config;
    // 所有成员使用默认值
    return config;
}

#endif // CONFIG_H