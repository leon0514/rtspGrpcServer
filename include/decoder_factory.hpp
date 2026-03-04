#pragma once
#include <memory>
#include "interfaces.hpp"
#include "stream_service.grpc.pb.h"

class DecoderFactory {
public:
    // 根据 Protobuf 枚举创建对应的解码器
    // gpu_id: 仅对 GPU 解码有效，默认 0
    static std::unique_ptr<IVideoDecoder> create(streamingservice::DecoderType type, int gpu_id = 0);
};