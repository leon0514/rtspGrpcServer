#include "rtsp_service.hpp"
#include "decoder_factory.hpp"
#include "hik_url_parser.hpp"
#include "opencv_encoder.hpp"
#include "nvjpeg_encoder.hpp"
#include "utils.hpp"
#include "task_scheduler.hpp"
#include <malloc.h>
#include <sys/mman.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

#ifdef __linux__
#include <jemalloc/jemalloc.h>
#endif

RTSPServiceImpl::RTSPServiceImpl() : manager_running_(true)
{
    // 启动时清理上一次运行遗留的孤儿 SHM，防止服务崩溃/重启后 /dev/shm 累积
    cleanupOrphanedShm();
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
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
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
    const std::string &req_url = request->rtsp_url();
    auto decoder_type = request->decoder_type();
    int gpu_id = request->gpu_id();
    bool only_key_frames = request->only_key_frames();

    // 海康 SDK 模式：统一通过 rtsp_url 识别 hik://... 格式
    // decoder_type == HIK_SDK 或 URL 以 hik:// 开头都会触发海康 SDK
    std::string effective_url = req_url;
    HikUrlInfo hik_info = parseHikUrl(req_url);
    bool use_hik_sdk = (decoder_type == streamingservice::DECODER_HIK_SDK) || hik_info.valid;

    if (use_hik_sdk)
    {
        if (!hik_info.valid)
        {
            response->set_success(false);
            response->set_message("HIK_SDK decoder requires rtsp_url like hik://user:pass@ip:port/channel/101");
            return grpc::Status::OK;
        }
        effective_url = "hik://" + hik_info.user + ":" + hik_info.password +
                        "@" + hik_info.ip + ":" + std::to_string(hik_info.port) +
                        "/channel/" + std::to_string(hik_info.channel);
    }

    // 校验心跳超时：0 表示非法（会导致永远无法自动清理），过大则容易耗尽 SHM
    int effective_heartbeat_ms = request->heartbeat_timeout_ms();
    if (effective_heartbeat_ms <= 0)
    {
        effective_heartbeat_ms = 100000; // 默认 100 秒
    }
    constexpr int MAX_HEARTBEAT_MS = 3600000; // 1 小时上限
    if (effective_heartbeat_ms > MAX_HEARTBEAT_MS)
    {
        effective_heartbeat_ms = MAX_HEARTBEAT_MS;
        spdlog::warn("[StartStream] heartbeat_timeout_ms too large, capped at {}", MAX_HEARTBEAT_MS);
    }

    // 使用 url_to_id_ 实现 O(1) 的快速查找，替代原来的 O(N) 遍历
    {
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        auto it = url_to_id_.find(effective_url);
        if (it != url_to_id_.end())
        {
            std::string existing_id = it->second;
            auto stream_it = streams_.find(existing_id);
            if (stream_it != streams_.end())
            {
                stream_it->second->keepAlive();
                spdlog::info("[REUSE] URL already exists. Returning ID: {}", existing_id);
                response->set_success(true);
                response->set_stream_id(existing_id);
                response->set_message("Stream already exists, reusing existing task.");
                return grpc::Status::OK;
            }
            else
            {
                // 防御性编程：如果 streams_ 中没有，但 url_to_id_ 中有，说明状态不一致，清理掉脏数据
                lock.unlock();
                std::lock_guard<std::shared_mutex> write_lock(map_mutex_);
                url_to_id_.erase(it);
            }
        }
    }

    auto actual_decoder_type = use_hik_sdk ? streamingservice::DECODER_HIK_SDK : decoder_type;

    std::string decode_type_str;
    if (actual_decoder_type == streamingservice::DECODER_CPU_FFMPEG)
        decode_type_str = "FFmpeg";
    else if (actual_decoder_type == streamingservice::DECODER_GPU_NVCUVID)
        decode_type_str = "GPU";
    else if (actual_decoder_type == streamingservice::DECODER_HIK_SDK)
        decode_type_str = "HIK_SDK";
    else
        decode_type_str = "UNKNOWN";
    spdlog::info("Decoder type: {}, GPU ID: {}, Only key frames: {}", decode_type_str, gpu_id, only_key_frames ? "Yes" : "No");

    // 耗时操作：在无锁状态下创建解码器和编码器
    auto decoder = DecoderFactory::create(actual_decoder_type, gpu_id, only_key_frames);
    if (!decoder)
    {
        response->set_success(false);
        response->set_message("Failed to create requested decoder type");
        return grpc::Status::OK;
    }

    const int jpeg_quality = 85;
    bool use_gpu_encoder = (actual_decoder_type == streamingservice::DECODER_GPU_NVCUVID);
    // 海康 SDK 抓图返回的已经是 JPEG，这里先用 CPU 编码器复用现有路径（后续可优化为直接透传）
    if (actual_decoder_type == streamingservice::DECODER_HIK_SDK)
        use_gpu_encoder = false;
    spdlog::info("Using {} encoder", use_gpu_encoder ? "NVJPEG GPU" : "OpenCV CPU");

    std::string stream_id = generate_uuid();
    spdlog::info("Using Shared Memory: {}", request->use_shared_mem() ? "Enabled" : "Disabled");
    auto task = std::make_shared<StreamTask>(
        effective_url,
        stream_id,
        effective_heartbeat_ms,
        request->decode_interval_ms(),
        static_cast<int>(actual_decoder_type),
        gpu_id,
        request->keep_on_failure(),
        request->use_shared_mem(),
        std::move(decoder),
        use_gpu_encoder,
        jpeg_quality);

    // 海康 SDK 模式下保存用户指定的 RTSP 解码参数，便于后续切回 RTSP 时恢复
    if (use_hik_sdk)
    {
        task->setSavedDecoderType(static_cast<int>(decoder_type));
        task->setSavedGpuId(gpu_id);
    }

    // 双重检查锁定（Double-Checked Locking）
    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        // 使用 url_to_id_ 进行 O(1) 的并发冲突检查
        auto it = url_to_id_.find(effective_url);
        if (it != url_to_id_.end())
        {
            task->stop(); // 释放掉当前线程白建的资源
            response->set_success(true);
            response->set_stream_id(it->second);
            response->set_message("Stream created by another request concurrently.");
            return grpc::Status::OK;
        }

        // 必须同时更新两个 Map；统一使用 effective_url 作为键，避免海康 URL 重复建流
        streams_[stream_id] = task;
        url_to_id_[effective_url] = stream_id;
    }

    task->start();

    response->set_success(true);
    response->set_stream_id(stream_id);
    response->set_message("Stream started successfully");
    spdlog::info("[START] New Stream ID: {} | URL: {}", stream_id, effective_url);
    return grpc::Status::OK;
}

