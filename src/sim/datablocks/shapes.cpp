// DataBlockPack group: see src/sim/datablock_pack.h.
//
// ShapeBaseData and its retail subclasses, plus ShapeBaseImageData, the
// turret image and the two station effects. Each writer mirrors the engine's
// Parent::packData chain (game/shapeBase.cc, shapeImage.cc, player.cc,
// vehicles/*.cc, item.cc, staticShape.cc, turret.cc, camera.cc,
// missionMarker.cc, stationFX*.cc) in the retail (build 25034) layout that
// Torch's reader (src/net/v12_datablocks.cpp) consumes.
//
// Values the engine computes in preload from the loaded shape (the shape CRC,
// sequence and node indices, hasFlash) cannot be derived from fields; they
// are sent as the engine's "not found" value (CRC 0, sequence -1, node -1,
// hasFlash false).
#include "sim/datablock_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <strings.h>

namespace DataBlockPack {

namespace {

using Point = TorqueBitWriter::Point;

std::string at(const char* base, int index) {
    return std::string(base) + "[" + std::to_string(index) + "]";
}

void writeF32(Context& c, const char* field, float fallback) { c.w.writeF32(c.f32(field, fallback)); }
void writeS32(Context& c, const char* field, int32_t fallback) { c.w.writeInt(c.s32(field, fallback), 32); }
void writeFlagField(Context& c, const char* field, bool fallback) { c.w.writeFlag(c.boolean(field, fallback)); }
void writeRefField(Context& c, const char* field) { c.writeRef(c.ref(field)); }
void writePointField(Context& c, const char* field, std::array<float, 3> fallback) {
    const auto p = c.point(field, fallback);
    c.w.writePoint({p[0], p[1], p[2]});
}

// TypeColorI: "r g b [a]" integers, alpha 255 when omitted.
std::array<int, 4> colorI(Context& c, const char* field, std::array<int, 4> fallback) {
    if (!c.has(field)) return fallback;
    std::array<int, 4> out{0, 0, 0, 255};
    int r = 0, g = 0, b = 0, a = 255;
    const int n = std::sscanf(c.str(field).c_str(), "%d %d %d %d", &r, &g, &b, &a);
    out[0] = r; out[1] = g; out[2] = b;
    if (n == 4) out[3] = a;
    return out;
}

// SimObject::computeCRC: the shape resource's CRC, which the server takes
// from the loaded .dts in preload. Not derivable from fields: 0 is sent.
void writeComputeCRC(Context& c) {
    if (c.w.writeFlag(c.boolean("computeCRC", false))) c.w.writeU32(0);
}

// --------------------------------------------------------------------------
// ShapeBaseData::packData. Subclass constructors change some of the
// ShapeBaseData defaults; the flags compare against gShapeBaseDataProto.
struct ShapeDefaults {
    float mass = 1.0f, drag = 0.0f, density = 1.0f, maxEnergy = 0.0f;
};

void shapeBase(Context& c, const ShapeDefaults& d = {}) {
    auto& w = c.w;
    writeComputeCRC(c);
    w.writeString(c.str("shapeFile"));

    auto optionalF32 = [&](float value, float proto) {
        if (w.writeFlag(value != proto)) w.writeF32(value);
    };
    optionalF32(c.f32("mass", d.mass), 1.0f);
    optionalF32(c.f32("drag", d.drag), 0.0f);
    optionalF32(c.f32("density", d.density), 1.0f);
    optionalF32(c.f32("maxEnergy", d.maxEnergy), 0.0f);
    optionalF32(c.f32("cameraMaxDist", 0.0f), 0.0f);
    optionalF32(c.f32("cameraMinDist", 0.2f), 0.2f);
    const float minFov = c.f32("cameraMinFov", 5.0f);
    const float maxFov = c.f32("cameraMaxFov", 120.0f);
    // cameraDefaultFov = mClampF(cameraDefaultFov, cameraMinFov, cameraMaxFov)
    float defaultFov = c.f32("cameraDefaultFov", 90.0f);
    defaultFov = defaultFov < minFov ? minFov : (defaultFov > maxFov ? maxFov : defaultFov);
    optionalF32(defaultFov, 90.0f);
    optionalF32(minFov, 5.0f);
    optionalF32(maxFov, 120.0f);

    w.writeString(c.str("debrisShapeName"));

    const float sensorRadius = c.f32("sensorRadius", 0.0f);
    if (w.writeFlag(sensorRadius != 0.0f)) {
        w.writeInt((int32_t)sensorRadius, 10);
        for (int v : colorI(c, "sensorColor", {255, 0, 0, 200})) w.writeInt(v & 0xff, 8);
    }
    const float heat = c.f32("heatSignature", 1.0f);
    if (w.writeFlag(heat != 1.0f)) w.writeF32(heat);

    w.writeString(c.str("cmdCategory"));
    w.writeString(c.str("cmdMiniIconName"));
    writeFlagField(c, "canControl", false);
    writeFlagField(c, "canObserve", false);
    writeFlagField(c, "observeThroughObject", false);
    writeFlagField(c, "emap", false);
    writeFlagField(c, "isInvincible", false);
    writeFlagField(c, "renderWhenDestroyed", true);
    writeRefField(c, "cmdIcon");
    writeRefField(c, "explosion");
    writeRefField(c, "underwaterExplosion");
    writeRefField(c, "debris");
    writeFlagField(c, "inheritEnergyFromMount", false);
    writeFlagField(c, "firstPersonOnly", false);
    writeFlagField(c, "useEyePoint", false);
    writeS32(c, "shieldEffectLifetimeMS", 300);

    // Tribes 2 shieldEffectScale (not in the V12 source): sent when set to
    // something other than unit scale.
    const auto scale = c.point("shieldEffectScale", {1.0f, 1.0f, 1.0f});
    if (w.writeFlag(scale[0] != 1.0f || scale[1] != 1.0f || scale[2] != 1.0f))
        w.writePoint({scale[0], scale[1], scale[2]});

    for (int i = 0; i < 8; ++i) {
        // hudImageName is an alias of hudImageNameFriendly.
        std::string friendly = c.str(at("hudImageNameFriendly", i).c_str());
        if (friendly.empty()) friendly = c.str(at("hudImageName", i).c_str());
        if (!w.writeFlag(!friendly.empty())) continue;
        w.writeString(friendly);
        const std::string enemy = c.str(at("hudImageNameEnemy", i).c_str());
        if (w.writeFlag(!enemy.empty())) w.writeString(enemy);
        writeFlagField(c, at("hudRenderCenter", i).c_str(), false);
        writeFlagField(c, at("hudRenderModulated", i).c_str(), false);
        writeFlagField(c, at("hudRenderAlways", i).c_str(), false);
        writeFlagField(c, at("hudRenderDistance", i).c_str(), false);
        writeFlagField(c, at("hudRenderName", i).c_str(), false);
    }
}

// StaticShapeData::packData
void staticShape(Context& c) {
    shapeBase(c);
    writeFlagField(c, "noIndividualDamage", false);
    writeS32(c, "dynamicType", 0);
}

// --------------------------------------------------------------------------
// PlayerData::packData (retail layout).
void player(Context& c) {
    auto& w = c.w;
    ShapeDefaults d;
    d.mass = 9.0f;
    d.maxEnergy = 60.0f;
    shapeBase(c, d);

    // The retail binary registers no renderFirstPerson field; the flag is
    // noFrictionOnSki, registered right before minLookAngle.
    writeFlagField(c, "noFrictionOnSki", false);
    writeF32(c, "minLookAngle", -1.4f);
    writeF32(c, "maxLookAngle", 1.4f);
    writeF32(c, "maxFreelookAngle", 3.0f);
    writeF32(c, "maxTimeScale", 1.5f);
    writeF32(c, "maxStepHeight", 1.0f);
    // Jetting (Tribes 2 only; constructor defaults not in the V12 source).
    writeF32(c, "jetForce", 0.0f);
    writeF32(c, "underwaterJetForce", 0.0f);
    writeF32(c, "underwaterVertJetFactor", 0.0f);
    writeF32(c, "jetEnergyDrain", 0.0f);
    writeF32(c, "underwaterJetEnergyDrain", 0.0f);
    writeF32(c, "minJetEnergy", 0.0f);
    writeF32(c, "maxJetForwardSpeed", 0.0f);
    writeF32(c, "maxJetHorizontalPercentage", 0.0f);
    writeRefField(c, "jetEmitter");
    writeRefField(c, "jetEffect");

    writeF32(c, "runForce", 40.0f * 9.0f);
    writeF32(c, "runEnergyDrain", 0.0f);
    writeF32(c, "minRunEnergy", 0.0f);
    writeF32(c, "maxForwardSpeed", 10.0f);
    writeF32(c, "maxBackwardSpeed", 10.0f);
    writeF32(c, "maxSideSpeed", 10.0f);
    writeF32(c, "maxUnderwaterForwardSpeed", 10.0f);
    writeF32(c, "maxUnderwaterBackwardSpeed", 10.0f);
    writeF32(c, "maxUnderwaterSideSpeed", 10.0f);
    writeF32(c, "runSurfaceAngle", 80.0f);

    // preload: recoverDelay and jumpDelay are clamped to 7 bits.
    constexpr int32_t DelayMax = (1 << 7) - 1;
    w.writeInt(std::min(c.s32("recoverDelay", 30), DelayMax), 32); // S32 on the wire
    writeF32(c, "recoverRunForceScale", 1.0f);

    const float jumpEnergyDrain = c.f32("jumpEnergyDrain", 0.0f);
    writeF32(c, "jumpForce", 75.0f);
    w.writeF32(jumpEnergyDrain);
    // preload: minJumpEnergy is raised to jumpEnergyDrain.
    w.writeF32(std::max(c.f32("minJumpEnergy", 0.0f), jumpEnergyDrain));
    const float minJumpSpeed = c.f32("minJumpSpeed", 500.0f);
    w.writeF32(minJumpSpeed);
    writeF32(c, "maxJumpSpeed", 2.0f * 500.0f);
    writeF32(c, "jumpSurfaceAngle", 78.0f);
    w.writeInt(std::min(c.s32("jumpDelay", 30), DelayMax), 7);

    writeF32(c, "horizMaxSpeed", 80.0f);
    writeF32(c, "horizResistSpeed", 38.0f);
    writeF32(c, "horizResistFactor", 1.0f);
    writeF32(c, "upMaxSpeed", 80.0f);
    writeF32(c, "upResistSpeed", 38.0f);
    writeF32(c, "upResistFactor", 1.0f);

    writeF32(c, "splashVelocity", 1.0f);
    writeF32(c, "splashAngle", 45.0f);
    writeF32(c, "splashFreqMod", 300.0f);
    writeF32(c, "splashVelEpsilon", 0.25f);
    writeF32(c, "bubbleEmitTime", 0.4f);
    writeF32(c, "mediumSplashSoundVelocity", 2.0f);
    writeF32(c, "hardSplashSoundVelocity", 3.0f);
    writeF32(c, "exitSplashSoundVelocity", 2.0f);
    writeF32(c, "footstepSplashHeight", 0.1f);
    writeF32(c, "minImpactSpeed", 25.0f);

    // PlayerData::Sounds, in the retail initPersistFields order.
    static const char* const sounds[32] = {
        "jetSound", "wetJetSound",
        "LFootSoftSound", "RFootSoftSound", "LFootHardSound", "RFootHardSound",
        "LFootMetalSound", "RFootMetalSound", "LFootSnowSound", "RFootSnowSound",
        "LFootShallowSound", "RFootShallowSound", "LFootWadingSound", "RFootWadingSound",
        "LFootUnderwaterSound", "RFootUnderwaterSound", "LFootBubblesSound", "RFootBubblesSound",
        "movingBubblesSound", "waterBreathSound",
        "impactSoftSound", "impactHardSound", "impactMetalSound", "impactSnowSound",
        "impactWaterEasy", "impactWaterMedium", "impactWaterHard", "exitingWater",
        "skiSoftSound", "skiHardSound", "skiMetalSound", "skiSnowSound"};
    for (const char* sound : sounds) writeRefField(c, sound);

    writePointField(c, "boundingBox", {1.0f, 1.0f, 2.3f});
    writeRefField(c, "footPuffEmitter");
    writeS32(c, "footPuffNumParts", 15);
    writeF32(c, "footPuffRadius", 0.25f);
    writeRefField(c, "decalData");
    writeF32(c, "decalOffset", 0.0f);
    writeRefField(c, "dustEmitter");
    writeRefField(c, "splash");
    for (int i = 0; i < 3; ++i) writeRefField(c, at("splashEmitter", i).c_str());

    // Tribes 2 heat rates (defaults not in the V12 source; the retail
    // armors' values), then the ground impact shake.
    writeF32(c, "heatDecayPerSec", 1.0f / 4.0f);
    writeF32(c, "heatIncreasePerSec", 1.0f / 3.0f);
    writeF32(c, "groundImpactMinSpeed", 10.0f);
    writePointField(c, "groundImpactShakeFreq", {10.0f, 10.0f, 10.0f});
    writePointField(c, "groundImpactShakeAmp", {20.0f, 20.0f, 20.0f});
    writeF32(c, "groundImpactShakeDuration", 1.0f);
    writeF32(c, "groundImpactShakeFalloff", 10.0f);
}

// --------------------------------------------------------------------------
// VehicleData::packData. `maxSteeringAngle` is the subclass default of the
// retail field, which the engine registers as "maxSteerinAngle".
void vehicle(Context& c, float maxSteeringAngle) {
    ShapeDefaults d;
    d.drag = 0.7f;
    d.density = 4.0f;
    shapeBase(c, d);

    writeF32(c, "bodyRestitution", 1.0f);
    writeF32(c, "bodyFriction", 0.0f);
    writeRefField(c, "softImpactSound");
    writeRefField(c, "hardImpactSound");

    writeF32(c, "minImpactSpeed", 25.0f);
    writeF32(c, "softImpactSpeed", 25.0f);
    writeF32(c, "hardImpactSpeed", 50.0f);
    writeF32(c, "minRollSpeed", 0.0f);
    writeF32(c, "maxSteerinAngle", maxSteeringAngle);
    writeF32(c, "maxDrag", 0.0f);
    writeF32(c, "minDrag", 0.0f);
    writeF32(c, "jetForce", 500.0f);
    writeF32(c, "jetEnergyDrain", 0.8f);
    writeF32(c, "minJetEnergy", 1.0f);
    writeF32(c, "cameraOffset", 0.0f);
    writeF32(c, "cameraLag", 0.0f);
    writeF32(c, "triggerDustHeight", 3.0f);
    writeF32(c, "dustHeight", 1.0f);
    writeF32(c, "numDmgEmitterAreas", 0.0f);
    writeF32(c, "exitSplashSoundVelocity", 2.0f);
    writeF32(c, "softSplashSoundVelocity", 1.0f);
    writeF32(c, "mediumSplashSoundVelocity", 2.0f);
    writeF32(c, "hardSplashSoundVelocity", 3.0f);

    // VehicleData::waterSound: ExitWater, ImpactSoft, ImpactMedium, ImpactHard, Wake.
    writeRefField(c, "exitingWater");
    writeRefField(c, "impactWaterEasy");
    writeRefField(c, "impactWaterMedium");
    writeRefField(c, "impactWaterHard");
    writeRefField(c, "waterWakeSound");

    writeRefField(c, "dustEmitter");
    for (int i = 0; i < 3; ++i) writeRefField(c, at("damageEmitter", i).c_str());
    for (int i = 0; i < 2; ++i) writeRefField(c, at("splashEmitter", i).c_str());
    for (int i = 0; i < 2; ++i) writePointField(c, at("damageEmitterOffset", i).c_str(), {0, 0, 0});
    for (int i = 0; i < 2; ++i) writeF32(c, at("damageLevelTolerance", i).c_str(), 0.0f);
    writeF32(c, "splashFreqMod", 300.0f);
    writeF32(c, "splashVelEpsilon", 0.5f);
    writeF32(c, "collDamageThresholdVel", 20.0f);
    writeF32(c, "collDamageMultiplier", 0.05f);
}

// FlyingVehicleData::packData
void flyingVehicle(Context& c) {
    vehicle(c, (float)M_PI);
    writeRefField(c, "jetSound");
    writeRefField(c, "engineSound");
    writeRefField(c, "forwardJetEmitter");
    writeRefField(c, "backwardJetEmitter");
    writeRefField(c, "downJetEmitter");
    writeRefField(c, "trailEmitter");
    writeF32(c, "maneuveringForce", 0.0f);
    writeF32(c, "horizontalSurfaceForce", 0.0f);
    writeF32(c, "verticalSurfaceForce", 0.0f);
    writeF32(c, "autoInputDamping", 1.0f);
    writeF32(c, "steeringForce", 1.0f);
    writeF32(c, "steeringRollForce", 1.0f);
    writeF32(c, "rollForce", 1.0f);
    writeF32(c, "autoAngularForce", 0.0f);
    writeF32(c, "rotationalDrag", 0.0f);
    writeF32(c, "autoLinearForce", 0.0f);
    writeF32(c, "maxAutoSpeed", 0.0f);
    writeF32(c, "hoverHeight", 2.0f);
    writeF32(c, "createHoverHeight", 2.0f);
    writeF32(c, "minTrailSpeed", 1.0f);
    writeF32(c, "vertThrustMultiple", 1.0f);
    // Tribes 2 maxForwardSpeed (the V12 constructor's maxSpeed = 100).
    writeF32(c, "maxForwardSpeed", 100.0f);
}

// HoverVehicleData::packData
void hoverVehicle(Context& c) {
    auto& w = c.w;
    vehicle(c, 0.785f);
    // preload validation, applied on the server before transmission.
    float dragForce = c.f32("dragForce", 0.0f);
    if (dragForce <= 0.01f) dragForce = 0.01f;
    auto unit = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    w.writeF32(dragForce);
    w.writeF32(unit(c.f32("vertFactor", 0.25f)));
    w.writeF32(unit(c.f32("floatingThrustFactor", 0.15f)));
    writeF32(c, "mainThrustForce", 0.0f);
    writeF32(c, "reverseThrustForce", 0.0f);
    writeF32(c, "strafeThrustForce", 0.0f);
    writeF32(c, "turboFactor", 1.0f);
    writeF32(c, "stabLenMin", 0.5f);
    writeF32(c, "stabLenMax", 2.0f);
    writeF32(c, "stabSpringConstant", 30.0f);
    writeF32(c, "stabDampingConstant", 10.0f);
    writeF32(c, "gyroDrag", 10.0f);
    writeF32(c, "normalForce", 30.0f);
    writeF32(c, "restorativeForce", 10.0f);
    writeF32(c, "steeringForce", 25.0f);
    writeF32(c, "rollForce", 2.5f);
    writeF32(c, "pitchForce", 2.5f);
    writePointField(c, "dustTrailOffset", {0, 0, 0});
    writeF32(c, "triggerTrailHeight", 2.5f);
    writeF32(c, "dustTrailFreqMod", 15.0f);
    writeRefField(c, "jetSound");
    writeRefField(c, "engineSound");
    writeRefField(c, "floatSound");
    // jetEmitter[3]: only the forward emitter is a persist field.
    writeRefField(c, "forwardJetEmitter");
    c.writeRef(0);
    c.writeRef(0);
    writeRefField(c, "dustTrailEmitter");
    writeF32(c, "floatingGravMag", 1.0f);
    writeF32(c, "brakingForce", 0.0f);
    writeF32(c, "brakingActivationSpeed", 0.0f);
}

// WheeledVehicleData::packData
void wheeledVehicle(Context& c) {
    vehicle(c, 0.785f);
    writeF32(c, "tireFriction", 0.3f);
    writeF32(c, "tireRestitution", 1.0f);
    writeF32(c, "tireRadius", 0.6f);
    writeF32(c, "tireLateralForce", 1000.0f);
    writeF32(c, "tireLateralDamping", 100.0f);
    writeF32(c, "tireLateralRelaxation", 1.0f);
    writeF32(c, "tireLongitudinalForce", 1000.0f);
    writeF32(c, "tireLongitudinalDamping", 100.0f);
    writeF32(c, "tireLogitudinalRelaxation", 1.0f); // engine spelling
    writeRefField(c, "tireEmitter");
    writeRefField(c, "jetSound");
    writeRefField(c, "engineSound");
    writeRefField(c, "squeelSound");
    writeRefField(c, "WheelImpactSound");
    writeF32(c, "springForce", 0.6f);
    writeF32(c, "springDamping", 0.0f); // not initialised by the constructor
    writeF32(c, "antiSwayForce", 1.0f);
    writeF32(c, "antiRockForce", 0.0f);
    writeF32(c, "maxWheelSpeed", 40.0f);
    writeF32(c, "engineTorque", 1.0f);
    writeF32(c, "breakTorque", 1.0f);
    writeF32(c, "staticLoadScale", 1.0f);
    writeF32(c, "stabilizerForce", 0.0f);
    writeF32(c, "gyroForce", 0.0f);
    writeF32(c, "gyroDamping", 0.0f);
}

// TurretData::packData
void turret(Context& c) {
    auto& w = c.w;
    staticShape(c);
    // preload clamps thetaMin to [0, 90] and thetaMax to [90, 180].
    float thetaMin = c.f32("thetaMin", 45.0f);
    float thetaMax = c.f32("thetaMax", 135.0f);
    if (thetaMin < 0.0f || thetaMin > 90.0f) thetaMin = thetaMin < 0.0f ? 0.0f : 90.0f;
    if (thetaMax < 90.0f || thetaMax > 180.0f) thetaMax = thetaMax < 90.0f ? 90.0f : 180.0f;
    w.writeF32(thetaMin);
    w.writeF32(thetaMax);
    writeF32(c, "thetaNull", -1.0f);
    writeFlagField(c, "neverUpdateControl", false);
    w.writeRangedU32((uint32_t)c.enumValue("primaryAxis", {"yaxis", "revyaxis", "zaxis", "revzaxis"}, 2), 0, 3);
    writeF32(c, "maxCapacitorEnergy", 0.0f);
    writeF32(c, "capacitorRechargeRate", 0.0f);
}

// ItemData::packData
void item(Context& c) {
    auto& w = c.w;
    ShapeDefaults d;
    d.density = 2.0f;
    d.drag = 0.5f;
    shapeBase(c, d);
    w.writeFloat(c.f32("friction", 0.0f), 10);
    w.writeFloat(c.f32("elasticity", 0.0f), 10);
    writeFlagField(c, "sticky", false);
    const float gravityMod = c.f32("gravityMod", 1.0f);
    if (w.writeFlag(gravityMod != 1.0f)) w.writeFloat(gravityMod, 10);
    const float maxVelocity = c.f32("maxVelocity", -1.0f);
    if (w.writeFlag(maxVelocity != -1.0f)) w.writeF32(maxVelocity);
    const int lightType = c.enumValue("lightType", {"NoLight", "ConstantLight", "PulsingLight"}, 0);
    if (w.writeFlag(lightType != 0)) {
        w.writeInt(lightType, 2);
        for (float v : c.colorF("lightColor", {1, 1, 1, 1})) w.writeFloat(v, 7);
        writeS32(c, "lightTime", 1000);
        writeF32(c, "lightRadius", 10.0f);
        writeFlagField(c, "lightOnlyStatic", false);
    }
}

// --------------------------------------------------------------------------
// ShapeBaseImageData::packData (retail layout).

// offsetTransform from the "offset" (TypeMatrixPosition) and "rotation"
// (TypeMatrixRotation: axis + degrees) fields, written as
// BitStream::writeAffineTransform when it is not the identity.
void writeOffsetTransform(Context& c) {
    auto& w = c.w;
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (c.has("rotation")) {
        float ax = 0, ay = 0, az = 0, deg = 0;
        std::sscanf(c.str("rotation").c_str(), "%g %g %g %g", &ax, &ay, &az, &deg);
        const float angle = deg * (float)(M_PI / 180.0);
        const float s = std::sin(angle * 0.5f), co = std::cos(angle * 0.5f);
        const float x = ax * s, y = ay * s, z = az * s, qw = co;
        // QuatF::setMatrix (identity below 10E-20f).
        if (x * x + y * y + z * z >= 10E-20f) {
            const float xs = x * 2, ys = y * 2, zs = z * 2;
            const float wx = qw * xs, wy = qw * ys, wz = qw * zs;
            const float xx = x * xs, xy = x * ys, xz = x * zs;
            const float yy = y * ys, yz = y * zs, zz = z * zs;
            m[0] = 1 - (yy + zz); m[4] = xy - wz;       m[8] = xz + wy;
            m[1] = xy + wz;       m[5] = 1 - (xx + zz); m[9] = yz - wx;
            m[2] = xz - wy;       m[6] = yz + wx;       m[10] = 1 - (xx + yy);
        }
    }
    if (c.has("offset")) {
        float p[4] = {0, 0, 0, 1};
        std::sscanf(c.str("offset").c_str(), "%g %g %g %g", &p[0], &p[1], &p[2], &p[3]);
        m[3] = p[0]; m[7] = p[1]; m[11] = p[2]; m[15] = p[3];
    }
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (w.writeFlag(std::equal(m, m + 16, identity))) return;

    w.writePoint({m[3], m[7], m[11]});
    // QuatF::set(const MatrixF&), then normalize.
    auto idx = [&](int r, int col) { return m[r * 4 + col]; };
    float q[4]; // x, y, z, w
    const float trace = idx(0, 0) + idx(1, 1) + idx(2, 2);
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f);
        q[3] = s * 0.5f;
        s = 0.5f / s;
        q[0] = (idx(1, 2) - idx(2, 1)) * s;
        q[1] = (idx(2, 0) - idx(0, 2)) * s;
        q[2] = (idx(0, 1) - idx(1, 0)) * s;
    } else {
        int i = 0;
        if (idx(1, 1) > idx(0, 0)) i = 1;
        if (idx(2, 2) > idx(i, i)) i = 2;
        const int j = (i + 1) % 3, k = (j + 1) % 3;
        float s = std::sqrt((idx(i, i) - (idx(j, j) + idx(k, k))) + 1.0f);
        q[i] = s * 0.5f;
        s = 0.5f / s;
        q[j] = (idx(i, j) + idx(j, i)) * s;
        q[k] = (idx(i, k) + idx(k, i)) * s;
        q[3] = (idx(j, k) - idx(k, j)) * s;
    }
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len == 0.0f) { q[0] = q[1] = q[2] = 0; q[3] = 1; }
    else for (float& v : q) v /= len;
    w.writeF32(q[0]);
    w.writeF32(q[1]);
    w.writeF32(q[2]);
    w.writeFlag(q[3] < 0.0f);
}

