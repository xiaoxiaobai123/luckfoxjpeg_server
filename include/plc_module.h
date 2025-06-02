// ============================================================================
// plc_module.h - 从机专用PLC模块头文件（修复版）
// ============================================================================

#ifndef PLC_MODULE_H
#define PLC_MODULE_H

#include <modbus.h>
#include <pthread.h>
#include <atomic>
#include <string>

// ============================================================================
// 从机ID检查和宏定义
// ============================================================================
#define SLAVE_ID 2
// 检查从机ID定义
#ifndef SLAVE_ID
#define SLAVE_ID 1  // 默认从机1，编译时应该用 -DSLAVE_ID=1 或 -DSLAVE_ID=2
#endif

// 根据SLAVE_ID自动定义对应的寄存器
#if SLAVE_ID == 1
    // 从机1寄存器
    #define MY_HEARTBEAT_REG     0x0003  // D3: 从机1心跳
    #define MY_COMMAND_REG       0x0034  // D52: 从机1命令 (从主机接收)
    #define MY_STATUS_REG        0x0032  // D50: 从机1状态 (上报给主机)
    #define MY_EXPOSURE_REG      0x000A  // D10: 从机1曝光值
    #define MY_RESULT_LOW_REG    0x0036  // D54: 从机1结果低16位
    #define MY_RESULT_HIGH_REG   0x0037  // D55: 从机1结果高16位
    #define MY_PROCESS_COUNT_REG 0x003A  // D58: 从机1处理计数
        // 从机1网络配置
    #define MY_UDP_TARGET_IP     "192.168.2.50"  // 从机1发送到网段2
    #define MY_UDP_TARGET_PORT   8888             // 从机1目标端口
    #define SLAVE_NAME          "从机1"
#elif SLAVE_ID == 2
    // 从机2寄存器
    #define MY_HEARTBEAT_REG     0x0004  // D4: 从机2心跳
    #define MY_COMMAND_REG       0x0035  // D53: 从机2命令 (从主机接收)
    #define MY_STATUS_REG        0x0033  // D51: 从机2状态 (上报给主机)
    #define MY_EXPOSURE_REG      0x000B  // D11: 从机2曝光值
    #define MY_RESULT_LOW_REG    0x0038  // D56: 从机2结果低16位
    #define MY_RESULT_HIGH_REG   0x0039  // D57: 从机2结果高16位
    #define MY_PROCESS_COUNT_REG 0x003B  // D59: 从机2处理计数
        // 从机2网络配置
    #define MY_UDP_TARGET_IP     "192.168.3.50"  // 从机2发送到网段3
    #define MY_UDP_TARGET_PORT   8889             // 从机2目标端口
    #define SLAVE_NAME          "从机2"
#else
    #error "SLAVE_ID must be 1 or 2"
#endif

// ============================================================================
// PLC寄存器地址定义
// ============================================================================

// === 原有兼容寄存器（保持不变） ===
#define PLC_REG_RESULT           0x0000  // D0: 最终比较结果 (主机使用)
#define PLC_REG_MAIN_COMMAND     0x0001  // D1: 主控制寄存器 (主机使用)
#define PLC_REG_MASTER_HEARTBEAT 0x0002  // D2: 主机心跳信号

// === 从机心跳寄存器 ===
#define PLC_REG_SLAVE1_HEARTBEAT 0x0003  // D3: 从机1心跳
#define PLC_REG_SLAVE2_HEARTBEAT 0x0004  // D4: 从机2心跳

// === 基础寄存器（兼容原有代码） ===
#define PLC_REG_HEARTBEAT        0x0000  // 心跳信号
#define PLC_REG_COMMAND          0x0001  // PLC发送给设备的命令
#define PLC_REG_STATUS           0x0002  // 设备状态
#define PLC_REG_STREAM_STATUS    0x0003  // 流状态（0:停止, 1:运行）
#define PLC_REG_FPS              0x0004  // 实际帧率（x10，如250表示25.0fps）
#define PLC_REG_ERROR            0x0005  // 错误代码

// === 曝光控制寄存器 ===
#define PLC_REG_CAM1_EXPOSURE    0x000A  // D10: 相机1曝光值 (us) 
#define PLC_REG_CAM2_EXPOSURE    0x000B  // D11: 相机2曝光值 (us)

// === 原有结果寄存器（兼容） ===  
#define PLC_REG_CAM1_EDGE_LOW    0x0014  // D20: 相机1 edge count 低16位
#define PLC_REG_CAM1_EDGE_HIGH   0x0015  // D21: 相机1 edge count 高16位
#define PLC_REG_CAM2_EDGE_LOW    0x0016  // D22: 相机2 edge count 低16位
#define PLC_REG_CAM2_EDGE_HIGH   0x0017  // D23: 相机2 edge count 高16位

// === 曝光控制寄存器（如果需要） ===
#define PLC_REG_EXPOSURE_LOW     0x0028  // D40: 曝光时间低16位 (us)
#define PLC_REG_EXPOSURE_HIGH    0x0029  // D41: 曝光时间高16位 (us)
#define PLC_REG_EXPOSURE_TRIGGER 0x002A  // D42: 曝光设置触发
#define PLC_REG_EXPOSURE_FEEDBACK 0x002B // D43: 曝光设置反馈

