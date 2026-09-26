#include "game/shape_lighting.h"
#include <cassert>
#include <cmath>

using namespace ShapeLighting;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    // Interior: 70% ambient plus 30% diffuse of the probed colour.
    const Color probed{0.5f, 0.4f, 0.2f};
    const Color sun{0.7f, 0.7f, 0.7f}, ambient{0.4f, 0.4f, 0.4f};
    Color c = lighting(Interior, probed, sun, ambient, 1.0f, 0.0f);
    assert(near(c.r, 0.35f) && near(c.g, 0.28f) && near(c.b, 0.14f));
    c = lighting(Interior, probed, sun, ambient, 0.0f, 1.0f);
    assert(near(c.r, 0.5f) && near(c.b, 0.2f));

    // Sun: full brightness scales the sun by (1 - average ambient).
    c = lighting(Sun, {}, sun, ambient, 1.0f, 0.0f);
    assert(near(c.r, 0.4f + 0.7f * 0.6f));
    c = lighting(Sun, {}, sun, ambient, -1.0f, 0.0f); // facing away: ambient only
    assert(near(c.r, 0.4f));
    // Terrain: a dark texel drops the sun term; clamped to [0, 1].
    c = lighting(Terrain, {0.3f, 0.3f, 0.3f}, sun, ambient, 1.0f, 0.0f);
    assert(near(c.r, 0.4f));
    c = lighting(Sun, {}, {2.0f, 2.0f, 2.0f}, ambient, 1.0f, 0.0f);
    assert(near(c.r, 1.0f));

    const Color texel = terrainTexelLighting(0.5f, sun, ambient);
    assert(near(texel.r, 0.75f));
    assert(near(terrainTexelLighting(2.0f, sun, ambient).r, 1.0f));

    // Probing happens only after moving past the epsilon.
    State state;
    assert(needsProbe(state, 0, 0, 0));
    recordProbe(state, 0, 0, 0, Interior, {0.2f, 0.2f, 0.2f});
    assert(!needsProbe(state, 0.01f, 0, 0));
    assert(needsProbe(state, 1.0f, 0, 0));

    // First sample snaps; later changes slew at 0.002 per millisecond.
    advance(state, 16.0f);
    assert(near(state.color.r, 0.2f) && state.slewing);
    recordProbe(state, 1, 0, 0, Interior, {1.0f, 1.0f, 1.0f});
    advance(state, 100.0f);
    assert(near(state.color.r, 0.4f));
    advance(state, 1000.0f);
    assert(near(state.color.r, 1.0f));
    // Sun mode resets the slew so the next indoor probe snaps again.
    recordProbe(state, 5, 0, 0, Sun, {});
    advance(state, 16.0f);
    assert(!state.slewing);
    return 0;
}
