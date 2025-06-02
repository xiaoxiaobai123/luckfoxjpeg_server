#ifndef UDP_SENDER_H
#define UDP_SENDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 1106图片传输协议 v2.0 定义
// ============================================================================

// 协议常量
#define IMG_PROTOCOL_MAGIC     0x31313036    // "1106" 的ASCII码，标识1106设备
#define IMG_PROTOCOL_VERSION   0x0200        // 版本2.0
#define IMG_DATA_TYPE_H264     0x0001        // H.264视频数据
#define IMG_DATA_TYPE_JPEG     0x0002        // JPEG图片数据 (预留)
#define IMG_DATA_TYPE_RAW      0x0003        // 原始数据 (预留)

// 分包标识
#define IMG_FRAME_SIZE_FRAGMENT  0xFFFFFFFF  // 分包标识

// 端口配置 - 支持多设备
#define UDP_PORT_SLAVE1        8888          // 从机1端口
#define UDP_PORT_SLAVE2        8889          // 从机2端口

// 协议头结构 (16字节) - 用于抓包和调试时识别
typedef struct {
    uint32_t magic;        // 协议魔数: 0x31313036 ("1106")
    uint16_t version;      // 协议版本: 0x0200 (v2.0)
    uint16_t data_type;    // 数据类型: 0x0001=H.264, 0x0002=JPEG, 0x0003=RAW
    uint32_t frame_id;     // 帧序号 (自增)
    uint32_t frame_size;   // 数据大小 (分包时为0xFFFFFFFF)
} __attribute__((packed)) ImgProtocolHeader;

// UDP发送器句柄（不透明指针）
typedef struct UDPSender UDPSender;

// ============================================================================
// 主要接口函数 - 简化集成使用
// ============================================================================

/**
 * 全局初始化UDP发送器（协议v2.0）
 * @param target_3568_ip 3568的IP地址
 * @return true=成功, false=失败
 */
bool init_udp_sender_global(const char* target_3568_ip,int device_id);

/**
 * 发送H.264帧数据（协议v2.0，自动分包）
 * @param h264_data H.264编码数据指针
 * @param size 数据大小（字节）
 * @param pts 时间戳（保留参数，兼容性）
 */
void push_frame_to_3568(void* h264_data, uint32_t size, uint64_t pts);

/**
 * 清理全局UDP发送器
 */
void cleanup_udp_sender_global(void);

/**
 * 打印协议信息
 */
void print_protocol_info(void);

/**
 * 测试小包发送
 */
void test_udp_small_packet(void);

// ============================================================================
// 高级接口函数 - 需要手动管理UDPSender对象
// ============================================================================

/**
 * 创建UDP发送器
 * @return 发送器句柄，失败返回NULL
 */
UDPSender* udp_sender_create(void);

/**
 * 初始化UDP发送器
 * @param sender 发送器句柄
 * @param target_ip 目标IP地址（3568的IP）
 * @param port 目标端口（建议8888）
 * @return true=成功, false=失败
 */
bool udp_sender_init(UDPSender* sender, const char* target_ip, int port);

/**
 * 测试到目标的连通性（协议v2.0）
 * @param sender 发送器句柄
 * @return true=连通, false=不通
 */
bool udp_sender_test_connection(UDPSender* sender);

/**
 * 发送H.264帧数据（协议v2.0）
 * @param sender 发送器句柄
 * @param h264_data H.264数据指针
 * @param size 数据大小
 * @return true=成功, false=失败
 */
bool udp_sender_push_frame_v2(UDPSender* sender, void* h264_data, uint32_t size);

/**
 * 获取发送统计
 * @param sender 发送器句柄
 * @return 已发送的帧数
 */
uint32_t udp_sender_get_frame_count(UDPSender* sender);

/**
 * 销毁UDP发送器
 * @param sender 发送器句柄
 */
void udp_sender_destroy(UDPSender* sender);

// ============================================================================
// 协议格式说明
// ============================================================================

/*
协议v2.0包格式：
┌─────────────┬─────────────┬─────────────┬─────────────┬──────────────────┐
│   魔数      │  版本+类型  │  frame_id   │ frame_size  │   H.264 Data     │
│  4 bytes    │   4 bytes   │  4 bytes    │  4 bytes    │    N bytes       │
│ "1106"      │ v2.0 H.264  │   自增      │  数据长度   │   实际视频数据   │
└─────────────┴─────────────┴─────────────┴─────────────┴──────────────────┘

单包示例 (十六进制)：
36 10 31 31  00 02 01 00  7B 00 00 00  E3 3D 00 00  00 00 00 01 67...
│  "1106"  │ v2.0 H.264 │ frame_id=123│  size=15843│   H.264数据开始...

分包规则：
- frame_size = 0xFFFFFFFF：中间包
- frame_size = 实际大小：最后包或单包
- 连续frame_id：同一帧的不同分包

协议优势：
- 明确的"1106"设备标识
- 版本控制，支持协议升级
- 数据类型标识，便于扩展
- 16字节头部，信息丰富
- 支持自动分包，处理大帧
- Wireshark可以轻松识别和分析
*/

#ifdef __cplusplus
}
#endif

#endif // UDP_SENDER_H