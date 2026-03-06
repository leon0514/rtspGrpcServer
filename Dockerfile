# 使用 NVIDIA 运行环境镜像，支持 GPU 加速
FROM nvcr.io/nvidia/cuda:12.6.3-runtime-ubuntu22.04

# 1. 设置非交互环境，更新镜像源
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# 2. 安装程序运行必须的动态链接库
# 注意：你需要安装程序实际依赖的运行库（.so）
RUN apt-get update && apt-get install -y \
    libopencv-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    libspdlog-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libjemalloc-dev \
    && rm -rf /var/lib/apt/lists/*

# 3. 配置 nvcuvid 软链接 (确保运行时能找到驱动库)
RUN ln -s /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 /usr/lib/x86_64-linux-gnu/libnvcuvid.so

# 4. 拷贝你的编译好的二进制文件
WORKDIR /app
COPY build/rtsp_server /app/rtsp_server
COPY entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh


# 暴露端口
EXPOSE 50051

ENV MALLOC_CONF="dirty_decay_ms:100"
# 启动服务
ENTRYPOINT ["/app/entrypoint.sh"]