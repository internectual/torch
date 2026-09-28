#pragma once
#include "core/math.h"
#include "core/input_parity.h"
#include "audio/audio_system.h"
#include "render/renderer.h"
#include "game/collision.h"
#include "game/shape_lighting.h"
#include "game/weapon.h"
#include "net/protocol.h"
#include "game/demo.h"
#include "game/mission_parser.h"
#include "game/death_respawn.h"
#include "game/mission_rules.h"
#include "game/trigger.h"
#include "game/hud.h"
#include "game/hud_parity.h"
#include "game/water_parity.h"
#include <vector>
#include <array>
#include <limits>
#include <algorithm>
#include <string>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <map>

class Menu;
class Game;

struct GameConfig {
    std::string playerName = "Player";
    std::string serverHost = "localhost";
    uint16_t serverPort = T2Protocol::DEFAULT_PORT;
    bool online = false;
    bool dedicated = false;
    // Native defaults; these remain configurable for local movement profiles.
    float moveSpeed = 15.0f;
    float jumpSpeed = 10.0f;
    float jetSpeed = 35.0f;
};

class Player {
public:
    Player();
    ~Player();

    enum AnimState { Stand, Run, Jump, Jet, Death };

    void update(float dt);
    void render();

    Point3F position() const { return pos; }
    Point3F rotation() const { return rot; }
    Point3F velocity() const { return vel; }

    void setPosition(const Point3F& p) {
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) pos = p;
    }
    void setRotation(const Point3F& r) {
        if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return;
        rot = r;
        rot.x = clampCameraPitch(rot.x);
    }
    void setEnergy(float e) {
        if (std::isfinite(e)) eng = std::clamp(e, 0.0f, maxEng);
    }
    void setRepairRate(float rate) {
        if (std::isfinite(rate)) repairRate = std::clamp(rate, 0.0f, 100.0f);
    }
    void setVelocity(const Point3F& v) {
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) vel = v;
    }
    void setOnGround(bool g) { onGround = g; }

    void applyMove(const Point3F& move, bool jump, bool jet, float dt = 1.0f / 60.0f);
    void applyDamage(float amount);
    void respawn();
    void setHealth(float value) {
        if (std::isfinite(value)) hp = std::clamp(value, 0.0f, maxHp);
    }

    float health() const { return hp; }
    float maxHealth() const { return maxHp; }
    void setMaxHealth(float value) {
        if (!std::isfinite(value) || value <= 0.0f) return;
        maxHp = value;
        hp = std::clamp(hp, 0.0f, maxHp);
    }
    float energy() const { return eng; }
    float maxEnergy() const { return maxEng; }
    void setMaxEnergy(float value) {
        if (!std::isfinite(value) || value <= 0.0f) return;
        maxEng = value;
        eng = std::clamp(eng, 0.0f, maxEng);
    }
    float getRepairRate() const { return repairRate; }
    float heat() const { return heatLevel; }
    float armor() const { return arm; }
    int team() const { return teamId; }
    void setTeam(int team) { teamId = team; }
    void setHeat(float value) {
        if (std::isfinite(value)) heatLevel = std::clamp(value, 0.0f, 100.0f);
    }
    bool isDead() const { return hp <= 0; }
    bool isOnGround() const { return onGround; }
    bool jumpWasDown() const { return jumpHeld; }
    void setJumpWasDown(bool value) { jumpHeld = value; }
    float jumpDelay() const { return jumpCooldown; }
    void setJumpDelay(float value) {
        if (std::isfinite(value)) jumpCooldown = std::max(0.0f, value);
    }
    AnimState animState() const { return anim; }

    // Camera
    Point3F cameraPos() const;
    Point3F cameraTarget() const;

    // Weapons
    void selectWeapon(int32_t idx);
    void fireWeapon(bool alt);
    int32_t currentWeapon() const { return curWeapon; }
    const Weapon& weapon(int32_t idx) const { return weapons[idx]; }
    Weapon& weapon(int32_t idx) { return weapons[idx]; }
    int32_t weaponCount() const { return (int32_t)weapons.size(); }
    void weaponCycle(int32_t dir);
    void updateWeaponHud();

    // HUD state
    int32_t kills = 0;
    int32_t deaths = 0;
    float score = 0.0f;
    void recordKill() { ++kills; score += 1.0f; }

    // Animation
    void updateAnimation(float dt, bool jetting);

    // Player model
    DTSShape modelShape;
    bool modelLoaded = false;
    DTSShape weaponShape;
    bool weaponLoaded = false;

private:
    Point3F pos{0, 5, 0};
    Point3F rot{0, 0, 0};
    Point3F vel{0, 0, 0};
    float hp = 100.0f;
    float maxHp = 100.0f;
    float eng = 100.0f;
    float maxEng = 100.0f;
    float repairRate = 0.0f;
    float heatLevel = 0.0f;
    float arm = 0.0f;
    int teamId = 1;
    bool onGround = true;
    bool jumpHeld = false;
    float jumpCooldown = 0.0f;
    float eyeHeight = 1.5f;
    float radius = 0.5f;
    AnimState anim = Stand;
    float animTime = 0;

    // Weapons
    std::vector<Weapon> weapons;
    int32_t curWeapon = 0;
    float fireCooldown = 0;
    float weaponAnimTime = 0;
    bool modelLoadAttempted = false;

    void loadModel();
    void loadWeaponModel();
};

class World {
public:
    struct PhysicalZoneEffect {
        float velocityMod = 1.0f;
        float gravityMod = 1.0f;
        Point3F appliedForce{};
    };

    World();
    ~World();

    bool load(const char* mapName);
    // The world from mission objects (a parsed .mis or demo scene ghosts).
    bool loadObjects(const char* mapName, const std::string& misPath, std::vector<MisObject> objects);
    // While a demo world loads: the recording's datablocks, which objects
    // carrying a "datablockid" resolve against instead of script datablocks.
    const std::map<uint32_t, ParsedDataBlock>* sceneDataBlocks = nullptr;
    bool isLoaded() const { return loaded; }
    // Terrain-only load (no shapes/materials) — safe for headless dedicated servers
    // that only need authoritative ground heights for collision.
    bool loadTerrain(const char* mapName);
    void cleanupMission();
    void update(float dt);
    void render(const Point3F& cameraPos, float dt = 1.0f / 60.0f);
    void updateRendererLights(Renderer& renderer) const;

