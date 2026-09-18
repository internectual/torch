#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

struct TextureFrameSource {
    std::string name;
    float duration = 1.0f;
};

// Parse Torque IFL lines without touching the filesystem. Durations are stored
// in seconds; malformed or absent durations retain the native one-second
// fallback. Missing frame files are filtered by the caller after parsing.
inline std::vector<TextureFrameSource> parseTextureFrameSources(const std::string& content) {
    std::vector<TextureFrameSource> result;
    size_t lineStart = 0;
    while (lineStart < content.size()) {
        size_t lineEnd = content.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = content.size();
        std::string line = content.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd < content.size() ? lineEnd + 1 : content.size();

        size_t comment = line.find_first_of("#;");
        size_t slashComment = line.find("//");
        if (slashComment != std::string::npos &&
            (comment == std::string::npos || slashComment < comment))
            comment = slashComment;
        if (comment != std::string::npos) line.resize(comment);
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line.erase(0, first);
        const size_t split = line.find_first_of(" \t\r");
        TextureFrameSource frame;
        frame.name = line.substr(0, split);
        if (frame.name.empty()) continue;
        if (split != std::string::npos) {
            const char* value = line.c_str() + split;
            char* end = nullptr;
            const float milliseconds = std::strtof(value, &end);
            if (end != value && milliseconds > 0.0f)
                frame.duration = milliseconds / 1000.0f;
        }
        result.push_back(std::move(frame));
    }
    return result;
}

// Torque IFL durations are milliseconds on disk. Callers store them in seconds.
inline size_t textureFrameIndex(const std::vector<float>& durations,
                                size_t frameCount, float age) {
    if (frameCount == 0) return 0;
    if (durations.size() != frameCount) {
        const float normalized = std::clamp(age, 0.0f, 0.999999f);
        return std::min(frameCount - 1,
                        (size_t)std::floor(normalized * frameCount));
    }
    float total = 0.0f;
    for (float duration : durations) total += std::max(0.0f, duration);
    if (total <= 0.0f) return 0;
    float time = std::fmod(std::max(0.0f, age), total);
    for (size_t i = 0; i < durations.size(); ++i) {
        time -= std::max(0.0f, durations[i]);
        if (time < 0.0f) return i;
    }
    return frameCount - 1;
}
