# RTSP gRPC 服务器

本项目是一个 C++ 实现的 **RTSP 流媒体服务器**，通过 gRPC 接口向客户端提供视频流服务。

---

## 特性

- **GPU 硬件加速**：支持 NVIDIA CUDA 硬件解码和 NVJPEG 编码
- **多解码器支持**：CPU (FFmpeg) / GPU (NVCUVID) / 海康 SDK 可选
- **海康 SDK 直接抓图**：支持 `hik://` URL 与 `DECODER_HIK_SDK`，IPC  JPEG 原图透传
- **RTSP ↔ 海康动态切换**：运行中通过 `UpdateStream` 在 RTSP 与海康 SDK 间无缝切换
- **流式传输**：gRPC 服务端流式推送，低延迟实时传输
- **多客户端共享**：单路解码，多客户端零拷贝共享
- **灵活帧率控制**：支持解码间隔和客户端独立帧率限制
- **流状态管理**：连接中 / 已连接 / 无法连接 / 不存在

---

## 依赖项

- C++17、CMake 3.10+、gRPC、Protobuf、OpenCV 4.x
- CUDA Toolkit（GPU 解码需要）
- 海康 SDK 头文件与库（`sdk/hikvision/hik_header/`、`sdk/hikvision/hik_libs/`）
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
    --ipc=host （关键）
    作用：开启容器与宿主机的 IPC 共享命名空间，使 POSIX 共享内存文件对宿主机可见。
    效果：C++ 在容器内调用 shm_open("/1732b0a8", ...)，宿主机的 /dev/shm/1732b0a8 会立刻出现这个文件。你的 Python 脚本（如果在宿主机运行）就能顺利 mmap 到它。
    --shm-size=2g
    作用：将容器内 /dev/shm 的上限从默认 64MB 提高到 2GB（你可以根据摄像头数量自行调整，如 1g, 4g 等）。
    效果：防止 C++ 服务端在多路并发解码时，调用 ftruncate 申请物理内存空间失败导致程序闪退。

#### Docker 镜像构建注意事项
修改 C++ 服务端代码后，必须使用 `--no-cache` 重新构建镜像，否则 Dockerfile 可能复用旧的 `build/rtsp_server` 缓存层：
```bash
cd build && cmake .. && make -j && cd ..
docker build --no-cache -t grpc_rtsp_server .
```



## 枚举定义

### 解码器类型 (DecoderType)
| 值 | 名称 | 说明 |
|----|------|------|
| 0 | DECODER_CPU_FFMPEG | FFmpeg 软解 |
| 1 | DECODER_GPU_NVCUVID | NVIDIA CUDA 硬解 |
| 2 | DECODER_HIK_SDK | 海康 SDK 直接抓图（JPEG 透传） |

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
| rtsp_url | string | RTSP 地址或海康 URL（见下方海康 SDK 说明） |
| heartbeat_timeout_ms | int32 | 心跳超时（毫秒），0 表示不超时 |
| decode_interval_ms | int32 | 解码间隔（毫秒），0 表示不限制 |
| decoder_type | DecoderType | 解码器类型 |
| gpu_id | int32 | GPU ID（仅 GPU 解码有效） |
| keep_on_failure | bool | 打开失败时是否保留任务 |
| use_shared_mem | bool | 是否同时写入 POSIX 共享内存 |
| only_key_frames | bool | 是否只解码/抓取关键帧 |


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
from remote_capture import RemoteCapture, DECODER_GPU_NVCUVID, DECODER_HIK_SDK

# 客户端默认连接到 127.0.0.1:50051，可通过环境变量 GRPC_SERVER 或在构造中指定其它地址
server_addr = os.getenv('GRPC_SERVER', '127.0.0.1:50051')
client = RemoteCapture(server_addr)
client.connect()

# 方式 1：普通 RTSP
stream_id = client.start_stream(
    rtsp_url='rtsp://admin:password@192.168.1.100:554/stream',
    decoder_type=DECODER_GPU_NVCUVID,
    heartbeat_timeout_ms=30000,
    decode_interval_ms=100,
    gpu_id=0,
    keep_on_failure=False,
    use_shared_mem=False,
    only_key_frames=False
)

