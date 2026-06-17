#include "hik_decoder.hpp"

#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

HikDecoder::HikDecoder() = default;

HikDecoder::~HikDecoder() {
    release();
}

bool HikDecoder::parseJpegSize(const std::vector<char> &buffer, int &width, int &height) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(buffer.data());
    size_t n = buffer.size();
    size_t i = 0;
    while (i + 1 < n) {
        if (p[i] != 0xFF) {
            ++i;
            continue;
        }
        unsigned char marker = p[i + 1];
        if (marker == 0xD8) { // SOI
            i += 2;
            continue;
        }
        if (marker == 0xD9) { // EOI
            break;
        }
        // 跳过填充字节 0xFF
        if (marker == 0xFF) {
            ++i;
            continue;
        }
        // SOF0/SOF1/SOF2: 包含宽高
        if ((marker == 0xC0 || marker == 0xC1 || marker == 0xC2) && i + 9 < n) {
            height = (p[i + 5] << 8) | p[i + 6];
            width  = (p[i + 7] << 8) | p[i + 8];
            return true;
        }
        // 其他 marker：跳过 segment length
        if (i + 3 < n) {
            size_t seg_len = (static_cast<size_t>(p[i + 2]) << 8) | static_cast<size_t>(p[i + 3]);
            i += 2 + seg_len;
        } else {
            break;
        }
    }
    return false;
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
    first_frame_after_open_ = true;
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

    spdlog::info("[HikDecoder] Grabbing channel {}", channel_);

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

    spdlog::info("[HikDecoder] Captured {} bytes for channel {}", jpeg_buffer_.size(), channel_);

    // 刚 open 后的第一帧可能是设备缓存旧图，丢弃并重新抓一帧
    if (first_frame_after_open_) {
        first_frame_after_open_ = false;
        if (!cap_.ForceIFrame(channel_)) {
            spdlog::info("[HikDecoder] ForceIFrame on first real capture failed, continue");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!cap_.Capture(channel_, jpeg_buffer_)) {
            DWORD err = NET_DVR_GetLastError();
            spdlog::warn("[HikDecoder] Second capture failed for channel {}, error {}", channel_, err);
            if (err == 47 || err == 1) {
                opened_ = false;
            }
            return false;
        }
        spdlog::info("[HikDecoder] Captured second frame {} bytes for channel {}", jpeg_buffer_.size(), channel_);
    }

    // 只轻量解析 JPEG 头获取宽高，避免完整解码消耗 CPU
    // gRPC 透传模式完全不需要解码；SHM / retrieve 按需完整解码
    if (!parseJpegSize(jpeg_buffer_, width_, height_)) {
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

    // 按需完整解码 JPEG（SHM 模式需要），解码结果缓存供下次复用
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
    first_frame_after_open_ = false;
}
