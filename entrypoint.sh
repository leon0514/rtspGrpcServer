#!/bin/bash

ldconfig

# 0. 清理上一次运行遗留的共享内存对象，防止服务崩溃/重启后 /dev/shm 累积导致 ENOSPC
#    stream_id 现在统一带 rtsp_grpc_ 前缀
rm -f /dev/shm/rtsp_grpc_* /dev/shm/sem.rtsp_grpc_*_notify

# 0. 确保海康 SDK 库可被动态加载（Dockerfile 中已通过 ENV 设置，此处作为兜底）
export LD_LIBRARY_PATH=/opt/hikvision/hik_libs:/opt/hikvision/hik_libs/HCNetSDKCom:${LD_LIBRARY_PATH}

# 1. 在运行时创建软链接 (此时驱动已通过 --gpus all 挂载进来了)
if [ -f /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 ]; then
    ln -sf /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 /usr/lib/x86_64-linux-gnu/libnvcuvid.so
fi

export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2

# 2. 确保挂载进来的二进制有执行权限（docker-compose 挂载时可能丢失 x 位）
chmod +x /app/rtsp_server

# 3. 启动你的服务
exec ./rtsp_server