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

SERVER = os.getenv("GRPC_SERVER", "127.0.0.1:50052")
RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901"

def align_up(value, alignment):
    """模拟 C++ alignas 对齐"""
    return (value + alignment - 1) & ~(alignment - 1)

# OpenCV depth 枚举 -> numpy dtype 映射 [[11]][[13]]
CV_DEPTH_TO_NUMPY = {
    0: np.uint8,   # CV_8U
    1: np.int8,    # CV_8S
    2: np.uint16,  # CV_16U
    3: np.int16,   # CV_16S
    4: np.int32,   # CV_32S
    5: np.float32, # CV_32F
    6: np.float64, # CV_64F
}

class ShmStreamReader:
    def __init__(self, stream_id):
        self.shm_path = f"/dev/shm/{stream_id}"  # 注意：C++ 端使用 /stream_id，但 Linux shm 通常挂载在 /dev/shm
        
        # --- 1. 基础参数 ---
        self.ALIGNMENT = 64
        self.UINT64_SIZE = 8
        self.UINT32_SIZE = 4
        self.MAX_SHM_FRAME_SIZE = 3 * 1024 * 1024
        self.SLOT_COUNT = 8
        
        # --- 2. 计算 ShmMeta 布局 (新增 4 个 uint32 字段) ---
        # 原始 4 个 uint64 + 新增 4 个 uint32 = 32 + 16 = 48 bytes
        self.META_DATA_SIZE = 4 * self.UINT64_SIZE + 4 * self.UINT32_SIZE  # 48 bytes
        self.META_STRUCT_SIZE = align_up(self.META_DATA_SIZE, self.ALIGNMENT)  # 64 bytes (对齐后)
        
        # --- 3. 计算 ShmFrameSlot 偏移量 ---
        self.SEQ_OFFSET = 0
        self.META_OFFSET = align_up(self.SEQ_OFFSET + self.UINT64_SIZE, self.ALIGNMENT)  # 64
        self.PAYLOAD_OFFSET = self.META_OFFSET + self.META_STRUCT_SIZE  # 128
        
        raw_slot_size = self.PAYLOAD_OFFSET + self.MAX_SHM_FRAME_SIZE
        self.SLOT_SIZE = align_up(raw_slot_size, self.ALIGNMENT)  # 3145856
        
        # --- 状态 ---
        self.last_read_idx = -1
        self.shm = None
        self._mmap_obj = None  # 保持引用防止被回收

    def connect(self):
        """连接共享内存"""
        # 尝试两个常见路径：C++ shm_open 使用 /name，但某些系统映射到 /dev/shm
        paths_to_try = [self.shm_path, f"/{self.shm_path.lstrip('/')}"]
        
        for path in paths_to_try:
            if os.path.exists(path):
                try:
                    fd = os.open(path, os.O_RDONLY)
                    total_size = self.SLOT_COUNT * self.SLOT_SIZE + align_up(self.UINT64_SIZE, self.ALIGNMENT)
                    self._mmap_obj = mmap.mmap(fd, total_size, prot=mmap.PROT_READ)
                    self.shm = memoryview(self._mmap_obj)  # 使用 memoryview 提高切片效率
                    os.close(fd)
                    print(f"✓ 已连接共享内存: {path}")
                    return True
                except Exception as e:
                    print(f"✗ 连接失败 {path}: {e}")
        return False

    def _read_uint64(self, offset):
        """安全读取 uint64"""
        if offset + self.UINT64_SIZE > len(self.shm):
            return None
        return struct.unpack("Q", self.shm[offset:offset + self.UINT64_SIZE])[0]

    def _read_meta(self, offset):
        """读取并解析元数据"""
        meta_start = offset + self.META_OFFSET
        meta_end = meta_start + self.META_DATA_SIZE
        if meta_end > len(self.shm):
            return None
            
        meta_raw = self.shm[meta_start:meta_end]
        # 解析: 4x uint64 + 4x uint32
        fields = struct.unpack("QQQQIIII", meta_raw)
        return {
            'actual_size': fields[0],
            'width': fields[1],
            'height': fields[2],
            'timestamp': fields[3],
            'channels': fields[4],
            'depth': fields[5],
            'step': fields[6],
            'reserved': fields[7],
        }

    def _rebuild_image(self, raw_data, meta):
        """根据元数据重建 OpenCV 图像"""
        w, h, c = meta['width'], meta['height'], meta['channels']
        depth, step = meta['depth'], meta['step']
        
        # 1. 获取 numpy dtype
        dtype = CV_DEPTH_TO_NUMPY.get(depth, np.uint8)
        elem_size = np.dtype(dtype).itemsize
        
        # 2. 转换为 numpy 数组
        img_array = np.frombuffer(raw_data, dtype=dtype)
        
        # 3. 处理步长 (step = 每行实际字节数，可能含 padding)
        expected_step = w * c * elem_size
        if step > 0 and step != expected_step:
            # 有 padding：按行读取后裁剪
            row_elements = step // elem_size
            if img_array.size < h * row_elements:
                return None
            img = img_array[:h * row_elements].reshape((h, row_elements))
            img = img[:, :w * c]  # 裁剪掉 padding 列
        else:
            # 连续内存：直接 reshape
            if img_array.size < h * w * c:
                return None
            img = img_array[:h * w * c]
        
        # 4. 重塑为 (H, W, C) 或 (H, W)
        if c == 1:
            img = img.reshape((h, w))
        else:
            img = img.reshape((h, w, c))
        
        # 5. 确保返回的是可写数组（cv2.imshow 等需要）
        if not img.flags.writeable:
            img = img.copy()
            
        return img

    def get_frame(self):
        """获取一帧图像，返回 (cv2.Mat, timestamp) 或 (None, 0)"""
        if not self.shm:
            return None, 0
        
        # 1. 定位 head_idx (紧跟在 slots 之后，对齐到 64)
        slots_end = self.SLOT_COUNT * self.SLOT_SIZE
        head_idx_offset = align_up(slots_end, self.ALIGNMENT)
        
        try:
            latest_idx = self._read_uint64(head_idx_offset)
            if latest_idx is None or latest_idx == self.last_read_idx:
                return None, 0
            
            # 2. 定位到目标 slot
            slot_idx = latest_idx % self.SLOT_COUNT
            slot_offset = slot_idx * self.SLOT_SIZE
            
            # 3. 读 sequence v1 (奇数=写入中)
            v1 = self._read_uint64(slot_offset + self.SEQ_OFFSET)
            if v1 is None or v1 % 2 != 0:
                return None, 0
            
            # 4. 读取元数据
            meta = self._read_meta(slot_offset)
            if not meta or meta['actual_size'] == 0 or meta['actual_size'] > self.MAX_SHM_FRAME_SIZE:
                return None, 0
            
            # 5. 读 sequence v2 (double-check)
            v2 = self._read_uint64(slot_offset + self.SEQ_OFFSET)
            if v1 != v2:
                return None, 0  # 写入过程中被修改，跳过
            
            # 6. 读取原始像素数据
            payload_start = slot_offset + self.PAYLOAD_OFFSET
            payload_end = payload_start + meta['actual_size']
            if payload_end > len(self.shm):
                return None, 0
                
            raw_data = self.shm[payload_start:payload_end]
            
            # 7. 重建图像
            img = self._rebuild_image(raw_data, meta)
            if img is None:
                return None, 0
            
            # 8. 更新状态并返回 (✅ 已是 BGR，无需转换!)
            self.last_read_idx = latest_idx
            return img, meta['timestamp']
            
        except Exception as e:
            # print(f"[DEBUG] get_frame error: {e}")
            return None, 0

    def close(self):
        """清理资源"""
        if self._mmap_obj:
            self._mmap_obj.close()
            self._mmap_obj = None
        self.shm = None

