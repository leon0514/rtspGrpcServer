"""
RTSP gRPC / SharedMemory 客户端综合示例

演示统一客户端 RTSPClient 的各类用法：
  1. 列出现有流
  2. 启动流（CPU / GPU / 仅关键帧 / 共享内存）
  3. 主动轮询读帧（gRPC JPEG）
  4. 服务端流式推送（gRPC streaming）
  5. 共享内存零拷贝读帧
  6. 多线程并发拉取多路流
  7. gRPC 与 SHM 性能对比
  8. 批量开关流
  9. 动态切换 RTSP URL
  10. 断线重连与异常处理

运行方式:
    python example.py

默认通过环境变量 GRPC_SERVER 指定服务端地址；未设置时使用 127.0.0.1:50051。
"""

import os
import sys
import time
import cv2
import threading
import datetime
from typing import Optional, List

from remote_capture import (
    RTSPClient,
    DECODER_GPU_NVCUVID,
    DECODER_CPU_FFMPEG,
    DECODER_HIK_SDK,
    DECODER_NAMES,
    STATUS_CONNECTING,
    STATUS_CONNECTED,
    STATUS_DISCONNECTED,
    STATUS_NOT_FOUND,
    STATUS_NAMES,
)

# ==================== 配置 ====================

SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50051")
# 请替换为你实际可用的 RTSP 地址
RTSP_URL = os.getenv(
    "RTSP_URL",
    "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"
)

OUTPUT_DIR = "images"


def ensure_output_dir(stream_id: str) -> str:
    """为每路流创建独立输出目录"""
    path = os.path.join(OUTPUT_DIR, stream_id)
    os.makedirs(path, exist_ok=True)
    return path


def wait_for_connection(client, stream_id: str, timeout_sec: int = 15) -> bool:
    """通用等待连接成功辅助函数"""
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        status = client.get_stream_status(stream_id)
        if status == STATUS_CONNECTED:
            return True
        if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
            logger.error(f"流连接失败: {STATUS_NAMES.get(status, '未知')}")
            return False
        time.sleep(0.2)
    logger.error("等待流连接超时")
    return False


# ==================== 示例 1: 列出现有流 ====================

def example_list_streams():
    """查询服务端当前所有流的基本信息"""
    print("\n" + "=" * 60)
    print("示例 1: 列出服务端所有流")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        streams = client.list_streams()
        print(f"流总数: {len(streams)}\n")
        for s in streams:
            print(f"ID:       {s['stream_id']}")
            print(f"  URL:    {s['rtsp_url']}")
            print(f"  状态:   {s['status_name']}")
            print(f"  解码器: {s['decoder_type']}")
            print(f"  分辨率: {s['width']}x{s['height']}")
            print(f"  SHM:    {s['use_shared_mem']}")
            print(f"  仅关键帧: {s['only_key_frames']}")
            print()


# ==================== 示例 2: 主动轮询读帧 (gRPC JPEG) ====================

