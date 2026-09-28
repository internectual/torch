// DataBlockPack group: see src/sim/datablock_pack.h.
//
// Explosions, debris, splashes, shockwaves, particles, decals, weather and
// the Tribes 2 effect datablocks. Layouts are Tribes 2 build 25034's
// packData as Torch's reader (src/net/v12_datablocks.cpp) consumes them;
// field names, defaults and onAdd validation follow the engine source
// (game/fx/explosion.cc, game/debris.cc, game/fx/splash.cc,
// game/shockwave.cc, game/fx/particleEngine.cc, game/fx/particleEmitter.cc,
// sim/decalManager.cc, game/fx/precipitation.cc, game/fx/lightning.cc,
// game/fireballAtmosphere.cc). EffectProfile, JetEffectData and
// RunningLightData exist only in the retail binary; their names and order
// come from the reference parser (DataBlockParsers.js).
#include "sim/datablock_pack.h"
#include "script/script_engine.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <strings.h>
#include <vector>

namespace DataBlockPack {
namespace {

using Point = TorqueBitWriter::Point;

// Key of element `index` of an array persist field: "name[index]" as the
// script engine stores it (any spelling of the index, e.g. "animTexName[00]"),
// or the bare name for element 0 (Torque's `texture = ...` sets texture[0]).
std::string arrayKey(const Context& c, const char* name, int index) {
    const std::string exact = std::string(name) + "[" + std::to_string(index) + "]";
    if (c.has(exact.c_str())) return exact;
    if (c.data) {
        const size_t n = std::char_traits<char>::length(name);
        for (const auto& entry : c.data->fields) {
            const std::string& key = entry.first;
            if (key.size() < n + 3 || strncasecmp(key.c_str(), name, n) != 0) continue;
            if (key[n] != '[' || key.back() != ']') continue;
            const std::string digits = key.substr(n + 1, key.size() - n - 2);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                               [](unsigned char ch) { return std::isdigit(ch); }))
                continue;
            if (std::atoi(digits.c_str()) == index) return key;
        }
    }
    if (index == 0 && c.has(name)) return name;
    return exact;
}

// Resolves a datablock name the way Context::ref resolves a field value.
Point toPoint(const std::array<float, 3>& p) { return Point{p[0], p[1], p[2]}; }

void writePoint(Context& c, const std::array<float, 3>& p) { c.w.writePoint(toPoint(p)); }

// Stream::write(S32 / U32): 32 raw bits.
void writeS32(Context& c, int32_t value) { c.w.writeInt(value, 32); }

