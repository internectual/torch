#pragma once
// The engine's per-object projected shadows (Tribes2.exe shadow.cc:
// Shadow::prepare, build, the light basis, the tables and
// setShadowDetailLevel), as ported by t2-mapper shadowProjection.ts. Pure
// math in Torch's Y-up world space.
#include "core/math.h"
#include <cmath>

namespace ShadowProjection {

// Receiver polys are gathered up to this many shape radii along the light.
inline constexpr float ReachFactor = 10.0f;
// The generic blob is drawn with 0.4 x the shape radius.
inline constexpr float GenericRadiusScale = 0.4f;
// Smallest on-screen radius that still casts at full detail.
inline constexpr float MinPixels = 6.0f;
// Fade-in begins when the minimum is more than half the projected size.
inline constexpr float FadeStart = 0.5f;
// Shadows this faded are not drawn at all.
inline constexpr float FadeCutoff = 0.99f;
// Receivers must face the light: normal . lightDir < -0.05.
inline constexpr float ReceiverFacing = -0.05f;
// The 32-px generic blob: alpha 180/255 x (1 - r^2) inside the unit disc.
inline constexpr float GenericAlpha = 180.0f / 255.0f;

// The shadow light is a constant, not the mission sun: Torque
// (0.57, 0.57, -0.57), the way the light travels.
inline Point3F constantLightDir() {
    const Point3F d = Math::torquePointToYUp({0.57f, 0.57f, -0.57f});
    const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return {d.x / l, d.y / l, d.z / l};
}

struct TileSpec {
    float minPixels;
    float intervalMs;  // silhouette refresh period
    int size;          // silhouette bitmap size; 0 = the generic blob
    bool blur;         // 3x3 blur of the mask
};

// Bitmap size / refresh by projected radius.
inline constexpr TileSpec TileTable[4] = {
    {130.0f, 25.0f, 64, true},
    {25.0f, 100.0f, 64, true},
    {10.0f, 100.0f, 32, false},
    {0.0f, 0.0f, 0, false},
};

inline TileSpec tileSpec(float px) {
    for (const TileSpec& spec : TileTable)
        if (px >= spec.minPixels) return spec;
    return TileTable[3];
}

// Projected radius in pixels of a sphere `radius` at `dist`.
inline float projectedRadiusPx(float radius, float dist, float viewportHeight, float fovDeg) {
    if (!(dist > 1e-3f)) return INFINITY;
    const float pixelScale = viewportHeight * 0.5f / std::tan(fovDeg * 3.14159265f / 360.0f);
    return radius / dist * pixelScale;
}

struct Visibility { bool visible; float fade; };

// Projected px x (0.5 level + 0.5) must reach max(smallestVisible,
// MinPixels); the fade grows linearly once the minimum exceeds half the
// projected size. `fade` is 1 - alpha scale.
inline Visibility visibility(float px, float smallestVisiblePx = 0.0f, float level = 1.0f) {
    const float minPx = std::fmax(smallestVisiblePx, MinPixels);
    const float scaled = px * (0.5f * level + 0.5f);
    if (!(scaled >= minPx)) return {false, 1.0f};
    const float ratio = minPx / scaled;
    const float fade = ratio > FadeStart ? (ratio - FadeStart) * 2.0f : 0.0f;
    return {fade < FadeCutoff, fade};
}

// Camera-distance table (dist, tilt, reachLoss): past 100 m the light tilts
// to vertical (fully by 500 m) and the reach shrinks to half at 500 m and
// 30% at 1 km.
struct DistanceFade { float tilt, reachLoss; };
inline DistanceFade distanceFade(float dist) {
    static constexpr float table[4][3] = {{0, 0, 0}, {100, 0, 0}, {500, 1, 0.5f}, {1000, 1, 0.7f}};
    if (dist <= table[0][0]) return {table[0][1], table[0][2]};
    for (int i = 1; i < 4; ++i) {
        if (dist <= table[i][0]) {
            const float f = (dist - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
            return {table[i - 1][1] + (table[i][1] - table[i - 1][1]) * f,
                    table[i - 1][2] + (table[i][2] - table[i - 1][2]) * f};
        }
    }
    return {table[3][1], table[3][2]};
}

// The light for a caster `dist` from the camera: the constant direction
// pulled toward straight down by the tilt (vertical component becomes
// v k - (1 - k), k = 1 - tilt).
inline Point3F lightDir(float dist) {
    Point3F d = constantLightDir();
    const float k = 1.0f - distanceFade(dist).tilt;
    if (k < FadeCutoff) {
        d.y = d.y * k - (1.0f - k);
        const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        d = {d.x / l, d.y / l, d.z / l};
    }
    return d;
}

// Light basis: `dir` along the light, lateral x = dir x up, z = x x dir.
// The silhouette camera and the receiver projection share it.
struct Basis { Point3F x, dir, z, center; };
inline Basis lightBasis(const Point3F& dir, const Point3F& center) {
    auto cross = [](const Point3F& a, const Point3F& b) {
        return Point3F{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    auto normalize = [](Point3F v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-8f ? Point3F{v.x / l, v.y / l, v.z / l} : v;
    };
    Basis b;
    b.dir = dir;
    b.center = center;
    if (std::fabs(dir.y) <= 0.99f) {
        b.x = normalize(cross(dir, {0, 1, 0}));
        b.z = cross(b.x, dir);
    } else {
        // Near-vertical light: seed z from the light's horizontal part.
        b.z = normalize({0.0f, dir.z, -dir.y});
        if (std::fabs(b.z.y) + std::fabs(b.z.z) < 1e-6f) b.z = {0, 0, 1};
        b.x = normalize(cross(dir, b.z));
        b.z = cross(b.x, dir);
    }
    return b;
}

// World point in light space: (lateral x, along the light, lateral z).
inline Point3F toLight(const Basis& b, const Point3F& p) {
    const Point3F d{p.x - b.center.x, p.y - b.center.y, p.z - b.center.z};
    return {d.x * b.x.x + d.y * b.x.y + d.z * b.x.z,
            d.x * b.dir.x + d.y * b.dir.y + d.z * b.dir.z,
            d.x * b.z.x + d.y * b.z.y + d.z * b.z.z};
}

} // namespace ShadowProjection
