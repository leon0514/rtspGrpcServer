#include "decoder_factory.hpp"
#include "cuda_decoder.hpp"
#include "cpu_decoder.hpp"
#include "cuda_tools.hpp"
#include <iostream>
#include <spdlog/spdlog.h>

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type, int gpu_id, bool only_key_frames)
{
    switch (type)
    {
    case streamingservice::DECODER_CPU_FFMPEG:
        return std::make_unique<CpuDecoder>(only_key_frames);

    case streamingservice::DECODER_GPU_NVCUVID:
        if (!CUDATools::check_device_id(gpu_id))
        {
            spdlog::error("[DecoderFactory] Invalid gpu_id {}, falling back to CPU decoder", gpu_id);
            return std::make_unique<CpuDecoder>(only_key_frames);
        }
        return std::make_unique<CudaDecoder>(gpu_id, only_key_frames);
        
    default:
        spdlog::warn("[DecoderFactory] Unknown decoder type {}, falling back to CPU decoder", type);
        return std::make_unique<CpuDecoder>(only_key_frames);
    }
}
