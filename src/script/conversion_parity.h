#pragma once

#include <cstdint>
#include <string>

namespace ScriptConversionParity {
inline std::string decToBin(int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    int first = 31;
    while (first > 0 && ((bits >> first) & 1u) == 0u) --first;

    std::string result;
    result.reserve(static_cast<size_t>(32 - first));
    for (int bit = first; bit >= 0; --bit)
        result.push_back(((bits >> bit) & 1u) != 0u ? '1' : '0');
    return result;
}

inline int32_t binToDec(const std::string& text) {
    if (text.empty() || text.size() > 32) return 0;
    uint32_t value = 0;
    for (char c : text) {
        if (c != '0' && c != '1') return 0;
        value = (value << 1) | static_cast<uint32_t>(c - '0');
    }
    return static_cast<int32_t>(value);
}
} // namespace ScriptConversionParity
