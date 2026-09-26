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

inline constexpr size_t MaxTextureFrames = 1024;

// Parse Torque IFL lines without touching the filesystem (TSShape::
// readIflMaterials): "name [count]", tabs as spaces, count in frames of
// 1/30 s, absent or zero meaning one frame. Durations are stored in
// seconds. Missing frame files are filtered by the caller after parsing.
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
        int frames = split != std::string::npos ? std::atoi(line.c_str() + split) : 1;
        if (frames <= 0) frames = 1;
        frame.duration = (float)frames / 30.0f;
        result.push_back(std::move(frame));
        if (result.size() >= MaxTextureFrames) break;
    }
    return result;
}

// IFL durations are in seconds (converted from 1/30 s frames).
inline size_t textureFrameIndex(const std::vector<float>& durations,
                                 size_t frameCount, float age) {
    if (frameCount == 0) return 0;
    // A mission/demo transition can briefly leave an animation clock invalid.
    // Keep the renderer on its first frame instead of converting NaN or
    // infinity to an implementation-defined vector index.
    const float safeAge = std::isfinite(age) ? std::max(0.0f, age) : 0.0f;
    if (durations.size() != frameCount) {
        // Missing IFL durations use Torque's one-second-per-frame default and
        // continue cycling rather than freezing on the last frame.
        const float time = std::fmod(safeAge, (float)frameCount);
        return std::min(frameCount - 1, (size_t)std::floor(time));
    }
    float total = 0.0f;
    for (float duration : durations) {
        // Torque treats an invalid IFL duration like an omitted duration.
        // Do not let one malformed entry make the whole animation stall.
        if (!(duration > 0.0f) || !std::isfinite(duration)) duration = 1.0f;
        total += duration;
    }
    if (total <= 0.0f) return 0;
    float time = std::fmod(safeAge, total);
    for (size_t i = 0; i < durations.size(); ++i) {
        const float duration = durations[i] > 0.0f && std::isfinite(durations[i])
            ? durations[i] : 1.0f;
        time -= duration;
        if (time < 0.0f) return i;
    }
    return frameCount - 1;
}
