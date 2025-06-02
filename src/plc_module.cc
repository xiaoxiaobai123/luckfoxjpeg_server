#include "plc_module.h"
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <errno.h>

PLCModule::PLCModule() : ctx(nullptr), running(false), connected(false), 
                         heartbeat_counter(0), thread_id(0),last_slave_command(0), process_count(0) {
    pthread_mutex_init(&mutex, nullptr);
}

PLCModule::~PLCModule() {
    stop();
    pthread_mutex_destroy(&mutex);
}
// 在PLCModule类中实现notifyCaptureComplete()方法
void PLCModule::notifyCaptureComplete() {
    // 向PLC发送捕捉完成的反馈
    // 具体实现取决于你的PLC通讯协议
}
bool PLCModule::init(const char* ip, int port, int slave_id) {
    // 保存连接参数用于重连
    ip_address = ip;
    port_number = port;
    slave_id_number = slave_id;
    
    pthread_mutex_lock(&mutex);
    
    // 创建modbus上下文
    ctx = modbus_new_tcp(ip, port);
    if (!ctx) {
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to create modbus context" << std::endl;
        return false;
    }
    
    // 设置从站ID
    if (modbus_set_slave(ctx, slave_id) == -1) {
        modbus_free(ctx);
        ctx = nullptr;
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to set slave ID" << std::endl;
        return false;
    }
    
    // 设置响应超时
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (modbus_set_response_timeout(ctx, timeout.tv_sec, timeout.tv_usec) == -1) {
        modbus_free(ctx);
        ctx = nullptr;
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to set timeout" << std::endl;
        return false;
    }
    
    // 设置错误恢复模式
    //modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    
    pthread_mutex_unlock(&mutex);
    return true;
}

bool PLCModule::start() {
    if (running.load()) return true;
    
    pthread_mutex_lock(&mutex);
    
    if (!ctx) {
        pthread_mutex_unlock(&mutex);
        std::cerr << "Modbus context not initialized" << std::endl;
        return false;
    }
    
    // 尝试连接
    if (modbus_connect(ctx) == -1) {
        pthread_mutex_unlock(&mutex);
        std::cerr << "PLC connection failed: " << modbus_strerror(errno) << std::endl;
        return false;
    }
    
    connected.store(true);
    running.store(true);
    
    pthread_mutex_unlock(&mutex);
    
    // 创建监控线程
    if (pthread_create(&thread_id, nullptr, monitorThread, this) != 0) {
        std::cerr << "Failed to create PLC monitor thread" << std::endl;
        pthread_mutex_lock(&mutex);
        modbus_close(ctx);
        connected.store(false);
        running.store(false);
        pthread_mutex_unlock(&mutex);
        return false;
    }
    
    std::cout << "PLC module started successfully" << std::endl;
    return true;
}

void PLCModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    // 等待监控线程结束
    if (thread_id) {
        pthread_join(thread_id, nullptr);
        thread_id = 0;
    }
    
    pthread_mutex_lock(&mutex);
    
    // 关闭连接
    if (ctx && connected.load()) {
        modbus_close(ctx);
        connected.store(false);
    }
    
    // 释放资源
    if (ctx) {
        modbus_free(ctx);
        ctx = nullptr;
    }
    
    pthread_mutex_unlock(&mutex);
    
    std::cout << "PLC module stopped" << std::endl;
}


// 32位寄存器操作
bool PLCModule::writeRegister32(int low_addr, int high_addr, uint32_t value) {
    uint16_t low_word = value & 0xFFFF;
    uint16_t high_word = (value >> 16) & 0xFFFF;
    
    bool result1 = writeRegister(low_addr, low_word);
    bool result2 = writeRegister(high_addr, high_word);
    
    return result1 && result2;
}

uint32_t PLCModule::readRegister32(int low_addr, int high_addr) {
    int low_word = readRegister(low_addr);
    int high_word = readRegister(high_addr);
    
    if (low_word == -1 || high_word == -1) return 0;
    
    return (static_cast<uint32_t>(high_word) << 16) | static_cast<uint32_t>(low_word);
}