    TerrainBlock* terrain() { return &terrainBlock; }
    Sky* sky() { return &skyBox; }
    const Point3F& spawnPoint() const { return playerSpawn; }
    float waterLevel() const {
        // Zero is a useful no-water default, but it must not also mask a
        // legitimate WaterBlock surface below the world origin.
        float level = 0.0f;
        bool found = false;
        for (const auto& body : waterBodies) {
            if (!waterBodyCanAffectSurface(body.active, body.liquidType)) continue;
            level = found ? std::max(level, body.level) : body.level;
            found = true;
        }
        return found ? level : 0.0f;
    }
    Point3F spawnPointForTeam(int teamId) const;
    bool setWaterLevel(const std::string& target, float level);
    bool setWaterType(const std::string& target, int type);
    bool setWaterOpacity(const std::string& target, float opacity);
    bool setWaterColor(const std::string& target, const ColorF& color);
    bool setSkyColor(const ColorF& color);
    bool setSkyMaterialList(const std::string& materialList);
    bool setSunDirection(const Point3F& direction);
    // Projected-shadow receivers: terrain and interior triangles inside
    // [lo, hi] that face `lightDir` (normal . lightDir < -0.05).
    void shadowReceiversInBox(const Point3F& lo, const Point3F& hi, const Point3F& lightDir,
                              std::vector<Point3F>& out) const;
    bool setSunColor(const ColorF& color);
    bool setSunAmbient(const ColorF& color);
    bool setFogTransition(float duration, float distance, const ColorF* color = nullptr);
    bool isUnderwater(const Point3F& camera) const {
        for (const auto& body : waterBodies) {
            if (!body.active)
                continue;
            if (!waterBodyContainsHorizontal(camera.x, camera.z, body.originX,
                                             body.originZ, body.sizeX, body.sizeY))
                continue;
            // Non-water liquid types (lava/quicksand) can overlap a normal
            // WaterBlock. They must not hide the underwater filter from a
            // lower water volume merely because their surface is higher.
            if (activeWaterBodySubmergesCamera(body.active, camera.y, body.level,
                                               body.liquidType) &&
                body.liquidType <= 3)
                return true;
        }
        return false;
    }
    PhysicalZoneEffect physicalZoneEffect(const Point3F& position) const;

    // Sun lighting from mission (public for demo playback override)
    Point3F sunLightDir{0.5f, 0.8f, 0.6f};
    ColorF sunColor{1, 1, 1, 1};
    ColorF sunAmbient{0.3f, 0.3f, 0.4f, 1.0f}; // scene ambient light (from Sun.ambient)
    bool sunLightDirUsed = false;
    bool sunColorUsed = false;

    // Fog from mission
    struct FogParams {
        bool enabled = true;
        ColorF color{0.75f, 0.8f, 0.85f, 1.0f};
        float density = 0.0015f;
        float distance = 666.0f;
        bool transitioning = false;
        float transitionElapsed = 0.0f;
        float transitionDuration = 0.0f;
        float transitionStartDensity = 0.0f;
        float transitionTargetDensity = 0.0f;
        ColorF transitionStartColor{0, 0, 0, 1};
        ColorF transitionTargetColor{0, 0, 0, 1};
    };
    FogParams fog;
    float visibleDistance = 1000.0f;
    struct FogVolume {
        float visibleDistance = 0.0f;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        float percentage = 1.0f;
    };
    std::vector<FogVolume> fogVolumes;

    // Sky material list
    std::string skyMaterialList;

    // Mission area bounding box (from MissionArea object in .mis)
    struct MissionAreaParams {
        bool valid = false;
        float x, z, width, height;
    };
    MissionAreaParams missionArea;

    struct ObserverCamera {
        Point3F pos;
        Point3F axis;
        float angleDeg = 0;
    };

    // Datablock InstanceName -> shapeFile path (from .cs scripts + inline .mis datablock defs)
    std::unordered_map<std::string, std::string> datablockShapes;

    // Object management
    struct WorldObject {
        std::string className;
        bool rotate = false; // Item/ItemData rotate: one turn every 3 s
        bool emap = false;   // its datablock's ShapeBaseData::emap
        int teamId = 0;
        Point3F pos;
        Point3F rot; // raw axis-angle: (axisX, axisY, axisZ, angle) but angle stored in angleDeg
        float rotAngleDeg = 0;
        Point3F scale{1,1,1};
        std::string shapeName;
        bool visible = true;
        std::string label;
        Point3F labelAnchor;
        bool labelAnchorValid = false;
        DTSShape* shape{};
        float boundsRadius = 0.0f;
        bool translucent = false;
        std::string mountedShapeName;
        DTSShape* mountedShape{};
        std::array<std::string, 8> mountedImages{};
        bool collidable = true;
        bool forceField = false;
        std::vector<uint32_t> forceFieldFrames;
        std::vector<float> forceFieldFrameDurations;
         ColorF forceFieldColor{1, 1, 1, 1};
         ColorF forceFieldPowerOffColor{0, 0, 0, 1};
         float forceFieldBaseTranslucency = 1.0f;
         float forceFieldPowerOffTranslucency = 0.0f;
         float forceFieldFadeMS = 1000.0f;
         float forceFieldFadePosition = 0.0f;
         float forceFieldUMapping = 1.0f;
        float forceFieldVMapping = 1.0f;
        float forceFieldFramesPerSec = 1.0f;
        float forceFieldScrollSpeed = 0.0f;
        bool forceFieldOpen = false;
        // The demo ghost this object stands for, or -1.
        int ghostIndex = -1;
        int forceFieldStateUpdates = -1;
        std::string animName; // empty = static render; non-empty = play this animation
        ShapeLighting::State shapeLight; // getLightingColor probe state
        float animTime = 0;
        int zoneManager = -1;
        int interiorZone = -1;
        std::string audioFileName;
        float audioVolume = 1.0f;
        bool audioIs3D = false;
        bool audioIsLooping = false;
        float audioMinDistance = 1.0f;
        float audioMaxDistance = 100.0f;
        bool audioEmitter = false;
        bool missionVolume = false;
        float volumeRadius = 0.0f;
        float physicalVelocityMod = 1.0f;
        float physicalGravityMod = 1.0f;
        Point3F physicalForce{};
        bool physicalActive = true;
        bool itemPickup = false;
        bool itemActive = true;
        std::string objectName;
        TriggerPolyhedron trigger;
        bool triggerVolume = false;
        std::unordered_set<std::string> triggerOccupants;
        bool missionObjective = false;
        std::string objectiveMode;
        std::string objectiveTarget;
        int objectiveTargetId = -1;
        float objectiveWeight = 0.0f;
        bool objectiveOffense = false;
        bool objectiveDefense = false;
        bool objectiveActive = true;
        int objectiveState = 0;
        float objectiveScore = 0.0f;
        float objectiveWeights[4]{};
    };

