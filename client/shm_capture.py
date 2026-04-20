"""
ShmCapture - 精简版共享内存视频捕获客户端

特点:
  ✅ 仅保留共享内存必需功能，移除 JPEG/网络帧传输代码
  ✅ gRPC 仅用于流管理（start/stop/status），帧数据走共享内存
  ✅ OpenCV 风格 API: open(), read(), grab(), retrieve(), isOpened()
  ✅ 自动资源清理 + 断线检测
  ✅ 零依赖：仅需 grpcio + opencv-python + numpy

使用示例:
    with ShmCapture('127.0.0.1:50052') as cap:
        if not cap.open(RTSP_URL):
            return
        while cap.isOpened():
            ret, frame = cap.read()
            if not ret:
                break
            cv2.imshow("SHM Stream", frame)
            if cv2.waitKey(1) == ord('q'):
                break
"""

import cv2
import time
import mmap
import struct
import numpy as np
import os
import logging
from typing import Optional, Tuple, List, Dict

# gRPC 导入
import grpc
import stream_service_pb2
import stream_service_pb2_grpc

# 日志配置
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)


# ==================== 常量定义 ====================

# 解码器类型（与 proto 保持一致）
DECODER_CPU_FFMPEG = stream_service_pb2.DECODER_CPU_FFMPEG
DECODER_GPU_NVCUVID = stream_service_pb2.DECODER_GPU_NVCUVID

DECODER_NAMES = {
    DECODER_CPU_FFMPEG: "CPU (FFmpeg)",
    DECODER_GPU_NVCUVID: "GPU (NVCUVID)"
}

# 流状态
STATUS_CONNECTING = stream_service_pb2.STATUS_CONNECTING
STATUS_CONNECTED = stream_service_pb2.STATUS_CONNECTED
STATUS_DISCONNECTED = stream_service_pb2.STATUS_DISCONNECTED
STATUS_NOT_FOUND = stream_service_pb2.STATUS_NOT_FOUND

STATUS_NAMES = {
    STATUS_CONNECTING: "连接中",
    STATUS_CONNECTED: "已连接", 
    STATUS_DISCONNECTED: "断开",
    STATUS_NOT_FOUND: "不存在"
}


# ==================== 工具函数 ====================

def align_up(value: int, alignment: int) -> int:
    """模拟 C++ alignas 内存对齐"""
    return (value + alignment - 1) & ~(alignment - 1)


# OpenCV depth 枚举 -> numpy dtype 映射
CV_DEPTH_TO_NUMPY = {
    0: np.uint8,   # CV_8U
    1: np.int8,    # CV_8S  
    2: np.uint16,  # CV_16U
    3: np.int16,   # CV_16S
    4: np.int32,   # CV_32S
    5: np.float32, # CV_32F
    6: np.float64, # CV_64F
}


# ==================== 共享内存读取器（内部类） ====================

