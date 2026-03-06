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
    // 初始化内存池
    frame_pool_ = std::make_shared<FrameMemoryPool>(3 * 1024 * 1024);
    updateHeartbeat();
    last_encode_time_ = std::chrono::steady_clock::now();
}

StreamTask::~StreamTask()
{
    spdlog::warn("==== ~StreamTask DESTROYED: {} ====", url_);
    stop();
    // 清理最新的引用，引用计数减一，可能触发内存归还或释放
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        latest_encoded_frame_.reset();
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
    bool stopped = sleep_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]()
                                      { return !running_.load(); });

    return !stopped;
}

void StreamTask::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
        return;
    }

    stopped_ = false;
    spdlog::info("StreamTask started: {}", url_);

    std::weak_ptr<StreamTask> weak_self = shared_from_this();
    TaskScheduler::instance().getIOPool().enqueue(
        [weak_self]()
        { if (auto self = weak_self.lock()) {self->stepIO();} });
}

void StreamTask::stop()
{
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false))
    {
        spdlog::info("StreamTask stopping: {}", url_);
        stopped_ = true;

        {
            std::lock_guard<std::mutex> sleep_lock(sleep_mutex_);
            sleep_cv_.notify_all();
        }

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            frame_cv_.notify_all();
        }

        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_)
        {
            decoder_->release();
        }

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            latest_encoded_frame_.reset();
        }

        if (frame_pool_)
        {
            frame_pool_.reset();
        }

        status_ = StreamStatus::DISCONNECTED;
        connected_ = false;
    }
}

void StreamTask::scheduleNext(int force_delay_ms)
{
    if (!running_)
        return;

    std::weak_ptr<StreamTask> weak_self = shared_from_this();

    if (force_delay_ms > 0)
    {
        TimerScheduler::instance().schedule(force_delay_ms, [weak_self]()
                                            {
            if (auto locked_self = weak_self.lock()) {  
                if (locked_self->running_) {
                    TaskScheduler::instance().getIOPool().enqueue([weak_self]() {
                        if (auto still_alive = weak_self.lock()) {
                            still_alive->stepIO();
                        }
                    });
                }
            } });
    }
    else
    {
        TaskScheduler::instance().getIOPool().enqueue([weak_self]()
                                                      {
            if (auto still_alive = weak_self.lock()) {
                if (still_alive->running_) still_alive->stepIO();
            } });
    }
}

void StreamTask::stepIO()
{
    if (!running_)
        return;

    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_)
        return;

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
            scheduleNext(1000);
            return;
        }
    }

    if (!decoder_->grab())
    {
        spdlog::warn("Frame grab failed: {}", url_);
        connected_ = false;
        decoder_->release();

        lock.unlock();
        if (interruptibleSleep(50))
        {
            scheduleNext(0);
        }
        return;
    }

    connected_ = true;
    reconnect_attempts_ = 0;

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

    if (should_process)
    {
        lock.unlock();
        std::weak_ptr<StreamTask> weak_self = shared_from_this();
        TaskScheduler::instance().getComputePool(gpu_id_).enqueue(
            [weak_self]()
            {  if (auto self = weak_self.lock()) {
                self->stepCompute();
            } });
    }
    else
    {
        int drop_count = 0;
        while (!should_process && drop_count < 5 && running_)
        {
            cv::Mat dummy;
            decoder_->retrieve(dummy, false);
            dummy.release();

            if (!interruptibleSleep(2))
            {
                return;
            }

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

            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_encode_time_).count();
            if (elapsed_us >= decode_interval_us)
            {
                should_process = true;
            }
        }
        lock.unlock();
        scheduleNext(0);
    }
}

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

    // 从内存池获取 Buffer
    auto encode_buffer = frame_pool_->acquire();
    bool encode_ok = false;

    if (decoder_->isGpuFrame() && encoder_->supportsGpuEncode())
    {
        if (decoder_->retrieve(reusable_frame_, true))
        {
            uint8_t *gpu_ptr = decoder_->getGpuFramePtr();
            if (gpu_ptr)
            {
                encode_ok = encoder_->encodeGpu(
                    gpu_ptr,
                    decoder_->getWidth(),
                    decoder_->getHeight(),
                    *encode_buffer);
            }
        }
    }
    else
    {
        if (decoder_->retrieve(reusable_frame_, true) && !reusable_frame_.empty())
        {
            encode_ok = encoder_->encode(reusable_frame_, *encode_buffer);
        }
    }

    lock.unlock();

    if (encode_ok)
    {
        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            latest_encoded_frame_ = encode_buffer;
            last_encode_time_ = std::chrono::steady_clock::now();
            frame_seq_++;
        }
        frame_cv_.notify_all();
    }

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

bool StreamTask::getLatestEncodedFrame(std::shared_ptr<std::string> &out_buffer)
{
    updateHeartbeat();
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (!latest_encoded_frame_)
    {
        return false;
    }
    out_buffer = latest_encoded_frame_;
    return true;
}

bool StreamTask::waitForNextFrame(std::shared_ptr<std::string> &out_buffer, uint64_t &current_seq, int timeout_ms)
{
    updateHeartbeat();
    std::unique_lock<std::mutex> lock(frame_mutex_);

    bool signaled = frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, current_seq]()
                                       { return frame_seq_ > current_seq || !running_.load(); });

    if (!running_.load() || !latest_encoded_frame_)
    {
        return false;
    }

    if (signaled && frame_seq_ > current_seq)
    {
        out_buffer = latest_encoded_frame_;
        current_seq = frame_seq_;
        return true;
    }

    return false;
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