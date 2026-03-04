#include "decoder_factory.hpp"
#include "cuda_decoder.hpp"
#include "cpu_decoder.hpp"
#include <iostream>

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type, int gpu_id)
{
    switch (type)
    {
    case streamingservice::DECODER_CPU_FFMPEG:
        return std::make_unique<CpuDecoder>();

    case streamingservice::DECODER_GPU_NVCUVID:
        return std::make_unique<CudaDecoder>(gpu_id);
        
    default:
        return std::make_unique<CpuDecoder>();
    }
}