// ExplosionData --------------------------------------------------------------
void packExplosion(Context& c) {
    auto& w = c.w;
    w.writeString(c.str("explosionShape"));
    c.writeRef(c.ref("soundProfile"));
    c.writeRef(c.ref("particleEmitter"));
    w.writeInt(c.s32("particleDensity", 10), 14);
    w.writeF32(c.f32("particleRadius", 1.0f));
    w.writeFlag(c.boolean("faceViewer", false));

    // ExplosionData::onAdd validation.
    auto scale = c.point("explosionScale", {1, 1, 1});
    for (float& v : scale) v = std::max(v, 0.01f);
    if (w.writeFlag(scale[0] != 1 || scale[1] != 1 || scale[2] != 1))
        for (float v : scale) w.writeInt((int32_t)(v * 100), 16);
    const float playSpeed = std::max(c.f32("playSpeed", 1.0f), 0.05f);
    w.writeInt((int32_t)(playSpeed * 20), 14);

    float thetaMin = std::max(c.f32("debrisThetaMin", 0.0f), 0.0f);
    const float thetaMax = std::min(c.f32("debrisThetaMax", 90.0f), 180.0f);
    thetaMin = std::min(thetaMin, thetaMax);
    float phiMin = std::max(c.f32("debrisPhiMin", 0.0f), 0.0f);
    const float phiMax = std::min(c.f32("debrisPhiMax", 360.0f), 360.0f);
    phiMin = std::min(phiMin, phiMax);
    w.writeRangedU32((uint32_t)thetaMin, 0, 180);
    w.writeRangedU32((uint32_t)std::max(thetaMax, 0.0f), 0, 180);
    w.writeRangedU32((uint32_t)phiMin, 0, 360);
    w.writeRangedU32((uint32_t)std::max(phiMax, 0.0f), 0, 360);
    w.writeRangedU32((uint32_t)std::clamp(c.s32("debrisNum", 1), 0, 1000), 0, 1000);
    w.writeRangedU32((uint32_t)std::clamp(c.s32("debrisNumVariance", 0), 0, 1000), 0, 1000);
    const float debrisVelocity = std::max(c.f32("debrisVelocity", 2.0f), 0.1f);
    const float debrisVelocityVariance = std::clamp(c.f32("debrisVelocityVariance", 0.0f), 0.0f, 1000.0f);
    w.writeInt((int32_t)(debrisVelocity * 10), 14);
    w.writeRangedU32((uint32_t)(debrisVelocityVariance * 10), 0, 10000);

    const int32_t delayMS = std::max(c.s32("delayMS", 0), 0);
    const int32_t delayVariance = std::min(c.s32("delayVariance", 0), delayMS);
    const int32_t lifetimeMS = std::max(c.s32("lifetimeMS", 1000), 1);
    const int32_t lifetimeVariance = std::min(c.s32("lifetimeVariance", 0), lifetimeMS);
    w.writeInt(delayMS >> 5, 16);
    w.writeInt(delayVariance >> 5, 16);
    w.writeInt(lifetimeMS >> 5, 16);
    w.writeInt(lifetimeVariance >> 5, 16);
    w.writeF32(std::max(c.f32("offset", 0.0f), 0.0f));

    // Retail: shakeCamera, then the Tribes 2 light flag (not in the V12 source).
    w.writeFlag(c.boolean("shakeCamera", false));
    w.writeFlag(c.boolean("hasLight", false));
    writePoint(c, c.point("camShakeFreq", {10, 10, 10}));
    writePoint(c, c.point("camShakeAmp", {1, 1, 1}));
    w.writeF32(c.f32("camShakeDuration", 1.5f));
    w.writeF32(c.f32("camShakeRadius", 10.0f));
    w.writeF32(c.f32("camShakeFalloff", 10.0f));

    // Retail adds the shockwave ahead of debris[1], emitter[4], subExplosion[5].
    c.writeRef(c.ref("shockwave"));
    c.writeRef(c.ref(arrayKey(c, "debris", 0).c_str()));
    for (int i = 0; i < 4; ++i) c.writeRef(c.ref(arrayKey(c, "emitter", i).c_str()));
    for (int i = 0; i < 5; ++i) c.writeRef(c.ref(arrayKey(c, "subExplosion", i).c_str()));

    constexpr int TimeKeys = 4;
    float times[TimeKeys];
    std::array<float, 3> sizes[TimeKeys];
    for (int i = 0; i < TimeKeys; ++i) {
        times[i] = c.f32(arrayKey(c, "times", i).c_str(), i == 0 ? 0.0f : 1.0f);
        sizes[i] = c.point(arrayKey(c, "sizes", i).c_str(), {1, 1, 1});
    }
    // Keys up to and including the first time >= 1.
    int count = 0;
    while (count < TimeKeys && times[count] < 1) ++count;
    count = std::min(count + 1, TimeKeys);
    w.writeRangedU32((uint32_t)count, 0, TimeKeys);
    for (int i = 0; i < count; ++i) w.writeFloat(times[i], 8);
    for (int i = 0; i < count; ++i)
        for (float v : sizes[i]) w.writeRangedU32((uint32_t)std::max(v * 100, 0.0f), 0, 16000);
}

