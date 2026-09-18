#include "render/environment_commands.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    float distance = 666.0f;
    float density = 1.0f / distance;
    assert(applyFogDistance(distance, density, 300.0f));
    assert(distance == 300.0f && std::abs(density - 1.0f / 300.0f) < 0.000001f);
    assert(!applyFogDistance(distance, density, 0.0f));
    assert(!applyFogDistance(distance, density, std::numeric_limits<float>::infinity()));

    assert(applyFogDensity(distance, density, 0.0f));
    assert(distance == 0.0f && density == 0.0f);
    assert(!applyFogDensity(distance, density, -1.0f));

    ColorF color{0, 0, 0, 0};
    assert(applyFogColor(color, 0.1f, 0.2f, 0.3f, 0.4f));
    assert(color.r == 0.1f && color.g == 0.2f && color.b == 0.3f && color.a == 0.4f);
    assert(!applyFogColor(color, 0.1f, std::numeric_limits<float>::quiet_NaN(), 0.3f, 1.0f));
    assert(validEnvironmentColor({0.0f, 0.5f, 1.0f, 1.0f}));
    assert(!validEnvironmentColor({-0.1f, 0.5f, 1.0f, 1.0f}));
    assert(validSunDirection({0.0f, 1.0f, 0.0f}));
    assert(!validSunDirection({0.0f, 0.0f, 0.0f}));

    float level = 4.0f;
    assert(applyWaterLevel(level, -12.5f) && level == -12.5f);
    assert(!applyWaterLevel(level, std::numeric_limits<float>::quiet_NaN()));
    float opacity = 0.5f;
    assert(applyWaterOpacity(opacity, 0.0f) && opacity == 0.0f);
    assert(!applyWaterOpacity(opacity, 1.01f));
    ColorF waterColor{0, 0, 0, 1};
    assert(applyWaterColor(waterColor, 0.2f, 0.4f, 0.8f));
    assert(waterColor.r == 0.2f && waterColor.g == 0.4f && waterColor.b == 0.8f);
    assert(!applyWaterColor(waterColor, -0.1f, 0.4f, 0.8f));
    int type = -1;
    assert(parseWaterType("CrustyLava", type) && type == 6);
    assert(parseWaterType("7", type) && type == 7);
    assert(!parseWaterType("steam", type));
    assert(validPrecipitationSettings(3, 0.25f));
    assert(!validPrecipitationSettings(8, 0.25f));
    assert(!validPrecipitationSettings(1, 1.1f));
    assert(precipitationDropCount(1000, 0.25f) == 250);
    assert(precipitationDropCount(1, 0.01f) == 1);
    assert(precipitationDropCount(1000, 0.0f) == 0);

    return 0;
}