    void addObject(const WorldObject& obj);
    void resetTriggerTracking();
    bool setMissionObjectEnabled(const std::string& name, bool enabled);
    bool setMissionObjectHidden(const std::string& name, bool hidden);
    bool setMissionObjectPosition(const std::string& name, const Point3F& position);
    bool getMissionObjectPosition(const std::string& name, Point3F& position) const;
    bool setMissionObjectRotation(const std::string& name, const Point3F& axis, float angleDeg);
    bool getMissionObjectRotation(const std::string& name, Point3F& axis, float& angleDeg) const;
    bool setMissionObjectScale(const std::string& name, const Point3F& scale);
    bool getMissionObjectScale(const std::string& name, Point3F& scale) const;
    bool setMissionObjectTransform(const std::string& name, const std::string& transform);
    bool getMissionObjectTransform(const std::string& name, std::string& transform) const;
    bool mountMissionObjectImage(const std::string& name, const std::string& image, int slot);
    bool unmountMissionObjectImage(const std::string& name, int slot);
    bool getMissionObjectImage(const std::string& name, int slot, std::string& image) const;
    bool deleteMissionObject(const std::string& name);
    const std::vector<WorldObject>& objects() const { return worldObjects; }
    const std::vector<AuthoredMissionObjective>& objectives() const { return missionObjectives; }
    bool setObjectiveActive(const std::string& name, bool active);
    bool setObjectiveState(const std::string& name, int state);
    bool setObjectiveTarget(const std::string& name, const std::string& target, int targetId);
    bool setObjectiveWeight(const std::string& name, int level, float weight);
    bool setObjectiveScore(const std::string& name, float score);
    bool setObjectiveTeam(const std::string& name, int team);
    const AuthoredNavigationGraph& navigationGraph() const { return navGraph; }
    const std::vector<ObserverCamera>& observerCameras() const { return cameras; }

    struct SceneState {
        Point3F cameraPosition{};
        MatrixF view;
        MatrixF projection;
        std::vector<std::vector<bool>> interiorVisibleZones;
        std::vector<uint8_t> interiorVisibilityComputed;
    };
    const SceneState& sceneState() const { return currentSceneState; }

    float getHeight(float x, float z) const;
    float getFloorHeight(float x, float y, float z) const;
    bool spawnTransformForTeam(int teamId, Point3F& position, Point3F& rotation) const;
    bool isObjectVisible(const WorldObject& obj, const Point3F& cameraPosition) const;
    bool isPositionVisible(const Point3F& torquePosition, const Point3F& cameraPosition) const;
    const CollisionMesh& collision() const { return interiorCollision; }
    std::vector<Projectile>& projectiles() { return projList; }
    void spawnProjectile(const Projectile& p);

    // Particle system (public for demo playback)
    struct Particle {
        Point3F pos;
        Point3F vel;
        float lifetime;
        float maxLifetime;
        float size;
        ColorF color;
        bool active = false;
    };
    std::vector<Particle> particles;
    void spawnExplosion(const Point3F& pos, const ColorF& color, float radius = 2.0f, int count = 20);
    void spawnExplosionEffect(const Point3F& pos,
                              const V12::DecodedDataBlock* projectileData,
                              const V12::DecodedDataBlock* explosionData,
                              const std::map<uint32_t, ParsedDataBlock>* dataBlocks = nullptr,
                              const Point3F& impactNormal = {0, 1, 0}, int tick = 0);
    void spawnSplashEffect(const Point3F& pos, const V12::DecodedDataBlock& splash,
                           const std::map<uint32_t, ParsedDataBlock>& dataBlocks);
    void spawnTrail(const Point3F& pos, const ColorF& color, float size = 0.2f);
    void updateParticles(float dt);
    void renderParticles();
    Point3F cameraShakeOffset(const Point3F& cameraPosition) const;
    void clearEffects();
    // A mission emitter (ParticleEmissionDummy) at `pos` along `axis` (Y-up).
    void addSceneEmitter(const Point3F& pos, const Point3F& axis,
                         const V12::DecodedDataBlock::ParticleEmitterData& emitter,
                         const V12::DecodedDataBlock::ParticleData& particle);
    // Player::updateActionThread footstep effects (Y-up): a burst of
    // `count` particles within `radius` coloured by the terrain's puff
    // colours, and a footprint decal facing `forward`.
    void addFootPuff(const Point3F& pos, uint32_t emitterRef, float radius, int count,
                     const ColorF colors[2], const std::map<uint32_t, ParsedDataBlock>& dataBlocks);
    void addFootprint(const Point3F& pos, const Point3F& normal, const Point3F& forward, uint32_t decalRef,
                      const std::map<uint32_t, ParsedDataBlock>& dataBlocks);
    // A Lightning object from mission-file fields (get returns "" if unset).
    void addSceneLightning(const std::function<std::string(const char*)>& get);
    void syncForceFieldGhost(int ghostIndex, int state, uint32_t position, int updates);
    // Player::updateWorkingCollisionSet's world polygons in a Torque-space
    // box: terrain, interiors and closed force fields, facing free space.
    void playerTrianglesInBox(const Point3F& min, const Point3F& max,
                              std::vector<PlayerPrediction::Triangle>& out) const;
    // The highest liquid surface (Torque z) over a Torque XY, or NaN.
    float waterSurfaceAt(float x, float y) const;
    void setScenePrecipitation(const std::function<std::string(const char*)>& get,
                               const std::function<std::string(const char*)>& data);
    void beginProjectileTrailSync();
    void syncProjectileTrail(int ownerId, const Point3F& pos, const Point3F& velocity,
                             const V12::DecodedDataBlock* projectileData,
                             const std::map<uint32_t, ParsedDataBlock>* dataBlocks);
    void endProjectileTrailSync();
    // Keep a ParticleEmitterData emitting at a moving node this frame.
    void syncNodeEmitter(int64_t key, uint32_t emitterRef, const Point3F& pos,
                         const Point3F& velocity, const Point3F& axis,
                         const std::map<uint32_t, ParsedDataBlock>& dataBlocks,
                         float emitScale = 1.0f);
    void removeProjectileTrail(int ownerId);

private:
    TerrainBlock terrainBlock;
    Sky skyBox;
    CollisionMesh interiorCollision;

