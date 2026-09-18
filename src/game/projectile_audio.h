#pragma once

#include <algorithm>
#include <cmath>

namespace ProjectileAudio {
inline unsigned fireProfile(bool underwater, unsigned dry, unsigned wet) {
    return underwater && wet ? wet : dry;
}

inline unsigned explosionProfile(bool underwater, unsigned dry, unsigned wet) {
    return underwater && wet ? wet : dry;
}

inline float attenuation(float distance, float reference, float maximum) {
    if (!std::isfinite(distance) || !std::isfinite(reference) || !std::isfinite(maximum) ||
        reference <= 0.0f || maximum < reference || distance >= maximum) return 0.0f;
    if (distance <= reference) return 1.0f;
    return std::clamp(reference / distance, 0.0f, 1.0f);
}
}
