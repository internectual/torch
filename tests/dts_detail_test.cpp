#include "render/dts_detail.h"
#include <cassert>
#include <cmath>
#include <vector>

struct Detail { float size; };

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    // Projected radius: a 2 m radius at 100 m, 768 px high at 90 degrees.
    const float pixelScale = 768.0f * 0.5f / std::tan(3.14159265f / 4.0f);
    assert(near(DTSDetail::pixelSize(2.0f, 1.0f, 100.0f, pixelScale, 1.0f), 7.68f));
    // Object scale and $pref::TS::detailAdjust scale it; zero distance is clamped.
    assert(near(DTSDetail::pixelSize(2.0f, 3.0f, 100.0f, pixelScale, 0.5f), 11.52f));
    assert(std::isfinite(DTSDetail::pixelSize(2.0f, 1.0f, 0.0f, pixelScale, 1.0f)));

    // Detail sizes descend; Collision/LOS details have negative sizes.
    const std::vector<Detail> details = {{64.0f}, {32.0f}, {8.0f}, {-1.0f}, {-1.0f}};
    float smallestSize = -1.0f;
    int32_t smallest = -2;
    DTSDetail::smallestVisible(details, smallestSize, smallest);
    assert(smallest == 2 && smallestSize == 8.0f);

    // The first detail whose size the projected size reaches.
    assert(DTSDetail::select(details, smallest, smallestSize, 500.0f) == 0);
    assert(DTSDetail::select(details, smallest, smallestSize, 64.0f) == 0);
    assert(DTSDetail::select(details, smallest, smallestSize, 63.9f) == 1);
    assert(DTSDetail::select(details, smallest, smallestSize, 20.0f) == 2);
    assert(DTSDetail::select(details, smallest, smallestSize, 8.5f) == 2);
    // At or below the smallest visible size nothing is drawn.
    assert(DTSDetail::select(details, smallest, smallestSize, 8.0f) == -1);
    assert(DTSDetail::select(details, smallest, smallestSize, 1.0f) == -1);

    // A shape whose smallest detail has size 0 is drawn at any distance.
    const std::vector<Detail> always = {{100.0f}, {0.0f}};
    DTSDetail::smallestVisible(always, smallestSize, smallest);
    assert(smallest == 1 && DTSDetail::select(always, smallest, smallestSize, 0.01f) == 1);
    // No sizes at all: nothing is selectable.
    const std::vector<Detail> utility = {{-1.0f}};
    DTSDetail::smallestVisible(utility, smallestSize, smallest);
    assert(smallest == -1 && DTSDetail::select(utility, smallest, smallestSize, 1000.0f) == -1);
    return 0;
}
