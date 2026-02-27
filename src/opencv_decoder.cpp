#include "opencv_decoder.h"

bool OpencvDecoder::open(const std::string& url) {
    cap_.open(url, cv::CAP_FFMPEG);
    if (cap_.isOpened()) {
        cap_.set(cv::CAP_PROP_BUFFERSIZE, 1); // 极低延迟配置
        return true;
    }
    return false;
}

bool OpencvDecoder::isOpened() const { return cap_.isOpened(); }
bool OpencvDecoder::grab() { return cap_.grab(); }
bool OpencvDecoder::retrieve(cv::Mat& frame) { return cap_.retrieve(frame); }
void OpencvDecoder::release() { cap_.release(); }