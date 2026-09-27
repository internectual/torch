#include "render/dynamic_lighting.h"
#include <cassert>
#include <cmath>
#include <limits>

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    assert(dynamicLightFade(0.0f, 0.0f, 1.0f) == 1.0f);
    assert(dynamicLightFade(0.5f, 0.25f, 1.0f) == 0.75f);
    assert(dynamicLightFade(0.0f, 1.0f, 1.0f) == 0.0f);
    assert(dynamicLightFade(0.0f, 0.0f, 0.0f) == 0.0f);
    assert(dynamicLightFade(std::numeric_limits<float>::infinity(), 0.0f, 1.0f) == 0.0f);

    // Screen-size gate: skipped at <= 10 px, full from 20 px, linear between.
    assert(dynamicLightScreenFade(10.0f) == 0.0f);
    assert(dynamicLightScreenFade(5.0f) == 0.0f);
    assert(near(dynamicLightScreenFade(15.0f), 0.5f));
    assert(dynamicLightScreenFade(20.0f) == 1.0f);
    assert(dynamicLightScreenFade(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    // Projected disc: R = 5, d = 3 -> disc radius 4, alpha 0.4.
    assert(near(dynamicLightDiscAlpha(3.0f, 5.0f), 0.4f));
    assert(near(dynamicLightDiscAlpha(-3.0f, 5.0f), 0.4f));
    assert(near(dynamicLightDiscCoord(3.0f, 2.0f, 5.0f), 0.5f));
    assert(dynamicLightDiscCoord(3.0f, 4.0f, 5.0f) < 0.0f);
    assert(dynamicLightDiscCoord(5.0f, 0.0f, 5.0f) < 0.0f);
    assert(dynamicLightDiscAlpha(6.0f, 5.0f) == 0.0f);
    assert(near(dynamicLightDiscAlpha(0.0f, 5.0f), 1.0f));
    return 0;
}
