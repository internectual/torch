#pragma once

#include "net/v12_bitstream.h"

#include <cstddef>
#include <array>
#include <string>
#include <vector>

namespace V12 {

struct DecodedDataBlock {
    // AudioProfile data is sent as a path without the .wav suffix.
    std::string audioFilename;
    uint32_t audioDescriptionRef = 0;
    uint32_t audioEnvironmentRef = 0;
    float audioVolume = 1.0f;
    float audioMinDistance = 1.0f;
    float audioMaxDistance = 100.0f;
    bool audioLooping = false;
    int32_t audioLoopCount = -1;
    int32_t audioMinLoopGapMs = 0;
    int32_t audioMaxLoopGapMs = 0;
    bool audioIs3D = false;
    struct ParticleKey {
        float red{}, green{}, blue{}, alpha{};
        float size{}, time{};
    };
    struct ParticleData {
        float dragCoefficient{}, windCoefficient{}, gravityCoefficient{};
        float inheritedVelFactor{}, constantAcceleration{};
        uint32_t lifetimeMS{}, lifetimeVarianceMS{};
        float spinSpeed{}, spinRandomMin{}, spinRandomMax{};
        bool useInvAlpha{};
        std::vector<ParticleKey> keys;
        std::vector<std::string> textures;
    };
    struct ParticleEmitterData {
        uint32_t ejectionPeriodMS{}, periodVariance{};
        uint32_t ejectionVelocity{}, velocityVariance{};
        uint32_t ejectionOffset{};
        uint32_t thetaMin{}, thetaMax{}, phiReferenceVel{}, phiVariance{};
        bool overrideAdvances{}, orientParticles{}, orientOnVelocity{};
        bool useEmitterSizes{}, useEmitterColors{};
        // These are supplied by runtime effect users in Torque. Network
        // datablocks do not carry them, so an empty array falls back to the
        // referenced ParticleData keyframes.
        std::vector<float> sizes;
        std::vector<ParticleKey> colors;
        uint32_t lifetimeMS{}, lifetimeVarianceMS{};
        std::vector<uint32_t> particleRefs;
    };
    struct ExplosionData {
        uint32_t soundProfileRef = 0;
        uint32_t particleEmitterRef = 0;
        int32_t particleDensity{};
        float particleRadius{};
        int32_t delayMS{}, delayVarianceMS{};
        int32_t lifetimeMS{}, lifetimeVarianceMS{};
        float offset{};
        int32_t debrisNum{}, debrisNumVariance{};
        float debrisVelocity{}, debrisVelocityVariance{};
        int32_t debrisThetaMin{}, debrisThetaMax{}, debrisPhiMin{}, debrisPhiMax{};
        uint32_t debrisRef = 0;
        std::vector<uint32_t> emitterRefs;
        uint32_t shockwaveRef = 0;
        std::vector<uint32_t> subExplosionRefs;
        bool hasLight{};
        bool shakeCamera{};
        std::array<float, 3> shakeFrequency{};
        std::array<float, 3> shakeAmplitude{};
        float shakeDuration{};
        float shakeRadius{};
        float shakeFalloff{};
    };
    struct DebrisData {
        std::string shape;
        int32_t lifetimeMS = 3000;
        int32_t lifetimeVarianceMS{};
        float velocity{};
        float velocityVariance{};
        float minSpin{}, maxSpin{};
        float elasticity = 0.35f;
        float friction = 0.5f;
        int32_t numBounces{};
        int32_t bounceVariance{};
        bool explodeOnMaxBounce{};
        bool staticOnMaxBounce{};
        bool snapOnMaxBounce{};
        float gravModifier = 1.0f;
        float terminalVelocity{};
    };
    struct DecalData {
        std::string texture;
        float sizeX = 1.0f;
        float sizeY = 1.0f;
        int32_t lifetimeMS{};
        int32_t fadeTimeMS{};
        uint32_t textureRows = 1;
        uint32_t textureCols = 1;
        bool randomize{};
        bool renderPriority{};
    };
    struct ShockwaveData {
        int32_t delayMS{}, delayVariance{}, lifetimeMS{}, lifetimeVariance{};
        float width{}, height{}, velocity{}, verticalCurve{}, acceleration{}, texWrap{};
        int32_t numSegments{}, numVertSegments{};
        bool is2D{}, orientToNormal{}, mapToTerrain{}, renderBottom{}, renderSquare{};
        std::vector<uint32_t> emitterRefs;
        std::vector<uint32_t> colors;
        std::vector<float> times;
        std::vector<std::string> textures;
    };
    struct SplashData {
        int32_t delayMS{}, delayVarianceMS{}, lifetimeMS{}, lifetimeVarianceMS{};
        V12Vec3 scale{1.0f, 1.0f, 1.0f};
        float width{}, height{}, velocity{}, acceleration{};
        uint32_t numSegments{};
        float texWrap{}, texFactor{}, ejectionFreq{}, ejectionAngle{};
        float ringLifetime{}, startRadius{};
        uint32_t explosionRef{};
        std::vector<uint32_t> emitterRefs, colors;
        std::vector<float> times;
        std::vector<std::string> textures;
    };

