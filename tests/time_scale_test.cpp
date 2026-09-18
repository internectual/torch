#include "game/time_scale.h"

#include <cassert>

int main() {
    assert(GameTime::clampScale(-1.0f) == 0.0f);
    assert(GameTime::clampScale(1.0f) == 1.0f);
    assert(GameTime::clampScale(101.0f) == 100.0f);
    assert(GameTime::scaledDelta(0.25f, 0.5f) == 0.125f);
    assert(GameTime::scaledDelta(-1.0f, 2.0f) == 0.0f);
    assert(GameTime::scaledDelta(0.25f, -1.0f) == 0.0f);
    return 0;
}
