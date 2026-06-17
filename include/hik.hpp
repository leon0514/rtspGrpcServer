#ifndef HIKNVRCAP_HPP
#define HIKNVRCAP_HPP

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include "HCNetSDK.h"

// -----------------------------------------------------------
// 全局 SDK 管理器 (单例模式)
// 负责 SDK 的全局初始化、清理和异常回调设置
// -----------------------------------------------------------
class HikSDKManager {
public:
    static HikSDKManager& Instance();
    bool IsInitialized() const { return initialized_; }

private:
    HikSDKManager();
    ~HikSDKManager();
    HikSDKManager(const HikSDKManager&) = delete;
    HikSDKManager& operator=(const HikSDKManager&) = delete;

    bool initialized_;
};

// -----------------------------------------------------------
// NVR 设备操作类
// 支持多线程并发调用 Capture
// -----------------------------------------------------------
class HikSDKCap {
public:
    HikSDKCap();
    ~HikSDKCap();

    // 禁止拷贝，防止重复 Logout
    HikSDKCap(const HikSDKCap&) = delete;
    HikSDKCap& operator=(const HikSDKCap&) = delete;

    /**
     * @brief 登录 NVR
     * @param ip 设备IP
     * @param port 端口 (通常为 8000)
     * @param user 用户名
     * @param pwd 密码
     * @return 成功返回 true
     */
    bool Login(const std::string& ip, int port, const std::string& user, const std::string& pwd);

    /**
     * @brief 登出 (析构时会自动调用)
     */
    void Logout();

    /**
     * @brief 获取在线通道列表
     */
    std::vector<int> GetOnlineChannels() const;


    /**
     * @brief 强制NVR为指定通道的码流生成一个I帧 (关键帧)。
     * @details 在调用 Capture 之前调用此函数，可以确保获取到的是最新的实时图像，
     *          而不是缓存中的旧图像。这对于解决短时间内连续抓图返回相同图片的问题非常有效。
     *          调用此函数后，建议稍作延时（例如50-100毫秒），再调用 Capture。
     * @param channel 通道号
     * @param stream_type 码流类型 (0: 主码流, 1: 子码流)。默认为主码流。
     * @return 成功返回 true，失败返回 false
     */
    bool ForceIFrame(int channel, int stream_type = 0) const;

    /**
     * @brief 抓图 (线程安全，无锁设计)
     * @param channel 通道号
     * @param out_buffer [输出/输入] 图片数据容器。
     *                   如果传入的 vector 容量足够，将复用内存；不足则自动扩容。
     * @return 成功返回 true
     */
    bool Capture(int channel, std::vector<char>& out_buffer) const;

    // 检查是否已登录
    bool IsConnected() const { return lUserID_ >= 0; }

private:
    long lUserID_;
};

#endif // HIKNVRCAP_HPP