    std::string shapeFile;
    int32_t emitterDelayMS = -1;
    float bubbleEmitTime = 0.5f;
    bool faceViewer{};
    V12Vec3 projectileScale{1.0f, 1.0f, 1.0f};
    bool hasProjectileScale{};
    uint32_t mountPoint = 0;
    bool hasMountPoint{};
    std::string debrisShape;
    std::string cloakTexture;
    uint32_t projectileDelayEmitterRef = 0;
    uint32_t projectileBubbleEmitterRef = 0;
    uint32_t projectileExplosionRef = 0;
    uint32_t projectileUnderwaterExplosionRef = 0;
    uint32_t projectileSplashRef = 0;
    uint32_t projectileSoundRef = 0;
    uint32_t projectileWetFireSoundRef = 0;
    uint32_t projectileFireSoundRef = 0;
    uint32_t projectileBaseEmitterRef = 0;
    std::vector<uint32_t> projectileDecalRefs;
    bool projectileHasLight{};
    float projectileLightRadius = 1.0f;
    std::array<float, 3> projectileLightColor{1.0f, 1.0f, 1.0f};
    bool projectileHasUnderwaterLightColor{};
    std::array<float, 3> projectileUnderwaterLightColor{1.0f, 1.0f, 1.0f};
    bool projectileExplodeOnWaterImpact{};
    float projectileDepthTolerance = 5.0f;
    float projectileDryVelocity = 5.0f;
    float projectileWetVelocity = 5.0f;
    int32_t projectileLifetimeMS = 1000;
    int32_t projectileFizzleTimeMS = 1000;
    bool projectileExplodeOnDeath{};
    enum class ProjectileMaterial : uint8_t { None, Cross, Flare, LinearFlare };
    ProjectileMaterial projectileMaterial = ProjectileMaterial::None;
    std::vector<std::string> projectileMaterialTextures;
    std::array<float, 3> projectileMaterialSizes{1.0f, 1.0f, 1.0f};
    std::array<float, 4> projectileMaterialColor{1.0f, 1.0f, 1.0f, 1.0f};
    float projectileCrossViewAngle = 0.0f;
    float projectileCrossSize = 1.0f;
    float projectileTracerLength = 0.0f;
    float projectileTracerMinPixels = 0.0f;
    float projectileTracerWidth = 0.0f;
    uint32_t projectileFlareCount = 0;
    bool projectileTracerAlpha{};
    bool projectileRenderCross{};
    bool projectileUseLensFlare{};
    ParticleData particle;
    ParticleEmitterData emitter;
    ExplosionData explosion;
    DebrisData debris;
    DecalData decal;
    std::vector<uint32_t> effectRefs;
    ShockwaveData shockwave;
    SplashData splash;
    bool hasParticle{}, hasEmitter{}, hasExplosion{}, hasShockwave{}, hasSplash{}, hasDecal{};
};

// Consume one Tribes 2 build-25034 SimDataBlock payload. classId may be
// either the registry index or the wire class id (128 + registry index).
// When supplied, decoded native asset references are returned in `decoded`.
bool readDataBlockPayload(V12BitStream& stream, size_t classId,
                          DecodedDataBlock* decoded = nullptr);

} // namespace V12
