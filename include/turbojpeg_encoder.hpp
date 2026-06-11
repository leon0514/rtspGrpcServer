#pragma once
#include "interfaces.hpp"
#include <turbojpeg.h>

class TurboJpegEncoder : public IImageEncoder {
public:
    explicit TurboJpegEncoder(int quality = 85);
    ~TurboJpegEncoder();

    bool encode(const cv::Mat& frame, std::string& out_buffer) override;

private:
    int quality_;
    tjhandle handle_;
};