// DebrisData -----------------------------------------------------------------
void packDebris(Context& c) {
    auto& w = c.w;
    // DebrisData::onAdd validation.
    float velocity = c.f32("velocity", 0.0f);
    float velocityVariance = std::min(c.f32("velocityVariance", 0.0f), velocity);
    float friction = c.f32("friction", 0.2f);
    if (friction < -10 || friction > 10) friction = 0.2f;
    float elasticity = c.f32("elasticity", 0.3f);
    if (elasticity < -10 || elasticity > 10) elasticity = 0.2f;
    float lifetime = c.f32("lifetime", 3.0f);
    if (lifetime < 0 || lifetime > 1000) lifetime = 3.0f;
    float lifetimeVariance = c.f32("lifetimeVariance", 0.0f);
    if (lifetimeVariance < 0 || lifetimeVariance > lifetime) lifetimeVariance = 0.0f;
    int32_t numBounces = c.s32("numBounces", 0);
    if (numBounces < 0 || numBounces > 10000) numBounces = 3;
    int32_t bounceVariance = c.s32("bounceVariance", 0);
    if (bounceVariance < 0 || bounceVariance > numBounces) bounceVariance = 0;
    float minSpin = c.f32("minSpinSpeed", 0.0f);
    float maxSpin = c.f32("maxSpinSpeed", 0.0f);
    if (minSpin < -10000 || minSpin > 10000 || minSpin > maxSpin) minSpin = maxSpin - 1.0f;
    if (maxSpin < -10000 || maxSpin > 10000) maxSpin = 0.0f;

    w.writeF32(elasticity);
    w.writeF32(friction);
    writeS32(c, numBounces);
    writeS32(c, bounceVariance);
    w.writeF32(minSpin);
    w.writeF32(maxSpin);
    c.writeBoolByte(c.boolean("render2D", false));
    c.writeBoolByte(c.boolean("explodeOnMaxBounce", false));
    c.writeBoolByte(c.boolean("staticOnMaxBounce", false));
    c.writeBoolByte(c.boolean("snapOnMaxBounce", false));
    w.writeF32(lifetime);
    w.writeF32(lifetimeVariance);
    w.writeF32(minSpin);   // written twice, as the engine does
    w.writeF32(maxSpin);
    w.writeF32(velocity);
    w.writeF32(velocityVariance);
    c.writeBoolByte(c.boolean("fade", true));
    c.writeBoolByte(c.boolean("useRadiusMass", false));
    w.writeF32(c.f32("baseRadius", 1.0f));
    w.writeF32(c.f32("gravModifier", 1.0f));
    w.writeF32(c.f32("terminalVelocity", 0.0f));
    c.writeBoolByte(c.boolean("ignoreWater", true));
    w.writeString(c.str("texture"));
    w.writeString(c.str("shapeName"));
    for (int i = 0; i < 2; ++i) c.writeRef(c.ref(arrayKey(c, "emitters", i).c_str()));
    c.writeRef(c.ref("explosion"));
}

// SplashData / ShockwaveData shared tail: emitters, colours, times, textures.
void packRingTail(Context& c, bool writeExplosion) {
    auto& w = c.w;
    if (writeExplosion) c.writeRef(c.ref("explosion"));
    for (int i = 0; i < 3; ++i) c.writeRef(c.ref(arrayKey(c, "emitter", i).c_str()));
    for (int i = 0; i < 4; ++i) c.writeColorI(c.colorF(arrayKey(c, "colors", i).c_str(), {1, 1, 1, 1}));
    for (int i = 0; i < 4; ++i) w.writeF32(c.f32(arrayKey(c, "times", i).c_str(), i == 0 ? 0.0f : 1.0f));
    // The V12 ShockwaveData skips empty textures; retail always sends both.
    for (int i = 0; i < 2; ++i) w.writeString(c.str(arrayKey(c, "texture", i).c_str()));
}

void packSplash(Context& c) {
    auto& w = c.w;
    writePoint(c, c.point("scale", {1, 1, 1}));
    writeS32(c, c.s32("delayMS", 0));
    writeS32(c, c.s32("delayVariance", 0));
    writeS32(c, c.s32("lifetimeMS", 1000));
    writeS32(c, c.s32("lifetimeVariance", 0));
    w.writeF32(c.f32("width", 4.0f));
    writeS32(c, c.s32("numSegments", 10));
    w.writeF32(c.f32("velocity", 5.0f));
    w.writeF32(c.f32("height", 0.0f));
    w.writeF32(c.f32("acceleration", 0.0f));
    w.writeF32(c.f32("texWrap", 1.0f));
    w.writeF32(c.f32("texFactor", 3.0f));
    w.writeF32(c.f32("ejectionFreq", 5.0f));
    w.writeF32(c.f32("ejectionAngle", 45.0f));
    w.writeF32(c.f32("ringLifetime", 1.0f));
    w.writeF32(c.f32("startRadius", 0.5f));
    packRingTail(c, true);
}

