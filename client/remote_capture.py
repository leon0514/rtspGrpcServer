"""
RemoteCapture - 与 RTSP URL 解耦的远程视频捕获客户端

支持管理多个流，提供统一的 gRPC 服务器连接管理。
"""
import grpc
import cv2
import numpy as np
import logging
from typing import Optional, List, Dict, Generator, Tuple

import stream_service_pb2
import stream_service_pb2_grpc

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# 解码器类型常量
DECODER_CPU_FFMPEG = stream_service_pb2.DECODER_CPU_FFMPEG
DECODER_GPU_NVCUVID = stream_service_pb2.DECODER_GPU_NVCUVID

# 解码器类型名称映射
DECODER_NAMES = {
    DECODER_CPU_FFMPEG: "CPU (FFmpeg)",
    DECODER_GPU_NVCUVID: "GPU (NVCUVID)"
}

# 流状态常量
STATUS_CONNECTING = stream_service_pb2.STATUS_CONNECTING
STATUS_CONNECTED = stream_service_pb2.STATUS_CONNECTED
STATUS_DISCONNECTED = stream_service_pb2.STATUS_DISCONNECTED
STATUS_NOT_FOUND = stream_service_pb2.STATUS_NOT_FOUND

# 流状态名称映射
STATUS_NAMES = {
    STATUS_CONNECTING: "连接中",
    STATUS_CONNECTED: "已连接",
    STATUS_DISCONNECTED: "无法连接",
    STATUS_NOT_FOUND: "不存在"
}


