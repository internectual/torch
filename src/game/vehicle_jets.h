#pragma once
// Client jet effects of FlyingVehicle / HoverVehicle ghosts
// (FlyingVehicle::updateJet, HoverVehicle::updateJet): each thrust
// direction has a non-cyclic Activate sequence and a cyclic Maintain
// sequence, nozzle emitters, and flying vehicles run contrails above
// minTrailSpeed.
#include <algorithm>
#include <cmath>

namespace VehicleJets {

// Vehicle::ThrustDirection as networked.
enum Thrust : int { ThrustForward = 0, ThrustBackward = 1, ThrustDown = 2 };

// FlyingVehicleData's node table: two nozzles per thrust direction
// (backward has none in the shipped shapes), then the contrail nodes.
inline constexpr const char* NozzleNodes[3][2] = {
    {"jetnozzle0", "jetnozzle1"},
    {"jetnozzlex", "jetnozzlex"},
    {"jetnozzle2", "jetnozzle3"},
};
inline constexpr const char* ContrailNodes[4] = {"contrail0", "contrail1", "contrail2", "contrail3"};

// One direction's Activate/Maintain pair. While maintaining, Activate is
// parked at 0 and Maintain loops from maintainStart.
struct Direction {
    float activatePosition = 0.0f;
    bool maintaining = false;
    float maintainStart = 0.0f;
};

// A +/-1 time-scale thread clamped to [0, 1] (flare threads).
inline float stepThread(float position, bool active, float dt, float duration) {
    if (!(duration > 0.0f)) return active ? 1.0f : 0.0f;
    return std::clamp(position + (active ? dt : -dt) / duration, 0.0f, 1.0f);
}

// Activate plays forward while the direction is on; at its end Maintain
// takes over. Turning off mid-Maintain stops Maintain and plays Activate
// back out from its end.
inline void step(Direction& d, bool active, float dt, float activateDuration,
                 bool hasMaintain, float now) {
    if (!d.maintaining || !active) {
        if (d.maintaining) {
            d.activatePosition = 1.0f;
            d.maintaining = false;
        }
        d.activatePosition = stepThread(d.activatePosition, active, dt, activateDuration);
    }
    if (d.activatePosition >= 1.0f && hasMaintain && !d.maintaining) {
        d.activatePosition = 0.0f;
        d.maintaining = true;
        d.maintainStart = now;
    }
}

// The back jets burn whenever thrust is forward, even with the jets off;
// the bottom jets need the jets on and thrust down.
inline bool backActive(int thrust) { return thrust == ThrustForward; }
inline bool bottomActive(int thrust, bool jetting) { return jetting && thrust == ThrustDown; }

// Fraction of the frame the contrail emitter receives: none at or below
// minTrailSpeed, ramping to all of it maneuveringForce/mass above it.
inline float contrailScale(float forwardSpeed, float minTrailSpeed, float accel) {
    if (!(forwardSpeed > minTrailSpeed)) return 0.0f;
    if (!(accel > 0.0f)) return 1.0f;
    return std::min(1.0f, (forwardSpeed - minTrailSpeed) / accel);
}

} // namespace VehicleJets
