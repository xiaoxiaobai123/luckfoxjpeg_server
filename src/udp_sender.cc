#include "udp_sender.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <time.h>

// ============================================================================
// 协议v2.0定义 - 使用头文件中的定义
// ============================================================================
// 注意：协议常量和结构体定义在udp_sender.h中，这里不重复定义

// UDP发送器内部结构
struct UDPSender {
    int sock_fd;
    struct sockaddr_in target_addr;
    uint32_t frame_counter;
    bool initialized;
    char target_ip_str[16];
    int target_port;
};

// 全局发送器（便于集成现有代码）
static UDPSender* g_global_sender = NULL;

// ============================================================================
// 协议调试函数
// ============================================================================

void debug_protocol_header(ImgProtocolHeader* header) {
    // 魔数解析
    char magic_str[5];
    memcpy(magic_str, &header->magic, 4);
    magic_str[4] = '\0';
    printf("[协议v2] 魔数: 0x%08X (\"%s\")\n", header->magic, magic_str);
    
    // 版本解析
    printf("[协议v2] 版本: %d.%d\n", (header->version >> 8) & 0xFF, header->version & 0xFF);
    
    // 数据类型
    const char* type_str = "未知";
    switch(header->data_type) {
        case IMG_DATA_TYPE_H264: type_str = "H.264视频"; break;
        case IMG_DATA_TYPE_JPEG: type_str = "JPEG图片"; break; 
        case IMG_DATA_TYPE_RAW:  type_str = "原始数据"; break;
    }
    printf("[协议v2] 数据类型: 0x%04X (%s)\n", header->data_type, type_str);
    
    // 帧信息
    printf("[协议v2] 帧序号: %u\n", header->frame_id);
    if (header->frame_size == IMG_FRAME_SIZE_FRAGMENT) {
        printf("[协议v2] 帧大小: 分包标识\n");
    } else {
        printf("[协议v2] 帧大小: %u bytes\n", header->frame_size);
    }
}

// ============================================================================
// 内部实现
// ============================================================================

UDPSender* udp_sender_create(void) {
    UDPSender* sender = (UDPSender*)malloc(sizeof(UDPSender));
    if (!sender) return NULL;
    
    memset(sender, 0, sizeof(UDPSender));
    sender->sock_fd = -1;
    sender->initialized = false;
    sender->frame_counter = 0;
    
    return sender;
}

bool udp_sender_init(UDPSender* sender, const char* target_ip, int port) {
    if (!sender || !target_ip) return false;
    
    // 保存配置
    strncpy(sender->target_ip_str, target_ip, sizeof(sender->target_ip_str) - 1);
    sender->target_ip_str[sizeof(sender->target_ip_str) - 1] = '\0';
    sender->target_port = port;
    
    // 创建UDP socket
    sender->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender->sock_fd < 0) {
        printf("[协议v2] UDP socket创建失败: %s\n", strerror(errno));
        return false;
    }
    
    // 设置发送缓冲区
    int buffer_size = 4 * 1024 * 1024; // 4MB
    if (setsockopt(sender->sock_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        printf("[协议v2] 警告: 设置发送缓冲区失败\n");
    }
    
    // 启用广播
    int broadcast = 1;
    setsockopt(sender->sock_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // 关闭路由检查（直连优化）
    int dont_route = 1;
    setsockopt(sender->sock_fd, SOL_SOCKET, SO_DONTROUTE, &dont_route, sizeof(dont_route));
    
    // 设置目标地址
    memset(&sender->target_addr, 0, sizeof(sender->target_addr));
    sender->target_addr.sin_family = AF_INET;
    sender->target_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &sender->target_addr.sin_addr) <= 0) {
        printf("[协议v2] IP地址转换失败: %s\n", target_ip);
        close(sender->sock_fd);
        sender->sock_fd = -1;
        return false;
    }
    
    sender->initialized = true;
    printf("[协议v2] UDP发送器初始化成功: %s:%d\n", target_ip, port);
    return true;
}

