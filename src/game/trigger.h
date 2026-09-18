#pragma once

#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

struct TriggerPolyhedron {
    std::vector<Point3F> vertices;
    std::vector<Point4F> planes;

    bool contains(const Point3F& point, float epsilon = 0.001f) const {
        if (planes.empty()) return false;
        for (const auto& plane : planes)
            if (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w > epsilon)
                return false;
        return true;
    }
};

inline std::vector<float> triggerNumbers(const std::string& value) {
    std::vector<float> result;
    const char* cursor = value.c_str();
    while (*cursor) {
        char* end = nullptr;
        const float number = std::strtof(cursor, &end);
        if (end != cursor) { result.push_back(number); cursor = end; }
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
