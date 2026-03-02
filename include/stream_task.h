#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <opencv2/opencv.hpp>
#include "interfaces.h"

// 流连接状态
enum class StreamStatus {
    CONNECTING = 0,    // 连接中
    CONNECTED = 1,     // 已连接
    DISCONNECTED = 2   // 无法连接
};

class StreamTask
{
public:
    // 修改构造函数：增加 encoder 参数和 decoder_type
    StreamTask(const std::string &url,
               int heartbeat_timeout_ms,
               int decode_interval_ms,
               int decoder_type,
               bool keep_on_failure,
               std::unique_ptr<IVideoDecoder> decoder,
               std::shared_ptr<IImageEncoder> encoder);
    ~StreamTask();

    void stop();

    // 返回 true 表示获取成功，out_buffer 中填入数据
    bool getLatestEncodedFrame(std::string &out_buffer);

    bool isConnected();
    bool isTimeout();
    bool isStopped() const { return stopped_; }
    bool shouldKeepOnFailure() const { return keep_on_failure_; }
    StreamStatus getStatus() const { return status_; }

    std::string getUrl() const { return url_; }
    int getDecoderType() const { return decoder_type_; }
    int getDecodeIntervalMs() const { return decode_interval_ms_; }
    int getWidth() const { return decoder_ ? decoder_->getWidth() : 0; }
    int getHeight() const { return decoder_ ? decoder_->getHeight() : 0; }
    void keepAlive();

private:
    void updateHeartbeat();
    void readLoop();

    std::string url_;
    int heartbeat_timeout_ms_;
    int decode_interval_ms_;
    int decoder_type_;
    bool keep_on_failure_;

    std::unique_ptr<IVideoDecoder> decoder_;
    std::shared_ptr<IImageEncoder> encoder_; // 【新增】持有编码器

    // 【修改】缓存的是编码后的字节流，而不是原始 Mat
    std::string latest_encoded_frame_;
    cv::Mat latest_frame_; // 可选：如果需要保留原始帧数据

    std::mutex frame_mutex_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<bool> stopped_{false};
    std::atomic<StreamStatus> status_{StreamStatus::CONNECTING};
    std::atomic<int64_t> last_access_time_;
    std::thread worker_thread_;
};