# 方式 2：海康 SDK 直接抓图（推荐：通过 hik:// URL 自动解析）
stream_id_hik = client.start_stream(
    rtsp_url='hik://admin:password@192.168.1.100:8000/channel/1',
    decoder_type=DECODER_HIK_SDK,
    heartbeat_timeout_ms=30000
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
    frame_seq, frame = client.read(stream_id)
    if frame_seq != -1:
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
for frame_seq, frame in client.stream_frames(stream_id, max_fps=15):
    if frame_seq != -1:
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

动态修改正在运行的流地址，无需停止并重新创建流任务。支持在同一 `stream_id` 下：
- RTSP URL ↔ RTSP URL 切换
- RTSP URL ↔ 海康 `hik://` URL 切换（服务端自动选择 `DECODER_HIK_SDK`）
- 切换时自动清空旧帧缓存，避免首帧残留

**请求参数**
| 字段 | 类型 | 说明 |
|------|------|------|
| stream_id | string | 现有的流 ID |
| new_rtsp_url | string | 新的 RTSP 地址或海康 URL |

** 响应 **
| 字段 | 类型 | 说明 |
|------|------|------|
| success | bool | 是否更新成功 |
| message | string | 错误或成功消息 |

**使用示例 1：RTSP 之间切换**  
在流式传输过程中直接切换 URL：
```python
# 示例：在接收帧的过程中动态切换地址
for frame_seq, frame in client.stream_frames(stream_id, max_fps=10):
    if frame_seq != -1:
        # 假设在第 50 帧时切换到备用摄像头
        if frame_seq == 50:
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

**使用示例 2：RTSP ↔ 海康 SDK 切换**
```python
from remote_capture import DECODER_GPU_NVCUVID, DECODER_HIK_SDK

rtsp_url = "rtsp://admin:password@192.168.1.100:554/stream"
hik_url = "hik://admin:password@192.168.1.100:8000/channel/1"

with RemoteCapture('127.0.0.1:50051') as client:
    # 1. 先用 RTSP 启动，保存解码参数供后续恢复
    stream_id = client.start_stream(rtsp_url, decoder_type=DECODER_GPU_NVCUVID, gpu_id=0)

    # 2. 切换到海康 SDK
    client.update_stream_url(stream_id, hik_url)

    # 海康抓图比 RTSP 慢，建议先阻塞等待第一帧真正到达
    for _ in range(50):
        ts, frame = client.read(stream_id)
        if frame is not None:
            break
        time.sleep(0.1)

    # 3. 切回 RTSP（服务端自动恢复为启动时保存的解码参数）
    client.update_stream_url(stream_id, rtsp_url)
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
        decoder_type=DECODER_GPU_NVCUVID
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
    for frame_seq, frame in client.stream_frames(stream_id, max_fps=15):
        if frame_seq != -1:
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

---

## 海康 SDK 使用说明

### URL 格式

除通过 `StartRequest` 的 `hik_*` 字段直接指定外，也可以使用统一 URL 自动触发海康 SDK：

```
hik://<用户名>:<密码>@<IP>:<端口>/channel/<通道号>
```

示例：
```
hik://admin:pass@192.168.1.100:8000/channel/1
```

服务端识别到 `hik://` 协议后会强制使用 `DECODER_HIK_SDK`，无需客户端额外指定解码器类型。

### 启动方式

统一通过 `hik://` URL 传参：
```python
stream_id = client.start_stream(
    rtsp_url='hik://admin:pass@192.168.1.100:8000/channel/35',
    decoder_type=DECODER_HIK_SDK
)
```

### 性能特点

- **JPEG 透传**：海康 SDK 抓取的 JPEG 原图直接通过 gRPC 返回，不做二次编解码，CPU/GPU 占用低。
- **按需解码**：`grab()` 仅解析 JPEG 头获取宽高，`retrieve()` 仅在需要 BGR 帧时完整解码。
- **抓图延迟**：海康 SDK 抓图依赖 `ForceIFrame` + `Capture`，首帧通常需要 100~300ms。客户端切换 URL 后建议先阻塞等待第一帧到达，再按业务帧率读取。

### 常见问题

| 现象 | 可能原因 | 排查方法 |
|------|----------|----------|
| 切换到海康后连续 read() 无帧 | 海康抓图慢，客户端读太快 | 切换后先等待首帧，读取间隔建议 ≥ 200ms |
| 首帧是切换前的旧图 | 设备或 SDK 缓存 | 服务端 `open()` 后会丢弃第一帧并重新抓图 |
| 只能读到 1~2 帧 | 文件名按时间戳保存冲突 | 保存文件时增加索引前缀，如 `hik_frame_{i:03d}_{timestamp}.jpg` |
| Docker 中抓图失败 | 未挂载海康 SDK 库或权限不足 | 检查 `entrypoint.sh` 软链接、`/dev/shm` 权限及 `--ipc=host` |

### 日志排查

服务端 `HikDecoder` 关键日志已输出为 `info` 级别，启动/切换后可查看：

```bash
docker logs grpc_rtsp_server | grep -i hik
```

预期输出包含：
- `HikDecoder opened` / `login success`
- `ForceIFrame` / `Capture` 成功/失败
- `Captured second frame ... bytes`
- `getEncodedFrame` / `retrieve` 调用情况

---

## 编译与镜像

### 本地编译

```bash
mkdir build && cd build
cmake .. && make -j
./rtsp_server [address:port]   # 默认 0.0.0.0:50051
```

### Docker 镜像（推荐生产环境）

> 修改 C++ 代码后必须 `--no-cache` 构建，否则 `Dockerfile` 中的 `COPY build/rtsp_server` 可能复用旧缓存。

```bash
cd build && cmake .. && make -j && cd ..
docker build --no-cache -t grpc_rtsp_server .
```

启动：
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
