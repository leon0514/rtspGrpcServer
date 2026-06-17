#include "hik.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <thread>

// -----------------------------------------------------------
// SDK Manager 实现
// -----------------------------------------------------------
HikSDKManager& HikSDKManager::Instance() {
    static HikSDKManager instance;
    return instance;
}

HikSDKManager::HikSDKManager() : initialized_(false) {
    // SDK初始化
    if (NET_DVR_Init()) {
        initialized_ = true;
        // 设置连接超时时间 (毫秒)
        NET_DVR_SetConnectTime(2000, 1);
        // 设置重连机制 (间隔毫秒, 是否启用)
        NET_DVR_SetReconnect(10000, true);
        // 可选：设置日志
        // NET_DVR_SetLogToFile(3, "./sdkLog", false);
        spdlog::info("[HikSDK] Initialized successfully.");
    } else {
        spdlog::error("[HikSDK] Init failed. Err: {}", NET_DVR_GetLastError());
    }
}

HikSDKManager::~HikSDKManager() {
    if (initialized_) {
        NET_DVR_Cleanup();
        spdlog::info("[HikSDK] Cleanup.");
    }
}

// -----------------------------------------------------------
// HikSDKCap 实现
// -----------------------------------------------------------
HikSDKCap::HikSDKCap() : lUserID_(-1) {
    // 构造时确保 SDK 已初始化
    HikSDKManager::Instance();
}

HikSDKCap::~HikSDKCap() {
    Logout();
}

bool HikSDKCap::Login(const std::string& ip, int port, const std::string& user, const std::string& pwd) {
    if (lUserID_ >= 0) {
        spdlog::debug("[HikSDKCap] Login called while already connected, logout first");
        Logout();
    }

    if (!HikSDKManager::Instance().IsInitialized()) {
        spdlog::error("[HikSDKCap] SDK not initialized.");
        return false;
    }

    NET_DVR_USER_LOGIN_INFO struLoginInfo = {0};
    NET_DVR_DEVICEINFO_V40 struDeviceInfoV40 = {0};

    struLoginInfo.bUseAsynLogin = 0; // 同步登录
    strncpy(struLoginInfo.sDeviceAddress, ip.c_str(), NET_DVR_DEV_ADDRESS_MAX_LEN - 1);
    struLoginInfo.wPort = port;
    strncpy(struLoginInfo.sUserName, user.c_str(), NAME_LEN - 1);
    strncpy(struLoginInfo.sPassword, pwd.c_str(), NAME_LEN - 1);

    lUserID_ = NET_DVR_Login_V40(&struLoginInfo, &struDeviceInfoV40);

    if (lUserID_ < 0) {
        spdlog::error("[HikSDKCap] Login failed ({}). Err: {}", ip, NET_DVR_GetLastError());
        return false;
    }

    spdlog::info("[HikSDKCap] Login success. Device ID: {}", lUserID_);
    return true;
}

void HikSDKCap::Logout() {
    if (lUserID_ >= 0) {
        if (!NET_DVR_Logout(lUserID_)) {
             spdlog::warn("[HikSDKCap] Logout warning. Err: {}", NET_DVR_GetLastError());
        }
        lUserID_ = -1;
    }
}

std::vector<int> HikSDKCap::GetOnlineChannels() const {
    std::vector<int> channels;
    if (lUserID_ < 0) return channels;

    NET_DVR_IPPARACFG_V40 struIPAccessCfgV40 = {0};
    DWORD dwReturned = 0;

    // 获取IP通道资源配置
    if (!NET_DVR_GetDVRConfig(lUserID_, NET_DVR_GET_IPPARACFG_V40, 0, &struIPAccessCfgV40, sizeof(struIPAccessCfgV40), &dwReturned)) {
        spdlog::warn("[HikSDKCap] Get IP Config failed. Err: {}", NET_DVR_GetLastError());
        return channels;
    }

    // 遍历64个IP通道
    for (DWORD i = 0; i < struIPAccessCfgV40.dwDChanNum; i++) {
        // byEnable: 1-启用
        if (struIPAccessCfgV40.struStreamMode[i].uGetStream.struChanInfo.byEnable == 1) {
            // 起始数字通道号 + 索引
            int iChannelID = i + struIPAccessCfgV40.dwStartDChan;
            channels.push_back(iChannelID);
        }
    }
    return channels;
}