// ============================================================================
// === 新增协调寄存器（主从机内部使用） ===
// ============================================================================

// 从机状态寄存器
#define PLC_REG_SLAVE1_STATUS    0x0032  // D50: 从机1状态
#define PLC_REG_SLAVE2_STATUS    0x0033  // D51: 从机2状态 
#define PLC_REG_SLAVE1_COMMAND   0x0034  // D52: 从机1命令 (从主机接收)
#define PLC_REG_SLAVE2_COMMAND   0x0035  // D53: 从机2命令 (从主机接收)

// 从机结果寄存器（主机从这里读取）
#define PLC_REG_SLAVE1_RESULT_LOW  0x0036  // D54: 从机1 edge count 低16位
#define PLC_REG_SLAVE1_RESULT_HIGH 0x0037  // D55: 从机1 edge count 高16位  
#define PLC_REG_SLAVE2_RESULT_LOW  0x0038  // D56: 从机2 edge count 低16位
#define PLC_REG_SLAVE2_RESULT_HIGH 0x0039  // D57: 从机2 edge count 高16位

// 从机处理计数器
#define PLC_REG_SLAVE1_PROCESS_COUNT 0x003A  // D58: 从机1处理次数计数
#define PLC_REG_SLAVE2_PROCESS_COUNT 0x003B  // D59: 从机2处理次数计数

// ============================================================================
// PLC命令定义
// ============================================================================

enum PLCCommand {
    CMD_NONE = -1,
    CMD_STOP_STREAM = 0,      // 0: 停止
    CMD_CAPTURE_SINGLE = 10,  // 10: 单次捕捉（修改：原来是4）
    CMD_START_STREAM = 11,    // 11: 连续处理（修改：原来是1）
    CMD_RESTART = 99          // 99: 重启
};

// 从机状态定义
enum SlaveStatus {
    SLAVE_IDLE = 0,        // 空闲
    SLAVE_PROCESSING = 1,  // 处理中
    SLAVE_COMPLETED = 2,   // 完成（单次模式）
    SLAVE_RUNNING = 3,     // 连续运行中
    SLAVE_ERROR = 4        // 错误
};

// ============================================================================
// PLCModule 类定义（修复版，匹配现有实现）
// ============================================================================

class PLCModule {
private:
    modbus_t* ctx;
    std::atomic<bool> running;
    std::atomic<bool> connected;           // 添加：连接状态
    std::atomic<uint16_t> heartbeat_counter; // 添加：心跳计数器
    pthread_t thread_id;
    std::atomic<int> last_slave_command;   // 添加：上次从机命令
    std::atomic<int> process_count;        // 添加：处理计数
    pthread_mutex_t mutex;                 // 添加：互斥锁
    
    // 连接参数（用于重连）
    std::string ip_address;               // 添加：IP地址
    int port_number;                      // 添加：端口号
    int slave_id_number;                  // 添加：从机ID
    
    // 状态变量（兼容原有代码）
    std::atomic<uint16_t> last_heartbeat;
    std::atomic<uint16_t> last_command;
    
    static void* connectionThread(void* arg);
    static void* monitorThread(void* arg);  // 添加：监控线程
    void monitorLoop();                     // 添加：监控循环
    
public:
    PLCModule();
    ~PLCModule();
    
    bool init(const char* plc_ip, int plc_port, int plc_slave_id);
    bool start();
    void stop();
    bool isConnected() { return connected.load(); }  // 修改：内联实现
    
    // 基础读写函数（修改签名匹配实现）
    int readRegister(int reg_addr);                   // 修改：返回int
    bool writeRegister(int reg_addr, int value);      // 修改：参数为int
    
    // 兼容原有接口
    int checkCommand();  // 检查命令变化
    
    // 32位寄存器操作
    uint32_t readRegister32(int low_reg, int high_reg);
    bool writeRegister32(int low_reg, int high_reg, uint32_t value);
    
    // 心跳更新
    void updateHeartbeat();                           // 修改：无参数
    
    // 从机专用函数（修改签名匹配实现）
    bool updateSlaveStatus(SlaveStatus status);       // 修改：无寄存器参数
    bool reportResult(uint32_t edge_count);           // 修改：无寄存器参数
    bool incrementProcessCount();                     // 修改：无参数
    uint32_t getExposureValue();                      // 修改：无参数
    int checkSlaveCommand();                          // 添加：检查从机命令
    
    // 其他功能函数
    void notifyCaptureComplete();                     // 添加：捕获完成通知
    bool reconnect();                                 // 添加：重连函数
    void updateStreamStatus(bool isStreaming, float fps); // 添加：更新流状态
};

// PLC连接参数（外部定义）
extern const char* PLC_IP;
extern const int PLC_PORT; 
extern const int PLC_SLAVE_ID;

#endif // PLC_MODULE_H