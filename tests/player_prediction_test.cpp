#include "game/player_prediction.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace PlayerPrediction;

static bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }

// A 200 x 200 floor at z = 0 as two upward triangles.
static void floorGather(const Point3F&, const Point3F&, std::vector<Triangle>& out) {
    out.push_back({{-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {0, 0, 1}});
    out.push_back({{-100, -100, 0}, {100, 100, 0}, {-100, 100, 0}, {0, 0, 1}});
}

static Data lightArmor() {
    Data d;
    d.mass = 90; d.maxEnergy = 60;
    d.runForce = 55 * 90; d.maxForwardSpeed = 15; d.maxBackwardSpeed = 13; d.maxSideSpeed = 13;
    d.runSurfaceAngle = 70; d.jumpSurfaceAngle = 80; d.maxStepHeight = 1.5f;
    d.boxSize = {1.2f, 1.2f, 2.3f};
    return d;
}

int main() {
    Collision collision;
    const Data d = lightArmor();
    State s;
    Update u;
    u.hasPosition = true;
    u.position = {0, 0, 5};
    unpackUpdate(s, d, u, true, false);
    assert(s.initialized && s.position.z == 5.0f);

    // Falls under gravity and comes to rest on the floor.
    Move idle;
    for (int i = 0; i < 64; ++i) processTick(s, d, -20.0f, &idle, 0, collision, floorGather, nullptr);
    assert(s.position.z >= -0.001f && s.position.z < 0.05f);
    assert(std::fabs(s.velocity.z) < 0.1f);

    // Running forward at full input reaches maxForwardSpeed along +y (yaw 0).
    s.energy = 60;
    Move forward;
    forward.y = 1;
    for (int i = 0; i < 96; ++i) processTick(s, d, -20.0f, &forward, 0, collision, floorGather, nullptr);
    assert(near(s.velocity.y, 15.0f, 0.3f));
    assert(std::fabs(s.velocity.x) < 0.01f);
    assert(s.position.z >= -0.001f && s.position.z < 0.05f);

    // A small correction warps over at most three ticks, then stops.
    const Point3F before = s.position;
    Update warp;
    warp.hasPosition = true;
    warp.position = {before.x + 0.5f, before.y, before.z};
    warp.velocity = {0, 0, 0};
    warp.rotZ = s.yaw;
    unpackUpdate(s, d, warp, false, false);
    assert(s.warpTicks >= 1 && s.warpTicks <= 3);
    const int ticks = s.warpTicks;
    for (int i = 0; i < ticks; ++i) processTick(s, d, -20.0f, nullptr, 0, collision, floorGather, nullptr);
    assert(near(s.position.x, before.x + 0.5f, 1e-4f));

    // Without moves a ghost predicts for at most 30 ticks.
    s.predictionCount = 2;
    const Point3F still = s.position;
    s.velocity = {0, 5, 0};
    for (int i = 0; i < 5; ++i) processTick(s, d, -20.0f, nullptr, 0, collision, floorGather, nullptr);
    assert(s.position.y > still.y && s.position.y < still.y + 5 * 5 * TickSec + 0.01f);
    return 0;
}