bool HikSDKCap::ForceIFrame(int channel, int stream_type) const {
    if (!IsConnected()) {
        return false;
    }

    // 先尝试请求的码流类型
    BOOL result = FALSE;
    DWORD err = 0;
    if (stream_type == 0) { // 主码流
        result = NET_DVR_MakeKeyFrame(lUserID_, channel);
    } else { // 子码流
        result = NET_DVR_MakeKeyFrameSub(lUserID_, channel);
    }

    if (!result) {
        err = NET_DVR_GetLastError();
        // 部分 NVR 对数字通道强制 I 帧会返回参数错误，尝试另一种码流
        if (stream_type == 0) {
            spdlog::debug("[HikSDKCap] MakeKeyFrame failed for channel {}, err={}, try sub stream", channel, err);
            result = NET_DVR_MakeKeyFrameSub(lUserID_, channel);
        } else {
            spdlog::debug("[HikSDKCap] MakeKeyFrameSub failed for channel {}, err={}, try main stream", channel, err);
            result = NET_DVR_MakeKeyFrame(lUserID_, channel);
        }
        if (!result) {
            err = NET_DVR_GetLastError();
            spdlog::debug("[HikSDKCap] ForceIFrame failed for channel {} finally, err={}", channel, err);
            return false;
        }
    }

    return true;
}


bool HikSDKCap::Capture(int channel, std::vector<char>& out_buffer) const {
    if (lUserID_ < 0) return false;

    NET_DVR_JPEGPARA strPicPara = {0};
    strPicPara.wPicQuality = 0; // 0-最好
    strPicPara.wPicSize = 0xff; // 0xff-Auto

    DWORD dwReturnedSize = 0;
    
    const size_t INITIAL_SIZE = 1024 * 1024; // 1MB
    if (out_buffer.size() < INITIAL_SIZE) {
        out_buffer.resize(INITIAL_SIZE); 
    }

    // 第一次尝试抓取
    // 注意：data() 返回指针，size() 是当前缓冲区大小
    BOOL bRet = NET_DVR_CaptureJPEGPicture_NEW(
        lUserID_, 
        channel, 
        &strPicPara, 
        out_buffer.data(), 
        static_cast<DWORD>(out_buffer.size()), 
        &dwReturnedSize
    );

    // 检查是否缓冲区过小 (SDK错误号 43: NET_DVR_BUFFER_OVERFLOW)
    // 某些旧版SDK可能不返回需要的 dwReturnedSize，这里做防御性扩容
    while (!bRet && NET_DVR_GetLastError() == NET_DVR_NOENOUGH_BUF) {
        size_t new_size = (dwReturnedSize > out_buffer.size()) ? dwReturnedSize : (out_buffer.size() * 2); // 最小扩到2MB
        out_buffer.resize(new_size);

        // 重试
        bRet = NET_DVR_CaptureJPEGPicture_NEW(
            lUserID_, 
            channel, 
            &strPicPara, 
            out_buffer.data(), 
            static_cast<DWORD>(out_buffer.size()), 
            &dwReturnedSize
        );
    }

    if (!bRet) {
        // 抓取失败，建议打印日志但不一定抛出异常
        spdlog::warn("[HikSDKCap] Capture failed Ch{} Err:{}", channel, NET_DVR_GetLastError());
        return false;
    }

    // 成功：调整 vector 的逻辑大小为实际图片大小
    // 这一步非常重要，否则 vector.size() 还是分配的大小，保存文件会多出垃圾数据
    out_buffer.resize(dwReturnedSize);
    
    return true;
}