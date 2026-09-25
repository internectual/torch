#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

// WaterBlock boxes are authored in Torque's Z-up frame.  Converting the
// lower corner to Y-up mirrors the horizontal authored Y axis, so the box's
// positive extent lies toward negative renderer Z.
inline float waterOriginZ(float torqueY, float authoredSizeY) {
    return -torqueY - authoredSizeY;
}

// Mission transforms can produce a reversed horizontal extent.  WaterBlock
// coverage is still the box between its two edges, not an empty interval.
inline bool waterBodyContainsHorizontal(float x, float z, float originX,
                                        float originZ, float sizeX,
                                        float sizeZ) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(originX) ||
        !std::isfinite(originZ) || !std::isfinite(sizeX) ||
        !std::isfinite(sizeZ))
        return false;
    const float minX = std::min(originX, originX + sizeX);
    const float maxX = std::max(originX, originX + sizeX);
    const float minZ = std::min(originZ, originZ + sizeZ);
    const float maxZ = std::max(originZ, originZ + sizeZ);
    return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
}

// Render the same normalized box used by coverage tests.  WaterBlock transforms
// can author either edge as the origin, but the surface tessellator needs a
// positive step so reversed extents do not wind its grid backwards.
inline void waterBodyRenderBounds(float originX, float originZ, float sizeX,
                                  float sizeZ, float& renderOriginX,
                                  float& renderOriginZ, float& renderSizeX,
                                  float& renderSizeZ) {
    const float endX = originX + sizeX;
    const float endZ = originZ + sizeZ;
    renderOriginX = std::min(originX, endX);
    renderOriginZ = std::min(originZ, endZ);
    renderSizeX = std::fabs(sizeX);
    renderSizeZ = std::fabs(sizeZ);
}

// Filter inactive/non-liquid volumes before choosing the highest surface.
// All authored WaterBlock liquid types (including lava and quicksand) have a
// visible surface; only the camera filter below is restricted to normal water.
inline bool waterBodySubmergesCamera(float cameraY, float level, int liquidType) {
    return liquidType >= 0 && liquidType <= 3 && cameraY < level;
}

inline bool activeWaterBodySubmergesCamera(bool active, float cameraY,
                                             float level, int liquidType) {
    return active && waterBodySubmergesCamera(cameraY, level, liquidType);
}

// Only active liquid WaterBlocks participate in splash and underwater effect
// placement. Non-liquid volumes are valid WaterBlocks but must not become a
// surface merely because they overlap the impact point.
inline bool waterBodyCanAffectSurface(bool active, int liquidType) {
    return active && liquidType >= 0 && liquidType <= 7;
}

inline float waterSurfaceLevel(float current, bool active, int liquidType, float level) {
    return waterBodyCanAffectSurface(active, liquidType) ? std::max(current, level) : current;
}

// WaterBlock opacity is already the alpha of its surface material. Applying
// the two authored values multiplicatively makes the default 0.5 surface
// render at 0.25 instead of the Tribes 2 0.5 opacity.
inline float waterSurfaceOpacity(float colorAlpha) {
    // NaN survives std::clamp because both comparisons are false. Do not send
    // an undefined alpha to the water shader; the stock WaterBlock default is
    // a half-opaque surface.
    return std::isfinite(colorAlpha)
        ? std::clamp(colorAlpha, 0.0f, 1.0f) : 0.5f;
}

// Water quads use the renderer's view distance just like terrain and mission
// geometry. A fixed short cutoff makes large lakes disappear before the rest
// of the scene at the native far plane.
inline bool waterQuadWithinRenderDistance(float horizontalDistance,
                                          float farPlane) {
    return std::isfinite(horizontalDistance) && horizontalDistance >= 0.0f &&
           std::isfinite(farPlane) &&
           horizontalDistance <= std::max(0.0f, farPlane);
}

// Shore state is selected per rendered quad. A body can have shore data while
// a particular quad cannot use it (for example outside the terrain), so the
// shader flag must be recomputed rather than inherited from the previous quad.
inline bool waterShoreTextureActive(bool terrainLoaded, bool hasFrames,
                                    float shoreDepth) {
    return terrainLoaded && hasFrames && shoreDepth > 0.0f;
}

// Torque console object names are resolved without case sensitivity.  Keep
// authored WaterBlock commands consistent with that lookup behavior.
inline bool waterBodyNameMatches(const std::string& authoredName,
                                 const std::string& requestedName) {
    if (authoredName.size() != requestedName.size()) return false;
    for (size_t i = 0; i < authoredName.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(authoredName[i])) !=
            std::tolower(static_cast<unsigned char>(requestedName[i])))
            return false;
    }
    return true;
}

// Console WaterBlock indices enumerate active authored surfaces, not the
// parser's mixed mission-object storage (which can contain inactive entries).
inline bool waterBodyOrdinalMatches(bool active, int ordinal, int requested) {
    return active && ordinal == requested;
}
