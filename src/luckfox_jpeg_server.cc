// ============================================================================
//  
// 编译时需要定义从机ID: -DSLAVE_ID=1 或 -DSLAVE_ID=2
// ============================================================================

// killall rkipc 
// ip addr add 192.168.1.129/24 dev eth0

// export LUCKFOX_SDK_PATH=/home/powin/yolo_workspace/luckfox-pico
// cd ~/luckfox_pico_rkmpi_example-kernel
// rm -rf build
// mkdir build
// cd build
// cmake .. -DEXAMPLE_DIR="example/luckfox_jpeg_server" -DEXAMPLE_NAME="luckfox_jpeg_server" -DSLAVE_ID=1
// make

// export LUCKFOX_SDK_PATH=/home/powin/yolo_workspace/luckfox-pico
// cd ~/luckfox_pico_rkmpi_example-kernel
// rm -rf build
// mkdir build
// cd build
// cmake .. -DEXAMPLE_DIR="example/luckfox_jpeg_server" -DEXAMPLE_NAME="luckfox_jpeg_server" -DSLAVE_ID=2
// make

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <cstring> // For memset

// Rockchip MPI 相关头文件
extern "C" {
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h" 
#include "rk_aiq_user_api2_awb.h"  
#include "rk_aiq_types.h" 
#include "rtsp_demo.h"
}
#include "udp_sender.h"
#include "luckfox_mpi.h"
#include "plc_module.h"  
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

// ============================================================================
// 从机配置参数
// ============================================================================
#define SLAVE_ID 1
// 检查从机ID定义
#ifndef SLAVE_ID
#define SLAVE_ID 1  // 默认从机1，编译时应该用 -DSLAVE_ID=1 或 -DSLAVE_ID=2
#endif

#if SLAVE_ID == 1
    #define RTSP_PORT 8554  // 从机1使用8555端口
    #define SLAVE_NAME "Slave1"
#elif SLAVE_ID == 2
    #define RTSP_PORT 8555 // 从机2使用8556端口
    #define SLAVE_NAME "Slave2"
#else
    #error "SLAVE_ID must be 1 or 2"
#endif

// 基础配置
#define CAP_WIDTH 640
#define CAP_HEIGHT 480
#define RTSP_PATH "/live/0"

// ============================================================================
// PLC 寄存器定义（从机专用）
// ============================================================================

// 从机基础寄存器（根据SLAVE_ID自动计算）
#if SLAVE_ID == 1
    // 从机1寄存器
    #define PLC_REG_MY_HEARTBEAT     0x0003  // D3: 从机1心跳
    #define PLC_REG_MY_COMMAND       0x0034  // D52: 从机1命令 (从主机接收)
    #define PLC_REG_MY_STATUS        0x0032  // D50: 从机1状态 (上报给主机)
    #define PLC_REG_MY_EXPOSURE      0x000A  // D10: 从机1曝光值
    #define PLC_REG_MY_RESULT_LOW    0x0014  // D20: 从机1结果低16位
    #define PLC_REG_MY_RESULT_HIGH   0x0015  // D21: 从机1结果高16位
    #define PLC_REG_MY_PROCESS_COUNT 0x003A  // D58: 从机1处理计数
#else // SLAVE_ID == 2
    // 从机2寄存器
    #define PLC_REG_MY_HEARTBEAT     0x0004  // D4: 从机2心跳
    #define PLC_REG_MY_COMMAND       0x0035  // D53: 从机2命令 (从主机接收)
    #define PLC_REG_MY_STATUS        0x0033  // D51: 从机2状态 (上报给主机)
    #define PLC_REG_MY_EXPOSURE      0x000B  // D11: 从机2曝光值
    #define PLC_REG_MY_RESULT_LOW    0x0016  // D22: 从机2结果低16位
    #define PLC_REG_MY_RESULT_HIGH   0x0017  // D23: 从机2结果高16位
    #define PLC_REG_MY_PROCESS_COUNT 0x003B  // D59: 从机2处理计数
#endif

// ============================================================================
// PLC 命令和状态定义
// ============================================================================

// // PLC命令定义（修改后的）
// enum PLCCommand {
//     CMD_NONE = -1,
//     CMD_STOP_STREAM = 0,      // 0: 停止
//     CMD_CAPTURE_SINGLE = 10,  // 10: 单次捕捉（修改：原来是4）
//     CMD_START_STREAM = 11,    // 11: 连续处理（修改：原来是1）
//     CMD_RESTART = 99          // 99: 重启
// };

// // 从机状态定义
// enum SlaveStatus {
//     SLAVE_IDLE = 0,        // 空闲
//     SLAVE_PROCESSING = 1,  // 处理中
//     SLAVE_COMPLETED = 2,   // 完成（单次模式）
//     SLAVE_RUNNING = 3,     // 连续运行中
//     SLAVE_ERROR = 4        // 错误
// };

// ============================================================================
// 控制变量
// ============================================================================

static volatile bool keep_running = true;
static volatile bool stream_enabled = true;
static volatile int capture_mode = 0;  // 0=停止, 11=连续, 10=单次
static volatile bool single_frame_triggered = false;  
static volatile bool need_clear_buffer = false;  

