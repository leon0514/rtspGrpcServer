import requests
import json
import base64
import time
import logging
import cv2
import numpy as np


# 配置日志
logging.basicConfig(level=logging.INFO, format='%(asctime)s - [%(levelname)s] - %(message)s')
logger = logging.getLogger(__name__)

BASE_URL = "http://172.16.20.193:8080"
session = requests.Session()


def base64_to_image(base64_str):
    """将Base64字符串转换为OpenCV图像"""
    img_data = base64.b64decode(base64_str)
    np_arr = np.frombuffer(img_data, np.uint8)
    img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
    return img

# ---------------------------------------------------------
# 1. 核心接口测试函数
# ---------------------------------------------------------

def test_list_streams():
    """GET /v1/streams"""
    logger.info("测试：获取流列表")
    resp = session.get(f"{BASE_URL}/v1/streams")
    assert resp.status_code == 200
    logger.info(f"响应: {resp.json()}")
    return resp.json()

def test_start_stream(rtsp_url):
    """POST /v1/streams/start"""
    logger.info(f"测试：启动流 {rtsp_url}")
    payload = {
        "rtspUrl": rtsp_url,
        "decodeIntervalMs": 33,
        "decoderType": "DECODER_CPU_FFMPEG",
        "keepOnFailure": False,
        "onlyKeyFrame": True
    }
    resp = session.post(f"{BASE_URL}/v1/streams/start", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    logger.info(f"启动成功，StreamId: {data.get('streamId')}")
    return data.get("streamId")

def test_check_stream(stream_id):
    """GET /v1/streams/{streamId}/status"""
    logger.info(f"测试：检查流状态 {stream_id}")
    resp = session.get(f"{BASE_URL}/v1/streams/{stream_id}/status")
    assert resp.status_code == 200
    status_info = resp.json()
    logger.info(f"当前状态: {status_info.get('status')}")
    return status_info

def test_get_latest_frame(stream_id):
    """GET /v1/streams/{streamId}/frame"""
    logger.info(f"测试：获取最新帧 {stream_id}")
    resp = session.get(f"{BASE_URL}/v1/streams/{stream_id}/frame")
    assert resp.status_code == 200
    data = resp.json()
    assert "imageData" in data
    logger.info(f"获取帧成功，Base64长度: {len(data['imageData'])}")
    if len(data["imageData"]) == 0 :
        logger.warning("Base64数据长度异常，可能未正确获取帧")
        return None
    image = base64_to_image(data["imageData"])
    return image

def test_update_stream(stream_id, new_url):
    """PUT /v1/streams/{streamId}"""
    logger.info(f"测试：更新流 URL {stream_id}")
    payload = {"newRtspUrl": new_url}
    resp = session.put(f"{BASE_URL}/v1/streams/{stream_id}", json=payload)
    assert resp.status_code == 200
    logger.info(f"更新结果: {resp.json()}")

def test_stream_frames_chunked(stream_id, duration=3):
    """GET /v1/streams/{streamId}/stream (流式接口)"""
    logger.info(f"测试：开始消费流数据 {stream_id}")
    url = f"{BASE_URL}/v1/streams/{stream_id}/stream?maxFps=5"
    with session.get(url, stream=True, timeout=10) as r:
        start = time.time()
        for line in r.iter_lines():
            if time.time() - start > duration: break
            if line:
                chunk = json.loads(line)
                print(f"收到数据: {chunk.keys()}")
    logger.info("流消费测试结束")

def test_stop_stream(stream_id):
    """POST /v1/streams/stop"""
    logger.info(f"测试：停止流 {stream_id}")
    resp = session.post(f"{BASE_URL}/v1/streams/stop", json={"streamId": stream_id})
    assert resp.status_code == 200
    logger.info(f"停止响应: {resp.json()}")

# ---------------------------------------------------------
# 2. 自动化编排逻辑
# ---------------------------------------------------------

if __name__ == "__main__":
    try:
        # 流程编排
        sid = test_start_stream("rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/501")
        time.sleep(1) # 给后端预留启动时间
        status = "CONNECTING"
        while status == "CONNECTING":
            status_info = test_check_stream(sid)
            status = status_info.get("status")
            logger.info(f"当前流状态: {status}")
            time.sleep(1)
        
        for _ in range(100):
            image = None
            while image is None:
                image = test_get_latest_frame(sid)
            cv2.imwrite("latest_frame.jpg", image)  # 保存最新帧到本地
        # test_update_stream(sid, "rtsp://new_url")
        # test_stream_frames_chunked(sid)
        
        test_list_streams()
        test_stop_stream(sid)
        
        print("\n[所有测试通过]")
    except Exception as e:
        logger.error(f"测试中断: {e}")