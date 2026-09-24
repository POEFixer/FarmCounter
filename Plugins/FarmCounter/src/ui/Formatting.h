#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace FcFormat {

// Compact active time. Map runs retain seconds below an hour when requested.
inline std::string Duration(double seconds, bool showSeconds = false) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const auto sec = static_cast<int64_t>((std::min)(seconds, 1.0e15));
    char buf[48];
    if (sec >= 86400)
        snprintf(buf, sizeof(buf), "%lldd %02lldh %02lldm",
                 static_cast<long long>(sec / 86400),
                 static_cast<long long>((sec / 3600) % 24),
                 static_cast<long long>((sec / 60) % 60));
    else if (sec >= 3600)
        snprintf(buf, sizeof(buf), "%lldh %02lldm",
                 static_cast<long long>(sec / 3600), static_cast<long long>((sec / 60) % 60));
    else if (showSeconds)
        snprintf(buf, sizeof(buf), "%02lld:%02lld",
                 static_cast<long long>(sec / 60), static_cast<long long>(sec % 60));
    else
        snprintf(buf, sizeof(buf), "%lldm", static_cast<long long>(sec / 60));
    return buf;
}

// At most three significant digits for XP; promote rounded 1000K to 1M.
inline std::string Count(double value, bool showPlus = false) {
    if (!std::isfinite(value)) return "--";
    double scaled = std::abs(value);
    if (scaled < 0.5) return "0";
    constexpr const char* suffix[] = { "", "K", "M", "B", "T" };
    int unit = 0;
    while (scaled >= 999.5 && unit < 4) { scaled /= 1000.0; ++unit; }
    char buf[48];
    if (scaled >= 999.5) {
        snprintf(buf, sizeof(buf), "%.2e", std::abs(value));
    } else {
        const int decimals = unit == 0 ? 0 : (scaled < 10.0 ? 2 : (scaled < 100.0 ? 1 : 0));
        snprintf(buf, sizeof(buf), "%.*f", decimals, scaled);
    }
    std::string result(buf);
    const auto exponent = result.find('e');
    std::string mantissa = result.substr(0, exponent);
    if (mantissa.find('.') != std::string::npos) {
        while (mantissa.back() == '0') mantissa.pop_back();
        if (mantissa.back() == '.') mantissa.pop_back();
    }
    result = mantissa + (exponent == std::string::npos ? suffix[unit] : result.substr(exponent));
    if (value < 0.0) result.insert(0, "-");
    else if (showPlus) result.insert(0, "+");
    return result;
}
}
