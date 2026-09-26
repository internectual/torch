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
    assert(projectileMaterialTexture({}, 0, 9) == 9);

    const auto quad = projectileBeamQuad({0, 0, 0}, {0, 0, 4}, {2, 1, 2}, 2.0f);
    assert(quad.size() == 4);
    assert(std::fabs(quad[0].x - quad[1].x) > 0.8f);
    return 0;
}