class RemoteCapture:
    """
    远程视频捕获客户端
    
    示例用法:
        # 连接服务器
        client = RemoteCapture('127.0.0.1:50052')
        client.connect()
        
        # 启动流
        stream_id = client.start_stream('rtsp://...', decoder_type=DECODER_GPU_CUDA)
        
        # 获取帧
        ret, frame = client.read(stream_id)
        
        # 查询所有流
        streams = client.list_streams()
        
        # 停止流
        client.stop_stream(stream_id)
        
        # 断开连接
        client.disconnect()
    """
    
    def __init__(self, server_address: str = '127.0.0.1:50052'):
        """
        初始化客户端
        :param server_address: gRPC 服务器地址
        """
        self.server_address = server_address
        self.channel: Optional[grpc.Channel] = None
        self.stub: Optional[stream_service_pb2_grpc.RTSPStreamServiceStub] = None
    
    def connect(self) -> bool:
        """
        连接到 gRPC 服务器
        :return: 是否连接成功
        """
        try:
            options = [
                ('grpc.max_receive_message_length', 10 * 1024 * 1024),
                ('grpc.keepalive_time_ms', 5000),
                ('grpc.keepalive_timeout_ms', 5000),
                ('grpc.keepalive_permit_without_calls', True)
            ]
            self.channel = grpc.insecure_channel(self.server_address, options=options)
            self.stub = stream_service_pb2_grpc.RTSPStreamServiceStub(self.channel)
            logging.info(f"已连接到服务器: {self.server_address}")
            return True
        except Exception as e:
            logging.error(f"连接服务器失败: {e}")
            return False
    
    def disconnect(self):
        """断开与服务器的连接"""
        if self.channel:
            self.channel.close()
            self.channel = None
            self.stub = None
            logging.info("已断开服务器连接")
    
    def is_connected(self) -> bool:
        """检查是否已连接到服务器"""
        return self.stub is not None
    
    # ==================== 流管理 ====================
    
    def start_stream(self,
                     rtsp_url: str,
                     heartbeat_timeout_ms: int = 100000,
                     decode_interval_ms: int = 0,
                     decoder_type: int = DECODER_CPU_FFMPEG,
                     gpu_id: int = 0,
                     keep_on_failure: bool = False,
                     use_shared_mem: bool = False,
                     only_key_frames: bool = False) -> Optional[str]:
        """
        启动一个新的 RTSP 流
        :param rtsp_url: RTSP 地址
        :param heartbeat_timeout_ms: 心跳超时时间（毫秒）
        :param decode_interval_ms: 解码间隔（毫秒），0 表示不限制
        :param decoder_type: 解码器类型
        :param gpu_id: GPU ID（仅 GPU 解码有效）
        :param keep_on_failure: 打开失败时是否保留任务，默认 False
        :return: 成功返回 stream_id，失败返回 None
        """
        if not self.stub:
            logging.error("未连接到服务器")
            return None
        
        try:
            print(f"正在启动流: {rtsp_url} (解码器: {DECODER_NAMES.get(decoder_type, 'Unknown')}, GPU ID: {gpu_id}), Only Key Frames: {'Yes' if only_key_frames else 'No'}")
            if decoder_type != DECODER_GPU_NVCUVID:
                gpu_id = -1  # CPU 解码不使用 GPU ID
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
            resp = self.stub.StartStream(req, timeout=10)
            
            if resp.success:
                logging.info(f"流启动成功: {resp.stream_id} -> {rtsp_url}")
                return resp.stream_id
            else:
                logging.error(f"流启动失败: {resp.message}")
                return None
        except grpc.RpcError as e:
            logging.error(f"启动流异常: {e.details()}")
            return None
    
    def stop_stream(self, stream_id: str) -> bool:
        """
        停止指定的流
        :param stream_id: 流 ID
        :return: 是否成功
        """
        if not self.stub:
            logging.error("未连接到服务器")
            return False
        
        try:
            req = stream_service_pb2.StopRequest(stream_id=stream_id)
            resp = self.stub.StopStream(req, timeout=5)
            
            if resp.success:
                logging.info(f"流已停止: {stream_id}")
            else:
                logging.warning(f"停止流失败: {resp.message}")
            return resp.success
        except grpc.RpcError as e:
            logging.error(f"停止流异常: {e.details()}")
            return False
    
    # ==================== 流查询 ====================
    
    def list_streams(self) -> List[Dict]:
        """
        查询所有流信息
        :return: 流信息列表
        """
        if not self.stub:
            logging.error("未连接到服务器")
            return []
        
        try:
            req = stream_service_pb2.ListStreamsRequest()
            resp = self.stub.ListStreams(req, timeout=5)
            
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
            logging.error(f"查询流列表失败: {e.details()}")
            return []
    
    def get_stream_count(self) -> int:
        """
        获取流总数
        :return: 流总数，失败返回 -1
        """
        if not self.stub:
            return -1
        
        try:
            req = stream_service_pb2.ListStreamsRequest()
            resp = self.stub.ListStreams(req, timeout=5)
            return resp.total_count
        except grpc.RpcError:
            return -1
    
    def check_stream(self, stream_id: str) -> Optional[Dict]:
        """
        查询单个流的详细信息
        :param stream_id: 流 ID
        :return: 流信息字典，不存在返回 None
        """
        if not self.stub:
            return None
        
        try:
            req = stream_service_pb2.CheckRequest(stream_id=stream_id)
            resp = self.stub.CheckStream(req, timeout=5)
            
            # 注意：resp 包含 stream 字段和 top-level 的 message 字段
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
                "server_message": resp.message  # CheckResponse 里的提示信息
            }
        except grpc.RpcError as e:
            logging.error(f"检查流状态异常: {e.details()}")
            return None


    def check_stream_exists(self, stream_id: str) -> bool:
        """
        检查流是否存在
        :param stream_id: 流 ID
        :return: 流是否存在
        """
        info = self.check_stream(stream_id)
        if info is None:
            return False
        status = info.get("status", STATUS_NOT_FOUND)
        return status != STATUS_NOT_FOUND
    
    def is_stream_connected(self, stream_id: str) -> bool:
        """
        检查流是否已连接
        :param stream_id: 流 ID
        :return: 是否连接
        """
        info = self.check_stream(stream_id)
        return info is not None and info.get("status") == STATUS_CONNECTED
    
    def get_stream_status(self, stream_id: str) -> int:
        """
        获取流的连接状态
        :param stream_id: 流 ID
        :return: 状态码，不存在返回 STATUS_NOT_FOUND
        """
        info = self.check_stream(stream_id)
        return info.get("status", STATUS_NOT_FOUND) if info else STATUS_NOT_FOUND
    
    def get_stream_status_name(self, stream_id: str) -> str:
        """
        获取流的连接状态名称
        :param stream_id: 流 ID
        :return: 状态名称字符串
        """
        return STATUS_NAMES.get(self.get_stream_status(stream_id), "未知")
    
    # ==================== 帧获取 ====================
    
    def read(self, stream_id: str) -> Tuple[bool, Optional[np.ndarray]]:
        """
        获取指定流的最新帧
        :param stream_id: 流 ID
        :return: (图像帧序列号, 图像帧)
        """
        if not self.stub:
            return -1, None
        
        try:
            req = stream_service_pb2.FrameRequest(stream_id=stream_id)
            resp = self.stub.GetLatestFrame(req, timeout=5)
            frame_seq = getattr(resp, "frame_seq", -1)
            if resp.success and resp.image_data:
                img_array = np.frombuffer(resp.image_data, dtype=np.uint8)
                img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
                if img is not None:
                    return frame_seq, img
            return frame_seq, None
        except grpc.RpcError:
            return -1, None
    
    def stream_frames(self, stream_id: str, max_fps: int = 0) -> Generator[Tuple[bool, Optional[np.ndarray]], None, None]:
        """
        流式获取视频帧（生成器）
        :param stream_id: 流 ID
        :param max_fps: 最大帧率，0 表示不限制
        :yield: (成功标志, 图像帧)
        """
        if not self.stub:
            logging.error("未连接到服务器")
            return
        
        try:
            req = stream_service_pb2.StreamRequest(stream_id=stream_id, max_fps=max_fps)
            for resp in self.stub.StreamFrames(req):                 
                if resp.success and resp.image_data and resp.frame_seq != -1:
                    img_array = np.frombuffer(resp.image_data, dtype=np.uint8)
                    img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
                    yield (resp.frame_seq, img)
                else:
                    yield (-1, None)
        except grpc.RpcError as e:
            logging.error(f"流式读取异常: {e.details()}")
    

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

    def update_stream_url_isolated(self, stream_id: str, new_rtsp_url: str) -> bool:
        """使用独立连接更新，防止头部阻塞"""
        with grpc.insecure_channel(self.server_address) as channel:
            stub = stream_service_pb2_grpc.RTSPStreamServiceStub(channel)
            try:
                req = stream_service_pb2.UpdateStreamRequest(stream_id=stream_id, new_rtsp_url=new_rtsp_url)
                resp = stub.UpdateStream(req, timeout=5)
                if resp.success:
                        logging.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
                else:
                    logging.warning(f"更新流 URL 失败: {resp.message}")
                return resp.success
            except grpc.RpcError as e:
                logging.error(f"更新流 URL 异常: {e.details()}")
                return False

    # ==================== 上下文管理 ====================
    
    def __enter__(self):
        if not self.connect():
            raise RuntimeError(f"无法连接到服务器: {self.server_address}")
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()


# ==================== 使用示例 ====================
if __name__ == "__main__":
    # 使用上下文管理器
    with RemoteCapture('127.0.0.1:50052') as client:
        # 查询当前所有流
        print(f"当前流数量: {client.get_stream_count()}")
        for s in client.list_streams():
            print(f"  - {s['stream_id']}: {s['rtsp_url']} ({s['status_name']})")
        
        # 启动新流
        # stream_id = client.start_stream(
        #     'rtsp://admin:password@192.168.1.100:554/stream',
        #     decoder_type=DECODER_GPU_CUDA
        # )
        # 
        # if stream_id:
        #     # 读取帧
        #     ret, frame = client.read(stream_id)
        #     if ret:
        #         cv2.imshow('Frame', frame)
        #         cv2.waitKey(0)
        #     
        #     # 停止流
        #     client.stop_stream(stream_id)
