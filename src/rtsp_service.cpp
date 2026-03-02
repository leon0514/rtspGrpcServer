#include "rtsp_service.h"
#include "decoder_factory.h"
#include "opencv_encoder.h"
#include "nvjpeg_encoder.h"
#include "utils.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h> // support for logging user-defined types if needed

// =============================================================
// 构造函数：启动清理线程
// =============================================================
RTSPServiceImpl::RTSPServiceImpl() : manager_running_(true)
{
    cleanup_thread_ = std::thread(&RTSPServiceImpl::cleanupLoop, this);
}

// =============================================================
// 析构函数：停止清理线程和所有流
// =============================================================
RTSPServiceImpl::~RTSPServiceImpl()
{
    manager_running_ = false;
    if (cleanup_thread_.joinable())
    {
        cleanup_thread_.join();
    }
    // 停止所有流任务
    for (auto &pair : streams_)
    {
        if (pair.second)
        {
            pair.second->stop();
        }
    }
}

// =============================================================
// StartStream：开启拉流（包含查重、注入解码器/编码器）
// =============================================================
grpc::Status RTSPServiceImpl::StartStream(grpc::ServerContext *context, const streamingservice::StartRequest *request, streamingservice::StartResponse *response)
{
    std::string req_url = request->rtsp_url();

    // 1. 查重逻辑：检查是否已经有相同的 URL 在拉流
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                // 复用已存在的流
                std::string existing_id = pair.first;
                pair.second->keepAlive(); // 刷新心跳保活

                spdlog::info("[REUSE] URL already exists. Returning ID: {}", existing_id);

                response->set_success(true);
                response->set_stream_id(existing_id);
                response->set_message("Stream already exists, reusing existing task.");
                return grpc::Status::OK;
            }
        }
    }

    // 2. 创建解码器 (Decoder)
    auto decoder_type = request->decoder_type();
    int gpu_id = request->gpu_id();  // 默认为 0
    // 0 opencv 1 gpu 2 ffmpeg
    std::string decode_type_str = (decoder_type == streamingservice::DECODER_CPU_OPENCV) ? "OpenCV" : (decoder_type == streamingservice::DECODER_GPU_CUDA) ? "GPU" : "FFmpeg";
    spdlog::info("Decoder type: {}, GPU ID: {}", decode_type_str, gpu_id);
    auto decoder = DecoderFactory::create(decoder_type, gpu_id);
    if (!decoder)
    {
        response->set_success(false);
        response->set_message("Failed to create requested decoder type");
        return grpc::Status::OK;
    }

    // 3. 创建编码器 (Encoder)
    // GPU 解码时使用 NVJPEG GPU 编码器，CPU 解码时使用 OpenCV
    std::shared_ptr<IImageEncoder> encoder;
    if (decoder_type == streamingservice::DECODER_GPU_CUDA)
    {
        encoder = std::make_shared<NvjpegEncoder>(85);
        spdlog::info("Using NVJPEG GPU encoder");
    }
    else
    {
        encoder = std::make_shared<OpencvEncoder>(85);
        spdlog::info("Using OpenCV CPU encoder");
    }

    // 4. 创建流任务 (StreamTask)
    // 将解码器和编码器都注入到任务中
    auto task = std::make_shared<StreamTask>(
        req_url,
        request->heartbeat_timeout_ms(),
        request->decode_interval_ms(),
        static_cast<int>(decoder_type),
        request->keep_on_failure(),
        std::move(decoder),
        encoder
    );

    std::string stream_id = generate_uuid();

    // 5. 再次加锁存入 Map (双重检查)
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        // 防止在创建 Task 的间隙，别的线程插入了同样的流
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                task->stop(); // 停止刚才新创建的冗余任务
                response->set_success(true);
                response->set_stream_id(pair.first);
                response->set_message("Stream created by another request concurrently.");
                return grpc::Status::OK;
            }
        }
        streams_[stream_id] = task;
    }
    // 如果打开流失败


    response->set_success(true);
    response->set_stream_id(stream_id);
    response->set_message("Stream started successfully");
    spdlog::info("[START] New Stream ID: {} | URL: {}", stream_id, req_url);
    return grpc::Status::OK;
}

// =============================================================
// StopStream：停止拉流
// =============================================================
grpc::Status RTSPServiceImpl::StopStream(grpc::ServerContext *context, const streamingservice::StopRequest *request, streamingservice::StopResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task_to_stop;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task_to_stop = it->second;
            streams_.erase(it); // 从 Map 中移除，引用计数 -1
        }
    }

    if (task_to_stop)
    {
        task_to_stop->stop(); // 在锁外执行耗时的停止操作
        spdlog::info("[STOP] Stream manually stopped: {}", stream_id);
        response->set_success(true);
        response->set_message("Stream stopped successfully");
    }
    else
    {
        response->set_success(false);
        response->set_message("Stream ID not found");
    }
    return grpc::Status::OK;
}

// =============================================================
// GetLatestFrame：获取最新帧
// =============================================================
grpc::Status RTSPServiceImpl::GetLatestFrame(grpc::ServerContext *context, const streamingservice::FrameRequest *request, streamingservice::FrameResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (!task)
    {
        response->set_success(false);
        response->set_message("Stream ID not found or expired");
        return grpc::Status::OK;
    }

    // 【核心修改】：直接获取预先编码好的 JPEG 字符串
    // 这里没有耗时的编码操作，只是内存拷贝，非常快
    std::string buffer;
    if (task->getLatestEncodedFrame(buffer))
    {
        response->set_success(true);
        response->set_image_data(buffer); // Protobuf 会处理 string 赋值
        response->set_message("OK");
    }
    else
    {
        response->set_success(false);
        response->set_message("No frame available yet or stream disconnected");
    }
    return grpc::Status::OK;
}