// 从机专用变量
static volatile uint32_t latest_edge_count = 0;
static volatile bool edge_count_ready = false;
static volatile PLCCommand slave_current_mode = CMD_STOP_STREAM;
static volatile bool processing_active = false;
static uint32_t process_counter = 0;

// 系统对象
static rk_aiq_sys_ctx_t* g_aiq_ctx = nullptr;  
rtsp_demo_handle g_rtsplive = NULL;
rtsp_session_handle g_rtsp_session = NULL;
PLCModule* g_plc = nullptr;

// PLC连接参数
const char* PLC_IP = "192.168.1.10";  // 根据实际情况修改
const int PLC_PORT = 502;
const int PLC_SLAVE_ID = 1;

// ============================================================================
// 信号处理函数
// ============================================================================

extern "C" void int_handler(int sig) {
    printf("\n收到信号 %d, 准备退出...\n", sig);
    keep_running = false;
}

// ============================================================================
// 曝光控制函数（保持原有）
// ============================================================================

void printCurrentExposureInfo() {
    if (!g_aiq_ctx) {
        std::cerr << "错误: AIQ 上下文未初始化!" << std::endl;
        return;
    }

    Uapi_ExpSwAttrV2_t exp_sw_attr{};
    XCamReturn ret_sw = rk_aiq_user_api2_ae_getExpSwAttr(g_aiq_ctx, &exp_sw_attr);

    if (ret_sw == XCAM_RETURN_NO_ERROR) {
        std::cout << "AE 配置信息:" << std::endl;
        std::cout << "  操作模式 (AecOpType): "
                  << (exp_sw_attr.AecOpType == RK_AIQ_OP_MODE_AUTO ? "自动" :
                     (exp_sw_attr.AecOpType == RK_AIQ_OP_MODE_MANUAL ? "手动" : "未知"))
                  << std::endl;

        std::cout << "  自动模式配置 - 线性曝光时间范围 (单位: 秒):" << std::endl;
        std::cout << "    Min: " << exp_sw_attr.stAuto.LinAeRange.stExpTimeRange.Min << std::endl;
        std::cout << "    Max: " << exp_sw_attr.stAuto.LinAeRange.stExpTimeRange.Max << std::endl;
        std::cout << "  自动模式配置 - 线性Sensor增益范围:" << std::endl;
        std::cout << "    Min: " << exp_sw_attr.stAuto.LinAeRange.stGainRange.Min << std::endl;
        std::cout << "    Max: " << exp_sw_attr.stAuto.LinAeRange.stGainRange.Max << std::endl;
        std::cout << "  自动模式配置 - 线性ISP数字增益范围:" << std::endl;
        std::cout << "    Min: " << exp_sw_attr.stAuto.LinAeRange.stIspDGainRange.Min << std::endl;
        std::cout << "    Max: " << exp_sw_attr.stAuto.LinAeRange.stIspDGainRange.Max << std::endl;

        if (exp_sw_attr.AecOpType == RK_AIQ_OP_MODE_MANUAL) {
            std::cout << "  手动模式配置 - 线性曝光设置:" << std::endl;
            std::cout << "    时间使能: " << (exp_sw_attr.stManual.LinearAE.ManualTimeEn ? "是" : "否")
                      << ", 时间值: " << exp_sw_attr.stManual.LinearAE.TimeValue << " s" << std::endl;
            std::cout << "    Sensor增益使能: " << (exp_sw_attr.stManual.LinearAE.ManualGainEn ? "是" : "否")
                      << ", Sensor增益值: " << exp_sw_attr.stManual.LinearAE.GainValue << std::endl;
            std::cout << "    ISP DGain使能: " << (exp_sw_attr.stManual.LinearAE.ManualIspDgainEn ? "是" : "否")
                      << ", ISP DGain值: " << exp_sw_attr.stManual.LinearAE.IspDGainValue << std::endl;
        }
    } else {
        std::cerr << "获取AE软件属性失败，错误码: " << ret_sw << std::endl;
    }
}

bool setManualLinearExposure(float target_time_sec, float target_sensor_gain, float target_isp_dgain) {
    if (!g_aiq_ctx) {
        std::cerr << "错误: AIQ 上下文未初始化，无法设置手动曝光!" << std::endl;
        return false;
    }

    Uapi_ExpSwAttrV2_t exp_attr_to_set{};
    XCamReturn ret_get = rk_aiq_user_api2_ae_getExpSwAttr(g_aiq_ctx, &exp_attr_to_set);

    if (ret_get != XCAM_RETURN_NO_ERROR && ret_get != XCAM_RETURN_BYPASS) {
        std::cerr << "警告: 获取当前AE属性失败 (错误码: " << ret_get 
                  << ")，将使用默认值并尝试设置手动曝光。" << std::endl;
        exp_attr_to_set.Enable = true; 
        exp_attr_to_set.AecOpType = RK_AIQ_OP_MODE_MANUAL; 
    }

    exp_attr_to_set.sync.sync_mode = RK_AIQ_UAPI_MODE_ASYNC; 
    exp_attr_to_set.sync.done = false;

    // 设置为手动模式
    exp_attr_to_set.AecOpType = RK_AIQ_OP_MODE_MANUAL;

    // 配置手动线性曝光参数
    exp_attr_to_set.stManual.LinearAE.ManualTimeEn = true;
    exp_attr_to_set.stManual.LinearAE.TimeValue = target_time_sec;
    exp_attr_to_set.stManual.LinearAE.ManualGainEn = false;
    exp_attr_to_set.stManual.LinearAE.ManualIspDgainEn = false;
     
    // 应用设置
    XCamReturn ret_set = rk_aiq_user_api2_ae_setExpSwAttr(g_aiq_ctx, exp_attr_to_set);

    if (ret_set == XCAM_RETURN_NO_ERROR) {
        std::cout << SLAVE_NAME << "：已设置AE为手动模式，曝光时间: " << target_time_sec << " s" << std::endl;
        return true;
    } else {
        std::cerr << SLAVE_NAME << "：设置手动AE失败，错误码: " << ret_set << std::endl;
        return false;
    }
}

