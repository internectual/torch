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

inline float projectileMaterialAlpha(const V12::DecodedDataBlock& data) {
    return data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Cross &&
                   data.projectileTracerAlpha
        ? 0.85f : 1.0f;
}

inline bool projectileMaterialAdditive(const V12::DecodedDataBlock& data) {
    return data.projectileMaterial != V12::DecodedDataBlock::ProjectileMaterial::None;
}

struct ProjectileVisualLayer {
    float width = 1.0f;
    float height = 1.0f;
    float alpha = 1.0f;
    size_t textureIndex = 0;
    float angle = 0.0f;
};

inline float projectileVisualFade(const V12::DecodedDataBlock& data, float age) {
    const float lifetime = std::max(0.001f, data.projectileLifetimeMS / 1000.0f);
    return std::clamp(1.0f - std::max(0.0f, age) / lifetime, 0.0f, 1.0f);
}

inline std::vector<ProjectileVisualLayer> projectileVisualLayers(
    const V12::DecodedDataBlock& data, float age = 0.0f) {
    std::vector<ProjectileVisualLayer> result;
    if (data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::None)
        return result;
    const float fade = projectileVisualFade(data, age);
    const float base = std::max(0.01f, data.projectileMaterialSizes[0]);
    const size_t textureCount = data.projectileMaterialTextures.size();
    if (data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::LinearFlare) {
        const size_t count = std::max<size_t>(1, std::max<size_t>(data.projectileFlareCount, textureCount));
        for (size_t i = 0; i < count; ++i) {
            const float size = data.projectileMaterialSizes[std::min<size_t>(i, 2)];
            result.push_back({std::max(0.01f, size > 0.0f ? size : base),
                              std::max(0.01f, data.projectileTracerWidth > 0.0f
                                  ? data.projectileTracerWidth : base * 0.35f),
                              fade * (1.0f - 0.18f * (float)i), i, 0.0f});
        }
    } else {
        const size_t count = data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Flare
            ? std::max<size_t>(1, std::max<size_t>(data.projectileFlareCount, textureCount))
            : std::max<size_t>(1, textureCount);
        for (size_t i = 0; i < count; ++i) {
            const float size = i == 0 ? base : std::max(0.01f, data.projectileMaterialSizes[std::min<size_t>(i, 2)]);
            result.push_back({size, size, fade, i,
                              data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Cross && i > 0
                                  ? data.projectileCrossViewAngle : 0.0f});
        }
    }
    return result;
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
