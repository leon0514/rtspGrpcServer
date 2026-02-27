#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <opencv2/opencv.hpp>
#include "interfaces.h"

class StreamTask {
public:
    // 修改构造函数：增加 encoder 参数
    StreamTask(const std::string& url, 
               int heartbeat_timeout_ms, 
               int decode_interval_ms,
               std::unique_ptr<IVideoDecoder> decoder,
               std::shared_ptr<IImageEncoder> encoder); // 【新增】
    ~StreamTask();

    void stop();
    
    // 返回 true 表示获取成功，out_buffer 中填入数据
    bool getLatestEncodedFrame(std::string& out_buffer);

    bool isConnected();
    bool isTimeout();
    
    std::string getUrl() const { return url_; }
    void keepAlive();

private:
    void updateHeartbeat();
    void readLoop();

    std::string url_;
    int heartbeat_timeout_ms_;
    int decode_interval_ms_;

    std::unique_ptr<IVideoDecoder> decoder_;
    std::shared_ptr<IImageEncoder> encoder_; // 【新增】持有编码器

    // 【修改】缓存的是编码后的字节流，而不是原始 Mat
    std::string latest_encoded_frame_;
    
    std::mutex frame_mutex_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<int64_t> last_access_time_;
    std::thread worker_thread_;
};