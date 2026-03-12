import cv2
import time
import mmap
import struct
import numpy as np
import os
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

SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50051")
RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901"

import cv2
import mmap
import struct
import numpy as np
import os

# 模拟 C/C++ 编译器的内存对齐机制
def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)

class ShmStreamReader:
    def __init__(self, stream_id):
        self.shm_path = f"/dev/shm/{stream_id}"
        
        # --- 1. 定义底层数据类型大小 (Bytes) ---
        self.ALIGNMENT = 64
        self.UINT64_SIZE = 8
        self.MAX_SHM_FRAME_SIZE = 3 * 1024 * 1024
        self.SLOT_COUNT = 8
        
        # --- 2. 动态计算 ShmMeta 结构体布局 ---
        # ShmMeta 包含 4 个 uint64_t (actual_size, width, height, timestamp)
        self.META_DATA_SIZE = 4 * self.UINT64_SIZE  # 32 bytes
        # 因为 C++ 中写了 struct alignas(64) ShmMeta，所以总大小会补齐到 64 的倍数
        self.META_STRUCT_SIZE = align_up(self.META_DATA_SIZE, self.ALIGNMENT) # 64 bytes
        
        # --- 3. 动态计算 ShmFrameSlot 结构体偏移量 ---
        self.SEQ_OFFSET = 0
        
        # Meta 的起点：紧跟 sequence 之后，但必须对齐到 64 字节边界
        self.META_OFFSET = align_up(self.SEQ_OFFSET + self.UINT64_SIZE, self.ALIGNMENT) # 64
        
        # Payload 的起点：紧跟 Meta 结构体之后
        self.PAYLOAD_OFFSET = self.META_OFFSET + self.META_STRUCT_SIZE # 64 + 64 = 128
        
        # 整个 Slot 的总大小：包含 Payload 后，再根据 alignas(64) 对齐一次
        raw_slot_size = self.PAYLOAD_OFFSET + self.MAX_SHM_FRAME_SIZE
        self.SLOT_SIZE = align_up(raw_slot_size, self.ALIGNMENT) # 3145856
        
        # --- 状态变量 ---
        self.last_read_idx = -1
        self.shm = None

    def connect(self):
        if not os.path.exists(self.shm_path): return False
        fd = os.open(self.shm_path, os.O_RDONLY)
        self.shm = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
        return True

    def get_frame(self):
        if not self.shm: return None, 0
        
        # 1. 计算 head_idx 的位置 (紧跟在所有 Slot 之后，且对齐到 64)
        head_idx_offset = self.SLOT_COUNT * self.SLOT_SIZE
        head_idx_offset = align_up(head_idx_offset, self.ALIGNMENT)
        
        try:
            # 读取最新的索引
            latest_idx = struct.unpack("Q", self.shm[head_idx_offset : head_idx_offset + self.UINT64_SIZE])[0]
            if latest_idx == self.last_read_idx: 
                return None, 0
            
            # 定位到当前的 Slot
            slot_idx = latest_idx % self.SLOT_COUNT
            offset = slot_idx * self.SLOT_SIZE
            
            # 2. 读 v1 (校验写状态)
            v1_raw = self.shm[offset + self.SEQ_OFFSET : offset + self.SEQ_OFFSET + self.UINT64_SIZE]
            v1 = struct.unpack("Q", v1_raw)[0]
            if v1 % 2 != 0: 
                return None, 0  # 正在写入中，跳过
            
            # 3. 读 Meta 数据
            # 注意：只需读取有效的 META_DATA_SIZE (32字节)，不需要把填充的空白也读出来
            meta_start = offset + self.META_OFFSET
            meta_raw = self.shm[meta_start : meta_start + self.META_DATA_SIZE]
            size, w, h, ts = struct.unpack("QQQQ", meta_raw)
            
            # 安全校验
            if size == 0 or size > self.MAX_SHM_FRAME_SIZE:
                return None, 0
                
            # 4. 读 v2 (Double-check 防止并发写覆盖)
            v2_raw = self.shm[offset + self.SEQ_OFFSET : offset + self.SEQ_OFFSET + self.UINT64_SIZE]
            v2 = struct.unpack("Q", v2_raw)[0]
            if v1 != v2: 
                return None, 0
            
            # 5. 读取 Payload 数据
            data_start = offset + self.PAYLOAD_OFFSET
            raw_data = self.shm[data_start : data_start + size]
            
            # 转换成 numpy 数组
            img_array = np.frombuffer(raw_data, dtype=np.uint8)
            if img_array.size == 0:
                return None, 0
                
            # 6. 解码图像
            img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
            
            if img is not None:
                self.last_read_idx = latest_idx
                img = cv2.cvtColor(img, cv2.COLOR_RGB2BGR) 
                return img, ts
            else:
                return None, 0
                
        except Exception as e:
            # 记录错误方便调试：print(f"Decode error: {e}")
            return None, 0

def main():
    stream_id = None
    with RemoteCapture(SERVER) as client:
        # 启动流
        stream_id = client.start_stream(RTSP_URL, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0, use_shared_mem=True)
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
    reader = ShmStreamReader(stream_id)
    
    if not reader.connect():
        print("无法连接到共享内存，请检查 C++ 端是否已启动")
        return

    print("开始读取共享内存帧...")
    # 增加计算fps的逻辑

    frame = None
    while frame is None:
        frame, ts = reader.get_frame()
        if frame is not None:
            print(f"获取到第一帧: {frame.shape}, ts={ts}")
        else:
            print("等待第一帧...")
            time.sleep(0.5)
    try:
        frame_count = 0
        start_time = time.time()
        while True:
            frame, ts = reader.get_frame()
            # print(frame)
            if frame is not None:
                frame_count += 1
                elapsed = time.time() - start_time
                fps = frame_count / elapsed if elapsed > 0 else 0
                print(f"获取到帧: {frame.shape}, ts={ts}, fps={fps:.2f}")
                # 转换 RGB 到 BGR 以供 OpenCV 显示
                bgr_frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                cv2.imwrite(f"images/frame_{ts}.jpg", bgr_frame)
            else:
                time.sleep(0.001)
    finally:
        pass

if __name__ == "__main__":
    main()
