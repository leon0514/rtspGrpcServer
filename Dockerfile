# ==========================================
# 第一阶段：编译环境 (Builder)
# ==========================================
FROM ubuntu:22.04 AS builder

# 设置非交互模式，防止 apt-get 安装时卡在时区选择
ENV DEBIAN_FRONTEND=noninteractive

# 【新增】替换为阿里云镜像源 (加速下载)
# 替换 archive.ubuntu.com 和 security.ubuntu.com 为 mirrors.aliyun.com
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# 安装编译所需的依赖链：GCC, CMake, OpenCV, gRPC, Protobuf
RUN apt-get update -y && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libopencv-dev \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    protobuf-compiler \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 拷贝当前目录的所有源码到容器内
COPY . /app

# 执行 CMake 构建
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# ==========================================
# 第二阶段：运行环境 (Runtime)
# ==========================================
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 【新增】替换为阿里云镜像源 (加速下载)
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# 安装运行所需的动态链接库 (OpenCV 和 gRPC)
# 注意：这里安装 -dev 包是为了确保所有的 .so 软链接都完整存在，体积增加可接受
RUN apt-get update && apt-get install -y \
    libopencv-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 从 builder 阶段把编译好的二进制可执行文件拷贝过来
COPY --from=builder /app/build/rtsp_server /app/rtsp_server

# 暴露 gRPC 默认监听的 50051 端口
EXPOSE 50051

# 启动服务端
CMD ["./rtsp_server"]