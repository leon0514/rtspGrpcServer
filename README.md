# RTSP gRPC 服务器

本项目是一个 C++ 实现的 **RTSP 流媒体服务器**，通过 gRPC 接口向客户端提供视频流服务。

---

## 特性

- **GPU 硬件加速**：支持 NVIDIA CUDA 硬件解码和 NVJPEG 编码
- **多解码器支持**：CPU (FFmpeg) / GPU (NVCUVID) 可选
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
docker 启动
```bash
docker run -itd \
  --gpus '"device=1"' \
  -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video \
  --name grpc_rtsp_server \
  -p 50051:50051 \
  --ipc=host \
  --shm-size=2g \
  grpc_rtsp_server
```

#### 核心参数原理解析：
    --ipc=host （最关键）
    作用：打破 Docker 的进程间通信（IPC）隔离，让容器直接使用宿主机的 /dev/shm 目录。
    效果：C++ 在容器内调用 shm_open("/1732b0a8", ...)，宿主机的 /dev/shm/1732b0a8 会立刻出现这个文件。你的 Python 脚本（如果在宿主机运行）就能顺利 mmap 到它。
    --shm-size=2g
    作用：将共享内存上限从可怜的 64MB 提高到 2GB（你可以根据摄像头数量自行调整，如 1g, 4g 等）。
    效果：防止 C++ 服务端在多路并发解码时，调用 ftruncate 申请物理内存空间失败导致程序闪退。



## 枚举定义

### 解码器类型 (DecoderType)
| 值 | 名称 | 说明 |
|----|------|------|
| 0 | DECODER_CPU_FFMPEG | FFmpeg 软解 |
| 1 | DECODER_GPU_NVCUVID | NVIDIA CUDA 硬解 |

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

### 6. UpdateStream - 修改流地址

动态修改正在运行的流地址，无需停止并重新创建流任务

**请求参数**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 现有的流 ID |
| new_rtsp_url | string | 新的 RTSP 地址 |

** 响应 **
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否更新成功 |
| message | string | 错误或成功消息 |

**使用示例**  
在流式传输过程中直接切换 URL：
```python
# 示例：在接收帧的过程中动态切换地址
for ret, frame in client.stream_frames(stream_id, max_fps=10):
    if ret:
        frame_count += 1
        # 假设在第 50 帧时切换到备用摄像头
        if frame_count == 50:
            new_url = "rtsp://admin:password@172.16.22.16:554/Streaming/Channels/101"
            print(f"\n[Action] 正在切换流地址至: {new_url}")
            
            # 调用更新接口
            if client.update_stream_url(stream_id, new_url):
                print("切换指令已送达，等待底层重新连接...")
            else:
                print("切换失败")

        cv2.imshow("Live Stream", frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

```

### 7. ListStreams - 查询所有流

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
    DECODER_CPU_FFMPEG,
    DECODER_GPU_NVCUVID,
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


## 使用共享内存
```python
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
```