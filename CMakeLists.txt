cmake_minimum_required(VERSION 3.10)

# Required:  -DRKMPI_ROOT=<path to LuckfoxTECH/luckfox_pico_rkmpi_example>
#            -DTOOLCHAIN_BIN=<path to arm-rockchip830-linux-uclibcgnueabihf/bin>
if(NOT RKMPI_ROOT OR NOT TOOLCHAIN_BIN)
    message(FATAL_ERROR "Set RKMPI_ROOT and TOOLCHAIN_BIN")
endif()

set(CMAKE_C_COMPILER   "${TOOLCHAIN_BIN}/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_BIN}/arm-rockchip830-linux-uclibcgnueabihf-g++")

project(luckfox_jpeg_server)

add_definitions(-DRV1106_1103)
link_directories(${RKMPI_ROOT}/lib/uclibc)

set(OpenCV_DIR "${RKMPI_ROOT}/lib/uclibc/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")
add_executable(${PROJECT_NAME} ${SRC_FILES})

target_compile_options(${PROJECT_NAME} PRIVATE
    -Os -Wall -ffunction-sections -fdata-sections
    -DISP_HW_V30 -DRKPLATFORM=ON -DARCH64=OFF -DUAPI2
    -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${RKMPI_ROOT}/lib:/usr/lib -Wl,--gc-sections -s")

target_link_libraries(${PROJECT_NAME}
    ${OpenCV_LIBS}
    rknnmrt
    Threads::Threads
    rockiva
    sample_comm
    rockit
    rockchip_mpp
    rkaiq
    pthread
    rga)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${RKMPI_ROOT}
    ${RKMPI_ROOT}/include
    ${RKMPI_ROOT}/include/rknn
    ${RKMPI_ROOT}/include/librga
    ${RKMPI_ROOT}/include/rkaiq
    ${RKMPI_ROOT}/include/rkaiq/uAPI2
    ${RKMPI_ROOT}/include/rkaiq/common
    ${RKMPI_ROOT}/include/rkaiq/xcore
    ${RKMPI_ROOT}/include/rkaiq/algos
    ${RKMPI_ROOT}/include/rkaiq/iq_parser
    ${RKMPI_ROOT}/include/rkaiq/iq_parser_v2
    ${RKMPI_ROOT}/include/rkaiq/smartIr)