def example_poll_frame(decoder_type: int = DECODER_CPU_FFMPEG,
                       only_key_frames: bool = False,
                       max_frames: int = 50):
    """
    启动一路流，使用主动轮询（GetLatestFrame）获取帧

    :param decoder_type: CPU 或 GPU 解码器
    :param only_key_frames: 是否只解码关键帧
    :param max_frames: 最多读取多少帧后退出
    """
    print("\n" + "=" * 60)
    print(f"示例 2: 主动轮询读帧 ({DECODER_NAMES.get(decoder_type, '?')}, "
          f"仅关键帧={only_key_frames})")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            RTSP_URL,
            decoder_type=decoder_type,
            gpu_id=0,
            only_key_frames=only_key_frames,
            use_shared_mem=False
        )
        if not stream_id:
            print("启动流失败")
            return

        print(f"流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return
        print("流已连接，开始读取...")

        ok_count = 0
        fail_count = 0
        start = time.time()

        while ok_count < max_frames:
            ts, frame = client.read(stream_id)
            if ts != -1 and frame is not None:
                ok_count += 1
                if ok_count % 10 == 0:
                    h, w = frame.shape[:2]
                    print(f"  已读取 {ok_count} 帧, 最新 ts={ts}, 分辨率={w}x{h}")
            else:
                fail_count += 1
                status = client.get_stream_status(stream_id)
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    print(f"  流已断开: {STATUS_NAMES.get(status)}")
                    break
                if status == STATUS_CONNECTING:
                    time.sleep(0.2)
                    continue
                if fail_count > 30:
                    print("  连续获取失败次数过多，退出")
                    break
                time.sleep(0.05)

        elapsed = time.time() - start
        print(f"\n读取 {ok_count} 帧, 失败 {fail_count} 次, 耗时 {elapsed:.2f}s, "
              f"平均 {(ok_count / elapsed):.1f} FPS")
        client.stop_stream(stream_id)
        print("流已停止")


# ==================== 示例 3: 服务端流式推送 (gRPC streaming) ====================

def example_stream_frames(save_images: bool = False, max_frames: int = 100):
    """
    使用服务端流式推送 StreamFrames 获取帧

    :param save_images: 是否把每帧保存为图片
    :param max_frames: 最多读取多少帧
    """
    print("\n" + "=" * 60)
    print("示例 3: 服务端流式推送读帧")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            RTSP_URL,
            decoder_type=DECODER_CPU_FFMPEG,
            only_key_frames=False,
            use_shared_mem=False
        )
        if not stream_id:
            print("启动流失败")
            return

        print(f"流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return
        print("流已连接，开始接收推送...")

        output_path = ensure_output_dir(stream_id) if save_images else None
        ok_count = 0
        start = time.time()

        for ts, frame in client.stream_frames(stream_id, max_fps=0):
            if ts != -1 and frame is not None:
                ok_count += 1
                if save_images:
                    fname = datetime.datetime.fromtimestamp(ts / 1000).strftime("%Y%m%d_%H%M%S_%f")[:-3]
                    cv2.imwrite(os.path.join(output_path, f"{fname}.jpg"), frame)
                if ok_count % 20 == 0:
                    print(f"  已接收 {ok_count} 帧, ts={ts}")
            else:
                status = client.get_stream_status(stream_id)
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    print(f"  流已断开: {STATUS_NAMES.get(status)}")
                    break
                if status == STATUS_CONNECTING:
                    time.sleep(0.2)
                    continue
                if status == STATUS_CONNECTING:
                    time.sleep(0.2)
                    continue
                print(f"  接收帧失败, 状态: {STATUS_NAMES.get(status, '未知')}")

            if ok_count >= max_frames:
                break

        elapsed = time.time() - start
        print(f"\n接收 {ok_count} 帧, 耗时 {elapsed:.2f}s, "
              f"平均 {(ok_count / elapsed):.1f} FPS")
        client.stop_stream(stream_id)
        print("流已停止")


# ==================== 示例 4: 共享内存读帧 (SHM) ====================

def example_shared_memory(blocking: bool = True,
                          duration_sec: int = 10,
                          decoder_type: int = DECODER_GPU_NVCUVID):
    """
    通过共享内存零拷贝读取原始帧

    注意：客户端必须与服务端在同一台机器，且服务端容器启动时挂载 -v /dev/shm:/dev/shm。
    """
    print("\n" + "=" * 60)
    print(f"示例 4: 共享内存读帧 ({DECODER_NAMES.get(decoder_type, '?')})")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            RTSP_URL,
            decoder_type=decoder_type,
            gpu_id=0,
            only_key_frames=False,
            use_shared_mem=True
        )
        if not stream_id:
            print("启动流失败")
            return

        print(f"流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return
        print("流已连接，开始通过 SHM 读取...")

        ok_count = 0
        start = time.time()

        while time.time() - start < duration_sec:
            ts, frame = client.read(stream_id, blocking=blocking, timeout_ms=1000)
            if ts != -1 and frame is not None:
                ok_count += 1
                if ok_count % 100 == 0:
                    h, w = frame.shape[:2]
                    print(f"  已读取 {ok_count} 帧, ts={ts}, {w}x{h}")
            else:
                status = client.get_stream_status(stream_id)
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    print(f"  流已断开: {STATUS_NAMES.get(status)}")
                    break
                if status == STATUS_CONNECTING:
                    time.sleep(0.2)
                    continue

        elapsed = time.time() - start
        print(f"\nSHM 读取 {ok_count} 帧, 耗时 {elapsed:.2f}s, "
              f"平均 {(ok_count / elapsed):.1f} FPS")
        client.stop_stream(stream_id)
        print("流已停止")


# ==================== 示例 5: cv2.VideoCapture 风格 SHM 接口 ====================

def example_shm_capture_style(duration_sec: int = 10):
    """使用 RTSPClient 模拟 cv2.VideoCapture 风格的单流 SHM API"""
    print("\n" + "=" * 60)
    print("示例 5: RTSPClient 单流 SHM 模式 (类 VideoCapture API)")
    print("=" * 60)

    client = RTSPClient(SERVER)
    if not client.connect():
        print("连接失败")
        return

    stream_id = client.start_stream(
        RTSP_URL,
        decoder_type=DECODER_GPU_NVCUVID,
        use_shared_mem=True,
        only_key_frames=False
    )
    if not stream_id:
        print("启动流失败")
        client.disconnect()
        return

    if not wait_for_connection(client, stream_id):
        client.stop_stream(stream_id)
        client.disconnect()
        return

    print(f"SHM 流已打开: {stream_id}")

    frame_count = 0
    start = time.time()
    opened = True

    while opened and time.time() - start < duration_sec:
        ts, frame = client.read(stream_id, blocking=True, timeout_ms=1000)
        if ts != -1 and frame is not None:
            frame_count += 1
            if frame_count % 100 == 0:
                print(f"  已读取 {frame_count} 帧, shape={frame.shape}")
        else:
            status = client.get_stream_status(stream_id)
            if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                print("  流已断开")
                opened = False
            else:
                print("  读取失败")

    elapsed = time.time() - start
    print(f"\n读取 {frame_count} 帧, 耗时 {elapsed:.2f}s, "
          f"平均 {(frame_count / elapsed):.1f} FPS")
    client.stop_stream(stream_id)
    client.disconnect()


# ==================== 示例 6: 多线程并发多路流 ====================

def worker_pull_stream(rtsp_url: str, index: int, duration_sec: int = 10):
    """工作线程：启动一路流并持续拉取"""
    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            rtsp_url,
            decoder_type=DECODER_GPU_NVCUVID,
            gpu_id=0,
            only_key_frames=False,
            use_shared_mem=True
        )
        if not stream_id:
            print(f"[线程 {index}] 启动流失败")
            return

        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return

        frame_count = 0
        start = time.time()
        while time.time() - start < duration_sec:
            ts, frame = client.read(stream_id, blocking=True, timeout_ms=1000)
            if ts != -1 and frame is not None:
                frame_count += 1

        elapsed = time.time() - start
        print(f"[线程 {index}] 流 {stream_id[:8]}... 读取 {frame_count} 帧, "
              f"平均 {(frame_count / elapsed):.1f} FPS")
        client.stop_stream(stream_id)


