#include "cpu_decoder.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

bool CpuDecoder::open(const std::string &url)
{
    last_url_ = url;

    // 每次尝试前先彻底清理资源，防止重连时泄漏
    release();

    // 1. 创建解封装器（auto_reboot=false，由上层 StreamTask 统一控制重连）
    demuxer_ = FFHDDemuxer::create_ffmpeg_demuxer(url, false, this->only_key_frames_);
    if (!demuxer_)
    {
        spdlog::warn("[CpuDecoder] Failed to create demuxer for {}", url);
        return false;
    }

    // 获取头部额外数据 (SPS/PPS 等)
    uint8_t *extra_data = nullptr;
    int extra_size = 0;
    demuxer_->get_extra_data(&extra_data, &extra_size);

    // 2. 创建软解码器
    decoder_ = FFHDDecoder::create_ffmpeg_decoder(
        static_cast<AVCodecID>(demuxer_->get_video_codec()),
        extra_data,
        extra_size);

    if (!decoder_)
    {
        spdlog::warn("[CpuDecoder] Failed to create decoder for {}", url);
        demuxer_.reset(); // 释放 demuxer
        return false;
    }

    is_opened_ = true;
    spdlog::info("[CpuDecoder] Successfully opened stream: {}", url);
    return true;
}

bool CpuDecoder::reconnect()
{
    // 重连策略统一由上层 StreamTask 控制，底层只负责重新 open 一次
    spdlog::warn("[CpuDecoder] Reconnecting to: {}", last_url_);
    return open(last_url_);
}

bool CpuDecoder::isOpened() const
{
    return is_opened_ && demuxer_ != nullptr && decoder_ != nullptr;
}

bool CpuDecoder::grab()
{
    if (!isOpened())
    {
        spdlog::warn("[CpuDecoder] grab() called but not opened");
        return false;
    }

    uint8_t *packet_data = nullptr;
    int packet_size = 0;
    bool is_key = false;

    while (true)
    {
        // 1. 优先尝试从解码器内部缓存区拉取已解码的帧（因为1个Packet可能解出多个Frame，或者B帧导致延迟）
        if (decoder_->receive_frame(&current_frame_))
        {
            frame_ready_.store(true, std::memory_order_release);
            return true; // 成功拿到一帧，退出 grab
        }

        // 2. 解码器内没有帧了，去 Demuxer 读新的数据包
        if (!demuxer_->demux(&packet_data, &packet_size, &last_pts_, &is_key))
        {
            // demux 失败直接返回，由上层 StreamTask 统一处理重连
            return false;
        }

        // 3. 将新读取的 Packet 送入解码器
        // 送入失败时跳过当前包，继续读取下一个包
        if (!decoder_->send_packet(packet_data, packet_size, last_pts_))
        {
            spdlog::warn("Invalid or non-decodable packet skipped for stream {}", last_url_);
            continue;
        }
    }

    return true;
}

bool CpuDecoder::retrieve(cv::Mat &frame, bool need_data)
{
    frames_to_skip_ = 0;
    if (!isOpened() || !frame_ready_.load(std::memory_order_acquire))
        return false;

    // 消费掉这一帧，标记为未准备，等待下一次 grab
    frame_ready_.store(false, std::memory_order_release);
    // 处理跳帧逻辑（预热防花屏）
    if (frames_to_skip_ > 0)
    {
        frames_to_skip_--;
        return false;
    }

    if (need_data && current_frame_)
    {
        int width = decoder_->get_width();
        int height = decoder_->get_height();

        // 防御编程：视频格式尚未解析时跳过
        if (width <= 0 || height <= 0)
        {
            spdlog::warn("Video format not yet parsed, width={}, height={}", width, height);
            return false;
        }

        // 创建对应大小的 cv::Mat
        frame.create(height, width, CV_8UC3);
        int bgr_linesize = width * 3;

        // 利用 FFmpegDecoder 内部的 sws_scale 将 YUV 转为 BGR 写进 cv::Mat 的内存中
        bool convert_ok = decoder_->convert_to_bgr(current_frame_, frame.data, bgr_linesize);
        if (!convert_ok)
        {
            spdlog::error("Failed to convert frame to BGR");
            return false;
        }
    }
    return true;
}

int CpuDecoder::getWidth() const
{
    return decoder_ ? decoder_->get_width() : 0;
}

int CpuDecoder::getHeight() const
{
    return decoder_ ? decoder_->get_height() : 0;
}

void CpuDecoder::release()
{
    is_opened_ = false;
    frame_ready_.store(false, std::memory_order_release);
    current_frame_ = nullptr;
    if (decoder_)
    {
        decoder_->send_packet(nullptr, 0);
        decoder_.reset();
    }

    if (demuxer_)
    {
        demuxer_.reset();
    }
}