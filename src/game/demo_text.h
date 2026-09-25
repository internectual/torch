#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Expand Torque's one-based remote-message placeholders without allowing %1
// to consume the prefix of %10.
inline std::string formatDemoRemoteText(const std::string& templ,
                                        const std::vector<std::string>& values) {
    std::string text;
    text.reserve(templ.size());
    for (size_t i = 0; i < templ.size();) {
        if (templ[i] != '%') {
            text.push_back(templ[i++]);
            continue;
        }
        size_t end = i + 1;
        while (end < templ.size() && std::isdigit((unsigned char)templ[end])) ++end;
        if (end == i + 1) {
            text.push_back(templ[i++]);
            continue;
        }
        size_t index = 0;
        for (size_t digit = i + 1; digit < end; ++digit)
            index = index * 10 + (size_t)(templ[digit] - '0');
        if (index > 0 && index <= values.size()) text += values[index - 1];
        i = end;
    }
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20;
    }), text.end());
    return text;
}
