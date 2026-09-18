#pragma once

#include <algorithm>

namespace GameTime {
constexpr float minScale = 0.0f;
constexpr float maxScale = 100.0f;

inline float clampScale(float scale) {
    return std::clamp(scale, minScale, maxScale);
}

inline float scaledDelta(float dt, float scale) {
    return std::max(0.0f, dt) * clampScale(scale);
}
}
