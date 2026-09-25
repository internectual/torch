#pragma once

#include <algorithm>
#include <cmath>

struct AuthoredFogVolume {
    float visibleDistance = 0.0f;
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    float percentage = 1.0f;
};

// Invalid mission fields must not reach the shader. A single NaN uniform can
// make the entire terrain pass render without fog until the mission changes.
inline bool fogVolumeIsUsable(const AuthoredFogVolume& volume) {
    return std::isfinite(volume.visibleDistance) && volume.visibleDistance > 0.0f &&
           std::isfinite(volume.minHeight) && std::isfinite(volume.maxHeight) &&
           volume.maxHeight > volume.minHeight && std::isfinite(volume.percentage);
}

inline float authoredFogContribution(const AuthoredFogVolume& volume,
                                     float cameraHeight, float fragmentHeight,
                                     float distance) {
    if (!fogVolumeIsUsable(volume) || !std::isfinite(cameraHeight) ||
        !std::isfinite(fragmentHeight) || !std::isfinite(distance) || distance < 0.0f)
        return 0.0f;
    const float low = std::max(std::min(cameraHeight, fragmentHeight), volume.minHeight);
    const float high = std::min(std::max(cameraHeight, fragmentHeight), volume.maxHeight);
    const float verticalSpan = high - low;
    const float factor = volume.percentage / volume.visibleDistance;
    if (verticalSpan > 0.0f && std::abs(fragmentHeight - cameraHeight) > 0.0001f)
        return distance * verticalSpan / std::abs(fragmentHeight - cameraHeight) * factor;
    return cameraHeight >= volume.minHeight && cameraHeight <= volume.maxHeight
        ? distance * factor : 0.0f;
}

inline float combineFogPrecedence(float haze, float volumeFog) {
    const float volume = std::clamp(volumeFog, 0.0f, 1.0f);
    return volume + std::min(std::max(haze, 0.0f), 1.0f - volume);
}
