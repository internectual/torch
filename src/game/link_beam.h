#pragma once

#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <vector>

// ELF / repair link beams (ELFProjectile / RepairProjectile renderObject,
// shared ribbon builder): the beam bows through the shooter's aim point, a
// quadratic from the muzzle through muzzle + aim x length to the target.
inline Point3F linkBeamSample(const Point3F& start, const Point3F& control,
                              const Point3F& end, float t) {
    const float a = (1.0f - t) * (1.0f - t), b = 2.0f * (1.0f - t) * t, c = t * t;
    return {a * start.x + b * control.x + c * end.x,
            a * start.y + b * control.y + c * end.y,
            a * start.z + b * control.z + c * end.z};
}

inline Point3F linkBeamControl(const Point3F& start, const Point3F& aim, float length) {
    return {start.x + aim.x * length, start.y + aim.y * length, start.z + aim.z * length};
}

// The player look-direction override of getRenderMuzzleVector in Tribes 2
// (binary-verified by t2-mapper): body yaw plus head yaw, and head pitch
// (positive looks down), the networked head angles scaled by
// PlayerData::maxLookAngle. Torque-space direction.
inline Point3F playerAimDirection(float bodyYaw, float headYaw, float headPitch,
                                  float maxLookAngle) {
    const float yaw = bodyYaw + headYaw * maxLookAngle;
    const float pitch = std::clamp(headPitch * maxLookAngle, -1.5f, 1.5f);
    return {std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch)};
}

// RepairProjectile::renderObject: visible while 90 - dot * 90 <= cutoff
// (deliberately not acos).
inline bool repairWithinCutoff(const Point3F& start, const Point3F& end,
                               const Point3F& aim, float cutoffAngle) {
    Point3F d{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(length > 0.0f)) return false;
    const float dot = (d.x * aim.x + d.y * aim.y + d.z * aim.z) / length;
    return 90.0f - dot * 90.0f <= cutoffAngle;
}

// RepairProjectile::advanceTime: the first hit snaps the endpoint, later
// frames ease toward the latest hit at 2 * dt, and a miss keeps the last.
struct RepairEndpoint {
    Point3F current{}, desired{};
    bool hasHit = false;
    void step(bool hit, const Point3F& point, float dt) {
        if (hit) {
            desired = point;
            if (!hasHit) { current = point; hasHit = true; return; }
        }
        if (!hasHit) return;
        const float k = std::clamp(2.0f * dt, 0.0f, 1.0f);
        current = {current.x + (desired.x - current.x) * k,
                   current.y + (desired.y - current.y) * k,
                   current.z + (desired.z - current.z) * k};
    }
};

// ShockLanceProjectile lightning (Tribes2.exe point generator): round(
// density x length) points (at most 50) spaced along +X, each but the pinned
// ends displaced by a random unit vector x amp. `random` returns [0, 1).
template <typename Random>
inline std::vector<Point3F> shockLightningPoints(float length, float density, float amp, Random&& random) {
    const int requested = (int)std::lround(density * length);
    const int count = std::min(requested, 50);
    std::vector<Point3F> points;
    if (count <= 0) return points;
    const float step = length / requested;
    for (int i = 0; i < count; ++i) {
        Point3F j{0, 0, 0};
        if (i != 0 && i != requested - 1) {
            j = {random() * 2.0f - 1.0f, random() * 2.0f - 1.0f, random() * 2.0f - 1.0f};
            const float l = std::sqrt(j.x * j.x + j.y * j.y + j.z * j.z);
            if (l * l > 1e-4f) j = {j.x / l, j.y / l, j.z / l};
            j = {j.x * amp, j.y * amp, j.z * amp};
        }
        points.push_back({i * step + j.x, j.y, j.z});
    }
    return points;
}

