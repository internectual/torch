#include "net/v12_datablocks.h"

#include <algorithm>

namespace {
using Stream = V12BitStream;

thread_local V12::DecodedDataBlock* activeDecoded = nullptr;

void refs(Stream& s, int n) { while (n--) { if (s.readFlag()) s.readUnsigned(11); } }
void strings(Stream& s, int n) { while (n--) s.readString(); }
std::vector<std::string> materialStrings(Stream& s, int n) {
    std::vector<std::string> result;
    result.reserve(n);
    while (n--) result.push_back(s.readString());
    return result;
}
void f32s(Stream& s, int n) { while (n--) s.readF32(); }
void u32s(Stream& s, int n) { while (n--) s.readUnsigned(32); }
void bools(Stream& s, int n) { while (n--) s.readUnsigned(8); }
void colors(Stream& s, int n) { while (n--) u32s(s, 1); }
void ranged(Stream& s, uint32_t max, int n) { while (n--) s.readRange(0, max); }
uint32_t rangedValue(Stream& s, uint32_t max) { return s.readRange(0, max); }
void rangedS32(Stream& s, int min, int max) { s.readRange(0, (uint32_t)(max - min)); }
void rangedF32(Stream& s, float min, float max, int bits) { s.readFloat(bits); }
uint32_t optionalRef(Stream& s) { return s.readFlag() ? s.readUnsigned(11) : 0; }
void audioDescription(Stream& s) {
    const float volume = s.readFloat(6);
    const bool looping = s.readFlag();
    int32_t loopCount = -1, minLoopGap = 0, maxLoopGap = 0;
    if (looping) {
        loopCount = s.readSigned(32);
        minLoopGap = s.readSigned(32);
        maxLoopGap = s.readSigned(32);
    }
    const bool is3d = s.readFlag();
    float minDistance = 1.0f, maxDistance = 100.0f;
    if (is3d) {
        minDistance = s.readF32();
        maxDistance = s.readF32();
        s.readUnsigned(9); s.readUnsigned(9); s.readFloat(6);
        s.readNormalVector(8); s.readF32();
    }
    s.readUnsigned(3);
    if (activeDecoded) {
        activeDecoded->audioVolume = volume;
        activeDecoded->audioLooping = looping;
        activeDecoded->audioLoopCount = loopCount;
        activeDecoded->audioMinLoopGapMs = std::max(0, minLoopGap);
        activeDecoded->audioMaxLoopGapMs = std::max(activeDecoded->audioMinLoopGapMs, maxLoopGap);
        activeDecoded->audioIs3D = is3d;
        activeDecoded->audioMinDistance = minDistance;
        activeDecoded->audioMaxDistance = maxDistance;
    }
}
void audioProfile(Stream& s) {
    const uint32_t description = optionalRef(s);
    const uint32_t environment = optionalRef(s);
    // AudioProfileData has a third optional datablock reference on the wire
    // before the filename. Keep it consumed even though runtime playback only
    // needs the description and environment references here.
    optionalRef(s);
    std::string filename = s.readString();
    if (!filename.empty() && (filename.size() < 4 ||
        filename.substr(filename.size() - 4) != ".wav"))
        filename += ".wav";
    if (activeDecoded) {
        activeDecoded->audioDescriptionRef = description;
        activeDecoded->audioEnvironmentRef = environment;
        activeDecoded->audioFilename = filename;
    }
}
void particle(Stream& s) {
    auto* d = activeDecoded;
    const float drag = s.readFloat(10);
    const bool hasWind = s.readFlag();
    const float wind = hasWind ? s.readF32() : 0.0f;
    const float gravity = s.readSignedFloat(12);
    const float inherited = s.readFloat(9);
    const bool hasAcceleration = s.readFlag();
    const float acceleration = hasAcceleration ? s.readF32() : 0.0f;
    const uint32_t lifetime = s.readUnsigned(10);
    const uint32_t lifetimeVariance = s.readUnsigned(10);
    const bool hasSpin = s.readFlag();
    const float spin = hasSpin ? s.readF32() : 0.0f;
    const bool hasSpinRandom = s.readFlag();
    const uint32_t spinMin = hasSpinRandom ? s.readUnsigned(11) : 0;
    const uint32_t spinMax = hasSpinRandom ? s.readUnsigned(11) : 0;
    const bool inverseAlpha = s.readFlag();
    if (d) {
        d->hasParticle = true;
        d->particle.dragCoefficient = drag * 5.0f;
        d->particle.windCoefficient = wind;
        d->particle.gravityCoefficient = gravity * 10.0f;
        d->particle.inheritedVelFactor = inherited;
        d->particle.constantAcceleration = acceleration;
        d->particle.lifetimeMS = lifetime << 5;
        d->particle.lifetimeVarianceMS = lifetimeVariance << 5;
        d->particle.spinSpeed = spin;
        d->particle.spinRandomMin = (float)spinMin - 1000.0f;
        d->particle.spinRandomMax = (float)spinMax - 1000.0f;
        d->particle.useInvAlpha = inverseAlpha;
    }
    const int keys = (int)s.readUnsigned(2) + 1;
    for (int i = 0; i < keys; ++i) {
        V12::DecodedDataBlock::ParticleKey key;
        key.red = s.readFloat(7); key.green = s.readFloat(7);
        key.blue = s.readFloat(7); key.alpha = s.readFloat(7);
        key.size = s.readFloat(14); key.time = s.readFloat(8);
        if (d) d->particle.keys.push_back(key);
    }
    const int textures = (int)s.readUnsigned(6);
    for (int i = 0; i < textures; ++i) {
        std::string texture = s.readString();
        if (d) d->particle.textures.push_back(std::move(texture));
    }
}
void emitter(Stream& s) {
    auto* d = activeDecoded;
    const uint32_t period = s.readUnsigned(10), variance = s.readUnsigned(10);
    const uint32_t velocity = s.readUnsigned(16), velocityVariance = s.readUnsigned(14);
    const bool hasOffset = s.readFlag();
    const uint32_t offset = hasOffset ? s.readUnsigned(16) : 0;
    const uint32_t thetaMin = s.readRange(0, 180), thetaMax = s.readRange(0, 180);
    const bool hasPhiRef = s.readFlag();
    const uint32_t phiRef = hasPhiRef ? s.readRange(0, 360) : 0;
    const bool hasPhiVariance = s.readFlag();
    const uint32_t phiVariance = hasPhiVariance ? s.readRange(0, 360) : 0;
    const bool overrideAdvances = s.readFlag();
    const bool orientParticles = s.readFlag();
    const bool orientOnVelocity = s.readFlag();
    const uint32_t lifetime = s.readUnsigned(10), lifetimeVariance = s.readUnsigned(10);
    const bool useEmitterSizes = s.readFlag();
    const bool useEmitterColors = s.readFlag();
    if (d) {
        d->hasEmitter = true;
        d->emitter.ejectionPeriodMS = period; d->emitter.periodVariance = variance;
        d->emitter.ejectionVelocity = velocity; d->emitter.velocityVariance = velocityVariance;
        d->emitter.ejectionOffset = offset; d->emitter.thetaMin = thetaMin; d->emitter.thetaMax = thetaMax;
        d->emitter.phiReferenceVel = phiRef; d->emitter.phiVariance = phiVariance;
        d->emitter.overrideAdvances = overrideAdvances; d->emitter.orientParticles = orientParticles;
        d->emitter.orientOnVelocity = orientOnVelocity; d->emitter.lifetimeMS = lifetime << 5;
        d->emitter.lifetimeVarianceMS = lifetimeVariance << 5;
        d->emitter.useEmitterSizes = useEmitterSizes;
        d->emitter.useEmitterColors = useEmitterColors;
    }
    const uint32_t count=s.readUnsigned(32);
    if (count > s.remainingBits()) {
        s.fail();
        return;
    }
    for (uint32_t i=0;i<count;++i) {
        const uint32_t ref = optionalRef(s);
        if (d && ref) d->emitter.particleRefs.push_back(ref);
    }
}
void explosion(Stream& s) {
    auto* d = activeDecoded;
    s.readString(); // dts file name
    const uint32_t soundProfile = optionalRef(s);
    const uint32_t particleEmitterRef = optionalRef(s);
    const int32_t density = s.readSigned(14);
    const float radius = s.readF32();
    s.readFlag();
    if (s.readFlag()) { s.readUnsigned(16); s.readUnsigned(16); s.readUnsigned(16); }
    s.readSigned(14); // play speed
    const int32_t debrisThetaMin = (int32_t)rangedValue(s, 180);
    const int32_t debrisThetaMax = (int32_t)rangedValue(s, 180);
    const int32_t debrisPhiMin = (int32_t)rangedValue(s, 360);
    const int32_t debrisPhiMax = (int32_t)rangedValue(s, 360);
    const int32_t debrisNum = (int32_t)rangedValue(s, 1000);
    const int32_t debrisNumVariance = (int32_t)rangedValue(s, 1000);
    const float debrisVelocity = (float)s.readSigned(14);
    const float debrisVelocityVariance = (float)rangedValue(s, 10000);
    const int32_t delay = s.readSigned(16) * 32;
    const int32_t delayVariance = s.readSigned(16) * 32;
    const int32_t lifetime = s.readSigned(16) * 32;
    const int32_t lifetimeVariance = s.readSigned(16) * 32;
    const float offset = s.readF32();
    const bool hasLight = s.readFlag();
    const bool shakeCamera = s.readFlag();
    std::array<float, 3> shakeFrequency{};
    std::array<float, 3> shakeAmplitude{};
    for (float& value : shakeFrequency) value = s.readF32();
    for (float& value : shakeAmplitude) value = s.readF32();
    const float shakeDuration = s.readF32();
    const float shakeRadius = s.readF32();
    const float shakeFalloff = s.readF32();
    const uint32_t shockwaveRef = optionalRef(s);
    const uint32_t debrisRef = optionalRef(s);
    std::vector<uint32_t> emitters;
    for (int i = 0; i < 4; ++i) emitters.push_back(optionalRef(s));
    std::vector<uint32_t> subExplosions;
    for (int i = 0; i < 5; ++i) subExplosions.push_back(optionalRef(s));
    const int timeCount = (int)s.readRange(0, 4);
    for (int i = 0; i < timeCount; ++i) s.readFloat(8);
    for (int i = 0; i < timeCount; ++i) {
        ranged(s, 16000, 1); ranged(s, 16000, 1); ranged(s, 16000, 1);
    }
    if (d) {
        d->hasExplosion = true;
        d->explosion.soundProfileRef = soundProfile;
        d->explosion.soundProfileRef = soundProfile;
        d->explosion.particleEmitterRef = particleEmitterRef;
        d->explosion.particleDensity = density;
        d->explosion.particleRadius = radius;
        d->explosion.delayMS = delay;
        d->explosion.delayVarianceMS = delayVariance;
        d->explosion.lifetimeMS = lifetime;
        d->explosion.lifetimeVarianceMS = lifetimeVariance;
        d->explosion.offset = offset;
        d->explosion.debrisThetaMin = debrisThetaMin;
        d->explosion.debrisThetaMax = debrisThetaMax;
        d->explosion.debrisPhiMin = debrisPhiMin;
        d->explosion.debrisPhiMax = debrisPhiMax;
        d->explosion.debrisNum = debrisNum;
        d->explosion.debrisNumVariance = debrisNumVariance;
        d->explosion.debrisVelocity = debrisVelocity;
        d->explosion.debrisVelocityVariance = debrisVelocityVariance;
        d->explosion.debrisRef = debrisRef;
        d->explosion.shockwaveRef = shockwaveRef;
        d->explosion.emitterRefs = std::move(emitters);
        d->explosion.subExplosionRefs = std::move(subExplosions);
        d->explosion.hasLight = hasLight;
        d->explosion.shakeCamera = shakeCamera;
        d->explosion.shakeFrequency = shakeFrequency;
        d->explosion.shakeAmplitude = shakeAmplitude;
        d->explosion.shakeDuration = shakeDuration;
        d->explosion.shakeRadius = shakeRadius;
        d->explosion.shakeFalloff = shakeFalloff;
    }
}

void shockwave(Stream& s) {
    auto* d = activeDecoded;
    f32s(s, 3);
    const int32_t delay = s.readSigned(32), delayVariance = s.readSigned(32);
    const int32_t lifetime = s.readSigned(32), lifetimeVariance = s.readSigned(32);
    const float width = s.readF32();
    const int32_t segments = s.readSigned(32), verticalSegments = s.readSigned(32);
    const float velocity = s.readF32(), height = s.readF32();
    const float verticalCurve = s.readF32(), acceleration = s.readF32(), texWrap = s.readF32();
    const bool is2D = s.readUnsigned(8) != 0;
    const bool orientToNormal = s.readUnsigned(8) != 0;
    const bool mapToTerrain = s.readUnsigned(8) != 0;
    const bool renderBottom = s.readUnsigned(8) != 0;
    const bool renderSquare = s.readUnsigned(8) != 0;
    std::vector<uint32_t> emitters;
    for (int i = 0; i < 3; ++i) emitters.push_back(optionalRef(s));
    std::vector<uint32_t> colors;
    for (int i = 0; i < 4; ++i) colors.push_back(s.readUnsigned(32));
    std::vector<float> times;
    for (int i = 0; i < 4; ++i) times.push_back(s.readF32());
    std::vector<std::string> textures;
    textures.push_back(s.readString()); textures.push_back(s.readString());
    if (d) {
        d->hasShockwave = true;
        d->shockwave.delayMS = delay; d->shockwave.delayVariance = delayVariance;
        d->shockwave.lifetimeMS = lifetime; d->shockwave.lifetimeVariance = lifetimeVariance;
        d->shockwave.width = width; d->shockwave.numSegments = segments;
        d->shockwave.numVertSegments = verticalSegments; d->shockwave.velocity = velocity;
        d->shockwave.height = height; d->shockwave.verticalCurve = verticalCurve;
        d->shockwave.acceleration = acceleration; d->shockwave.texWrap = texWrap;
        d->shockwave.is2D = is2D; d->shockwave.orientToNormal = orientToNormal;
        d->shockwave.mapToTerrain = mapToTerrain; d->shockwave.renderBottom = renderBottom;
        d->shockwave.renderSquare = renderSquare; d->shockwave.emitterRefs = std::move(emitters);
        d->shockwave.colors = std::move(colors); d->shockwave.times = std::move(times);
        d->shockwave.textures = std::move(textures);
    }
}

void splash(Stream& s) {
    auto* d = activeDecoded;
    V12::DecodedDataBlock::SplashData value;
    value.scale.x = s.readF32(); value.scale.y = s.readF32(); value.scale.z = s.readF32();
    value.delayMS = s.readSigned(32); value.delayVarianceMS = s.readSigned(32);
    value.lifetimeMS = s.readSigned(32); value.lifetimeVarianceMS = s.readSigned(32);
    value.width = s.readF32(); value.numSegments = s.readUnsigned(32);
    value.velocity = s.readF32(); value.height = s.readF32();
    value.acceleration = s.readF32(); value.texWrap = s.readF32();
    value.texFactor = s.readF32(); value.ejectionFreq = s.readF32();
    value.ejectionAngle = s.readF32(); value.ringLifetime = s.readF32();
    value.startRadius = s.readF32();
    value.explosionRef = optionalRef(s);
    for (int i = 0; i < 3; ++i) value.emitterRefs.push_back(optionalRef(s));
    for (int i = 0; i < 4; ++i) value.colors.push_back(s.readUnsigned(32));
    for (int i = 0; i < 4; ++i) value.times.push_back(s.readF32());
    for (int i = 0; i < 2; ++i) value.textures.push_back(s.readString());
    if (d) { d->hasSplash = true; d->splash = std::move(value); }
}

void decal(Stream& s) {
    auto* d = activeDecoded;
    const std::string texture = s.readString();
    const int32_t lifetime = s.readSigned(32);
    const int32_t fadeTime = s.readSigned(32);
    const uint32_t rows = s.readUnsigned(8);
    const uint32_t cols = s.readUnsigned(8);
    const bool randomize = s.readFlag();
    const bool renderPriority = s.readFlag();
    if (d) {
        d->hasDecal = true;
        d->decal.texture = texture;
        d->decal.lifetimeMS = lifetime;
        d->decal.fadeTimeMS = fadeTime;
        d->decal.textureRows = rows ? rows : 1;
        d->decal.textureCols = cols ? cols : 1;
        d->decal.randomize = randomize;
        d->decal.renderPriority = renderPriority;
    }
}
void legacyDecal(Stream& s) {
    const float sizeX = s.readF32();
    const float sizeY = s.readF32();
    const std::string texture = s.readString();
    if (activeDecoded) {
        activeDecoded->hasDecal = true;
        activeDecoded->decal.texture = texture;
        activeDecoded->decal.sizeX = sizeX;
        activeDecoded->decal.sizeY = sizeY;
        // Legacy DecalManager used a fixed five-second queue timeout and
        // faded during its final quarter; the legacy datablock has no timing
        // fields of its own.
        activeDecoded->decal.lifetimeMS = 5000;
        activeDecoded->decal.fadeTimeMS = 1250;
    }
}
void shapeBase(Stream& s) {
    if (s.readFlag()) s.readUnsigned(32);
    const std::string shape = s.readHuffmanString();
    if (activeDecoded) activeDecoded->shapeFile = shape;
    for (int i = 0; i < 9; ++i) if (s.readFlag()) s.readF32();
    const std::string debrisShape = s.readHuffmanString();
    if (activeDecoded) activeDecoded->debrisShape = debrisShape;
    if (s.readFlag()) { s.readUnsigned(10); u32s(s, 1); }
    if (s.readFlag()) s.readF32();
    if (activeDecoded) activeDecoded->cloakTexture = s.readString();
    else s.readString();
    s.readString();
    for (int i = 0; i < 6; ++i) s.readFlag();
    refs(s, 4);
    for (int i = 0; i < 3; ++i) s.readFlag();
    s.readUnsigned(32);
    if (s.readFlag()) f32s(s, 3);
    for (int i = 0; i < 8; ++i) {
        if (!s.readFlag()) continue;
        s.readHuffmanString();
        if (s.readFlag()) s.readHuffmanString();
        for (int j = 0; j < 5; ++j) s.readFlag();
    }
}

void projectile(Stream& s) {
    const std::string shape = s.readString();
    const int32_t emitterDelay = s.readSigned(32);
    const float bubbleEmitTime = s.readF32();
    const bool faceViewer = s.readFlag();
    V12Vec3 scale{1.0f, 1.0f, 1.0f};
    const bool hasScale = s.readFlag();
    if (hasScale) {
        scale.x = s.readF32(); scale.y = s.readF32(); scale.z = s.readF32();
    }
    const uint32_t baseEmitter = optionalRef(s);
    std::vector<uint32_t> effectRefs;
    effectRefs.push_back(baseEmitter);
    for (int i = 0; i < 8; ++i) effectRefs.push_back(optionalRef(s));
    std::vector<uint32_t> decalRefs;
    for (int i = 0; i < 6; ++i) decalRefs.push_back(optionalRef(s));
    const bool hasLight = s.readFlag();
    const float lightRadius = hasLight ? s.readFloat(8) * 20.0f : 1.0f;
    std::array<float, 3> lightColor{1.0f, 1.0f, 1.0f};
    if (hasLight)
        for (float& value : lightColor) value = s.readFloat(7);
    const bool hasUnderwaterLight = s.readFlag();
    std::array<float, 3> underwaterLightColor{1.0f, 1.0f, 1.0f};
    if (hasUnderwaterLight)
        for (float& value : underwaterLightColor) value = s.readFloat(7);
    const bool explodeOnWaterImpact = s.readUnsigned(8) != 0;
    const float depthTolerance = s.readF32();
    if (activeDecoded) {
        activeDecoded->shapeFile = shape;
        activeDecoded->emitterDelayMS = emitterDelay;
        activeDecoded->bubbleEmitTime = bubbleEmitTime;
        activeDecoded->faceViewer = faceViewer;
        activeDecoded->projectileScale = scale;
        activeDecoded->hasProjectileScale = hasScale;
        activeDecoded->projectileBaseEmitterRef = baseEmitter;
        activeDecoded->projectileDelayEmitterRef = effectRefs[1];
        activeDecoded->projectileBubbleEmitterRef = effectRefs[2];
        activeDecoded->projectileExplosionRef = effectRefs[3];
        activeDecoded->projectileUnderwaterExplosionRef = effectRefs[4];
        activeDecoded->projectileSplashRef = effectRefs[5];
        activeDecoded->projectileSoundRef = effectRefs[6];
        activeDecoded->projectileWetFireSoundRef = effectRefs[7];
        activeDecoded->projectileFireSoundRef = effectRefs[8];
        activeDecoded->effectRefs = std::move(effectRefs);
        activeDecoded->projectileDecalRefs = std::move(decalRefs);
        activeDecoded->projectileHasLight = hasLight;
        activeDecoded->projectileLightRadius = lightRadius;
        activeDecoded->projectileLightColor = lightColor;
        activeDecoded->projectileHasUnderwaterLightColor = hasUnderwaterLight;
        activeDecoded->projectileUnderwaterLightColor = underwaterLightColor;
        activeDecoded->projectileExplodeOnWaterImpact = explodeOnWaterImpact;
        activeDecoded->projectileDepthTolerance = depthTolerance;
    }
}

void linear(Stream& s) {
    projectile(s);
    const float dryVelocity = s.readF32(), wetVelocity = s.readF32();
    const int32_t fizzle = (int32_t)s.readUnsigned(32);
    const int32_t lifetime = (int32_t)s.readUnsigned(32);
    const bool explodeOnDeath = s.readFlag();
    const uint32_t reflectAngle = rangedValue(s, 90), deflection = rangedValue(s, 90);
    const int32_t fizzleUnderwater = (int32_t)s.readUnsigned(32);
    const int32_t activateDelay = (int32_t)s.readUnsigned(32);
    const bool dynamicHits = s.readFlag();
    if (activeDecoded) {
        activeDecoded->projectileDryVelocity = dryVelocity;
        activeDecoded->projectileWetVelocity = wetVelocity;
        activeDecoded->projectileFizzleTimeMS = fizzle;
        activeDecoded->projectileLifetimeMS = lifetime;
        activeDecoded->projectileExplodeOnDeath = explodeOnDeath;
        (void)reflectAngle; (void)deflection; (void)fizzleUnderwater;
        (void)activateDelay; (void)dynamicHits;
    }
}
void grenade(Stream& s) { projectile(s); s.readUnsigned(32); f32s(s, 6); s.readUnsigned(32); }
void shapeImage(Stream& s) {
    if (s.readFlag()) s.readUnsigned(32);
    const std::string shapeName = s.readString();
    const uint32_t mountPoint = s.readUnsigned(32);
    if (activeDecoded) {
        activeDecoded->shapeFile = shapeName;
        activeDecoded->mountPoint = mountPoint;
        activeDecoded->hasMountPoint = true;
    }
    if (!s.readFlag()) {
        s.readPoint3F();
        s.readF32();
        s.readF32();
        s.readF32();
        s.readFlag();
    }
    s.readFlag(); s.readF32(); s.readFlag(); s.readF32(); s.readFlag(); refs(s, 2);
    if (s.readFlag()) { f32s(s, 4); s.readFlag(); s.readF32(); }
    s.readFlag();
    const uint32_t lightType = s.readRange(0, 3);
    if (lightType != 0) { s.readF32(); s.readSigned(32); for (int i=0;i<4;++i)s.readFloat(7); }
    f32s(s, 3); s.readF32(); s.readF32(); refs(s, 1); s.readFlag();
    for (int i=0;i<31;++i) {
        const bool hasState = s.readFlag();
        if (!hasState) continue;
        s.readString();
        for (int j=0;j<11;++j)s.readUnsigned(5);
        if (s.readFlag())s.readF32(); for (int j=0;j<6;++j)s.readFlag();
        if (s.readFlag()) s.readF32();
        s.readUnsigned(3); s.readUnsigned(3); s.readUnsigned(3);
        if(s.readFlag())s.readSignedInt(16); if(s.readFlag())s.readSignedInt(16);
        s.readFlag(); s.readFlag();
        const bool hasEmitter = s.readFlag();
        if (hasEmitter) { s.readUnsigned(11); s.readF32(); s.readSigned(32); }
        if (s.readFlag()) s.readUnsigned(11);
    }
}

void player(Stream& s) {
    shapeBase(s); s.readFlag(); f32s(s, 13); refs(s, 2); f32s(s, 9); s.readF32(); f32s(s, 8);
    s.readUnsigned(7); f32s(s, 6); f32s(s, 9); s.readF32(); refs(s, 32); f32s(s, 3);
    refs(s, 1); f32s(s, 2); refs(s, 1); s.readF32(); refs(s, 2); refs(s, 3); f32s(s, 11);
}

void vehicle(Stream& s) { shapeBase(s); f32s(s,2); refs(s,2); f32s(s,19); refs(s,5); refs(s,1); refs(s,3); refs(s,2); f32s(s,12); }

void effect(Stream& s, int index) {
    switch (index) {
    case 0: f32s(s,1); s.readFlag(); if (s.readFlag()) u32s(s,3); s.readFlag(); if(s.readFlag()){f32s(s,2);s.readUnsigned(9);s.readUnsigned(9);s.readFloat(6);s.readNormalVector(8);s.readF32();}s.readUnsigned(3); break;
    case 2: refs(s,3); strings(s,1); break;
    case 3: s.readFlag(); if(!s.readFlag()){for(int i=0;i<4;++i)s.readUnsigned(32);for(int i=0;i<5;++i)s.readFloat(8);}s.readFloat(8);break;
    case 4: f32s(s,6); bools(s,4); f32s(s,2); bools(s,2); f32s(s,3); bools(s,1); strings(s,2); refs(s,3); break;
     case 9: f32s(s,2); strings(s,1); break;
    case 11: f32s(s,3); bools(s,1); strings(s,1); break;
    case 22: colors(s,2); f32s(s,7); if(s.readFlag())strings(s,1); break;
    case 28: f32s(s,1); break;
    case 34: f32s(s,1); colors(s,1); f32s(s,1); strings(s,1); f32s(s,6); if(s.readFlag())strings(s,1); break;
    default: break;
    }
}
}