void shapeImage(Context& c) {
    auto& w = c.w;
    writeComputeCRC(c);
    w.writeString(c.str("shapeFile"));
    writeS32(c, "mountPoint", 0);
    writeOffsetTransform(c);
    writeFlagField(c, "firstPerson", true);
    writeF32(c, "mass", 0.0f);
    writeFlagField(c, "usesEnergy", false);
    writeF32(c, "minEnergy", 2.0f);
    // hasFlash: set in preload when a state sequence has a "_vis" twin in
    // the shape. Not derivable from fields.
    w.writeFlag(false);
    writeRefField(c, "projectile");
    writeRefField(c, "muzzleFlash");
    if (w.writeFlag(c.boolean("isSeeker", false))) {
        writeF32(c, "seekRadius", 0.0f);
        writeF32(c, "maxSeekAngle", 0.0f);
        writeF32(c, "seekTime", 0.0f);
        writeF32(c, "minSeekHeat", 0.4f);
        writeFlagField(c, "useTargetAudio", true);
        writeF32(c, "minTargetingDistance", 0.0f);
    }
    writeFlagField(c, "cloakable", true);
    const int lightType = c.enumValue("lightType",
        {"NoLight", "ConstantLight", "PulsingLight", "WeaponFireLight"}, 0);
    w.writeRangedU32((uint32_t)lightType, 0, 3);
    if (lightType != 0) {
        writeF32(c, "lightRadius", 10.0f);
        writeS32(c, "lightTime", 1000);
        for (float v : c.colorF("lightColor", {1, 1, 1, 1})) w.writeFloat(v, 7);
    }
    const float r = (float)(1.0 / std::sqrt(2.0)); // (1, 0, 1) normalized
    writePointField(c, "shellExitDir", {r, 0.0f, r});
    writeF32(c, "shellExitVariance", 20.0f);
    writeF32(c, "shellVelocity", 1.0f);
    writeRefField(c, "casing");
    writeFlagField(c, "accuFire", false);

    // State machine (MaxStates 31). Transitions are written +1 so that the
    // engine's -1 (none) is 0; an unresolvable name is state 0 (lookupState).
    constexpr int MaxStates = 31;
    std::string names[MaxStates];
    for (int i = 0; i < MaxStates; ++i) names[i] = c.str(at("stateName", i).c_str());
    auto lookup = [&](const std::string& name) -> int {
        if (name.empty()) return -1;
        for (int i = 0; i < MaxStates; ++i)
            if (!names[i].empty() && strcasecmp(names[i].c_str(), name.c_str()) == 0) return i;
        return 0;
    };
    // Packing order: loaded[0..1], ammo[0..1], target[0..1], wet[0..1],
    // trigger[0..1], timeout.
    static const char* const transitions[11] = {
        "stateTransitionOnNotLoaded", "stateTransitionOnLoaded",
        "stateTransitionOnNoAmmo", "stateTransitionOnAmmo",
        "stateTransitionOnNoTarget", "stateTransitionOnTarget",
        "stateTransitionOnNotWet", "stateTransitionOnWet",
        "stateTransitionOnTriggerUp", "stateTransitionOnTriggerDown",
        "stateTransitionOnTimeout"};
    for (int i = 0; i < MaxStates; ++i) {
        if (!w.writeFlag(!names[i].empty())) continue;
        auto field = [&](const char* base) { return at(base, i); };
        w.writeString(names[i]);
        for (const char* t : transitions)
            w.writeInt(lookup(c.str(field(t).c_str())) + 1, 5);
        const float timeout = c.f32(field("stateTimeoutValue").c_str(), 0.0f);
        if (w.writeFlag(timeout != 0.0f)) w.writeF32(timeout);
        writeFlagField(c, field("stateWaitForTimeout").c_str(), true);
        writeFlagField(c, field("stateFire").c_str(), false);
        writeFlagField(c, field("stateEjectShell").c_str(), false);
        w.writeFlag(true); // scaleAnimation: not a persist field, always true
        writeFlagField(c, field("stateDirection").c_str(), true);
        // Retail's extra state flag; the retail binary registers
        // stateShockwave (no V12 counterpart) among the state fields.
        writeFlagField(c, field("stateShockwave").c_str(), false);
        const float drain = c.f32(field("stateEnergyDrain").c_str(), 0.0f);
        if (w.writeFlag(drain != 0.0f)) w.writeF32(drain);
        w.writeInt(c.enumValue(field("stateLoadedFlag").c_str(), {"Ignore", "Loaded", "Empty"}, 0), 3);
        w.writeInt(c.enumValue(field("stateSpinThread").c_str(),
                               {"Ignore", "Stop", "SpinUp", "SpinDown", "FullSpeed"}, 0), 3);
        w.writeInt(c.enumValue(field("stateRecoil").c_str(),
                               {"NoRecoil", "LightRecoil", "MediumRecoil", "HeavyRecoil"}, 0), 3);
        // sequence / sequenceVis: shape->findSequence in preload; -1 (the
        // default, so no value is sent) without the shape.
        w.writeFlag(false);
        w.writeFlag(false);
        w.writeFlag(false); // flashSequence (needs sequenceVis)
        writeFlagField(c, field("stateIgnoreLoadedForReady").c_str(), false);
        const uint32_t emitter = c.ref(field("stateEmitter").c_str());
        c.writeRef(emitter);
        if (emitter >= ObjectIdFirst && emitter <= ObjectIdLast) {
            writeF32(c, field("stateEmitterTime").c_str(), 0.0f);
            w.writeInt(-1, 32); // emitterNode: shape->findNode in preload
        }
        writeRefField(c, field("stateSound").c_str());
    }
}

