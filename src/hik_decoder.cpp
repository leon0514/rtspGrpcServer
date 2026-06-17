#include "hik_decoder.hpp"

#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

HikDecoder::HikDecoder() = default;

HikDecoder::~HikDecoder() {
    release();
}

bool HikDecoder::open(const std::string &url) {
    HikUrlInfo info = parseHikUrl(url);
    if (!info.valid) {
        spdlog::error("[HikDecoder] Invalid Hikvision URL: {}", url);
        return false;
    }

    // 如果已经登录且参数完全相同，避免 release()/Login() 造成的重复登录
    if (opened_ && cap_.IsConnected() &&
        ip_ == info.ip && port_ == info.port &&
        user_ == info.user && password_ == info.password &&
        channel_ == info.channel) {
        spdlog::debug("[HikDecoder] Already logged in to {}:{}/{}, skip re-open", ip_, port_, channel_);
        return true;
    }

    release();
    return open(info);
}

bool HikDecoder::open(const HikUrlInfo &info) {
    // 如果已经登录且参数完全相同，避免重复登录/登出
    if (opened_ && cap_.IsConnected() &&
        ip_ == info.ip && port_ == info.port &&
        user_ == info.user && password_ == info.password &&
        channel_ == info.channel) {
        spdlog::debug("[HikDecoder] Already logged in to {}:{}/{}, skip re-login", ip_, port_, channel_);
        return true;
    }

    release();

    spdlog::info("[HikDecoder] Logging in {}@{}:{}, channel {}", info.user, info.ip, info.port, info.channel);
    if (!cap_.Login(info.ip, info.port, info.user, info.password)) {
        spdlog::error("[HikDecoder] Failed to login {}@{}:{}", info.user, info.ip, info.port);
        return false;
    }

    ip_ = info.ip;
    port_ = info.port;
    user_ = info.user;
    password_ = info.password;
    channel_ = info.channel;
    opened_ = true;

    // 校验通道是否在线
    std::vector<int> online = cap_.GetOnlineChannels();
    if (!online.empty()) {
        bool found = false;
        for (int ch : online) {
            if (ch == channel_) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::string ch_list;
            for (size_t i = 0; i < online.size(); ++i) {
                if (i) ch_list += ", ";
                ch_list += std::to_string(online[i]);
            }
            spdlog::warn("[HikDecoder] Channel {} is NOT in online channel list. Available channels: {}",
                         channel_, ch_list);
            // 不直接返回 false，因为 GetOnlineChannels 可能因设备差异获取不全；
            // 但日志会提示用户检查通道号。
        } else {
            spdlog::info("[HikDecoder] Channel {} is online", channel_);
        }
    } else {
        spdlog::warn("[HikDecoder] Failed to get online channel list, continue anyway");
    }

    spdlog::info("[HikDecoder] Opened hik channel {}, device {}:{}", channel_, ip_, port_);
    return true;
}

bool HikDecoder::isOpened() const {
    return opened_ && cap_.IsConnected();
}

bool HikDecoder::grab() {
    if (!isOpened()) {
        spdlog::warn("[HikDecoder] grab() called but not opened");
        return false;
    }

    spdlog::debug("[HikDecoder] Grabbing channel {}", channel_);

    // 强制生成一个关键帧，避免拿到缓存旧图；失败不致命
    // 如果连续失败超过阈值，则禁用 ForceIFrame，避免日志刷屏
    if (!force_iframe_disabled_) {
        if (cap_.ForceIFrame(channel_)) {
            if (force_iframe_failures_ > 0) {
                spdlog::info("[HikDecoder] ForceIFrame recovered for channel {}", channel_);
            }
            force_iframe_failures_ = 0;
        } else {
            force_iframe_failures_++;
            if (force_iframe_failures_ >= 3) {
                force_iframe_disabled_ = true;
                spdlog::warn("[HikDecoder] ForceIFrame disabled for channel {} after {} failures",
                             channel_, force_iframe_failures_);
            } else {
                spdlog::warn("[HikDecoder] ForceIFrame failed for channel {} ({}/3), continue capture",
                             channel_, force_iframe_failures_);
            }
        }
    }

    // 给设备一点时间生成并编码新帧
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    if (!cap_.Capture(channel_, jpeg_buffer_)) {
        DWORD err = NET_DVR_GetLastError();
        spdlog::warn("[HikDecoder] Capture failed for channel {}, error {}", channel_, err);
        // 错误 47 (USERNOTEXIST) 等会话类错误才认为掉线，其它错误不重登录
        if (err == 47 || err == 1) {
            opened_ = false;
        }
        return false;
    }

    spdlog::debug("[HikDecoder] Captured {} bytes for channel {}", jpeg_buffer_.size(), channel_);

    // 预解码一帧，得到宽高并缓存 last_frame_，供 retrieve / getWidth / getHeight 使用
    cv::Mat raw(1, static_cast<int>(jpeg_buffer_.size()), CV_8UC1, jpeg_buffer_.data());
    cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
    if (!decoded.empty()) {
        last_frame_ = decoded;
        width_ = last_frame_.cols;
        height_ = last_frame_.rows;
    } else {
        spdlog::warn("[HikDecoder] Captured data is not a valid JPEG, size={}", jpeg_buffer_.size());
    }

    return true;
}

bool HikDecoder::retrieve(cv::Mat &frame, bool need_data) {
    if (jpeg_buffer_.empty()) {
        return false;
    }

    if (!need_data) {
        return true;
    }

    // grab() 已预解码并缓存，直接复用
    if (last_frame_.empty()) {
        cv::Mat raw(1, static_cast<int>(jpeg_buffer_.size()), CV_8UC1, jpeg_buffer_.data());
        cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
        if (decoded.empty()) {
            spdlog::error("[HikDecoder] Failed to decode JPEG, size={}", jpeg_buffer_.size());
            return false;
        }
        last_frame_ = decoded;
        width_ = last_frame_.cols;
        height_ = last_frame_.rows;
    }

    frame = last_frame_.clone();
    return true;
}

bool HikDecoder::getEncodedFrame(std::string &out_buffer) {
    if (jpeg_buffer_.empty()) {
        return false;
    }
    out_buffer.assign(jpeg_buffer_.data(), jpeg_buffer_.size());
    return true;
}

void HikDecoder::release() {
    cap_.Logout();
    opened_ = false;
    jpeg_buffer_.clear();
    last_frame_.release();
    width_ = 0;
    height_ = 0;
    force_iframe_failures_ = 0;
    force_iframe_disabled_ = false;
}
