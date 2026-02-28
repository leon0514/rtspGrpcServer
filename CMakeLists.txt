cmake_minimum_required(VERSION 3.18)
# 显式启用 CXX 和 CUDA 语言支持
project(rtsp_grpc_server LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)

# ==========================================
# 1. 修复 GPU 架构问题
# ==========================================
# 显式指定常见架构，避免 native 探测失败
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 75 80 86 89)
endif()

# ==========================================
# 2. 查找依赖
# ==========================================

# 查找 CUDA 工具包（这会定义 CUDA::cudart, CUDA::cuda_driver 等目标）
find_package(CUDAToolkit REQUIRED)

# 查找 OpenCV
find_package(OpenCV REQUIRED)

# 查找 Protobuf
find_package(Protobuf REQUIRED)

# 查找 PkgConfig 工具
find_package(PkgConfig REQUIRED)

# 查找 gRPC
pkg_check_modules(GRPC REQUIRED grpc++ grpc)

# 查找 spdlog
pkg_check_modules(SPDLOG REQUIRED spdlog)

# 查找 FFmpeg 组件
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)

find_library(NVCUVID_LIBRARY 
    NAMES nvcuvid 
    PATHS /usr/local/cuda/lib64 /usr/lib/x86_64-linux-gnu
    NO_DEFAULT_PATH # 强制只从指定路径找
)

if(NOT NVCUVID_LIBRARY)
    # 仅警告，不调用 FATAL_ERROR
    message(WARNING "nvcuvid library NOT found during build time. Ensure it is available at runtime!")
    # 设置一个空值，确保链接不报错
    set(NVCUVID_LIBRARY "")
else()
    message(STATUS "Found nvcuvid: ${NVCUVID_LIBRARY}")
endif()

# 查找 gRPC C++ 插件
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

# ==========================================
# 3. Protobuf 生成
# ==========================================
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/stream_service.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/stream_service.pb.cc"
           "${PROTO_GEN_DIR}/stream_service.pb.h"
           "${PROTO_GEN_DIR}/stream_service.grpc.pb.cc"
           "${PROTO_GEN_DIR}/stream_service.grpc.pb.h"
    COMMAND protoc
    ARGS --grpc_out="${PROTO_GEN_DIR}"
         --cpp_out="${PROTO_GEN_DIR}"
         -I "${CMAKE_CURRENT_SOURCE_DIR}"
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}"
)

# ==========================================
# 4. 头文件路径
# ==========================================
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvcuvid"
    "${PROTO_GEN_DIR}"
    ${OpenCV_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${CUDAToolkit_INCLUDE_DIRS} # 关键：包含 cuda.h 路径
)

# ==========================================
# 5. 源文件管理
# ==========================================
set(SRC_FILES
    src/main.cpp
    src/rtsp_service.cpp
    src/stream_task.cpp
    src/decoder_factory.cpp
    src/opencv_encoder.cpp
    src/nvjpeg_encoder.cpp   # GPU JPEG 编码器
    src/opencv_decoder.cpp
    src/ffmpeg_demuxer.cpp
    src/cuda_tools.cpp
    src/cuvid_decoder.cpp
    src/color.cu            
    src/simple-logger.cpp
    src/cuda_decoder.cpp
)

# ==========================================
# 6. 构建目标
# ==========================================
add_executable(rtsp_server 
    ${SRC_FILES}
    "${PROTO_GEN_DIR}/stream_service.pb.cc" 
    "${PROTO_GEN_DIR}/stream_service.grpc.pb.cc"
)

# 开启 CUDA 分离编译
set_target_properties(rtsp_server PROPERTIES 
    CUDA_SEPARABLE_COMPILATION ON
)

# ==========================================
# 7. 链接库 (修复了 CUDA 目标的命名)
# ==========================================
target_link_libraries(rtsp_server 
    PRIVATE
    ${OpenCV_LIBS}
    ${GRPC_LIBRARIES}
    ${SPDLOG_LIBRARIES}
    ${FFMPEG_LIBRARIES}     
    ${NVCUVID_LIBRARY}      
    CUDA::cudart              # CUDA Runtime API (cudaMalloc等)
    CUDA::cuda_driver         # CUDA Driver API (cuInit等，解决cuda.h相关链接)
    CUDA::nvjpeg              # NVJPEG GPU 编码库
    protobuf::libprotobuf 
    grpc++_reflection
)