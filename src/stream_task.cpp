#include "stream_task.hpp"
#include "task_scheduler.hpp"
#include "nvjpeg_encoder.hpp"
#include "opencv_encoder.hpp"
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>
#include "cuda_tools.hpp"
#include <unordered_map>

namespace {
    using EncoderPtr = std::shared_ptr<IImageEncoder>;

    static size_t makeEncoderKey(bool use_gpu_encoder, int gpu_id, int quality)
    {
        uint64_t key = use_gpu_encoder ? (1ULL << 63) : 0ULL;
        key |= (static_cast<uint64_t>(static_cast<uint32_t>(gpu_id)) << 32);
        key |= static_cast<uint64_t>(static_cast<uint32_t>(quality));
        return static_cast<size_t>(key);
    }

    static EncoderPtr getThreadLocalEncoder(bool use_gpu_encoder, int gpu_id, int quality)
    {
        static thread_local std::unordered_map<size_t, EncoderPtr> encoders;
        size_t key = makeEncoderKey(use_gpu_encoder, gpu_id, quality);
        auto it = encoders.find(key);
        if (it != encoders.end())
        {
            return it->second;
        }

        EncoderPtr encoder;
        if (use_gpu_encoder)
        {
            encoder = std::make_shared<NvjpegEncoder>(quality, gpu_id);
        }
        else
        {
            encoder = std::make_shared<OpencvEncoder>(quality);
        }
        encoders.emplace(key, encoder);
        return encoder;
    }
}

StreamTask::StreamTask(const std::string &url,
                       const std::string &stream_id,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       int gpu_id,
                       bool keep_on_failure,
                       bool use_shared_mem,
                       std::unique_ptr<IVideoDecoder> decoder,
                       bool use_gpu_encoder,
                       int jpeg_quality)
    : url_(url),
      stream_id_(stream_id),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
      gpu_id_(gpu_id),
      keep_on_failure_(keep_on_failure),
      use_shared_mem_(use_shared_mem),
      decoder_(std::move(decoder)),
      use_gpu_encoder_(use_gpu_encoder),
      jpeg_quality_(jpeg_quality)
{
    // 初始化内存池
    frame_pool_ = std::make_shared<FrameMemoryPool>(3 * 1024 * 1024);
    updateHeartbeat();
    last_encode_time_ = std::chrono::steady_clock::now();

    if (use_shared_mem_)
    {
        try
        {
            shm_channel_ = std::make_unique<ZeroCopyChannel>(stream_id_, 0); // 生产者角色
            spdlog::info("SharedMemory enabled for stream: {}", stream_id_);
        }
        catch (const std::exception &e)
        {
            spdlog::error("Failed to init SHM: {}", e.what());
            use_shared_mem_ = false;
        }
    }
}

StreamTask::~StreamTask()
{
    spdlog::warn("==== ~StreamTask DESTROYED: {} ====", url_);
    running_ = false;
    
    // 2. 必须强制 join，确保线程不再访问此对象的成员
    if (io_thread_.joinable()) 
    {
        // 如果当前线程就是 io_thread_ 自己，这里会死锁，所以要判断
        if (std::this_thread::get_id() != io_thread_.get_id()) 
        {
            io_thread_.join();
        } 
    }
    stop();
    // 清理最新的引用，引用计数减一，可能触发内存归还或释放
    {
        std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
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
    sleep_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]()
                      { return !running_.load() || io_wakeup_; });

    return running_.load();
}

void StreamTask::ioLoop()
{
    while (running_)
    {
        stepIO();
        if (!running_)
            break;

        int delay_ms = 0;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            delay_ms = next_io_delay_ms_;
            next_io_delay_ms_ = 0;
            io_wakeup_ = false;
        }

        if (delay_ms <= 0)
            delay_ms = 1; // 避免 busy loop

        if (!interruptibleSleep(delay_ms))
            break;
    }
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
    io_thread_ = std::thread([weak_self](){
        if (auto self = weak_self.lock())
        {
            self->ioLoop();
        } }
    );
}

