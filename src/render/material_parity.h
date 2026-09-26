#pragma once

#include "render/renderer.h"
#include "net/v12_datablocks.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline uint32_t projectileMaterialTexture(const std::vector<uint32_t>& textures,
                                         size_t index, uint32_t fallback = 0) {
    return index < textures.size() && textures[index] != UINT32_MAX ? textures[index] : fallback;
}

inline float materialAlphaTestThreshold(uint32_t flags) {
    if (flags & MatFlag_Translucent) return 0.5f;
    if (flags & MatFlag_Additive) return 0.0001f;
    return 0.0f;
}

inline float materialReflectionFactor(int32_t rawAmount) {
    if (rawAmount <= 0) return 0.0f;
    return std::clamp(rawAmount / 255.0f, 0.0f, 1.0f);
}