// TurretImageData::packData (retail layout), with onAdd's validation.
void turretImage(Context& c) {
    auto& w = c.w;
    shapeImage(c);
    constexpr int32_t TickMs = 32;
    auto delay = [&](const char* field) {
        int32_t v = c.s32(field, 1000);
        if (v < TickMs || v > 5000) v = v < TickMs ? TickMs : 5000;
        return (v >> 5) << 5;
    };
    auto degPerSec = [&](const char* field, float fallback) {
        float v = c.f32(field, fallback);
        if (v < 1.0f || v > 1080.0f) v = v < 1.0f ? 1.0f : 1080.0f;
        return (uint32_t)v;
    };
    w.writeInt(delay("activationMS") >> 5, 8);
    w.writeInt(delay("deactivateDelayMS") >> 5, 8);
    w.writeRangedU32(degPerSec("degPerSecTheta", 45.0f), 0, 1080);
    w.writeRangedU32(degPerSec("degPerSecPhi", 180.0f), 0, 1080);
    writeFlagField(c, "dontFireInsideDamageRadius", false);
    writeF32(c, "damageRadius", 0.0f);
    writeFlagField(c, "useCapacitor", false);
}

// --------------------------------------------------------------------------
// StationFXPersonalData::packData (numArcSegments is sent twice).
void stationFXPersonal(Context& c) {
    auto& w = c.w;
    const int32_t arcs = c.s32("numArcSegments", 10);
    writeF32(c, "delay", 0.0f);
    writeF32(c, "fadeDelay", 1.5f);
    writeF32(c, "lifetime", 2.0f);
    writeF32(c, "height", 2.5f);
    w.writeInt(arcs, 32);
    writeF32(c, "numDegrees", 180.0f);
    writeF32(c, "trailFadeTime", 0.2f);
    writeF32(c, "leftRadius", 2.0f);
    writeF32(c, "rightRadius", 2.0f);
    w.writeInt(arcs, 32);
    w.writeString(c.str("leftNodeName"));
    w.writeString(c.str("rightNodeName"));
    for (int i = 0; i < 2; ++i) w.writeString(c.str(at("texture", i).c_str()));
}

