#include "stream_task.h"
#include <chrono>

StreamTask::StreamTask(const std::string& url, 
                       int heartbeat_timeout_ms, 
                       int decode_interval_ms, 
                       std::unique_ptr<IVideoDecoder> decoder,
                       std::shared_ptr<IImageEncoder> encoder)
    : url_(url), 
      heartbeat_timeout_ms_(heartbeat_timeout_ms), 
      decode_interval_ms_(decode_interval_ms),
      decoder_(std::move(decoder)), 
      encoder_(std::move(encoder)),
      running_(true), 
      connected_(false) 
{
    updateHeartbeat();
    worker_thread_ = std::thread(&StreamTask::readLoop, this);
}

StreamTask::~StreamTask() {
    stop();
}

void StreamTask::stop() {
    if (running_) {
        running_ = false;
        if (worker_thread_.joinable()) worker_thread_.join();
        if (decoder_ && decoder_->isOpened()) decoder_->release();
    }
}

bool StreamTask::getLatestEncodedFrame(std::string& out_buffer) {
    updateHeartbeat();
    std::lock_guard<std::mutex> lock(frame_mutex_);
    
    if (latest_encoded_frame_.empty()) {
        return false;
    }
    
    // 直接赋值字符串，发生内存拷贝，但比 JPEG 编码快得多
    out_buffer = latest_encoded_frame_;
    return true;
}

bool StreamTask::isConnected() {
    updateHeartbeat();
    return connected_;
}

void StreamTask::keepAlive() {
    updateHeartbeat();
}

void StreamTask::updateHeartbeat() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
}

bool StreamTask::isTimeout() {
    if (heartbeat_timeout_ms_ <= 0) return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last_access = last_access_time_.load();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration(now - last_access)).count();
    return duration_ms > heartbeat_timeout_ms_;
}

void StreamTask::readLoop() {
    decoder_->open(url_);
    auto last_decode_time = std::chrono::steady_clock::now();
    
    // 【新增】定义丢帧计数器
    int startup_drop_count = 0;
    const int DROP_FRAMES_ON_START = 15; // 启动时丢弃前 15 帧（根据 fps 不同，约等待 0.5-1秒）

    while (running_) {
        if (!decoder_->isOpened()) {
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::seconds(1)); 
            decoder_->open(url_);
            // 重连后，也要重置计数器，防止重连瞬间花屏
            startup_drop_count = 0; 
            continue;
        }

        if (!decoder_->grab()) {
            connected_ = false;
            decoder_->release(); 
            continue;
        }
        connected_ = true;

        // 解码间隔控制 (保持不变)
        if (decode_interval_ms_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_decode_time).count();
            if (elapsed < decode_interval_ms_) continue; 
            last_decode_time = now;
        }

        cv::Mat frame;
        if (decoder_->retrieve(frame) && !frame.empty()) {
            
            // 【新增】热身逻辑：如果还没丢够帧数，直接跳过编码
            if (startup_drop_count < DROP_FRAMES_ON_START) {
                startup_drop_count++;
                // 可以在这里打印日志调试
                // std::cout << "Dropping startup frame: " << startup_drop_count << std::endl;
                continue; 
            }

            // 只有画面稳定后，才执行编码并存入缓存
            std::string temp_encoded_buffer;
            if (encoder_->encode(frame, temp_encoded_buffer)) {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                latest_encoded_frame_ = std::move(temp_encoded_buffer);
            }
        }
    }
}