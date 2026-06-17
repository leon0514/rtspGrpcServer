#pragma once

#include <string>

/**
 * @brief 海康 URL 解析结果
 *
 * 与 SDK 无关，可在不链接海康库的情况下使用。
 */
struct HikUrlInfo {
    std::string ip;
    int port = 8000;
    std::string user;
    std::string password;
    int channel = 1;
    bool valid = false;
};

/**
 * @brief 解析海康 URL
 * @param url 形如 hik://user:password@ip:port/channel/101
 * @return HikUrlInfo valid=false 表示解析失败
 */
HikUrlInfo parseHikUrl(const std::string &url);
