#pragma once

#include <algorithm>
#include <cmath>

namespace GameTime {
constexpr float minScale = 0.0f;
constexpr float maxScale = 100.0f;

inline float clampScale(float scale) {
    // Torque treats an invalid time scale as the paused value rather than
    // allowing NaN to contaminate every simulation delta.
    if (!std::isfinite(scale)) return 0.0f;
    return std::clamp(scale, minScale, maxScale);
}

inline float scaledDelta(float dt, float scale) {
    if (!std::isfinite(dt)) return 0.0f;
    return std::max(0.0f, dt) * clampScale(scale);
}
}