class _ShmReader:
    """共享内存帧读取器（内部使用，不暴露给用户）"""
    
    def __init__(self, stream_id: str, max_frame_size_mb: int = 3):
        self.stream_id = stream_id
        # C++ shm_open 使用 "/stream_id"，Linux 通常映射到 /dev/shm/stream_id
        self.shm_paths = [f"/dev/shm/{stream_id}", f"/{stream_id.lstrip('/')}"]
        
        # --- 内存布局参数（必须与 C++ 端完全一致）---
        self.ALIGNMENT = 64
        self.UINT64_SIZE = 8
        self.UINT32_SIZE = 4
        self.MAX_FRAME_BYTES = 6  * 2560 * 1440
        self.SLOT_COUNT = 8
        
        # ShmMeta: 4×uint64 + 4×uint32 = 48 bytes → align to 64
        self.META_DATA_SIZE = 4 * self.UINT64_SIZE + 4 * self.UINT32_SIZE
        self.META_STRUCT_SIZE = align_up(self.META_DATA_SIZE, self.ALIGNMENT)  # 64
        
        # ShmFrameSlot 偏移计算
        self.SEQ_OFFSET = 0
        self.META_OFFSET = align_up(self.SEQ_OFFSET + self.UINT64_SIZE, self.ALIGNMENT)  # 64
        self.PAYLOAD_OFFSET = self.META_OFFSET + self.META_STRUCT_SIZE  # 128
        
        slot_raw_size = self.PAYLOAD_OFFSET + self.MAX_FRAME_BYTES
        self.SLOT_SIZE = align_up(slot_raw_size, self.ALIGNMENT)  # ~3.000125MB
        
        # 状态
        self._mmap_obj: Optional[mmap.mmap] = None
        self._shm_view: Optional[memoryview] = None
        self._last_idx = -1
        self._connected = False

    def connect(self) -> bool:
        """连接共享内存"""
        if self._connected:
            return True
        for path in self.shm_paths:
            if os.path.exists(path):
                try:
                    fd = os.open(path, os.O_RDONLY)
                    total_size = self.SLOT_COUNT * self.SLOT_SIZE + align_up(self.UINT64_SIZE, self.ALIGNMENT)
                    self._mmap_obj = mmap.mmap(fd, total_size, prot=mmap.PROT_READ)
                    self._shm_view = memoryview(self._mmap_obj)  # 高效切片
                    os.close(fd)
                    self._connected = True
                    logger.debug(f"✓ SHM connected: {path}")
                    return True
                except Exception as e:
                    logger.debug(f"✗ SHM connect failed {path}: {e}")
        return False

    def _read_u64(self, offset: int) -> Optional[int]:
        """安全读取 uint64"""
        end = offset + self.UINT64_SIZE
        if end > len(self._shm_view):
            return None
        return struct.unpack("Q", self._shm_view[offset:end])[0]

    def _read_meta(self, slot_offset: int) -> Optional[dict]:
        """读取并解析元数据"""
        start = slot_offset + self.META_OFFSET
        end = start + self.META_DATA_SIZE
        if end > len(self._shm_view):
            return None
        raw = self._shm_view[start:end]
        # 4×uint64 + 4×uint32
        f = struct.unpack("QQQQIIII", raw)
        return {
            'size': f[0], 'w': f[1], 'h': f[2], 'ts': f[3],
            'ch': f[4], 'depth': f[5], 'step': f[6], '_rsv': f[7]
        }

    def _rebuild_frame(self, raw_data, meta: dict) -> Optional[np.ndarray]:
        """根据元数据重建 numpy 数组（原始像素，无需解码）"""
        w, h, c = meta['w'], meta['h'], meta['ch']
        depth, step = meta['depth'], meta['step']
        dtype = CV_DEPTH_TO_NUMPY.get(depth, np.uint8)
        elem_sz = np.dtype(dtype).itemsize
        
        arr = np.frombuffer(raw_data, dtype=dtype)
        expected_step = w * c * elem_sz
        
        # 处理行对齐 padding
        if step > 0 and step != expected_step:
            row_elems = step // elem_sz
            if arr.size < h * row_elems:
                return None
            img = arr[:h * row_elems].reshape((h, row_elems))
            img = img[:, :w * c]  # 裁剪 padding
        else:
            if arr.size < h * w * c:
                return None
            img = arr[:h * w * c]
        
        # Reshape to (H,W) or (H,W,C)
        if c == 1:
            img = img.reshape((h, w))
        else:
            img = img.reshape((h, w, c))
        
        # 确保可写（cv2.imshow 等需要）
        if not img.flags.writeable:
            img = img.copy()
        return img

    def grab(self) -> bool:
        """预检查是否有新帧（不拷贝数据）"""
        if not self._shm_view:
            print("✗ grab failed: SHM not connected")
            return False
        try:
            # 读 head_idx
            head_off = align_up(self.SLOT_COUNT * self.SLOT_SIZE, self.ALIGNMENT)
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return False
            # 检查目标 slot
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):  # 奇数=写入中
                return False
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0:
                return False
            # Double-check sequence
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            return v1 == v2
        except Exception as e:
            print(f"✗ grab error: {e}")
            return False

    def retrieve(self) -> Tuple[Optional[np.ndarray], int]:
        """读取帧数据 + 时间戳"""
        if not self._shm_view:
            return None, 0
        try:
            head_off = align_up(self.SLOT_COUNT * self.SLOT_SIZE, self.ALIGNMENT)
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return None, 0
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            # Sequence check
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):
                return None, 0
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0 or meta['size'] > self.MAX_FRAME_BYTES:
                return None, 0
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 != v2:  # 写入中被修改
                return None, 0
            # 读取 payload
            pay_start = slot_off + self.PAYLOAD_OFFSET
            pay_end = pay_start + meta['size']
            if pay_end > len(self._shm_view):
                return None, 0
            raw = self._shm_view[pay_start:pay_end]
            img = self._rebuild_frame(raw, meta)
            if img is None:
                return None, 0
            self._last_idx = latest
            return img, meta['ts']
        except Exception as e:
            logger.debug(f"retrieve error: {e}")
            return None, 0

    def read(self) -> Tuple[bool, Optional[np.ndarray], int]:
        """grab + retrieve 组合"""
        if not self.grab():
            return False, None, 0
        img, ts = self.retrieve()
        return (img is not None), img, ts

    def close(self):
        """释放资源"""
        if self._mmap_obj:
            self._mmap_obj.close()
            self._mmap_obj = None
        self._shm_view = None
        self._connected = False

    def __del__(self):
        self.close()