// ============================================================================
// 图像处理函数（修改：添加从机结果上报）
// ============================================================================

int processImage(cv::Mat& image) {
    // 定义ROI区域 (可以根据需要调整)
    cv::Rect roi_rect;
    if (image.cols > 200 && image.rows > 200) {
        int roi_width = image.cols * 0.6;   
        int roi_height = image.rows * 0.6;  
        int roi_x = (image.cols - roi_width) / 2;
        int roi_y = (image.rows - roi_height) / 2;
        roi_rect = cv::Rect(roi_x, roi_y, roi_width, roi_height);
    } else {
        roi_rect = cv::Rect(0, 0, image.cols, image.rows);
    }
    
    // 提取ROI区域
    cv::Mat roi = image(roi_rect);
    
    // 转换为灰度图（如果输入是彩色的）
    cv::Mat gray_roi;
    if (roi.channels() == 3) {
        cv::cvtColor(roi, gray_roi, cv::COLOR_BGR2GRAY);
    } else {
        gray_roi = roi.clone();
    }
    
    // Step 1: 3x3 模糊
    cv::Mat thresh_03;
    cv::blur(gray_roi, thresh_03, cv::Size(3, 3));
    
    // Step 2: Sobel 边缘检测 (X方向)
    cv::Mat thresh_04;
    cv::Sobel(thresh_03, thresh_04, CV_64F, 1, 0, 3);
    
    // Step 3: 转换为8位无符号整数
    cv::Mat thresh_05;
    cv::convertScaleAbs(thresh_04, thresh_05);
    
    // Step 4: 计算边缘像素数量 (阈值为30)
    cv::Mat mask;
    cv::threshold(thresh_05, mask, 30, 255, cv::THRESH_BINARY);
    int edge_count = cv::countNonZero(mask);
    
    // 在原图上绘制ROI边框
    cv::rectangle(image, roi_rect, cv::Scalar(0, 255, 0), 2);
    
    // 添加基本信息
    cv::putText(image, "Processed", cv::Point(10, 30), 
                cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 255, 0), 2);
    
    // 显示边缘数量
    char edge_text[128];
    snprintf(edge_text, sizeof(edge_text), "Edge Count: %d", edge_count);
    cv::putText(image, edge_text, cv::Point(10, 70), 
                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 0), 2);
    
    // === 从机专用：添加从机信息 ===
    
    // 显示从机ID和模式
    const char* mode_str = (slave_current_mode == CMD_CAPTURE_SINGLE) ? "Single(10)" : 
                          (slave_current_mode == CMD_START_STREAM) ? "Continuous(11)" : "Stopped(0)";
    char slave_info[128];
    snprintf(slave_info, sizeof(slave_info), "%s - %s", SLAVE_NAME, mode_str);
    cv::putText(image, slave_info, cv::Point(10, 110), 
                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
    
    // 显示处理计数
    char count_text[64];
    snprintf(count_text, sizeof(count_text), "Process Count: %d", process_counter);
    cv::putText(image, count_text, cv::Point(10, 150), 
                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 0, 255), 2);
    
    // 添加时间戳
    time_t now = time(NULL);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
    cv::putText(image, timeStr, cv::Point(10, image.rows-30), 
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);
    
    // 可选：在图像上显示处理后的边缘检测结果（作为小窗口）
    if (thresh_05.cols > 0 && thresh_05.rows > 0) {
        int display_width = 160;
        int display_height = 120;
        cv::Mat edge_display;
        cv::resize(thresh_05, edge_display, cv::Size(display_width, display_height));
        
        cv::Mat edge_bgr;
        cv::cvtColor(edge_display, edge_bgr, cv::COLOR_GRAY2BGR);
        
        int display_x = image.cols - display_width - 10;
        int display_y = 30;
        
        if (display_x >= 0 && display_y >= 0 && 
            display_x + display_width <= image.cols && 
            display_y + display_height <= image.rows) {
            
            cv::Rect display_rect(display_x, display_y, display_width, display_height);
            edge_bgr.copyTo(image(display_rect));
            
            cv::rectangle(image, display_rect, cv::Scalar(255, 0, 0), 2);
            cv::putText(image, "Edge Map", cv::Point(display_x, display_y - 5), 
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 0), 1);
        }
    }
    
    // === 从机专用：保存结果并上报给PLC ===
    latest_edge_count = static_cast<uint32_t>(edge_count);
    edge_count_ready = true;
    process_counter++;
    
    // 立即上报结果到PLC
    if (g_plc && g_plc->isConnected()) {
        // 写入32位结果（分为两个16位寄存器）
        uint16_t low_word = latest_edge_count & 0xFFFF;
        uint16_t high_word = (latest_edge_count >> 16) & 0xFFFF;
        g_plc->writeRegister(PLC_REG_MY_RESULT_LOW, low_word);
        g_plc->writeRegister(PLC_REG_MY_RESULT_HIGH, high_word);
        
        // 写入处理计数
        g_plc->writeRegister(PLC_REG_MY_PROCESS_COUNT, process_counter & 0xFFFF);
    }
    
    return edge_count;
}

