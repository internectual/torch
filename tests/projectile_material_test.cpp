#include "render/material_parity.h"
#include "game/link_beam.h"
#include <cassert>
#include <cmath>

int main() {
    V12::DecodedDataBlock tracer;
    tracer.projectileMaterial = V12::DecodedDataBlock::ProjectileMaterial::Cross;
    tracer.projectileTracerAlpha = true;
    const std::vector<uint32_t> textures{UINT32_MAX, 17};
    assert(projectileMaterialTexture(textures, 0) == 0);
    assert(projectileMaterialTexture(textures, 1) == 17);
    assert(projectileMaterialTexture(textures, 2, 23) == 23);
    assert(projectileMaterialAlpha(tracer) == 0.85f);
    assert(projectileMaterialAdditive(tracer));
    tracer.projectileMaterialTextures.push_back("cross_a");
    tracer.projectileMaterialTextures.push_back("cross_b");
    tracer.projectileCrossViewAngle = 0.5f;
    const auto crossLayers = projectileVisualLayers(tracer);
    assert(crossLayers.size() == 2 && crossLayers[1].angle == 0.5f);

    V12::DecodedDataBlock plain;
    assert(projectileMaterialAlpha(plain) == 1.0f);
    assert(!projectileMaterialAdditive(plain));
    assert(projectileMaterialTexture({}, 0, 9) == 9);

    V12::DecodedDataBlock flare;
    flare.projectileMaterial = V12::DecodedDataBlock::ProjectileMaterial::LinearFlare;
    flare.projectileFlareCount = 3;
    flare.projectileTracerWidth = 2.0f;
    flare.projectileTracerLength = 6.0f;
    flare.projectileLifetimeMS = 1000;
    const auto flareLayers = projectileVisualLayers(flare, 0.25f);
    assert(flareLayers.size() == 3 && flareLayers[0].alpha == 0.75f);
    assert(projectileVisualFade(flare, 2.0f) == 0.0f);

    const auto quad = projectileBeamQuad({0, 0, 0}, {0, 0, 4}, {2, 1, 2}, 2.0f);
    assert(quad.size() == 4);
    assert(std::fabs(quad[0].x - quad[1].x) > 0.8f);
    return 0;
}
