#include "stream_task.h"
#include <chrono>

#include <spdlog/spdlog.h>

StreamTask::StreamTask(const std::string &url,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       bool keep_on_failure,
                       std::unique_ptr<IVideoDecoder> decoder,
                       std::shared_ptr<IImageEncoder> encoder)
    : url_(url),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
      keep_on_failure_(keep_on_failure),
      decoder_(std::move(decoder)),
      encoder_(std::move(encoder)),
      running_(true),
      connected_(false)
{
    updateHeartbeat();
    worker_thread_ = std::thread(&StreamTask::readLoop, this);
}

StreamTask::~StreamTask()
{
    stop();
}

void StreamTask::stop()
{
    if (running_)
    {
        running_ = false;
        if (worker_thread_.joinable())
            worker_thread_.join();
        if (decoder_ && decoder_->isOpened())
            decoder_->release();
        spdlog::info("StreamTask stopped for URL: {}", url_);
    }
}

bool StreamTask::getLatestEncodedFrame(std::string &out_buffer)
{
    updateHeartbeat();
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (latest_encoded_frame_.empty())
    {
        return false;
    }

    // 直接赋值字符串，发生内存拷贝，但比 JPEG 编码快得多
    out_buffer = latest_encoded_frame_;
    return true;
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

void StreamTask::updateHeartbeat()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
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

void StreamTask::readLoop()
{
    spdlog::info("StreamTask starting for URL: {}", url_);
    
    // 初始状态为 CONNECTING
    status_ = StreamStatus::CONNECTING;
    
    if (!decoder_->open(url_))
    {
        spdlog::error("Failed to open stream, stopping task: {}", url_);
        status_ = StreamStatus::DISCONNECTED;
        stopped_ = true;
        return;
    }
    
    // 打开成功
    status_ = StreamStatus::CONNECTED;
    
    // 帧率控制
    auto last_encode_time = std::chrono::steady_clock::now();
    const int64_t encode_interval_us = decode_interval_ms_ > 0 
        ? decode_interval_ms_ * 1000LL 
        : 0;  // 0 表示不限制，每帧都编码
    
    const int max_reconnect_attempts = 5;  // 最大重连次数
    int reconnect_attempts = 0;

    while (running_)
    {   
        if (!decoder_->isOpened())
        {
            // 曾经打开过，现在断开了，尝试重连
            if (reconnect_attempts >= max_reconnect_attempts)
            {
                spdlog::error("Max reconnect attempts reached, stopping task: {}", url_);
                connected_ = false;
                status_ = StreamStatus::DISCONNECTED;
                stopped_ = true;
                break;
            }
            
            reconnect_attempts++;
            status_ = StreamStatus::CONNECTING;
            spdlog::warn("Decoder disconnected, reconnect attempt {}/{}: {}", 
                         reconnect_attempts, max_reconnect_attempts, url_);
            connected_ = false;
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (decoder_->open(url_))
            {
                spdlog::info("Reconnected successfully: {}", url_);
                status_ = StreamStatus::CONNECTED;
                reconnect_attempts = 0;
                last_encode_time = std::chrono::steady_clock::now();
            }
            continue;
        }

        if (!decoder_->grab())
        {
            if (connected_)
            {
                spdlog::warn("Frame grab failed for URL: {}", url_);
            }
            connected_ = false;
            decoder_->release();
            continue;
        }

        connected_ = true;
        reconnect_attempts = 0;  // 正常工作，重置重连计数

        // 帧率控制：检查是否到达编码间隔
        bool should_encode = true;
        if (encode_interval_us > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_encode_time).count();
            if (elapsed_us < encode_interval_us)
            {
                // 还没到编码时间，消费帧但不处理
                cv::Mat dummy;
                decoder_->retrieve(dummy, false);
                should_encode = false;
            }
            else
            {
                last_encode_time = now;
            }
        }

        if (should_encode)
        {
            std::string temp_encoded_buffer;
            bool encode_ok = false;
            
            // GPU 解码 + GPU 编码：零拷贝路径
            if (decoder_->isGpuFrame() && encoder_->supportsGpuEncode())
            {
                // 触发解码器更新状态（不需要实际数据）
                cv::Mat dummy;
                if (decoder_->retrieve(dummy, true))
                {
                    uint8_t* gpu_ptr = decoder_->getGpuFramePtr();
                    if (gpu_ptr)
                    {
                        encode_ok = encoder_->encodeGpu(
                            gpu_ptr, 
                            decoder_->getWidth(), 
                            decoder_->getHeight(), 
                            temp_encoded_buffer
                        );
                    }
                }
            }
            else
            {
                // CPU 路径：需要拷贝数据
                cv::Mat frame;
                if (decoder_->retrieve(frame, true) && !frame.empty())
                {
                    encode_ok = encoder_->encode(frame, temp_encoded_buffer);
                }
            }
            
            if (encode_ok)
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                latest_encoded_frame_ = std::move(temp_encoded_buffer);
            }
        }
    }
}