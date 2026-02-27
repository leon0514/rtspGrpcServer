#pragma once
#include <string>
#include <opencv2/opencv.hpp>

// 视频解码器接口
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool open(const std::string& url) = 0;
    virtual bool isOpened() const = 0;
    virtual bool grab() = 0;
    virtual bool retrieve(cv::Mat& frame) = 0;
    virtual void release() = 0;
};

// 图像压缩编码器接口
class IImageEncoder {
public:
    virtual ~IImageEncoder() = default;
    virtual bool encode(const cv::Mat& frame, std::string& out_buffer) = 0;
};