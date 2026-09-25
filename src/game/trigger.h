#pragma once

#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

struct TriggerPolyhedron {
    std::vector<Point3F> vertices;
    std::vector<Point4F> planes;

    bool contains(const Point3F& point, float epsilon = 0.001f) const {
        if (planes.empty()) return false;
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z) || !std::isfinite(epsilon)) return false;
        for (const auto& plane : planes) {
            // NaN comparisons are false, which would otherwise make a
            // malformed mission plane contain every actor.
            if (!std::isfinite(plane.x) || !std::isfinite(plane.y) ||
                !std::isfinite(plane.z) || !std::isfinite(plane.w)) return false;
            if (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w > epsilon)
                return false;
        }
        return true;
    }
};

inline TriggerPolyhedron triggerFromVertices(const std::vector<Point3F>& vertices);

// Apply authored scale before converting a mission point from Torque's Z-up
// frame.  The conversion itself swaps the rendered Y/Z axes; scale does not.
inline Point3F triggerLocalPointToYUp(const Point3F& authoredPoint,
                                      const Point3F& authoredScale) {
    return Math::torquePointToYUp({authoredPoint.x * authoredScale.x,
                                   authoredPoint.y * authoredScale.y,
                                   authoredPoint.z * authoredScale.z});
}

// Replicated ShapeBase positions remain in the native Torque Z-up frame until
// presentation. Trigger hulls are already in Torch's Y-up frame, so convert
// remote actor positions before testing occupancy as well.
inline Point3F triggerNetworkPointToYUp(const Point3F& networkPoint) {
    return Math::torquePointToYUp(networkPoint);
}

// PhysicalZone volumes use the same Z-up transform and axis-angle rotation as
// mission triggers.  Keep the test in world space so rotated zones affect
// movement instead of being treated as axis-aligned boxes.
inline bool orientedBoxContains(const Point3F& worldPoint, const Point3F& center,
                                const Point3F& axis, float angleDeg,
                                const Point3F& halfT2, float epsilon = 0.001f) {
    const float axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    Point3F local{worldPoint.x - center.x, worldPoint.y - center.y, worldPoint.z - center.z};
    if (axisLength > 0.0001f) {
        const Point3F normalized{axis.x / axisLength, axis.y / axisLength, axis.z / axisLength};
        const MatrixF rotation = Math::torqueRotationToYUp(
            normalized, -Math::DEG2RAD(angleDeg));
        local = rotation.inverse().transform(local);
    }
    return std::fabs(local.x) <= std::fabs(halfT2.x) + epsilon &&
           std::fabs(local.y) <= std::fabs(halfT2.z) + epsilon &&
           std::fabs(local.z) <= std::fabs(halfT2.y) + epsilon;
}

// Transform an authored volume into Torch's Y-up world frame. PhysicalZone
// volumes are polyhedra, not necessarily boxes described by object scale.
inline bool transformedTriggerContains(const TriggerPolyhedron& localVolume,
                                       const Point3F& worldPoint,
                                       const Point3F& center,
                                       const Point3F& scale,
                                       const Point3F& axis,
                                       float angleDeg) {
    const float axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    Point3F normalized = axis;
    if (axisLength > 0.0001f) {
        normalized.x /= axisLength;
        normalized.y /= axisLength;
        normalized.z /= axisLength;
    }
    const MatrixF rotation = axisLength > 0.0001f
        ? Math::torqueRotationToYUp(normalized, -Math::DEG2RAD(angleDeg)) : MatrixF{};
    std::vector<Point3F> transformed;
    transformed.reserve(localVolume.vertices.size());
    for (const auto& local : localVolume.vertices) {
        const Point3F converted = triggerLocalPointToYUp(local, scale);
        const Point3F rotated = rotation.transform(converted);
        transformed.push_back({rotated.x + center.x, rotated.y + center.y,
                               rotated.z + center.z});
    }
    return triggerFromVertices(transformed).contains(worldPoint);
}

// PhysicalZone modifiers use native defaults when malformed mission values are
// supplied.  Keeping that fallback here prevents NaNs from reaching movement
// and projectile integration.
inline float physicalZoneModifier(float value) {
    return std::isfinite(value) ? std::clamp(value, -40.0f, 40.0f) : 1.0f;
}

// PhysicalZone::appliedForce is authored in the zone's local Torque frame.
// Apply the same axis-angle transform as the volume before converting the
// vector to Torch's world frame.
inline Point3F physicalZoneForceToYUp(const Point3F& authoredForce,
                                      const Point3F& axis, float angleDeg) {
    const Point3F safeForce{
        std::isfinite(authoredForce.x) ? authoredForce.x : 0.0f,
        std::isfinite(authoredForce.y) ? authoredForce.y : 0.0f,
        std::isfinite(authoredForce.z) ? authoredForce.z : 0.0f};
    const float axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (axisLength <= 0.0001f)
        return Math::torquePointToYUp(safeForce);
    const Point3F normalized{axis.x / axisLength, axis.y / axisLength, axis.z / axisLength};
    const MatrixF rotation = Math::torqueRotationToYUp(
        normalized, -Math::DEG2RAD(angleDeg));
    return rotation.transform(Math::torquePointToYUp(safeForce));
}

