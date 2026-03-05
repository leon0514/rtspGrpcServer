#include "stream_task.hpp"
#include "task_scheduler.hpp"
#include "timer_scheduler.hpp"
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>
#include "cuda_tools.hpp"

StreamTask::StreamTask(const std::string &url,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       int gpu_id,
                       bool keep_on_failure,
                       std::unique_ptr<IVideoDecoder> decoder,
                       std::shared_ptr<IImageEncoder> encoder)
    : url_(url),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
      gpu_id_(gpu_id),
      keep_on_failure_(keep_on_failure),
      decoder_(std::move(decoder)),
      encoder_(std::move(encoder))
{
    updateHeartbeat();
    last_encode_time_ = std::chrono::steady_clock::now();
}

StreamTask::~StreamTask()
{
    stop();
    // 再次清理数据，确保对象销毁后不保留大容量缓存
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        std::string().swap(latest_encoded_frame_);
    }
}

// ---------------------------------------------------------
// 可中断的休眠函数，取代死循环或阻塞式 sleep
// ---------------------------------------------------------
bool StreamTask::interruptibleSleep(int ms)
{
    if (ms <= 0)
        return true;

    std::unique_lock<std::mutex> lock(sleep_mutex_);
    // wait_for 会阻塞等待。如果返回 true，说明满足了 predicate (!running_)，即被外部 stop 中断了
    // 如果返回 false，说明是超时自然唤醒的
    bool stopped = sleep_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]()
                                      { return !running_.load(); });

    // 返回 true 表示可以继续执行 (未被 stop)
    return !stopped;
}

void StreamTask::start()
{
    // 防止重复启动
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
        return;
    }

    stopped_ = false;
    spdlog::info("StreamTask started: {}", url_);

    // 投递第一个 IO 任务
    auto self = shared_from_this();
    TaskScheduler::instance().getIOPool().enqueue([self]()
                                                  { self->stepIO(); });
}

void StreamTask::stop()
{
    // CAS 操作将 running_ 置为 false
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false))
    {
        spdlog::info("StreamTask stopping: {}", url_);
        stopped_ = true;

        // 【关键优化】唤醒所有正在 condition_variable 上等待的休眠，使其立即退出，不卡顿
        {
            std::lock_guard<std::mutex> sleep_lock(sleep_mutex_);
            sleep_cv_.notify_all();
        }

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            frame_cv_.notify_all(); // 唤醒客户端，让其安全退出
        }

        // 加锁确保当前没有正在进行的 stepIO/stepCompute 操作 decoder
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_)
        {
            decoder_->release();
        }

        // 清空缓存的编码帧，释放内存
        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            std::string().swap(latest_encoded_frame_);
        }

        status_ = StreamStatus::DISCONNECTED;
        connected_ = false;
    }
}

// 调度辅助函数：决定是立即执行 IO，还是延迟执行 IO
void StreamTask::scheduleNext(int force_delay_ms)
{
    if (!running_)
        return;

    auto self = shared_from_this();

    if (force_delay_ms > 0)
    {
        // 使用定时器调度器处理长延时（如1000ms的重连），避免长时间占据线程池资源
        TimerScheduler::instance().schedule(force_delay_ms, [self]()
                                            {
            if (auto locked = self) {  
                if (locked->running_) {
                    TaskScheduler::instance().getIOPool().enqueue([self]() {
                        if (auto stillAlive = self) {
                            stillAlive->stepIO();
                        }
                    });
                }
            } });
    }
    else
    {
        // 立即投递到 IO 线程池
        TaskScheduler::instance().getIOPool().enqueue([self]()
                                                      {
            if (auto stillAlive = self) {
                stillAlive->stepIO();
            } });
    }
}