def example_multi_thread(channels: int = 4, duration_sec: int = 10):
    """
    并发启动多路流，每路流在独立线程中通过 SHM 拉取

    :param channels: 并发路数
    :param duration_sec: 每路流拉取时长
    """
    print("\n" + "=" * 60)
    print(f"示例 6: 多线程并发 {channels} 路流")
    print("=" * 60)

    # 根据通道号构造不同 URL（假设摄像头通道为 101, 201, 301...）
    urls = [
        RTSP_URL.replace("Channels/101", f"Channels/{(i + 1) * 100 + 1}")
        for i in range(channels)
    ]

    threads = []
    for i, url in enumerate(urls):
        t = threading.Thread(target=worker_pull_stream, args=(url, i, duration_sec))
        t.start()
        threads.append(t)
        time.sleep(0.3)  # 错开启动，避免服务端瞬时压力过大

    for t in threads:
        t.join()

    print("\n所有线程已结束")


# ==================== 示例 7: gRPC vs SHM 性能对比 ====================

def example_benchmark(frames: int = 200):
    """
    对比同一路流在 gRPC JPEG 模式与 SHM 模式下的拉取性能

    注意：两次测试是顺序执行的，服务端会先关闭再重新开流。
    """
    print("\n" + "=" * 60)
    print(f"示例 7: gRPC JPEG vs SHM 性能对比 ({frames} 帧)")
    print("=" * 60)

    # gRPC JPEG
    with RTSPClient(SERVER) as client:
        sid = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_NVCUVID, use_shared_mem=False)
        if sid and wait_for_connection(client, sid):
            start = time.time()
            count = 0
            while count < frames:
                ts, frame = client.read(sid)
                if ts != -1 and frame is not None:
                    count += 1
            elapsed = time.time() - start
            print(f"gRPC JPEG: {count} 帧 / {elapsed:.2f}s = {count / elapsed:.1f} FPS")
            client.stop_stream(sid)
        else:
            print("gRPC 模式启动失败")

    time.sleep(2)

    # SHM
    with RTSPClient(SERVER) as client:
        sid = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_NVCUVID, use_shared_mem=True)
        if sid and wait_for_connection(client, sid):
            start = time.time()
            count = 0
            while count < frames:
                ts, frame = client.read(sid, blocking=True, timeout_ms=1000)
                if ts != -1 and frame is not None:
                    count += 1
            elapsed = time.time() - start
            print(f"SHM      : {count} 帧 / {elapsed:.2f}s = {count / elapsed:.1f} FPS")
            client.stop_stream(sid)
        else:
            print("SHM 模式启动失败")