bool udp_sender_test_connection(UDPSender* sender) {
    if (!sender || !sender->initialized) return false;
    
    // 使用新协议发送测试包
    ImgProtocolHeader test_header;
    test_header.magic = IMG_PROTOCOL_MAGIC;
    test_header.version = IMG_PROTOCOL_VERSION;
    test_header.data_type = IMG_DATA_TYPE_RAW;  // 测试数据类型
    test_header.frame_id = 0;  // 测试帧ID
    test_header.frame_size = 9;  // "1106_TEST"长度
    
    const char* test_msg = "1106_TEST";
    
    struct iovec iov[2];
    iov[0].iov_base = &test_header;
    iov[0].iov_len = sizeof(test_header);
    iov[1].iov_base = (void*)test_msg;
    iov[1].iov_len = strlen(test_msg);
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &sender->target_addr;
    msg.msg_namelen = sizeof(sender->target_addr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    
    ssize_t sent = sendmsg(sender->sock_fd, &msg, MSG_DONTWAIT);
    
    if (sent > 0) {
        printf("[协议v2] 连通性测试包已发送到 %s:%d (协议头+数据=%ld字节)\n", 
               sender->target_ip_str, sender->target_port, sent);
        return true;
    } else {
        printf("[协议v2] 连通性测试失败: %s\n", strerror(errno));
        return false;
    }
}

// 发送单个协议包
bool send_protocol_packet(UDPSender* sender, ImgProtocolHeader* header, void* data, uint32_t data_size) {
    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len = sizeof(ImgProtocolHeader);
    iov[1].iov_base = data;
    iov[1].iov_len = data_size;
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &sender->target_addr;
    msg.msg_namelen = sizeof(sender->target_addr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    
    ssize_t sent = sendmsg(sender->sock_fd, &msg, MSG_DONTWAIT);
    bool success = (sent == (sizeof(ImgProtocolHeader) + data_size));
    
    if (!success) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("[协议v2] 发送缓冲区满\n");
        } else {
            printf("[协议v2] 发送失败: %s\n", strerror(errno));
        }
    }
    
    return success;
}

