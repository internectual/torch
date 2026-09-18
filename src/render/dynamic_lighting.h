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
    float falloff = 2.0f;
};

inline float dynamicLightFade(float age, float delay, float lifetime) {
    if (!std::isfinite(age) || !std::isfinite(delay) || !std::isfinite(lifetime) ||
        lifetime <= 0.0f || age < delay)
        return 0.0f;
    return std::clamp(1.0f - (age - delay) / lifetime, 0.0f, 1.0f);
}

inline float dynamicLightAttenuation(float distance, float radius, float falloff) {
    if (!std::isfinite(distance) || !std::isfinite(radius) || !std::isfinite(falloff) ||
        radius <= 0.0f || distance >= radius)
        return 0.0f;
    const float edge = std::clamp(1.0f - distance / radius, 0.0f, 1.0f);
    return std::pow(edge, std::max(0.1f, falloff));
}
