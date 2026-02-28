#include "stream_task.h"
#include <chrono>

#include <spdlog/spdlog.h>

StreamTask::StreamTask(const std::string &url,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       std::unique_ptr<IVideoDecoder> decoder,
                       std::shared_ptr<IImageEncoder> encoder)
    : url_(url),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
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
    decoder_->open(url_);
    
    // 帧率控制
    auto last_encode_time = std::chrono::steady_clock::now();
    const int64_t encode_interval_us = decode_interval_ms_ > 0 
        ? decode_interval_ms_ * 1000LL 
        : 0;  // 0 表示不限制，每帧都编码

    while (running_)
    {   
        if (!decoder_->isOpened())
        {
            if (connected_)
            {
                spdlog::warn("Decoder lost connection for URL: {}", url_);
            }
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            decoder_->open(url_);
            last_encode_time = std::chrono::steady_clock::now();
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
            // 获取帧并编码
            cv::Mat frame;
            if (decoder_->retrieve(frame, true) && !frame.empty())
            {
                std::string temp_encoded_buffer;
                if (encoder_->encode(frame, temp_encoded_buffer))
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    latest_encoded_frame_ = std::move(temp_encoded_buffer);
                }
            }
        }
    }
}