void packShockwave(Context& c) {
    auto& w = c.w;
    writePoint(c, c.point("scale", {1, 1, 1}));
    writeS32(c, c.s32("delayMS", 0));
    writeS32(c, c.s32("delayVariance", 0));
    writeS32(c, c.s32("lifetimeMS", 1000));
    writeS32(c, c.s32("lifetimeVariance", 0));
    w.writeF32(c.f32("width", 4.0f));
    writeS32(c, c.s32("numSegments", 10));
    writeS32(c, c.s32("numVertSegments", 1));
    w.writeF32(c.f32("velocity", 30.0f));
    w.writeF32(c.f32("height", 0.0f));
    w.writeF32(c.f32("verticalCurve", 1.0f));
    w.writeF32(c.f32("acceleration", 0.0f));
    w.writeF32(c.f32("texWrap", 1.0f));
    c.writeBoolByte(c.boolean("is2D", false));
    c.writeBoolByte(c.boolean("orientToNormal", false));
    c.writeBoolByte(c.boolean("mapToTerrain", true));
    c.writeBoolByte(c.boolean("renderBottom", false));
    c.writeBoolByte(c.boolean("renderSquare", false));
    packRingTail(c, false);
}

// ParticleEmitterData ----------------------------------------------------------
void packEmitter(Context& c) {
    auto& w = c.w;
    // ParticleEmitterData::onAdd validation.
    const int32_t period = std::max(c.s32("ejectionPeriodMS", 100), 1);
    int32_t periodVariance = c.s32("periodVarianceMS", 0);
    if (periodVariance >= period) periodVariance = period - 1;
    const float velocity = std::max(c.f32("ejectionVelocity", 2.0f), 0.0f);
    const float velocityVariance = std::min(c.f32("velocityVariance", 1.0f), velocity);
    const float offset = std::max(c.f32("ejectionOffset", 0.0f), 0.0f);
    float thetaMin = std::max(c.f32("thetaMin", 0.0f), 0.0f);
    const float thetaMax = std::min(c.f32("thetaMax", 90.0f), 180.0f);
    thetaMin = std::min(thetaMin, thetaMax);
    const float phiReferenceVel = c.f32("phiReferenceVel", 0.0f);
    const float phiVariance = std::clamp(c.f32("phiVariance", 360.0f), 0.0f, 360.0f);
    const int32_t lifetimeMS = std::max(c.s32("lifetimeMS", 0), 0);
    const int32_t lifetimeVarianceMS = std::min(c.s32("lifetimeVarianceMS", 0), lifetimeMS);

    w.writeInt(period, 10);
    w.writeInt(periodVariance, 10);
    w.writeInt((int32_t)(velocity * 100), 16);
    w.writeInt((int32_t)(velocityVariance * 100), 14);
    if (w.writeFlag(offset != 0.0f)) w.writeInt((int32_t)(offset * 100), 16);
    w.writeRangedU32((uint32_t)thetaMin, 0, 180);
    w.writeRangedU32((uint32_t)std::max(thetaMax, 0.0f), 0, 180);
    if (w.writeFlag(phiReferenceVel != 0.0f)) w.writeRangedU32((uint32_t)phiReferenceVel, 0, 360);
    if (w.writeFlag(phiVariance != 360.0f)) w.writeRangedU32((uint32_t)phiVariance, 0, 360);
    // Retail scripts' "overrideAdvances" is a dynamic field; the engine
    // sends only the persist field overrideAdvance.
    w.writeFlag(c.boolean("overrideAdvance", false));
    w.writeFlag(c.boolean("orientParticles", false));
    w.writeFlag(c.boolean("orientOnVelocity", true));
    w.writeInt(lifetimeMS >> 5, 10);
    w.writeInt(lifetimeVarianceMS >> 5, 10);
    w.writeFlag(c.boolean("useEmitterSizes", false));
    w.writeFlag(c.boolean("useEmitterColors", false));

    // onAdd: the particles string's datablocks that resolve, in order.
    std::vector<uint32_t> ids;
    std::istringstream tokens(c.str("particles"));
    std::string token;
    while (tokens >> token)
        if (uint32_t id = c.resolve(token)) ids.push_back(id);
    w.writeU32((uint32_t)ids.size());
    for (uint32_t id : ids) c.writeRef(id);
}

