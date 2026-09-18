#include "game/decal_runtime.h"

#include <cassert>
#include <cmath>

static bool close(float a, float b) { return std::fabs(a - b) < 0.0001f; }

int main() {
    const DecalBasis floor = makeDecalBasis({10, 2, -3}, {0, 7, 0});
    assert(close(floor.position.x, 10.0f) && close(floor.position.y, 2.008f));
    assert(close(floor.normal.y, 1.0f));
    assert(close(floor.tangent.x * floor.normal.x + floor.tangent.y * floor.normal.y + floor.tangent.z * floor.normal.z, 0.0f));
    assert(close(floor.bitangent.x * floor.normal.x + floor.bitangent.y * floor.normal.y + floor.bitangent.z * floor.normal.z, 0.0f));

    const DecalBasis wall = makeDecalBasis({0, 0, 0}, {0, 0, 3});
    assert(close(wall.normal.z, 1.0f));
    assert(!decalIsDuplicate(floor, wall, 4));
    assert(decalIsDuplicate(floor, makeDecalBasis({10, 2, -3}, {0, 1, 0}), 4));
    assert(!decalIsDuplicate(floor, floor, 0));

    assert(decalTextureFrame({0.1f, 0.2f}, 2, 0.15f, 1.0f, false, 0) == 1);
    assert(decalTextureFrame({}, 4, 0.0f, 1.0f, true, 5) == 1);
    assert(close(decalAlpha(0.75f, 1.0f, 250), 1.0f));
    assert(close(decalAlpha(0.875f, 1.0f, 250), 0.5f));
    assert(close(decalAlpha(1.0f, 1.0f, 250), 0.0f));
    return 0;
}
