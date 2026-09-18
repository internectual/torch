#pragma once

#include "core/math.h"
#include <vector>

inline std::vector<Point3F> linkBeamPoints(const Point3F& start, const Point3F& end,
                                           bool curved) {
    if (!curved) return {start, end};
    Point3F control = start;
    Point3F aim{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
    if (length > 0.001f) {
        aim.x /= length; aim.y /= length; aim.z /= length;
        const float reach = std::min(length * 0.55f, 12.0f);
        control = {start.x + aim.x * reach, start.y + aim.y * reach,
                   start.z + aim.z * reach};
        control.y += std::min(length * 0.18f, 3.0f);
    }
    std::vector<Point3F> points;
    points.reserve(9);
    for (int i = 0; i <= 8; ++i) {
        const float t = i / 8.0f;
        const float a = (1.0f - t) * (1.0f - t);
        const float b = 2.0f * (1.0f - t) * t;
        const float c = t * t;
        points.push_back({a * start.x + b * control.x + c * end.x,
                          a * start.y + b * control.y + c * end.y,
                          a * start.z + b * control.z + c * end.z});
    }
    return points;
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
