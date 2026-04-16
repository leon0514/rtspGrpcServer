"""
RTSP gRPC 客户端示例
"""
import cv2
import time
from remote_capture import (
    RemoteCapture, 
    DECODER_GPU_NVCUVID,
    DECODER_CPU_FFMPEG,
    STATUS_CONNECTING,
    STATUS_CONNECTED,
    STATUS_DISCONNECTED,
    STATUS_NOT_FOUND,
    STATUS_NAMES
)

import os
import datetime
import threading

# default address can still be overridden via environment var
SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50052")
RTSP_URL = "rtsp://admin:Admin12345@219.129.97.98:2011/Streaming/channels/101"
# RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901"
RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"


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
            print(f"  解码间隔：{s['decode_interval_ms']}")
            print(f"  心跳时间：{s['heartbeat_timeout_ms']}")
            print(f"  仅关键帧：{s['only_key_frames']}")
            print()


def example_poll_frame():
    """示例2: 主动请求获取帧"""
    print("=" * 50)
    print("示例2: 主动请求获取帧")
    print("=" * 50)
    
    with RemoteCapture(SERVER) as client:
        # 启动流
        stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0, only_key_frames=True)
        if not stream_id:
            return
        
        print(f"流已启动: {stream_id[:8]}...")
        
        # 等待连接成功
        status = STATUS_CONNECTING
        while status == STATUS_CONNECTING:
            status = client.get_stream_status(stream_id)
            if status != STATUS_CONNECTING:
                break
            print(f"等待连接...")
            time.sleep(1)
        
        if status != STATUS_CONNECTED:
            print("连接失败")
            return
        else:
            print("连接成功")

        while True:
            frame_seq, frame = client.read(stream_id)
            if frame_seq != -1:
                print(f"获取帧成功, 序列号: {frame_seq}")
                cv2.imwrite("capture.jpg", frame)
                break
            else:
                status = client.get_stream_status(stream_id)
                if status == STATUS_DISCONNECTED or status == STATUS_NOT_FOUND:
                    print("连接已断开")
                    break
                print(f"获取帧失败, 状态: {STATUS_NAMES[status]}")
                
        # client.stop_stream(stream_id)
        print("流已停止")


def example_stream_frames(rtsp_url):
    """示例3: 流式传输获取帧"""
    print("=" * 50)
    print("示例3: 流式传输获取帧")
    print("=" * 50)
    
    with RemoteCapture(SERVER) as client:
        # 启动流
        stream_id = client.start_stream(rtsp_url, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0, only_key_frames=True)
        # stream_id = "50fdcc9dd42e15f7"
        if not stream_id:
            return
        
        if not os.path.exists(f"images/{stream_id}"):
            os.mkdir(f"images/{stream_id}")
        
        status = STATUS_CONNECTING
        while status == STATUS_CONNECTING:
            status = client.get_stream_status(stream_id)
            if status != STATUS_CONNECTING:
                break
            print(f"等待连接...")
            time.sleep(1)
        
        if status != STATUS_CONNECTED:
            print("连接失败")
            return
        else:
            print("连接成功")

        start = time.time()
        
        for seq, frame in client.stream_frames(stream_id, max_fps=25):
            if seq != -1:
                print(f"接收帧: {seq}")
                time_stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                cv2.imwrite(f"images/{stream_id}/{time_stamp}.jpg", frame)
            else:
                status = client.get_stream_status(stream_id)
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    print("连接已断开或流不存在")
                    break
                else:
                    print(f"接收帧失败, 状态: {STATUS_NAMES[status]}")
        
        fps = seq / (time.time() - start)
        print(f"\n接收 {seq} 帧, 平均 {fps:.1f} FPS")
        client.stop_stream(stream_id)
        print("流已停止")


if __name__ == "__main__":
    # 运行示例 (取消注释需要运行的示例)
    
    example_list_streams()
    # example_poll_frame()
    rtsp_list = ["rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/201",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/301",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/401",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/501",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/601",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/701",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/801",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1001",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1101",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1201",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1401",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1501",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1601"]
    # 使用线程来测试
    threads = []

    for rtsp_url in rtsp_list:
        t = threading.Thread(target=example_stream_frames, args=(rtsp_url,))
        t.start()
        threads.append(t)
    
    # 等待所有线程完成
    for t in threads:
        t.join()
    print("所有示例已完成")
