#pragma once
#include "interfaces.hpp"
#include <opencv2/opencv.hpp>

class OpencvEncoder : public IImageEncoder {
public:
    explicit OpencvEncoder(int quality = 85);
    bool encode(const cv::Mat& frame, std::string& out_buffer) override;

private:
    int quality_;
};