#include "rtsp_service.h"
#include "decoder_factory.h"
#include "opencv_encoder.h"
#include "utils.h"
#include <iostream>

// =============================================================
// 构造函数：启动清理线程
// =============================================================
RTSPServiceImpl::RTSPServiceImpl() : manager_running_(true) {
    cleanup_thread_ = std::thread(&RTSPServiceImpl::cleanupLoop, this);
}

// =============================================================
// 析构函数：停止清理线程和所有流
// =============================================================
RTSPServiceImpl::~RTSPServiceImpl() {
    manager_running_ = false;
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    // 停止所有流任务
    for (auto& pair : streams_) {
        if (pair.second) {
            pair.second->stop();
        }
    }
}

// =============================================================
// StartStream：开启拉流（包含查重、注入解码器/编码器）
// =============================================================
grpc::Status RTSPServiceImpl::StartStream(grpc::ServerContext* context, const streamingservice::StartRequest* request, streamingservice::StartResponse* response) {
    std::string req_url = request->rtsp_url();

    // 1. 查重逻辑：检查是否已经有相同的 URL 在拉流
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto& pair : streams_) {
            if (pair.second->getUrl() == req_url) {
                // 复用已存在的流
                std::string existing_id = pair.first;
                pair.second->keepAlive(); // 刷新心跳保活
                
                std::cout << "[REUSE] URL already exists. Returning ID: " << existing_id << "\n";
                
                response->set_success(true);
                response->set_stream_id(existing_id);
                response->set_message("Stream already exists, reusing existing task.");
                return grpc::Status::OK;
            }
        }
    }

    // 2. 创建解码器 (Decoder)
    auto decoder = DecoderFactory::create(request->decoder_type());
    if (!decoder) {
        response->set_success(false);
        response->set_message("Failed to create requested decoder type");
        return grpc::Status::OK;
    }

    // 3. 创建编码器 (Encoder)
    // 这里默认创建 OpenCV JPEG 编码器，质量 85
    auto encoder = std::make_shared<OpencvEncoder>(85);

    // 4. 创建流任务 (StreamTask)
    // 将解码器和编码器都注入到任务中
    auto task = std::make_shared<StreamTask>(
        req_url, 
        request->heartbeat_timeout_ms(), 
        request->decode_interval_ms(),
        std::move(decoder),
        encoder // 传入编码器
    );
    
    std::string stream_id = generate_uuid();

    // 5. 再次加锁存入 Map (双重检查)
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        // 防止在创建 Task 的间隙，别的线程插入了同样的流
        for (auto& pair : streams_) {
            if (pair.second->getUrl() == req_url) {
                task->stop(); // 停止刚才新创建的冗余任务
                response->set_success(true);
                response->set_stream_id(pair.first);
                response->set_message("Stream created by another request concurrently.");
                return grpc::Status::OK;
            }
        }
        streams_[stream_id] = task;
    }

    response->set_success(true);
    response->set_stream_id(stream_id);
    response->set_message("Stream started successfully");
    std::cout << "[START] New Stream ID: " << stream_id << " | URL: " << req_url << "\n";
    return grpc::Status::OK;
}

// =============================================================
// StopStream：停止拉流
// =============================================================
grpc::Status RTSPServiceImpl::StopStream(grpc::ServerContext* context, const streamingservice::StopRequest* request, streamingservice::StopResponse* response) {
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task_to_stop;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            task_to_stop = it->second;
            streams_.erase(it); // 从 Map 中移除，引用计数 -1
        }
    }

    if (task_to_stop) {
        task_to_stop->stop(); // 在锁外执行耗时的停止操作
        std::cout << "[STOP] Stream manually stopped: " << stream_id << "\n";
        response->set_success(true);
        response->set_message("Stream stopped successfully");
    } else {
        response->set_success(false);
        response->set_message("Stream ID not found");
    }
    return grpc::Status::OK;
}

// =============================================================
// GetLatestFrame：获取最新帧 
// =============================================================
grpc::Status RTSPServiceImpl::GetLatestFrame(grpc::ServerContext* context, const streamingservice::FrameRequest* request, streamingservice::FrameResponse* response) {
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            task = it->second;
        }
    }

    if (!task) {
        response->set_success(false);
        response->set_message("Stream ID not found or expired");
        return grpc::Status::OK;
    }

    // 【核心修改】：直接获取预先编码好的 JPEG 字符串
    // 这里没有耗时的编码操作，只是内存拷贝，非常快
    std::string buffer;
    if (task->getLatestEncodedFrame(buffer)) {
        response->set_success(true);
        response->set_image_data(buffer); // Protobuf 会处理 string 赋值
        response->set_message("OK");
    } else {
        response->set_success(false);
        response->set_message("No frame available yet or stream disconnected");
    }
    return grpc::Status::OK;
}

// =============================================================
// CheckStream：查询流状态
// =============================================================
grpc::Status RTSPServiceImpl::CheckStream(grpc::ServerContext* context, const streamingservice::CheckRequest* request, streamingservice::CheckResponse* response) {
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            task = it->second;
        }
    }

    if (task) {
        response->set_exists(true);
        response->set_is_connected(task->isConnected());
        response->set_message("Stream exists");
    } else {
        response->set_exists(false);
        response->set_is_connected(false);
        response->set_message("Stream ID not found");
    }
    return grpc::Status::OK;
}

// =============================================================
// cleanupLoop：后台线程，清理心跳超时的流
// =============================================================
void RTSPServiceImpl::cleanupLoop() {
    while (manager_running_) {
        // 每 2 秒检查一次
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::vector<std::shared_ptr<StreamTask>> tasks_to_stop;
        
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            for (auto it = streams_.begin(); it != streams_.end(); ) {
                if (it->second->isTimeout()) {
                    std::cout << "[TIMEOUT] Auto-cleaning zombie stream ID: " << it->first << "\n";
                    tasks_to_stop.push_back(it->second);
                    it = streams_.erase(it); // 从 Map 中移除
                } else {
                    ++it;
                }
            }
        }

        // 在锁外停止任务，防止死锁或阻塞主 Map
        for (auto& task : tasks_to_stop) {
            task->stop();
        }
    }
}