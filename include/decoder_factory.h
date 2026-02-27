#pragma once
#include <memory>
#include "interfaces.h"
#include "stream_service.grpc.pb.h"

class DecoderFactory {
public:
    // 根据 Protobuf 枚举创建对应的解码器
    static std::unique_ptr<IVideoDecoder> create(streamingservice::DecoderType type);
};