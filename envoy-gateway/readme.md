# RTSPStreamService API 详细文档

## 1. API 路径与操作列表

| 路径 | 方法 | 操作 ID | 描述 |
| :--- | :--- | :--- | :--- |
| `/v1/streams` | `GET` | `RTSPStreamService_ListStreams` | 列出所有流 |
| `/v1/streams/start` | `POST` | `RTSPStreamService_StartStream` | 启动流服务 |
| `/v1/streams/stop` | `POST` | `RTSPStreamService_StopStream` | 停止流服务 |
| `/v1/streams/{streamId}` | `PUT` | `RTSPStreamService_UpdateStream` | 更新流地址 |
| `/v1/streams/{streamId}/frame` | `GET` | `RTSPStreamService_GetLatestFrame` | 获取最新帧 |
| `/v1/streams/{streamId}/status` | `GET` | `RTSPStreamService_CheckStream` | 检查流状态 |
| `/v1/streams/{streamId}/stream` | `GET` | `RTSPStreamService_StreamFrames` | 流式传输帧数据 |

---

## 2. 详细请求参数与响应体模型

### 2.1 启动与管理请求模型
| 模型名称 | 字段名 | 类型 | 必填 | 描述 |
| :--- | :--- | :--- | :--- | :--- |
| **StartRequest** | `rtspUrl` | string | 是 | RTSP 流媒体源地址 |
| | `heartbeatTimeoutMs` | integer | 否 | 心跳超时时间 (ms) |
| | `decodeIntervalMs` | integer | 否 | 解码间隔 (ms) |
| | `decoderType` | enum | 否 | `DECODER_CPU_FFMPEG` / `DECODER_GPU_NVCUVID` |
| | `gpuId` | integer | 否 | GPU ID |
| | `keepOnFailure` | boolean | 否 | 出错后是否维持状态 |
| | `useSharedMem` | boolean | 否 | 是否启用共享内存 |
| | `onlyKeyFrames` | boolean | 否 | 是否仅抓取关键帧 |
| **StopRequest** | `streamId` | string | 是 | 需要停止的流 ID |
| **UpdateBody** | `newRtspUrl` | string | 是 | 更新后的目标 RTSP URL |

### 2.2 响应模型 (详细结构)
| 模型名称 | 字段名 | 类型 | 描述 |
| :--- | :--- | :--- | :--- |
| **ListStreamsResponse** | `totalCount` | integer | 流总数 |
| | `streams` | array | `streamingserviceStreamInfo` 列表 |
| **StreamInfo** | `streamId` | string | 流 ID |
| | `rtspUrl` | string | 流 URL |
| | `status` | enum | 状态 (`STATUS_...`) |
| | `decoderType` | enum | 解码器类型 |
| | `width` | integer | 图像宽 |
| | `height` | integer | 图像高 |
| | `decodeIntervalMs` | integer | 解码间隔 |
| **FrameResponse**| `success` | boolean | 操作成功标志 |
| | `imageData` | string | Base64 格式的图像二进制 |
| | `message` | string | 状态或错误描述 |
| | `frameSeq` | integer | 图像帧序列号 |
| **CheckResponse** | `status` | enum | 当前连接状态 |
| | `message` | string | 状态消息 |
| | `rtspUrl` | string | 流 URL |
| | `decoderType` | enum | 解码器类型 |
| | `width` | integer | 宽 |
| | `height` | integer | 高 |
| | `decodeIntervalMs` | integer | 解码间隔 |

---

## 3. 标准错误对象 (rpcStatus)
所有接口的 `default` 响应均为该模型：

| 属性名 | 类型 | 描述 |
| :--- | :--- | :--- |
| `code` | integer | 错误代码 (int32) |
| `message` | string | 错误信息描述 |
| `details` | array | 详细错误集合 (protobufAny 对象) |

---

## 4. 枚举值定义 (Enum)

### 4.1 流状态 (`streamingserviceStreamStatus`)
| 枚举值 | 说明 |
| :--- | :--- |
| `STATUS_CONNECTING` | 正在连接中 |
| `STATUS_CONNECTED` | 已成功连接 |
| `STATUS_DISCONNECTED` | 连接已中断 |
| `STATUS_NOT_FOUND` | 未找到指定的流 |

### 4.2 解码器类型 (`streamingserviceDecoderType`)
| 枚举值 | 说明 |
| :--- | :--- |
| `DECODER_CPU_FFMPEG` | CPU 软件解码 (默认) |
| `DECODER_GPU_NVCUVID` | NVIDIA 硬件加速解码 |

---

## 5. 特殊接口说明：StreamFrames (`/v1/streams/{streamId}/stream`)
该接口为流式响应，其返回结构如下：

| 结构层级 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| **Root** | `result` | object | `streamingserviceFrameResponse` 实例 |
| **Root** | `error` | object | `rpcStatus` 实例 |

* **注意**：该接口同时支持 `maxFps` (integer, query parameter) 用于控制下行帧率，单位为帧/秒。文档中提到的 `Chunked response` 意味着客户端需要持续读取 HTTP 响应体流，直到连接关闭或发生错误。