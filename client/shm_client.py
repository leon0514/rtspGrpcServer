#!/usr/bin/env python3
import cv2
import sys
import time
from shm_capture import ShmCapture, DECODER_GPU_NVCUVID

SERVER = "127.0.0.1:50052"
RTSP = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901"

def main():
    with ShmCapture(SERVER) as cap:
        # 打开流（自动启用共享内存）
        if not cap.open(RTSP, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0):
            print("✗ 打开失败")
            return 1
        print(f"✓ 已打开: {RTSP[:50]}...")
        
        # 主循环
        frame_count = 0
        start_time = time.time()
        
        while cap.isOpened():
            ret, frame = cap.read()
            if not ret or frame is None:
                time.sleep(0.05)  # 无新帧时短暂休眠
                continue
            frame_count += 1
            
            # 每秒打印统计
            now = time.time()
            if now - start_time >= 1.0:
                fps = frame_count / (now - start_time)
                h, w = frame.shape[:2]
                print(f"📊 {fps:.1f} FPS | {w}x{h} | {frame.dtype}")
                start_time = now
                frame_count = 0
        
        print("✓ 退出")
        return 0

if __name__ == "__main__":
    sys.exit(main())