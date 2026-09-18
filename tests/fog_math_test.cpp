#include "render/fog_math.h"

#include <cassert>
#include <cmath>

int main() {
    const AuthoredFogVolume volume{100.0f, 0.0f, 50.0f, 1.0f};
    assert(std::abs(authoredFogContribution(volume, 25.0f, 75.0f, 100.0f) - 0.5f) < 0.0001f);
    assert(std::abs(authoredFogContribution(volume, 25.0f, 25.0f, 100.0f) - 1.0f) < 0.0001f);
    assert(std::abs(combineFogPrecedence(0.9f, 0.4f) - 1.0f) < 0.0001f);
    assert(std::abs(combineFogPrecedence(0.2f, 0.4f) - 0.6f) < 0.0001f);
    return 0;
}