// Moller-Trumbore: segment parameter in [0, 1] where a -> b crosses the
// triangle, or a negative value.
inline float segmentTriangle(const Point3F& a, const Point3F& b, const Point3F& p0,
                             const Point3F& p1, const Point3F& p2) {
    const Point3F d{b.x - a.x, b.y - a.y, b.z - a.z};
    const Point3F e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const Point3F e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    const Point3F h{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
    const float det = e1.x * h.x + e1.y * h.y + e1.z * h.z;
    if (std::fabs(det) < 1e-9f) return -1.0f;
    const float inv = 1.0f / det;
    const Point3F s{a.x - p0.x, a.y - p0.y, a.z - p0.z};
    const float u = (s.x * h.x + s.y * h.y + s.z * h.z) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const Point3F q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
    const float v = (d.x * q.x + d.y * q.y + d.z * q.z) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
    return t >= 0.0f && t <= 1.0f ? t : -1.0f;
}

// Build the camera-facing ribbon used by tracer and beam-style projectiles.
inline std::vector<Point3F> projectileBeamQuad(const Point3F& start, const Point3F& end,
                                               const Point3F& camera, float width) {
    Point3F direction{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length <= 0.001f || !std::isfinite(width)) return {start, end};
    direction.x /= length; direction.y /= length; direction.z /= length;

    Point3F toCamera{camera.x - start.x, camera.y - start.y, camera.z - start.z};
    Point3F side{toCamera.y * direction.z - toCamera.z * direction.y,
                 toCamera.z * direction.x - toCamera.x * direction.z,
                 toCamera.x * direction.y - toCamera.y * direction.x};
    float sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    if (sideLength <= 0.001f) {
        const Point3F fallback = std::fabs(direction.y) < 0.9f
            ? Point3F{0, 1, 0} : Point3F{1, 0, 0};
        side = {fallback.y * direction.z - fallback.z * direction.y,
                fallback.z * direction.x - fallback.x * direction.z,
                fallback.x * direction.y - fallback.y * direction.x};
        sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    }
    if (sideLength <= 0.001f) return {start, end};
    const float halfWidth = std::fabs(width) * 0.5f / sideLength;
    side.x *= halfWidth; side.y *= halfWidth; side.z *= halfWidth;
    return {{start.x - side.x, start.y - side.y, start.z - side.z},
            {start.x + side.x, start.y + side.y, start.z + side.z},
            {end.x + side.x, end.y + side.y, end.z + side.z},
             {end.x - side.x, end.y - side.y, end.z - side.z}};
}

// ShockLanceProjectile renders two short, jittered lightning ribbons with
// pinned endpoints. Keep the jitter deterministic so replay and live rendering
// do not change the bolt geometry merely because frame timing differs.
inline std::vector<Point3F> shockLancePoints(const Point3F& start, const Point3F& end,
                                             float phase, float amplitude = 0.1f,
                                             int strand = 0) {
    Point3F direction{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length <= 0.001f) return {start, end};
    direction.x /= length; direction.y /= length; direction.z /= length;

    const Point3F up = std::fabs(direction.y) < 0.9f ? Point3F{0, 1, 0} : Point3F{1, 0, 0};
    Point3F side{direction.y * up.z - direction.z * up.y,
                 direction.z * up.x - direction.x * up.z,
                 direction.x * up.y - direction.y * up.x};
    const float sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    if (sideLength <= 0.001f) return {start, end};
    side.x /= sideLength; side.y /= sideLength; side.z /= sideLength;
    Point3F other{direction.y * side.z - direction.z * side.y,
                  direction.z * side.x - direction.x * side.z,
                  direction.x * side.y - direction.y * side.x};

    const int requested = std::max(2, (int)std::lround(length * 20.0f));
    const int count = std::min(50, requested);
    std::vector<Point3F> points;
    points.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float t = (float)i / (float)(count - 1);
        Point3F point{start.x + direction.x * length * t,
                      start.y + direction.y * length * t,
                      start.z + direction.z * length * t};
        if (i != 0 && i != count - 1) {
            const float seed = (float)(i * 17 + strand * 113) + phase * 10.0f;
            const float a = std::sin(seed * 12.9898f) * amplitude;
            const float b = std::cos(seed * 78.233f) * amplitude;
            point.x += side.x * a + other.x * b;
            point.y += side.y * a + other.y * b;
            point.z += side.z * a + other.z * b;
        }
        points.push_back(point);
    }
    return points;
}