grpc::Status RTSPServiceImpl::StopStream(grpc::ServerContext *context, const streamingservice::StopRequest *request, streamingservice::StopResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task_to_stop;
    std::string url_to_remove;

    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task_to_stop = it->second;
            url_to_remove = task_to_stop->getUrl();
            streams_.erase(it);
            url_to_id_.erase(url_to_remove);
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
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
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
        response->set_frame_seq(-1);
        return grpc::Status::OK;
    }

    std::shared_ptr<std::string> frame_ptr;
    if (task->getLatestEncodedFrame(frame_ptr))
    {
        response->set_success(true);
        response->set_image_data(*frame_ptr);
        response->set_message("OK");
        response->set_frame_seq(task->getFrameSequence());
    }
    else
    {
        response->set_success(false);
        response->set_message("No frame available yet or stream disconnected");
        response->set_frame_seq(-1);
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
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
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
            resp.set_frame_seq(-1);
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
        response.set_frame_seq(client_seq);

        if (!writer->Write(response))
        {
            break;
        }
        response.Clear();

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
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
            task = it->second;
    }

    // 2. 获取响应中 StreamInfo 的可变指针
    auto *info = response->mutable_stream();
    info->set_stream_id(stream_id); // 建议把 ID 也填回去，方便客户端校验

    if (task)
    {
        // CheckStream 同时充当心跳保活
        task->keepAlive();

        // 填充基本信息
        info->set_status(static_cast<streamingservice::StreamStatus>(task->getStatus()));
        info->set_rtsp_url(task->getUrl());
        info->set_decoder_type(static_cast<streamingservice::DecoderType>(task->getDecoderType()));
        info->set_width(task->getWidth());
        info->set_height(task->getHeight());
        info->set_decode_interval_ms(task->getDecodeIntervalMs());
        info->set_only_key_frames(task->onlyKeyFrames());
        info->set_use_shared_mem(task->usesSharedMemory());
        info->set_heartbeat_timeout_ms(task->getHeartbeatTimeMs());
        info->set_keep_on_failure(task->shouldKeepOnFailure());

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
        default:
            response->set_message("未知状态");
            break;
        }
    }
    else
    {
        info->set_status(streamingservice::STATUS_NOT_FOUND);
        response->set_message("流不存在");
    }
    return grpc::Status::OK;
}