# ==================== 示例 8: 批量开关流 ====================

def example_batch_operations(urls: List[str]):
    """批量启动多路流，批量查询状态，再批量停止"""
    print("\n" + "=" * 60)
    print(f"示例 8: 批量操作 ({len(urls)} 路流)")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        stream_ids = []
        for url in urls:
            sid = client.start_stream(url, decoder_type=DECODER_CPU_FFMPEG, use_shared_mem=False)
            if sid:
                stream_ids.append(sid)
                print(f"  启动: {sid[:8]}... -> {url}")
            else:
                print(f"  启动失败: {url}")

        print(f"\n成功启动 {len(stream_ids)} 路流，等待连接...")
        time.sleep(3)

        print("\n当前流状态:")
        for sid in stream_ids:
            status = client.get_stream_status(sid)
            print(f"  {sid[:8]}... : {STATUS_NAMES.get(status, '未知')}")

        print("\n批量停止...")
        for sid in stream_ids:
            client.stop_stream(sid)
        print("批量停止完成")


# ==================== 示例 9: 动态切换 RTSP URL ====================

def example_update_url(new_url: Optional[str] = None):
    """启动一路流后，动态修改其 RTSP URL"""
    print("\n" + "=" * 60)
    print("示例 9: 动态切换 RTSP URL")
    print("=" * 60)

    if new_url is None:
        new_url = RTSP_URL.replace("Channels/101", "Channels/201")

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_CPU_FFMPEG)
        if not stream_id:
            print("启动流失败")
            return

        print(f"原始流: {stream_id}")
        if wait_for_connection(client, stream_id):
            ts, _ = client.read(stream_id)
            print(f"  切换前读取 ts={ts}")

        if client.update_stream_url(stream_id, new_url):
            print(f"  已切换 URL 为: {new_url}")
            # 等待重新连接
            time.sleep(2)
            if wait_for_connection(client, stream_id):
                ts, _ = client.read(stream_id)
                print(f"  切换后读取 ts={ts}")
        else:
            print("  切换 URL 失败")

        client.stop_stream(stream_id)


# ==================== 示例 10: 断线检测与重连 ====================

def example_reconnect(max_retry: int = 3):
    """演示连接断开后使用新 RTSPClient 重新连接"""
    print("\n" + "=" * 60)
    print("示例 10: 断线检测与重连")
    print("=" * 60)

    stream_id = None
    for attempt in range(1, max_retry + 1):
        print(f"\n第 {attempt} 次尝试连接...")
        with RTSPClient(SERVER) as client:
            stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_CPU_FFMPEG)
            if not stream_id:
                print("  启动失败，重试...")
                time.sleep(1)
                continue

            if not wait_for_connection(client, stream_id, timeout_sec=10):
                print("  连接超时，重试...")
                client.stop_stream(stream_id)
                time.sleep(1)
                continue

            print(f"  连接成功: {stream_id}")
            ts, frame = client.read(stream_id)
            if ts != -1 and frame is not None:
                print(f"  读取成功: ts={ts}, shape={frame.shape}")
                client.stop_stream(stream_id)
                return
            else:
                print("  读取失败，重试...")
                client.stop_stream(stream_id)
                time.sleep(1)

    print(f"\n重试 {max_retry} 次后仍未成功")


