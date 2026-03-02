"""
RTSP gRPC 客户端示例
"""
import cv2
import time
from remote_capture import (
    RemoteCapture, 
    DECODER_GPU_CUDA,
    DECODER_CPU_OPENCV,
    DECODER_FFMPEG_NATIVE,
    STATUS_CONNECTING,
    STATUS_CONNECTED,
    STATUS_DISCONNECTED,
    STATUS_NOT_FOUND,
    STATUS_NAMES
)

import os

# default address can still be overridden via environment var
SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50051")
RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/801"
RTSP_URL = "rtsp://admin:Admin12345@219.129.97.98:2010/Streaming/channels/101"


def example_list_streams():
    """示例1: 获取所有流信息"""
    print("=" * 50)
    print("示例1: 获取所有流信息")
    print("=" * 50)
    
    with RemoteCapture(SERVER) as client:
        streams = client.list_streams()
        print(f"流总数: {len(streams)}\n")
        
        for s in streams:
            print(f"ID: {s['stream_id'][:8]}...")
            print(f"  URL:    {s['rtsp_url']}")
            print(f"  状态:   {s['status_name']}")
            print(f"  解码器: {s['decoder_type']}")
            print(f"  分辨率: {s['width']}x{s['height']}")
            print()


def example_poll_frame():
    """示例2: 主动请求获取帧"""
    print("=" * 50)
    print("示例2: 主动请求获取帧")
    print("=" * 50)
    
    with RemoteCapture(SERVER) as client:
        # 启动流
        stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_CUDA)
        if not stream_id:
            return
        
        print(f"流已启动: {stream_id[:8]}...")
        
        # 等待连接成功
        for i in range(10):
            if client.get_stream_status(stream_id) == STATUS_CONNECTED:
                print("连接成功")
                break
            print(f"等待连接... ({i+1}/10)")
            time.sleep(1)
        
        if client.get_stream_status(stream_id) != STATUS_CONNECTED:
            print("连接失败")
            return

        while True:
            ret, frame = client.read(stream_id)
            if ret:
                cv2.imwrite("capture.jpg", frame)
            else:
                status = client.get_stream_status(stream_id)
                if status == STATUS_DISCONNECTED or status == STATUS_NOT_FOUND:
                    print("连接已断开")
                    break
                print(f"获取帧失败, 状态: {STATUS_NAMES[status]}")
                
        client.stop_stream(stream_id)
        print("流已停止")


def example_stream_frames():
    """示例3: 流式传输获取帧"""
    print("=" * 50)
    print("示例3: 流式传输获取帧")
    print("=" * 50)
    
    with RemoteCapture(SERVER) as client:
        # 启动流
        stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_CUDA)
        if not stream_id:
            return
        
        for i in range(10):
            if client.get_stream_status(stream_id) == STATUS_CONNECTED:
                print("连接成功")
                break
            print(f"等待连接... ({i+1}/10)")
            time.sleep(1)
        
        frame_count = 0
        start = time.time()
        
        for ret, frame in client.stream_frames(stream_id, max_fps=1):
            if ret:
                frame_count += 1
                print(f"接收帧: {frame_count}")
            else:
                status = client.get_stream_status(stream_id)
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    print("连接已断开或流不存在")
                    break
                else:
                    print(f"接收帧失败, 状态: {STATUS_NAMES[status]}")
        
        fps = frame_count / (time.time() - start)
        print(f"\n接收 {frame_count} 帧, 平均 {fps:.1f} FPS")
        client.stop_stream(stream_id)
        print("流已停止")


if __name__ == "__main__":
    # 运行示例 (取消注释需要运行的示例)
    
    example_list_streams()
    # example_poll_frame()
    example_stream_frames()