# ==================== 主类：ShmCapture ====================

class ShmCapture:
    """
    共享内存视频捕获客户端
    
    类似 cv2.VideoCapture 的 API，但帧数据来自共享内存（零拷贝）
    gRPC 仅用于流生命周期管理
    """
    
    def __init__(self, server: str = '127.0.0.1:50052'):
        self._server = server
        self._channel: Optional[grpc.Channel] = None
        self._stub: Optional[stream_service_pb2_grpc.RTSPStreamServiceStub] = None
        self._shm: Optional[_ShmReader] = None
        self._stream_id: Optional[str] = None
        self._url: Optional[str] = None
        self._opened = False
        self._props: Dict = {}

    # --- 连接管理 ---
    
    def _ensure_connected(self) -> bool:
        """确保 gRPC 连接可用"""
        if self._stub is not None:
            return True
        try:
            opts = [
                ('grpc.max_receive_message_length', 10 * 1024 * 1024),
                ('grpc.keepalive_time_ms', 5000),
            ]
            self._channel = grpc.insecure_channel(self._server, options=opts)
            self._stub = stream_service_pb2_grpc.RTSPStreamServiceStub(self._channel)
            return True
        except Exception as e:
            logger.error(f"gRPC connect failed: {e}")
            return False

    # --- 核心 API ---
    
    def open(self, rtsp_url: str,
             decoder_type: int = DECODER_GPU_NVCUVID,
             gpu_id: int = 0,
             heartbeat_ms: int = 30000,
             interval_ms: int = 0,
             keep_on_fail: bool = True,
             max_frame_mb: int = 3) -> bool:
        """
        打开 RTSP 流并启用共享内存传输
        
        :param rtsp_url: RTSP 地址
        :param decoder_type: DECODER_GPU_NVCUVID 或 DECODER_CPU_FFMPEG
        :param gpu_id: GPU ID（CPU 解码时忽略）
        :param heartbeat_ms: 心跳超时（毫秒）
        :param interval_ms: 抽帧间隔，0=全帧
        :param keep_on_fail: 连接失败是否保留任务
        :param max_frame_mb: 单帧最大大小（必须与 C++ 端一致）
        :return: 是否成功
        """
        if self._opened:
            logger.warning("Already opened, call release() first")
            return False
        if not self._ensure_connected():
            return False
        try:
            # 1. 启动流（强制 use_shared_mem=True）
            req = stream_service_pb2.StartRequest(
                rtsp_url=rtsp_url,
                heartbeat_timeout_ms=heartbeat_ms,
                decode_interval_ms=interval_ms,
                decoder_type=decoder_type,
                gpu_id=gpu_id if decoder_type == DECODER_GPU_NVCUVID else -1,
                keep_on_failure=keep_on_fail,
                use_shared_mem=True,  # ✅ 关键
                only_key_frames=False
            )
            resp = self._stub.StartStream(req, timeout=10)
            if not resp.success:
                logger.error(f"Start failed: {resp.message}")
                return False
            self._stream_id = resp.stream_id
            
            # 2. 等待连接成功
            for _ in range(50):  # 10s 超时
                status = self.get_stream_status(self._stream_id)
                if status == STATUS_CONNECTED:
                    break
                if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                    logger.error(f"Connect failed: {STATUS_NAMES.get(status)}")
                    self.release()
                    return False
                time.sleep(0.2)
            else:
                logger.error("Connect timeout")
                self.release()
                return False
            
            # 3. 连接共享内存
            self._shm = _ShmReader(self._stream_id, max_frame_mb)
            if not self._shm.connect():
                logger.error("SHM connect failed")
                self.release()
                return False
            
            # 4. 初始化状态
            self._url = rtsp_url
            self._opened = True
            self._props = {
                'decoder': decoder_type,
                'gpu_id': gpu_id,
                'width': 0, 'height': 0,  # 动态更新
            }
            logger.info(f"✓ Opened: {rtsp_url[:60]}...")
            return True
        except Exception as e:
            logger.error(f"Open error: {e}")
            self.release()
            return False

    def release(self):
        """释放所有资源"""
        # 1. 停止流
        if self._stream_id and self._stub:
            try:
                req = stream_service_pb2.StopRequest(stream_id=self._stream_id)
                self._stub.StopStream(req, timeout=3)
            except:
                pass  # 忽略停止错误
        # 2. 关闭共享内存
        if self._shm:
            self._shm.close()
            self._shm = None
        # 3. 关闭 gRPC
        if self._channel:
            self._channel.close()
            self._channel = None
            self._stub = None
        # 4. 重置状态
        self._stream_id = None
        self._url = None
        self._opened = False
        logger.debug("✓ Released")

    def isOpened(self) -> bool:
        """检查是否已打开"""
        if not self._opened or not self._shm:
            return False
        # 额外检查流状态
        status = self.get_stream_status(self._stream_id)
        return status == STATUS_CONNECTED

    # --- 帧获取（核心功能）---
    
    def grab(self) -> bool:
        """预取帧（不拷贝数据，用于快速检查）"""
        return self._shm.grab() if self._shm else False

    def retrieve(self) -> Tuple[bool, Optional[np.ndarray]]:
        """获取帧数据（必须配合 grab 使用，或直接用 read）"""
        if not self._shm:
            return False, None
        img, _ = self._shm.retrieve()
        # 更新属性（分辨率）
        if img is not None:
            self._props['height'], self._props['width'] = img.shape[:2]
        return (img is not None), img

    def read(self) -> Tuple[bool, Optional[np.ndarray]]:
        """grab + retrieve 组合（推荐用法）"""
        if not self._shm:
            return False, None
        ok, img, _ = self._shm.read()
        if ok and img is not None:
            self._props['height'], self._props['width'] = img.shape[:2]
        return ok, img

    # --- 流管理（仅保留必需方法）---
    
    def get_stream_status(self, stream_id: Optional[str] = None) -> int:
        """获取流状态"""
        sid = stream_id or self._stream_id
        if not sid or not self._stub:
            return STATUS_NOT_FOUND
        try:
            req = stream_service_pb2.CheckRequest(stream_id=sid)
            resp = self._stub.CheckStream(req, timeout=3)
            return resp.stream.status if resp.stream else STATUS_NOT_FOUND
        except:
            return STATUS_NOT_FOUND

    def stop(self):
        """停止当前流"""
        if self._stream_id and self._stub:
            try:
                req = stream_service_pb2.StopRequest(stream_id=self._stream_id)
                self._stub.StopStream(req, timeout=3)
            except:
                pass
        self.release()

    def list_streams(self) -> List[Dict]:
        """列出所有流（调试用）"""
        if not self._stub:
            return []
        try:
            req = stream_service_pb2.ListStreamsRequest()
            resp = self._stub.ListStreams(req, timeout=3)
            return [{
                'id': s.stream_id,
                'url': s.rtsp_url,
                'status': STATUS_NAMES.get(s.status, '?'),
                'shm': s.use_shared_mem,
            } for s in resp.streams]
        except:
            return []
    
    def update_stream_url(self, stream_id: str, new_rtsp_url: str) -> bool:
        """
        更新流的 RTSP URL
        :param stream_id: 流 ID
        :param new_rtsp_url: 新的 RTSP URL
        :return: 是否更新成功
        """
        if not self.stub:
            logging.error("未连接到服务器")
            return False
        
        try:
            req = stream_service_pb2.UpdateStreamRequest(stream_id=stream_id, new_rtsp_url=new_rtsp_url)
            resp = self.stub.UpdateStream(req, timeout=5)
            
            if resp.success:
                logging.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
            else:
                logging.warning(f"更新流 URL 失败: {resp.message}")
            return resp.success
        except grpc.RpcError as e:
            logging.error(f"更新流 URL 异常: {e.details()}")
            return False


    # --- 上下文管理 ---
    
    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.release()

    def __del__(self):
        self.release()