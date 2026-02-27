#pragma once
#include "interfaces.h"
#include <opencv2/opencv.hpp>

class OpencvDecoder : public IVideoDecoder {
public:
    bool open(const std::string& url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat& frame) override;
    void release() override;

private:
    cv::VideoCapture cap_;
};