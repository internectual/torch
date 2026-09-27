#include "game/shape_threads.h"
#include <cassert>
#include <cmath>

using namespace ShapeThreads;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    Clock clock;
    // A playing, non-cyclic 2 s sequence advances from 0.
    Update door{5, Play, true, false};
    assert(near(timeAt(clock, door, 10.0f, 2.0f, false), 0.0f));
    assert(near(timeAt(clock, door, 11.0f, 2.0f, false), 1.0f));
    // The same update again keeps going; it clamps at the end.
    assert(near(timeAt(clock, door, 13.0f, 2.0f, false), 2.0f));
    // Reversing direction on the same sequence keeps the position (a door
    // closing from where it is instead of snapping).
    Clock half;
    timeAt(half, door, 0.0f, 2.0f, false);
    assert(near(timeAt(half, door, 1.5f, 2.0f, false), 1.5f));
    Update closing{5, Play, false, false};
    assert(near(timeAt(half, closing, 1.5f, 2.0f, false), 1.5f));
    assert(near(timeAt(half, closing, 2.0f, 2.0f, false), 1.0f));
    // Stop resets to 0; Play at its end snaps to the end in its direction.
    Update stopped{5, Stop, true, false};
    assert(near(timeAt(half, stopped, 3.0f, 2.0f, false), 0.0f));
    Update atEnd{5, Play, true, true};
    assert(near(timeAt(half, atEnd, 4.0f, 2.0f, false), 2.0f));
    assert(near(timeAt(half, atEnd, 9.0f, 2.0f, false), 2.0f));
    // A different sequence restarts at 0; cyclic sequences wrap.
    Update ambient{7, Play, true, false};
    assert(near(timeAt(half, ambient, 10.0f, 1.0f, true), 0.0f));
    assert(near(timeAt(half, ambient, 12.25f, 1.0f, true), 0.25f));
    // Paused threads hold.
    Update paused{7, Pause, true, false};
    const float held = timeAt(half, paused, 12.5f, 1.0f, true);
    assert(near(timeAt(half, paused, 20.0f, 1.0f, true), held));
    return 0;
}