# ==================== 示例 11: 持续保存图片到指定文件夹 ====================

def example_save_images(output_dir: Optional[str] = None,
                        use_shared_mem: bool = False,
                        decoder_type: int = DECODER_CPU_FFMPEG):
    """
    持续拉流并把每一帧保存为图片，直到流断开或按 Ctrl+C 停止。

    :param output_dir: 图片保存目录，默认 ./images/<stream_id>
    :param use_shared_mem: 是否使用共享内存模式
    :param decoder_type: 解码器类型
    """
    print("\n" + "=" * 60)
    print("示例 11: 持续保存图片到指定文件夹（按 Ctrl+C 停止）")
    print("=" * 60)

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            RTSP_URL,
            decoder_type=decoder_type,
            gpu_id=0,
            only_key_frames=False,
            use_shared_mem=use_shared_mem
        )
        if not stream_id:
            print("启动流失败")
            return

        print(f"流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return
        print("流已连接，开始保存图片...")

        base_output_dir = output_dir
        current_id = stream_id

        def ensure_output_dir(active_id: str) -> str:
            """根据当前有效 stream_id 创建/切换保存目录"""
            if base_output_dir is not None:
                # 用户指定了固定目录，仍在该目录下按 stream_id 分子目录
                d = os.path.join(base_output_dir, active_id)
            else:
                d = os.path.join(OUTPUT_DIR, active_id)
            os.makedirs(d, exist_ok=True)
            return d

        output_dir = ensure_output_dir(current_id)
        print(f"图片保存目录: {output_dir}")

        saved_count = 0
        empty_count = 0
        start = time.time()
        last_report = start
        connecting_log_count = 0

        try:
            while True:
                ts, frame = client.read(stream_id, blocking=use_shared_mem, timeout_ms=1000)

                # 检测流是否已自动重启，必要时切换保存目录并等待连接
                active_id = client.get_active_stream_id(stream_id)
                if active_id and active_id != current_id:
                    current_id = active_id
                    output_dir = ensure_output_dir(current_id)
                    empty_count = 0
                    print(f"  流已重启，切换到新目录: {output_dir}")
                    print(f"  等待新流连接...")
                    if not wait_for_connection(client, stream_id, timeout_sec=30):
                        print("  新流连接失败，停止保存")
                        break
                    print("  新流已连接，继续保存")

                if ts != -1 and frame is not None:
                    connecting_log_count = 0
                    empty_count = 0
                    fname = datetime.datetime.fromtimestamp(ts / 1000).strftime("%Y%m%d_%H%M%S_%f")[:-3]
                    path = os.path.join(output_dir, f"{fname}_{saved_count:08d}.jpg")
                    cv2.imwrite(path, frame)
                    saved_count += 1

                    now = time.time()
                    if now - last_report >= 1.0:
                        print(f"  已保存 {saved_count} 张, 平均 {saved_count / (now - start):.1f} 张/秒")
                        last_report = now
                else:
                    empty_count += 1
                    status = client.get_stream_status(stream_id)
                    if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                        print(f"\n流已断开: {STATUS_NAMES.get(status)}")
                        break
                    if status == STATUS_CONNECTING:
                        connecting_log_count += 1
                        if connecting_log_count % 10 == 0:
                            print(f"  流正在连接中，已等待 {connecting_log_count * 0.2:.1f}s...")
                        time.sleep(0.2)
                        continue
                    # 流已连接但暂无帧，避免 busy loop
                    if empty_count % 50 == 0:
                        print(f"  流已连接但连续 {empty_count} 次未读到帧，继续轮询... (ts={ts})")
                    time.sleep(0.05)
        except KeyboardInterrupt:
            print("\n收到 Ctrl+C，停止保存")

        elapsed = time.time() - start
        print(f"\n共保存 {saved_count} 张图片到 {output_dir}, "
              f"耗时 {elapsed:.2f}s, 平均 {saved_count / elapsed:.1f} 张/秒")
        client.stop_stream(stream_id)


def example_hik_sdk():
    """
    示例 12: 通过海康 SDK 直接抓图。
    统一使用 hik:// URL 传参：
      环境变量 HIK_URL，例如 hik://admin:lww123456@172.16.22.16:8000/channel/101
    """
    print("\n" + "=" * 60)
    print("示例 12: 海康 SDK 直接抓图")
    print("=" * 60)

    hik_url = os.environ.get("HIK_URL", "hik://admin:lww123456@172.16.22.16:8000/channel/33")
    print(f"使用 HIK_URL: {hik_url}")

    with RTSPClient(SERVER) as client:
        stream_id = client.start_stream(
            rtsp_url=hik_url,
            decoder_type=DECODER_HIK_SDK,
        )
        if not stream_id:
            print("启动海康流失败")
            return

        print(f"海康流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return


        print("已连接，开始读取 10 帧...")
        for i in range(10000000):
            ts, frame = client.read(stream_id)
            if frame is not None:
                print(f"  帧 {i + 1}: ts={ts}, shape={frame.shape}")
                # cv2.imwrite(f"images/hik_frame_{i + 1}.jpg", frame)
            else:
                print(f"  帧 {i + 1}: 无数据")
            time.sleep(0.5)

        client.stop_stream(stream_id)
        print("海康流已停止")


def example_switch_rtsp_hik():
    """
    示例 13: RTSP 与海康 SDK 之间动态切换。

    演示启动 RTSP 流时指定 GPU 解码，切换到海康 URL 后，
    再切回 RTSP 时自动恢复之前保存的 GPU 配置。
    """
    print("\n" + "=" * 60)
    print("示例 13: RTSP <-> 海康 SDK 动态切换")
    print("=" * 60)

    rtsp_url = os.getenv(
        "RTSP_URL",
        "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"
    )
    hik_url = os.getenv(
        "HIK_URL",
        "hik://admin:lww123456@172.16.22.16:8000/channel/35"
    )

    with RTSPClient(SERVER) as client:
        # 1. 启动 RTSP，指定 GPU 解码
        stream_id = client.start_stream(
            rtsp_url,
            decoder_type=DECODER_GPU_NVCUVID,
            gpu_id=0,
            use_shared_mem=False,
        )
        if not stream_id:
            print("启动 RTSP 流失败")
            return
        print(f"RTSP 流已启动: {stream_id}")
        if not wait_for_connection(client, stream_id):
            client.stop_stream(stream_id)
            return
        print("RTSP 已连接")
        # 2. 切换到海康 URL
        print(f"\n切换到海康 URL: {hik_url}")
        if client.update_stream_url(stream_id, hik_url):
            if wait_for_connection(client, stream_id, timeout_sec=20):
                # 海康抓图比 RTSP 慢，先等待第一帧真正到达
                print("等待海康第一帧...")
                first_ts, first_frame = -1, None
                for _ in range(50):
                    first_ts, first_frame = client.read(stream_id)
                    if first_frame is not None:
                        break
                    time.sleep(0.1)
                if first_frame is None:
                    print("海康连接成功但 5 秒内未读到任何帧")
                else:
                    print(f"海康第一帧到达: ts={first_ts}, shape={first_frame.shape}")

                saved_count = 0
                for i in range(5):
                    ts, frame = client.read(stream_id)
                    if frame is not None:
                        saved_count += 1
                        timestamp = datetime.datetime.fromtimestamp(ts / 1000).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                        print(datetime.datetime.fromtimestamp(ts / 1000).strftime("%Y-%m-%d %H:%M:%S.%f"))
                        cv2.imwrite(f"images/hik_frame_{i:03d}_{timestamp}.jpg", frame)
                    else:
                        print(f"  海康第 {i + 1}/5 帧无数据")
                    # 海康抓图需要 ForceIFrame + sleep + Capture，间隔比一般 RTSP 大
                    time.sleep(0.3)
                print(f"海康已连接，实际保存 {saved_count}/5 帧，最后一帧 ts={ts}, "
                      f"shape={frame.shape if frame is not None else None}")
            else:
                print("海康连接超时")
        else:
            print("切换到海康 URL 失败")
        # 3. 切回 RTSP，应自动使用之前保存的 GPU_NVCUVID / gpu_id=0
        print(f"\n切回 RTSP URL: {rtsp_url}")
        if client.update_stream_url(stream_id, rtsp_url):
            if wait_for_connection(client, stream_id, timeout_sec=20):
                saved_count = 0
                for i in range(5):
                    ts, frame = client.read(stream_id)
                    if frame is not None:
                        saved_count += 1
                        timestamp = datetime.datetime.fromtimestamp(ts / 1000).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                        cv2.imwrite(f"images/rtsp_frame_{i:03d}_{timestamp}.jpg", frame)
                    else:
                        print(f"  RTSP 第 {i + 1}/5 帧无数据")
                    time.sleep(0.1)
                print(f"RTSP 已恢复，实际保存 {saved_count}/5 帧，最后一帧 ts={ts}, "
                      f"shape={frame.shape if frame is not None else None}")
            else:
                print("RTSP 恢复连接超时")
        else:
            print("切回 RTSP URL 失败")

        client.stop_stream(stream_id)
        print("流已停止")


# ==================== 主入口 ====================

def print_usage():
    print("""
用法: python example.py [示例编号]

可用示例:
  1  - 列出服务端所有流
  2  - 主动轮询读帧 (gRPC JPEG)
  3  - 服务端流式推送 (gRPC streaming)
  4  - 共享内存读帧 (SHM)
  5  - RTSPClient 单流 SHM 模式 (类 VideoCapture API)
  6  - 多线程并发多路流
  7  - gRPC JPEG vs SHM 性能对比
  8  - 批量开关流
  9  - 动态切换 RTSP URL
  10 - 断线检测与重连
  11 - 持续保存图片到指定文件夹（按 Ctrl+C 停止）
  12 - 海康 SDK 直接抓图
  13 - RTSP <-> 海康 SDK 动态切换
  all - 顺序运行所有示例（部分示例耗时较长）

环境变量:
  GRPC_SERVER - gRPC 服务端地址，默认 127.0.0.1:50051
  RTSP_URL    - 测试用 RTSP 地址
  HIK_URL     - 海康 URL，例如 hik://admin:pass@172.16.22.16:8000/channel/101
  HIK_IP      - 海康设备 IP
  HIK_PORT    - 海康设备端口，默认 8000
  HIK_USER    - 海康用户名
  HIK_PASSWORD - 海康密码
  HIK_CHANNEL - 海康通道号，默认 1
""")


EXAMPLES = {
    "1": example_list_streams,
    "2": example_poll_frame,
    "3": example_stream_frames,
    "4": example_shared_memory,
    "5": example_shm_capture_style,
    "6": example_multi_thread,
    "7": example_benchmark,
    "8": example_batch_operations,
    "9": example_update_url,
    "10": example_reconnect,
    "11": example_save_images,
    "12": example_hik_sdk,
    "13": example_switch_rtsp_hik,
}


def main():
    # 兼容旧 client.py 直接运行：默认执行示例 3（流式推送）
    arg = sys.argv[1] if len(sys.argv) > 1 else "3"

    if arg in ("-h", "--help", "help"):
        print_usage()
        return

    if arg == "all":
        urls = [RTSP_URL.replace("Channels/101", f"Channels/{(i + 1) * 100 + 1}") for i in range(4)]
        example_list_streams()
        example_poll_frame()
        example_stream_frames(save_images=False, max_frames=50)
        example_shared_memory(duration_sec=5)
        example_shm_capture_style(duration_sec=5)
        example_multi_thread(channels=2, duration_sec=5)
        example_benchmark(frames=100)
        example_batch_operations(urls[:2])
        example_update_url()
        example_reconnect()
        # example_save_images 不放入 all，避免无限制运行
        return

    func = EXAMPLES.get(arg)
    if func is None:
        print(f"未知示例编号: {arg}")
        print_usage()
        return

    func()


if __name__ == "__main__":
    main()