struct TriggerTransitions {
    std::vector<std::string> entered;
    std::vector<std::string> left;
};

// Force the next tick to reevaluate a volume after its transform or state
// changes; stale occupants otherwise suppress the new transition callbacks.
inline void triggerVolumeChanged(std::unordered_set<std::string>& occupants) {
    occupants.clear();
}

// Destroyed ShapeBases no longer participate in mission trigger volumes.
inline bool triggerActorIsActive(bool alive) {
    return alive;
}

inline TriggerTransitions triggerTransitions(std::unordered_set<std::string>& previous,
                                              const std::unordered_set<std::string>& current) {
    TriggerTransitions result;
    for (const auto& actor : current)
        if (!previous.count(actor)) result.entered.push_back(actor);
    for (const auto& actor : previous)
        if (!current.count(actor)) result.left.push_back(actor);
    std::sort(result.entered.begin(), result.entered.end());
    std::sort(result.left.begin(), result.left.end());
    previous = current;
    return result;
}

inline std::vector<float> triggerNumbers(const std::string& value) {
    std::vector<float> result;
    const char* cursor = value.c_str();
    while (*cursor) {
        char* end = nullptr;
        const float number = std::strtof(cursor, &end);
        if (end != cursor) {
            if (std::isfinite(number)) result.push_back(number);
            cursor = end;
        }
        else ++cursor;
    }
    return result;
}

inline TriggerPolyhedron triggerFromVertices(const std::vector<Point3F>& vertices) {
    TriggerPolyhedron result;
    result.vertices = vertices;
    if (vertices.size() < 4) return result;
    const Point3F centroid = [&] {
        Point3F c{};
        for (const auto& p : vertices) { c.x += p.x; c.y += p.y; c.z += p.z; }
        const float scale = 1.0f / vertices.size();
        return Point3F{c.x * scale, c.y * scale, c.z * scale};
    }();
    constexpr float epsilon = 0.0001f;
    for (size_t i = 0; i < vertices.size(); ++i)
        for (size_t j = i + 1; j < vertices.size(); ++j)
            for (size_t k = j + 1; k < vertices.size(); ++k) {
                const Point3F a{vertices[j].x - vertices[i].x, vertices[j].y - vertices[i].y,
                                vertices[j].z - vertices[i].z};
                const Point3F b{vertices[k].x - vertices[i].x, vertices[k].y - vertices[i].y,
                                vertices[k].z - vertices[i].z};
                Point3F n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                          a.x * b.y - a.y * b.x};
                const float length = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
                if (length < epsilon) continue;
                n.x /= length; n.y /= length; n.z /= length;
                float d = -(n.x * vertices[i].x + n.y * vertices[i].y + n.z * vertices[i].z);
                if (n.x * centroid.x + n.y * centroid.y + n.z * centroid.z + d > 0) {
                    n.x = -n.x; n.y = -n.y; n.z = -n.z; d = -d;
                }
                bool supporting = true;
                for (const auto& p : vertices)
                    if (n.x*p.x + n.y*p.y + n.z*p.z + d > epsilon) { supporting = false; break; }
                if (!supporting) continue;
                bool duplicate = false;
                for (const auto& plane : result.planes)
                    if (std::fabs(plane.x-n.x) < epsilon && std::fabs(plane.y-n.y) < epsilon &&
                        std::fabs(plane.z-n.z) < epsilon && std::fabs(plane.w-d) < epsilon) {
                        duplicate = true; break;
                    }
                if (!duplicate) result.planes.push_back({n.x, n.y, n.z, d});
            }
    return result;
}

inline TriggerPolyhedron triggerBox(const Point3F& half) {
    const Point3F h{std::max(0.001f, std::fabs(half.x)), std::max(0.001f, std::fabs(half.y)),
                    std::max(0.001f, std::fabs(half.z))};
    return triggerFromVertices({{-h.x,-h.y,-h.z},{h.x,-h.y,-h.z},{h.x,h.y,-h.z},{-h.x,h.y,-h.z},
                                {-h.x,-h.y,h.z},{h.x,-h.y,h.z},{h.x,h.y,h.z},{-h.x,h.y,h.z}});
}

// Mission fallback volumes are scaled by their WorldObject transform after
// parsing, so their local hull must represent one authored unit.
inline TriggerPolyhedron triggerUnitBox() {
    return triggerBox({0.5f, 0.5f, 0.5f});
}