    // Interior render triangles for the shape lighting probe: each keeps its
    // lightmap UVs so the floor colour under a roof can be sampled.
    struct LightProbeTriangle {
        Point2F uv[3];
        uint32_t lightmap = 0; // GL texture id, 0 = no lighting data
        bool outsideVisible = false;
    };
    std::vector<CollisionTri> lightProbeTris;
    std::vector<LightProbeTriangle> lightProbeInfo;
    CollisionGrid lightProbeGrid;
    struct LightmapPixels { int width = 0, height = 0; std::vector<uint8_t> rgba; };
    std::unordered_map<uint32_t, LightmapPixels> lightmapPixelCache;
    bool sampleInteriorLight(int triangle, const Point3F& point, ShapeLighting::Color& out);
public:
    // SceneObject::getLightingColor for a shape centred at `center` (Y-up
    // world). Returns false when a roof has no floor beneath it, leaving
    // the previous result standing.
    bool probeShapeLighting(const Point3F& center, int& mode, ShapeLighting::Color& color);
    // Probe if moved, slew, and store the result on the shape for its next
    // render. `renderModel` places the shape in the world.
    void applyShapeLighting(DTSShape& shape, ShapeLighting::State& state,
                            const MatrixF& renderModel, float dtMs);
private:
    std::vector<WorldObject> worldObjects;
    // SpawnSphere rotation is per team. A respawn by the opposing team must
    // not change which authored start this team receives next.
    mutable std::unordered_map<int, size_t> spawnCursors;
    std::vector<AuthoredMissionObjective> missionObjectives;
    AuthoredNavigationGraph navGraph;
    mutable SceneState currentSceneState;
    std::vector<ObserverCamera> cameras;
    std::vector<DTSShape> shapes;
    std::vector<Projectile> projList;

    // Temp explosion struct for API compatibility
    struct Explosion {
        Point3F pos;
        float lifetime;
        float maxLifetime;
        float radius;
        ColorF color;
        std::vector<size_t> damagedBots;
    };
    std::vector<Explosion> explosions;

    struct EffectParticle {
        Point3F pos{}, vel{}, acc{};
        Point3F orientDir{0, 1, 0};
        float age = 0.0f;
        float lifetime = 1.0f;
        float spin = 0.0f;
        float spinSpeed = 0.0f;
        float size = 1.0f;
        float initialAdvance = -1.0f;
        uint32_t texture = UINT32_MAX;
        size_t textureIndex = 0;
        bool additive = true;
        ColorF color{1, 1, 1, 1};
        bool active = true;
    };
    struct EffectEmitter {
        Point3F pos{};
        V12::DecodedDataBlock::ParticleEmitterData emitter;
        V12::DecodedDataBlock::ParticleData particle;
        std::vector<EffectParticle> particles;
        float age = 0.0f;
        float delay = 0.0f;
        float nextEmission = 0.0f;
        float lifetime = 0.0f;
        int burstCount = 0;
        uint32_t texture = UINT32_MAX;
        bool burst = false;
        int ownerId = -1;
        bool projectileTrail = false;
        Point3F ownerVelocity{};
        Point3F axis{0, 1, 0};
        uint64_t trailGeneration = 0;
        // Node emitters (jet nozzles) keyed by owner; once no longer synced
        // they stop emitting and are removed when their particles are gone.
        bool nodeEmitter = false;
        int64_t nodeKey = -1;
        bool stopped = false;
        float emitScale = 1.0f; // node emitters: share of the frame they emit over
        // ParticleEmitter::emitParticles(centre, normal, radius, ..., count):
        // a burst placed in a half box of `radialRadius` about the normal,
        // each particle ejected along its own offset. Negative: not radial.
        float radialRadius = -1.0f;
        Point3F radialNormal{0, 1, 0};
        std::vector<uint32_t> textures;
        std::vector<float> textureDurations;
    };
    std::vector<EffectEmitter> effectEmitters;
    uint64_t trailGeneration = 0;
    struct EffectShockwave {
        Point3F pos{};
        V12::DecodedDataBlock::ShockwaveData data;
        float age = 0.0f;
        float radius = 0.0f;
        float velocity = 0.0f;
        float lifetime = 0.0f;
        uint32_t texture = UINT32_MAX;
        uint32_t mapTexture = UINT32_MAX;
        bool drawMapTexture = false;
    };
    std::vector<EffectShockwave> effectShockwaves;

    struct EffectDebris {
        Point3F pos{}, vel{};
        Point3F rotationAxis{0, 1, 0};
        float rotation = 0.0f;
        float age = 0.0f;
        float lifetime = 3.0f;
        float radius = 0.25f;
        float elasticity = 0.35f;
        float friction = 0.5f;
        float gravModifier = 1.0f;
        float terminalVelocity = 0.0f;
        int bounces = 0;
        int maxBounces = 3;
        int shapeIndex = -1;
        bool explodeOnMaxBounce = false;
        bool active = true;
    };
    std::vector<EffectDebris> effectDebris;
    std::vector<DTSShape> debrisShapes;
    std::unordered_map<std::string, int> debrisShapeIndex; // path -> debrisShapes index (-1: failed)

    // ExplosionData dtsFileName: the explosion's own animated shape.
    struct EffectExplosionShape {
        Point3F pos{};
        int shapeIndex = -1;
        int ambientIndex = -1;
        float age = 0.0f;
        float delay = 0.0f;
        float lifetime = 1.0f;
        float playSpeed = 1.0f;
        float roll = 0.0f;
        bool faceViewer = false;
        std::vector<float> times;
        std::vector<std::array<float, 3>> sizes;
        bool active = true;
    };
    std::vector<EffectExplosionShape> effectExplosionShapes;
    std::vector<DTSShape> explosionShapes;
    std::unordered_map<std::string, int> explosionShapeIndex;

    struct EffectLightning {
        Point3F pos{};
        Point3F scale{1, 1, 1};
        MatrixF rotation;
        float strikeWidth = 1.0f;
        float strikesPerMinute = 0.0f;
        float chanceToHitTarget = 0.0f;
        float strikeRadius = 0.0f;
        float boltStartRadius = 0.0f;
        uint32_t randomSeed = 0x6d2b79f5u;
        uint32_t texture = UINT32_MAX;
        ColorF color{1, 1, 1, 1};
        ColorF fadeColor{1, 1, 1, 1};
        float age = 0.0f;
        float nextStrike = 0.0f;
        Point3F start{}, end{};
        float life = 0.0f;
        bool enabled = true;
    };
    std::vector<EffectLightning> effectLightnings;
    bool lightningEnabled = true;

    struct EffectLight {
        Point3F pos{};
        ColorF color{1.0f, 0.72f, 0.28f, 1.0f};
        float age = 0.0f;
        float delay = 0.0f;
        float lifetime = 0.0f;
        float radius = 0.0f;
    };
    std::vector<EffectLight> effectLights;

    struct EffectCameraShake {
        Point3F pos{};
        std::array<float, 3> frequency{};
        std::array<float, 3> amplitude{};
        float age = 0.0f;
        float delay = 0.0f;
        float duration = 0.0f;
        float radius = 0.0f;
        float falloff = 1.0f;
    };
    std::vector<EffectCameraShake> effectCameraShakes;