def main():
    stream_id = None
    
    # 1. 启动远程流 (启用共享内存)
    with RemoteCapture(SERVER) as client:
        stream_id = client.start_stream(
            RTSP_URL, 
            decoder_type=DECODER_GPU_NVCUVID, 
            gpu_id=0, 
            use_shared_mem=True  # ✅ 关键：启用共享内存模式
        )
        if not stream_id:
            print("✗ 启动流失败")
            return
        
        print(f"✓ 流已启动: {stream_id[:8]}...")
        
        # 2. 等待连接
        status = STATUS_CONNECTING
        while status == STATUS_CONNECTING:
            status = client.get_stream_status(stream_id)
            if status not in (STATUS_CONNECTING, STATUS_NOT_FOUND):
                break
            print(f"⏳ 等待连接... (status={STATUS_NAMES.get(status, status)})")
            time.sleep(0.5)
        
        if status != STATUS_CONNECTED:
            print(f"✗ 连接失败: {STATUS_NAMES.get(status, status)}")
            return
        print("✓ 连接成功")
    
    # 3. 连接共享内存读取器
    reader = ShmStreamReader(stream_id)
    if not reader.connect():
        print("✗ 无法连接共享内存，请确认 C++ 端已启动且使用相同 stream_id")
        return

    print("🚀 开始读取共享内存帧...")
    
    # 4. 等待第一帧
    frame = None
    while frame is None:
        frame, ts = reader.get_frame()
        if frame is not None:
            print(f"✓ 首帧: shape={frame.shape}, dtype={frame.dtype}, ts={ts}")
        else:
            print("⏳ 等待首帧...")
            time.sleep(0.1)
    
    # 5. 主循环 + FPS 统计
    try:
        frame_count = 0
        start_time = time.time()
        last_print_time = start_time
        
        while True:
            frame, ts = reader.get_frame()
            if frame is not None:
                frame_count += 1
                now = time.time()
                elapsed = now - start_time
                
                # 每秒打印一次统计
                if now - last_print_time >= 1.0:
                    fps = frame_count / (now - last_print_time)
                    print(f"📊 FPS: {fps:.2f} | 分辨率: {frame.shape[1]}x{frame.shape[0]}x{frame.shape[2] if frame.ndim==3 else 1} | dtype: {frame.dtype}")
                    last_print_time = now
                    frame_count = 0  # 重置计数
                
                # ✅ 直接显示/保存 (已是 BGR 格式!)
                # cv2.imshow("SHM Stream", frame)  # 如需显示请取消注释
                # if cv2.waitKey(1) & 0xFF == ord('q'):
                #     break
                    
                # 保存示例帧 (每 30 帧)
                if frame_count % 30 == 0:
                    save_path = f"frames/frame_{ts}.jpg"
                    os.makedirs(os.path.dirname(save_path), exist_ok=True)
                    cv2.imwrite(save_path, frame)
                    
            else:
                # 无新帧时短暂休眠，避免忙等
                time.sleep(0.001)
                
    except KeyboardInterrupt:
        print("\n⏹ 用户中断")
    finally:
        reader.close()
        cv2.destroyAllWindows()
        print("✓ 资源已清理")

if __name__ == "__main__":
    main()