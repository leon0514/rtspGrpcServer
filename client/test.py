import time
import os
import cv2
import threading
from remote_capture import (
    RemoteCapture, 
    DECODER_GPU_NVCUVID,
    STATUS_CONNECTED,
    STATUS_DISCONNECTED,
    STATUS_NOT_FOUND
)

SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50051")
# 请替换为你实际可用的 RTSP 地址
RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/1001"

def wait_for_connection(client, stream_id, timeout_sec=10):
    """等待流连接成功的辅助函数"""
    for i in range(timeout_sec):
        status = client.get_stream_status(stream_id)
        if status == STATUS_CONNECTED:
            return True
        elif status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
            return False
        time.sleep(1)
    return False

def test_sequential_open_close(iterations=10, frames_per_iter=30):
    """
    测试场景一：串行反复开关流
    目的：观察内存是否在最初几次增长后达到天花板，还是会无限增长。
    """
    print("=" * 60)
    print(f"🚀 开始测试：串行反复开关流 (共 {iterations} 次循环)")
    print("👉 请在服务器端同时观察 top 和 nvidia-smi 的内存变化")
    print("=" * 60)

    with RemoteCapture(SERVER) as client:
        for i in range(iterations):
            print(f"\n--- [循环 {i+1}/{iterations}] ---")
            
            # 1. 开流
            stream_id = client.start_stream(
                RTSP_URL, 
                decoder_type=DECODER_GPU_NVCUVID, 
                gpu_id=0,
                decode_interval_ms=30 # 限制一下解码速度，防止刷太快
            )
            
            if not stream_id:
                print("❌ 开流请求失败，跳过本次循环")
                continue

            print(f"✅ 发起开流请求成功，Stream ID: {stream_id}")
            
            # 2. 等待连接
            if not wait_for_connection(client, stream_id):
                print("❌ 流连接失败，强制关流")
                client.stop_stream(stream_id)
                continue

            print("📡 流已连接，开始拉取图像...")
            
            # 3. 读取指定数量的帧
            count = 0
            # 注意：这里使用 stream_frames 生成器。
            # 当我们 break 退出循环时，Python 回收生成器会自动断开 gRPC Stream，
            # 服务器端的 context->IsCancelled() 会触发。
            for ret, frame in client.stream_frames(stream_id, max_fps=0):
                if ret:
                    count += 1
                    if count % 10 == 0:
                        print(f"  -> 已获取 {count}/{frames_per_iter} 帧")
                else:
                    print("  -> 获取帧失败，可能流已断开")
                    break
                
                if count >= frames_per_iter:
                    break
            
            # 4. 关流
            print("🛑 停止获取，正在发送关流请求...")
            client.stop_stream(stream_id)
            
            # 5. 留出时间让 C++ 服务器执行 cleanupLoop 和内存回收
            print("⏳ 关流完成，等待 3 秒让服务器回收内存...")
            time.sleep(3)
            
    print("\n🎉 串行测试结束！")


def test_concurrent_stress(concurrent_num=5, iterations=5, read_seconds=5):
    """
    测试场景二：并发压力测试
    目的：测试多线程并发开流/关流时，是否存在内存碎片剧增或线程池死锁。
    """
    print("\n" + "=" * 60)
    print(f"🚀 开始测试：并发压力测试 ({concurrent_num}路并发, 共 {iterations} 轮)")
    print("=" * 60)

    with RemoteCapture(SERVER) as client:
        for i in range(iterations):
            print(f"\n--- [并发轮次 {i+1}/{iterations}] ---")
            
            stream_ids = []
            # 1. 并发开流
            for j in range(concurrent_num):
                sid = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0)
                if sid:
                    stream_ids.append(sid)
            
            print(f"✅ 成功发起 {len(stream_ids)} 路开流请求，等待连接...")
            time.sleep(3) # 粗略等待连接建立

            # 2. 持续拉取一段时间（使用主动 read 模式轮询多路流）
            print(f"📡 开始持续拉取 {read_seconds} 秒...")
            start_time = time.time()
            frames_read = 0
            while time.time() - start_time < read_seconds:
                for sid in stream_ids:
                    ret, frame = client.read(sid)
                    if ret:
                        frames_read += 1
                time.sleep(0.05)
            
            print(f"📊 本轮共成功读取 {frames_read} 帧")

            # 3. 并发关流
            print("🛑 正在批量关流...")
            for sid in stream_ids:
                client.stop_stream(sid)
            
            print("⏳ 等待 4 秒回收内存...")
            time.sleep(4)

    print("\n🎉 并发测试结束！")

if __name__ == "__main__":
    print("请在运行此脚本前，在服务器上打开两个终端：")
    print("终端1: watch -n 1 nvidia-smi")
    print("终端2: top -p $(pidof your_server_executable)")
    input("\n准备好后，按回车键开始测试...")

    # 运行串行测试（强烈建议先跑这个，观察内存是否封顶）
    # test_sequential_open_close(iterations=300, frames_per_iter=50)
    
    # 如果串行测试通过（内存涨到一定程度不涨了），再跑并发测试
    test_concurrent_stress(concurrent_num=4, iterations=5, read_seconds=5)
