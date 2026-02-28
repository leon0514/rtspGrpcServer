import grpc
import cv2
import numpy as np
import time
import logging

# 导入 gRPC 生成的代码
import stream_service_pb2
import stream_service_pb2_grpc

# 配置简单的日志输出
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

DECODER_CPU_OPENCV = stream_service_pb2.DECODER_CPU_OPENCV
DECODER_GPU_CUDA = stream_service_pb2.DECODER_GPU_CUDA

class RemoteVideoCapture:
    def __init__(self, 
                 rtsp_url: str, 
                 server_address: str = '127.0.0.1:50052',
                 heartbeat_timeout_ms: int = 10000,
                 decode_interval_ms: int = 0,
                 decoder_type: int = None):
        self.rtsp_url = rtsp_url
        self.server_address = server_address
        self.heartbeat_timeout_ms = heartbeat_timeout_ms
        self.decode_interval_ms = decode_interval_ms
        
        # 使用传入的类型，如果未指定则默认使用 OpenCV CPU
        self.decoder_type = decoder_type if decoder_type is not None else stream_service_pb2.DECODER_CPU_OPENCV
        
        self.channel = None
        self.stub = None
        self.stream_id = None

    def connect(self) -> bool:
        options = [
            ('grpc.max_receive_message_length', 10 * 1024 * 1024),
            ('grpc.keepalive_time_ms', 10000), # 增加心跳保活
            ('grpc.keepalive_timeout_ms', 5000),
            ('grpc.keepalive_permit_without_calls', True)
        ]
        self.channel = grpc.insecure_channel(self.server_address, options=options)
        self.stub = stream_service_pb2_grpc.RTSPStreamServiceStub(self.channel)

        try:
            req = stream_service_pb2.StartRequest(
                rtsp_url=self.rtsp_url,
                heartbeat_timeout_ms=self.heartbeat_timeout_ms,
                decode_interval_ms=self.decode_interval_ms,
                decoder_type=self.decoder_type
            )
            # 增加超时限制，防止卡死
            resp = self.stub.StartStream(req, timeout=5) 
            
            if not resp.success:
                logging.error(f"服务器拒绝拉流: {resp.message}")
                return False
                
            self.stream_id = resp.stream_id
            logging.info(f"拉流任务创建成功: {self.stream_id}")
            return True
        except grpc.RpcError as e:
            logging.error(f"gRPC 错误: {e.code()} - {e.details()}")
            return False

    def is_connected(self) -> bool:
        """检查后台流是否健康连通"""
        if not self.stream_id or not self.stub:
            return False
            
        req = stream_service_pb2.CheckRequest(stream_id=self.stream_id)
        try:
            resp = self.stub.CheckStream(req)
            return resp.exists and resp.is_connected
        except grpc.RpcError:
            return False

    def get_stream_info(self) -> dict:
        """
        获取流的详细信息
        :return: 包含流信息的字典，如果获取失败返回 None
        """
        if not self.stream_id or not self.stub:
            return None
            
        req = stream_service_pb2.CheckRequest(stream_id=self.stream_id)
        try:
            resp = self.stub.CheckStream(req)
            if not resp.exists:
                return None
            
            decoder_names = {
                stream_service_pb2.DECODER_CPU_OPENCV: "CPU (OpenCV)",
                stream_service_pb2.DECODER_GPU_CUDA: "GPU (CUDA)",
                stream_service_pb2.DECODER_FFMPEG_NATIVE: "FFmpeg Native"
            }
            
            return {
                "stream_id": self.stream_id,
                "rtsp_url": resp.rtsp_url,
                "is_connected": resp.is_connected,
                "decoder_type": decoder_names.get(resp.decoder_type, "Unknown"),
                "width": resp.width,
                "height": resp.height,
                "decode_interval_ms": resp.decode_interval_ms,
                "message": resp.message
            }
        except grpc.RpcError as e:
            logging.error(f"获取流信息失败: {e.details()}")
            return None

    def read(self):
        """
        获取最新一帧画面
        :return: (ret, frame) ret 为 bool，frame 为 numpy array (BGR格式)
        """
        if not self.stream_id or not self.stub:
            return False, None

        req = stream_service_pb2.FrameRequest(stream_id=self.stream_id)
        try:
            resp = self.stub.GetLatestFrame(req)
            
            if resp.success and resp.image_data:
                # 字节流转 NumPy 矩阵
                img_array = np.frombuffer(resp.image_data, dtype=np.uint8)
                img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
                if img is not None:
                    return True, img
                    
            return False, None
        except grpc.RpcError as e:
            logging.error(f"读取画面异常: {e.details()}")
            return False, None

    def release(self):
        """释放资源，通知服务端关闭流"""
        if self.stream_id and self.stub:
            logging.info(f"通知服务器关闭流: {self.stream_id}")
            try:
                req = stream_service_pb2.StopRequest(stream_id=self.stream_id)
                resp = self.stub.StopStream(req)
                if resp.success:
                    logging.info("服务器已成功释放流资源！")
                else:
                    logging.warning(f"关闭流失败: {resp.message}")
            except grpc.RpcError:
                logging.warning("无法联系服务器关闭流 (可能已断开)")
            finally:
                self.stream_id = None
                
        if self.channel:
            self.channel.close()
            self.channel = None

    def __enter__(self):
        if not self.connect():
            raise RuntimeError("无法连接到视频流服务器")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.release()