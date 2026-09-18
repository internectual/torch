#pragma once

#include <cstdint>

// The world pass is intentionally split at water. Transparent geometry must
// remain depth-tested, but must not write depth or inherit additive blending.
enum class RenderStage : uint8_t {
    Sky,
    Opaque,
    Water,
    TransparentWorld,
    Projectiles,
    Decals,
    Particles,
    Lightning,
    Precipitation,
    Gui,
};

struct TransparentPassState {
    bool depthTest = true;
    bool depthWrite = false;
    bool blending = true;
    bool additive = false;
};

constexpr TransparentPassState transparentPassState() { return {}; }

constexpr bool renderStageBefore(RenderStage a, RenderStage b) {
    return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
}