// ParticleData -------------------------------------------------------------------
void packParticle(Context& c) {
    auto& w = c.w;
    constexpr float MaxParticleSize = 50.0f;
    constexpr int MaxTextures = 50;
    // ParticleData::onAdd validation.
    const float drag = std::max(c.f32("dragCoefficient", 0.0f), 0.0f);
    const int32_t lifetimeMS = std::max(c.s32("lifetimeMS", 1000), 1);
    int32_t lifetimeVarianceMS = c.s32("lifetimeVarianceMS", 0);
    if (lifetimeVarianceMS >= lifetimeMS) lifetimeVarianceMS = lifetimeMS - 1;
    const float wind = c.f32("windCoefficient", 1.0f);
    const float acceleration = c.f32("constantAcceleration", 0.0f);
    const float spinSpeed = c.f32("spinSpeed", 0.0f);
    const float spinMin = c.f32("spinRandomMin", 0.0f);
    const float spinMax = c.f32("spinRandomMax", 0.0f);

    w.writeFloat(drag / 5, 10);
    if (w.writeFlag(wind != 1.0f)) w.writeF32(wind);
    w.writeSignedFloat(c.f32("gravityCoefficient", 0.0f) / 10, 12);
    w.writeFloat(c.f32("inheritedVelFactor", 0.0f), 9);
    if (w.writeFlag(acceleration != 0.0f)) w.writeF32(acceleration);
    w.writeInt(lifetimeMS >> 5, 10);
    w.writeInt(lifetimeVarianceMS >> 5, 10);
    if (w.writeFlag(spinSpeed != 0.0f)) w.writeF32(spinSpeed);
    if (w.writeFlag(spinMin != 0.0f || spinMax != 0.0f)) {
        w.writeInt((int32_t)(spinMin + 1000), 11);
        w.writeInt((int32_t)(spinMax + 1000), 11);
    }
    w.writeFlag(c.boolean("useInvAlpha", false));

    static const float defaultTimes[4] = {0.0f, 1.0f, 2.0f, 2.0f};
    float times[4];
    for (int i = 0; i < 4; ++i) times[i] = c.f32(arrayKey(c, "times", i).c_str(), defaultTimes[i]);
    times[0] = 0.0f; // onAdd: times start at 0 and never decrease
    for (int i = 1; i < 4; ++i) times[i] = std::max(times[i], times[i - 1]);
    int count = 0;
    while (count < 3 && times[count] < 1) ++count;
    ++count;
    w.writeInt(count - 1, 2);
    for (int i = 0; i < count; ++i) {
        const auto color = c.colorF(arrayKey(c, "colors", i).c_str(), {1, 1, 1, 1});
        for (float v : color) w.writeFloat(v, 7);
        w.writeFloat(c.f32(arrayKey(c, "sizes", i).c_str(), 1.0f) / MaxParticleSize, 14);
        w.writeFloat(times[i], 8);
    }

    // textureName and animTexName[] share textureNameList; element 0 is
    // either, the list ends at the first unset entry.
    std::vector<std::string> textures;
    for (int i = 0; i < MaxTextures; ++i) {
        std::string found = arrayKey(c, "animTexName", i);
        if (!c.has(found.c_str())) {
            if (i != 0 || !c.has("textureName")) break;
            found = "textureName";
        }
        textures.push_back(c.str(found.c_str()));
    }
    w.writeInt((int32_t)textures.size(), 6);
    for (const auto& texture : textures) w.writeString(texture);
}

// Remaining classes --------------------------------------------------------------
void packDecal(Context& c) {
    c.w.writeF32(c.f32("sizeX", 1.0f));
    c.w.writeF32(c.f32("sizeY", 1.0f));
    c.w.writeString(c.str("textureName"));
}

