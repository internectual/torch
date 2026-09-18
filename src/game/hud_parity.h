#pragma once

#include <algorithm>

namespace HudParity {
inline float messageAlpha(double age, double duration) {
    if (duration <= 0.0) return 1.0f;
    return std::clamp((float)(1.0 - age / duration), 0.0f, 1.0f);
}

inline float scoreboardHeaderY(float top, int teamRows) {
    return top + 86.0f + std::max(0, teamRows) * 16.0f;
}
} // namespace HudParity
