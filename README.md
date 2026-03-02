# RTSP gRPC 服务器

本项目是一个 C++ 实现的 **RTSP 流媒体服务器**，通过 gRPC 接口向客户端提供视频流服务。

---

## 特性

- **GPU 硬件加速**：支持 NVIDIA CUDA 硬件解码和 NVJPEG 编码
- **多解码器支持**：CPU (OpenCV) / GPU (CUDA) / FFmpeg 可选
- **流式传输**：gRPC 服务端流式推送，低延迟实时传输
- **多客户端共享**：单路解码，多客户端零拷贝共享
- **灵活帧率控制**：支持解码间隔和客户端独立帧率限制
- **流状态管理**：连接中 / 已连接 / 无法连接 / 不存在

---

## 依赖项

- C++17、CMake 3.10+、gRPC、Protobuf、OpenCV 4.x
- CUDA Toolkit（GPU 解码需要）
- Python3、grpcio、opencv-python（客户端）

---

## 快速开始

### 编译服务端
```bash
mkdir build && cd build
cmake .. && make -j
./rtsp_server
```

### 生成 Python Proto
```bash
cd client
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. stream_service.proto
```

---

## 枚举定义

### 解码器类型 (DecoderType)
| 值 | 名称 | 说明 |
|----|------|------|
| 0 | DECODER_CPU_OPENCV | OpenCV 软解 |
| 1 | DECODER_GPU_CUDA | NVIDIA CUDA 硬解 |
| 2 | DECODER_FFMPEG_NATIVE | FFmpeg 软解 |

### 流状态 (StreamStatus)
| 值 | 名称 | 说明 |
|----|------|------|
| 0 | STATUS_CONNECTING | 连接中 |
| 1 | STATUS_CONNECTED | 已连接 |
| 2 | STATUS_DISCONNECTED | 无法连接 |
| 3 | STATUS_NOT_FOUND | 不存在 |

---

## API 接口

### 1. StartStream - 启动流

启动一个 RTSP 流任务。

**请求参数：**
| 字段 | 类型 | 说明 |
|------|------|------|
| rtsp_url | string | RTSP 地址 |
| heartbeat_timeout_ms | int32 | 心跳超时（毫秒），0 表示不超时 |
| decode_interval_ms | int32 | 解码间隔（毫秒），0 表示不限制 |
| decoder_type | DecoderType | 解码器类型 |
| gpu_id | int32 | GPU ID（仅 GPU 解码有效） |
| keep_on_failure | bool | 打开失败时是否保留任务 |

**响应：**
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否成功 |
| stream_id | string | 流 ID |
| message | string | 消息 |


> **服务启动**：执行可执行文件时可指定监听地址/端口，格式 `address:port`。
> 如 `./rtsp_server` 默认为 `0.0.0.0:50051`，
> `./rtsp_server 0.0.0.0:6000` 则监听 6000 端口。

**Python 示例：**
```python
import os
from remote_capture import RemoteCapture, DECODER_GPU_CUDA

# 客户端默认连接到 127.0.0.1:50051，可通过环境变量 GRPC_SERVER 或在构造中指定其它地址
server_addr = os.getenv('GRPC_SERVER', '127.0.0.1:50051')
client = RemoteCapture(server_addr)
client.connect()

stream_id = client.start_stream(
    rtsp_url='rtsp://admin:password@192.168.1.100:554/stream',
    decoder_type=DECODER_GPU_CUDA,
    heartbeat_timeout_ms=30000,
    decode_interval_ms=100,
    gpu_id=0,
    keep_on_failure=False
)
print(f"流已启动: {stream_id}")
```

---

### 2. StopStream - 停止流

停止指定的流任务。

**请求参数：**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 流 ID |

**响应：**
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否成功 |
| message | string | 消息 |

**Python 示例：**
```python
success = client.stop_stream(stream_id)
print(f"停止结果: {success}")
```

---

### 3. GetLatestFrame - 获取单帧

获取指定流的最新一帧图像（轮询模式）。

**请求参数：**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 流 ID |

**响应：**
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否成功获取帧 |
| image_data | bytes | JPEG 编码的图像数据 |
| message | string | 消息 |