// 协议v2.0的分包发送函数
bool udp_sender_push_frame_v2(UDPSender* sender, void* h264_data, uint32_t size) {
    if (!sender || !sender->initialized || !h264_data || size == 0) {
        return false;
    }
    
    const uint32_t MAX_UDP_SIZE = 30000;  // 16KB包大小
    const uint32_t HEADER_SIZE = sizeof(ImgProtocolHeader);
    const uint32_t PAYLOAD_SIZE = MAX_UDP_SIZE - HEADER_SIZE;
    
    printf("[协议v2] 发送H.264数据: %d bytes, 需要分包: %s\n", 
           size, (size > PAYLOAD_SIZE) ? "是" : "否");
    
    if (size <= PAYLOAD_SIZE) {
        // 单包发送
        printf("[协议v2] 单包发送\n");
        
        ImgProtocolHeader header;
        header.magic = IMG_PROTOCOL_MAGIC;
        header.version = IMG_PROTOCOL_VERSION;
        header.data_type = IMG_DATA_TYPE_H264;
        header.frame_id = ++sender->frame_counter;
        header.frame_size = size;
        
        debug_protocol_header(&header);
        
        bool success = send_protocol_packet(sender, &header, h264_data, size);
        printf("[协议v2] 单包发送结果: %s\n", success ? "成功" : "失败");
        return success;
    } else {
        // 分包发送
        printf("[协议v2] 开始分包发送，总大小: %d bytes\n", size);
        
        uint8_t* data_ptr = (uint8_t*)h264_data;
        uint32_t remaining = size;
        uint32_t base_frame_id = ++sender->frame_counter;
        uint32_t packet_count = 0;
        uint32_t total_packets = (size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
        
        printf("[协议v2] 总计需要发送 %d 个分包\n", total_packets);
        
        while (remaining > 0) {
            uint32_t chunk_size = (remaining > PAYLOAD_SIZE) ? PAYLOAD_SIZE : remaining;
            
            ImgProtocolHeader header;
            header.magic = IMG_PROTOCOL_MAGIC;
            header.version = IMG_PROTOCOL_VERSION;
            header.data_type = IMG_DATA_TYPE_H264;
            header.frame_id = base_frame_id + packet_count;
            
            // 分包标识逻辑
            if (remaining == chunk_size) {
                // 最后一包：包含原始总大小
                header.frame_size = size;
                printf("[协议v2] 最后分包 %d/%d: chunk_size=%u, 原始总大小=%u\n",
                       packet_count + 1, total_packets, chunk_size, size);
            } else {
                // 中间包：分包标识
                header.frame_size = IMG_FRAME_SIZE_FRAGMENT;
                printf("[协议v2] 中间分包 %d/%d: chunk_size=%u\n",
                       packet_count + 1, total_packets, chunk_size);
            }
            
            if (!send_protocol_packet(sender, &header, data_ptr, chunk_size)) {
                printf("[协议v2] 分包 %d/%d 发送失败!\n", packet_count + 1, total_packets);
                return false;
            }
            
            data_ptr += chunk_size;
            remaining -= chunk_size;
            packet_count++;
            
            // 分包间微延迟
            if (remaining > 0) {
                usleep(100);  // 0.1ms
            }
        }
        
        printf("[协议v2] 分包发送完成，共发送 %d 个包\n", packet_count);
        return true;
    }
}

uint32_t udp_sender_get_frame_count(UDPSender* sender) {
    if (!sender) return 0;
    return sender->frame_counter;
}

void udp_sender_destroy(UDPSender* sender) {
    if (!sender) return;
    
    if (sender->sock_fd >= 0) {
        close(sender->sock_fd);
        sender->sock_fd = -1;
    }
    
    if (sender->initialized) {
        printf("[协议v2] UDP发送器已关闭，总共发送 %d 帧\n", sender->frame_counter);
    }
    
    free(sender);
}

int get_udp_port_by_device_id(int device_id) {
    switch (device_id) {
        case 1:
            return UDP_PORT_SLAVE1;  // 8888
        case 2:
            return UDP_PORT_SLAVE2;  // 8889
        default:
            printf("[协议v2] 警告: 未知设备ID %d，使用默认端口 %d\n", 
                   device_id, UDP_PORT_SLAVE1);
            return UDP_PORT_SLAVE1;
    }
}

// ============================================================================
// 便利函数实现 - 简化集成
// ============================================================================

bool init_udp_sender_global(const char* target_3568_ip,int device_id) {
    if (g_global_sender) {
        printf("[协议v2] 警告: 全局UDP发送器已存在，先清理\n");
        cleanup_udp_sender_global();
    }
    
    g_global_sender = udp_sender_create();
    if (!g_global_sender) {
        printf("[协议v2] 创建UDP发送器失败\n");
        return false;
    }
        // 根据设备ID选择端口
    int port = get_udp_port_by_device_id(device_id);
    if (udp_sender_init(g_global_sender, target_3568_ip, port)) {
        printf("[协议v2] UDP推送模式已启动，目标: %s:8888\n", target_3568_ip);
        printf("[协议v2] 协议版本: 2.0, 支持数据类型: H.264视频\n");
        
        // 测试连通性
        if (udp_sender_test_connection(g_global_sender)) {
            printf("[协议v2] 到3568的网络连通正常\n");
        }
        return true;
    } else {
        printf("[协议v2] UDP初始化失败\n");
        udp_sender_destroy(g_global_sender);
        g_global_sender = NULL;
        return false;
    }
}

// 协议v2.0的主要发送函数
void push_frame_to_3568(void* h264_data, uint32_t size, uint64_t pts) {
    printf("=== [协议v2] UDP发送被调用: 数据大小 %d bytes ===\n", size);
    fflush(stdout);
    
    (void)pts;  // 保留兼容性参数
    
    if (!g_global_sender) {
        printf("[协议v2] 错误: UDP发送器未初始化!\n");
        return;
    }
    
    if (!h264_data || size == 0) {
        printf("[协议v2] 错误: 无效的H.264数据!\n");
        return;
    }
    
    printf("[协议v2] 开始协议v2.0发送...\n");
    bool success = udp_sender_push_frame_v2(g_global_sender, h264_data, size);
    printf("[协议v2] 发送结果: %s\n", success ? "成功" : "失败");
    
    // 发送统计
    static uint32_t total_frames = 0;
    static uint32_t success_frames = 0;
    total_frames++;
    if (success) success_frames++;
    
    // 每5帧打印一次统计
    if (total_frames % 5 == 0) {
        float success_rate = (float)success_frames / total_frames * 100.0f;
        printf("[协议v2] UDP发送统计 - 成功率: %.1f%% (%d/%d)\n", 
               success_rate, success_frames, total_frames);
    }
}

// 测试小包发送
void test_udp_small_packet() {
    printf("=== [协议v2] 测试发送小包 ===\n");
    char test_data[100];
    sprintf(test_data, "TEST_H264_DATA_%ld", time(NULL));
    push_frame_to_3568(test_data, strlen(test_data), 0);
    printf("=== [协议v2] 小包测试完成 ===\n");
}

void cleanup_udp_sender_global(void) {
    if (g_global_sender) {
        udp_sender_destroy(g_global_sender);
        g_global_sender = NULL;
    }
}

// ============================================================================
// 协议信息打印
// ============================================================================

void print_protocol_info() {
    printf("=== 1106图片传输协议 v2.0 ===\n");
    printf("协议魔数: 0x%08X (\"1106\")\n", IMG_PROTOCOL_MAGIC);
    printf("协议版本: %d.%d\n", (IMG_PROTOCOL_VERSION >> 8) & 0xFF, IMG_PROTOCOL_VERSION & 0xFF);
    printf("支持类型: H.264视频(0x%04X), JPEG图片(0x%04X), 原始数据(0x%04X)\n",
           IMG_DATA_TYPE_H264, IMG_DATA_TYPE_JPEG, IMG_DATA_TYPE_RAW);
    printf("协议头大小: %zu bytes\n", sizeof(ImgProtocolHeader));
    printf("最大单包: 16KB\n");
    printf("分包支持: 是\n");
    printf("============================\n");
}