namespace V12 {
bool readDataBlockPayload(V12BitStream& s, size_t classId,
                          DecodedDataBlock* decoded) {
    activeDecoded = decoded;
    if (activeDecoded) *activeDecoded = {};
    const size_t i = classId >= 128 ? classId - 128 : classId;
    if (i >= 54) { activeDecoded = nullptr; return false; }
    switch (i) {
     case 0: audioDescription(s); break;
     case 1: if (s.readFlag()) s.readRange(0, 26); else { rangedS32(s,-10000,0); rangedS32(s,-10000,10000); rangedS32(s,-10000,2000); rangedF32(s,0.1f,10,8); rangedF32(s,0.1f,20,8); rangedF32(s,0.1f,20,8); rangedF32(s,0,0.3f,9); rangedF32(s,0,0.1f,7); rangedS32(s,-10000,0); rangedF32(s,0,1,9); rangedF32(s,0,2,10); rangedF32(s,1,100,8); rangedF32(s,0,1,10); s.readUnsigned(6); } rangedF32(s,0,1,8); break;
     case 2: audioProfile(s); break;
     case 3: rangedS32(s,-10000,1000); rangedS32(s,-10000,0); rangedS32(s,-10000,1000); rangedS32(s,-10000,0); rangedF32(s,0,1,9); rangedF32(s,0,1,8); rangedF32(s,0,1,9); rangedF32(s,0,1,8); rangedF32(s,0,10,9); rangedF32(s,0,10,9); rangedF32(s,0,10,9); rangedS32(s,-10000,0); s.readUnsigned(3); break;
     case 4: grenade(s); f32s(s,6); strings(s,2); break;
      case 5: shapeBase(s); break;
      case 6: {
          // DebrisData carries the debris shape as a regular network string.
          // It is not shapeBase's Huffman-encoded field layout.
          const std::string shape = s.readString();
           if (activeDecoded) {
               activeDecoded->debrisShape = shape;
               activeDecoded->debris.shape = shape;
           }
          break;
      }
      case 7: strings(s,5); break;
     case 8: f32s(s,2); u32s(s,2); f32s(s,2); bools(s,4); f32s(s,4); f32s(s,2); bools(s,2); f32s(s,3); bools(s,1); strings(s,2); refs(s,3); break;
     case 9: (classId >= 128 ? legacyDecal : decal)(s); break; case 10: projectile(s); f32s(s,6); strings(s,3); refs(s,1); break;
     case 11: f32s(s,3); s.readUnsigned(8); strings(s,1); break; case 12: {
         grenade(s); float values[7]; for (float& value : values) value = s.readF32();
         const auto textures = materialStrings(s, 2);
         if (activeDecoded) {
             activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::Cross;
             activeDecoded->projectileCrossViewAngle = values[0]; activeDecoded->projectileCrossSize = values[1];
             activeDecoded->projectileMaterialTextures = textures;
         }
         break;
     }
      case 13: explosion(s); break;
     case 14: refs(s,1); break; case 15: {
         grenade(s); const float size = s.readF32(); const bool lens = s.readUnsigned(8) != 0;
         const auto textures = materialStrings(s, 2);
         if (activeDecoded) {
             activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::Flare;
             activeDecoded->projectileMaterialSizes[0] = size; activeDecoded->projectileUseLensFlare = lens;
             activeDecoded->projectileMaterialTextures = textures;
         }
         break;
     }
    case 16: vehicle(s); refs(s,6); f32s(s,16); break; case 17: f32s(s,3); s.readFlag();s.readFlag();colors(s,2);s.readUnsigned(32);s.readUnsigned(32);f32s(s,3);strings(s,5);break;
    case 18: break; case 19: grenade(s); break; case 20: vehicle(s); f32s(s,17);f32s(s,3);f32s(s,2);refs(s,7);f32s(s,3);break;
    case 21: shapeBase(s); s.readFloat(10);s.readFloat(10);s.readFlag();if(s.readFlag())s.readFloat(10);if(s.readFlag())s.readF32();if(s.readFlag()){s.readUnsigned(2);for(int j=0;j<4;++j)s.readFloat(7);s.readSigned(32);s.readF32();s.readFlag();}break;
     case 22: effect(s,22); break; case 23: refs(s,8);strings(s,8);refs(s,1);break; case 24: {
         linear(s); const uint32_t count = s.readUnsigned(32); const auto color = s.readUnsigned(32);
         const auto textures = materialStrings(s, 2); float sizes[3]; for (float& size : sizes) size = s.readF32();
         if (activeDecoded) {
             activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::LinearFlare;
             activeDecoded->projectileFlareCount = count; activeDecoded->projectileMaterialTextures = textures;
             activeDecoded->projectileMaterialSizes = {sizes[0], sizes[1], sizes[2]};
             activeDecoded->projectileMaterialColor = {((color >> 0) & 0xff) / 255.0f,
                 ((color >> 8) & 0xff) / 255.0f, ((color >> 16) & 0xff) / 255.0f,
                 ((color >> 24) & 0xff) / 255.0f};
         }
         break;
     }
    case 25: linear(s); break; case 26: shapeBase(s); break; case 27: particle(s); break;
    case 28: s.readF32(); break; case 29: emitter(s); break;
     case 30: player(s); break; case 31: refs(s,1);s.readUnsigned(32);s.readF32();strings(s,1);f32s(s,13);break; case 32: projectile(s);break; case 33: projectile(s);f32s(s,8);strings(s,2);break;
     case 34: s.readF32();colors(s,1);f32s(s,2);strings(s,1);f32s(s,6);if(s.readFlag())strings(s,1);break; case 35: projectile(s);f32s(s,10);s.readUnsigned(8);f32s(s,2);s.readUnsigned(32);s.readUnsigned(32);strings(s,2);refs(s,3);break; case 36: break; case 37: shapeBase(s);break; case 38: shapeImage(s);break;
      case 39: projectile(s);f32s(s,7);refs(s,1);f32s(s,8);strings(s,4);refs(s,1);break; case 40: shockwave(s); break; case 41: break; case 42: projectile(s);f32s(s,11);colors(s,2);f32s(s,1);strings(s,12);break; case 43: splash(s); break;
       case 44: shapeBase(s);s.readFlag();s.readUnsigned(32);break; case 45: f32s(s,10);strings(s,4);break; case 46: f32s(s,11);colors(s,1);f32s(s,6);strings(s,11);break; case 47: strings(s,1);strings(s,(int)s.readUnsigned(7));break; case 48: projectile(s);s.readF32();colors(s,1);f32s(s,7);strings(s,4);break; case 49: {
           linear(s); float values[3]; for (float& value : values) value = s.readF32();
           const bool tracerAlpha = s.readUnsigned(8) != 0; const auto color = s.readUnsigned(32);
           float cross[2]; for (float& value : cross) value = s.readF32();
           const bool renderCross = s.readUnsigned(8) != 0; const auto textures = materialStrings(s, 2);
           if (activeDecoded) {
               activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::Cross;
               activeDecoded->projectileTracerLength = values[0]; activeDecoded->projectileTracerMinPixels = values[1];
               activeDecoded->projectileTracerWidth = values[2]; activeDecoded->projectileTracerAlpha = tracerAlpha;
               activeDecoded->projectileCrossViewAngle = cross[0]; activeDecoded->projectileCrossSize = cross[1];
               activeDecoded->projectileRenderCross = renderCross; activeDecoded->projectileMaterialTextures = textures;
               activeDecoded->projectileMaterialColor = {((color >> 0) & 0xff) / 255.0f,
                   ((color >> 8) & 0xff) / 255.0f, ((color >> 16) & 0xff) / 255.0f,
                   ((color >> 24) & 0xff) / 255.0f};
           }
           break;
       } case 50:s.readUnsigned(32);break;
       case 51:shapeBase(s);s.readFlag();s.readUnsigned(32);f32s(s,3);s.readFlag();ranged(s,3,1);f32s(s,2);break;
      case 52:shapeImage(s);s.readUnsigned(8);s.readUnsigned(8);ranged(s,1080,2);s.readFlag();s.readF32();s.readFlag();break; case 53:vehicle(s);f32s(s,9);refs(s,5);f32s(s,11);break;
    }
    const bool ok = !s.failed();
    activeDecoded = nullptr;
    return ok;
}
}