// StationFXVehicleData::packData
void stationFXVehicle(Context& c) {
    auto& w = c.w;
    writeF32(c, "glowTopHeight", 0.5f);
    writeF32(c, "glowBottomHeight", 0.1f);
    writeF32(c, "glowTopRadius", 8.0f);
    writeF32(c, "glowBottomRadius", 7.5f);
    writeS32(c, "numGlowSegments", 20);
    writeF32(c, "glowFadeTime", 1.0f);
    writeF32(c, "armLightDelay", 2.0f);
    writeF32(c, "armLightLifetime", 5.0f);
    writeF32(c, "armLightFadeTime", 2.0f);
    writeF32(c, "lifetime", 6.0f);
    writeS32(c, "numArcSegments", 10);
    c.writeColorI(c.colorF("sphereColor", {0.1f, 0.1f, 0.5f, 1.0f}));
    writeS32(c, "spherePhiSegments", 13);
    writeS32(c, "sphereThetaSegments", 5);
    writeF32(c, "sphereRadius", 12.0f);
    writePointField(c, "sphereScale", {1.0f, 1.0f, 0.85f});
    w.writeString(c.str("glowNodeName"));
    for (int i = 0; i < 4; ++i) {
        w.writeString(c.str(at("leftNodeName", i).c_str()));
        w.writeString(c.str(at("rightNodeName", i).c_str()));
    }
    for (int i = 0; i < 2; ++i) w.writeString(c.str(at("texture", i).c_str()));
}

} // namespace

void registerShapes() {
    registerClass("ShapeBaseData", [](Context& c) { shapeBase(c); });
    registerClass("CameraData", [](Context& c) { shapeBase(c); });
    registerClass("MissionMarkerData", [](Context& c) { shapeBase(c); });
    registerClass("StaticShapeData", staticShape);
    registerClass("TurretData", turret);
    registerClass("ItemData", item);
    registerClass("PlayerData", player);
    registerClass("FlyingVehicleData", flyingVehicle);
    registerClass("HoverVehicleData", hoverVehicle);
    registerClass("WheeledVehicleData", wheeledVehicle);
    registerClass("ShapeBaseImageData", shapeImage);
    registerClass("TurretImageData", turretImage);
    registerClass("StationFXPersonalData", stationFXPersonal);
    registerClass("StationFXVehicleData", stationFXVehicle);
}

} // namespace DataBlockPack