grpc::Status RTSPServiceImpl::ListStreams(grpc::ServerContext *context, const streamingservice::ListStreamsRequest *request, streamingservice::ListStreamsResponse *response)
{
    std::shared_lock<std::shared_mutex> lock(map_mutex_);
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
        stream_info->set_only_key_frames(task->onlyKeyFrames());
        stream_info->set_use_shared_mem(task->usesSharedMemory());
        stream_info->set_heartbeat_timeout_ms(task->getHeartbeatTimeMs());
        stream_info->set_keep_on_failure(task->shouldKeepOnFailure());

    }
    return grpc::Status::OK;
}

void RTSPServiceImpl::cleanupOrphanedShm()
{
    // Linux 下 POSIX shared memory 对象通常映射在 /dev/shm 目录
    constexpr const char *SHM_DIR = "/dev/shm";
    constexpr const char *SHM_PATTERN = "rtsp_grpc_*";
    constexpr const char *SEM_PATTERN = "sem.rtsp_grpc_*_notify";

    DIR *dir = opendir(SHM_DIR);
    if (!dir)
    {
        spdlog::warn("[RTSPServiceImpl] Failed to open {} for orphan SHM cleanup: {} ({})", SHM_DIR, errno, strerror(errno));
        return;
    }

    int unlinked_count = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (fnmatch(SHM_PATTERN, entry->d_name, 0) == 0 ||
            fnmatch(SEM_PATTERN, entry->d_name, 0) == 0)
        {
            // shm_unlink 只需要对象名（即 /dev/shm 下的文件名）
            if (shm_unlink(entry->d_name) == 0)
            {
                spdlog::info("[RTSPServiceImpl] Unlinked orphan SHM object: {}", entry->d_name);
                unlinked_count++;
            }
            else
            {
                if (errno != ENOENT)
                {
                    spdlog::warn("[RTSPServiceImpl] Failed to unlink orphan SHM object {}: {} ({})", entry->d_name, errno, strerror(errno));
                }
            }
        }
    }
    closedir(dir);

    if (unlinked_count > 0)
    {
        spdlog::warn("[RTSPServiceImpl] Cleaned up {} orphan SHM objects from previous run", unlinked_count);
    }
}

void RTSPServiceImpl::cleanupLoop()
{
    while (manager_running_)
    {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // 异常保护：任何单个 task 的清理异常都不能终止整个清理循环，否则 SHM 会泄漏
        try
        {
            std::vector<std::shared_ptr<StreamTask>> tasks_to_stop;

            {
                std::lock_guard<std::shared_mutex> lock(map_mutex_);
                // spdlog::info("Current active streams: {}", streams_.size());
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
                        std::string url = it->second->getUrl();
                        url_to_id_.erase(url);
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
                try
                {
                    task->stop();
                }
                catch (const std::exception &e)
                {
                    spdlog::error("[cleanupLoop] task->stop() exception: {}", e.what());
                }
                catch (...)
                {
                    spdlog::error("[cleanupLoop] task->stop() unknown exception");
                }
            }
        }
        catch (const std::exception &e)
        {
            spdlog::error("[cleanupLoop] outer exception: {}", e.what());
        }
        catch (...)
        {
            spdlog::error("[cleanupLoop] outer unknown exception");
        }
        // 🔧 强制命令 jemalloc 立即进行内存收缩 (Purge)
        // 这会告诉 jemalloc：把我所有 Dirty 页面全部交还给 OS
// #ifdef __linux__
//         mallctl("arena.0.purge", NULL, NULL, NULL, 0);
// #endif
    }
}