// ---------------------------------------------------------
// 阶段 1: IO 线程池中执行
// 负责：重连、av_read_frame (grab)
// ---------------------------------------------------------
void StreamTask::stepIO()
{
    if (!running_)
        return;

    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_)
        return;

    // 1. 检查连接状态 & 重连逻辑
    if (!decoder_->isOpened())
    {
        const int max_reconnect_attempts = 5;
        if (reconnect_attempts_ >= max_reconnect_attempts)
        {
            if (!keep_on_failure_)
            {
                spdlog::error("Max reconnect attempts reached. Stopping task: {}", url_);
                lock.unlock();
                stop();
                return;
            }
            reconnect_attempts_ = 0;
        }

        reconnect_attempts_++;
        status_ = StreamStatus::CONNECTING;
        connected_ = false;

        spdlog::warn("Attempting to open/reconnect {}/{}: {}", reconnect_attempts_, max_reconnect_attempts, url_);

        if (decoder_->open(url_))
        {
            spdlog::info("Connected successfully: {}", url_);
            status_ = StreamStatus::CONNECTED;
            reconnect_attempts_ = 0;
            last_encode_time_ = std::chrono::steady_clock::now();
        }
        else
        {
            lock.unlock();
            // 重连失败，使用 timer_scheduler 等待 1 秒（不阻塞当前 IO 线程）
            scheduleNext(1000);
            return;
        }
    }

    // 2. 抓取数据 (Network IO)
    if (!decoder_->grab())
    {
        spdlog::warn("Frame grab failed: {}", url_);
        connected_ = false;
        decoder_->release();

        lock.unlock();
        // 【优化】抓流失败时，休眠几十毫秒再重试，防止本地文件/坏流导致的死循环疯狂投递拉满 CPU
        if (interruptibleSleep(50))
        {
            scheduleNext(0);
        }
        return;
    }

    connected_ = true;
    reconnect_attempts_ = 0;

    // 3. 帧率控制 (Decide whether to process)
    bool should_process = true;
    int64_t decode_interval_us = decode_interval_ms_ * 1000LL;

    if (decode_interval_us > 0)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_encode_time_).count();
        if (elapsed_us < decode_interval_us)
        {
            should_process = false;
        }
    }

    // 4. 根据判断决定走向
    if (should_process)
    {
        // 需要处理：将任务转移到【计算线程池】
        lock.unlock();

        auto self = shared_from_this();
        TaskScheduler::instance().getComputePool(gpu_id_).enqueue([self]()
                                                                  { self->stepCompute(); });
    }
    else
    {
        // 内部循环丢帧，减少线程池入队/出队开销
        // 最多连续在当前线程丢 5 帧，防止霸占 IO 线程太久导致其他视频流卡顿
        int drop_count = 0;
        while (!should_process && drop_count < 5 && running_)
        {
            cv::Mat dummy;
            decoder_->retrieve(dummy, false); // 消耗缓存中的帧
            dummy.release();                  // 释放占用的缓存
            // 休眠 2ms 等待下一帧到来，防止死循环
            if (!interruptibleSleep(2))
            {
                return; // 被 stop 打断
            }

            // 尝试抓取下一帧
            if (!decoder_->grab())
            {
                spdlog::warn("Frame grab failed during drop: {}", url_);
                connected_ = false;
                decoder_->release();
                lock.unlock();
                if (interruptibleSleep(50))
                    scheduleNext(0);
                return;
            }
            drop_count++;

            // 重新计算是否该处理了
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_encode_time_).count();
            if (elapsed_us >= decode_interval_us)
            {
                should_process = true;
            }
        }
        // 丢弃了几帧后，无论是否需要处理，都重新投递一次以让出当前线程
        lock.unlock();
        scheduleNext(0);
    }
}

// ---------------------------------------------------------
// 阶段 2: 计算线程池中执行
// 负责：解码 (retrieve / copy to host)、转码 (resize / jpeg encode)
// ---------------------------------------------------------
void StreamTask::stepCompute()
{
    if (!running_)
        return;

    if (gpu_id_ >= 0)
    {
        CUDATools::AutoDevice auto_device_exchange(gpu_id_);
    }

    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_ || !decoder_->isOpened())
    {
        lock.unlock();
        scheduleNext(0);
        return;
    }

    // 【修复2】清空字符串长度，但保留其底层容量 (Capacity)
    reusable_encode_buffer_.clear();
    bool encode_ok = false;

    if (decoder_->isGpuFrame() && encoder_->supportsGpuEncode())
    {
        // === 全 GPU 路径 (Zero Copy) ===

        if (decoder_->retrieve(reusable_frame_, true))
        {
            uint8_t *gpu_ptr = decoder_->getGpuFramePtr();
            if (gpu_ptr)
            {
                encode_ok = encoder_->encodeGpu(
                    gpu_ptr,
                    decoder_->getWidth(),
                    decoder_->getHeight(),
                    reusable_encode_buffer_ // 使用复用 buffer
                );
            }
        }
    }
    else
    {
        // === CPU 路径 (Memory Copy) ===
        // 使用 reusable_frame_，OpenCV内部尺寸不变时不会重新分配内存
        if (decoder_->retrieve(reusable_frame_, true) && !reusable_frame_.empty())
        {
            encode_ok = encoder_->encode(reusable_frame_, reusable_encode_buffer_);
        }
    }

    lock.unlock(); // 耗时操作结束，释放锁

    // 3. 更新结果
    if (encode_ok)
    {
        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            latest_encoded_frame_ = reusable_encode_buffer_;
            last_encode_time_ = std::chrono::steady_clock::now();
            frame_seq_++;
        }
        frame_cv_.notify_all();
    }

    // 4. 回到 IO 循环抓取下一帧
    if (interruptibleSleep(1))
    {
        scheduleNext(0);
    }
}

void StreamTask::updateHeartbeat()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
}

bool StreamTask::getLatestEncodedFrame(std::string &out_buffer)
{
    updateHeartbeat();
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (latest_encoded_frame_.empty())
    {
        return false;
    }
    out_buffer = latest_encoded_frame_;
    return true;
}

bool StreamTask::waitForNextFrame(std::string &out_buffer, uint64_t &current_seq, int timeout_ms)
{
    updateHeartbeat();
    std::unique_lock<std::mutex> lock(frame_mutex_);

    // 阻塞等待，直到帧序号更新，或者任务被停止
    bool signaled = frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, current_seq]()
                                       { return frame_seq_ > current_seq || !running_.load(); });

    if (!running_.load() || latest_encoded_frame_.empty())
    {
        return false;
    }

    if (signaled && frame_seq_ > current_seq)
    {
        out_buffer = latest_encoded_frame_; // 拷贝新帧
        current_seq = frame_seq_;           // 更新客户端持有的序号
        return true;
    }

    return false; // 超时
}

bool StreamTask::isConnected()
{
    updateHeartbeat();
    return connected_;
}

void StreamTask::keepAlive()
{
    updateHeartbeat();
}

bool StreamTask::isTimeout()
{
    if (heartbeat_timeout_ms_ <= 0)
        return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last_access = last_access_time_.load();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::duration(now - last_access))
                           .count();
    return duration_ms > heartbeat_timeout_ms_;
}