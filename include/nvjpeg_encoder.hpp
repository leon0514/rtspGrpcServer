#pragma once
#include "interfaces.hpp"
#include <opencv2/opencv.hpp>
#include <nvjpeg.h>
#include <cuda_runtime.h>

class NvjpegEncoder : public IImageEncoder {
public:
    explicit NvjpegEncoder(int quality = 85);
    ~NvjpegEncoder();
    
    bool encode(const cv::Mat& frame, std::string& out_buffer) override;
    
    // GPU 帧直接编码（零拷贝）
    bool encodeGpu(uint8_t* gpu_bgr_ptr, int width, int height, std::string& out_buffer) override;
    bool supportsGpuEncode() const override { return true; }

private:
    bool initialize();
    void cleanup();
    bool encodeInternal(uint8_t* gpu_ptr, int width, int height, int pitch, std::string& out_buffer);
    
    nvjpegHandle_t nvjpeg_handle_ = nullptr;
    nvjpegEncoderState_t encoder_state_ = nullptr;
    nvjpegEncoderParams_t encoder_params_ = nullptr;
    cudaStream_t stream_ = nullptr;
    
    // GPU 缓冲区（仅用于 CPU 输入时）
    uint8_t* d_input_ = nullptr;
    size_t d_input_size_ = 0;
    
    int quality_;
    bool initialized_ = false;
    
    // 缓存上一次的图像尺寸
    int last_width_ = 0;
    int last_height_ = 0;
};
