以下是对 `/mnt/leon/workspace/project/cpp/rtspGrpcServer` 中 7 个基础组件的代码审查结果。按文件列出，标注严重程度和具体行号/代码片段。

> 说明：`frame_memory_pool`、`task_scheduler`、`thread_pool` 都是**仅头文件实现，没有对应的 `.cpp`**。

---

## 1. `include/zero_copy_channel.hpp`（共享内存通道）

### 严重/中高

#### 1.1 头文件不自包含：使用了 `cv::Mat` 但未包含 OpenCV
- **位置**：`write_frame_mat` 使用 `cv::Mat`（第 150 行），但头文件未 `#include <opencv2/opencv.hpp>`。
- **影响**：如果某个 `.cpp` 先于本头文件之前没有 include OpenCV，会编译失败。
- **建议**：在头文件顶部显式 include。

```cpp
// 第 150 行
bool write_frame_mat(const cv::Mat& frame, uint64_t timestamp)
```

#### 1.2 缺少 `<stdexcept>` / `<cerrno>`
- **位置**：第 61 行使用 `std::runtime_error`，第 61/169 行使用 `errno`。
- **影响**：依赖其他头文件间接 include，可能导致在不同编译器/环境下编译失败。
- **建议**：显式 `#include <stdexcept>` 和 `#include <cerrno>`。

```cpp
// 第 61 行
throw std::runtime_error("shm_open failed for " + shm_path + ": " + std::to_string(errno));
```

#### 1.3 生产者创建 SHM 时未清除 `umask`
- **位置**：第 58 行 `shm_open(..., O_CREAT | O_RDWR, 0666)`。
- **影响**：如果进程 `umask` 不为 0（常见为 `022`），实际权限可能是 `0644`，导致跨用户/跨容器的消费者无法访问。
- **建议**：像信号量一样临时清除 `umask`。

```cpp
// 第 58 行（仅信号量做了 umask 处理，SHM 没有）
shm_fd_ = shm_open(shm_path.c_str(), O_CREAT | O_RDWR, 0666);
```

#### 1.4 析构函数不 `shm_unlink`，存在共享内存对象泄漏风险
- **位置**：第 132-148 行析构函数。
- **影响**：只有在 `cleanup()` 被显式调用时才会 `shm_unlink`。如果某个异常路径导致对象被直接 `delete`/`reset` 而未调 `cleanup()`，`/dev/shm/<stream_id>` 会残留。
- **建议**：生产者在析构时也应 `shm_unlink`，或让 `cleanup()` 在析构中幂等地调用。

```cpp
// 第 132-148 行
~ZeroCopyChannel()
{
    if (layout_) munmap(layout_, sizeof(ShmLayout));
    if (shm_fd_ >= 0) close(shm_fd_);
    // 缺少对生产者的 shm_unlink
    ...
}
```

#### 1.5 `cleanup()` 不区分角色，消费者调用会误删 SHM/信号量
- **位置**：第 249-271 行。
- **影响**：`cleanup()` 无条件 `shm_unlink` 和 `sem_unlink`。虽然当前只有生产者调用，但接口上消费者也可调用，导致其他进程使用的共享内存被提前删除。
- **建议**：`if (role_ == 0)` 再执行 unlink。

```cpp
// 第 262 行
shm_unlink(("/" + stream_id_).c_str());   // 应仅在 role_ == 0 时执行
```

#### 1.6 类没有禁用拷贝/移动
- **位置**：第 46-50 行类声明。
- **影响**：默认拷贝会导致 `layout_`/`shm_fd_`/`notify_sem_` 被复制，造成 double-close/double-munmap。
- **建议**：添加 `ZeroCopyChannel(const ZeroCopyChannel&) = delete;` 等。

### 中低/轻微

#### 1.7 `MAX_SHM_FRAME_SIZE` 注释与实际值不符
- **位置**：第 14 行。
- `6 * 2560 * 1440 ≈ 21 MB`，注释却写 `3MB`；第 38 行注释也算错了。

```cpp
constexpr size_t MAX_SHM_FRAME_SIZE = 6  * 2560 * 1440; // 3MB   // 实际约 21MB
```

#### 1.8 `write_frame()` 写入时未清零/设置 `channels/depth/step`
- **位置**：第 212-247 行。
- 该函数只写了 `actual_size/width/height/timestamp`，如果与 `write_frame_mat()` 混用，`meta` 中的 `channels/depth/step` 可能是上一帧的脏数据。
- 当前代码中 `write_frame()` 似乎未被调用，但接口存在隐患。

