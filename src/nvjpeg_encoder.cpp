#include "nvjpeg_encoder.h"
#include <spdlog/spdlog.h>

#define CHECK_NVJPEG(call) \
    do { \
        nvjpegStatus_t status = call; \
        if (status != NVJPEG_STATUS_SUCCESS) { \
            spdlog::error("NVJPEG error: {} at {}:{}", status, __FILE__, __LINE__); \
            return false; \
        } \
    } while(0)

#define CHECK_CUDA(call) \
    do { \
        cudaError_t err = call; \
        if (err != cudaSuccess) { \
            spdlog::error("CUDA error: {} at {}:{}", cudaGetErrorString(err), __FILE__, __LINE__); \
            return false; \
        } \
    } while(0)

NvjpegEncoder::NvjpegEncoder(int quality) : quality_(quality) {
    initialize();
}

NvjpegEncoder::~NvjpegEncoder() {
    cleanup();
}

bool NvjpegEncoder::initialize() {
    if (initialized_) return true;
    
    // 创建 nvjpeg 句柄
    nvjpegStatus_t status = nvjpegCreateSimple(&nvjpeg_handle_);
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to create nvjpeg handle: {}", status);
        return false;
    }
    
    // 创建编码器状态
    status = nvjpegEncoderStateCreate(nvjpeg_handle_, &encoder_state_, nullptr);
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to create encoder state: {}", status);
        cleanup();
        return false;
    }
    
    // 创建编码参数
    status = nvjpegEncoderParamsCreate(nvjpeg_handle_, &encoder_params_, nullptr);
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to create encoder params: {}", status);
        cleanup();
        return false;
    }
    
    // 设置编码质量
    nvjpegEncoderParamsSetQuality(encoder_params_, quality_, nullptr);
    
    // 设置采样格式为 4:2:0 (默认 JPEG 格式)
    nvjpegEncoderParamsSetSamplingFactors(encoder_params_, NVJPEG_CSS_420, nullptr);
    
    // 创建 CUDA 流
    cudaError_t cuda_err = cudaStreamCreate(&stream_);
    if (cuda_err != cudaSuccess) {
        spdlog::error("Failed to create CUDA stream: {}", cudaGetErrorString(cuda_err));
        cleanup();
        return false;
    }
    
    initialized_ = true;
    spdlog::info("NVJPEG encoder initialized with quality {}", quality_);
    return true;
}

void NvjpegEncoder::cleanup() {
    if (d_input_) {
        cudaFree(d_input_);
        d_input_ = nullptr;
        d_input_size_ = 0;
    }
    
    if (stream_) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
    
    if (encoder_params_) {
        nvjpegEncoderParamsDestroy(encoder_params_);
        encoder_params_ = nullptr;
    }
    
    if (encoder_state_) {
        nvjpegEncoderStateDestroy(encoder_state_);
        encoder_state_ = nullptr;
    }
    
    if (nvjpeg_handle_) {
        nvjpegDestroy(nvjpeg_handle_);
        nvjpeg_handle_ = nullptr;
    }
    
    initialized_ = false;
}

bool NvjpegEncoder::encode(const cv::Mat& frame, std::string& out_buffer) {
    if (frame.empty()) return false;
    if (!initialized_ && !initialize()) return false;
    
    int width = frame.cols;
    int height = frame.rows;
    int channels = frame.channels();
    
    // 只支持 BGR 或 RGB 图像
    if (channels != 3) {
        spdlog::error("NVJPEG encoder only supports 3-channel images");
        return false;
    }
    
    size_t input_size = width * height * channels;
    
    // 如果缓冲区大小不够，重新分配
    if (d_input_size_ < input_size) {
        if (d_input_) {
            cudaFree(d_input_);
        }
        cudaError_t err = cudaMalloc(&d_input_, input_size);
        if (err != cudaSuccess) {
            spdlog::error("Failed to allocate GPU memory: {}", cudaGetErrorString(err));
            d_input_ = nullptr;
            d_input_size_ = 0;
            return false;
        }
        d_input_size_ = input_size;
    }
    
    // 将图像数据拷贝到 GPU
    cudaError_t err = cudaMemcpyAsync(d_input_, frame.data, input_size, cudaMemcpyHostToDevice, stream_);
    if (err != cudaSuccess) {
        spdlog::error("Failed to copy image to GPU: {}", cudaGetErrorString(err));
        return false;
    }
    
    return encodeInternal(d_input_, width, height, width * channels, out_buffer);
}

bool NvjpegEncoder::encodeGpu(uint8_t* gpu_bgr_ptr, int width, int height, std::string& out_buffer) {
    if (!gpu_bgr_ptr || width <= 0 || height <= 0) return false;
    if (!initialized_ && !initialize()) return false;
    
    // 直接使用 GPU 指针，零拷贝！
    return encodeInternal(gpu_bgr_ptr, width, height, width * 3, out_buffer);
}

bool NvjpegEncoder::encodeInternal(uint8_t* gpu_ptr, int width, int height, int pitch, std::string& out_buffer) {
    // 设置输入图像
    nvjpegImage_t nv_image;
    nv_image.channel[0] = gpu_ptr;
    nv_image.pitch[0] = pitch;
    for (int i = 1; i < NVJPEG_MAX_COMPONENT; i++) {
        nv_image.channel[i] = nullptr;
        nv_image.pitch[i] = 0;
    }
    
    // 编码 (BGR interleaved 格式)
    nvjpegStatus_t status = nvjpegEncodeImage(
        nvjpeg_handle_,
        encoder_state_,
        encoder_params_,
        &nv_image,
        NVJPEG_INPUT_BGRI,  // BGR interleaved
        width,
        height,
        stream_
    );
    
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to encode image: {}", status);
        return false;
    }
    
    // 获取编码后数据大小（不需要在这里同步，nvjpegEncodeRetrieveBitstream 会处理）
    size_t length = 0;
    status = nvjpegEncodeRetrieveBitstream(
        nvjpeg_handle_,
        encoder_state_,
        nullptr,
        &length,
        stream_
    );
    
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to get bitstream size: {}", status);
        return false;
    }
    
    // 分配输出缓冲区并获取数据
    out_buffer.resize(length);
    status = nvjpegEncodeRetrieveBitstream(
        nvjpeg_handle_,
        encoder_state_,
        reinterpret_cast<uint8_t*>(&out_buffer[0]),
        &length,
        stream_
    );
    
    if (status != NVJPEG_STATUS_SUCCESS) {
        spdlog::error("Failed to retrieve bitstream: {}", status);
        return false;
    }
    
    cudaStreamSynchronize(stream_);
    
    last_width_ = width;
    last_height_ = height;
    
    return true;
}
