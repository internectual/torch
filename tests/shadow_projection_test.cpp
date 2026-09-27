#include "render/shadow_projection.h"
#include <cassert>
#include <cmath>
#include <initializer_list>

using namespace ShadowProjection;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
static float dot(const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

int main() {
    // The constant light travels down: Torque (0.57, 0.57, -0.57).
    const Point3F light = constantLightDir();
    assert(near(light.x, 0.57735f, 1e-3f) && near(light.y, -0.57735f, 1e-3f) && near(light.z, -0.57735f, 1e-3f));
    // Within 100 m no tilt; by 500 m the vertical part is -1 before
    // renormalising (the lateral part is kept).
    assert(near(lightDir(50.0f).y, light.y));
    assert(near(lightDir(600.0f).y, -1.0f / std::sqrt(1.0f + 2.0f * light.x * light.x), 1e-3f));
    const float mid = lightDir(300.0f).y;
    assert(mid < light.y && mid > -1.0f);
    // Reach loss: half at 500 m, 30% of reach left past 1 km.
    assert(near(distanceFade(500.0f).reachLoss, 0.5f));
    assert(near(distanceFade(2000.0f).reachLoss, 0.7f));

    // Visibility: below 6 px nothing; the fade grows once 6 px exceeds half
    // the projected size.
    assert(!visibility(5.0f).visible);
    assert(visibility(12.0f).visible && visibility(12.0f).fade == 0.0f);
    assert(near(visibility(8.0f).fade, 0.5f));

    // Tile rows by projected radius.
    assert(tileSpec(200.0f).intervalMs == 25.0f && tileSpec(200.0f).blur);
    assert(tileSpec(50.0f).intervalMs == 100.0f);
    assert(tileSpec(15.0f).size == 32 && !tileSpec(15.0f).blur);
    assert(tileSpec(5.0f).size == 0);

    // Projected radius: a 1 m sphere at 10 m with a 90-degree vertical FOV
    // on a 1000 px viewport spans 50 px.
    assert(near(projectedRadiusPx(1.0f, 10.0f, 1000.0f, 90.0f), 50.0f, 1e-2f));

    // The light basis is orthonormal for the constant and the vertical light.
    for (const Point3F& d : {light, lightDir(800.0f)}) {
        const Basis b = lightBasis(d, {1, 2, 3});
        assert(near(dot(b.x, b.x), 1.0f) && near(dot(b.z, b.z), 1.0f));
        assert(near(dot(b.x, b.dir), 0.0f) && near(dot(b.z, b.dir), 0.0f) && near(dot(b.x, b.z), 0.0f));
        // A point down the light projects onto the centre of the tile.
        const Point3F p = toLight(b, {1 + d.x * 4, 2 + d.y * 4, 3 + d.z * 4});
        assert(near(p.x, 0.0f) && near(p.y, 4.0f) && near(p.z, 0.0f));
    }
    return 0;
}
