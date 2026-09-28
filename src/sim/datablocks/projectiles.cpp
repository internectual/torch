// DataBlockPack group: see src/sim/datablock_pack.h.
//
// ProjectileData family (game/projectile.cc, linearProjectile.cc,
// projGrenade.cc, proj*.cc). Each writer mirrors the engine's
// Parent::packData chain; values are what packData sees after onAdd's
// validation (clamps, tick rounding), so the written bits match what a
// server running the engine would send for the same script fields.
#include "sim/datablock_pack.h"
#include <algorithm>
#include <string>

namespace DataBlockPack {

namespace {

constexpr int32_t TickMs = 32;
constexpr int32_t LinearMaxLivingTicks = 511; // LinearProjectile::MaxLivingTicks

int32_t tickRound(int32_t ms) { return (ms + TickMs - 1) & ~(TickMs - 1); }

std::array<float, 4> clampColor(std::array<float, 4> c) {
    for (float& v : c) v = std::clamp(v, 0.0f, 1.0f);
    return c;
}

// Array persist field element: "name[i]"; element 0 also answers to the
// bare name (the console assigns an unindexed array field to element 0).
std::string arrayKey(const char* name, int i) { return std::string(name) + "[" + std::to_string(i) + "]"; }

std::string arrayStr(const Context& c, const char* name, int i, const std::string& fallback = {}) {
    const std::string key = arrayKey(name, i);
    if (c.has(key.c_str())) return c.str(key.c_str());
    if (i == 0 && c.has(name)) return c.str(name);
    return fallback;
}

float arrayF32(const Context& c, const char* name, int i, float fallback) {
    const std::string key = arrayKey(name, i);
    if (c.has(key.c_str())) return c.f32(key.c_str(), fallback);
    if (i == 0 && c.has(name)) return c.f32(name, fallback);
    return fallback;
}

uint32_t arrayRef(const Context& c, const char* name, int i) {
    const std::string key = arrayKey(name, i);
    if (c.has(key.c_str())) return c.ref(key.c_str());
    if (i == 0 && c.has(name)) return c.ref(name);
    return 0;
}

// ProjectileData::packData (GameBaseData writes nothing).
void packProjectile(Context& c) {
    auto& w = c.w;
    w.writeString(c.str("projectileShapeName"));
    int32_t emitterDelay = c.s32("emitterDelay", -1);
    if (emitterDelay < 0) emitterDelay = -1;
    w.writeInt(emitterDelay, 32);
    w.writeF32(c.f32("bubbleEmitTime", 0.5f));
    w.writeFlag(c.boolean("faceViewer", false));
    const auto scale = c.point("scale", {1.0f, 1.0f, 1.0f});
    if (w.writeFlag(scale[0] != 1.0f || scale[1] != 1.0f || scale[2] != 1.0f))
        w.writePoint({scale[0], scale[1], scale[2]});

    for (const char* ref : {"baseEmitter", "delayEmitter", "bubbleEmitter", "explosion",
                            "underwaterExplosion", "splash", "sound", "wetFireSound", "fireSound"})
        c.writeRef(c.ref(ref));
    for (int i = 0; i < 6; ++i) c.writeRef(arrayRef(c, "decalData", i));

    const bool hasLight = c.boolean("hasLight", false);
    if (w.writeFlag(hasLight)) {
        const float radius = std::clamp(c.f32("lightRadius", 1.0f), 1.0f, 20.0f);
        const auto color = clampColor(c.colorF("lightColor", {1.0f, 1.0f, 1.0f, 1.0f}));
        w.writeFloat(radius / 20.0f, 8);
        for (int i = 0; i < 3; ++i) w.writeFloat(color[i], 7);
    }
    if (w.writeFlag(c.boolean("hasLightUnderwaterColor", false))) {
        const auto color = clampColor(c.colorF("underWaterLightColor", {1.0f, 1.0f, 1.0f, 1.0f}));
        for (int i = 0; i < 3; ++i) w.writeFloat(color[i], 7);
    }
    c.writeBoolByte(c.boolean("explodeOnWaterImpact", false));
    w.writeF32(c.f32("depthTolerance", 5.0f));
}

// LinearProjectileData::packData, values after LinearProjectileData::onAdd.
void packLinear(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    const float dryVelocity = std::max(c.f32("dryVelocity", 5.0f), 0.1f);
    float wetVelocity = c.f32("wetVelocity", 5.0f);
    if (wetVelocity < 0.1f && wetVelocity != -1.0f) wetVelocity = 0.1f;
    int32_t lifetimeMS = std::clamp(c.s32("lifetimeMS", 1000), TickMs, (LinearMaxLivingTicks - 1) * TickMs);
    int32_t fizzleTimeMS = std::min(std::max(c.s32("fizzleTimeMS", 1000), 0), lifetimeMS);
    // Negative reflect angles become -1 in onAdd (U32(-1.0f), undefined);
    // written here as 0, i.e. no reflection.
    const float reflect = std::clamp(c.f32("reflectOnWaterImpactAngle", 0.0f), 0.0f, 90.0f);
    const float deflect = std::clamp(c.f32("deflectionOnWaterImpact", 0.0f), 0.0f, 90.0f);
    int32_t fizzleUnderwaterMS = c.s32("fizzleUnderwaterMS", -1);
    if (fizzleUnderwaterMS < 0 || fizzleUnderwaterMS > lifetimeMS) fizzleUnderwaterMS = -1;
    else fizzleUnderwaterMS = tickRound(fizzleUnderwaterMS);
    int32_t activateDelayMS = c.s32("activateDelayMS", -1);
    if (activateDelayMS < 0 || activateDelayMS > lifetimeMS) activateDelayMS = -1;
    lifetimeMS = tickRound(lifetimeMS);
    fizzleTimeMS = tickRound(fizzleTimeMS);

    w.writeF32(dryVelocity);
    w.writeF32(wetVelocity);
    w.writeInt(fizzleTimeMS, 32);
    w.writeInt(lifetimeMS, 32);
    w.writeFlag(c.boolean("explodeOnDeath", false));
    w.writeRangedU32((uint32_t)reflect, 0, 90);
    w.writeRangedU32((uint32_t)deflect, 0, 90);
    w.writeInt(fizzleUnderwaterMS, 32);
    w.writeInt(activateDelayMS, 32);
    w.writeFlag(c.boolean("doDynamicClientHits", false));
}

// GrenadeProjectileData::packData, values after GrenadeProjectileData::onAdd.
void packGrenade(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    const int32_t armingDelayMS = tickRound(std::max(c.s32("armingDelayMS", 3000), 250));
    const float muzzleVelocity = std::max(c.f32("muzzleVelocity", 75.0f), 0.1f);
    float elasticity = c.f32("grenadeElasticity", 0.999f);
    if (elasticity < 0.0f || elasticity > 0.999f) elasticity = elasticity < 0.0f ? 0.0f : 0.999f;
    const float friction = std::clamp(c.f32("grenadeFriction", 0.3f), 0.0f, 1.0f);
    const float drag = std::max(c.f32("drag", 0.0f), 0.0f);
    const float density = std::clamp(c.f32("density", 1.0f), 0.1f, 10.0f);
    int32_t lifetimeMS = c.s32("lifetimeMS", 20000);
    if (lifetimeMS == 0) lifetimeMS = 1000;

    w.writeInt(armingDelayMS, 32);
    w.writeF32(muzzleVelocity);
    w.writeF32(elasticity);
    w.writeF32(friction);
    w.writeF32(drag);
    w.writeF32(density);
    w.writeF32(c.f32("gravityMod", 1.0f));
    w.writeInt(lifetimeMS, 32);
}

void packSeeker(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    const int32_t lifetimeMS = tickRound(std::clamp(c.s32("lifetimeMS", 3000), 500, 30000));
    const float muzzleVelocity = std::max(c.f32("muzzleVelocity", 100.0f), 0.1f);
    const float turningSpeed = std::clamp(c.f32("turningSpeed", 180.0f), 0.0f, 2000.0f);
    const float avoidSpeed = std::clamp(c.f32("terrainAvoidanceSpeed", 180.0f), 0.0f, 2000.0f);
    const float scanAhead = std::clamp(c.f32("terrainScanAhead", 25.0f), 0.0f, 200.0f);
    const float heightFail = std::max(c.f32("terrainHeightFail", 2.0f), 0.0f);
    const float avoidRadius = std::max(c.f32("terrainAvoidanceRadius", 50.0f), 0.0f);
    const float flareDistance = std::max(c.f32("flareDistance", 200.0f), 0.0f);
    const float flareAngle = std::max(c.f32("flareAngle", 20.0f), 0.0f);
    float maxVelocity = c.f32("maxVelocity", 65.0f);
    if (maxVelocity < 0.1f) maxVelocity = muzzleVelocity + 1.0f;
    float acceleration = c.f32("acceleration", 10.0f);
    if (acceleration < 0.0f || acceleration > 30000.0f) acceleration = 0.0f;
    int32_t flechetteDelayMs = c.s32("flechetteDelayMs", 300);
    if (flechetteDelayMs > 30000) flechetteDelayMs = 500;

    w.writeInt(lifetimeMS, 32);
    w.writeF32(muzzleVelocity);
    w.writeF32(turningSpeed);
    w.writeF32(c.f32("proximityRadius", 10.0f));
    w.writeF32(avoidSpeed);
    w.writeF32(scanAhead);
    w.writeF32(heightFail);
    w.writeF32(avoidRadius);
    w.writeF32(flareDistance);
    w.writeF32(flareAngle);
    c.writeBoolByte(c.boolean("useFlechette", false));
    w.writeF32(maxVelocity);
    w.writeF32(acceleration);
    w.writeInt(flechetteDelayMs, 32);
    w.writeInt(c.s32("exhaustTimeMs", 1000), 32);
    w.writeString(c.str("exhaustNodeName"));
    w.writeString(c.str("casingShapeName"));
    c.writeRef(c.ref("casingDeb"));
    c.writeRef(c.ref("puffEmitter"));
    c.writeRef(c.ref("exhaustEmitter"));
}

void packSniper(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    const float maxRifleRange = std::clamp(c.f32("maxRifleRange", 1000.0f), 10.0f, 2000.0f);
    w.writeF32(maxRifleRange);
    w.writeF32(std::max(c.f32("rifleHeadMultiplier", 1.2f), 1.0f));
    c.writeColorI(clampColor(c.colorF("beamColor", {1.0f, 0.25f, 0.25f, 1.0f})));
    w.writeF32(std::max(c.f32("fadeTime", 1.0f), 0.25f));
    w.writeF32(c.f32("startBeamWidth", 0.25f));
    w.writeF32(c.f32("endBeamWidth", 0.5f));
    w.writeF32(c.f32("pulseBeamWidth", 0.5f));
    w.writeF32(c.f32("beamFlareAngle", 3.0f));
    w.writeF32(c.f32("minFlareSize", 0.0f));
    w.writeF32(c.f32("maxFlareSize", 400.0f));
    w.writeF32(c.f32("pulseSpeed", 6.0f));
    w.writeF32(c.f32("pulseLength", 0.150f));
    // SniperProjectileData re-registers "lightColor"/"lightRadius" for its
    // own members, but AbstractClassRep::findField returns the first match,
    // ProjectileData's; the sniper's copies keep their constructor values.
    c.writeColorI({0.4f, 0.0f, 0.0f, 1.0f});
    w.writeF32(1.0f);
    static const char* const defaults[12] = {
        "special/flare", "special/nonlingradient", "special/laserrip01", "special/laserrip02",
        "special/laserrip03", "special/laserrip04", "special/laserrip05", "special/laserrip06",
        "special/laserrip07", "special/laserrip08", "special/laserrip09", "special/sniper00"};
    for (int i = 0; i < 12; ++i) w.writeString(arrayStr(c, "textureName", i, defaults[i]));
}

void packShockLance(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    float zapDuration = c.f32("zapDuration", 0.5f);
    if (zapDuration < 0.05f || zapDuration >= 30.0f) zapDuration = zapDuration < 0.05f ? 0.05f : 2.0f;
    float boltLength = c.f32("boltLength", 2.0f);
    if (boltLength < 0.5f || boltLength >= 50.0f) boltLength = boltLength < 0.5f ? 0.5f : 5.0f;
    w.writeF32(zapDuration);
    w.writeF32(boltLength);
    w.writeInt(c.s32("numParts", 25), 32); // TypeS32 in the engine
    w.writeF32(c.f32("lightningFreq", 10.0f));
    w.writeF32(c.f32("lightningDensity", 3.0f));
    w.writeF32(c.f32("lightningAmp", 0.5f));
    w.writeF32(c.f32("lightningWidth", 0.1f));
    c.writeRef(c.ref("shockwave"));
    static const float startWidth[2] = {0.2f, 0.2f}, endWidth[2] = {1.0f, 1.0f};
    static const float boltSpeed[2] = {1.0f, 1.2f}, texWrap[2] = {1.0f, 1.0f};
    for (int i = 0; i < 2; ++i) {
        w.writeF32(arrayF32(c, "startWidth", i, startWidth[i]));
        w.writeF32(arrayF32(c, "endWidth", i, endWidth[i]));
        w.writeF32(arrayF32(c, "boltSpeed", i, boltSpeed[i]));
        w.writeF32(arrayF32(c, "texWrap", i, texWrap[i]));
    }
    for (int i = 0; i < 4; ++i) w.writeString(arrayStr(c, "texture", i));
    c.writeRef(arrayRef(c, "emitter", 0));
}

void packELF(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    w.writeF32(std::max(c.f32("beamRange", 10.0f), 2.0f));
    w.writeF32(c.f32("mainBeamWidth", 0.2f));
    w.writeF32(c.f32("mainBeamSpeed", 9.0f));
    w.writeF32(c.f32("mainBeamRepeat", 0.25f));
    w.writeF32(c.f32("lightningWidth", 0.15f));
    w.writeF32(c.f32("lightningDist", 0.15f));
    for (int i = 0; i < 3; ++i) w.writeString(arrayStr(c, "textures", i));
    c.writeRef(arrayRef(c, "emitter", 0));
}

void packRepair(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    w.writeF32(std::max(c.f32("beamRange", 10.0f), 2.0f));
    w.writeF32(c.f32("beamWidth", 0.15f));
    w.writeInt(c.s32("numSegments", 20), 32);
    w.writeF32(c.f32("beamSpeed", 1.0f));
    w.writeF32(c.f32("texRepeat", 1.0f / 5.0f));
    w.writeF32(c.f32("blurFreq", 10.0f));
    w.writeF32(c.f32("blurLifetime", 1.0f));
    w.writeF32(c.f32("cutoffAngle", 40.0f));
    w.writeString(arrayStr(c, "textures", 0, "special/redbump2"));
    w.writeString(arrayStr(c, "textures", 1, "special/redflare"));
}

void packTarget(Context& c) {
    packProjectile(c);
    auto& w = c.w;
    w.writeF32(std::clamp(c.f32("maxRifleRange", 1000.0f), 10.0f, 2000.0f));
    c.writeColorI(clampColor(c.colorF("beamColor", {0.1f, 1.0f, 0.1f, 1.0f})));
    w.writeF32(c.f32("startBeamWidth", 1.0f));
    w.writeF32(c.f32("pulseBeamWidth", 0.5f));
    w.writeF32(c.f32("beamFlareAngle", 3.0f));
    w.writeF32(c.f32("minFlareSize", 0.0f));
    w.writeF32(c.f32("maxFlareSize", 400.0f));
    w.writeF32(c.f32("pulseSpeed", 6.0f));
    w.writeF32(c.f32("pulseLength", 0.150f));
    for (int i = 0; i < 4; ++i) w.writeString(arrayStr(c, "textureName", i));
}

void packTracer(Context& c) {
    packLinear(c);
    auto& w = c.w;
    w.writeF32(std::clamp(c.f32("tracerLength", 10.0f), 1.0f, 50.0f));
    w.writeF32(c.f32("tracerWidth", 0.5f));
    w.writeF32(std::clamp(c.f32("tracerMinPixels", 3.0f), 1.0f, 20.0f));
    c.writeBoolByte(c.boolean("tracerAlpha", false));
    c.writeColorI(clampColor(c.colorF("tracerColor", {1.0f, 1.0f, 1.0f, 1.0f})));
    w.writeF32(c.f32("crossViewAng", 0.98f));
    w.writeF32(c.f32("crossSize", 0.45f));
    c.writeBoolByte(c.boolean("renderCross", true));
    for (int i = 0; i < 2; ++i) w.writeString(arrayStr(c, "tracerTex", i));
}

void packEnergy(Context& c) {
    packGrenade(c);
    auto& w = c.w;
    w.writeF32(c.f32("crossViewAng", 0.98f));
    w.writeF32(c.f32("crossSize", 0.45f));
    w.writeF32(c.f32("blurLifetime", 0.5f));
    w.writeF32(c.f32("blurWidth", 0.25f));
    const auto blur = c.colorF("blurColor", {0.4f, 0.0f, 0.0f, 1.0f});
    for (int i = 0; i < 3; ++i) w.writeF32(blur[i]);
    for (int i = 0; i < 2; ++i) w.writeString(arrayStr(c, "texture", i));
}

void packLinearFlare(Context& c) {
    packLinear(c);
    auto& w = c.w;
    w.writeInt(std::max(c.s32("numFlares", 25), 0), 32);
    c.writeColorI(clampColor(c.colorF("flareColor", {0.25f, 0.25f, 1.0f, 1.0f})));
    w.writeString(c.str("flareModTexture"));
    w.writeString(c.str("flareBaseTexture"));
    static const float sizes[3] = {0.2f, 0.25f, 3.0f};
    for (int i = 0; i < 3; ++i) w.writeF32(arrayF32(c, "size", i, sizes[i]));
}

void packBomb(Context& c) {
    packGrenade(c);
    auto& w = c.w;
    const auto minRot = c.point("minRotSpeed", {0.0f, 0.0f, 0.0f});
    const auto maxRot = c.point("maxRotSpeed", {0.0f, 0.0f, 0.0f});
    for (float v : minRot) w.writeF32(v);
    for (float v : maxRot) w.writeF32(v);
    for (int i = 0; i < 2; ++i) w.writeString(arrayStr(c, "texture", i));
}

void packFlare(Context& c) {
    packGrenade(c);
    auto& w = c.w;
    w.writeF32(c.f32("size", 50.0f));
    c.writeBoolByte(c.boolean("useLensFlare", true));
    for (int i = 0; i < 2; ++i) w.writeString(arrayStr(c, "texture", i));
}

} // namespace

void registerProjectiles() {
    registerClass("ProjectileData", packProjectile);
    registerClass("LinearProjectileData", packLinear);
    registerClass("GrenadeProjectileData", packGrenade);
    registerClass("SeekerProjectileData", packSeeker);
    registerClass("SniperProjectileData", packSniper);
    registerClass("ShockLanceProjectileData", packShockLance);
    registerClass("ELFProjectileData", packELF);
    registerClass("RepairProjectileData", packRepair);
    registerClass("TargetProjectileData", packTarget);
    registerClass("TracerProjectileData", packTracer);
    registerClass("EnergyProjectileData", packEnergy);
    registerClass("LinearFlareProjectileData", packLinearFlare);
    registerClass("BombProjectileData", packBomb);
    registerClass("FlareProjectileData", packFlare);
}

} // namespace DataBlockPack
