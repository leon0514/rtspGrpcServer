from remote_video_capture import RemoteVideoCapture
from stream_service_pb2 import DECODER_CPU_OPENCV, DECODER_GPU_CUDA
import time
import cv2

if __name__ == '__main__':
    url = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/801"
    with RemoteVideoCapture(url, decode_interval_ms=1000, decoder_type=1) as cap:
        while not cap.is_connected():
            print("摄像头暂时未连接成功，后台正尝试重连...")
            time.sleep(2)
            
        print("▶️ 开始拉取画面...")
        
        index = 0
        while True:
            ret, frame = cap.read()
            if ret:
                # 成功取到画面
                filename = f"capture_{index}.jpg"
                # cv2.imwrite(filename, frame)
                # print(f"已保存: {filename}")
            else:
                print("未取到画面")
                time.sleep(0.05)
            index += 1
            time.sleep(2)
                