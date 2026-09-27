#pragma once
#include "game/player_prediction.h"
#include "game/weapon_image_state.h"

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
        std::string shape; // dtsFileName
        bool faceViewer = false;
        std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
        float playSpeed = 1.0f;
        // Size keyframes over the explosion lifetime (normalized times).
        std::vector<float> times;
        std::vector<std::array<float, 3>> sizes;
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
    // ShapeBaseImageData states, indexed by the engine's 31 state slots.
    std::vector<WeaponImage::StateData> imageStates;
    // TSShapeConstructor datablock.
    std::string constructorShape;
    std::vector<std::string> constructorSequences;
    // PlayerData fields used by client-side animation.
    bool isPlayerData = false;
    float playerMinLookAngle = 0.0f, playerMaxLookAngle = 0.0f, playerMaxFreelookAngle = 0.0f;
    // PlayerData fields the client Player simulation uses.
    PlayerPrediction::Data playerPhysics;
    // ShapeBaseData drag/density/maxEnergy (flag-gated; engine defaults).
    float shapeDrag = 0.0f, shapeDensity = 1.0f, shapeMaxEnergy = 0.0f;
    float playerRunSurfaceAngle = 0.0f;
    float playerBoxSize[3] = {0.0f, 0.0f, 0.0f}; // Torque x, y, z
    uint32_t playerJetEmitterRef = 0; // ParticleEmitterData at jetNozzle0/1
    std::vector<uint32_t> playerSounds; // PlayerData::Sounds AudioProfile refs
    float shapeMass = 1.0f;             // ShapeBaseData::mass
    bool imageCloakable = false;        // ShapeBaseImageData::cloakable
    // SniperProjectileData beam (SniperProjectile::renderObject).
    struct SniperBeam {
        bool valid = false;
        float color[4] = {1, 1, 1, 1};
        float fadeTime = 0, startWidth = 0, endWidth = 0;
        float pulseSpeed = 0, pulseLength = 0;
        std::vector<std::string> textures; // [0] unused by the beam, [1..11]
    } sniperBeam;
    // ELFProjectileData / RepairProjectileData link beams.
    struct LinkBeam {
        bool valid = false, elf = false;
        float beamRange = 0, width = 0, scrollSpeed = 0, texRepeat = 0;
        float lightningWidth = 0, lightningDist = 0, cutoffAngle = 40;
        std::string texture, lightningTexture, flareTexture;
    } linkBeam;
    // ShockLanceProjectileData bolt.
    struct ShockLance {
        bool valid = false;
        float zapDuration = 0, lightningFreq = 0, lightningDensity = 0, lightningAmp = 0, lightningWidth = 0;
        float startWidth[2]{}, endWidth[2]{}, boltSpeed[2]{}, texWrap[2]{};
        std::vector<std::string> textures;
    } shockLance;
    // FlyingVehicleData / HoverVehicleData jets (forward, backward, down;
    // flying adds the trail emitter fourth).
    bool isFlyingVehicleData = false, isHoverVehicleData = false;
    std::vector<uint32_t> vehicleJetEmitters;
    uint32_t vehicleJetSound = 0;
    float vehicleManeuveringForce = 0.0f;
    float vehicleMinTrailSpeed = 0.0f;
    int32_t emitterDelayMS = -1;
    float bubbleEmitTime = 0.5f;
    bool faceViewer{};
    V12Vec3 projectileScale{1.0f, 1.0f, 1.0f};
    bool hasProjectileScale{};
    uint32_t mountPoint = 0;
    bool hasMountPoint{};
    std::string debrisShape;
    std::string cloakTexture;
    bool shapeEmap = false;       // ShapeBaseData::emap
    // TurretData elevation limits (degrees); t2-mapper clamps and defaults
    // them to [0, 90] / [90, 180] and 45 / 135.
    bool hasTurretTheta = false;
    float turretThetaMin = 45.0f, turretThetaMax = 135.0f;
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
    // SeekerProjectile::registerLights withholds the light for
    // flechetteDelayMs >> 5 ticks when useFlechette is set.
    bool seekerUseFlechette{};
    int32_t seekerFlechetteDelayMS = 0;
    // ItemData light (Item::registerLights) and ShapeBaseImageData light:
    // type 0 none, 1 constant, 2 pulsing, 3 weapon fire (images only).
    // ForceFieldBareData.
    bool hasForceField{};
    int32_t forceFieldFadeMS = 1000;
    float forceFieldBaseTranslucency = 1.0f, forceFieldPowerOffTranslucency = 0.0f;
    std::array<float, 4> forceFieldColor{1, 1, 1, 1}, forceFieldPowerOffColor{0, 0, 0, 1};
    uint32_t forceFieldFramesPerSec = 1, forceFieldNumFrames = 1;
    float forceFieldScrollSpeed = 0.0f, forceFieldUMapping = 1.0f, forceFieldVMapping = 1.0f;
    std::vector<std::string> forceFieldTextures;
    // PrecipitationData.
    bool hasPrecipitation{};
    int32_t precipitationType = 0;
    float precipitationMaxSize = 0.0f, precipitationSizeX = 1.0f, precipitationSizeY = 1.0f;
    std::string precipitationMaterialList;
    // ShapeBaseData third-person camera distances.
    float cameraMaxDist = 0.0f, cameraMinDist = 0.2f;
    // ItemData physics (Item::updateVelocity / updatePos).
    float itemFriction = 0.0f, itemElasticity = 0.0f;
    bool itemSticky{};
    float itemGravityMod = 1.0f, itemMaxVelocity = -1.0f;
    int32_t shapeLightType = 0;
    std::array<float, 3> shapeLightColor{1.0f, 1.0f, 1.0f};
    int32_t shapeLightTimeMS = 1000;
    float shapeLightRadius = 10.0f;
    bool shapeLightOnlyStatic{};
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
    bool projectileIsEnergyBolt = false; // EnergyProjectileData
    // GrenadeProjectileData flight.
    int32_t grenadeArmingDelayMS = 0;
    float grenadeElasticity = 0.999f, grenadeFriction = 0.3f, grenadeGravityMod = 1.0f;
    float projectileBlurLifetime = 0.0f, projectileBlurWidth = 0.0f;
    std::array<float, 3> projectileBlurColor{1.0f, 1.0f, 1.0f};
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