// 保存处理后的图像（保持原有）
void saveProcessedImage(const cv::Mat& image) {
    char filename[64];
    time_t now = time(NULL);
    strftime(filename, sizeof(filename), "/tmp/processed_%Y%m%d_%H%M%S.jpg", localtime(&now));
    
    cv::imwrite(filename, image);
    std::cout << SLAVE_NAME << "：处理后的图像已保存: " << filename << std::endl;
}

// 清空VI缓冲区（保持原有）
void clear_vi_buffer() {
    VIDEO_FRAME_INFO_S stViFrame;
    int cleared_frames = 0;
    
    for (int i = 0; i < 10; i++) {
        RK_S32 ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, 0); 
        if (ret == RK_SUCCESS) {
            RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
            cleared_frames++;
        } else {
            break;
        }
    }
    
    if (cleared_frames > 0) {
        std::cout << SLAVE_NAME << "：清理了 " << cleared_frames << " 个VI缓冲帧" << std::endl;
    }
}

// 清空编码器缓冲区（保持原有）
void clear_encoder_buffer(VENC_CHN VencChn) {
    VENC_STREAM_S stStream;
    stStream.pstPack = (VENC_PACK_S*)malloc(sizeof(VENC_PACK_S));
    
    int cleared_streams = 0;
    while (RK_MPI_VENC_GetStream(VencChn, &stStream, 0) == RK_SUCCESS) {
        RK_MPI_VENC_ReleaseStream(VencChn, &stStream);
        cleared_streams++;
    }
    
    free(stStream.pstPack);
    if (cleared_streams > 0) {
        std::cout << SLAVE_NAME << "：清理了 " << cleared_streams << " 个编码器缓冲帧" << std::endl;
    }
}

// ============================================================================
// 图像捕获和编码线程（修改：适配从机模式）
// ============================================================================
// ============================================================================
// 修复后的图像捕获和编码线程 - 解决模式切换问题
// ============================================================================

// ============================================================================
// 简化的图像捕获线程 - 去掉复杂的模式切换逻辑
// ============================================================================
// ============================================================================
// 完全清理RTSP后的capture线程函数
// ============================================================================

