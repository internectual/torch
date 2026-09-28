#include "game/dts_triggers.h"
#include <cassert>

int main() {
    // A run cycle: left foot down at 0.35, right at 0.87.
    std::vector<DTSShape::Animation::Trigger> t = {{0x80000002u, 0.35f}, {0x80000001u, 0.87f}};
    uint32_t s = DTSTriggers::advance(t, 0.1f, 0.3f, true, 0);
    assert(s == 0);
    s = DTSTriggers::advance(t, 0.3f, 0.4f, true, 0);
    assert(s == 2);
    s = DTSTriggers::advance(t, 0.8f, 0.9f, true, 0);
    assert(s == 1);
    // Crossing the loop point (unwrapped phases) fires both.
    s = DTSTriggers::advance(t, 0.8f, 1.4f, true, 0);
    assert(s == 3);
    // An off trigger clears its bit.
    std::vector<DTSShape::Animation::Trigger> off = {{0x00000001u, 0.5f}};
    assert(DTSTriggers::advance(off, 0.4f, 0.6f, false, 1) == 0);
    // No movement, no change.
    assert(DTSTriggers::advance(t, 0.5f, 0.5f, true, 5) == 5);
    return 0;
}
