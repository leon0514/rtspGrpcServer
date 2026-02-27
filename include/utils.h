#pragma once
#include <string>
#include <random>

inline std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* digits = "0123456789abcdef";
    std::string res;
    for (int i = 0; i < 16; i++) res += digits[dis(gen)];
    return res;
}