bool PLCModule::writeRegister(int addr, int value) {
    if (!running.load() || !connected.load()) return false;
    
    pthread_mutex_lock(&mutex);
    
    if (!ctx || !connected.load()) {
        pthread_mutex_unlock(&mutex);
        return false;
    }
    
    int result = modbus_write_register(ctx, addr, value);
    if (result == -1) {
        std::cerr << "Failed to write register " << addr << ": " << modbus_strerror(errno) << std::endl;
        
        // 检查是否是连接问题
        if (errno == EBADF || errno == EPIPE || errno == ENOTCONN || errno == ECONNRESET) {
            connected.store(false);
        }
        pthread_mutex_unlock(&mutex);
        return false;
    }
    
    pthread_mutex_unlock(&mutex);
    return true;
}

int PLCModule::readRegister(int addr) {
    if (!running.load() || !connected.load()) return -1;
    
    pthread_mutex_lock(&mutex);
    
    if (!ctx || !connected.load()) {
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    
    uint16_t value;
    int result = modbus_read_registers(ctx, addr, 1, &value);
    if (result == -1) {
        std::cerr << "Failed to read register " << addr << ": " << modbus_strerror(errno) << std::endl;
        
        // 检查是否是连接问题
        if (errno == EBADF || errno == EPIPE || errno == ENOTCONN || errno == ECONNRESET) {
            connected.store(false);
        }
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    
    pthread_mutex_unlock(&mutex);
    return value;
}

bool PLCModule::reconnect() {
    std::cout << "Attempting to reconnect to PLC..." << std::endl;
    
    pthread_mutex_lock(&mutex);
    
    // 先关闭旧连接
    if (ctx && connected.load()) {
        modbus_close(ctx);
        connected.store(false);
    }
    
    // 如果ctx已损坏，完全重新创建
    if (ctx) {
        modbus_free(ctx);
        ctx = nullptr;
    }
    
    // 重新创建modbus上下文
    ctx = modbus_new_tcp(ip_address.c_str(), port_number);
    if (!ctx) {
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to recreate modbus context" << std::endl;
        return false;
    }
    
    // 设置从站ID
    if (modbus_set_slave(ctx, slave_id_number) == -1) {
        modbus_free(ctx);
        ctx = nullptr;
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to set slave ID during reconnection" << std::endl;
        return false;
    }
    
    // 设置超时
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (modbus_set_response_timeout(ctx, timeout.tv_sec, timeout.tv_usec) == -1) {
        modbus_free(ctx);
        ctx = nullptr;
        pthread_mutex_unlock(&mutex);
        std::cerr << "Failed to set timeout during reconnection" << std::endl;
        return false;
    }
    
    // 设置错误恢复模式
    //modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    
    // 尝试连接
    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        ctx = nullptr;
        pthread_mutex_unlock(&mutex);
        std::cerr << "Reconnection failed: " << modbus_strerror(errno) << std::endl;
        return false;
    }
    
    connected.store(true);
    pthread_mutex_unlock(&mutex);
    
    std::cout << "PLC reconnected successfully" << std::endl;
    return true;
}

void PLCModule::updateStreamStatus(bool isStreaming, float fps) {
    // 更新流状态寄存器
    writeRegister(PLC_REG_STREAM_STATUS, isStreaming ? 1 : 0);
    
    // 更新FPS寄存器（保留一位小数）
    writeRegister(PLC_REG_FPS, static_cast<int>(fps * 10));
}

// int PLCModule::checkCommand() {
//     // 读取命令寄存器
//     int cmd = readRegister(PLC_REG_COMMAND);
    
//     // 如果有有效命令，清除命令寄存器
//     if (cmd > 0) {
//         writeRegister(PLC_REG_COMMAND, CMD_NONE);
//     }
    
//     return cmd;
// }

// 修改 checkCommand 函数
int PLCModule::checkCommand() {
    if (!isConnected()) return -1;
    
    static int last_command = -1;
    int new_command = readRegister(MY_COMMAND_REG);
    
    // 只在命令发生变化时返回新命令
    if (new_command != last_command) {
        last_command = new_command;
        std::cout << "从机" << SLAVE_ID << "：命令变化 -> " << new_command << std::endl;
        return new_command;  // 直接返回原始命令值：0, 10, 11, 99等
    }
    
    return -1;  // 命令没有变化
}


void* PLCModule::monitorThread(void* arg) {
    PLCModule* plc = static_cast<PLCModule*>(arg);
    plc->monitorLoop();
    return nullptr;
}

void PLCModule::monitorLoop() {
    int reconnect_attempts = 0;
    const int MAX_RECONNECT_ATTEMPTS = 3;
    
    while (running.load()) {
        if (!connected.load()) {
            // 连接丢失，尝试重连
            if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                std::cout << "Connection lost, attempting reconnect (" 
                         << (reconnect_attempts + 1) << "/" << MAX_RECONNECT_ATTEMPTS << ")" << std::endl;
                
                if (reconnect()) {
                    reconnect_attempts = 0;  // 重连成功，重置计数器
                    // 等待一小段时间让连接稳定
                    usleep(500000); // 500ms
                } else {
                    reconnect_attempts++;
                    // 重连失败，等待更长时间再试
                    usleep(5000000); // 5秒
                    continue;
                }
            } else {
                // 达到最大重试次数，等待更长时间后重置计数器
                std::cerr << "Max reconnection attempts reached, waiting 30 seconds..." << std::endl;
                reconnect_attempts = 0;
                usleep(30000000); // 30秒
                continue;
            }
        }
        
        // 更新心跳
        // heartbeat_counter++;
        // if (!writeRegister(PLC_REG_HEARTBEAT, heartbeat_counter)) {
        //     std::cerr << "Heartbeat failed, connection might be lost" << std::endl;
        //     continue;
        // }
        
        // // 定期检查连接状态
        // if (heartbeat_counter % 50 == 0) {  // 每5秒检查一次
        //     int status = readRegister(PLC_REG_STATUS);
        //     if (status == -1) {
        //         std::cerr << "Failed to read status register, connection might be lost" << std::endl;
        //     }
        // }
        
        usleep(100000); // 100ms
    }
    
    std::cout << "PLC monitor thread exiting" << std::endl;
}


