#pragma once

#include "interfaces.hpp" // 包含 IVideoDecoder, IImageEncoder
#include "frame_memory_pool.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <condition_variable>
#include "zero_copy_channel.hpp"

enum class StreamStatus
{
    CONNECTING,
    CONNECTED,
    DISCONNECTED
};

// 继承 enable_shared_from_this 至关重要，防止任务在线程池排队时对象被析构
class StreamTask : public std::enable_shared_from_this<StreamTask>
{
public:
    StreamTask(const std::string &url,
               const std::string &stream_id,
               int heartbeat_timeout_ms,
               int decode_interval_ms,
               int decoder_type,
               int gpu_id,
               bool keep_on_failure,
               bool use_shared_mem,
               std::unique_ptr<IVideoDecoder> decoder,
               std::shared_ptr<IImageEncoder> encoder);

    ~StreamTask();

    // 启动任务循环
    void start();

    // 停止任务
    void stop();

    // 获取最新编码好的帧（线程安全，零拷贝）
    bool getLatestEncodedFrame(std::shared_ptr<std::string> &out_buffer);

    // Getters / Setters
    bool isConnected();
    bool isStopped() const { return stopped_; }
    StreamStatus getStatus() const { return status_; }
    const std::string &getUrl() const { return url_; }
    int getDecoderType() const { return decoder_type_; }
    int getWidth() const { return decoder_ ? decoder_->getWidth() : 0; }
    int getHeight() const { return decoder_ ? decoder_->getHeight() : 0; }
    int getDecodeIntervalMs() const { return decode_interval_ms_; }
    bool shouldKeepOnFailure() const { return keep_on_failure_; }
    bool usesSharedMemory() const { return use_shared_mem_; }
    int getHeartbeatTimeMs() const { return heartbeat_timeout_ms_; }
    bool onlyKeyFrames() const { return decoder_->onlyKeyFrames(); }

    // 条件变量等待下一帧（零拷贝）
    bool waitForNextFrame(std::shared_ptr<std::string> &out_buffer, uint64_t &current_seq, int timeout_ms);

    // 心跳保活
    void keepAlive();
    bool isTimeout();

    void updateUrl(const std::string &new_url);
    int getFrameSequence() const { return frame_seq_.load(); }

private:
    void updateHeartbeat();

    // --- 异步调度逻辑 ---

    // 调度下一步操作
    void scheduleNext(int force_delay_ms = 0);
    
    // IO 操作，每个流一个线程，负责 grab 和调度计算任务
    void stepIO();
    void ioLoop();

    // 阶段2：计算操作 (Decode / Convert / Encode) -> 运行在 计算线程池
    void stepCompute();

    // 返回 false 表示休眠被 stop() 中断；返回 true 表示休眠正常结束
    bool interruptibleSleep(int ms);

    // --- 成员变量 ---
    std::string url_;
    std::string pending_url_;
    std::atomic<bool> url_changed_{false};
    std::string stream_id_;
    int heartbeat_timeout_ms_;
    int decode_interval_ms_;
    int decoder_type_;
    bool keep_on_failure_;
    bool use_shared_mem_;
    int gpu_id_ = -1;

    std::unique_ptr<IVideoDecoder> decoder_;
    std::shared_ptr<IImageEncoder> encoder_;

    std::unique_ptr<ZeroCopyChannel> shm_channel_;

    // 状态控制
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false}; // 显式停止标志
    std::atomic<StreamStatus> status_{StreamStatus::DISCONNECTED};
    std::atomic<bool> connected_{false};

    // 保护 decoder_ 的互斥锁（防止多线程重入或与 stop 冲突）
    std::mutex decoder_mutex_;

    // 每路流独立 IO 线程
    std::thread io_thread_;
    std::mutex io_mutex_;
    int next_io_delay_ms_ = 0;
    std::atomic<bool> io_wakeup_{false};

    // 保护最新帧数据的读写锁
    std::shared_mutex frame_mutex_;
    std::condition_variable_any frame_cv_;
    std::shared_ptr<std::string> latest_encoded_frame_;

    // 帧内存池
    std::shared_ptr<FrameMemoryPool> frame_pool_;

    // 心跳时间戳
    std::atomic<int64_t> last_access_time_;

    // 内部逻辑变量
    int reconnect_attempts_ = 0;
    std::chrono::steady_clock::time_point last_encode_time_;

    // 优化：休眠控制相关
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;

    std::atomic<uint64_t> frame_seq_{0};

    // 用于 CPU 路径的图像缓存，避免反复分配 cv::Mat
    cv::Mat reusable_frame_;

    // 性能监控：记录 grab 结束时间，用于计算调度延迟
    std::chrono::steady_clock::time_point last_grab_end_;
};