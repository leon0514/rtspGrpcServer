"""
RTSP gRPC / SharedMemory 统一客户端
"""

import os
import sys
import time
import mmap
import struct
import ctypes
import ctypes.util
import logging
from typing import Optional, Tuple, List, Dict, Generator

import cv2
import grpc
import numpy as np

# 可选依赖：turbojpeg 仅在 gRPC JPEG 模式下使用
try:
    import turbojpeg
    _HAS_TURBOJPEG = True
except Exception:
    turbojpeg = None
    _HAS_TURBOJPEG = False

import stream_service_pb2
import stream_service_pb2_grpc

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

__all__ = [
    "RTSPClient",
    "DECODER_CPU_FFMPEG",
    "DECODER_GPU_NVCUVID",
    "DECODER_NAMES",
    "STATUS_CONNECTING",
    "STATUS_CONNECTED",
    "STATUS_DISCONNECTED",
    "STATUS_NOT_FOUND",
    "STATUS_NAMES",
]

# ==================== 常量（与 proto 保持一致）====================

DECODER_CPU_FFMPEG = stream_service_pb2.DECODER_CPU_FFMPEG
DECODER_GPU_NVCUVID = stream_service_pb2.DECODER_GPU_NVCUVID

DECODER_NAMES = {
    DECODER_CPU_FFMPEG: "CPU (FFmpeg)",
    DECODER_GPU_NVCUVID: "GPU (NVCUVID)"
}

STATUS_CONNECTING = stream_service_pb2.STATUS_CONNECTING
STATUS_CONNECTED = stream_service_pb2.STATUS_CONNECTED
STATUS_DISCONNECTED = stream_service_pb2.STATUS_DISCONNECTED
STATUS_NOT_FOUND = stream_service_pb2.STATUS_NOT_FOUND

STATUS_NAMES = {
    STATUS_CONNECTING: "连接中",
    STATUS_CONNECTED: "已连接",
    STATUS_DISCONNECTED: "无法连接",
    STATUS_NOT_FOUND: "不存在"
}

_DEFAULT_CHANNEL_OPTIONS = [
    ('grpc.max_receive_message_length', 10 * 1024 * 1024),
    ('grpc.keepalive_time_ms', 5000),
    ('grpc.keepalive_timeout_ms', 5000),
    ('grpc.keepalive_permit_without_calls', True),
]


# ==================== 共享内存辅助 ====================

def align_up(value: int, alignment: int) -> int:
    """模拟 C++ alignas 内存对齐"""
    return (value + alignment - 1) & ~(alignment - 1)


CV_DEPTH_TO_NUMPY = {
    0: np.uint8,
    1: np.int8,
    2: np.uint16,
    3: np.int16,
    4: np.int32,
    5: np.float32,
    6: np.float64,
}


_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class _timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


_libc.sem_open.restype = ctypes.c_void_p
_libc.sem_close.argtypes = [ctypes.c_void_p]
_libc.sem_close.restype = ctypes.c_int
_libc.sem_wait.argtypes = [ctypes.c_void_p]
_libc.sem_wait.restype = ctypes.c_int
_libc.sem_trywait.argtypes = [ctypes.c_void_p]
_libc.sem_trywait.restype = ctypes.c_int
_libc.sem_timedwait.argtypes = [ctypes.c_void_p, ctypes.POINTER(_timespec)]
_libc.sem_timedwait.restype = ctypes.c_int
_libc.sem_post.argtypes = [ctypes.c_void_p]
_libc.sem_post.restype = ctypes.c_int
_libc.clock_gettime.argtypes = [ctypes.c_int, ctypes.POINTER(_timespec)]
_libc.clock_gettime.restype = ctypes.c_int

CLOCK_REALTIME = 0
SEM_FAILED = ctypes.c_void_p(-1).value