    struct EffectDecal {
        Point3F pos{};
        Point3F normal{0, 1, 0};
        // The decal's in-plane up axis: DecalManager::addDecal's direction
        // for footprints, else the fixed decal basis.
        Point3F up{};
        V12::DecodedDataBlock::DecalData data;
        float age = 0.0f;
        float lifetime = 1.0f;
        float size = 1.0f;
        float sizeX = 1.0f;
        float sizeY = 1.0f;
        float angle = 0.0f;
        uint32_t texture = UINT32_MAX;
        std::vector<uint32_t> textures;
        std::vector<float> textureDurations;
        uint32_t frameSeed = 0;
        uint32_t sourceRef = 0;
    };
    std::vector<EffectDecal> effectDecals;

    struct ItemPickup {
        Point3F pos;
        enum Type { Health, Energy, Ammo } type;
         float respawnTimer = 0.0f;
         float amount = 25.0f;
          // Match the native Item datablock fallback when no script
          // datablock can be resolved during mission loading.
          float respawnDelay = 20.0f;
         int worldObjectIndex = -1;
         bool enabled = true;
         bool active = true;
    };
    std::vector<ItemPickup> items;

    Point3F playerSpawn{0, 5, 0};
    bool loaded = false;

public:
    // Precipitation system
    struct PrecipitationDrop {
        Point3F pos;
        Point3F vel;
        bool active = false;
        int cell = 0; // 4x4 atlas cell (Precipitation::setupTexCoords)
    };
    struct PrecipitationState {
        int type = 0;
        float percentage = 1.0f;
        int numDrops = 1024;
        int configuredDrops = 1024;
        // Keep the authored coverage while the script-level enabled toggle
        // temporarily drives the live percentage to zero.
        float configuredPercentage = 1.0f;
        float boxWidth = 200.0f;
        float boxHeight = 100.0f;
        float dropSize = 0.5f;
        // PrecipitationData sizeX/sizeY; zero uses the square dropSize sprite.
        float sizeX = 0.0f, sizeY = 0.0f;
        float minSpeed = 1.5f;
        float maxSpeed = 2.0f;
        bool followCam = true;
        bool useWind = true;
        Point3F origin{};
        ColorF color{0.8f, 0.85f, 0.9f, 0.4f};
        std::vector<uint32_t> textures;
        std::vector<float> textureDurations;
        float textureAge = 0.0f;
        bool active = false;
        uint32_t randomSeed = 0x4d595df4u;
        std::vector<PrecipitationDrop> drops;
    };
    PrecipitationState precipitation;
    void initPrecipitation(const PrecipitationState& state);
    void updatePrecipitation(float dt, const Point3F& camPos);
    void renderPrecipitation();
    bool setPrecipitation(int type, float percentage);
    bool setPrecipitationEnabled(bool enabled);
    bool setPrecipitationType(int type);
    bool setPrecipitationWind(const Point3F& velocity);
    bool setPrecipitationBox(float width, float height);
    bool setLightningEnabled(bool enabled);
    bool strikeLightning();

    // Water rendering
    struct WaterState {
        bool active = false;
        std::string name;
        float level = 0.0f;           // Y position of water surface
        ColorF surfaceColor{0.1f, 0.3f, 0.6f, 0.5f};
        float opacity = 0.5f;
        float waveSpeed = 0.5f;
        float waveMagnitude = 0.15f;
        float size = 2048.0f;         // Coverage area
        float originX = -1024.0f;
        float originZ = -1024.0f;
        float sizeX = 2048.0f;
        float sizeY = 2048.0f;
        int liquidType = 1;           // WaterBlock::eOceanWater
        std::vector<uint32_t> surfaceFrames;
        std::vector<float> surfaceFrameDurations;
        std::vector<uint32_t> shoreFrames;
        std::vector<float> shoreFrameDurations;
        std::vector<uint32_t> envFrames;
        std::vector<float> envFrameDurations;
        float envIntensity = 0.0f;
        float shoreDepth = 0.0f;
        // The engine fluid (fluid::SetInfo): the block's min corner in fluid
        // space (Torque XY + 1024) and extent, removeWetEdges, and the mesh
        // of the accepted 5x5-vertex blocks, built once the terrain is in.
        float fluidX0 = 0.0f, fluidY0 = 0.0f;
        bool removeWetEdges = true;
        bool fluidBuilt = false;
        bool fluidHighRes = false;
        float fluidStep4 = 64.0f;
        uint32_t fluidVao = 0, fluidVbo = 0;
        int fluidVertexCount = 0;
    };
    WaterState water;
    std::vector<WaterState> waterBodies;
    void renderWater();

private:
    // Bots (simple AI targets)
    struct Bot {
        Point3F pos{0, 5, 0};
        Point3F startPos{0, 5, 0};
        float health = 100.0f;
        bool alive = true;
        float respawnTimer = 0;
        DTSShape* shape{};
        float patrolOffset = 0;
        float moveYaw = 0;
        float animTime = 0;
        float lastHitTime = -10.0f; // time of last damage (-10 = never)
    };
public:
    std::vector<Bot> bots;
    void spawnBots(int count);
};

inline float spawnYawFromTorqueRotation(const Point3F& axis, float angleDeg) {
    const Point3F forward = Math::torqueCameraForwardToYUp(
        axis, Math::DEG2RAD(angleDeg));
    return std::atan2(forward.x, forward.z);
}

class Game {
public:
    Game();
    ~Game();

    bool init();
    void shutdown();

    void update(float dt);
    void render(float dt);

    // With sceneObjects, the world is built from them (a demo's scene
    // ghosts) rather than from the mission file.
    void startLocalGame(const char* map = nullptr, std::vector<MisObject>* sceneObjects = nullptr);
    // The world a demo's SceneObject ghosts describe, in mission-file form;
    // empty until its TerrainBlock ghost is in.
    std::vector<MisObject> demoSceneObjects() const;
    // Scene effects a demo's ghosts carry with recorded datablocks (mission
    // emitters, lightning, precipitation, force fields), added once the
    // world is loaded.
    void applyDemoSceneEffects();
    // demoParser->getGhostResets() the demo world was built at.
    int demoWorldGhostResets = -1;
    void connectToServer(const char* host, uint16_t port, bool observer = false,
                         const char* password = nullptr);
    bool playDemo(const char* path);
    void stopDemoPlayback();
    void disconnectedCleanup();

    GameConfig& config() { return cfg; }
    const GameConfig& config() const { return cfg; }
    Player& player() { return *pl; }
    const Player& player() const { return *pl; }
    World& world() { return *w; }
    bool hasWorld() const { return w != nullptr; }
    bool deleteMissionObjectIfPresent(const std::string& name);
    bool isTestShapeLoaded() const { return testShapeLoaded; }

