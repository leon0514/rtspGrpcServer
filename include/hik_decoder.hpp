#pragma once

#include "interfaces.hpp"
#include "hik.hpp"
#include "hik_url_parser.hpp"

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief 海康 SDK 解码器实现
 *
 * 将海康 NVR 的 JPEG 抓图能力封装为 IVideoDecoder 接口，
 * 复用现有的 StreamTask 生命周期、gRPC/SHM 帧分发机制。
 *
 * 支持的 URL 格式：
 *   hik://user:password@ip:port/channel/101
 *
 * 也可以不传 URL，直接在 StartStream 的 hik_* 字段中指定参数。
 */
class HikDecoder : public IVideoDecoder {
public:
    HikDecoder();
    ~HikDecoder() override;

    HikDecoder(const HikDecoder&) = delete;
    HikDecoder& operator=(const HikDecoder&) = delete;

    bool open(const std::string &url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat &frame, bool need_data = true) override;
    bool getEncodedFrame(std::string &out_buffer) override;
    void release() override;

    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }
    bool onlyKeyFrames() const override { return false; }
    bool releaseOnGrabFailure() const override { return false; }

private:
    bool open(const HikUrlInfo &info);

    HikSDKCap cap_;
    std::string ip_;
    int port_ = 8000;
    std::string user_;
    std::string password_;
    int channel_ = 1;

    std::vector<char> jpeg_buffer_;
    cv::Mat last_frame_;
    int width_ = 0;
    int height_ = 0;
    bool opened_ = false;

    // ForceIFrame 失败次数统计，连续失败多次后禁用，避免日志刷屏
    int force_iframe_failures_ = 0;
    bool force_iframe_disabled_ = false;
};
