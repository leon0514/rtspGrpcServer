#include "opencv_encoder.hpp"
#include <vector>

OpencvEncoder::OpencvEncoder(int quality) : quality_(quality) {}

bool OpencvEncoder::encode(const cv::Mat& frame, std::string& out_buffer) {
    if (frame.empty()) return false;
    std::vector<uchar> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality_};
    
    if (cv::imencode(".jpg", frame, buf, params)) {
        out_buffer.assign(buf.begin(), buf.end());
        return true;
    }
    return false;
}