#### 1.9 写帧前 `sequence.fetch_add(1, acquire)` 的语义
- 第 179/222 行用 `memory_order_acquire` 标记“写入中”。
- 更常见的是用 `memory_order_release`（或 `relaxed` 亦可），因为这里是要发布“我开始写”的信号。虽然不会导致功能错误，但语义上不够清晰。

---

## 2. `include/frame_memory_pool.hpp`（帧内存池）

### 中高

#### 2.1 `acquire()` 依赖 `shared_from_this()`，但类未提供安全的创建方式
- **位置**：第 18-52 行，第 41 行 `std::weak_ptr<FrameMemoryPool> weak_pool = shared_from_this();`。
- **影响**：如果用户把 `FrameMemoryPool` 创建在栈上或裸指针上，`acquire()` 会抛 `std::bad_weak_ptr`。
- **建议**：提供 `static std::shared_ptr<FrameMemoryPool> create(...)` 工厂，并把构造函数设为 `private` 或至少文档化。

### 中

#### 2.2 自定义删除器可能因 `release()` 抛异常而 `std::terminate`
- **位置**：第 44-51 行删除器调用 `pool->release(...)`，第 56-64 行 `release()` 内部 `pool_.push_back` 可能抛 `std::bad_alloc`。
- **影响**：在 `shared_ptr` 析构时抛异常会导致 `std::terminate`。
- **建议**：在删除器中捕获所有异常。

```cpp
return std::shared_ptr<std::string>(str.release(), [weak_pool](std::string *ptr) {
    try {
        if (auto pool = weak_pool.lock()) {
            pool->release(std::unique_ptr<std::string>(ptr));
        } else {
            delete ptr;
        }
    } catch (...) {
        delete ptr;   // 兜底
    }
});
```

### 低

#### 2.3 池大小硬编码为 3，且无预热
- 第 60 行 `if (pool_.size() < 3)`。
- 不是问题，但可配置性较差。

---

## 3. `include/task_scheduler.hpp`（任务调度器）

> 只有头文件，无 `.cpp`。

### 严重

#### 3.1 双重检查锁定（DCL）使用非原子对象，存在数据竞争 + UB
- **位置**：第 34-45 行。
- **影响**：`gpu_compute_pools_[gpu_id]` 是 `std::unique_ptr<ThreadPool>`，不是原子类型。外层 `if (!gpu_compute_pools_[gpu_id])` 与内层 `gpu_compute_pools_[gpu_id] = std::make_unique<...>` 之间没有同步，属于 C++ 标准意义上的 data race，行为未定义。

```cpp
// 第 34-45 行
if (!gpu_compute_pools_[gpu_id])              // 无锁读非原子对象
{
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (!gpu_compute_pools_[gpu_id])
    {
        gpu_compute_pools_[gpu_id] = std::make_unique<ThreadPool>(threads_per_gpu_);
    }
}
return *gpu_compute_pools_[gpu_id];
```

- **建议**：
  - 方案 A：在 `init()` 中直接创建所有 GPU 池，去掉懒加载。
  - 方案 B：使用 `std::call_once` 或 `std::atomic<std::shared_ptr<ThreadPool>>`。
  - 方案 C：每个 GPU 配一个 `std::once_flag`。

### 严重

#### 3.2 `getComputePool()` 返回的 CPU 池可能为 `nullptr`
- **位置**：第 22-25 行 `return *cpu_compute_pool_;`。
- **影响**：如果 `init()` 没被调用或 `shutdown()` 之后调用，会直接解引用空指针。
- **建议**：增加空指针检查，或确保调用顺序。

### 中

#### 3.3 `init()` 不是线程安全的，且可被重复调用
- **位置**：第 49-64 行。
- **影响**：多次调用会覆盖 `cpu_compute_pool_`、重新 resize `gpu_compute_pools_`，可能导致旧池泄漏或正在使用的引用失效。
- **建议**：使用 `std::once_flag` 或 `std::atomic<bool> initialized_` 保护。

### 中

#### 3.4 `shutdown()` 与 `getComputePool()` 并发不安全
- **位置**：第 68-74 行。
- **影响**：`shutdown()` 会 `clear()` vector，而 `getComputePool()` 可能在无锁情况下访问 vector 元素或索引，导致越界/悬空引用。
- **建议**：所有读写 `gpu_compute_pools_`/`cpu_compute_pool_` 的操作都应在 `init_mutex_` 保护下，或确保 `shutdown()` 只在完全无任务时调用。

