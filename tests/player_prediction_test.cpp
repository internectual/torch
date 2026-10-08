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

    // Demo GameState control-player transforms are exact compression anchors,
    // but playback must ease them rather than snapping to each packet value.
    State anchorState;
    anchorState.initialized = true;
    anchorState.position = {0, 0, 10};
    anchorState.velocity = {1, 0, 0};
    Update exactAnchor;
    exactAnchor.hasPosition = true;
    exactAnchor.allowWarp = false;
    exactAnchor.position = {3, 0, 10};
    exactAnchor.velocity = {1, 0, 0};
    const Update playbackAnchor = interpolatedAnchor(exactAnchor);
    unpackUpdate(anchorState, d, playbackAnchor, false, false);
    assert(anchorState.warpTicks == ControlAnchorWarpTicks &&
           anchorState.position.x == 0.0f && anchorState.simulateDuringWarp);
    Collision openCollision;
    processTick(anchorState, d, -20.0f, nullptr, 0, openCollision, nullptr, nullptr);
    assert(anchorState.position.x > 0.0f && anchorState.position.x < 3.0f);
    assert(anchorState.velocity.z < 0.0f); // prediction continues during warp
    assert(anchorState.warpTicks == ControlAnchorWarpTicks - 1);

    // Without moves a ghost predicts for at most 30 ticks.
    s.predictionCount = 2;
    const Point3F still = s.position;
    s.velocity = {0, 5, 0};
    for (int i = 0; i < 5; ++i) processTick(s, d, -20.0f, nullptr, 0, collision, floorGather, nullptr);
    assert(s.position.y > still.y && s.position.y < still.y + 5 * 5 * TickSec + 0.01f);
    {
        Data jetData = lightArmor();
        jetData.maxEnergy = 60.0f;
        jetData.minJetEnergy = 2.0f;
        jetData.jetEnergyDrain = 10.0f;
        jetData.jetForce = 9000.0f;
        State jet;
        jet.initialized = true;
        jet.position = {0, 0, 100};
        Update emptyTank;
        emptyTank.hasEnergy = true;
        emptyTank.energy = 0.0f;
        unpackUpdate(jet, jetData, emptyTank, false, false);
        assert(jet.energy == 0.0f);
        Move heldJet;
        heldJet.trigger[3] = true;
        processTick(jet, jetData, -20.0f, &heldJet, 0, collision, nullptr, nullptr);
        assert(!jet.jetting);

        Update fueled;
        fueled.hasEnergy = true;
        fueled.energy = 0.5f;
        fueled.energyNormalized = true;
        unpackUpdate(jet, jetData, fueled, false, false);
        assert(near(jet.energy, 30.0f, 1e-6f));
        processTick(jet, jetData, -20.0f, &heldJet, 0, collision, nullptr, nullptr);
        assert(jet.jetting && jet.energy < 30.0f);
    }
    {
        // Player::updatePos: a physical zone's face the swept box meets
        // scales the velocity (velocityMod); the free move covers the travel.
        Collision zoned;
        Zone zone;
        zone.velocityMod = 0.1f;
        // The zone's -x face at x = 0 (outward normal -x).
        zone.triangles.push_back({{0, -5, -5}, {0, 5, 5}, {0, 5, -5}, {-1, 0, 0}});
        zone.triangles.push_back({{0, -5, -5}, {0, -5, 5}, {0, 5, 5}, {-1, 0, 0}});
        zoned.gatherZones = [&](const Point3F&, const Point3F&, std::vector<Zone>& out) { out.push_back(zone); };
        State z;
        z.initialized = true;
        z.position = {-0.7f, 0, 0};
        z.velocity = {10, 0, 0};
        zoned.prepare(nullptr, z.position, d.boxSize, mul(z.velocity, TickSec), d.maxStepHeight);
        updatePos(z, d, zoned, z.position);
        assert(near(z.velocity.x, 1.0f, 1e-4f));
        assert(near(z.position.x, -0.7f + 10 * TickSec, 1e-4f));
        // Moving away from the face (a back face) leaves the velocity alone.
        z.position = {-0.7f, 0, 0};
        z.velocity = {-10, 0, 0};
        zoned.prepare(nullptr, z.position, d.boxSize, mul(z.velocity, TickSec), d.maxStepHeight);
        updatePos(z, d, zoned, z.position);
        assert(near(z.velocity.x, -10.0f, 1e-4f));
    }
    return 0;
}
