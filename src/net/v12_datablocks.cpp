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
    std::string shape = s.readString(); // dtsFileName
    const uint32_t soundProfile = optionalRef(s);
    const uint32_t particleEmitterRef = optionalRef(s);
    const int32_t density = (int32_t)s.readUnsigned(14);
    const float radius = s.readF32();
    const bool faceViewer = s.readFlag();
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    if (s.readFlag())
        for (float& value : scale) value = (float)s.readUnsigned(16) / 100.0f;
    const float playSpeed = (float)s.readUnsigned(14) / 20.0f;
    const int32_t debrisThetaMin = (int32_t)rangedValue(s, 180);
    const int32_t debrisThetaMax = (int32_t)rangedValue(s, 180);
    const int32_t debrisPhiMin = (int32_t)rangedValue(s, 360);
    const int32_t debrisPhiMax = (int32_t)rangedValue(s, 360);
    const int32_t debrisNum = (int32_t)rangedValue(s, 1000);
    const int32_t debrisNumVariance = (int32_t)rangedValue(s, 1000);
    // ExplosionData::unpackData packs debris speeds in tenths.
    const float debrisVelocity = (float)s.readUnsigned(14) / 10.0f;
    const float debrisVelocityVariance = (float)rangedValue(s, 10000) / 10.0f;
    const int32_t delay = (int32_t)s.readUnsigned(16) << 5;
    const int32_t delayVariance = (int32_t)s.readUnsigned(16) << 5;
    const int32_t lifetime = (int32_t)s.readUnsigned(16) << 5;
    const int32_t lifetimeVariance = (int32_t)s.readUnsigned(16) << 5;
    const float offset = s.readF32();
    // Retail order: shakeCamera, then the Tribes 2 hasLight flag.
    const bool shakeCamera = s.readFlag();
    const bool hasLight = s.readFlag();
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
    std::vector<float> times;
    for (int i = 0; i < timeCount; ++i) times.push_back(s.readFloat(8));
    std::vector<std::array<float, 3>> sizes;
    for (int i = 0; i < timeCount; ++i) {
        std::array<float, 3> size{};
        for (float& value : size) value = (float)rangedValue(s, 16000) / 100.0f;
        sizes.push_back(size);
    }
    if (d) {
        d->hasExplosion = true;
        d->explosion.shape = std::move(shape);
        d->explosion.faceViewer = faceViewer;
        d->explosion.scale = scale;
        d->explosion.playSpeed = playSpeed;
        d->explosion.times = std::move(times);
        d->explosion.sizes = std::move(sizes);
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
    // mass (default 1), drag, density, maxEnergy, camera distances, ...
    const float mass = s.readFlag() ? s.readF32() : 1.0f;
    if (activeDecoded) activeDecoded->shapeMass = mass;
    // drag, density, maxEnergy, cameraMaxDist, cameraMinDist,
    // cameraDefaultFov, cameraMinFov, cameraMaxFov.
    for (int i = 0; i < 8; ++i) {
        if (!s.readFlag()) continue;
        const float value = s.readF32();
        if (activeDecoded && i == 0) activeDecoded->shapeDrag = value;
        if (activeDecoded && i == 1) activeDecoded->shapeDensity = value;
        if (activeDecoded && i == 2) activeDecoded->shapeMaxEnergy = value;
        if (activeDecoded && i == 3) activeDecoded->cameraMaxDist = value;
        if (activeDecoded && i == 4) activeDecoded->cameraMinDist = value;
    }
    const std::string debrisShape = s.readHuffmanString();
    if (activeDecoded) activeDecoded->debrisShape = debrisShape;
    if (s.readFlag()) { s.readUnsigned(10); u32s(s, 1); }
    if (s.readFlag()) s.readF32();
    if (activeDecoded) activeDecoded->cloakTexture = s.readString();
    else s.readString();
    s.readString();
    // canControl, canObserve, observeThroughObject, emap, isInvincible,
    // renderWhenDestroyed.
    for (int i = 0; i < 6; ++i) {
        const bool flag = s.readFlag();
        if (i == 3 && activeDecoded) activeDecoded->shapeEmap = flag;
    }
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
void grenade(Stream& s) {
    projectile(s);
    // armingDelayMS, muzzleVelocity, grenadeElasticity, grenadeFriction,
    // drag, density, gravityMod, lifetimeMS
    const int32_t armingDelay = (int32_t)s.readUnsigned(32);
    s.readF32();
    const float elasticity = s.readF32(), friction = s.readF32();
    s.readF32(); s.readF32();
    const float gravityMod = s.readF32();
    const int32_t lifetime = (int32_t)s.readUnsigned(32);
    if (activeDecoded) {
        activeDecoded->grenadeArmingDelayMS = armingDelay;
        activeDecoded->grenadeElasticity = elasticity;
        activeDecoded->grenadeFriction = friction;
        activeDecoded->grenadeGravityMod = gravityMod;
        activeDecoded->projectileLifetimeMS = lifetime;
    }
}
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
    const bool cloakable = s.readFlag();
    if (activeDecoded) activeDecoded->imageCloakable = cloakable;
    const uint32_t lightType = s.readRange(0, 3);
    if (lightType != 0) {
        const float radius = s.readF32();
        const int32_t time = s.readSigned(32);
        float color[4];
        for (float& value : color) value = s.readFloat(7);
        if (activeDecoded) {
            activeDecoded->shapeLightType = (int32_t)lightType;
            activeDecoded->shapeLightRadius = radius;
            activeDecoded->shapeLightTimeMS = time;
            activeDecoded->shapeLightColor = {color[0], color[1], color[2]};
        }
    }
    f32s(s, 3); s.readF32(); s.readF32(); refs(s, 1); s.readFlag();
    std::vector<WeaponImage::StateData> states(31);
    bool anyState = false;
    for (int i=0;i<31;++i) {
        const bool hasState = s.readFlag();
        if (!hasState) continue;
        WeaponImage::StateData& st = states[i];
        st.valid = anyState = true;
        s.readString(); // name
        // Engine packing order; 1-based state indices, 0 = no transition.
        int* transitions[11] = {
            &st.transitionOnNotLoaded, &st.transitionOnLoaded, &st.transitionOnNoAmmo,
            &st.transitionOnAmmo, &st.transitionOnNoTarget, &st.transitionOnTarget,
            &st.transitionOnNotWet, &st.transitionOnWet, &st.transitionOnTriggerUp,
            &st.transitionOnTriggerDown, &st.transitionOnTimeout};
        for (int* transition : transitions) *transition = (int)s.readUnsigned(5) - 1;
        if (s.readFlag()) st.timeoutValue = s.readF32();
        st.waitForTimeout = s.readFlag();
        st.fire = s.readFlag();
        s.readFlag(); // ejectShell
        st.scaleAnimation = s.readFlag();
        st.direction = s.readFlag();
        s.readFlag(); // reload
        if (s.readFlag()) s.readF32(); // energyDrain
        s.readUnsigned(3); // loaded
        st.spin = (int)s.readUnsigned(3);
        s.readUnsigned(3); // recoil
        if (s.readFlag()) st.sequence = s.readSignedInt(16);
        if (s.readFlag()) st.sequenceVis = s.readSignedInt(16);
        st.flashSequence = s.readFlag();
        s.readFlag(); // ignoreLoadedForReady
        const bool hasEmitter = s.readFlag();
        if (hasEmitter) { s.readUnsigned(11); s.readF32(); s.readSigned(32); }
        if (s.readFlag()) st.sound = (int)s.readUnsigned(11);
    }
    if (activeDecoded && anyState) activeDecoded->imageStates = std::move(states);
}