extern "C" void* capture_encode_thread_func(void *arg) {
    std::cout << SLAVE_NAME << "：后台图像处理线程已启动" << std::endl;
    
    RK_S32 s32Ret;
    VENC_CHN VencChn = reinterpret_cast<long>(arg);
    
    // 创建内存池用于保存处理后的图像（RGB888格式）
    MB_POOL_CONFIG_S PoolCfg;
    memset(&PoolCfg, 0, sizeof(PoolCfg));
    PoolCfg.u64MBSize = CAP_WIDTH * CAP_HEIGHT * 3;  
    PoolCfg.u32MBCnt = 1;
    PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    MB_POOL processed_pool = RK_MPI_MB_CreatePool(&PoolCfg);
    MB_BLK processed_blk = RK_MPI_MB_GetMB(processed_pool, CAP_WIDTH * CAP_HEIGHT * 3, RK_TRUE);
    
    // 构建用于编码的帧
    VIDEO_FRAME_INFO_S processed_frame;
    processed_frame.stVFrame.u32Width = CAP_WIDTH;
    processed_frame.stVFrame.u32Height = CAP_HEIGHT;
    processed_frame.stVFrame.u32VirWidth = CAP_WIDTH;
    processed_frame.stVFrame.u32VirHeight = CAP_HEIGHT;
    processed_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;  
    processed_frame.stVFrame.pMbBlk = processed_blk;
    
    uint8_t* processed_data = (uint8_t*)RK_MPI_MB_Handle2VirAddr(processed_blk);
    cv::Mat processed_mat(CAP_HEIGHT, CAP_WIDTH, CV_8UC3, processed_data);
    
    int last_capture_mode = -1;  
    
    while (keep_running) {
        // 模式变化检测
        if (capture_mode != last_capture_mode || need_clear_buffer) {
            std::cout << SLAVE_NAME << "：模式变化: " << last_capture_mode << " -> " << capture_mode << std::endl;
            
            if (need_clear_buffer) {
                clear_vi_buffer();
                clear_encoder_buffer(VencChn);
                usleep(50000);  // 50ms
            }
            
            if (capture_mode == 10) {
                single_frame_triggered = false;
                edge_count_ready = false;
                std::cout << SLAVE_NAME << "：进入单次模式" << std::endl;
            } else if (capture_mode == 11) {
                std::cout << SLAVE_NAME << "：进入连续模式" << std::endl;
            } else if (capture_mode == 0) {
                std::cout << SLAVE_NAME << "：进入停止模式" << std::endl;
            }
            
            last_capture_mode = capture_mode;
            need_clear_buffer = false;
        }
        
        // 判断是否需要获取新帧
        bool should_get_new_frame = false;
        
        switch (capture_mode) {
            case 0:  // 停止模式
                should_get_new_frame = false;
                break;
                
            case 11:  // 连续模式
                should_get_new_frame = true;
                break;
                
            case 10:  // 单次模式
                should_get_new_frame = (!single_frame_triggered);
                break;
                
            default:
                should_get_new_frame = false;
                break;
        }
        
        if (should_get_new_frame) {
            // 添加调试：确认进入帧获取逻辑
            printf("%s: [DEBUG] 尝试获取新帧 (mode=%d)\n", SLAVE_NAME, capture_mode);
            
            VIDEO_FRAME_INFO_S stViFrame;
            s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, 33);
            
            if (s32Ret == RK_SUCCESS) {
                printf("%s: [DEBUG] 成功获取VI帧\n", SLAVE_NAME);
                
                void* yuv_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
                
                // YUV转BGR
                cv::Mat yuv_mat(CAP_HEIGHT + CAP_HEIGHT/2, CAP_WIDTH, CV_8UC1, yuv_data);
                cv::Mat bgr_mat;
                cv::cvtColor(yuv_mat, bgr_mat, cv::COLOR_YUV420sp2BGR);
                
                printf("%s: [DEBUG] 完成YUV->BGR转换\n", SLAVE_NAME);
                
                // 图像处理
                int edge_result = processImage(bgr_mat);
                printf("%s: [DEBUG] 图像处理完成，边缘数: %d\n", SLAVE_NAME, edge_result);
                
                // 添加模式显示
                char mode_text[64];
                const char* mode_str = (capture_mode == 11) ? "Continuous(11)" : 
                                      (capture_mode == 10) ? "Single(10)" : "Stopped(0)";
                snprintf(mode_text, sizeof(mode_text), "Mode: %s", mode_str);
                cv::putText(bgr_mat, mode_text, cv::Point(10, 190), 
                           cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 0, 255), 2);
                
                // 单次模式特殊处理
                if (capture_mode == 10) {
                    saveProcessedImage(bgr_mat);
                    single_frame_triggered = true;
                    std::cout << SLAVE_NAME << "：单次捕获完成，Edge Count: " << edge_result << std::endl;
                }
                
                // 复制处理后的BGR数据到编码器内存
                memcpy(processed_data, bgr_mat.data, CAP_WIDTH * CAP_HEIGHT * 3);
                printf("%s: [DEBUG] 数据已复制到编码器内存\n", SLAVE_NAME);
                
                // 发送给编码器进行H.264压缩
                processed_frame.stVFrame.u64PTS = stViFrame.stVFrame.u64PTS;
                processed_frame.stVFrame.u32TimeRef = stViFrame.stVFrame.u32TimeRef;
                s32Ret = RK_MPI_VENC_SendFrame(VencChn, &processed_frame, -1);
                
                if (s32Ret == RK_SUCCESS) {
                    printf("%s: [DEBUG] 成功发送到编码器\n", SLAVE_NAME);
                    
                    // 获取H.264编码结果
                    VENC_STREAM_S stStream;
                    stStream.pstPack = (VENC_PACK_S*)malloc(sizeof(VENC_PACK_S));
                    
                    s32Ret = RK_MPI_VENC_GetStream(VencChn, &stStream, -1);
                    if (s32Ret == RK_SUCCESS) {
                        void* encoded_data = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
                        printf("%s: [DEBUG] 获取H.264编码数据成功，大小: %d bytes\n", 
                               SLAVE_NAME, stStream.pstPack->u32Len);
                        
                        // 【关键】：发送H.264数据到3568
                        printf("%s: [DEBUG] 准备发送UDP数据...\n", SLAVE_NAME);
                        push_frame_to_3568(encoded_data, stStream.pstPack->u32Len, stStream.pstPack->u64PTS);
                        printf("%s: [DEBUG] UDP发送调用完成\n", SLAVE_NAME);
                        
                        RK_MPI_VENC_ReleaseStream(VencChn, &stStream);
                    } else {
                        printf("%s: [DEBUG] 获取编码数据失败，错误码: %d\n", SLAVE_NAME, s32Ret);
                    }
                    free(stStream.pstPack);
                } else {
                    printf("%s: [DEBUG] 发送到编码器失败，错误码: %d\n", SLAVE_NAME, s32Ret);
                }
                
                RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
            } else {
                // 帧获取失败
                static int fail_count = 0;
                if (++fail_count % 10 == 0) {
                    printf("%s: [DEBUG] 获取帧失败 %d 次，错误码: %d\n", SLAVE_NAME, fail_count, s32Ret);
                }
            }
        } else {
            // 不获取新帧的情况
            if (capture_mode == 0) {
                usleep(33000);  // 停止模式：休眠
                continue;
            }
            
            static int skip_count = 0;
            if (++skip_count % 100 == 0) {
                printf("%s: [DEBUG] 跳过帧获取 %d 次，capture_mode=%d, single_triggered=%d\n", 
                       SLAVE_NAME, skip_count, capture_mode, single_frame_triggered);
            }
        }
        
        usleep(33000); // 30fps间隔
    }
    
    // 清理资源
    RK_MPI_MB_ReleaseMB(processed_blk);
    RK_MPI_MB_DestroyPool(processed_pool);
    
    std::cout << SLAVE_NAME << "：图像处理线程退出" << std::endl;
    return nullptr;
}