    // Mapper mode access
    bool isMapperMode() const { return mapperMode; }
    void setMapperMode(bool m) { mapperMode = m; }
    void setFreeCamPos(const Point3F& p) { freeCamPos = p; }
    void setFreeCamTarget(const Point3F& t) {
        freeCamTarget = t;
        const Point3F d{t.x - freeCamPos.x, t.y - freeCamPos.y, t.z - freeCamPos.z};
        const float horizontal = std::sqrt(d.x * d.x + d.z * d.z);
        const float distance = std::sqrt(horizontal * horizontal + d.y * d.y);
        if (distance > 0.0001f) {
            freeCamRot.z = std::atan2(d.x, d.z);
            freeCamRot.x = std::asin(Math::clamp(d.y / distance, -1.0f, 1.0f));
        }
    }
    void setFreeCamActive(bool a) { freeCamActive = a; }
    bool isFreeCamActive() const { return freeCamActive; }
    void selectMapperObserverCamera(int index);

    enum State {
        MenuScreen,
        Loading,
        Playing,
        Dead,
        Scoreboard
    };

    State state() const { return gameState; }
    void setState(State s) { gameState = s; }
    Connection* activeConnection() const { return activeConn; }
    void togglePauseGame() { gamePaused = !gamePaused; }
    bool isGamePaused() const { return gamePaused; }

    bool isRunning() const { return gameState != MenuScreen; }
    float gameTime() const { return time; }

    // Input handling
    struct InputMove {
        bool forward{}, backward{}, left{}, right{};
        bool jump{}, jet{}, fire{}, altFire{};
        bool zoom{}, reload{};
        bool freeCam{}, orbitCam{}, showScoreboard{};
        bool demoPause{}, demoStepFrame{}, demoShowEvents{};
        Point3F lookDelta{};
    };

    void applyInput(const InputMove& input);
    void resetInputState();
    void setGravity(float value) { gravity = value; }
    float getGravity() const { return gravity; }
    void setTimeScale(float value);
    float getTimeScale() const { return timeScale; }
    GameServer& gameServer() { return server; }

    // Client-side prediction
    struct StoredMove {
        uint32_t seq;
        InputMove input;
        float dt;
    };
    uint32_t moveSeq = 0;
    std::deque<StoredMove> pendingMoves;
    void reconcile(const Point3F& serverPos, const Point3F& serverVel, uint32_t lastProcessedSeq);
    void dispatchClientCommand(const std::vector<std::string>& raw, const std::vector<bool>& tagged);
    void resetLiveMissionState();
    void clearMissionAudio();
    void clearProjectileAudio();

    bool scoreboardShown() const { return showScoreboard; }
    bool isZooming() const { return currentInput.zoom; }
    bool isDemoPlaying() const { return demoPlaying; }
    bool isDemoPaused() const { return demoPaused; }
    bool isDemoFastForward() const { return demoFastForward || demoJetHeld; }
    float getDemoSpeed() const { return isDemoFastForward() ? std::max(demoPlaybackRate, 4.0f) : demoPlaybackRate; }
    bool demoStepFeedback() const { return demoStepFeedbackTime > 0.0f; }
    bool demoFirstPersonCamActive() const { return demoFirstPersonCam; }
    float getDemoTime() const { return demoTime; }
    float getDemoTotalTime() const { return demoTotalTime; }
    bool demoHasPosition() const { return demoHasPos; }
    int getControlGhostIndex() const {
        if (demoPlaying) return controlGhostIndex;
        return serverPlayerGhostSynced ? (int)serverPlayerGhostIndex : -1;
    }
    int getSpectateGhostIndex() const { return spectateGhostIndex; }
    // Each demo ghost's mount-node frames as last drawn (world, Y-up).
    struct MountFrames { MatrixF frame[32]; uint32_t valid = 0; };
    std::unordered_map<int, MountFrames> demoMountFrames;
    // Point lights registered by demo ghosts (projectiles, items, images)
    // during the last ghost pass.
    std::vector<DynamicPointLight> demoLights;
    // Each demo ghost's animated "eye" node as last drawn (world, Y-up):
    // Player::getEyeTransform's position.
    std::unordered_map<int, Point3F> demoEyePositions;
    // Each demo ghost's camera node ("cam", else "eye") as last drawn.
    std::unordered_map<int, Point3F> demoCamPositions;
    // Each demo ghost's shape frame (model x upOrientation) as last drawn:
    // Player::getRenderEyeTransform's rotation when mounted.
    std::unordered_map<int, MatrixF> demoShapeFrames;
    // The recorder's mHead.z (freelook yaw) and whether it pilots a vehicle.
    float demoHeadZ = 0.0f;
    bool demoPiloting = false;
    // The recording's $firstPerson (initial block, then info blocks).
    bool demoRecordedFirstPerson = true;
    void applyDemoInfoBlock(const DemoBlock& block);
    // One 32 ms client tick of every demo Player ghost's simulation (the
    // recorder's with this Move block's move).
    void tickDemoPlayers(const DemoBlock& moveBlock);
    PlayerPrediction::Collision demoPlayerCollision;
    float demoLastPlayerTick = 0.0f;
    bool targetFinderOpen() const { return targetFinderShown; }
    void toggleTargetFinder();
    void closeTargetFinder() { targetFinderShown = false; }
    void selectSpectateTarget(int ghostIndex);
    float getDamageFlash() const { return damageFlash; }
    float getWhiteOut() const { return whiteOut; }
    void recordDamageFlash(float amount);
    bool isUnderwater() const {
        if (!w) return false;
        return w->isUnderwater(demoPlaying ? demoCameraPos : player().cameraPos());
    }
    void toggleDemoPause();
    void pauseDemo();
    void resumeDemo();
    void toggleDemoEvents() { demoShowEvents = !demoShowEvents; }
    bool demoEventsShown() const { return demoShowEvents; }
    bool demoOrbitCamActive() const { return demoOrbitCam; }
    const std::vector<DemoTimedEvent>& getDemoEventLog() const { return demoEventLog; }
    void requestDemoStep(int blocks = 1) {
        demoStepBlocks = std::max(1, blocks);
        demoStepRequest = true;
        demoStepFeedbackTime = 0.75f;
    }
    const std::vector<Point3F>& getDemoPath() const { return demoPath; }
    void setDemoFastForward(bool v) { demoFastForward = v; }
    void setDemoPlaybackSpeed(float speed);
    void resetDemoEvents();
    void resetDemoHud();
    void resetDemoCamera();
    void resetDemoEffects();
    void resetDemoPresentation();
    bool tryLoadDemoMission(const std::string& mission, bool resetParserState = true);
    DemoParser* getDemoParser() const { return demoParser; }
    Menu& menu() { return *mMenu; }
    int getDemoBlocksDone() const { return demoBlocksDone; }
    int getDemoBlocksTotal() const { return demoBlocksTotal; }
    // TSShapeConstructor: import each "file.dsq alias" entry from shapes/
    // and build the shape's player action table.
    // Client-side player animation state, advanced once per 32 ms tick.
    void updateDemoPlayerAnimation(float tickTime);
    void importShapeSequences(DTSShape& shape, const std::string& shapePath,
                              const std::vector<std::string>& sequences);
    DTSShape* getOrLoadDemoShape(const std::string& className, const std::string& skinName = "",
                                 const std::string& datablockInstance = "");