**Python 示例：**
```python
import cv2

# 循环获取最新帧
while True:
    ret, frame = client.read(stream_id)
    if ret:
        cv2.imshow('Frame', frame)
        if cv2.waitKey(30) & 0xFF == ord('q'):
            break
```

---

### 4. StreamFrames - 流式传输

服务端流式推送视频帧（推荐方式，低延迟）。

**请求参数：**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 流 ID |
| max_fps | int32 | 最大帧率，0 表示不限制 |

**响应（流式）：**
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否成功获取帧 |
| image_data | bytes | JPEG 编码的图像数据 |
| message | string | 消息 |

**Python 示例：**
```python
import cv2

# 流式接收，限制 15fps
for ret, frame in client.stream_frames(stream_id, max_fps=15):
    if ret:
        cv2.imshow('Stream', frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

cv2.destroyAllWindows()
```

---

### 5. CheckStream - 查询流状态

查询指定流的详细信息。

**请求参数：**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 流 ID |

**响应：**
| 字段 | 类型 | 说明 |
|------|------|------|
| status | StreamStatus | 连接状态 |
| message | string | 状态消息 |
| rtsp_url | string | RTSP 地址 |
| decoder_type | DecoderType | 解码器类型 |
| width | int32 | 视频宽度 |
| height | int32 | 视频高度 |
| decode_interval_ms | int32 | 解码间隔 |

**Python 示例：**
```python
from remote_capture import STATUS_CONNECTED, STATUS_NAMES

# 获取流详细信息
info = client.check_stream(stream_id)
if info:
    print(f"RTSP: {info['rtsp_url']}")
    print(f"状态: {info['status_name']}")
    print(f"分辨率: {info['width']}x{info['height']}")
    print(f"解码器: {info['decoder_type']}")

# 检查是否已连接
if client.is_stream_connected(stream_id):
    print("流已连接")

# 获取状态名称
status_name = client.get_stream_status_name(stream_id)
print(f"当前状态: {status_name}")  # "连接中" / "已连接" / "无法连接" / "不存在"
```

---

### 6. ListStreams - 查询所有流

查询服务端所有流的信息。

**请求参数：** 无

**响应：**
| 字段 | 类型 | 说明 |
|------|------|------|
| total_count | int32 | 流总数 |
| streams | StreamInfo[] | 流信息列表 |

**StreamInfo 结构：**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 流 ID |
| rtsp_url | string | RTSP 地址 |
| status | StreamStatus | 连接状态 |
| decoder_type | DecoderType | 解码器类型 |
| width | int32 | 视频宽度 |
| height | int32 | 视频高度 |
| decode_interval_ms | int32 | 解码间隔 |

**Python 示例：**
```python
# 获取所有流信息
streams = client.list_streams()
print(f"流总数: {len(streams)}")

for s in streams:
    print(f"[{s['stream_id'][:8]}...]")
    print(f"  URL: {s['rtsp_url']}")
    print(f"  状态: {s['status_name']}")
    print(f"  分辨率: {s['width']}x{s['height']}")

# 获取流数量
count = client.get_stream_count()
```

---

## 完整示例

```python
import cv2
import time
from remote_capture import (
    RemoteCapture,
    DECODER_GPU_CUDA,
    STATUS_CONNECTED,
    STATUS_NAMES
)

# 连接服务器
with RemoteCapture('127.0.0.1:50051') as client:
    
    # 1. 查看现有流
    print(f"当前流数量: {client.get_stream_count()}")
    
    # 2. 启动新流
    stream_id = client.start_stream(
        'rtsp://admin:password@192.168.1.100:554/stream',
        decoder_type=DECODER_GPU_CUDA
    )
    
    # 3. 等待连接
    for _ in range(10):
        if client.get_stream_status(stream_id) == STATUS_CONNECTED:
            break
        time.sleep(1)
    
    # 4. 获取流信息
    info = client.check_stream(stream_id)
    print(f"分辨率: {info['width']}x{info['height']}")
    
    # 5. 流式获取视频
    for ret, frame in client.stream_frames(stream_id, max_fps=15):
        if ret:
            cv2.imshow('Video', frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
    
    # 6. 停止流
    client.stop_stream(stream_id)
```

---

## 调试工具

使用 grpcui 进行接口测试：
```bash
grpcui -plaintext 127.0.0.1:50051
```
