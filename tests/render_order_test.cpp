#include "render/render_order.h"
#include "game/link_beam.h"

#include <cassert>
#include <cmath>

static bool close(float a, float b) { return std::fabs(a - b) < 0.0001f; }

int main() {
    constexpr auto transparent = transparentPassState();
    assert(transparent.depthTest && !transparent.depthWrite);
    assert(transparent.blending && !transparent.additive);
    assert(renderStageBefore(RenderStage::Opaque, RenderStage::Water));
    assert(renderStageBefore(RenderStage::Water, RenderStage::TransparentWorld));
    assert(renderStageBefore(RenderStage::TransparentWorld, RenderStage::Particles));
    assert(renderStageBefore(RenderStage::Particles, RenderStage::Precipitation));
    assert(!renderStageBefore(RenderStage::Precipitation, RenderStage::Water));

    const auto quad = projectileBeamQuad({0, 0, 0}, {0, 0, 4}, {2, 1, 2}, 2.0f);
    assert(quad.size() == 4);
    assert(close(quad[0].z, 0.0f) && close(quad[3].z, 4.0f));
    const float width = std::sqrt((quad[1].x - quad[0].x) * (quad[1].x - quad[0].x) +
                                  (quad[1].y - quad[0].y) * (quad[1].y - quad[0].y));
    assert(close(width, 2.0f));
    return 0;
}