    // Live ghost accessors for HUD/scoreboard
    bool isConnected() const { return activeConn && activeConn->isConnected(); }
    std::vector<int> getLiveGhostIndices() const { return liveGhosts.getAllIndices(); }
    const GhostEntry* getLiveGhost(int idx) const { return liveGhosts.getGhost(idx); }
    struct LiveTeamScore {
        int teamId = 0;
        std::string name;
        int score = 0;
        std::string flagStatus = "home";
        std::string flagCarrier;
    };
    const std::map<int, LiveTeamScore>& getLiveTeamScores() const { return liveTeamScores; }
    bool liveMatchStarted() const { return liveMatchStarted_; }
    bool liveMatchEnded() const { return liveMatchEnded_; }
    const std::string& liveMissionDisplayName() const { return liveMissionDisplayName_; }
    const std::string& liveMissionType() const { return liveMissionType_; }
    int liveClockRemainingMs() const;
    const std::vector<std::string>& liveLoadInfoLines() const { return liveLoadInfoLines_; }
    size_t getLiveTargetCount() const { return liveTargets.size(); }
    const V12::ServerEvent::TargetInfo* getLiveTarget(uint16_t id) const {
        auto it = liveTargets.find(id);
        return it == liveTargets.end() ? nullptr : &it->second;
    }
     uint32_t getLiveMissionCrc() const { return liveMissionCrc; }

    void setSensorGroupCount(int count) { sensorGroupCount = std::clamp(count, 0, 32); }
    int getSensorGroupCount() const { return sensorGroupCount; }
    void setSensorGroupListenMask(int group, uint32_t mask) {
        sensorGroupListenMasks[group] = mask;
    }
    uint32_t getSensorGroupListenMask(int group) const {
        auto it = sensorGroupListenMasks.find(group);
        return it == sensorGroupListenMasks.end() ? 0xffffffffu : it->second;
    }
    bool isSensorGroupTargetVisible(int listenerGroup, int targetGroup) const {
        if (!HudParity::sensorGroupInRange(targetGroup, sensorGroupCount)) return false;
        auto native = liveSensorGroupListenMasks.find(listenerGroup);
        if (native != liveSensorGroupListenMasks.end())
            return (native->second & (uint32_t(1) << targetGroup)) != 0;
        auto it = sensorGroupListenMasks.find(listenerGroup);
        return it == sensorGroupListenMasks.end() ||
            (targetGroup < 32 && (it->second & (uint32_t(1) << targetGroup)) != 0);
    }

     void setSensorGroupColor(int group, uint32_t targetMask, const ColorF& color) {
         sensorGroupColors[{group, targetMask}] = color;
    }
    const ColorF* getSensorGroupColor(int group, uint32_t targetMask = 1) const {
        auto it = sensorGroupColors.find({group, targetMask});
        return it == sensorGroupColors.end() ? nullptr : &it->second;
    }
    void setSensorGroupFriendlyMask(int group, uint32_t mask) { sensorGroupFriendlyMasks[group] = mask; }
    void setTargetFriendlyMask(int group, uint32_t mask) { setSensorGroupFriendlyMask(group, mask); }
    uint32_t getTargetFriendlyMask(int group) const {
        auto it = sensorGroupFriendlyMasks.find(group);
        return it == sensorGroupFriendlyMasks.end() ? 0 : it->second;
    }
    bool isTargetFriendly(int listenerGroup, int targetGroup) const {
        auto mask = sensorGroupFriendlyMasks.find(listenerGroup);
        if (targetGroup >= 0 && targetGroup < 32 && mask != sensorGroupFriendlyMasks.end())
            return (mask->second & (uint32_t(1) << targetGroup)) != 0;
        return pl && targetGroup >= 0 && targetGroup == player().team();
    }

    // Shape viewer mode
    bool shapeViewerActive = false;
    std::vector<std::string> shapeViewerFiles;
    int shapeViewerIndex = 0;
    DTSShape shapeViewerShape;
    float shapeViewerYaw = 0.6f;
    float shapeViewerPitch = 0.25f;
    float shapeViewerAnimTime = 0;
    bool shapeViewerBoundsInit = false;
    Point3F shapeViewerCenter{0,0,0};
    float shapeViewerFitScale = 1.0f;
    bool shapeViewerAutoCycle = false;
    int shapeViewerCycleDelay = 0;
    int shapeViewerFramesRemaining = 0;

    void enterShapeViewer();
    void shapeViewerNext();
    void shapeViewerPrev();
    void shapeViewerLoadCurrent();
    bool isShapeViewerActive() const { return shapeViewerActive; }

private:
    GameConfig cfg;
    Player* pl{};
    World* w{};
    Menu* mMenu{};
    HUD* hud{};
    State gameState = MenuScreen;
    float time = 0;
    float gravity = -20.0f;
    float timeScale = 1.0f;
    float deathTimer = 0.0f;
    bool missionRespawn = true;
    SoundSource* ambientSource{};
    SoundBuffer* ambientSound{};
    std::vector<SoundSource*> emitterSources;
    std::unordered_map<uint64_t, SoundSource*> shapeBaseSoundSources;
    std::unordered_map<int, SoundSource*> demoJetSoundSources; // ghost -> looping jetSound
    std::unordered_map<uint16_t, SoundSource*> projectileSoundSources;
    std::unordered_set<uint64_t> demoAudioEventsPlayed;
    int32_t weatherType = 0; // 0=dry, 1=cold, 2=wet
    InputMove currentInput;
    bool previousFire = false;
    bool previousAltFire = false;
    bool previousReload = false;
    bool previousZoom = false;
    bool zoomed = false;
    bool freeCamActive = false;
    bool showScoreboard = false;
    Point3F freeCamPos{0, 10, 0};
    Point3F freeCamTarget{0, 10, -1};
    Point3F freeCamRot{0, 0, 0};
    bool mapperMode = false;  // -mapper: no player, free-fly camera only
    GameServer server;
    Connection* activeConn{};