---

## 4. `include/thread_pool.hpp` / `src/thread_pool.cpp`（线程池）

> 只有头文件 `include/thread_pool.hpp`，无 `.cpp`。

### 严重

#### 4.1 缺少头文件保护
- **位置**：第 1 行。
- 没有 `#pragma once`，也没有 include guard。如果某个翻译单元直接或间接 include 两次，会报重定义错误。

### 严重

#### 4.2 析构函数存在“丢失唤醒”死锁风险
- **位置**：第 56-66 行。
- **影响**：`running_` 是 `atomic`，`notify_all()` 在锁外执行。工作线程可能在“检查 `running_` 仍为 true”之后、“真正进入 `wait`”之前丢失唤醒，导致 `join()` 永远阻塞。

```cpp
// 第 56-66 行
~ThreadPool()
{
    running_ = false;      // 在锁外修改
    cv_.notify_all();      // 在锁外通知
    for (auto &thread : threads_)
        if (thread.joinable()) thread.join();
}
```

- **建议**：在持有 `mtx_` 的情况下设置 `running_ = false` 并 `notify_all()`。

### 中

#### 4.3 `enqueue()` 在停止后仍可接受任务，导致任务丢失
- **位置**：第 73-88 行。
- **影响**：`enqueue()` 不检查 `running_`。如果在线程池析构期间或之后调用，任务会被加入队列但不会被处理，且析构后 `tasks_` 中残留的任务会随着 `ThreadPool` 销毁而释放，任务实际未执行。
- **建议**：`enqueue()` 中检查 `running_`，停止后拒绝新任务或抛异常。

### 中

#### 4.4 `std::bind` 无法完美转发非拷贝参数
- **位置**：第 81-82 行。
- 如果传入 `std::unique_ptr` 等不可拷贝参数，`std::bind` 会编译失败。当前项目可能没有这种用法，但接口能力受限。

---

## 5. `include/timer_scheduler.hpp` / `src/timer_scheduler.cpp`（定时器调度器）

### 严重

#### 5.1 `runLoop()` 中任务抛异常会导致定时器线程直接退出
- **位置**：第 65-69 行。
- **影响**：`for (auto &t : ready) t();` 没有 try-catch。如果某个定时任务抛异常，`runLoop()` 异常退出，后续所有定时任务都不再执行，且 `stop()` 的 `join()` 可能行为异常。

```cpp
// timer_scheduler.cpp:65-69
for (auto &t : ready)
{
    t();   // 未捕获异常
}
```

- **建议**：在循环内捕获 `std::exception` 和 `...` 并记录日志。

### 严重

#### 5.2 `stop()` 同样存在“丢失唤醒”风险
- **位置**：第 22-29 行。
- **影响**：`running_.exchange(false)` 和 `cv_.notify_one()` 都不在 `mutex_` 保护下。`runLoop()` 的 `cv_.wait(lock)`（第 48 行）或 `cv_.wait_until`（第 74 行）可能丢失唤醒，导致 `stop()` 的 `join()` 死锁。
- **建议**：在持有 `mutex_` 时修改 `running_` 并通知；或者把所有 `wait` 都改成带谓词的形式。

### 中

#### 5.3 `cv_.wait(lock)` 没有谓词
- **位置**：第 48 行。
- 虽然循环会重新检查 `tasks_.empty()`，但丢失唤醒时可能长时间阻塞。结合 5.2，退出时可能挂起。

### 低

#### 5.4 注释与实现不符
- **位置**：`timer_scheduler.hpp` 第 26-27 行。
- 注释说“task 将在 IO 线程池中执行（通过 TaskScheduler）”，但实际是在 `TimerScheduler` 自己的线程里直接执行。

---

## 6. `include/cuda_tools.hpp` / `src/cuda_tools.cpp`（CUDA 工具）

### 严重

#### 6.1 `AutoDevice` 构造函数忽略 `cudaGetDevice` 返回值
- **位置**：`cuda_tools.cpp` 第 66-71 行。
- **影响**：如果 `cudaGetDevice(&old_)` 失败（例如 CUDA 未初始化），`old_` 是未初始化值。析构时 `cudaSetDevice(old_)` 可能设置一个非法设备。