void packEmissionDummy(Context& c) {
    c.w.writeF32(std::clamp(c.f32("timeMultiple", 1.0f), 0.01f, 100.0f));
}

void packPrecipitation(Context& c) {
    auto& w = c.w;
    c.writeRef(c.ref("soundProfile"));
    writeS32(c, c.s32("type", 0));
    w.writeF32(c.f32("maxSize", 1.0f));
    w.writeString(c.str("materialList"));
    float sizeX = c.f32("sizeX", 1.0f), sizeY = c.f32("sizeY", 1.0f);
    if (sizeX <= 0 || sizeX > 20) sizeX = 1.0f;
    if (sizeY <= 0 || sizeY > 20) sizeY = 1.0f;
    w.writeF32(sizeX);
    w.writeF32(sizeY);
    // Box tuning: the constructor leaves these unset; unset is sent as 0.
    for (const char* field : {"movingBoxPer", "divHeightVal", "sizeBigBox", "topBoxSpeed",
                              "frontBoxSpeed", "topBoxDrawPer", "bottomDrawHeight", "skipIfPer",
                              "bottomSpeedPer", "frontSpeedPer", "frontRadiusPer"})
        w.writeF32(c.f32(field, 0.0f));
}

void packLightning(Context& c) {
    for (int i = 0; i < 8; ++i) c.writeRef(c.ref(arrayKey(c, "thunderSounds", i).c_str()));
    for (int i = 0; i < 8; ++i) c.w.writeString(c.str(arrayKey(c, "strikeTextures", i).c_str()));
    c.writeRef(c.ref("strikeSound"));
}

void packFireball(Context& c) { c.writeRef(c.ref("fireball")); }

// Retail-only classes (constructor defaults unknown: zero / white).
void packEffectProfile(Context& c) {
    c.w.writeF32(c.f32("minDistance", 0.0f));
    c.w.writeF32(c.f32("maxDistance", 0.0f));
    c.w.writeF32(c.f32("audioScale", 0.0f));
    c.writeBoolByte(c.boolean("directional", false));
    c.w.writeString(c.str("effectName"));
}

// texture[0]: a flag, then the name when set.
void packOptionalTexture(Context& c) {
    const std::string key = arrayKey(c, "texture", 0);
    if (c.w.writeFlag(c.has(key.c_str()))) c.w.writeString(c.str(key.c_str()));
}

void packJetEffect(Context& c) {
    c.writeColorI(c.colorF("coolColor", {1, 1, 1, 1}));
    c.writeColorI(c.colorF("hotColor", {1, 1, 1, 1}));
    for (const char* field : {"activateTime", "deactivateTime", "length", "width", "speed",
                              "stretch", "yOffset"})
        c.w.writeF32(c.f32(field, 0.0f));
    packOptionalTexture(c);
}

void packRunningLight(Context& c) {
    c.w.writeF32(c.f32("radius", 0.0f));
    c.writeColorI(c.colorF("color", {1, 1, 1, 1}));
    c.w.writeF32(c.f32("type", 0.0f));
    c.w.writeF32(c.f32("length", 0.0f));
    c.w.writeString(c.str("nodeName"));
    writePoint(c, c.point("direction", {0, 0, 0}));
    writePoint(c, c.point("offset", {0, 0, 0}));
    packOptionalTexture(c);
}

} // namespace

void registerEffects() {
    registerClass("ExplosionData", packExplosion);
    registerClass("DebrisData", packDebris);
    registerClass("SplashData", packSplash);
    registerClass("ShockwaveData", packShockwave);
    registerClass("ParticleEmitterData", packEmitter);
    registerClass("ParticleData", packParticle);
    registerClass("DecalData", packDecal);
    registerClass("ParticleEmissionDummyData", packEmissionDummy);
    registerClass("PrecipitationData", packPrecipitation);
    registerClass("LightningData", packLightning);
    registerClass("FireballAtmosphereData", packFireball);
    registerClass("JetEffectData", packJetEffect);
    registerClass("RunningLightData", packRunningLight);
    registerClass("EffectProfile", packEffectProfile);
}

} // namespace DataBlockPack
