#pragma once

#include <algorithm>
#include <cmath>
#include "core/math.h"

// Weather is advanced by the render tick. Invalid or hitch-sized deltas must
// not teleport drops or turn their positions into NaN values.
inline float precipitationDelta(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return 0.0f;
    return std::min(dt, 0.05f);
}

inline float precipitationFiniteOrZero(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

// A live drop keeps its randomized fall speed when wind changes. Only the
// change in wind's vertical component should be added to its current velocity.
inline Point3F precipitationWindAdjustedVelocity(const Point3F& velocity,
                                                 const Point3F& oldWind,
                                                 const Point3F& newWind) {
    return {precipitationFiniteOrZero(newWind.x),
            precipitationFiniteOrZero(velocity.y) +
                precipitationFiniteOrZero(newWind.y) -
                precipitationFiniteOrZero(oldWind.y),
            precipitationFiniteOrZero(newWind.z)};
}

inline Point3F precipitationAdvancedPosition(const Point3F& position,
                                              const Point3F& velocity, float dt) {
    const float safeDt = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    return {precipitationFiniteOrZero(position.x) +
                precipitationFiniteOrZero(velocity.x) * safeDt,
            precipitationFiniteOrZero(position.y) +
                precipitationFiniteOrZero(velocity.y) * safeDt,
            precipitationFiniteOrZero(position.z) +
                precipitationFiniteOrZero(velocity.z) * safeDt};
}

// Camera-following precipitation is tiled around the camera.  A single
// add/subtract wrap only works for small camera moves; mapper camera jumps and
// teleports can cross several tiles in one update.
inline float precipitationRecenterCoordinate(float coordinate, float camera,
                                             float width) {
    if (!std::isfinite(coordinate) || !std::isfinite(camera) ||
        !std::isfinite(width) || width <= 0.0f)
        return coordinate;
    float offset = std::fmod(coordinate - camera + width * 0.5f, width);
    if (offset < 0.0f) offset += width;
    return camera - width * 0.5f + offset;
}
