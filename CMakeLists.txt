cmake_minimum_required(VERSION 3.10)

# 检查 SDK 路径环境变量
set(SDK_PATH $ENV{LUCKFOX_SDK_PATH})
if(NOT SDK_PATH)
    message(WARNING "Please Set Luckfox-pico SDK Path. Such as: export LUCKFOX_SDK_PATH=/home/user/luckfox-pico")
    return()
endif()

# 设置交叉编译工具链
set(CMAKE_C_COMPILER "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")

# 项目名称
project(luckfox_jpeg_server)

# 固定路径设置 - 使用绝对路径而不是相对路径
set(ROOT_PATH "/home/powin/luckfox_pico_rkmpi_example-kernel")
set(PROJECT_PATH "${ROOT_PATH}/example/luckfox_jpeg_server")

# 设置库路径
link_directories(${ROOT_PATH}/lib)

# 添加3rdparty
add_subdirectory(${ROOT_PATH}/3rdparty/ 3rdparty.out)

# 定义
add_definitions(-DRV1106_1103)

# 设置线程库
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# 添加源文件
file(GLOB SRC_FILES "${PROJECT_PATH}/src/*.cc")
add_executable(${PROJECT_NAME} ${SRC_FILES})

# 编译选项
target_compile_options(${PROJECT_NAME} PRIVATE
    -g 
    -Wall
    -DISP_HW_V30 
    -DRKPLATFORM=ON 
    -DARCH64=OFF
    -DROCKIVA 
    -DUAPI2
    -D_LARGEFILE_SOURCE 
    -D_LARGEFILE64_SOURCE 
    -D_FILE_OFFSET_BITS=64
)

# 链接器标志
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${ROOT_PATH}/lib:/usr/lib")
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

# 链接库
target_link_libraries(${PROJECT_NAME}
    Threads::Threads
    rockiva
    sample_comm
    rockit
    rockchip_mpp
    rkaiq
    pthread
    rtsp
)

# 包含目录 - 使用与根目录 CMakeLists.txt 相同的路径
target_include_directories(${PROJECT_NAME} PRIVATE
    ${PROJECT_PATH}/include
    ${ROOT_PATH}
    ${ROOT_PATH}/utils
    ${ROOT_PATH}/3rdparty/rknpu2/include
    ${ROOT_PATH}/common 
    ${ROOT_PATH}/common/isp3.x   
    ${ROOT_PATH}/include
    ${ROOT_PATH}/include/rknn
    ${ROOT_PATH}/include/rkaiq
    ${ROOT_PATH}/include/rkaiq/uAPI2
    ${ROOT_PATH}/include/rkaiq/common
    ${ROOT_PATH}/include/rkaiq/xcore
    ${ROOT_PATH}/include/rkaiq/algos
    ${ROOT_PATH}/include/rkaiq/iq_parser
    ${ROOT_PATH}/include/rkaiq/iq_parser_v2
    ${ROOT_PATH}/include/rkaiq/smartIr
)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
