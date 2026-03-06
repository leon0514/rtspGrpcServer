#include "rtsp_service.hpp"
#include "decoder_factory.hpp"
#include "opencv_encoder.hpp"
#include "nvjpeg_encoder.hpp"
#include "utils.hpp"
#include "task_scheduler.hpp"
#include <malloc.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

RTSPServiceImpl::RTSPServiceImpl() : manager_running_(true)
{
    cleanup_thread_ = std::thread(&RTSPServiceImpl::cleanupLoop, this);
}

RTSPServiceImpl::~RTSPServiceImpl()
{
    manager_running_ = false;
    if (cleanup_thread_.joinable())
    {
        cleanup_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second)
            {
                pair.second->stop();
            }
        }
    }
}

grpc::Status RTSPServiceImpl::StartStream(grpc::ServerContext *context, const streamingservice::StartRequest *request, streamingservice::StartResponse *response)
{
    std::string req_url = request->rtsp_url();

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                std::string existing_id = pair.first;
                pair.second->keepAlive();
                spdlog::info("[REUSE] URL already exists. Returning ID: {}", existing_id);
                response->set_success(true);
                response->set_stream_id(existing_id);
                response->set_message("Stream already exists, reusing existing task.");
                return grpc::Status::OK;
            }
        }
    }

    auto decoder_type = request->decoder_type();
    int gpu_id = request->gpu_id();
    std::string decode_type_str = (decoder_type == streamingservice::DECODER_CPU_FFMPEG) ? "FFmpeg" : (decoder_type == streamingservice::DECODER_GPU_NVCUVID) ? "GPU"
                                                                                                                                                              : "UNKONW";
    spdlog::info("Decoder type: {}, GPU ID: {}", decode_type_str, gpu_id);

    auto decoder = DecoderFactory::create(decoder_type, gpu_id);
    if (!decoder)
    {
        response->set_success(false);
        response->set_message("Failed to create requested decoder type");
        return grpc::Status::OK;
    }

    std::shared_ptr<IImageEncoder> encoder;
    if (decoder_type == streamingservice::DECODER_GPU_NVCUVID)
    {
        encoder = std::make_shared<NvjpegEncoder>(85, gpu_id);
        spdlog::info("Using NVJPEG GPU encoder");
    }
    else
    {
        encoder = std::make_shared<OpencvEncoder>(85);
        spdlog::info("Using OpenCV CPU encoder");
    }

    auto task = std::make_shared<StreamTask>(
        req_url,
        request->heartbeat_timeout_ms(),
        request->decode_interval_ms(),
        static_cast<int>(decoder_type),
        gpu_id,
        request->keep_on_failure(),
        std::move(decoder),
        encoder);

    std::string stream_id = generate_uuid();

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                task->stop();
                response->set_success(true);
                response->set_stream_id(pair.first);
                response->set_message("Stream created by another request concurrently.");
                return grpc::Status::OK;
            }
        }
        streams_[stream_id] = task;
    }

    task->start();

    response->set_success(true);
    response->set_stream_id(stream_id);
    response->set_message("Stream started successfully");
    spdlog::info("[START] New Stream ID: {} | URL: {}", stream_id, req_url);
    return grpc::Status::OK;
}

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
            streams_.erase(it);
        }
    }

    if (task_to_stop)
    {
        task_to_stop->stop();
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

    std::shared_ptr<std::string> frame_ptr;
    if (task->getLatestEncodedFrame(frame_ptr))
    {
        response->set_success(true);
        response->set_image_data(*frame_ptr);
        response->set_message("OK");
    }
    else
    {
        response->set_success(false);
        response->set_message("No frame available yet or stream disconnected");
    }
    return grpc::Status::OK;
}

grpc::Status RTSPServiceImpl::StreamFrames(grpc::ServerContext *context,
                                           const streamingservice::StreamRequest *request,
                                           grpc::ServerWriter<streamingservice::FrameResponse> *writer)
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

    int64_t frame_interval_us = 0;
    if (max_fps > 0)
    {
        frame_interval_us = 1000000LL / max_fps;
    }

    uint64_t client_seq = 0;
    auto last_send_time = std::chrono::steady_clock::now() - std::chrono::hours(1);

    spdlog::info("[STREAM] Client connected to stream: {}, max_fps: {}", stream_id, max_fps);

    std::shared_ptr<std::string> encoded_frame;
    streamingservice::FrameResponse response;

    while (!context->IsCancelled())
    {
        if (task->isStopped())
        {
            streamingservice::FrameResponse resp;
            resp.set_success(false);
            resp.set_message("Stream has been stopped");
            writer->Write(resp);
            break;
        }

        bool has_new_frame = task->waitForNextFrame(encoded_frame, client_seq, 500);

        if (!has_new_frame)
        {
            continue;
        }

        if (frame_interval_us > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_send_time).count();
            if (elapsed_us < frame_interval_us)
            {
                continue;
            }
            last_send_time = now;
        }

        response.set_success(true);
        response.set_image_data(*encoded_frame);
        response.set_message("OK");

        if (!writer->Write(response))
        {
            break;
        }

        task->keepAlive();
    }

    spdlog::info("[STREAM] Client disconnected from stream: {}", stream_id);
    return grpc::Status::OK;
}

grpc::Status RTSPServiceImpl::CheckStream(grpc::ServerContext *context, const streamingservice::CheckRequest *request, streamingservice::CheckResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
            task = it->second;
    }

    if (task)
    {
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
        response->set_status(streamingservice::STATUS_NOT_FOUND);
        response->set_message("流不存在");
    }
    return grpc::Status::OK;
}

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
    return grpc::Status::OK;
}

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
                    should_remove = true;
                    reason = "STOPPED";
                }
                else if (it->second->isTimeout())
                {
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

        for (auto &task : tasks_to_stop)
        {
            task->stop();
        }
        // 🔧 修复：强制通知 Glibc 将多线程产生的缓存碎片内存交还给操作系统
        // 这对于流媒体这种高频大内存申请/释放的 C++ 服务至关重要
#ifdef __linux__
        malloc_trim(0);
#endif
    }
}