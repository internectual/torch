#include "render/dynamic_lighting.h"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    assert(dynamicLightFade(0.0f, 0.0f, 1.0f) == 1.0f);
    assert(dynamicLightFade(0.5f, 0.25f, 1.0f) == 0.75f);
    assert(dynamicLightFade(0.0f, 1.0f, 1.0f) == 0.0f);
    assert(dynamicLightFade(0.0f, 0.0f, 0.0f) == 0.0f);
    assert(dynamicLightFade(std::numeric_limits<float>::infinity(), 0.0f, 1.0f) == 0.0f);
    assert(dynamicLightAttenuation(0.0f, 10.0f, 2.0f) == 1.0f);
    assert(dynamicLightAttenuation(5.0f, 10.0f, 1.0f) == 0.5f);
    assert(dynamicLightAttenuation(10.0f, 10.0f, 2.0f) == 0.0f);
    assert(dynamicLightAttenuation(1.0f, -1.0f, 2.0f) == 0.0f);
    assert(dynamicLightAttenuation(std::numeric_limits<float>::quiet_NaN(), 10.0f, 2.0f) == 0.0f);
    return 0;
}