// 从机PLC通信线程（修改：适配从机模式和新命令）
// ============================================================================
// ============================================================================
// 简化修复：只修改PLC线程的单次完成处理部分
// 其他部分保持你的原有逻辑不变
// ============================================================================

void* slave_plc_command_thread(void* arg) {
    uint16_t last_cmd = 0;
    uint16_t heartbeat_counter = 0;
    uint32_t current_exposure_us = 0;  // 当前曝光值
    
    std::cout << SLAVE_NAME << "：PLC通信线程已启动" << std::endl;

    while (keep_running) {
        if (g_plc && g_plc->isConnected()) {
            
            // === 更新心跳 ===
            heartbeat_counter++;
            g_plc->writeRegister(PLC_REG_MY_HEARTBEAT, heartbeat_counter);
            
            // === 实时监控曝光值变化 ===
            uint32_t new_exposure_us = g_plc->readRegister(PLC_REG_MY_EXPOSURE);
            if (new_exposure_us != current_exposure_us && new_exposure_us > 0) {
                current_exposure_us = new_exposure_us;
                
                // 只有在处理活跃时才立即应用曝光设置
                if (processing_active && (slave_current_mode == CMD_START_STREAM || slave_current_mode == CMD_CAPTURE_SINGLE)) {
                    float exposure_sec = static_cast<float>(current_exposure_us) / 1000000.0f;
                    if (setManualLinearExposure(exposure_sec, 1.0f, 1.0f)) {
                        std::cout << SLAVE_NAME << "：实时更新曝光 " << current_exposure_us << "us" << std::endl;
                    }
                }
            }
           
            // === 主要命令处理（简化版本）===
            uint16_t current_cmd = g_plc->readRegister(PLC_REG_MY_COMMAND);
            
            // 检测命令变化
            if (current_cmd != last_cmd) {
                std::cout << SLAVE_NAME << "：命令变化 " << last_cmd << " -> " << current_cmd << std::endl;
                
                switch (current_cmd) {
                    case 11:  // CMD_START_STREAM: 连续模式
                        {
                            std::cout << SLAVE_NAME << "：开始连续处理模式 (11)" << std::endl;
                            
                            slave_current_mode = CMD_START_STREAM;
                            processing_active = true;
                            single_frame_triggered = false;
                            edge_count_ready = false;
                            
                            // 更新PLC状态
                            g_plc->writeRegister(PLC_REG_MY_STATUS, SLAVE_RUNNING);
                            
                            // 应用当前曝光设置
                            if (current_exposure_us > 0) {
                                float exposure_sec = static_cast<float>(current_exposure_us) / 1000000.0f;
                                setManualLinearExposure(exposure_sec, 1.0f, 1.0f);
                                std::cout << SLAVE_NAME << "：应用曝光设置 " << current_exposure_us << "us" << std::endl;
                            }
                            
                            // 启动连续捕获
                            capture_mode = 11;  // 连续模式
                            need_clear_buffer = true;
                        }
                        break;
                
                    case 0:  // CMD_STOP_STREAM: 停止 
                        {
                            std::cout << SLAVE_NAME << "：停止处理模式 (0)" << std::endl;
                            
                            slave_current_mode = CMD_STOP_STREAM;
                            processing_active = false;
                            single_frame_triggered = false;
                            edge_count_ready = false;
                            
                            // 更新PLC状态
                            g_plc->writeRegister(PLC_REG_MY_STATUS, SLAVE_IDLE);
                            
                            // 停止捕获
                            capture_mode = 0;
                            need_clear_buffer = true;
                        }
                        break;
                
                    case 10: // CMD_CAPTURE_SINGLE: 单次捕捉
                        {
                            std::cout << SLAVE_NAME << "：开始单次捕捉模式 (10)" << std::endl;
                            
                            slave_current_mode = CMD_CAPTURE_SINGLE;
                            processing_active = true;
                            single_frame_triggered = false;  // 重要：每次都重置
                            edge_count_ready = false;        // 重要：每次都重置
                            
                            // 更新PLC状态
                            g_plc->writeRegister(PLC_REG_MY_STATUS, SLAVE_PROCESSING);
                            
                            // 应用当前曝光设置
                            if (current_exposure_us > 0) {
                                float exposure_sec = static_cast<float>(current_exposure_us) / 1000000.0f;
                                setManualLinearExposure(exposure_sec, 1.0f, 1.0f);
                                std::cout << SLAVE_NAME << "：应用曝光设置 " << current_exposure_us << "us" << std::endl;
                            }
                            
                            // 触发单次捕获
                            capture_mode = 10;  // 单次模式
                            need_clear_buffer = true;
                        }
                        break;
                        
                    case 99:  // CMD_RESTART: 重启
                        std::cout << SLAVE_NAME << "：收到重启命令" << std::endl;
                        keep_running = false;
                        break;
            
                    default:
                        break;
                }
                last_cmd = current_cmd;
            }
            
            // === 处理当前模式状态（关键修改：只在这里加自动清零）===
            if (processing_active) {
                switch (slave_current_mode) {
                    case CMD_CAPTURE_SINGLE:  // 单次模式状态检测
                        // 检查单次是否完成
                        if (capture_mode == 10 && single_frame_triggered && edge_count_ready) {
                            // 更新状态为完成
                            g_plc->writeRegister(PLC_REG_MY_STATUS, SLAVE_COMPLETED);
                            std::cout << SLAVE_NAME << "：单次处理完成，Edge Count: " << latest_edge_count << std::endl;
                            
                            // *** 关键修改：自动清零命令寄存器 ***
                            g_plc->writeRegister(PLC_REG_MY_COMMAND, 0);
                            last_cmd = 0;  // 同步内部状态
                            std::cout << SLAVE_NAME << "：自动清零命令寄存器" << std::endl;
                            
                            processing_active = false;  // 单次完成后停止
                        }
                        break;
                        
                    case CMD_START_STREAM:  // 连续模式
                        // 连续模式持续运行，结果已在processImage中实时更新
                        static int continuous_counter = 0;
                        if (++continuous_counter % 50 == 0) {  // 每5秒确认一次
                            g_plc->writeRegister(PLC_REG_MY_STATUS, SLAVE_RUNNING);
                        }
                        break;
                }
            }
        }
        
        usleep(100000); // 100ms
    }
    
    return nullptr;
}
// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char *argv[]) {
    RK_S32 s32Ret = RK_FAILURE;
    RK_U32 u32Width = CAP_WIDTH;
    RK_U32 u32Height = CAP_HEIGHT;
    const RK_CHAR *pcIqFileDir = "/etc/iqfiles";
    pthread_t capture_tid = 0;
    pthread_t plc_tid = 0;

    VI_DEV ViDev = 0;
    VI_PIPE ViPipe = 0;
    VI_CHN ViChn = 0;
    VENC_CHN VencChn = 0;
    
    // 打印从机信息
    std::cout << "=================================" << std::endl;
    std::cout << "启动 " << SLAVE_NAME << " (SLAVE_ID=" << SLAVE_ID << ")" << std::endl;
    std::cout << "UDP传输端口: 8888" << std::endl;  // 改为UDP端口
    std::cout << "PLC寄存器映射:" << std::endl;
    std::cout << "  心跳: D" << PLC_REG_MY_HEARTBEAT << std::endl;
    std::cout << "  命令: D" << PLC_REG_MY_COMMAND << " (接收)" << std::endl;
    std::cout << "  状态: D" << PLC_REG_MY_STATUS << " (上报)" << std::endl;
    std::cout << "  曝光: D" << PLC_REG_MY_EXPOSURE << std::endl;
    std::cout << "  结果: D" << PLC_REG_MY_RESULT_LOW << "-" << PLC_REG_MY_RESULT_HIGH << std::endl;
    std::cout << "支持命令: 0(停止), 10(单次), 11(连续)" << std::endl;
    std::cout << "=================================" << std::endl;
    
    // 设置信号处理
    struct sigaction sig_action;
    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_handler = int_handler;
    sigaction(SIGINT, &sig_action, nullptr);
    sigaction(SIGTERM, &sig_action, nullptr);
    
    // 初始化PLC模块
    g_plc = new PLCModule();
    if (g_plc->init(PLC_IP, PLC_PORT, PLC_SLAVE_ID)) {
        if (g_plc->start()) {
            pthread_create(&plc_tid, nullptr, slave_plc_command_thread, nullptr);
            std::cout << SLAVE_NAME << "：PLC通讯已启动" << std::endl;
        } else {
            std::cout << SLAVE_NAME << "：PLC连接失败，但继续运行" << std::endl;
        }
    } else {
        std::cout << SLAVE_NAME << "：PLC初始化失败，继续运行但没有PLC控制" << std::endl;
    }
    
    // 1. 初始化 ISP
    std::cout << SLAVE_NAME << "：初始化 ISP..." << std::endl;
    s32Ret = SAMPLE_COMM_ISP_Init(ViDev, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, pcIqFileDir);
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：SAMPLE_COMM_ISP_Init 失败: " << s32Ret << std::endl;
        return RK_FAILURE;
    }
    
    s32Ret = SAMPLE_COMM_ISP_Run(ViDev);
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：SAMPLE_COMM_ISP_Run 失败: " << s32Ret << std::endl;
        SAMPLE_COMM_ISP_Stop(ViDev);
        return RK_FAILURE;
    }
    std::cout << SLAVE_NAME << "：ISP 初始化并运行中" << std::endl;
       
    // 获取 AIQ 上下文
    std::cout << SLAVE_NAME << "：尝试获取 AIQ 上下文 (CamId: " << ViDev << ")" << std::endl;
    g_aiq_ctx = SAMPLE_COMM_ISP_GetAiqSysCtx(ViDev); 
    if (!g_aiq_ctx) { 
        std::cerr << SLAVE_NAME << "：错误: SAMPLE_COMM_ISP_GetAiqSysCtx 返回了 NULL!" << std::endl;
        SAMPLE_COMM_ISP_Stop(ViDev);
        if(g_plc){ delete g_plc; g_plc = nullptr;}
        return RK_FAILURE;
    }
    std::cout << SLAVE_NAME << "：AIQ 上下文获取成功" << std::endl;

    // 2. 初始化 RK_MPI 系统
    std::cout << SLAVE_NAME << "：初始化 RK_MPI 系统..." << std::endl;
    s32Ret = RK_MPI_SYS_Init();
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：RK_MPI_SYS_Init 失败: " << s32Ret << std::endl;
        SAMPLE_COMM_ISP_Stop(ViDev);
        return RK_FAILURE;
    }
    std::cout << SLAVE_NAME << "：RK_MPI 系统已初始化" << std::endl;
    
    // 3. 初始化 VI
    std::cout << SLAVE_NAME << "：初始化 VI..." << std::endl;
    s32Ret = vi_dev_init();
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：vi_dev_init 失败: " << s32Ret << std::endl;
        goto cleanup_sys;
    }
    
    s32Ret = vi_chn_init(ViChn, u32Width, u32Height);
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：vi_chn_init 失败: " << s32Ret << std::endl;
        goto cleanup_vi_dev;
    }
    std::cout << SLAVE_NAME << "：VI 通道 " << ViChn << " 已使能" << std::endl;
    
    // 4. 初始化 VENC 通道
    std::cout << SLAVE_NAME << "：初始化 VENC 通道 " << VencChn << "..." << std::endl;
    s32Ret = venc_init(VencChn, u32Width, u32Height, RK_VIDEO_ID_AVC ); 
    if (s32Ret != RK_SUCCESS) {
        std::cerr << SLAVE_NAME << "：venc_init 失败: " << s32Ret << std::endl;
        goto cleanup_vi_chn;
    }
    std::cout << SLAVE_NAME << "：VENC 通道 " << VencChn << " 已创建" << std::endl;

    // 关闭自动白平衡模块
    {
        std::cout << SLAVE_NAME << "：尝试关闭自动白平衡模块..." << std::endl;
        XCamReturn awb_ctl_ret = rk_aiq_uapi2_sysctl_setModuleCtl(g_aiq_ctx, RK_MODULE_AWB, false);
        if (awb_ctl_ret == XCAM_RETURN_NO_ERROR) {
            std::cout << SLAVE_NAME << "：自动白平衡模块已关闭" << std::endl;
        } else {
            std::cerr << SLAVE_NAME << "：警告: 关闭自动白平衡模块失败，错误码: " << awb_ctl_ret << std::endl;
        }
        
        printCurrentExposureInfo();
    }
    
    // 5. 初始化UDP传输（替代RTSP）
    std::cout << SLAVE_NAME << "：初始化UDP传输..." << std::endl;
    if (!init_udp_sender_global(MY_UDP_TARGET_IP,SLAVE_ID)) {  // 3568的IP
        std::cerr << SLAVE_NAME << "：UDP传输初始化失败，程序退出" << std::endl;
        goto cleanup_venc;
    }
    std::cout << SLAVE_NAME << "：UDP传输已启动" << std::endl;

    // 6. 启动图像处理线程
    std::cout << SLAVE_NAME << "：启动后台图像处理线程..." << std::endl;
    if (pthread_create(&capture_tid, nullptr, capture_encode_thread_func, reinterpret_cast<void*>(VencChn)) != 0) {
        perror("创建后台线程失败");
        goto cleanup_udp;
    }
    std::cout << SLAVE_NAME << "：后台图像处理线程已创建" << std::endl;
    
    // 主循环
    std::cout << SLAVE_NAME << "：服务器运行中。按 Ctrl+C 退出" << std::endl;
    std::cout << SLAVE_NAME << "：等待主机命令..." << std::endl;
    
    // 添加手动触发测试（调试用）
    sleep(5);
    std::cout << SLAVE_NAME << "：手动触发连续模式测试..." << std::endl;
    capture_mode = 11;
    need_clear_buffer = true;
        
    while (keep_running) {
        sleep(1);
    }
    
    std::cout << SLAVE_NAME << "：正在关闭..." << std::endl;
    
    // 清理PLC
    if (plc_tid) {
        pthread_join(plc_tid, nullptr);
    }
    
    if (g_plc) {
        g_plc->stop();
        delete g_plc;
    }
    
    // 清理图像处理线程
    if (capture_tid) {
        std::cout << SLAVE_NAME << "：等待后台线程结束..." << std::endl;
        pthread_join(capture_tid, nullptr);
        std::cout << SLAVE_NAME << "：后台线程已结束" << std::endl;
    }

cleanup_udp:
    cleanup_udp_sender_global();
    
cleanup_venc:
    std::cout << SLAVE_NAME << "：停止 VENC 接收帧..." << std::endl;
    RK_MPI_VENC_StopRecvFrame(VencChn);
    std::cout << SLAVE_NAME << "：退出 VENC 通道 " << VencChn << "..." << std::endl;
    RK_MPI_VENC_DestroyChn(VencChn);
    
cleanup_vi_chn:
    std::cout << SLAVE_NAME << "：退出 VI 通道 " << ViChn << "..." << std::endl;
    RK_MPI_VI_DisableChn(ViPipe, ViChn);
    
cleanup_vi_dev:
    std::cout << SLAVE_NAME << "：退出 VI 设备..." << std::endl;
    RK_MPI_VI_DisableDev(ViDev);
    
cleanup_sys:
    std::cout << SLAVE_NAME << "：退出 RK_MPI 系统..." << std::endl;
    RK_MPI_SYS_Exit();
    std::cout << SLAVE_NAME << "：停止 ISP..." << std::endl;
    SAMPLE_COMM_ISP_Stop(ViDev);
    
    std::cout << SLAVE_NAME << "：清理完成。程序退出" << std::endl;
    return 0;
}