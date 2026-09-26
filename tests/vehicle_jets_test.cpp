#include "game/vehicle_jets.h"
#include <cassert>
#include <cmath>

using namespace VehicleJets;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    // Activate ramps over its duration, then Maintain takes over.
    Direction d;
    step(d, true, 0.25f, 0.5f, true, 1.0f);
    assert(near(d.activatePosition, 0.5f) && !d.maintaining);
    step(d, true, 0.25f, 0.5f, true, 1.25f);
    assert(d.maintaining && near(d.activatePosition, 0.0f) && near(d.maintainStart, 1.25f));
    // Staying on keeps Maintain running.
    step(d, true, 0.25f, 0.5f, true, 1.5f);
    assert(d.maintaining && near(d.maintainStart, 1.25f));
    // Turning off plays Activate back out from its end.
    step(d, false, 0.25f, 0.5f, true, 1.75f);
    assert(!d.maintaining && near(d.activatePosition, 0.5f));
    step(d, false, 1.0f, 0.5f, true, 2.75f);
    assert(near(d.activatePosition, 0.0f));

    // Without a Maintain sequence Activate holds at its end.
    Direction held;
    step(held, true, 1.0f, 0.5f, false, 0.0f);
    assert(near(held.activatePosition, 1.0f) && !held.maintaining);

    assert(backActive(ThrustForward) && !backActive(ThrustDown));
    assert(bottomActive(ThrustDown, true) && !bottomActive(ThrustDown, false));
    assert(!bottomActive(ThrustForward, true));

    // Contrails start above minTrailSpeed and ramp over force / mass.
    assert(contrailScale(10.0f, 20.0f, 5.0f) == 0.0f);
    assert(contrailScale(20.0f, 20.0f, 5.0f) == 0.0f);
    assert(near(contrailScale(22.5f, 20.0f, 5.0f), 0.5f));
    assert(near(contrailScale(40.0f, 20.0f, 5.0f), 1.0f));
    assert(near(contrailScale(21.0f, 20.0f, 0.0f), 1.0f));
    return 0;
}
