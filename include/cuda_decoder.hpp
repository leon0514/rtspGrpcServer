#pragma once
#include "interfaces.hpp"
#include <opencv2/opencv.hpp>
#include <memory>
#include <queue>

// 假设这些头文件在您的包含路径中
#include "ffmpeg_demuxer.hpp"
#include "cuvid_decoder.hpp"
#include "cuda_tools.hpp"

class CudaDecoder : public IVideoDecoder
{
public:
    CudaDecoder(int gpu_id = 0) : gpu_id_(gpu_id) {}
    virtual ~CudaDecoder() { release(); }

    bool open(const std::string &url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat &frame, bool need_data = true) override;
    void release() override;
    
    // GPU 帧支持
    bool isGpuFrame() const override { return true; }
    uint8_t* getGpuFramePtr() override { return last_gpu_frame_ptr_; }
    int getWidth() const override;
    int getHeight() const override;
    
    // GPU ID
    int getGpuId() const { return gpu_id_; }

private:
    std::shared_ptr<FFHDDemuxer::FFmpegDemuxer> demuxer_;
    std::shared_ptr<FFHDDecoder::CUVIDDecoder> decoder_;

    int gpu_id_ = 0;              // GPU ID
    bool is_opened_ = false;
    int64_t last_pts_ = 0;
    unsigned int last_frame_index_ = 0;
    uint8_t* last_gpu_frame_ptr_ = nullptr;  // 最后一帧的 GPU 指针

    // 用于处理解码器延迟（多个输入包才产出一帧，或一个包产出多帧的情况）
    int decoded_frames_available_ = 0;

private:
    std::string last_url_;    // 保存 URL 用于重连
    int frames_to_skip_ = 15; // 待丢弃帧计数
    bool reconnect();         // 重连辅助函数
};