void player(Stream& s) {
    // PlayerData::unpackData (retail layout, as the reference parser decodes it).
    shapeBase(s);
    PlayerPrediction::Data d;
    d.renderFirstPerson = s.readFlag();
    d.minLookAngle = s.readF32();
    d.maxLookAngle = s.readF32();
    d.maxFreelookAngle = s.readF32();
    s.readF32(); // maxTimeScale
    d.maxStepHeight = s.readF32();
    d.jetForce = s.readF32();
    d.underwaterJetForce = s.readF32();
    d.underwaterVertJetFactor = s.readF32();
    d.jetEnergyDrain = s.readF32();
    d.underwaterJetEnergyDrain = s.readF32();
    d.minJetEnergy = s.readF32();
    d.maxJetForwardSpeed = s.readF32();
    d.maxJetHorizontalPercentage = s.readF32();
    const uint32_t jetEmitter = optionalRef(s);
    refs(s, 1); // jetEffect
    d.runForce = s.readF32();
    d.runEnergyDrain = s.readF32();
    d.minRunEnergy = s.readF32();
    d.maxForwardSpeed = s.readF32();
    d.maxBackwardSpeed = s.readF32();
    d.maxSideSpeed = s.readF32();
    d.maxUnderwaterForwardSpeed = s.readF32();
    d.maxUnderwaterBackwardSpeed = s.readF32();
    d.maxUnderwaterSideSpeed = s.readF32();
    d.runSurfaceAngle = s.readF32();
    d.recoverDelay = s.readF32();
    d.recoverRunForceScale = s.readF32();
    d.jumpForce = s.readF32();
    d.jumpEnergyDrain = s.readF32();
    d.minJumpEnergy = s.readF32();
    d.minJumpSpeed = s.readF32();
    d.maxJumpSpeed = s.readF32();
    d.jumpSurfaceAngle = s.readF32();
    d.jumpDelay = (int)s.readUnsigned(7);
    d.horizMaxSpeed = s.readF32();
    d.horizResistSpeed = s.readF32();
    d.horizResistFactor = s.readF32();
    d.upMaxSpeed = s.readF32();
    d.upResistSpeed = s.readF32();
    d.upResistFactor = s.readF32();
    f32s(s, 9); // splash and bubble tuning
    d.minImpactSpeed = s.readF32();
    // PlayerData::Sounds (MaxSounds 32); jetSound is first, wetJetSound second.
    std::vector<uint32_t> playerSounds(32);
    for (auto& sound : playerSounds) sound = optionalRef(s);
    const float boxX = s.readF32(), boxY = s.readF32(), boxZ = s.readF32();
    d.boxSize = {boxX, boxY, boxZ};
    const uint32_t footPuffEmitter = optionalRef(s);
    const int32_t footPuffNumParts = s.readSigned(32);
    const float footPuffRadius = s.readF32();
    const uint32_t decalData = optionalRef(s);
    const float decalOffset = s.readF32();
    const uint32_t dustEmitter = optionalRef(s);
    refs(s, 1); // splash
    refs(s, 3); // splash emitters
    f32s(s, 11); // heat rates, ground impact shake
    if (activeDecoded) {
        d.mass = activeDecoded->shapeMass;
        d.drag = activeDecoded->shapeDrag;
        d.density = activeDecoded->shapeDensity;
        d.maxEnergy = activeDecoded->shapeMaxEnergy;
        activeDecoded->playerPhysics = d;
        activeDecoded->playerMinLookAngle = d.minLookAngle;
        activeDecoded->playerMaxLookAngle = d.maxLookAngle;
        activeDecoded->playerMaxFreelookAngle = d.maxFreelookAngle;
        activeDecoded->playerRunSurfaceAngle = d.runSurfaceAngle;
        activeDecoded->playerJetEmitterRef = jetEmitter;
        activeDecoded->isPlayerData = true;
        activeDecoded->playerSounds = std::move(playerSounds);
        activeDecoded->playerBoxSize[0] = boxX;
        activeDecoded->playerBoxSize[1] = boxY;
        activeDecoded->playerBoxSize[2] = boxZ;
        activeDecoded->playerFootPuffEmitter = footPuffEmitter;
        activeDecoded->playerFootPuffNumParts = footPuffNumParts;
        activeDecoded->playerFootPuffRadius = footPuffRadius;
        activeDecoded->playerDecalData = decalData;
        activeDecoded->playerDecalOffset = decalOffset;
        activeDecoded->playerDustEmitter = dustEmitter;
    }
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
     case 9: (classId >= 128 ? legacyDecal : decal)(s); break; case 10: { // ELFProjectileData
        projectile(s);
        float f[6];
        for (float& v : f) v = s.readF32(); // beamRange, mainBeam width/speed/repeat, lightning width/dist
        std::vector<std::string> textures = materialStrings(s, 3); // beam, lightning, flare
        refs(s, 1); // emitter
        if (activeDecoded) {
            auto& link = activeDecoded->linkBeam;
            link.valid = true; link.elf = true;
            link.beamRange = f[0]; link.width = f[1] * 2.0f; link.scrollSpeed = f[2];
            link.texRepeat = f[3]; link.lightningWidth = f[4]; link.lightningDist = f[5];
            link.texture = textures[0]; link.lightningTexture = textures[1]; link.flareTexture = textures[2];
        }
        break;
    }
     case 11: f32s(s,3); s.readUnsigned(8); strings(s,1); break; case 12: {
         grenade(s); float values[7]; for (float& value : values) value = s.readF32();
         const auto textures = materialStrings(s, 2);
         if (activeDecoded) {
             activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::Cross;
             activeDecoded->projectileCrossViewAngle = values[0]; activeDecoded->projectileCrossSize = values[1];
             // EnergyProjectileData: blurLifetime, blurWidth, blurColor rgb.
             activeDecoded->projectileIsEnergyBolt = true;
             activeDecoded->projectileRenderCross = true;
             activeDecoded->projectileBlurLifetime = values[2]; activeDecoded->projectileBlurWidth = values[3];
             activeDecoded->projectileBlurColor = {values[4], values[5], values[6]};
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
    case 16: { // FlyingVehicleData
        vehicle(s);
        const uint32_t jetSound = optionalRef(s);
        refs(s, 1); // jetDeactivateSound
        std::vector<uint32_t> jetEmitters(4); // forward, backward, down, trail
        for (auto& ref : jetEmitters) ref = optionalRef(s);
        const float maneuveringForce = s.readF32();
        f32s(s, 12);
        const float minTrailSpeed = s.readF32();
        f32s(s, 2);
        if (activeDecoded) {
            activeDecoded->isFlyingVehicleData = true;
            activeDecoded->vehicleJetSound = jetSound;
            activeDecoded->vehicleJetEmitters = std::move(jetEmitters);
            activeDecoded->vehicleManeuveringForce = maneuveringForce;
            activeDecoded->vehicleMinTrailSpeed = minTrailSpeed;
        }
        break;
    } case 17: { // ForceFieldBareData::packData
        const int32_t fadeMS = s.readSigned(32);
        const float baseTranslucency = s.readF32(), powerOffTranslucency = s.readF32();
        s.readFlag(); s.readFlag(); // team / other permeable
        auto color = [&]() { // Stream::write(ColorF) sends a ColorI
            const uint32_t c = s.readUnsigned(32);
            return std::array<float, 4>{(c & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
                                        ((c >> 16) & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f};
        };
        const auto fieldColor = color(), powerOffColor = color();
        const uint32_t framesPerSec = s.readUnsigned(32), numFrames = s.readUnsigned(32);
        const float scrollSpeed = s.readF32(), umapping = s.readF32(), vmapping = s.readF32();
        std::vector<std::string> textures;
        for (int i = 0; i < 5; ++i) textures.push_back(s.readString());
        if (activeDecoded) {
            activeDecoded->hasForceField = true;
            activeDecoded->forceFieldFadeMS = fadeMS;
            activeDecoded->forceFieldBaseTranslucency = baseTranslucency;
            activeDecoded->forceFieldPowerOffTranslucency = powerOffTranslucency;
            activeDecoded->forceFieldColor = fieldColor;
            activeDecoded->forceFieldPowerOffColor = powerOffColor;
            activeDecoded->forceFieldFramesPerSec = framesPerSec;
            activeDecoded->forceFieldNumFrames = numFrames;
            activeDecoded->forceFieldScrollSpeed = scrollSpeed;
            activeDecoded->forceFieldUMapping = umapping;
            activeDecoded->forceFieldVMapping = vmapping;
            activeDecoded->forceFieldTextures = std::move(textures);
        }
        break;
    }
    case 18: break; case 19: grenade(s); break; case 20: { // HoverVehicleData
        vehicle(s); f32s(s,17); f32s(s,3); f32s(s,2);
        refs(s, 3); // floatSound, thrustSound, turboSound
        std::vector<uint32_t> jetEmitters(3);
        for (auto& ref : jetEmitters) ref = optionalRef(s);
        refs(s, 1); // dustTrailEmitter
        f32s(s, 3);
        if (activeDecoded) {
            activeDecoded->isHoverVehicleData = true;
            activeDecoded->vehicleJetEmitters = std::move(jetEmitters);
        }
        break;
    }
    case 21: { // ItemData
        shapeBase(s);
        const float friction = s.readFloat(10), elasticity = s.readFloat(10);
        const bool sticky = s.readFlag();
        const float gravityMod = s.readFlag() ? s.readFloat(10) : 1.0f;
        const float maxVelocity = s.readFlag() ? s.readF32() : -1.0f;
        if (activeDecoded) {
            activeDecoded->itemFriction = friction;
            activeDecoded->itemElasticity = elasticity;
            activeDecoded->itemSticky = sticky;
            activeDecoded->itemGravityMod = gravityMod;
            activeDecoded->itemMaxVelocity = maxVelocity;
        }
        if (s.readFlag()) { // ItemData light
            const int32_t type = (int32_t)s.readUnsigned(2);
            float color[4];
            for (float& value : color) value = s.readFloat(7);
            const int32_t time = s.readSigned(32);
            const float radius = s.readF32();
            const bool onlyStatic = s.readFlag();
            if (activeDecoded) {
                activeDecoded->shapeLightType = type;
                activeDecoded->shapeLightColor = {color[0], color[1], color[2]};
                activeDecoded->shapeLightTimeMS = time;
                activeDecoded->shapeLightRadius = radius;
                activeDecoded->shapeLightOnlyStatic = onlyStatic;
            }
        }
        break;
    }
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
     case 30: player(s); break; case 31: { // PrecipitationData
        refs(s,1); // sound profile
        const int32_t type = (int32_t)s.readUnsigned(32);
        const float maxSize = s.readF32();
        const std::string materialList = s.readString();
        const float sizeX = s.readF32(), sizeY = s.readF32();
        f32s(s,11); // box tuning
        if (activeDecoded) {
            activeDecoded->hasPrecipitation = true;
            activeDecoded->precipitationType = type;
            activeDecoded->precipitationMaxSize = maxSize;
            activeDecoded->precipitationMaterialList = materialList;
            activeDecoded->precipitationSizeX = sizeX;
            activeDecoded->precipitationSizeY = sizeY;
        }
        break;
    } case 32: projectile(s);break; case 33: { // RepairProjectileData
        projectile(s);
        const float beamRange = s.readF32();
        s.readF32(); // beamWidth
        s.readUnsigned(32); // numSegments
        const float beamSpeed = s.readF32(), texRepeat = s.readF32();
        f32s(s, 2); // blurFreq, blurLifetime
        const float cutoffAngle = s.readF32();
        std::vector<std::string> textures = materialStrings(s, 2); // beam, flare
        if (activeDecoded) {
            auto& link = activeDecoded->linkBeam;
            link.valid = true; link.elf = false;
            link.beamRange = beamRange; link.width = 0.2f; link.scrollSpeed = beamSpeed;
            link.texRepeat = texRepeat; link.cutoffAngle = cutoffAngle;
            link.texture = textures[0]; link.flareTexture = textures[1];
        }
        break;
    }
     case 34: s.readF32();colors(s,1);f32s(s,2);strings(s,1);f32s(s,6);if(s.readFlag())strings(s,1);break; case 35: { // SeekerProjectileData
        projectile(s); f32s(s,10);
        const bool useFlechette = s.readUnsigned(8) != 0;
        f32s(s,2);
        const int32_t flechetteDelay = (int32_t)s.readUnsigned(32);
        s.readUnsigned(32); strings(s,2); refs(s,3);
        if (activeDecoded) {
            activeDecoded->seekerUseFlechette = useFlechette;
            activeDecoded->seekerFlechetteDelayMS = flechetteDelay;
        }
        break;
    } case 36: break; case 37: shapeBase(s);break; case 38: shapeImage(s);break;
      case 39: { // ShockLanceProjectileData
        projectile(s);
        float f[7];
        for (float& v : f) v = s.readF32(); // zapDuration, boltLength, numParts, lightningFreq/Density/Amp/Width
        refs(s, 1); // shockwave
        float widths[2][4];
        for (auto& part : widths) for (float& v : part) v = s.readF32(); // startWidth, endWidth, boltSpeed, texWrap
        std::vector<std::string> textures = materialStrings(s, 4);
        refs(s, 1); // emitter
        if (activeDecoded) {
            auto& lance = activeDecoded->shockLance;
            lance.valid = true;
            lance.zapDuration = f[0];
            lance.lightningFreq = f[3]; lance.lightningDensity = f[4];
            lance.lightningAmp = f[5]; lance.lightningWidth = f[6];
            for (int i = 0; i < 2; ++i) {
                lance.startWidth[i] = widths[i][0]; lance.endWidth[i] = widths[i][1];
                lance.boltSpeed[i] = widths[i][2]; lance.texWrap[i] = widths[i][3];
            }
            lance.textures = std::move(textures);
        }
        break;
    } case 40: shockwave(s); break; case 41: break; case 42: { // SniperProjectileData
        projectile(s);
        f32s(s, 2); // maxRifleRange, rifleHeadMultiplier
        const uint32_t color[4] = {s.readUnsigned(8), s.readUnsigned(8), s.readUnsigned(8), s.readUnsigned(8)};
        const float fadeTime = s.readF32(), startWidth = s.readF32(), endWidth = s.readF32();
        f32s(s, 4); // pulseBeamWidth, beamFlareAngle, min/maxFlareSize
        const float pulseSpeed = s.readF32(), pulseLength = s.readF32();
        const uint32_t lightColor = s.readUnsigned(32);
        const float lightRadius = s.readF32();
        std::vector<std::string> textures = materialStrings(s, 12);
        if (activeDecoded) {
            auto& beam = activeDecoded->sniperBeam;
            beam.valid = true;
            for (int i = 0; i < 4; ++i) beam.color[i] = color[i] / 255.0f;
            beam.fadeTime = fadeTime;
            beam.startWidth = startWidth;
            beam.endWidth = endWidth;
            beam.pulseSpeed = pulseSpeed;
            beam.pulseLength = pulseLength;
            beam.textures = std::move(textures);
            // The sniper's own lightColor/lightRadius replace the base
            // ProjectileData light.
            activeDecoded->projectileHasLight = lightRadius > 0.0f;
            activeDecoded->projectileLightRadius = lightRadius;
            activeDecoded->projectileLightColor = {(lightColor & 0xff) / 255.0f,
                ((lightColor >> 8) & 0xff) / 255.0f, ((lightColor >> 16) & 0xff) / 255.0f};
        }
        break;
    } case 43: splash(s); break;
       case 44: shapeBase(s);s.readFlag();s.readUnsigned(32);break; case 45: f32s(s,10);strings(s,4);break; case 46: f32s(s,11);colors(s,1);f32s(s,6);strings(s,11);break; case 47: {
               // TSShapeConstructor: base shape plus "file.dsq alias" entries.
               std::string shape = s.readString();
               const int count = (int)s.readUnsigned(7);
               std::vector<std::string> sequences = materialStrings(s, count);
               if (activeDecoded) {
                   activeDecoded->constructorShape = std::move(shape);
                   activeDecoded->constructorSequences = std::move(sequences);
               }
               break;
           } case 48: projectile(s);s.readF32();colors(s,1);f32s(s,7);strings(s,4);break; case 49: {
           linear(s); float values[3]; for (float& value : values) value = s.readF32();
           const bool tracerAlpha = s.readUnsigned(8) != 0; const auto color = s.readUnsigned(32);
           float cross[2]; for (float& value : cross) value = s.readF32();
           const bool renderCross = s.readUnsigned(8) != 0; const auto textures = materialStrings(s, 2);
           if (activeDecoded) {
               activeDecoded->projectileMaterial = DecodedDataBlock::ProjectileMaterial::Cross;
               // Stream order: tracerLength, tracerWidth, tracerMinPixels.
               activeDecoded->projectileTracerLength = values[0]; activeDecoded->projectileTracerWidth = values[1];
               activeDecoded->projectileTracerMinPixels = values[2]; activeDecoded->projectileTracerAlpha = tracerAlpha;
               activeDecoded->projectileCrossViewAngle = cross[0]; activeDecoded->projectileCrossSize = cross[1];
               activeDecoded->projectileRenderCross = renderCross; activeDecoded->projectileMaterialTextures = textures;
               activeDecoded->projectileMaterialColor = {((color >> 0) & 0xff) / 255.0f,
                   ((color >> 8) & 0xff) / 255.0f, ((color >> 16) & 0xff) / 255.0f,
                   ((color >> 24) & 0xff) / 255.0f};
           }
           break;
       } case 50:s.readUnsigned(32);break;
       case 51: { // TurretData: StaticShapeData, thetaMin/Max/Null, ...
           shapeBase(s); s.readFlag(); s.readUnsigned(32);
           const float thetaMin = s.readF32(), thetaMax = s.readF32();
           s.readF32(); // thetaNull
           s.readFlag(); ranged(s,3,1); f32s(s,2);
           if (activeDecoded) {
               activeDecoded->hasTurretTheta = true;
               activeDecoded->turretThetaMin = std::clamp(thetaMin, 0.0f, 90.0f);
               activeDecoded->turretThetaMax = std::clamp(thetaMax, 90.0f, 180.0f);
           }
           break;
       }
      case 52:shapeImage(s);s.readUnsigned(8);s.readUnsigned(8);ranged(s,1080,2);s.readFlag();s.readF32();s.readFlag();break; case 53:vehicle(s);f32s(s,9);refs(s,5);f32s(s,11);break;
    }
    const bool ok = !s.failed();
    activeDecoded = nullptr;
    return ok;
}
}