class _NotifySemaphore:
    """跨进程 POSIX 有名信号量包装器"""

    def __init__(self, name: str):
        self._sem = _libc.sem_open(name.encode(), 0)
        if self._sem is None or self._sem == SEM_FAILED:
            errno = ctypes.get_errno()
            raise OSError(errno, f"sem_open failed for {name}: {os.strerror(errno)}")

    def wait(self, timeout_ms: Optional[float] = None) -> bool:
        if timeout_ms is None or timeout_ms < 0:
            return _libc.sem_wait(self._sem) == 0
        abs_ts = _timespec()
        if _libc.clock_gettime(CLOCK_REALTIME, ctypes.byref(abs_ts)) != 0:
            return False
        sec = int(timeout_ms // 1000)
        nsec = int((timeout_ms % 1000) * 1_000_000)
        abs_ts.tv_sec += sec
        abs_ts.tv_nsec += nsec
        if abs_ts.tv_nsec >= 1_000_000_000:
            abs_ts.tv_sec += 1
            abs_ts.tv_nsec -= 1_000_000_000
        return _libc.sem_timedwait(self._sem, ctypes.byref(abs_ts)) == 0

    def close(self):
        if self._sem:
            _libc.sem_close(self._sem)
            self._sem = None


class _ShmReader:
    """共享内存帧读取器（内部使用）"""

    def __init__(self, stream_id: str):
        self.stream_id = stream_id
        self.shm_paths = [f"/dev/shm/{stream_id}", f"/{stream_id.lstrip('/')}"]

        self.ALIGNMENT = 64
        self.UINT64_SIZE = 8
        self.UINT32_SIZE = 4
        self.MAX_FRAME_BYTES = 6 * 2560 * 1440
        self.SLOT_COUNT = 8

        self.META_DATA_SIZE = 4 * self.UINT64_SIZE + 4 * self.UINT32_SIZE
        self.META_STRUCT_SIZE = align_up(self.META_DATA_SIZE, self.ALIGNMENT)

        self.SEQ_OFFSET = 0
        self.META_OFFSET = align_up(self.SEQ_OFFSET + self.UINT64_SIZE, self.ALIGNMENT)
        self.PAYLOAD_OFFSET = self.META_OFFSET + self.META_STRUCT_SIZE

        slot_raw_size = self.PAYLOAD_OFFSET + self.MAX_FRAME_BYTES
        self.SLOT_SIZE = align_up(slot_raw_size, self.ALIGNMENT)

        self._mmap_obj: Optional[mmap.mmap] = None
        self._shm_view: Optional[memoryview] = None
        self._last_idx = -1
        self._connected = False
        self._notify_sem: Optional[_NotifySemaphore] = None
        self._blocking_mode_logged = False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def exists(self) -> bool:
        """检查本地是否存在该共享内存文件"""
        return any(os.path.exists(p) for p in self.shm_paths)

    def connect(self) -> bool:
        if self._connected:
            return True
        for path in self.shm_paths:
            if os.path.exists(path):
                try:
                    fd = os.open(path, os.O_RDONLY)
                    total_size = self.SLOT_COUNT * self.SLOT_SIZE + align_up(self.UINT64_SIZE, self.ALIGNMENT)
                    self._mmap_obj = mmap.mmap(fd, total_size, prot=mmap.PROT_READ)
                    self._shm_view = memoryview(self._mmap_obj)
                    os.close(fd)
                    try:
                        self._notify_sem = _NotifySemaphore(f"/{self.stream_id}_notify")
                        logger.info(f"✓ SHM notify semaphore connected: /{self.stream_id}_notify")
                    except OSError as e:
                        logger.warning(f"SHM notify semaphore not available (will use polling fallback): {e}")
                        self._notify_sem = None
                    self._connected = True
                    logger.debug(f"✓ SHM connected: {path}")
                    return True
                except Exception as e:
                    logger.error(f"✗ SHM connect failed {path}: {e}")
        return False

    def _read_u64(self, offset: int) -> Optional[int]:
        end = offset + self.UINT64_SIZE
        if end > len(self._shm_view):
            return None
        return struct.unpack("Q", self._shm_view[offset:end])[0]

    def _read_meta(self, slot_offset: int) -> Optional[dict]:
        start = slot_offset + self.META_OFFSET
        end = start + self.META_DATA_SIZE
        if end > len(self._shm_view):
            return None
        raw = self._shm_view[start:end]
        f = struct.unpack("QQQQIIII", raw)
        return {
            'size': f[0], 'w': f[1], 'h': f[2], 'ts': f[3],
            'ch': f[4], 'depth': f[5], 'step': f[6], '_rsv': f[7]
        }

    def _rebuild_frame(self, raw_data, meta: dict) -> Optional[np.ndarray]:
        w, h, c = meta['w'], meta['h'], meta['ch']
        depth, step = meta['depth'], meta['step']
        dtype = CV_DEPTH_TO_NUMPY.get(depth, np.uint8)
        elem_sz = np.dtype(dtype).itemsize

        arr = np.frombuffer(raw_data, dtype=dtype)
        expected_step = w * c * elem_sz

        if step > 0 and step != expected_step:
            row_elems = step // elem_sz
            if arr.size < h * row_elems:
                return None
            img = arr[:h * row_elems].reshape((h, row_elems))
            img = img[:, :w * c]
        else:
            if arr.size < h * w * c:
                return None
            img = arr[:h * w * c]

        if c == 1:
            img = img.reshape((h, w))
        else:
            img = img.reshape((h, w, c))

        if not img.flags.writeable:
            img = img.copy()
        return img

    def grab(self) -> bool:
        if not self._shm_view:
            return False
        try:
            head_off = align_up(self.SLOT_COUNT * self.SLOT_SIZE, self.ALIGNMENT)
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return False
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):
                return False
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0:
                return False
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            return v1 == v2
        except Exception as e:
            logger.debug(f"grab error: {e}")
            return False

    def retrieve(self) -> Tuple[Optional[np.ndarray], int]:
        if not self._shm_view:
            return None, 0
        try:
            head_off = align_up(self.SLOT_COUNT * self.SLOT_SIZE, self.ALIGNMENT)
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return None, 0
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):
                return None, 0
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0 or meta['size'] > self.MAX_FRAME_BYTES:
                return None, 0
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 != v2:
                return None, 0
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

    def read(self, blocking: bool = False, timeout_ms: Optional[float] = None) -> Tuple[bool, Optional[np.ndarray], int]:
        if not blocking:
            return self._try_read()

        if self._notify_sem:
            if not self._blocking_mode_logged:
                logger.info(f"[_ShmReader] Using semaphore blocking mode for {self.stream_id}")
                self._blocking_mode_logged = True
            ok, img, ts = self._try_read()
            if ok:
                return ok, img, ts
            if not self._notify_sem.wait(timeout_ms):
                return False, None, 0
            return self._try_read()
        else:
            if not self._blocking_mode_logged:
                logger.warning(f"[_ShmReader] Semaphore unavailable for {self.stream_id}, using adaptive polling fallback")
                self._blocking_mode_logged = True
            start = time.time()
            sleep_ms = 1.0
            max_sleep_ms = 50.0
            while True:
                ok, img, ts = self._try_read()
                if ok:
                    return ok, img, ts
                if timeout_ms is not None:
                    elapsed = (time.time() - start) * 1000
                    if elapsed >= timeout_ms:
                        return False, None, 0
                    remaining = timeout_ms - elapsed
                    if sleep_ms > remaining:
                        sleep_ms = remaining
                if sleep_ms > 0:
                    time.sleep(sleep_ms / 1000.0)
                sleep_ms = min(sleep_ms * 1.5, max_sleep_ms)

    def _try_read(self) -> Tuple[bool, Optional[np.ndarray], int]:
        if not self.grab():
            return False, None, 0
        img, ts = self.retrieve()
        return (img is not None), img, ts

    def close(self):
        self._shm_view = None
        if self._mmap_obj:
            try:
                self._mmap_obj.close()
            except BufferError:
                pass
            self._mmap_obj = None
        if self._notify_sem:
            try:
                self._notify_sem.close()
            except Exception:
                pass
            self._notify_sem = None
        self._connected = False


