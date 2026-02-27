from remote_video_capture import RemoteVideoCapture
import time
import cv2

if __name__ == '__main__':
    url = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/801"
    with RemoteVideoCapture(url, decode_interval_ms=100) as cap:
        while not cap.is_connected():
            print("摄像头暂时未连接成功，后台正尝试重连...")
            time.sleep(2)
            
        print("▶️ 开始拉取画面...")
        
        index = 0
        while index < 100:
            ret, frame = cap.read()
            if ret:
                # 成功取到画面
                filename = f"capture_{index}.jpg"
                cv2.imwrite(filename, frame)
                print(f"已保存: {filename}")
                index += 1
            else:
                time.sleep(0.05)