#pragma once
// How Tribes 2 lights shapes (players, items, stations, vehicles): never with
// a shadow map. SceneObject::getLightingColor probes the world when the
// object moves. Under an interior roof it takes the interior lighting at the
// floor below; otherwise it takes the terrain lightmap texel under the
// object. The colour slews toward the new value so doorways do not pop.
// installLights then feeds gamma-space GL lights (see the shape branch of the
// default shader).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ShapeLighting {

// How far the probe casts up (for a roof) and down (for the floor).
inline constexpr float ProbeReach = 100.0f;
// Under a roof: ambient and diffuse shares of the probed colour.
inline constexpr float InteriorAmbient = 0.7f;
inline constexpr float InteriorDiffuse = 0.3f;
// Colour slew per millisecond (0.01 x 0.2).
inline constexpr float SlewPerMs = 0.01f * 0.2f;
// A shape moving less than this since its last probe keeps its result.
inline constexpr float ProbeMoveEpsilon = 0.05f;

enum Mode : int {
    Sun = 0,      // nothing below: the sun at full brightness
    Interior = 1, // under a roof
    Terrain = 2,  // above terrain
};

struct Color { float r = 1.0f, g = 1.0f, b = 1.0f; };

struct State {
    bool hasProbe = false;
    float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    int mode = Sun;
    Color target;
    Color color;
    bool slewing = false; // the engine's +0x7c flag
};

inline bool needsProbe(const State& state, float x, float y, float z) {
    if (!state.hasProbe) return true;
    const float dx = x - state.lastX, dy = y - state.lastY, dz = z - state.lastZ;
    return dx * dx + dy * dy + dz * dz > ProbeMoveEpsilon * ProbeMoveEpsilon;
}

// Record a probe. An Interior result with no colour (a roof but no floor)
// leaves the previous result standing, as the engine keeps its stored colour.
inline void recordProbe(State& state, float x, float y, float z, int mode,
                        const Color& color) {
    state.hasProbe = true;
    state.lastX = x; state.lastY = y; state.lastZ = z;
    state.mode = mode;
    state.target = color;
}

inline float slewChannel(float current, float target, float step) {
    if (current < target) return std::min(std::min(current + step, target), 1.0f);
    if (current > target) return std::max(std::max(current - step, target), 0.0f);
    return current;
}

// Per frame after probing: slew toward the target at the engine's rate,
// except from "nothing below", where the engine snaps.
inline void advance(State& state, float dtMs) {
    if (state.mode == Sun) {
        state.slewing = false;
        return;
    }
    if (!state.slewing) {
        state.color = state.target;
        state.slewing = true;
        return;
    }
    const float step = SlewPerMs * std::max(0.0f, dtMs);
    state.color.r = slewChannel(state.color.r, state.target.r, step);
    state.color.g = slewChannel(state.color.g, state.target.g, step);
    state.color.b = slewChannel(state.color.b, state.target.b, step);
}

// The terrain lightmap texel is clamp(ambient + NdotL*shadow x sun).
inline Color terrainTexelLighting(float ndotl, const Color& sun, const Color& ambient) {
    const float n = std::clamp(ndotl, 0.0f, 1.0f);
    return {std::min(1.0f, ambient.r + n * sun.r),
            std::min(1.0f, ambient.g + n * sun.g),
            std::min(1.0f, ambient.b + n * sun.b)};
}

// CPU reference of the shader's lighting (without point lights), used by
// tests: returns the clamped gamma-space light for a normal dot product.
inline Color lighting(int mode, const Color& probed, const Color& sun, const Color& ambient,
                      float sunNdotL, float interiorNdotL) {
    Color out;
    if (mode == Interior) {
        const float d = InteriorDiffuse * std::max(interiorNdotL, 0.0f);
        out = {probed.r * InteriorAmbient + probed.r * d,
               probed.g * InteriorAmbient + probed.g * d,
               probed.b * InteriorAmbient + probed.b * d};
    } else {
        const float brightness = mode == Terrain
            ? std::clamp((probed.r + probed.g + probed.b) / 3.0f, 0.0f, 1.0f) : 1.0f;
        const float ambientAverage = (ambient.r + ambient.g + ambient.b) / 3.0f;
        const float scale = std::clamp(brightness - ambientAverage, 0.0f, 1.0f) *
                            std::max(sunNdotL, 0.0f);
        out = {ambient.r + sun.r * scale, ambient.g + sun.g * scale, ambient.b + sun.b * scale};
    }
    return {std::clamp(out.r, 0.0f, 1.0f), std::clamp(out.g, 0.0f, 1.0f),
            std::clamp(out.b, 0.0f, 1.0f)};
}

// Engine direction the indoor light travels, Torque space.
inline constexpr float InteriorDirection[3] = {0.57735f, 0.57735f, -0.57735f};

} // namespace ShapeLighting
