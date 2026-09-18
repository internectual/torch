#pragma once

#include "core/math.h"
#include "render/texture_frames.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

struct DecalBasis {
    Point3F position{};
    Point3F normal{0, 1, 0};
    Point3F tangent{1, 0, 0};
    Point3F bitangent{0, 0, 1};
};

inline DecalBasis makeDecalBasis(const Point3F& position, Point3F normal) {
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length < 0.0001f) normal = {0, 1, 0};
    else { normal.x /= length; normal.y /= length; normal.z /= length; }

    // Match V12's stable cross-product choice while avoiding a degenerate basis.
    const Point3F reference = std::fabs(normal.z) > 0.9f ? Point3F{0, 1, 0} : Point3F{0, 0, 1};
    Point3F tangent{normal.y * reference.z - normal.z * reference.y,
                   normal.z * reference.x - normal.x * reference.z,
                   normal.x * reference.y - normal.y * reference.x};
    const float tangentLength = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
    if (tangentLength < 0.0001f) tangent = {1, 0, 0};
    else { tangent.x /= tangentLength; tangent.y /= tangentLength; tangent.z /= tangentLength; }
    Point3F bitangent{tangent.y * normal.z - tangent.z * normal.y,
                      tangent.z * normal.x - tangent.x * normal.z,
                      tangent.x * normal.y - tangent.y * normal.x};
    return {{position.x + normal.x * 0.008f, position.y + normal.y * 0.008f,
             position.z + normal.z * 0.008f}, normal, tangent, bitangent};
}

inline bool decalIsDuplicate(const DecalBasis& a, const DecalBasis& b, uint32_t sourceRef) {
    if (sourceRef == 0) return false;
    const float dx = a.position.x - b.position.x, dy = a.position.y - b.position.y;
    const float dz = a.position.z - b.position.z;
    const float dot = a.normal.x * b.normal.x + a.normal.y * b.normal.y + a.normal.z * b.normal.z;
    return dx * dx + dy * dy + dz * dz < 0.01f * 0.01f && dot > 0.98f;
}

inline size_t decalTextureFrame(const std::vector<float>& durations, size_t frameCount,
                                float age, float lifetime, bool randomize, uint32_t seed) {
    if (frameCount == 0) return 0;
    if (randomize && age == 0.0f) return seed % frameCount;
    return textureFrameIndex(durations, frameCount, std::max(0.0f, age));
}

inline float decalAlpha(float age, float lifetime, int32_t fadeTimeMS) {
    const float life = std::max(0.001f, lifetime);
    const float fade = std::clamp(fadeTimeMS / 1000.0f, 0.0f, life);
    const float fadeStart = life - fade;
    return age <= fadeStart ? 1.0f : std::clamp((life - age) / std::max(0.001f, fade), 0.0f, 1.0f);
}