```cpp
// cuda_tools.cpp:66-71
AutoDevice::AutoDevice(int device_id)
{
    cudaGetDevice(&old_);            // 返回值未检查
    checkCudaRuntime(cudaSetDevice(device_id));
}
```

- **建议**：检查返回值，失败时将 `old_` 设为 0 或抛异常。

### 中

#### 6.2 `grid_dims(0)` 会导致除零 / 非法 grid
- **位置**：`cuda_tools.cpp` 第 55-59 行。
- 当 `numJobs == 0` 时，`numBlockThreads = 0`，随后 `(0 + 0 - 1) / 0.0f` 是除零，转换为 `dim3` 后可能产生巨大值，导致内核启动失败或异常。

```cpp
// cuda_tools.cpp:55-59
dim3 grid_dims(int numJobs)
{
    int numBlockThreads = numJobs < GPU_BLOCK_THREADS ? numJobs : GPU_BLOCK_THREADS;
    return dim3(((numJobs + numBlockThreads - 1) / (float)numBlockThreads));
}
```

- **建议**：对 `numJobs <= 0` 做保护。

### 中

#### 6.3 `Assert` 宏只记录日志，不终止程序
- **位置**：`cuda_tools.hpp` 第 33-41 行。
- `INFOF` 只是记录 Fatal 级别日志（见 `simple-logger.hpp`），不会 `abort` 或抛异常。名字为 `Assert` 但实际起不到断言作用，可能导致错误继续执行。

```cpp
#define Assert(op)                        \
    do                                    \
    {                                     \
        bool cond = !(!(op));             \
        if (!cond)                        \
        {                                 \
            INFOF("Assert failed, " #op); \
        }                                 \
    } while (false)
```

- **建议**：改为 `std::abort()` 或抛异常。

### 中

#### 6.4 `check_driver` 可能使用空指针调用 `%s`
- **位置**：`cuda_tools.cpp` 第 16-20 行。
- `cuGetErrorString`/`cuGetErrorName` 的返回指针在未知错误码时可能为 `nullptr`，直接传给 `%s` 是未定义行为。

---

## 7. `include/decoder_factory.hpp` / `src/decoder_factory.cpp`（解码器工厂）

### 中

#### 7.1 `default` 分支创建的 `CpuDecoder` 丢失了 `only_key_frames` 参数
- **位置**：`decoder_factory.cpp` 第 16-17 行。
- **影响**：当传入不认识的 `DecoderType` 时，回退到 CPU 解码器，但忽略了 `only_key_frames`，行为不一致。

```cpp
// decoder_factory.cpp:16-17
default:
    return std::make_unique<CpuDecoder>();   // 应改为 CpuDecoder(only_key_frames)
```

- **建议**：改为 `return std::make_unique<CpuDecoder>(only_key_frames);`。

### 低

#### 7.2 对 `gpu_id` 没有合法性校验
- **位置**：`decoder_factory.cpp` 第 13-14 行。
- 非法 `gpu_id` 会一直传递到 `CudaDecoder`，直到 `open()` 时才失败。可以在工厂层通过 `CUDATools::check_device_id` 提前检查。

### 低

#### 7.3 默认分支静默回退
- 对于非法枚举直接回退到 CPU 解码器。如果这是预期行为，建议加日志；如果是错误，应抛异常或返回 `nullptr`。

---

## 总结（最需要优先修复的问题）

| 优先级 | 文件 | 问题 |
|--------|------|------|
| 高 | `task_scheduler.hpp` | DCL 使用非原子对象，data race/UB |
| 高 | `thread_pool.hpp` | 析构丢失唤醒可能导致死锁；缺少头文件保护 |
| 高 | `timer_scheduler.cpp` | 任务异常导致线程退出；`stop()` 丢失唤醒 |
| 高 | `zero_copy_channel.hpp` | 头文件不自包含；SHM 权限/泄漏；析构不 `shm_unlink` |
| 中 | `cuda_tools.cpp` | `AutoDevice` 未检查错误；`grid_dims(0)` 除零 |
| 中 | `decoder_factory.cpp` | `default` 分支丢失 `only_key_frames` |
| 中 | `frame_memory_pool.hpp` | 删除器可能因 `release()` 抛异常而 `terminate` |

以上问题大多集中在**多线程同步、资源生命周期、头文件自包含**三方面，建议优先修复。