void StreamTask::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
    {
        return;
    }
    
    if (io_thread_.joinable()) 
    {
        if (std::this_thread::get_id() != io_thread_.get_id()) 
        {
            sleep_cv_.notify_all();
            io_thread_.join();
        }
    }

    spdlog::info("StreamTask stopping: {}", url_);
    stopped_ = true;

    {
        std::unique_lock<std::mutex> sleep_lock(sleep_mutex_);
        sleep_cv_.notify_all();
    }

    std::shared_ptr<std::string> old_frame_to_release;
    {
        std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
        old_frame_to_release = std::move(latest_encoded_frame_); // 此时 latest_encoded_frame_ 变为空
        frame_cv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_)
        {
            decoder_->release();
        }
    }

    // 5. 共享内存清理
    if (shm_channel_)
    {
        shm_channel_->cleanup();
        shm_channel_.reset(); // 安全重置
    }

    // 6. 状态重置
    status_ = StreamStatus::DISCONNECTED;
    connected_ = false;
}

void StreamTask::scheduleNext(int force_delay_ms)
{
    if (!running_)
        return;

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        next_io_delay_ms_ = force_delay_ms;
        io_wakeup_ = true;
    }

    sleep_cv_.notify_all();
}

void StreamTask::stepIO()
{
    if (!running_)
        return;

    std::unique_lock<std::mutex> lock(decoder_mutex_);

    if (url_changed_)
    {
        spdlog::info("IO Thread: Switching to new URL: {}", pending_url_);
        url_ = pending_url_;
        url_changed_ = false;
        if (decoder_)
        {
            decoder_->release(); // 在 IO 线程释放，不卡 gRPC 线程
        }
    }

    if (!decoder_)
        return;

    // 1. 处理连接断开重连逻辑
    if (!decoder_->isOpened())
    {
        const int max_reconnect_attempts = 5;
        if (reconnect_attempts_ >= max_reconnect_attempts)
        {
            if (!keep_on_failure_)
            {
                spdlog::error("Max reconnect attempts reached. Stopping task: {}", url_);
                lock.unlock(); // 尽早释放锁
                stop();
                return;
            }
            reconnect_attempts_ = 0; // 重置计数，准备下一轮无尽重连
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
            // 连接成功后，重置编码时间，准备立即出第一帧
            last_encode_time_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
        }
        else
        {
            lock.unlock();
            // 连接失败，使用定时器延迟 1000ms 后再次触发，不阻塞线程池！
            scheduleNext(1000);
            return;
        }
    }

    // 2. 抓取网络包 (grab 本身是阻塞的，由摄像头帧率控制节奏)
    auto grab_start = std::chrono::steady_clock::now();
    if (!decoder_->grab())
    {
        spdlog::warn("Frame grab failed: {}", url_);
        connected_ = false;
        decoder_->release();
        lock.unlock();

        // 抓取失败，退避 50ms 后重试
        scheduleNext(50);
        return;
    }
    auto grab_end = std::chrono::steady_clock::now();

    connected_ = true;
    reconnect_attempts_ = 0;
    last_grab_end_ = grab_end;

    // 3. 抽帧逻辑判断 (Frame dropping)
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
        // 需要解码：释放锁，将任务派发给计算线程池
        lock.unlock();
        std::weak_ptr<StreamTask> weak_self = shared_from_this();
        TaskScheduler::instance().getComputePool(gpu_id_).enqueue(
            [weak_self]()
            {
                if (auto self = weak_self.lock())
                {
                    self->stepCompute();
                }
            });
    }
    else
    {
        // 不需要解码（抽帧丢弃）：仅仅把刚才 grab 的数据从内部缓冲区清空
        cv::Mat dummy;
        decoder_->retrieve(dummy, false); // false 可能代表 fast/dummy retrieve

        lock.unlock();
        // 丢弃完毕，立即准备抓取下一帧。
        // 因为 grab() 是网络阻塞的，所以传 0 也不会导致 CPU 100% 空转。
        scheduleNext(1);
    }
}