# ==================== gRPC 基类 ====================

class _BaseRTSPClient:
    """gRPC 连接与流生命周期管理（内部基类）"""

    def __init__(self, server_address: str = '127.0.0.1:50052'):
        self.server_address = server_address
        self._channel: Optional[grpc.Channel] = None
        self._stub: Optional[stream_service_pb2_grpc.RTSPStreamServiceStub] = None

    def connect(self, options: Optional[List[tuple]] = None) -> bool:
        try:
            opts = options if options is not None else _DEFAULT_CHANNEL_OPTIONS
            self._channel = grpc.insecure_channel(self.server_address, options=opts)
            self._stub = stream_service_pb2_grpc.RTSPStreamServiceStub(self._channel)
            logger.info(f"已连接到服务器: {self.server_address}")
            return True
        except Exception as e:
            logger.error(f"连接服务器失败: {e}")
            return False

    def disconnect(self):
        if self._channel:
            self._channel.close()
            self._channel = None
            self._stub = None
            logger.info("已断开服务器连接")

    def is_connected(self) -> bool:
        return self._stub is not None

    def _ensure_stub(self) -> bool:
        if self._stub is not None:
            return True
        return self.connect()

    def start_stream(self,
                     rtsp_url: str,
                     heartbeat_timeout_ms: int = 100000,
                     decode_interval_ms: int = 0,
                     decoder_type: int = DECODER_CPU_FFMPEG,
                     gpu_id: int = 0,
                     keep_on_failure: bool = False,
                     use_shared_mem: bool = False,
                     only_key_frames: bool = False) -> Optional[str]:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return None

        try:
            logger.info(
                f"正在启动流: {rtsp_url} "
                f"(解码器: {DECODER_NAMES.get(decoder_type, 'Unknown')}, "
                f"GPU ID: {gpu_id}, SHM: {use_shared_mem}, "
                f"Only Key Frames: {'Yes' if only_key_frames else 'No'})"
            )
            if decoder_type != DECODER_GPU_NVCUVID:
                gpu_id = -1

            req = stream_service_pb2.StartRequest(
                rtsp_url=rtsp_url,
                heartbeat_timeout_ms=heartbeat_timeout_ms,
                decode_interval_ms=decode_interval_ms,
                decoder_type=decoder_type,
                gpu_id=gpu_id,
                keep_on_failure=keep_on_failure,
                use_shared_mem=use_shared_mem,
                only_key_frames=only_key_frames
            )
            resp = self._stub.StartStream(req, timeout=10)

            if resp.success:
                logger.info(f"流启动成功: {resp.stream_id} -> {rtsp_url}")
                return resp.stream_id
            else:
                logger.error(f"流启动失败: {resp.message}")
                return None
        except grpc.RpcError as e:
            logger.error(f"启动流异常: {e.details()}")
            return None

    def stop_stream(self, stream_id: str) -> bool:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return False
        try:
            req = stream_service_pb2.StopRequest(stream_id=stream_id)
            resp = self._stub.StopStream(req, timeout=5)
            if resp.success:
                logger.info(f"流已停止: {stream_id}")
            else:
                logger.warning(f"停止流失败: {resp.message}")
            return resp.success
        except grpc.RpcError as e:
            logger.error(f"停止流异常: {e.details()}")
            return False

    def update_stream_url(self, stream_id: str, new_rtsp_url: str) -> bool:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return False
        try:
            req = stream_service_pb2.UpdateStreamRequest(stream_id=stream_id, new_rtsp_url=new_rtsp_url)
            resp = self._stub.UpdateStream(req, timeout=5)
            if resp.success:
                logger.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
            else:
                logger.warning(f"更新流 URL 失败: {resp.message}")
            return resp.success
        except grpc.RpcError as e:
            logger.error(f"更新流 URL 异常: {e.details()}")
            return False

    def update_stream_url_isolated(self, stream_id: str, new_rtsp_url: str) -> bool:
        with grpc.insecure_channel(self.server_address) as channel:
            stub = stream_service_pb2_grpc.RTSPStreamServiceStub(channel)
            try:
                req = stream_service_pb2.UpdateStreamRequest(stream_id=stream_id, new_rtsp_url=new_rtsp_url)
                resp = stub.UpdateStream(req, timeout=5)
                if resp.success:
                    logger.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
                else:
                    logger.warning(f"更新流 URL 失败: {resp.message}")
                return resp.success
            except grpc.RpcError as e:
                logger.error(f"更新流 URL 异常: {e.details()}")
                return False

    def list_streams(self) -> List[Dict]:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return []
        try:
            req = stream_service_pb2.ListStreamsRequest()
            resp = self._stub.ListStreams(req, timeout=5)
            streams = []
            for s in resp.streams:
                streams.append({
                    "stream_id": s.stream_id,
                    "rtsp_url": s.rtsp_url,
                    "status": s.status,
                    "status_name": STATUS_NAMES.get(s.status, "未知"),
                    "decoder_type": DECODER_NAMES.get(s.decoder_type, "Unknown"),
                    "decoder_type_raw": s.decoder_type,
                    "width": s.width,
                    "height": s.height,
                    "decode_interval_ms": s.decode_interval_ms,
                    "heartbeat_timeout_ms": s.heartbeat_timeout_ms,
                    "keep_on_failure": s.keep_on_failure,
                    "only_key_frames": s.only_key_frames,
                    "use_shared_mem": s.use_shared_mem
                })
            return streams
        except grpc.RpcError as e:
            logger.error(f"查询流列表失败: {e.details()}")
            return []

    def get_stream_count(self) -> int:
        if not self._ensure_stub():
            return -1
        try:
            req = stream_service_pb2.ListStreamsRequest()
            resp = self._stub.ListStreams(req, timeout=5)
            return resp.total_count
        except grpc.RpcError:
            return -1

    def check_stream(self, stream_id: str) -> Optional[Dict]:
        if not self._ensure_stub():
            return None
        try:
            req = stream_service_pb2.CheckRequest(stream_id=stream_id)
            resp = self._stub.CheckStream(req, timeout=5)
            s = resp.stream
            return {
                "stream_id": s.stream_id or stream_id,
                "rtsp_url": s.rtsp_url,
                "status": s.status,
                "status_name": STATUS_NAMES.get(s.status, "未知"),
                "decoder_type": DECODER_NAMES.get(s.decoder_type, "Unknown"),
                "decoder_type_raw": s.decoder_type,
                "width": s.width,
                "height": s.height,
                "decode_interval_ms": s.decode_interval_ms,
                "heartbeat_timeout_ms": s.heartbeat_timeout_ms,
                "keep_on_failure": s.keep_on_failure,
                "only_key_frames": s.only_key_frames,
                "use_shared_mem": s.use_shared_mem,
                "server_message": resp.message
            }
        except grpc.RpcError as e:
            logger.error(f"检查流状态异常: {e.details()}")
            return None

    def check_stream_exists(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        if info is None:
            return False
        return info.get("status", STATUS_NOT_FOUND) != STATUS_NOT_FOUND

    def is_stream_connected(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        return info is not None and info.get("status") == STATUS_CONNECTED

    def get_stream_status(self, stream_id: str) -> int:
        info = self.check_stream(stream_id)
        return info.get("status", STATUS_NOT_FOUND) if info else STATUS_NOT_FOUND

    def get_stream_status_name(self, stream_id: str) -> str:
        return STATUS_NAMES.get(self.get_stream_status(stream_id), "未知")

    def __enter__(self):
        if not self.connect():
            raise RuntimeError(f"无法连接到服务器: {self.server_address}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()


# ==================== 统一客户端 ====================

class RTSPClient(_BaseRTSPClient):
    """
    统一 RTSP 客户端：支持 gRPC JPEG 与共享内存两种帧获取方式

    通过 start_stream(..., use_shared_mem=...) 选择路径：
      - False -> read(stream_id) 走 gRPC JPEG，返回 (timestamp, frame)
      - True  -> read(stream_id) 走本地共享内存，返回 (timestamp, frame)

    注：服务端 gRPC 字段名为 frame_seq，但实际下发的是时间戳（毫秒），
    因此两种模式统一理解为“帧时间戳”。

    若 use_shared_mem=True 但本地不存在 /dev/shm/<stream_id>，
    read() 会报错并返回 (-1, None)（说明客户端与服务端不在同一机器或 SHM 创建失败）。
    """

    def __init__(self, server_address: str = '127.0.0.1:50051'):
        super().__init__(server_address)
        self._shm_readers: Dict[str, _ShmReader] = {}
        self._stream_modes: Dict[str, bool] = {}  # stream_id -> use_shared_mem 缓存

    def disconnect(self):
        # 清理所有 SHM 读取器
        for reader in self._shm_readers.values():
            try:
                reader.close()
            except Exception:
                pass
        self._shm_readers.clear()
        self._stream_modes.clear()
        super().disconnect()

    @staticmethod
    def _decode_jpeg(jpeg_data: bytes) -> Optional[np.ndarray]:
        if not jpeg_data:
            return None
        if not _HAS_TURBOJPEG:
            # 降级到 OpenCV
            try:
                img_array = np.frombuffer(jpeg_data, dtype=np.uint8)
                return cv2.imdecode(img_array, cv2.IMREAD_COLOR)
            except Exception as e:
                logger.error(f"OpenCV JPEG 解码失败: {e}")
                return None
        try:
            img = turbojpeg.decompress(jpeg_data, pixelformat=turbojpeg.BGR)
            return np.array(img)
        except Exception as e:
            logger.error(f"TurboJPEG 解码失败: {e}")
            return None

    def _get_shm_reader(self, stream_id: str) -> Optional[_ShmReader]:
        """获取或创建指定流的 SHM 读取器；若本地无 SHM 则返回 None 并记录错误"""
        reader = self._shm_readers.get(stream_id)
        if reader is not None:
            if not reader.exists():
                logger.error(
                    f"[RTSPClient] 共享内存已消失: /dev/shm/{stream_id}，"
                    f"请确认客户端与服务端在同一主机且 --ipc=host 已开启"
                )
                return None
            return reader

        reader = _ShmReader(stream_id)
        if not reader.exists():
            logger.error(
                f"[RTSPClient] 未找到共享内存: /dev/shm/{stream_id}。"
                f"可能原因：1) 客户端与服务端不在同一机器；2) 服务端 SHM 创建失败；3) 该流未启用共享内存。"
            )
            return None

        if not reader.connect():
            logger.error(f"[RTSPClient] 共享内存连接失败: /dev/shm/{stream_id}")
            return None

        self._shm_readers[stream_id] = reader
        return reader

    def _stream_uses_shm(self, stream_id: str) -> bool:
        """查询服务端确认该流是否启用了共享内存（结果缓存，避免每次 read 都查）"""
        if stream_id in self._stream_modes:
            return self._stream_modes[stream_id]
        info = self.check_stream(stream_id)
        uses = info is not None and info.get("use_shared_mem", False)
        self._stream_modes[stream_id] = uses
        return uses

    def read(self, stream_id: str, blocking: bool = False, timeout_ms: Optional[float] = None) -> Tuple[int, Optional[np.ndarray]]:
        """
        获取指定流的最新帧，自动根据 use_shared_mem 选择路径

        :param stream_id: 流 ID
        :param blocking: 仅对 SHM 模式有效，是否阻塞等待新帧
        :param timeout_ms: 仅对 SHM 模式有效，阻塞超时（毫秒）
        :return: (帧时间戳, 图像帧)。失败返回 (-1, None)
        """
        if not self._ensure_stub():
            return -1, None

        # 优先按服务端配置决定路径
        if self._stream_uses_shm(stream_id):
            reader = self._get_shm_reader(stream_id)
            if reader is None:
                return -1, None
            ok, img, ts = reader.read(blocking=blocking, timeout_ms=timeout_ms)
            if ok and img is not None:
                return int(ts), img
            return -1, None

        # gRPC JPEG 路径
        try:
            req = stream_service_pb2.FrameRequest(stream_id=stream_id)
            resp = self._stub.GetLatestFrame(req, timeout=5)
            frame_seq = getattr(resp, "frame_seq", -1)
            if resp.success and resp.image_data:
                img = self._decode_jpeg(resp.image_data)
                return frame_seq, img
            return frame_seq, None
        except Exception as e:
            logger.error(f"[RTSPClient] gRPC 读取失败: {e}")
            return -1, None

    def stream_frames(self, stream_id: str, max_fps: int = 0) -> Generator[Tuple[int, Optional[np.ndarray]], None, None]:
        """
        流式获取视频帧（仅 gRPC JPEG 模式支持生成器；SHM 模式会提示并降级为空）
        """
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return

        if self._stream_uses_shm(stream_id):
            logger.error(
                f"[RTSPClient] stream_frames 不支持共享内存模式，"
                f"请使用 read(stream_id, blocking=True)。"
            )
            return

        try:
            req = stream_service_pb2.StreamRequest(stream_id=stream_id, max_fps=max_fps)
            for resp in self._stub.StreamFrames(req):
                if resp.success and resp.image_data and resp.frame_seq != -1:
                    img = self._decode_jpeg(resp.image_data)
                    yield (resp.frame_seq, img)
                else:
                    yield (-1, None)
        except Exception as e:
            logger.error(f"流式读取异常: {e}")


# ==================== 使用示例 ====================
if __name__ == "__main__":
    server = '127.0.0.1:50051'
    rtsp_url = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"

    def wait_connected(client, stream_id, timeout_sec=10):
        for _ in range(timeout_sec * 5):
            status = client.get_stream_status(stream_id)
            if status == STATUS_CONNECTED:
                return True
            if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                return False
            time.sleep(0.2)
        return False

    # 示例1: gRPC JPEG 模式
    with RTSPClient(server) as client:
        sid = client.start_stream(rtsp_url, use_shared_mem=False)
        if sid and wait_connected(client, sid):
            ts, frame = client.read(sid)
            print(f"JPEG 模式读取: ts={ts}, shape={frame.shape if frame is not None else None}")
            client.stop_stream(sid)
        else:
            print("JPEG 模式：流未成功连接")

    # 示例2: 共享内存模式
    with RTSPClient(server) as client:
        sid = client.start_stream(rtsp_url, use_shared_mem=True)
        if sid and wait_connected(client, sid):
            ts, frame = client.read(sid, blocking=True, timeout_ms=1000)
            print(f"SHM 模式读取: ts={ts}, shape={frame.shape if frame is not None else None}")
            client.stop_stream(sid)
        else:
            print("SHM 模式：流未成功连接")
