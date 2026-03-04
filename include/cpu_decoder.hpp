#pragma once

#include "interfaces.hpp" // 继承 IVideoDecoder
#include <string>
#include <memory>
#include <opencv2/opencv.hpp>

// 假设这些头文件在您的包含路径中
#include "ffmpeg_demuxer.hpp"
#include "ffmpeg_decoder.hpp"

class CpuDecoder : public IVideoDecoder
{
public:
    CpuDecoder() = default;
    virtual ~CpuDecoder() { release(); }

    // 实现 IVideoDecoder 核心接口
    bool open(const std::string &url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat &frame, bool need_data = true) override;
    void release() override;
    
    // 获取分辨率
    int getWidth() const override;
    int getHeight() const override;

    // CPU 帧属性 (覆写基类方法以声明身份)
    bool isGpuFrame() const override { return false; }
    uint8_t* getGpuFramePtr() override { return nullptr; }

private:
    bool reconnect(); // 重连辅助函数

private:
    std::string last_url_;    // 保存 URL 供后续重连使用
    int frames_to_skip_ = 5;  // 待丢弃帧计数（CPU端通常不需要丢弃太多）
    bool is_opened_ = false;

    // FFMPEG 模块
    std::shared_ptr<FFHDDemuxer::FFmpegDemuxer> demuxer_;
    std::shared_ptr<FFHDDecoder::FFmpegDecoder> decoder_;

    // 内部状态缓冲
    int64_t last_pts_ = 0;
    AVFrame* current_frame_ = nullptr; // 保存当前 grab 到的帧
    bool frame_ready_ = false;         // 标志位：是否有一帧准备好被 retrieve
};