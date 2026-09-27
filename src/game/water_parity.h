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

// ─── Engine fluid (terrain/fluidSupport.cc) ─────────────────────────────────
#include <cstdint>
#include <vector>

struct FluidInfo {
    int squareX0 = 0, squareY0 = 0;   // min corner, terrain squares
    int squaresX = 8, squaresY = 8;
    int blocksX = 1, blocksY = 1;
    bool highRes = false;             // blocks of 4 squares, verts every 8
    float step4 = 64.0f;              // block size (m_Step[4])
};

// fluid::SetInfo: snap the min corner to terrain squares within the rep, and
// size the block grid. Fluids covering at most 128 x 128 squares use high
// resolution (4-square blocks), others 8-square blocks of at most 256.
inline FluidInfo fluidSetInfo(float x0, float y0, float sizeX, float sizeY) {
    FluidInfo info;
    info.squareX0 = std::clamp((int)((x0 / 8.0f) + 0.5f), 0, 2040);
    info.squareY0 = std::clamp((int)((y0 / 8.0f) + 0.5f), 0, 2040);
    info.squaresX = (int)((sizeX / 8.0f) + 0.5f);
    info.squaresY = (int)((sizeY / 8.0f) + 0.5f);
    if (info.squaresX <= 128 && info.squaresY <= 128) {
        info.highRes = true;
        info.squaresX = (info.squaresX + 3) & ~0x03;
        info.squaresY = (info.squaresY + 3) & ~0x03;
        if (info.squaresX <= 0) info.squaresX = 4;
        if (info.squaresY <= 0) info.squaresY = 4;
        info.blocksX = info.squaresX >> 2;
        info.blocksY = info.squaresY >> 2;
        info.step4 = 32.0f;
    } else {
        info.squaresX = std::clamp((info.squaresX + 7) & ~0x07, 8, 256);
        info.squaresY = std::clamp((info.squaresY + 7) & ~0x07, 8, 256);
        info.blocksX = info.squaresX >> 3;
        info.blocksY = info.squaresY >> 3;
        info.step4 = 64.0f;
    }
    return info;
}

// fluid::RebuildMasks: a block is drawn when any terrain point in it lies
// below the fluid level (surface + half the wave amplitude, in the height
// field's 1/32 units). With removeWetEdges, wet points connected to the
// fluid's border are dried first (FloodFill). `heights` is the 256 x 256
// height field in world units, indexed [y * 256 + x]; null accepts all.
inline std::vector<uint8_t> fluidAcceptMask(const FluidInfo& info, float surfaceZ,
                                            float waveAmplitude, bool removeWetEdges,
                                            const float* heights) {
    const int gw = info.squaresX + 1, gh = info.squaresY + 1;
    std::vector<uint8_t> grid((size_t)gw * gh, 1);
    if (heights) {
        const uint16_t level = (uint16_t)((surfaceZ + waveAmplitude / 2.0f) * 32.0f);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int i = (((info.squareY0 + y) & 255) << 8) + ((info.squareX0 + x) & 255);
                const uint16_t terrain = (uint16_t)std::lround(heights[i] * 32.0f);
                grid[(size_t)y * gw + x] = level > terrain;
            }
        if (removeWetEdges) {
            std::vector<int> stack;
            auto seed = [&](int x, int y) {
                if (grid[(size_t)y * gw + x]) { grid[(size_t)y * gw + x] = 0; stack.push_back(y * gw + x); }
            };
            for (int x = 0; x < gw; ++x) { seed(x, 0); seed(x, gh - 1); }
            for (int y = 0; y < gh; ++y) { seed(0, y); seed(gw - 1, y); }
            while (!stack.empty()) {
                const int at = stack.back();
                stack.pop_back();
                const int x = at % gw, y = at / gw;
                if (x > 0) seed(x - 1, y);
                if (x + 1 < gw) seed(x + 1, y);
                if (y > 0) seed(x, y - 1);
                if (y + 1 < gh) seed(x, y + 1);
            }
        }
    }
    const int per = info.highRes ? 4 : 8;
    std::vector<uint8_t> accept((size_t)info.blocksX * info.blocksY, 0);
    for (int by = 0; by < info.blocksY; ++by)
        for (int bx = 0; bx < info.blocksX; ++bx) {
            bool any = false;
            for (int y = 0; y <= per && !any; ++y)
                for (int x = 0; x <= per && !any; ++x)
                    any = grid[(size_t)(by * per + y) * gw + (bx * per + x)] != 0;
            accept[(size_t)by * info.blocksX + bx] = any;
        }
    return accept;
}

// The fluid's terrain rep holding the eye (fluid::RunQuadTree).
inline int fluidRepIndex(float fluidCoord) {
    int i = (int)(fluidCoord / 2048.0f);
    if (fluidCoord < 0.0f) i--;
    return i;
}
