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

    // 如果解码器本身已输出编码后的帧（如海康 SDK 抓图返回 JPEG），
    // 可实现此方法直接透传，避免解码再编码的二次损耗。默认不支持。
    virtual bool getEncodedFrame(std::string &out_buffer) { (void)out_buffer; return false; }

    // GPU 帧支持（可选实现）
    virtual bool isGpuFrame() const { return false; }
    virtual uint8_t* getGpuFramePtr() { return nullptr; }
    virtual int getWidth() const { return 0; }
    virtual int getHeight() const { return 0; }
    virtual bool onlyKeyFrames() const { return false; }

    // grab() 失败后是否立即 release() 并重新 open()。默认 true（RTSP/FFmpeg 行为）。
    // 海康 SDK 等短连接抓图模式可返回 false，由解码器内部决定是否重连，避免重复登录。
    virtual bool releaseOnGrabFailure() const { return true; }
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