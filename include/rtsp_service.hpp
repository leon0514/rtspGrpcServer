#pragma once
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "stream_service.grpc.pb.h"
#include "stream_task.hpp"
#include "interfaces.hpp"

class RTSPServiceImpl final : public streamingservice::RTSPStreamService::Service
{
public:
    RTSPServiceImpl();
    ~RTSPServiceImpl();

    grpc::Status StartStream(grpc::ServerContext *context, const streamingservice::StartRequest *request, streamingservice::StartResponse *response) override;
    grpc::Status StopStream(grpc::ServerContext *context, const streamingservice::StopRequest *request, streamingservice::StopResponse *response) override;
    grpc::Status GetLatestFrame(grpc::ServerContext *context, const streamingservice::FrameRequest *request, streamingservice::FrameResponse *response) override;
    grpc::Status StreamFrames(grpc::ServerContext *context, const streamingservice::StreamRequest *request, grpc::ServerWriter<streamingservice::FrameResponse> *writer) override;
    grpc::Status CheckStream(grpc::ServerContext *context, const streamingservice::CheckRequest *request, streamingservice::CheckResponse *response) override;
    grpc::Status ListStreams(grpc::ServerContext *context, const streamingservice::ListStreamsRequest *request, streamingservice::ListStreamsResponse *response) override;

private:
    void cleanupLoop();

    std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamTask>> streams_;
    std::unordered_map<std::string, std::string> url_to_id_;
    std::atomic<bool> manager_running_;
    std::thread cleanup_thread_;
};