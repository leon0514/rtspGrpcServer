#include "turbojpeg_encoder.hpp"
#include <spdlog/spdlog.h>

TurboJpegEncoder::TurboJpegEncoder(int quality)
    : quality_(quality), handle_(nullptr)
{
    handle_ = tjInitCompress();
    if (!handle_)
    {
        spdlog::error("TurboJPEG encoder init failed: {}", tjGetErrorStr2(nullptr));
    }
}

TurboJpegEncoder::~TurboJpegEncoder()
{
    if (handle_)
    {
        tjDestroy(handle_);
        handle_ = nullptr;
    }
}

bool TurboJpegEncoder::encode(const cv::Mat& frame, std::string& out_buffer)
{
    if (!handle_)
    {
        spdlog::error("TurboJPEG encoder not initialized");
        return false;
    }
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0)
    {
        return false;
    }
    if (frame.channels() != 3)
    {
        spdlog::error("TurboJPEG encoder only supports 3-channel BGR images");
        return false;
    }

    unsigned char* jpeg_buf = nullptr;
    unsigned long jpeg_size = 0;

    int width = frame.cols;
    int height = frame.rows;
    int pitch = static_cast<int>(frame.step[0]);

    int ret = tjCompress2(
        handle_,
        frame.data,
        width,
        pitch,
        height,
        TJPF_BGR,
        &jpeg_buf,
        &jpeg_size,
        TJSAMP_420,
        quality_,
        TJFLAG_FASTDCT);

    if (ret < 0)
    {
        spdlog::error("TurboJPEG encode failed: {}", tjGetErrorStr2(handle_));
        return false;
    }

    out_buffer.assign(reinterpret_cast<const char*>(jpeg_buf), jpeg_size);
    tjFree(jpeg_buf);
    return true;
}
