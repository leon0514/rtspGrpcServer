#include "hik_url_parser.hpp"

#include <spdlog/spdlog.h>
#include <cstdlib>

HikUrlInfo parseHikUrl(const std::string &url) {
    HikUrlInfo info;
    const std::string prefix = "hik://";
    if (url.rfind(prefix, 0) != 0) {
        return info; // valid 保持 false
    }

    std::string rest = url.substr(prefix.size());

    // Split credentials and host part by '@'
    auto at_pos = rest.find('@');
    if (at_pos == std::string::npos) {
        spdlog::error("[HikUrlParser] Missing credentials in URL: {}", url);
        return info;
    }
    std::string creds = rest.substr(0, at_pos);
    std::string host_part = rest.substr(at_pos + 1);

    auto colon_pos = creds.find(':');
    if (colon_pos == std::string::npos) {
        spdlog::error("[HikUrlParser] Missing password in credentials: {}", creds);
        return info;
    }
    info.user = creds.substr(0, colon_pos);
    info.password = creds.substr(colon_pos + 1);

    // Split host:port and path
    std::string addr_part;
    std::string path_part;
    auto slash_pos = host_part.find('/');
    if (slash_pos == std::string::npos) {
        addr_part = host_part;
        path_part.clear();
    } else {
        addr_part = host_part.substr(0, slash_pos);
        path_part = host_part.substr(slash_pos + 1);
    }

    auto port_colon = addr_part.find(':');
    if (port_colon == std::string::npos) {
        info.ip = addr_part;
        info.port = 8000;
    } else {
        info.ip = addr_part.substr(0, port_colon);
        int port = std::atoi(addr_part.substr(port_colon + 1).c_str());
        if (port <= 0 || port > 65535) {
            spdlog::error("[HikUrlParser] Invalid port in URL: {}", url);
            return info;
        }
        info.port = port;
    }

    // Path expected: channel/<number>
    const std::string channel_prefix = "channel/";
    if (path_part.rfind(channel_prefix, 0) == 0) {
        info.channel = std::atoi(path_part.substr(channel_prefix.size()).c_str());
    } else {
        spdlog::warn("[HikUrlParser] No channel in URL, default to 1: {}", url);
        info.channel = 1;
    }

    if (info.ip.empty() || info.user.empty() || info.password.empty()) {
        spdlog::error("[HikUrlParser] IP, user or password is empty: {}", url);
        return info;
    }

    info.valid = true;
    return info;
}
