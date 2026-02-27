#include "decoder_factory.h"
#include "opencv_decoder.h"
#include <iostream>

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type) {
    switch (type) {
        case streamingservice::DECODER_CPU_OPENCV:
            return std::make_unique<OpencvDecoder>();
            
        case streamingservice::DECODER_GPU_CUDA:
            // TODO: 未来实现 CudaDecoder 并在这里返回
            std::cerr << "[WARN] CUDA Decoder not implemented yet. Falling back to CPU OpencvDecoder.\n";
            return std::make_unique<OpencvDecoder>();
            
        case streamingservice::DECODER_FFMPEG_NATIVE:
            // TODO: 未来实现 FFmpegDecoder 并在这里返回
            std::cerr << "[WARN] FFmpeg Decoder not implemented yet. Falling back to CPU OpencvDecoder.\n";
            return std::make_unique<OpencvDecoder>();
            
        default:
            return std::make_unique<OpencvDecoder>();
    }
}