// 从机专用函数
int PLCModule::checkSlaveCommand() {
    int current_command = readRegister(MY_COMMAND_REG);
    
    // 只在命令变化时返回新命令
    if (current_command != last_slave_command.load()) {
        int old_command = last_slave_command.load();
        last_slave_command.store(current_command);
        
        std::cout << "从机" << SLAVE_ID << "：命令变化 " << old_command << " -> " << current_command << std::endl;
        return current_command;
    }
    
    return CMD_NONE;
}

bool PLCModule::updateSlaveStatus(SlaveStatus status) {
    bool result = writeRegister(MY_STATUS_REG, static_cast<int>(status));
    if (result) {
        const char* status_names[] = {"空闲", "处理中", "完成", "运行中", "错误"};
        if (status <= SLAVE_ERROR) {
            std::cout << "从机" << SLAVE_ID << "：状态更新为 " << status_names[status] << std::endl;
        }
    }
    return result;
}

// bool PLCModule::reportResult(uint32_t edge_count) {
//     return writeRegister32(MY_RESULT_LOW_REG, MY_RESULT_HIGH_REG, edge_count);
// }

bool PLCModule::incrementProcessCount() {
    int count = process_count.load() + 1;
    process_count.store(count);
    return writeRegister(MY_PROCESS_COUNT_REG, count & 0xFFFF);
}

uint32_t PLCModule::getExposureValue() {
    int value = readRegister(MY_EXPOSURE_REG);
    return (value == -1) ? 0 : static_cast<uint32_t>(value);
}

void PLCModule::updateHeartbeat() {
    heartbeat_counter++;
    writeRegister(MY_HEARTBEAT_REG, heartbeat_counter.load());
}

 

// bool PLCModule::updateSlaveStatus(SlaveStatus status) {
//     return writeRegister(MY_STATUS_REG, static_cast<int>(status));
// }

bool PLCModule::reportResult(uint32_t edge_count) {
    uint16_t low_word = edge_count & 0xFFFF;
    uint16_t high_word = (edge_count >> 16) & 0xFFFF;
    
    bool result1 = writeRegister(MY_RESULT_LOW_REG, low_word);
    bool result2 = writeRegister(MY_RESULT_HIGH_REG, high_word);
    
    return result1 && result2;
}