grpc::Status RTSPServiceImpl::UpdateStream(grpc::ServerContext *context,
                                           const streamingservice::UpdateStreamRequest *request,
                                           streamingservice::UpdateStreamResponse *response)
{
    std::string stream_id = request->stream_id();
    const std::string &new_url = request->new_rtsp_url();
    std::shared_ptr<StreamTask> task;

    // 解析新 URL，判断是否为海康协议
    HikUrlInfo hik_info = parseHikUrl(new_url);
    std::string effective_url = new_url;

    if (hik_info.valid)
    {
        effective_url = "hik://" + hik_info.user + ":" + hik_info.password +
                        "@" + hik_info.ip + ":" + std::to_string(hik_info.port) +
                        "/channel/" + std::to_string(hik_info.channel);
    }

    int target_decoder_type = streamingservice::DECODER_CPU_FFMPEG;
    int target_gpu_id = -1;
    bool use_gpu_encoder = false;

    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);

        // 1. 找到对应的 Task
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            response->set_success(false);
            response->set_message("Stream ID not found");
            return grpc::Status::OK;
        }
        task = it->second;

        // 2. 检查新 URL 是否已被其他 Task 占用 (防止重复)
        auto url_it = url_to_id_.find(effective_url);
        if (url_it != url_to_id_.end() && url_it->second != stream_id)
        {
            response->set_success(false);
            response->set_message("New URL already in use by another task");
            return grpc::Status::OK;
        }

        // 3. 重要：同步更新索引 Map
        std::string old_url = task->getUrl();
        if (old_url != effective_url)
        {
            url_to_id_.erase(old_url);              // 删掉旧索引
            url_to_id_[effective_url] = stream_id;  // 建立新索引
            spdlog::info("[MAP UPDATE] Swapped URL index for ID: {}", stream_id);
        }

        // 4. 在持有 task 后确定目标解码器类型
        //    海康 URL 强制 HIK_SDK；否则使用启动时保存的 RTSP 解码参数
        if (hik_info.valid)
        {
            target_decoder_type = streamingservice::DECODER_HIK_SDK;
            target_gpu_id = task->getSavedGpuId();
        }
        else
        {
            int saved_type = task->getSavedDecoderType();
            // 如果保存的类型是 HIK_SDK（用户未指定 RTSP 解码方式），默认回退到 CPU_FFMPEG
            target_decoder_type = (saved_type == streamingservice::DECODER_HIK_SDK)
                                      ? streamingservice::DECODER_CPU_FFMPEG
                                      : saved_type;
            target_gpu_id = task->getSavedGpuId();
        }
        use_gpu_encoder = (target_decoder_type == streamingservice::DECODER_GPU_NVCUVID);
    } // 锁在这里释放，避免后续 Task 处理阻塞 gRPC 响应

    // 4. 如果解码器类型发生变化，切换 decoder；否则只更新 URL
    if (target_decoder_type != task->getDecoderType())
    {
        spdlog::info("[UPDATE] Decoder type changed for stream {}: {} -> {}",
                     stream_id, task->getDecoderType(), target_decoder_type);
        auto new_decoder = DecoderFactory::create(
            static_cast<streamingservice::DecoderType>(target_decoder_type),
            target_gpu_id);
        if (!new_decoder)
        {
            response->set_success(false);
            response->set_message("Failed to create target decoder type");
            return grpc::Status::OK;
        }
        task->switchDecoder(target_decoder_type,
                            std::move(new_decoder),
                            effective_url,
                            use_gpu_encoder);
    }
    else
    {
        task->updateUrl(effective_url);
    }

    response->set_success(true);
    response->set_message("URL updated in management map, task will reconnect.");
    return grpc::Status::OK;
}