void StreamTask::stepCompute()
{
    auto compute_start = std::chrono::steady_clock::now();
    auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(compute_start - last_grab_end_).count();

    if (!running_)
        return;
    if (gpu_id_ >= 0)
    {
        CUDATools::AutoDevice auto_device_exchange(gpu_id_);
    }

    if (use_shared_mem_ && shm_channel_)
    {
        keepAlive();
    }
    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_ || !decoder_->isOpened())
    {
        lock.unlock();
        scheduleNext(0);
        return;
    }

    bool frame_ready = false;
    
    // === 分支 1: 共享内存模式 -> 直接传原始 Mat ===
    if (use_shared_mem_ && shm_channel_)
    {
        // 注意：SHM 模式下不要在服务端自己 keepAlive，否则心跳永远不失效
        // 客户端应通过 gRPC CheckStream / isOpened() 来维持心跳
        // 直接获取解码后的 Mat (GPU/CPU 自适应)
        bool retrieved = decoder_->retrieve(reusable_frame_, true);
        if (retrieved && !reusable_frame_.empty())
        {
            auto now = std::chrono::steady_clock::now();
            uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch()).count();
            
            // 写入共享内存（跳过编码！）
            if (shm_channel_->write_frame_mat(reusable_frame_, ts))
            {
                frame_ready = true;
                // 可选：更新统计信息
                // spdlog::info("SHM frame written: {}x{}@{}ch, size={}B", 
                //               reusable_frame_.cols, reusable_frame_.rows,
                //               reusable_frame_.channels(), 
                //               reusable_frame_.total() * reusable_frame_.elemSize());
            }
        }
    }
    else
    {
        auto encode_buffer = frame_pool_->acquire();
        auto encoder = getThreadLocalEncoder(use_gpu_encoder_, gpu_id_, jpeg_quality_);
        
        if (decoder_->isGpuFrame() && encoder->supportsGpuEncode())
        {
            if (decoder_->retrieve(reusable_frame_, true))
            {
                uint8_t *gpu_ptr = decoder_->getGpuFramePtr();
                if (gpu_ptr)
                {
                    frame_ready = encoder->encodeGpu(gpu_ptr, decoder_->getWidth(), 
                                                     decoder_->getHeight(), *encode_buffer);
                }
            }
        }
        else
        {
            if (decoder_->retrieve(reusable_frame_, true) && !reusable_frame_.empty())
            {
                frame_ready = encoder->encode(reusable_frame_, *encode_buffer);
            }
        }
        
        // 编码成功后通知订阅者
        if (frame_ready)
        {
            std::shared_ptr<std::string> prev_frame;
            {
                std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
                prev_frame = std::move(latest_encoded_frame_);
                latest_encoded_frame_ = encode_buffer;
                last_encode_time_ = std::chrono::steady_clock::now();
                frame_seq_.fetch_add(1, std::memory_order_release);
            }
            frame_cv_.notify_all();
        }
    }
    
    lock.unlock();
    
    // 立即调度下一帧（避免硬编码 sleep）
    scheduleNext(0);
}

void StreamTask::updateHeartbeat()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
}

bool StreamTask::getLatestEncodedFrame(std::shared_ptr<std::string> &out_buffer)
{
    updateHeartbeat();
    std::shared_lock<std::shared_mutex> lock(frame_mutex_);

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
    std::unique_lock<std::shared_mutex> lock(frame_mutex_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (running_.load() && frame_seq_.load(std::memory_order_acquire) <= current_seq)
    {
        if (frame_cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            break;
        }
    }

    if (!running_.load() || !latest_encoded_frame_)
        return false;

    if (frame_seq_.load(std::memory_order_acquire) > current_seq)
    {
        out_buffer = latest_encoded_frame_;
        current_seq = frame_seq_.load(std::memory_order_acquire);
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

void StreamTask::updateUrl(const std::string &new_url)
{
    // 这里只设置 pending 状态，不执行耗时的 release
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    if (url_ != new_url)
    {
        pending_url_ = new_url;
        url_changed_ = true; // 原子标记 URL 已经改变，等待 stepIO() 循环自然处理
        spdlog::info("StreamTask URL updated: {} -> {}", url_, new_url);
        status_ = StreamStatus::CONNECTING;
    }
}