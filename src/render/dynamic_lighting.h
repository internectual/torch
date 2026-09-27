#pragma once

#include <algorithm>
#include <cmath>

// Small, renderer-independent part of the dynamic light policy. Keeping this
// deterministic also lets effect code validate untrusted/network values before
// they reach OpenGL uniforms.
struct DynamicPointLight {
    float x{}, y{}, z{};
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float radius = 1.0f;
};

inline float dynamicLightFade(float age, float delay, float lifetime) {
    if (!std::isfinite(age) || !std::isfinite(delay) || !std::isfinite(lifetime) ||
        lifetime <= 0.0f || age < delay)
        return 0.0f;
    return std::clamp(1.0f - (age - delay) / lifetime, 0.0f, 1.0f);
}

// $pref::*::DynamicLightsClipPix / DynamicLightsFadePix defaults.
constexpr float DynamicLightsClipPix = 10.0f;
constexpr float DynamicLightsFadePix = 20.0f;

// TerrainRender::buildLightArray and InteriorInstance::renderObject skip a
// light whose projected radius is at most ClipPix pixels and fade it in up
// to FadePix. Shapes take GL lights, which are never faded.
inline float dynamicLightScreenFade(float projectedRadiusPixels) {
    if (!std::isfinite(projectedRadiusPixels) || projectedRadiusPixels <= DynamicLightsClipPix)
        return 0.0f;
    if (projectedRadiusPixels >= DynamicLightsFadePix) return 1.0f;
    return 1.0f - (DynamicLightsFadePix - projectedRadiusPixels) /
                  (DynamicLightsFadePix - DynamicLightsClipPix);
}

// The terrain/interior light pass projects a disc onto each plane: at plane
// distance d < R the disc has radius sqrt(R^2 - d^2) and alpha (R - d) / R.
// Returns the disc coordinate t in [0, 1) (0 at the centre) for a point
// `inPlane` from the disc centre, or a negative value outside the disc.
inline float dynamicLightDiscCoord(float planeDistance, float inPlane, float radius) {
    planeDistance = std::fabs(planeDistance);
    if (!(radius > 0.0f) || planeDistance >= radius) return -1.0f;
    const float disc = std::sqrt(radius * radius - planeDistance * planeDistance);
    const float t = inPlane / disc;
    return t < 1.0f ? t : -1.0f;
}

inline float dynamicLightDiscAlpha(float planeDistance, float radius) {
    planeDistance = std::fabs(planeDistance);
    if (!(radius > 0.0f) || planeDistance >= radius) return 0.0f;
    return (radius - planeDistance) / radius;
}
