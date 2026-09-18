#pragma once

#include "core/math.h"
#include <cmath>
#include <string>
#include <algorithm>
#include <cctype>

// Validation shared by the TorqueScript environment natives and their tests.
inline bool applyFogDistance(float& distance, float& density, float value) {
    if (!std::isfinite(value) || value <= 0.0f) return false;
    distance = value;
    density = 1.0f / value;
    return true;
}

inline bool applyFogDensity(float& distance, float& density, float value) {
    if (!std::isfinite(value) || value < 0.0f) return false;
    density = value;
    distance = value > 0.0f ? 1.0f / value : 0.0f;
    return true;
}

inline bool applyFogColor(ColorF& color, float r, float g, float b, float a) {
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
        return false;
    color = {r, g, b, a};
    return true;
}

inline bool validEnvironmentColor(const ColorF& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) &&
           std::isfinite(color.b) && std::isfinite(color.a) &&
           color.r >= 0.0f && color.r <= 1.0f &&
           color.g >= 0.0f && color.g <= 1.0f &&
           color.b >= 0.0f && color.b <= 1.0f &&
           color.a >= 0.0f && color.a <= 1.0f;
}

inline bool validSunDirection(const Point3F& direction) {
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z)) return false;
    return direction.x * direction.x + direction.y * direction.y +
           direction.z * direction.z > 0.000001f;
}

inline bool applyWaterLevel(float& level, float value) {
    if (!std::isfinite(value)) return false;
    level = value;
    return true;
}

inline bool applyWaterOpacity(float& opacity, float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
    opacity = value;
    return true;
}

inline bool applyWaterColor(ColorF& color, float r, float g, float b) {
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ||
        r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f)
        return false;
    color.r = r;
    color.g = g;
    color.b = b;
    return true;
}

inline bool parseWaterType(const std::string& input, int& type) {
    std::string value = input;
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    static constexpr const char* names[] = {
        "water", "oceanwater", "riverwater", "stagnantwater",
        "lava", "hotlava", "crustylava", "quicksand"
    };
    for (int i = 0; i < 8; ++i) {
        if (value == names[i]) { type = i; return true; }
    }
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '7') {
        type = value[0] - '0';
        return true;
    }
    return false;
}

inline bool validPrecipitationSettings(int type, float percentage) {
    return type >= 0 && type <= 7 && std::isfinite(percentage) &&
           percentage >= 0.0f && percentage <= 1.0f;
}

inline int precipitationDropCount(int configuredDrops, float percentage) {
    if (configuredDrops <= 0 || !validPrecipitationSettings(0, percentage) || percentage == 0.0f)
        return 0;
    return std::max(1, (int)std::lround(configuredDrops * percentage));
}