    // Demo playback
    DemoParser* demoParser{};
    std::map<int, DemoParserSnapshot> demoSnapshots;
    // The recorder's view at each snapshot, so a seek resumes it.
    struct DemoViewSnapshot {
        int controlGhostIndex = -1;
        float viewYaw = 0.0f, viewPitch = 0.0f;
        bool hasOrientation = false, hasPos = false, authoredCamera = false;
        Point3F cameraPos{}, cameraTarget{};
        float cameraFov = -1.0f;
        int cameraMode = -1, orbitGhost = -1;
        bool recordedFirstPerson = true;
        float headZ = 0.0f;
        bool piloting = false;
        float orbitMinDist = 0.0f, orbitMaxDist = 0.0f;
        Point3F orbitPoint{};
    };
    std::map<int, DemoViewSnapshot> demoViewSnapshots;
    DemoViewSnapshot captureDemoView() const;
    void restoreDemoView(const DemoViewSnapshot& view);
    void applyDemoMoveView(const DemoBlock& block);
    void applyDemoPacketView(const PacketData& pd);
    T2Demo::MissionReplacementState demoMissionState;
    bool demoPlaying = false;
    bool gamePaused = false;
    bool demoPaused = false;
    bool demoStepRequest = false;
    int demoStepBlocks = 1;
    bool demoJetHeld = false; // Space key held (fast-forward indicator)
    float demoStepFeedbackTime = 0.0f;
    std::unordered_map<std::string, DTSShape> demoShapeCache;
    DTSShape testShape;
    bool testShapeLoaded = false;
    bool demoFastForward = false;
    float demoPlaybackRate = 1.0f;
    float demoTime = 0;
    float demoTotalTime = 0;
    float demoInterpolationDt = 0;
    int demoPacketsParsed = 0;
    int demoBlocksTotal = 0;
    int demoBlocksDone = 0;
    Point3F demoCameraPos{0, 5, 0};
    Point3F demoCameraTarget{0, 5, -1};
    Point3F demoPrevCameraPos{0, 5, 0};
    Point3F demoPrevCameraTarget{0, 5, -1};
    float demoMoveBlend = 1.0f;
    bool demoHasPos = false;
    bool demoHasOrientation = false;
    float demoViewYaw = 0.0f;
    float demoViewPitch = 0.0f;
    int controlGhostIndex = -1;  // control object ghost index during demo
    int spectateGhostIndex = -1; // spectating a specific ghost (-1 = follow control object)
    bool targetFinderShown = false;
    float damageFlash = -1.0f;  // red screen flash during demo playback
    float whiteOut = -1.0f;     // white screen flash during demo playback
    float demoCameraFov = -1.0f; // FOV from demo stream
    bool demoAuthoredCamera = false;
    // The recorded Camera control object's mode and orbit (Torque space).
    int demoCameraMode = -1;
    int demoOrbitGhost = -1;
    float demoOrbitMinDist = 0.0f, demoOrbitMaxDist = 0.0f;
    Point3F demoOrbitPoint{};
    // Each demo ghost's world-box centre as last drawn (Y-up): the orbit
    // centre of Camera::interpolateTick (getRenderWorldBox().getCenter()).
    std::unordered_map<int, Point3F> demoBoxCenters;
    std::vector<DemoTimedEvent> demoEventLog;
    bool demoShowEvents = false;
    // Orbit camera for demo spectator mode
    bool demoOrbitCam = false;
    // demoObserve: a chase camera behind one ghost (-1 = off).
    int demoObserveGhost = -1;
    float demoObserveDistance = 4.0f, demoObserveHeight = 1.5f;
    // Match-over interval (MissionEnd / debrief) until the next
    // MsgClientReady: the world is held at its final state.
    bool demoMatchEnded = false;
    float demoMatchEndedAt = 0.0f;
    GhostTracker demoEndedGhosts;
    void setDemoMatchEnded(bool ended);
    bool demoFirstPersonCam = false;
    float orbitAngle = 0;
    float orbitDistance = 300.0f;
    float orbitHeight = 150.0f;
    Point3F orbitCenter{};
    bool orbitCenterInit = false;
    std::vector<Point3F> demoPath;
    int demoPathCount = 0;

    // Live spectator
    bool liveSpectateInit = false;
    bool liveSpectateRespawned = false;
    int liveFollowGhostIndex = -1;
    Point3F liveFollowCenter{};
     bool liveFollowCenterInit = false;
    bool previousDemoPause = false;
    bool previousDemoStep = false;
    bool previousDemoEvent = false;
    bool previousObserverCycle = false;

    // Editor mode
    bool editorActive = false;
    int editorPlaceClass = 31; // classId to place
    uint32_t editorLastGhost = 0;
    bool editorHasGhost = false;

    // Projectile trail system

    // Live network ghost tracking
    GhostTracker liveGhosts;
    // Received datablock tracking: classId → list of datablocks with payload
    struct ReceivedDatablock {
        T2Protocol::DatablockHeader hdr;
        std::vector<uint8_t> payload;
    };
    std::map<uint32_t, std::vector<ReceivedDatablock>> receivedDatablocks;
    std::map<uint16_t, std::string> nativeDatablockShapes;
    std::map<uint32_t, ParsedDataBlock> nativeDatablocks;
     std::map<uint16_t, V12::ServerEvent::TargetInfo> liveTargets;
     int sensorGroupCount = 32;
     std::map<int, uint32_t> sensorGroupListenMasks;
     std::map<std::pair<int, uint32_t>, ColorF> sensorGroupColors;
     std::map<int, uint32_t> sensorGroupFriendlyMasks;
     std::map<int, uint32_t> liveSensorGroupListenMasks;
    uint32_t liveMissionCrc = 0;
    std::map<int, LiveTeamScore> liveTeamScores;
    std::map<int, int> livePlayerScores;
    std::map<int, int> liveClientTargetIds;
    std::map<int, std::string> liveClientNames;
    std::map<int, int> liveClientTeams;
    bool liveMatchStarted_ = false;
    bool liveMatchEnded_ = false;
    std::string liveMissionDisplayName_;
    std::string liveMissionType_;
    int liveClockDurationMs_ = 0;
    double liveClockReceivedAt_ = 0.0;
    std::vector<std::string> liveLoadInfoLines_;
    const std::vector<ReceivedDatablock>* getDatablocksForClass(uint32_t classId) const {
        auto it = receivedDatablocks.find(classId);
        return it != receivedDatablocks.end() ? &it->second : nullptr;
    }
    // Ghost index assigned by server for this client's player
    uint32_t serverPlayerGhostIndex = 0;
    bool serverPlayerGhostSynced = false;
};
