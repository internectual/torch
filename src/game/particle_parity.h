#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <vector>

struct ParticleKeyInterpolation {
    size_t lower = 0;
    size_t upper = 0;
    float fraction = 0.0f;
};

// Particle simulation is presentation-only: a paused or malformed frame must
// not move effects or propagate NaNs into their render state.
inline bool particleTickIsUsable(float dt) {
    return std::isfinite(dt) && dt > 0.0f;
}

inline float particleLifetimeAfterTick(float lifetime, float dt) {
    if (!std::isfinite(lifetime)) return 0.0f;
    if (!particleTickIsUsable(dt)) return lifetime;
    return lifetime - dt;
}

// Debris is a world effect, so it follows the mission's gravity rather than
// using a platform-specific Earth constant.  gravModifier is authored on the
// debris datablock and may intentionally reverse or amplify gravity.
inline float debrisGravityAcceleration(float worldGravity, float gravModifier) {
    if (!std::isfinite(worldGravity)) worldGravity = -20.0f;
    if (!std::isfinite(gravModifier)) gravModifier = 1.0f;
    return worldGravity * gravModifier;
}

// Particle tracks clamp at their first and last authored keys.
inline ParticleKeyInterpolation particleKeyInterpolation(
    float normalizedAge, const std::vector<float>& times) {
    if (times.empty()) return {};
    // A malformed track must not make the final lookup jump back to an older
    // key.  Stock particle tracks are ordered; use the valid prefix when a
    // script supplies a non-finite or descending key time.
    if (!std::isfinite(times.front())) return {};
    size_t validCount = 1;
    while (validCount < times.size() &&
           std::isfinite(times[validCount]) &&
           times[validCount] >= times[validCount - 1])
        ++validCount;
    // A newly spawned effect can be evaluated once before its clock is
    // initialized.  Treat an invalid age as the first key instead of letting
    // NaN bypass every comparison and snap the particle to its final key.
    if (!std::isfinite(normalizedAge)) normalizedAge = 0.0f;
    normalizedAge = std::clamp(normalizedAge, 0.0f, 1.0f);
    if (normalizedAge <= times.front()) return {};
    for (size_t upper = 1; upper < validCount; ++upper) {
        if (normalizedAge <= times[upper]) {
            const float span = times[upper] - times[upper - 1];
            const float fraction = span > 0.0f
                ? std::clamp((normalizedAge - times[upper - 1]) / span, 0.0f, 1.0f)
                : 0.0f;
            return {upper - 1, upper, fraction};
        }
    }
    const size_t last = validCount - 1;
    return {last, last, 0.0f};
}