// =============================================================
// StreamFrames：流式传输视频帧
// =============================================================
grpc::Status RTSPServiceImpl::StreamFrames(grpc::ServerContext *context, 
                                            const streamingservice::StreamRequest *request, 
                                            grpc::ServerWriter<streamingservice::FrameResponse>* writer)
{
    std::string stream_id = request->stream_id();
    int max_fps = request->max_fps();
    
    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (!task)
    {
        streamingservice::FrameResponse response;
        response.set_success(false);
        response.set_message("Stream ID not found");
        writer->Write(response);
        return grpc::Status::OK;
    }

    // 计算帧间隔（微秒）
    int64_t frame_interval_us = 0;
    if (max_fps > 0)
    {
        frame_interval_us = 1000000LL / max_fps;
    }

    auto last_send_time = std::chrono::steady_clock::now();
    std::string last_frame_data;

    spdlog::info("[STREAM] Client connected to stream: {}, max_fps: {}", stream_id, max_fps);

    // 持续发送帧，直到客户端断开
    while (!context->IsCancelled())
    {
        // 帧率控制
        if (frame_interval_us > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_send_time).count();
            if (elapsed_us < frame_interval_us)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(frame_interval_us - elapsed_us));
            }
            last_send_time = std::chrono::steady_clock::now();
        }

        // 获取最新帧
        std::string encoded_frame;
        if (task->getLatestEncodedFrame(encoded_frame))
        {
            // 避免发送重复帧
            if (encoded_frame != last_frame_data)
            {
                streamingservice::FrameResponse response;
                response.set_success(true);
                response.set_image_data(encoded_frame);
                response.set_message("OK");

                if (!writer->Write(response))
                {
                    // 客户端断开
                    break;
                }

                last_frame_data = std::move(encoded_frame);
                task->keepAlive();
            }
            else
            {
                // 没有新帧，短暂休眠
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
        {
            // 流未连接或无数据
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    spdlog::info("[STREAM] Client disconnected from stream: {}", stream_id);
    return grpc::Status::OK;
}

// =============================================================
// CheckStream：查询流状态
// =============================================================
grpc::Status RTSPServiceImpl::CheckStream(grpc::ServerContext *context, const streamingservice::CheckRequest *request, streamingservice::CheckResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (task)
    {
        response->set_exists(true);
        response->set_status(static_cast<streamingservice::StreamStatus>(task->getStatus()));
        response->set_rtsp_url(task->getUrl());
        response->set_decoder_type(static_cast<streamingservice::DecoderType>(task->getDecoderType()));
        response->set_width(task->getWidth());
        response->set_height(task->getHeight());
        response->set_decode_interval_ms(task->getDecodeIntervalMs());
        
        switch (task->getStatus())
        {
            case StreamStatus::CONNECTED:
                response->set_message("已连接");
                break;
            case StreamStatus::CONNECTING:
                response->set_message("连接中");
                break;
            case StreamStatus::DISCONNECTED:
                response->set_message("无法连接");
                break;
        }
    }
    else
    {
        response->set_exists(false);
        response->set_status(streamingservice::STATUS_NOT_FOUND);
        response->set_message("流不存在");
    }
    return grpc::Status::OK;
}

// =============================================================
// ListStreams：查询所有流信息
// =============================================================
grpc::Status RTSPServiceImpl::ListStreams(grpc::ServerContext *context, const streamingservice::ListStreamsRequest *request, streamingservice::ListStreamsResponse *response)
{
    std::lock_guard<std::mutex> lock(map_mutex_);

    response->set_total_count(static_cast<int>(streams_.size()));

    for (const auto &pair : streams_)
    {
        const std::string &stream_id = pair.first;
        const std::shared_ptr<StreamTask> &task = pair.second;

        auto *stream_info = response->add_streams();
        stream_info->set_stream_id(stream_id);
        stream_info->set_rtsp_url(task->getUrl());
        stream_info->set_status(static_cast<streamingservice::StreamStatus>(task->getStatus()));
        stream_info->set_decoder_type(static_cast<streamingservice::DecoderType>(task->getDecoderType()));
        stream_info->set_width(task->getWidth());
        stream_info->set_height(task->getHeight());
        stream_info->set_decode_interval_ms(task->getDecodeIntervalMs());
    }

    spdlog::info("[LIST] Listed {} streams", streams_.size());
    return grpc::Status::OK;
}

// =============================================================
// cleanupLoop：后台线程，清理心跳超时或已停止的流
// =============================================================
void RTSPServiceImpl::cleanupLoop()
{
    while (manager_running_)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::vector<std::shared_ptr<StreamTask>> tasks_to_stop;

        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            for (auto it = streams_.begin(); it != streams_.end();)
            {
                bool should_remove = false;
                std::string reason;
                
                if (it->second->isStopped() && !it->second->shouldKeepOnFailure())
                {
                    // 任务已停止且未设置保留，自动清理
                    should_remove = true;
                    reason = "STOPPED";
                }
                else if (it->second->isTimeout())
                {
                    // 心跳超时，无论是否设置保留都清理
                    should_remove = true;
                    reason = "TIMEOUT";
                }
                
                if (should_remove)
                {
                    spdlog::info("[{}] Auto-cleaning stream ID: {}", reason, it->first);
                    tasks_to_stop.push_back(it->second);
                    it = streams_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 在锁外停止任务，防止死锁或阻塞主 Map
        for (auto &task : tasks_to_stop)
        {
            task->stop();
        }
    }
}