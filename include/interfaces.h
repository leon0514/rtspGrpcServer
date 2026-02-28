#pragma once
#include <string>
#include <opencv2/opencv.hpp>
#include <cstdint>

// 视频解码器接口
class IVideoDecoder
{
public:
    virtual ~IVideoDecoder() = default;
    virtual bool open(const std::string &url) = 0;
    virtual bool isOpened() const = 0;
    virtual bool grab() = 0;
    virtual bool retrieve(cv::Mat &frame, bool need_data = true) = 0;
    virtual void release() = 0;
    
    // GPU 帧支持（可选实现）
    virtual bool isGpuFrame() const { return false; }
    virtual uint8_t* getGpuFramePtr() { return nullptr; }
    virtual int getWidth() const { return 0; }
    virtual int getHeight() const { return 0; }
};

// 图像压缩编码器接口
class IImageEncoder
{
public:
    virtual ~IImageEncoder() = default;
    virtual bool encode(const cv::Mat &frame, std::string &out_buffer) = 0;
    
    // GPU 帧编码支持（可选实现）
    virtual bool encodeGpu(uint8_t* gpu_bgr_ptr, int width, int height, std::string &out_buffer) {
        return false; // 默认不支持
    }
    virtual bool supportsGpuEncode() const { return false; }
};