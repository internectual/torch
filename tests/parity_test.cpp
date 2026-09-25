#include "net/v12_datablocks.h"
#include "net/v12_ghosts.h"
#include "net/protocol.h"
#include "net/v12_events.h"
#include "render/renderer.h"
#include "game/game.h"
#include "game/demo.h"
#include "game/collision.h"
#include "render/texture_frames.h"
#include "core/input_parity.h"
#include "core/console.h"
#include "game/movement.h"
#include "game/item_parity.h"
#include "game/weapon.h"
#include "game/death_respawn.h"
#include "game/physics.h"
#include "game/hud_parity.h"
#include "game/mission_rules.h"
#include "game/ctf_runtime.h"
#include "game/match_runtime.h"
#include "game/objective_parity.h"
#include "game/link_beam.h"
#include "game/water_parity.h"
#include "game/observer_parity.h"
#include "game/damage_parity.h"
#include "game/precipitation_parity.h"
#include "game/ghost_parity.h"
#include "game/animation_parity.h"
#include "game/particle_parity.h"
#include "game/wind.h"
#include "render/dts_animation.h"

#include <cassert>
#include <cstring>
#include <limits>

static void f32(V12BitWriter& w, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    w.writeUnsigned(bits, 32);
}

static void testParticleTickParity() {
    assert(!particleTickIsUsable(0.0f));
    assert(!particleTickIsUsable(-0.1f));
    assert(!particleTickIsUsable(std::numeric_limits<float>::quiet_NaN()));
    assert(particleTickIsUsable(1.0f / 60.0f));
    assert(particleLifetimeAfterTick(1.0f, 0.25f) == 0.75f);
    assert(particleLifetimeAfterTick(1.0f, 0.0f) == 1.0f);
    assert(particleLifetimeAfterTick(std::numeric_limits<float>::quiet_NaN(), 0.1f) == 0.0f);
    assert(debrisGravityAcceleration(-20.0f, 1.0f) == -20.0f);
    assert(debrisGravityAcceleration(-20.0f, -0.5f) == 10.0f);
    assert(debrisGravityAcceleration(std::numeric_limits<float>::quiet_NaN(), 1.0f) == -20.0f);
}

static void testDtsThreadTimingParity() {
    assert(dtsThreadTime(0.5f, 2.0f, 0.25f, 1.0f, false, true) == 1.25f);
    assert(dtsThreadTime(2.0f, 2.0f, 0.0f, 1.0f, false, true) == 2.0f);
    assert(dtsThreadTime(-1.0f, 2.0f, 0.0f, 1.0f, false, true) == 0.0f);
    assert(dtsThreadTime(0.5f, 2.0f, 4.0f, -1.0f, false, true) == 0.0f);
    assert(dtsThreadTime(0.5f, 2.0f, 0.0f, 1.0f, true, false) == 0.0f);
    assert(dtsThreadTime(0.5f, std::numeric_limits<float>::quiet_NaN(),
                        0.0f, 1.0f, false, true) == 0.0f);
}

static void testLoopingAnimationReverseParity() {
    assert(animationSampleTime(2.25f, 2.0f) == 0.25f);
    assert(animationSampleTime(-0.25f, 2.0f) == 1.75f);
    assert(animationSampleTime(-4.25f, 2.0f) == 1.75f);
    assert(animationSampleTime(-0.25f, 2.0f, false) == 0.0f);
}

static void testBotRespawnClearsReactionState() {
    float patrolOffset = 12.0f;
    float moveYaw = 2.0f;
    float animTime = 4.0f;
    float lastHitTime = 91.0f;
    DeathRespawn::resetBotMotion(patrolOffset, moveYaw, animTime, lastHitTime);
    assert(patrolOffset == 0.0f && moveYaw == 0.0f && animTime == 0.0f);
    assert(lastHitTime == DeathRespawn::NeverHitTime);
}

static void testWindRejectsInvalidComponents() {
    setTorchWindVelocity({1.0f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()});
    const Point3F wind = getTorchWindVelocity();
    assert(wind.x == 1.0f && wind.y == 0.0f && wind.z == 0.0f);
    setTorchWindVelocity({});
}

static void testSpawnSphereFacingParity() {
    assert(std::fabs(spawnYawFromTorqueRotation({0.0f, 0.0f, 1.0f}, 90.0f)) > 1.0f);
    const float defaultYaw = spawnYawFromTorqueRotation({0.0f, 0.0f, 1.0f}, 0.0f);
    assert(std::fabs(std::fabs(defaultYaw) - Math::PI) < 1.0e-6f);
}

static void testDamageFlashHealthCapParity() {
    assert(damageFlashForAmount(25.0f) == 0.25f);
    // Damage feedback follows the authored PlayerData health cap, not a
    // universal 100-point denominator.
    assert(damageFlashForAmount(50.0f, 200.0f) == 0.25f);
    assert(damageFlashForAmount(250.0f, 200.0f) == 1.0f);
    assert(damageFlashForAmount(50.0f, 0.0f) == 0.0f);
    assert(applySignedDamage(50.0f,  -15.0f) == 65.0f);
    assert(applySignedDamage(95.0f,  -15.0f) == 100.0f);
    assert(applySignedDamage(50.0f,   60.0f) == 0.0f);
}

static void testFiniteAmmoSentinelParity() {
    Weapon finiteAmmo;
    finiteAmmo.type = 0; // Spinfusor has a finite reserve.
    finiteAmmo.ammo = -1;
    assert(!finiteAmmo.canFire(100.0f));

    Weapon unlimitedAmmo;
    unlimitedAmmo.type = 1; // Blaster uses the native unlimited sentinel.
    unlimitedAmmo.ammo = -1;
    assert(unlimitedAmmo.canFire(100.0f));
}

static void testVerticalProjectilePlayerHit() {
    float hitT = -1.0f;
    assert(segmentPlayerHit({0.0f, 5.0f, 0.0f}, {0.0f, -5.0f, 0.0f},
                            {0.0f, 0.0f, 0.0f}, hitT));
    assert(hitT >= 0.0f && hitT <= 1.0f);
    hitT = -1.0f;
    assert(!segmentPlayerHit({2.0f, 5.0f, 0.0f}, {2.0f, -5.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f}, hitT));
    // ShapeBase bounds include their horizontal edge, including for a trace
    // that is stationary at that edge.
    hitT = -1.0f;
    assert(segmentPlayerHit({std::sqrt(2.0f), 0.0f, 0.0f},
                             {std::sqrt(2.0f), 0.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f}, hitT));
    assert(hitT == 0.0f);
}

static void ref(V12BitWriter& w, uint32_t value) {
    w.writeFlag(value != 0);
    if (value) w.writeUnsigned(value, 11);
}

static V12BitStream stream(const V12BitWriter& w) {
    return V12BitStream(w.data().data(), w.data().size());
}

static void testDataBlocks() {
    V12BitWriter debris;
    debris.writeHuffmanString("shapes/debris.dts");
    debris.writeUnsigned(0x0a, 5);
    V12::DecodedDataBlock debrisDecoded;
    auto debrisStream = stream(debris);
    assert(V12::readDataBlockPayload(debrisStream, 6, &debrisDecoded));
    assert(debrisDecoded.debrisShape == "shapes/debris.dts");
    assert(debrisStream.readUnsigned(5) == 0x0a && !debrisStream.failed());

    V12BitWriter particle;
    particle.writeUnsigned(0, 10); particle.writeFlag(true); f32(particle, 2.0f);
    particle.writeUnsigned(0, 12); particle.writeUnsigned(0, 9);
    particle.writeFlag(true); f32(particle, 3.0f);
    particle.writeUnsigned(4, 10); particle.writeUnsigned(2, 10);
    particle.writeFlag(true); f32(particle, 4.0f); particle.writeFlag(true);
    particle.writeUnsigned(1001, 11); particle.writeUnsigned(1003, 11); particle.writeFlag(true);
    particle.writeUnsigned(0, 2);
    for (int i = 0; i < 4; ++i) particle.writeUnsigned(0, 7);
    particle.writeUnsigned(0, 14); particle.writeUnsigned(0, 8);
    particle.writeUnsigned(1, 6); particle.writeHuffmanString("smoke");
    V12::DecodedDataBlock decoded;
    auto particleStream = stream(particle);
    assert(V12::readDataBlockPayload(particleStream, 27, &decoded));
    assert(decoded.hasParticle && decoded.particle.lifetimeMS == 128);
    assert(decoded.particle.keys.size() == 1 && decoded.particle.textures[0] == "smoke");
    assert(decoded.particle.useInvAlpha && decoded.particle.spinRandomMin == 1.0f);

    V12BitWriter emitter;
    emitter.writeUnsigned(7, 10); emitter.writeUnsigned(2, 10);
    emitter.writeUnsigned(300, 16); emitter.writeUnsigned(4, 14);
    emitter.writeFlag(true); emitter.writeUnsigned(9, 16);
    emitter.writeUnsigned(10, 8); emitter.writeUnsigned(20, 8);
    emitter.writeFlag(true); emitter.writeUnsigned(30, 9);
    emitter.writeFlag(true); emitter.writeUnsigned(40, 9);
    emitter.writeFlag(true); emitter.writeFlag(true); emitter.writeFlag(false);
    emitter.writeUnsigned(5, 10); emitter.writeUnsigned(1, 10);
    emitter.writeFlag(true); emitter.writeFlag(true); emitter.writeUnsigned(2, 32);
    ref(emitter, 17); ref(emitter, 0);
    decoded = {};
    auto emitterStream = stream(emitter);
    assert(V12::readDataBlockPayload(emitterStream, 29, &decoded));
    assert(decoded.hasEmitter && decoded.emitter.ejectionVelocity == 300);
    assert(decoded.emitter.particleRefs.size() == 1 && decoded.emitter.particleRefs[0] == 17);
    assert(decoded.emitter.useEmitterSizes && decoded.emitter.useEmitterColors);

    V12BitWriter decal;
    decal.writeHuffmanString("decals/hit");
    decal.writeUnsigned(2500, 32); decal.writeUnsigned(300, 32);
    decal.writeUnsigned(2, 8); decal.writeUnsigned(4, 8);
    decal.writeFlag(true); decal.writeFlag(true);
    decoded = {};
    auto decalStream = stream(decal);
    assert(V12::readDataBlockPayload(decalStream, 9, &decoded));
    assert(decoded.hasDecal && decoded.decal.texture == "decals/hit");
    assert(decoded.decal.lifetimeMS == 2500 && decoded.decal.textureCols == 4);
    assert(decoded.decal.randomize && decoded.decal.renderPriority);

    V12BitWriter explosion;
    explosion.writeHuffmanString("explosion"); ref(explosion, 0); ref(explosion, 31);
    explosion.writeUnsigned(12, 14); f32(explosion, 1.5f);
    explosion.writeFlag(false); explosion.writeFlag(false); explosion.writeUnsigned(0, 14);
    for (int i = 0; i < 2; ++i) explosion.writeUnsigned(0, 8);
    for (int i = 0; i < 2; ++i) explosion.writeUnsigned(0, 9);
    for (int i = 0; i < 2; ++i) explosion.writeUnsigned(0, 10);
    explosion.writeUnsigned(0, 14);
    explosion.writeUnsigned(0, 14);
    for (int i = 2; i < 6; ++i) explosion.writeUnsigned((uint32_t)i, 16);
    f32(explosion, 0.25f);
    explosion.writeFlag(true); explosion.writeFlag(true);
    for (float value : {2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 0.8f, 12.0f, 2.0f}) f32(explosion, value);
    ref(explosion, 41); ref(explosion, 0);
    ref(explosion, 51); ref(explosion, 52); ref(explosion, 0); ref(explosion, 0);
    for (int i = 0; i < 5; ++i) ref(explosion, i == 0 ? 61 : 0);
    explosion.writeUnsigned(0, 3);
    decoded = {};
    auto explosionStream = stream(explosion);
    assert(V12::readDataBlockPayload(explosionStream, 13, &decoded));
    assert(decoded.hasExplosion && decoded.explosion.particleEmitterRef == 31);
    assert(decoded.explosion.particleDensity == 12 && decoded.explosion.delayMS == 64);
    assert(decoded.explosion.shockwaveRef == 41 && decoded.explosion.emitterRefs[1] == 52);
    assert(decoded.explosion.hasLight && decoded.explosion.shakeCamera);
    assert(decoded.explosion.shakeFrequency[0] == 2.0f && decoded.explosion.shakeFrequency[2] == 4.0f);
    assert(decoded.explosion.shakeAmplitude[0] == 5.0f && decoded.explosion.shakeAmplitude[2] == 7.0f);
    assert(decoded.explosion.shakeDuration == 0.8f && decoded.explosion.shakeRadius == 12.0f);
    assert(decoded.explosion.shakeFalloff == 2.0f);

    V12BitWriter shockwave;
    for (int i = 0; i < 3; ++i) f32(shockwave, 0.0f);
    for (int i = 0; i < 4; ++i) shockwave.writeUnsigned(0, 32);
    f32(shockwave, 12.0f); shockwave.writeUnsigned(6, 32); shockwave.writeUnsigned(8, 32);
    for (float value : {2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) f32(shockwave, value);
    for (bool value : {true, false, true, false, true}) shockwave.writeUnsigned(value, 8);
    ref(shockwave, 21); ref(shockwave, 0); ref(shockwave, 22);
    for (int i = 0; i < 4; ++i) shockwave.writeUnsigned(0x100 + i, 32);
    for (int i = 0; i < 4; ++i) f32(shockwave, 0.25f * i);
    shockwave.writeHuffmanString("shock"); shockwave.writeHuffmanString("map");
    for (int i = 0; i < 8; ++i) shockwave.writeUnsigned(0, 8);
    decoded = {};
    auto shockwaveStream = stream(shockwave);
    assert(V12::readDataBlockPayload(shockwaveStream, 40, &decoded));
    assert(decoded.hasShockwave && decoded.shockwave.width == 12.0f);
    assert(decoded.shockwave.emitterRefs[0] == 21 && decoded.shockwave.emitterRefs[2] == 22);
    assert(decoded.shockwave.textures[1] == "map" && decoded.shockwave.renderSquare);
}

static void testObserverTargetParity() {
    assert(!ObserverParity::isPositionReady(false));
    assert(ObserverParity::isPositionReady(true));
    assert(ObserverParity::isLiveObserver(true, true));
    assert(!ObserverParity::isLiveObserver(false, true));
    assert(!ObserverParity::isLiveObserver(true, false));
    assert(ObserverParity::controlGhostIndex(0) == -1);
    assert(ObserverParity::controlGhostIndex(7) == 7);
    assert(ObserverParity::isPlayerTarget("Player", 0));
    assert(ObserverParity::isPlayerTarget("MPB", 0));
    assert(ObserverParity::isPlayerTarget("AIPlayer", 0));
    assert(ObserverParity::isPlayerTarget("CommanderPlayer", 0));
    assert(ObserverParity::isPlayerTarget("SiegeMPB", 0));
    // Class names are resolved case-insensitively by Torque's object system;
    // custom servers can therefore replicate a differently cased class name.
    assert(ObserverParity::isPlayerTarget("player", 0));
    assert(ObserverParity::isPlayerTarget("mpb", 1));
    assert(ObserverParity::isPlayerTarget("Player", 1));
    assert(!ObserverParity::isPlayerTarget("Player", 2));
    assert(!ObserverParity::isPlayerTarget("Player", -1));
    assert(!ObserverParity::isPlayerTarget("MPB", 2));
    assert(!ObserverParity::isPlayerTarget("Turret", 0));
    assert(!ObserverParity::isPlayerTarget("Player", 0, false));
    assert(ObserverParity::isVisiblePlayerTarget("Player", 0, true));
    assert(!ObserverParity::isVisiblePlayerTarget("Player", 0, false));
    assert(ObserverParity::isVehicleClass("FlyingVehicle"));
    assert(ObserverParity::isVehicleClass("hovervehicle"));
    assert(ObserverParity::isVehicleClass("ScoutVehicle"));
    assert(ObserverParity::isVehicleClass("CUSTOMVEHICLE"));
    assert(!ObserverParity::isVehicleClass("VehicleData"));
    assert(!ObserverParity::isVehicleClass("Turret"));
    assert(ObserverParity::isSpectatableTarget("player", 1, true));
    assert(ObserverParity::isSpectatableTarget("wheeledvehicle", 1, true));
    // Observer target search must expose vehicles, not only player ghosts.
    assert(ObserverParity::isSpectatableTarget("ScoutVehicle", 0, true));
    // An unresolved vehicle ghost has no valid damage state and must not be
    // selectable until its initial state arrives.
    assert(!ObserverParity::isSpectatableTarget("ScoutVehicle", -1, true));
    assert(!ObserverParity::isSpectatableTarget("Player", 2, true));
    assert(!ObserverParity::isSpectatableTarget("wheeledvehicle", 2, true));
    assert(!ObserverParity::isSpectatableTarget("Vehicle", 0, false));
    assert(ObserverParity::isReadySpectatableTarget("Player", 0, true));
    assert(!ObserverParity::isReadySpectatableTarget("Player", 0, false));
    assert(!ObserverParity::isReadySpectatableTarget("Vehicle", 0, false));
}

static void testMissionHiddenObjectParity() {
    assert(missionObjectCanBeHidden("InteriorInstance"));
    assert(missionObjectCanBeHidden("interiorinstance"));
    assert(missionObjectCanBeHidden("StaticShape"));
    assert(missionObjectCanBeHidden("ForceFieldBare"));
    assert(!missionObjectCanBeHidden("Trigger"));
}

static void testGhostClassParity() {
    // Torque resolves class names case-insensitively, including streamed demo ghosts.
    assert(ghostClassIs("wAtErBlOcK", "WaterBlock"));
    assert(isWorldLevelGhostClass("terrainblock"));
    assert(!isWorldLevelGhostClass("Player"));
    assert(isProjectileGhostClass("energybolt"));
    assert(isProjectileGhostClass("customprojectile"));
    assert(isWheeledVehicleGhostClass("WHEELEDVEHICLE"));
    assert(isWheeledVehicleGhostClass("wildcat"));
    assert(isWheeledVehicleGhostClass("MPB"));
    assert(isWheeledVehicleGhostClass("CustomWheeledVehicle"));
    assert(!isWheeledVehicleGhostClass("FlyingVehicle"));
    assert(isVehicleGhostClass("wheeledvehicle"));
    assert(isVehicleGhostClass("sHrIkE"));
    assert(isTurretGhostClass("sentry"));
    assert(!isTurretGhostClass("turretdata"));
}

static void testFreeCameraSpeedParity() {
    assert(freeCameraMoveScale(false, false, false, false, false, false) == 1.0f);
    assert(freeCameraMoveScale(true, false, false, false, false, false) == 1.0f);
    assert(std::fabs(freeCameraMoveScale(true, false, false, true, false, false) -
                     0.7071067f) < 0.00001f);
    assert(std::fabs(freeCameraMoveScale(true, false, false, true, true, false) -
                     0.5773502f) < 0.00001f);
}

static void testCameraPitchParity() {
    assert(clampCameraPitch(1.5f) == 1.5f);
    assert(clampCameraPitch(-1.5f) == -1.5f);
    assert(clampCameraPitch(2.0f) == cameraPitchLimit);
    assert(clampCameraPitch(-2.0f) == -cameraPitchLimit);
}

static void testAnimationDeltaParity() {
    assert(animationDelta(1.0f) == 1.0f);
    assert(animationDelta(0.1f) == 0.1f);
    assert(animationDelta(0.0f) == 0.0f);
    assert(animationDelta(-1.0f) == 0.0f);
    assert(animationDelta(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

static void testProjectileLifetimeParity() {
    float lifetime = 1.0f;
    assert(projectileLifetimeStep(lifetime, 0.25f));
    assert(lifetime == 0.75f);
    assert(!projectileLifetimeStep(lifetime, 1.0f));

    lifetime = std::numeric_limits<float>::quiet_NaN();
    assert(!projectileLifetimeStep(lifetime, 1.0f));
    lifetime = 1.0f;
    assert(projectileLifetimeStep(lifetime, 0.0f));
}

static void testHitscanResolvesInOneTick() {
    assert(projectileResolvesThisTick(ProjectileType::Hitscan));
    assert(!projectileResolvesThisTick(ProjectileType::Disc));
    assert(!projectileResolvesThisTick(ProjectileType::Bolt));
}

static void testPhysicalZoneInvalidValues() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(physicalZoneModifier(nan) == 1.0f);
    assert(physicalZoneModifier(100.0f) == 40.0f);
    assert(physicalZoneModifier(-100.0f) == -40.0f);

    const Point3F force = physicalZoneForceToYUp({nan, 2.0f, nan}, {}, 0.0f);
    assert(force.x == 0.0f && force.y == 0.0f && force.z == -2.0f);
}

static void testDamageLevelUsesAuthoredHealthCap() {
    assert(damageLevelForHealth(100.0f, 100.0f) == 0.0f);
    assert(std::fabs(damageLevelForHealth(100.0f, 150.0f) - 1.0f / 3.0f) < 1.0e-6f);
    assert(damageLevelForHealth(0.0f, 150.0f) == 1.0f);
    assert(damageLevelForHealth(100.0f, 0.0f) == 0.0f);
}

static void testRepairRateParity() {
    assert(applyRepairRate(50.0f, 10.0f, 0.5f) == 55.0f);
    assert(applyRepairRate(98.0f, 10.0f, 1.0f) == 100.0f);
    // Repair ticks do not resurrect a dead ShapeBase.
    assert(applyRepairRate(0.0f, 100.0f, 1.0f) == 0.0f);
    assert(applyRepairRate(50.0f, 10.0f, -1.0f) == 50.0f);
    assert(applyRepairRate(50.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f) == 50.0f);

    Player player;
    player.setHealth(50.0f);
    player.setRepairRate(10.0f);
    player.update(0.5f);
    assert(player.health() == 55.0f);
    player.setHealth(50.0f);
    player.update(10.0f);
    assert(player.health() == 60.0f);
    // Authored PlayerData repair rates use the same bounded runtime state as
    // direct script updates, including rejecting malformed values.
    player.setRepairRate(std::numeric_limits<float>::quiet_NaN());
    assert(player.getRepairRate() == 10.0f);
}

static void testMissionClassNamesAreCaseInsensitive() {
    assert(missionClassIs("PhysicalZone", "PhysicalZone"));
    assert(missionClassIs("physicalzone", "PhysicalZone"));
    assert(missionClassIs("PHYSICALZONE", "PhysicalZone"));
    assert(missionClassIs("forcefieldbare", "ForceFieldBare"));
    assert(missionClassIs("tUrReT", "Turret"));
    assert(missionClassIs("ITEM", "Item"));
    assert(!missionClassIs("Trigger", "PhysicalZone"));
    assert(missionClassIs("item", "Item"));
    // Runtime mission filters must use the same case-insensitive resolver as
    // the parser; mixed-case authored infrastructure must not disappear.
    assert(missionClassIs("aUdIoEmItTeR", "AudioEmitter"));
    assert(missionClassIs("mIsSiOnMaRkEr", "MissionMarker"));
    assert(missionClassIs("pArTiClEeMiSsIoNdUmMy", "ParticleEmissionDummy"));
    assert(missionClassIs("lIgHtNiNg", "Lightning"));
    // MissionArea uses the same resolver when loading the authored boundary.
    assert(missionClassIs("missionarea", "MissionArea"));
}

static void testTeamSpawnSelection() {
    auto prop = [](const char* name, const char* value) {
        return MisProp{std::string(name), std::string(value)};
    };
    std::vector<MisObject> objects;
    MisObject neutral;
    neutral.className = "SpawnSphere";
    neutral.objName = "Neutral";
    neutral.props.push_back(prop("team", "0"));
    neutral.props.push_back(prop("position", "1 2 3"));
    objects.push_back(neutral);

    MisObject inferno;
    inferno.className = "spawnsphere";
    inferno.objName = "InfernoSpawn";
    inferno.props.push_back(prop("team", "2"));
    inferno.props.push_back(prop("position", "4 5 6"));
    objects.push_back(inferno);

    const MisObject* selected = selectAuthoredSpawn(objects, 2);
    assert(selected && selected->objName == "InfernoSpawn");
    selected = selectAuthoredSpawn(objects, 1);
    assert(selected && selected->objName == "Neutral");
}

static void testObserverTeamLabel() {
    assert(std::strcmp(HudParity::teamName(1), "Storm") == 0);
    assert(std::strcmp(HudParity::teamName(2), "Inferno") == 0);
    assert(std::strcmp(HudParity::teamName(0), "Observer") == 0);
    assert(std::strcmp(HudParity::teamName(-1), "N/A") == 0);
}

static void testResourceBarFractions() {
    assert(HudParity::resourceFraction(0.0f) == 0.0f);
    assert(HudParity::resourceFraction(50.0f) == 0.5f);
    assert(HudParity::resourceFraction(150.0f) == 1.0f);
    assert(HudParity::resourceFraction(75.0f, 150.0f) == 0.5f);
    assert(HudParity::resourceFraction(150.0f) == 1.0f);
    assert(HudParity::resourceFraction(-1.0f) == 0.0f);
    assert(!HudParity::resourceFillVisible(0.9f, 100.0f, 200.0f));
    assert(HudParity::resourceFillVisible(1.5f, 100.0f, 200.0f));
    // Scoreboard health is a percentage of the replicated datablock maximum,
    // not raw hit points (heavy/custom armor can exceed 100 HP).
    assert(HudParity::resourceFraction(75.0f, 150.0f) * 100.0f == 50.0f);
}

static void testScoreboardTeamRowsOnlyReserveVisibleStrip() {
    assert(HudParity::scoreboardTeamRows(false, false, 2) == 0);
    assert(HudParity::scoreboardTeamRows(true, false, 2) == 0);
    assert(HudParity::scoreboardTeamRows(true, true, 2) == 2);
    assert(HudParity::scoreboardTeamRows(true, true, -1) == 0);
}

static void testArmorCannotAbsorbMoreThanRemains() {
    float health = 100.0f;
    float armor = 10.0f;
    assert(std::fabs(applyArmorDamage(health, armor, 100.0f) - 10.0f) < 0.001f);
    assert(std::fabs(health - 10.0f) < 0.001f);
    assert(std::fabs(armor) < 0.001f);

    health = 100.0f;
    armor = 100.0f;
    assert(std::fabs(applyArmorDamage(health, armor, 50.0f) - 30.0f) < 0.001f);
    assert(std::fabs(health - 80.0f) < 0.001f);
    assert(std::fabs(armor - 70.0f) < 0.001f);

    health = 10.0f;
    armor = 5.0f;
    assert(std::fabs(applyArmorDamage(health, armor, 100.0f) - 5.0f) < 0.001f);
    assert(health == 0.0f);
}

static void testLocalBotKillsUpdateScoreboard() {
    Player player;
    player.recordKill();
    player.recordKill();
    assert(player.kills == 2);
    assert(player.score == 2.0f);
}

static void testJetHeatUsesActualJetting() {
    MovementInput input;
    input.jet = true;
    assert(Movement::isJetting(input, 1.0f));
    assert(!Movement::isJetting(input, 0.0f));
    input.jet = false;
    assert(!Movement::isJetting(input, 100.0f));
}

static void testMovementRejectsInvalidInput() {
    MovementState state;
    state.onGround = true;
    state.velocity = {2.0f, 0.0f, -1.0f};
    MovementInput input;
    input.forward = std::numeric_limits<float>::quiet_NaN();
    input.strafe = std::numeric_limits<float>::infinity();
    input.yaw = std::numeric_limits<float>::quiet_NaN();
    MovementEnvironment environment;
    Movement::step(state, input, environment, 1.0f / 60.0f);
    assert(std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
           std::isfinite(state.position.z));
    assert(std::isfinite(state.velocity.x) && std::isfinite(state.velocity.y) &&
           std::isfinite(state.velocity.z));

    const MovementState before = state;
    Movement::step(state, {}, environment, std::numeric_limits<float>::quiet_NaN());
    assert(state.position.x == before.position.x && state.position.y == before.position.y &&
           state.position.z == before.position.z);
}

static void testWheelStepOverflowParity() {
    assert(mouseWheelSteps(3) == 3);
    assert(mouseWheelSteps(-3) == 3);
    // INT_MIN has no representable positive magnitude; ignore it rather than
    // invoking signed overflow or starting a multi-billion-step cycle.
    assert(mouseWheelSteps(std::numeric_limits<int>::min()) ==
           0);
}

static void testMovementRejectsInvalidReplicatedState() {
    MovementState state;
    state.position = {NAN, 2.0f, INFINITY};
    state.velocity = {NAN, -1.0f, -INFINITY};
    state.energy = NAN;
    state.jumpDelay = INFINITY;
    state.landingSpeed = NAN;
    MovementEnvironment environment;
    Movement::step(state, {}, environment, 1.0f / 60.0f);
    assert(std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
           std::isfinite(state.position.z));
    assert(std::isfinite(state.velocity.x) && std::isfinite(state.velocity.y) &&
           std::isfinite(state.velocity.z));
    assert(std::isfinite(state.energy) && std::isfinite(state.jumpDelay) &&
           std::isfinite(state.landingSpeed));
}

static void testRayCastRejectsInvalidViewInput() {
    assert(!rayCastInputUsable({0.0f, 0.0f, 0.0f}, 100.0f));
    assert(!rayCastInputUsable({std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f}, 100.0f));
    assert(!rayCastInputUsable({0.0f, 0.0f, 1.0f}, std::numeric_limits<float>::infinity()));
    assert(!rayCastInputUsable({0.0f, 0.0f, 1.0f}, 0.0f));
    assert(rayCastInputUsable({0.0f, 0.0f, 1.0f}, 100.0f));
}

static void testWorldTickDeltaIsBounded() {
    assert(precipitationDelta(1.0f) == 0.05f);
    assert(precipitationDelta(0.0f) == 0.0f);
    assert(precipitationDelta(-1.0f) == 0.0f);
    assert(precipitationDelta(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

static void testDroppedFlagRejectsInvalidTime() {
    CtfRuntime::Flag flag;
    flag.team = 1;
    flag.state = CtfRuntime::FlagState::Dropped;
    flag.returnTimer = 10.0;
    assert(!CtfRuntime::tickFlag(flag, std::numeric_limits<double>::quiet_NaN()));
    assert(flag.state == CtfRuntime::FlagState::Dropped && flag.returnTimer == 10.0);
    assert(!CtfRuntime::tickFlag(flag, std::numeric_limits<double>::infinity()));
    assert(flag.state == CtfRuntime::FlagState::Dropped && flag.returnTimer == 10.0);
}

static void testDroppedFlagRecoversInvalidDeadline() {
    CtfRuntime::Flag flag;
    flag.team = 1;
    flag.state = CtfRuntime::FlagState::Dropped;
    flag.returnTimer = std::numeric_limits<double>::quiet_NaN();
    assert(!CtfRuntime::tickFlag(flag, 1.0));
    assert(flag.state == CtfRuntime::FlagState::Dropped);
    assert(flag.returnTimer == CtfRuntime::DroppedFlagReturnSeconds - 1.0);
    assert(CtfRuntime::tickFlag(flag, CtfRuntime::DroppedFlagReturnSeconds - 1.0));
    assert(flag.state == CtfRuntime::FlagState::Home && flag.carrier == -1);
}

static void testMissionFlagTeamNameIsCaseInsensitive() {
    assert(CtfRuntime::missionFlagTeam("Team2Flag") == 2);
    assert(CtfRuntime::missionFlagTeam("team2Flag") == 2);
    assert(CtfRuntime::missionFlagTeam("Team1Flag") == 1);
}

static void testItemPickupRejectsNonFiniteAmounts() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(!itemPickupWouldApply(ItemKind::Health, 50.0f, nan, 100.0f));
    assert(!itemPickupWouldApply(ItemKind::Energy, nan, 10.0f, 100.0f));
    assert(applyItemAmount(ItemKind::Health, 50.0f, nan, 100.0f) == 50.0f);
    assert(applyItemAmount(ItemKind::Ammo, 4.0f, 2.0f,
                           std::numeric_limits<float>::infinity()) == 4.0f);
}

static void testDamageRejectsNonFiniteAmounts() {
    assert(isValidDamageAmount(25.0f));
    assert(!isValidDamageAmount(std::numeric_limits<float>::quiet_NaN()));
    assert(!isValidDamageAmount(std::numeric_limits<float>::infinity()));
    assert(damageFlashForAmount(25.0f) == 0.25f);
    assert(damageFlashForAmount(200.0f) == 1.0f);
    assert(damageFlashForAmount(-1.0f) == 0.0f);
    assert(damageFlashForAmount(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

static void testPlayerStateRejectsNonFiniteMutations() {
    Player player;
    const Point3F initialPosition = player.position();
    const Point3F initialRotation = player.rotation();
    const Point3F initialVelocity = player.velocity();
    player.setHealth(75.0f);
    player.setEnergy(60.0f);
    player.setHeat(25.0f);
    player.setRepairRate(10.0f);
    player.setHealth(std::numeric_limits<float>::quiet_NaN());
    player.setEnergy(std::numeric_limits<float>::infinity());
    player.setHeat(-std::numeric_limits<float>::infinity());
    player.setRepairRate(std::numeric_limits<float>::quiet_NaN());
    player.setPosition({std::numeric_limits<float>::quiet_NaN(), 2.0f, 3.0f});
    player.setRotation({4.0f, std::numeric_limits<float>::infinity(), 6.0f});
    player.setVelocity({1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()});
    player.setJumpDelay(std::numeric_limits<float>::quiet_NaN());
    assert(player.health() == 75.0f && player.energy() == 60.0f &&
           player.heat() == 25.0f && player.getRepairRate() == 10.0f);
    assert(player.position().x == initialPosition.x &&
           player.position().y == initialPosition.y &&
           player.position().z == initialPosition.z);
    assert(player.rotation().x == initialRotation.x &&
           player.rotation().y == initialRotation.y &&
           player.rotation().z == initialRotation.z);
    assert(player.velocity().x == initialVelocity.x &&
           player.velocity().y == initialVelocity.y &&
           player.velocity().z == initialVelocity.z);
    player.setRotation({10.0f, 0.0f, 2.0f});
    assert(player.rotation().x == cameraPitchLimit && player.rotation().z == 2.0f);
}

static void testPlayerUsesAuthoredMaximumHealth() {
    Player player;
    player.setMaxHealth(150.0f);
    player.setHealth(150.0f);
    assert(player.maxHealth() == 150.0f && player.health() == 150.0f);
    // The local HUD uses the same authored maximum as remote health bars.
    assert(HudParity::resourceFraction(player.health(), player.maxHealth()) == 1.0f);
    player.setHealth(200.0f);
    assert(player.health() == 150.0f);
    player.setHealth(50.0f);
    assert(HudParity::resourceFraction(player.health(), player.maxHealth()) == 1.0f / 3.0f);
    player.setRepairRate(100.0f);
    player.update(1.0f);
    assert(player.health() == 150.0f);
    // A health pickup must remain eligible until the authored armor maximum,
    // not the stock 100-point cap.
    assert(itemPickupWouldApply(ItemKind::Health, 100.0f, 25.0f,
                                player.maxHealth()));
    player.setMaxHealth(100.0f);
    assert(player.health() == 100.0f);
}

static void testProjectileImpact() {
    V12BitWriter writer;
    writer.writeFlag(true); writer.writeUnsigned(77, 11);
    writer.writeFlag(false); writer.writeFlag(false);
    writer.writeUnsigned(0, 2);
    for (int i = 0; i < 3; ++i) { writer.writeFlag(false); writer.writeUnsigned(0, 15); }
    writer.writeUnsigned(0, 15); writer.writeUnsigned(0, 14);
    writer.writeFlag(false);
    std::vector<V12::ProjectileImpact> impacts;
    V12::PlayerGhostState state;
    auto payload = stream(writer);
    assert(V12::readGhostPayload(payload, 19, false, {}, &state, &impacts));
    assert(impacts.size() == 1 && impacts[0].hasDatablock && impacts[0].datablockId == 77);
}

static void testTerrainRaycastNormalParity() {
    const Point3F normal = terrainNormalFromHeights(0.0f, 2.0f, 0.0f, 0.0f,
                                                    1.0f);
    // A rising surface toward +X must push a contact toward -X, not report
    // every terrain ray as a horizontal floor.
    assert(normal.x < -0.7f);
    assert(normal.y > 0.6f);
}

static void testProjectileDatablockReferences() {
    V12BitWriter writer;
    writer.writeHuffmanString("disc");
    writer.writeUnsigned(0, 32); f32(writer, 0.5f); // emitterDelay, bubbleEmitTime
    writer.writeFlag(true); // faceViewer
    writer.writeFlag(false); // nonDefaultScale
    ref(writer, 11);
    for (int i = 0; i < 8; ++i) ref(writer, i == 2 ? 22 : (i == 0 ? 12 : 0));
    for (int i = 0; i < 6; ++i) ref(writer, i == 0 ? 71 : 0);
    writer.writeFlag(true); writer.writeUnsigned(128, 8);
    for (int i = 0; i < 3; ++i) writer.writeUnsigned(90, 7);
    writer.writeFlag(true);
    for (int i = 0; i < 3; ++i) writer.writeUnsigned(64, 7);
    writer.writeUnsigned(1, 8); f32(writer, 6.0f);

    V12::DecodedDataBlock decoded;
    auto payload = stream(writer);
    assert(V12::readDataBlockPayload(payload, 32, &decoded));
    assert(decoded.projectileBaseEmitterRef == 11);
    assert(decoded.projectileExplosionRef == 22 && decoded.projectileDelayEmitterRef == 12);
    assert(decoded.effectRefs.size() == 9 && decoded.effectRefs[3] == 22);
    assert(decoded.projectileDecalRefs.size() == 6 && decoded.projectileDecalRefs[0] == 71);
    assert(decoded.faceViewer && decoded.projectileHasLight &&
           decoded.projectileLightRadius > 9.9f && decoded.projectileLightRadius < 10.1f);
    assert(decoded.projectileExplodeOnWaterImpact && decoded.projectileDepthTolerance == 6.0f);
    assert(decoded.shapeFile == "disc");
    assert(!decoded.hasProjectileScale);

    V12BitWriter scaled;
    scaled.writeHuffmanString("shapes/bolt");
    scaled.writeUnsigned(0, 32); scaled.writeUnsigned(0, 32);
    scaled.writeFlag(false);
    scaled.writeFlag(true);
    f32(scaled, 2.0f); f32(scaled, 3.0f); f32(scaled, 4.0f);
    ref(scaled, 0);
    for (int i = 0; i < 8; ++i) ref(scaled, 0);
    for (int i = 0; i < 6; ++i) ref(scaled, 0);
    scaled.writeFlag(false); scaled.writeFlag(false);
    scaled.writeUnsigned(0, 8); f32(scaled, 0.0f);
    decoded = {};
    auto scaledStream = stream(scaled);
    assert(V12::readDataBlockPayload(scaledStream, 32, &decoded));
    assert(decoded.shapeFile == "shapes/bolt" && decoded.hasProjectileScale);
    assert(decoded.projectileScale.x == 2.0f && decoded.projectileScale.y == 3.0f &&
           decoded.projectileScale.z == 4.0f && !scaledStream.failed());

}

static void testProjectileVisualDefaults() {
    V12::DecodedDataBlock decoded;
    decoded.hasProjectileScale = true;
    decoded.projectileScale = {2.0f, 3.0f, 4.0f};
    assert(decoded.hasProjectileScale && decoded.projectileScale.x == 2.0f &&
           decoded.projectileScale.y == 3.0f && decoded.projectileScale.z == 4.0f);

    // Missing or invalid decoded scale must remain safe for render fallback.
    decoded = {};
    assert(!decoded.hasProjectileScale && decoded.projectileScale.x == 1.0f &&
           decoded.projectileScale.y == 1.0f && decoded.projectileScale.z == 1.0f);
}

static void testTerrainHoles() {
    TerrainBlock terrain;
    terrain.size = 4; terrain.squareSize = 1.0f; terrain.worldOffset = {0, 0, 4};
    assert(terrain.contains(0.0f, 4.0f));
    assert(terrain.contains(4.0f, 0.0f));
    assert(!terrain.contains(-0.01f, 4.0f));
    assert(!terrain.contains(0.0f, 4.01f));
    terrain.setEmptySquareRuns({2u | (1u << 8) | (3u << 16), 1u | (9u << 8) | (1u << 16)});
    assert(terrain.isEmptySquare(2.1f, 2.9f));
    assert(terrain.isEmptySquare(0.1f, 2.9f));
    assert(!terrain.isEmptySquare(1.1f, 2.9f));
    assert(!terrain.isEmptySquare(0.1f, 4.1f));
}

static void testTerrainSplitInterpolation() {
    TerrainBlock terrain;
    terrain.size = 2;
    terrain.squareSize = 1.0f;
    terrain.worldOffset = {0, 0, 2};
    terrain.heights = {0, 0, 0, 32};
    // Split45's lower-left triangle, not a bilinear blend (0.1875).
    assert(terrain.sampleHeight(0.25f, 1.25f) == 8.0f);

    terrain.size = 3;
    terrain.worldOffset = {0, 0, 3};
    terrain.heights.assign(9, 0.0f);
    terrain.heights[5] = 32.0f;
    // Cell (1,0) uses Split135 and the upper triangle contains the peak.
    assert(terrain.sampleHeight(1.75f, 2.25f) == 16.0f);

    // The final square wraps to sample column/row zero, as in Torque's
    // 256-square terrain block; it is not clamped to the penultimate sample.
    terrain.size = 2;
    terrain.worldOffset = {0, 0, 2};
    terrain.heights = {0, 0, 0, 32};
    assert(terrain.sampleHeight(1.75f, 0.25f) == 8.0f);
}

static void testSparseVehicleStateMerge() {
    V12::PlayerGhostState base;
    base.wheels[0].angularVelocity = 1.0f;
    base.wheels[0].valid = true;
    V12::PlayerGhostState update;
    update.velocity = {4.0f, 5.0f, 6.0f};
    update.hasVelocity = true;
    update.wheels[0] = {9.0f, 0.25f, -0.5f, true};
    const auto merged = V12::mergePlayerGhostState(base, update);
    assert(merged.hasVelocity && merged.velocity.x == 4.0f && merged.velocity.z == 6.0f);
    assert(merged.wheels[0].valid && merged.wheels[0].angularVelocity == 9.0f);
    assert(merged.wheels[0].suspension == 0.25f && merged.wheels[0].lateral == -0.5f);
}

static void testSparseAppearanceStateMerge() {
    V12::PlayerGhostState base;
    base.cloaked = true;
    base.hasCloak = true;
    base.shieldLevel = 0.75f;
    base.hasShield = true;
    V12::PlayerGhostState update;
    update.cloaked = false;
    update.hasCloak = true;
    update.shieldLevel = 0.25f;
    update.hasShield = true;
    update.threads[0] = {9, 2, 1.5f, 0.25f, false, true, true};
    update.soundThreads[0] = {321, true, true};
    const auto merged = V12::mergePlayerGhostState(base, update);
    assert(merged.hasCloak && !merged.cloaked);
    assert(merged.hasShield && merged.shieldLevel == 0.25f);
    assert(merged.threads[0].sequence == 9 && merged.threads[0].timescale == 1.5f &&
           merged.threads[0].position == 0.25f && !merged.threads[0].forward);
    assert(merged.soundThreads[0].playing && merged.soundThreads[0].profileId == 321);
}

static void testSparseDamageAndUnmountMerge() {
    V12::PlayerGhostState base;
    base.health = 10.0f;
    base.damageState = 1;
    base.hasHealth = true;
    base.hasDamageState = true;
    base.mountedImages[0] = {77, true, true, true};

    V12::PlayerGhostState update;
    update.health = 0.0f;
    update.damageState = 2;
    update.hasHealth = true;
    update.hasDamageState = true;
    update.mountedImages[0].valid = true;

    const auto merged = V12::mergePlayerGhostState(base, update);
    assert(merged.hasDamageState && merged.damageState == 2);
    assert(merged.mountedImages[0].valid && merged.mountedImages[0].datablockId == -1);

    base.maxHealth = 125.0f;
    base.hasMaxHealth = true;
    base.kills = 3; base.deaths = 2; base.score = 9; base.team = 1; base.hasStats = true;
    update = {};
    update.maxHealth = 150.0f; update.hasMaxHealth = true;
    update.kills = 4; update.deaths = 2; update.score = 12; update.team = 2;
    update.hasStats = true;
    const auto metadata = V12::mergePlayerGhostState(base, update);
    assert(metadata.hasMaxHealth && metadata.maxHealth == 150.0f);
    assert(metadata.hasStats && metadata.kills == 4 && metadata.deaths == 2 &&
           metadata.score == 12 && metadata.team == 2);
}

static void testVehicleStateMergeAndControl() {
    V12BitWriter writer;
    writer.writeFlag(false); // no game-base datablock
    writer.writeFlag(false); // no game-base mask
    writer.writeFlag(false); // no shape-base mask
    writer.writeFlag(true);  // jetting
    writer.writeFlag(true);  // controlled-object shortcut

    V12::PlayerGhostState state;
    auto payload = stream(writer);
    assert(V12::readGhostPayload(payload, 14, false, {}, &state));
    assert(state.hasVehicleState && state.hasJetting && state.jetting &&
           state.hasControlObject && state.controlObject);

    V12::PlayerGhostState base;
    base.energy = 25.0f;
    base.hasEnergy = true;
    base.braking = false;
    base.hasBraking = true;
    V12::PlayerGhostState update;
    update.energy = 75.0f;
    update.hasEnergy = true;
    update.braking = true;
    update.hasBraking = true;
    update.hasVehicleState = true;
    const auto merged = V12::mergePlayerGhostState(base, update);
    assert(merged.energy == 75.0f && merged.braking && merged.hasVehicleState);
}

static void testMountedImageWireOrder() {
    V12BitWriter writer;
    writer.writeFlag(false); // no game-base datablock
    writer.writeFlag(false); // no game-base mask
    writer.writeFlag(true);  // shape-base mask
    writer.writeFlag(false); // no damage
    writer.writeFlag(false); // no threads
    writer.writeFlag(false); // no sound threads
    writer.writeFlag(true);  // image mask
    writer.writeFlag(true);  // slot 0
    writer.writeFlag(false); // no image datablock
    writer.writeFlag(false); // no skin tag
    writer.writeFlag(false); // no script animation prefix
    writer.writeFlag(false); // animate all shapes
    writer.writeFlag(true);  // wet
    writer.writeFlag(false); // motion
    writer.writeFlag(false); // ammo
    writer.writeFlag(true);  // loaded
    writer.writeFlag(false); // target
    writer.writeFlag(false); // trigger down
    writer.writeFlag(false); // alt trigger down
    for (int i = 0; i < 8; ++i) writer.writeFlag(false); // generic triggers
    writer.writeUnsigned(3, 3); // fire count
    writer.writeUnsigned(0, 3); // alt fire count
    writer.writeUnsigned(0, 3); // reload count
    writer.writeFlag(true);  // firing
    writer.writeFlag(false); // alt firing
    writer.writeFlag(false); // reloading
    writer.writeFlag(true);  // initial image state
    for (int i = 1; i < 8; ++i) writer.writeFlag(false); // unused image slots
    writer.writeFlag(false); // no cloak/shield/invincible mask
    writer.writeFlag(false); // no mount update

    V12::PlayerGhostState state;
    auto payload = stream(writer);
    assert(V12::readGhostPayload(payload, 31, true, {}, &state));
    assert(state.mountedImages[0].valid);
    assert(state.mountedImages[0].loaded);
    assert(state.mountedImages[0].firing);
}

static void testShapeBaseV12OrderAndReset() {
    V12BitWriter writer;
    writer.writeFlag(false); // no game-base datablock
    writer.writeFlag(false); // no game-base target
    writer.writeFlag(true);  // ShapeBase mask
    writer.writeFlag(true);  // damage mask
    writer.writeUnsigned(10, 6);
    writer.writeUnsigned(2, 2);
    writer.writeFlag(true);  // whiteout
    writer.writeUnsigned(0, 9); writer.writeUnsigned(0, 8);
    writer.writeFlag(true);  // animation mask
    writer.writeFlag(true);  // animation slot 0
    writer.writeUnsigned(17, 5);
    writer.writeUnsigned(2, 2);
    f32(writer, 1.5f);
    f32(writer, 0.25f);
    writer.writeFlag(true);  // at end
    for (int i = 1; i < 4; ++i) writer.writeFlag(false);
    writer.writeFlag(true);  // sound mask
    writer.writeFlag(true);  // sound slot 0
    writer.writeFlag(true);  // playing
    writer.writeUnsigned(321, 11);
    for (int i = 1; i < 4; ++i) writer.writeFlag(false);
    writer.writeFlag(true);  // image mask
    writer.writeFlag(true);  // image slot 0
    writer.writeFlag(true); writer.writeUnsigned(77, 11);
    writer.writeFlag(false); // no skin tag
    writer.writeFlag(false); // no script animation prefix
    writer.writeFlag(false); // animate all shapes
    writer.writeFlag(true);  // wet
    writer.writeFlag(false); // motion
    writer.writeFlag(true);  // ammo
    writer.writeFlag(false); // loaded
    writer.writeFlag(false); // target
    writer.writeFlag(false); // trigger down
    writer.writeFlag(false); // alt trigger down
    for (int i = 0; i < 8; ++i) writer.writeFlag(false);
    writer.writeUnsigned(4, 3); writer.writeUnsigned(0, 3); writer.writeUnsigned(0, 3);
    writer.writeFlag(true); writer.writeFlag(false); writer.writeFlag(false);
    for (int i = 1; i < 8; ++i) writer.writeFlag(false);
    writer.writeFlag(true);  // cloak/shield/invincible mask
    writer.writeFlag(true);  // cloak update
    writer.writeFlag(true);  // cloaked
    writer.writeFlag(false); // cloak skin update
    writer.writeFlag(false); // no cloak fade
    writer.writeFlag(true);  // shield update
    writer.writeFlag(false); // shield normal, then level
    writer.writeUnsigned(0, 9); writer.writeUnsigned(0, 8);
    writer.writeUnsigned(13, 5);
    writer.writeFlag(false); // no invincible update
    writer.writeFlag(false); // no mount update
    writer.writeUnsigned(0x2a, 6); // alignment sentinel

    V12::PlayerGhostState state;
    auto payload = stream(writer);
    assert(V12::readGhostPayload(payload, 31, false, {}, &state));
    assert(state.hasHealth && state.hasMaxHealth && state.maxHealth == 100.0f &&
           state.damageState == 2);
    assert(state.soundThreads[0].valid && state.soundThreads[0].playing &&
           state.soundThreads[0].profileId == 321);
    assert(state.threads[0].sequence == 17 && state.threads[0].state == 2 &&
           state.threads[0].timescale == 1.5f && state.threads[0].position == 0.25f);
    assert(state.mountedImages[0].datablockId == 77 && state.mountedImages[0].firing);
    assert(state.hasCloak && state.cloaked && state.hasShield);
    assert(payload.readUnsigned(6) == 0x2a && !payload.failed());

    V12BitWriter reset;
    reset.writeFlag(false); reset.writeFlag(false); reset.writeFlag(true);
    for (int i = 0; i < 6; ++i) reset.writeFlag(false);
    reset.writeUnsigned(0x15, 5);
    V12::PlayerGhostState reused = state;
    auto resetPayload = stream(reset);
    assert(V12::readGhostPayload(resetPayload, 31, false, {}, &reused));
    assert(!reused.hasHealth && !reused.hasCloak && !reused.hasShield &&
           !reused.soundThreads[0].valid && !reused.threads[0].valid);
    assert(resetPayload.readUnsigned(5) == 0x15 && !resetPayload.failed());
}

static void testDemoClockMath() {
    assert(T2Demo::playbackBlockDuration(10.0f, 100) == 0.1f);
    assert(T2Demo::playbackBlockDuration(0.0f, 0) == 0.032f);
    assert(T2Demo::playbackBlockTime(0, 10.0f, 100) == 0.0f);
    assert(T2Demo::playbackBlockTime(25, 10.0f, 100) == 2.5f);
    assert(T2Demo::playbackBlockTime(100, 10.0f, 100) == 10.0f);
    assert(T2Demo::playbackTargetBlock(0.0f, 10.0f, 100) == 0);
    assert(T2Demo::playbackTargetBlock(0.099f, 10.0f, 100) == 0);
    assert(T2Demo::playbackTargetBlock(0.1f, 10.0f, 100) == 1);
    assert(T2Demo::playbackTargetBlock(100.0f, 10.0f, 100) == 100);

    DemoTimedEvent event;
    event.time = 2.5;
    assert(!demoEventVisibleAt(event, 2.49f));
    assert(demoEventVisibleAt(event, 2.5f));
}

static void testInputParity() {
    assert(mouseLeftButton == 1);
    assert(mouseMiddleButton == 2);
    assert(mouseRightButton == 3);
    assert(weaponSlotForScancode(30) == 0);
    assert(weaponSlotForScancode(38) == 8);
    assert(weaponSlotForScancode(39) == 9);
    assert(weaponSlotForScancode(29) == -1);
    assert(observerCyclePressed(false, false) == false);
    assert(observerCyclePressed(true, false) == true);
    assert(observerCyclePressed(false, true) == true);
    assert(buttonPressed(true, false));
    assert(!buttonPressed(true, true));
    assert(freeCameraYaw(1.0f, 0.25f) == 1.25f);
    assert(mouseWheelDelta(1.0f) == 1);
    assert(mouseWheelDelta(1.0f, true) == -1);
    assert(mouseWheelDelta(-2.0f, true) == 2);
}

static void testTextureFrameTiming() {
    const std::vector<float> durations{0.1f, 0.2f, 0.3f};
    assert(textureFrameIndex(durations, 3, 0.0f) == 0);
    assert(textureFrameIndex(durations, 3, 0.1f) == 1);
    assert(textureFrameIndex(durations, 3, 0.29f) == 1);
    assert(textureFrameIndex(durations, 3, 0.3f) == 2);
    assert(textureFrameIndex(durations, 3, 0.6f) == 0);
    // Missing durations use one second per frame, matching the IFL default.
    assert(textureFrameIndex({}, 3, 0.5f) == 0);
}

static void testDemoCameraMath() {
    const float vertical = T2Demo::horizontalFovToVertical(90.0f, 16.0f / 9.0f);
    assert(vertical > 58.7f && vertical < 58.8f);
    assert(T2Demo::horizontalFovToVertical(90.0f, 0.0f) > 73.7f);
    assert(std::fabs(Math::verticalFovToHorizontal(vertical, 16.0f / 9.0f) - 90.0f) < 0.0001f);
    assert(std::fabs(Math::horizontalFovToVertical(90.0f, NAN) -
                     Math::horizontalFovToVertical(90.0f, 4.0f / 3.0f)) < 0.0001f);
    assert(std::fabs(Math::horizontalFovToVertical(NAN, 16.0f / 9.0f) - vertical) < 0.0001f);

    const Vec3 amplitude{2.0f, 3.0f, 4.0f};
    const Vec3 frequency{1.0f, 2.0f, 3.0f};
    const Vec3 phase{0.0f, 0.25f, 0.5f};
    const Vec3 first = T2Demo::cameraShakeOffset(0.125f, amplitude, frequency, phase);
    const Vec3 second = T2Demo::cameraShakeOffset(0.125f, amplitude, frequency, phase);
    assert(first.x == second.x && first.y == second.y && first.z == second.z);
    assert(first.x > 1.4f && first.x < 1.5f);

    const Vec3 forward = T2Demo::cameraDirectionFromYawPitch(0.5f, 0.25f);
    assert(std::fabs(forward.x + std::sin(0.5f) * std::cos(0.25f)) < 0.0001f);
    assert(std::fabs(forward.y - std::cos(0.5f) * std::cos(0.25f)) < 0.0001f);
    assert(std::fabs(forward.z - std::sin(0.25f)) < 0.0001f);
}

static void testDemoGhostHudCoordinateParity() {
    // HUD labels must use the same coordinate basis as rendered demo ghosts.
    const Point3F renderPosition = Math::torquePointToYUp({1.0f, 2.0f, 3.0f});
    assert(renderPosition.x == 1.0f && renderPosition.y == 3.0f &&
           renderPosition.z == -2.0f);
}

static void testDemoAudioIdentity() {
    NetEventInfo event;
    event.classId = T2Demo::NetEventClassFirst + 18;
    event.audioProfileId = 12;
    event.hasAudioPosition = true;
    event.audioPosition = {1.0f, 2.0f, 3.0f};
    assert(demoAudioEventKey(4, 0, event) != demoAudioEventKey(5, 0, event));
    assert(demoAudioEventKey(4, 0, event) != demoAudioEventKey(4, 1, event));
    auto moved = event;
    moved.audioPosition.x += 1.0f;
    assert(demoAudioEventKey(4, 0, event) != demoAudioEventKey(4, 0, moved));
}

static void testLiveAudioEventPayload() {
    V12::ServerEvent event;
    event.hasAudio = true;
    event.audioProfileId = 12;
    event.audioHasPosition = true;
    event.audioPosition = {1, 2, 3};
    assert(event.hasAudio && event.audioProfileId == 12 &&
           event.audioPosition.z == 3.0f);
}

static void testLiveEventOrderingAndPayloads() {
    V12BitWriter writer;
    writer.writeFlag(true); // unguaranteed event list
    V12::EventHeader simple{false, false, 22, 0};
    V12::writeEventHeader(writer, false, simple);
    writer.writeHuffmanString("live chat");
    writer.writeFlag(true);
    V12::EventHeader info{false, false, 24, 0};
    V12::writeEventHeader(writer, false, info);
    writer.writeUnsigned(9, 9);
    writer.writeFlag(true); writer.writeFlag(false); // inline name is absent
    for (int i = 0; i < 4; ++i) writer.writeFlag(false);
    for (int i = 0; i < 4; ++i) writer.writeFlag(false);
    writer.writeFlag(true); // next event
    V12::EventHeader targetTo{false, false, 25, 0};
    V12::writeEventHeader(writer, false, targetTo);
    writer.writeFlag(true); writer.writeUnsigned(9, 9);
    writer.writeFlag(true); f32(writer, 1.0f); f32(writer, 2.0f); f32(writer, 3.0f);
    writer.writeFlag(true);
    writer.writeFlag(true); // next event
    V12::EventHeader colors{false, false, 12, 0};
    V12::writeEventHeader(writer, false, colors);
    writer.writeUnsigned(3, 5); writer.writeUnsigned(1u << 2, 32);
    writer.writeFlag(true); writer.writeUnsigned(0x44332211u, 32);
    writer.writeFlag(true);
    V12::EventHeader group{false, false, 15, 0};
    V12::writeEventHeader(writer, false, group);
    writer.writeUnsigned(7, 5);
    writer.writeFlag(true);
    V12::EventHeader audio{false, false, 17, 0};
    V12::writeEventHeader(writer, false, audio);
    writer.writeUnsigned(77, 11);
    writer.writeFlag(false);
    writer.writeFlag(false); // end unguaranteed and guaranteed lists

    V12::NetStringTable strings;
    std::vector<V12::ServerEvent> events;
    auto input = stream(writer);
    assert(V12::readServerEvents(input, strings, events));
    assert(events.size() == 6);
    assert(events[0].classId == 22 && events[0].message == "live chat");
    assert(events[1].hasTargetInfo && events[1].targetInfo.targetId == 9 &&
           !events[1].targetInfo.hasSensorGroup && !events[1].targetInfo.hasRenderFlags);
    assert(events[2].hasTargetTo && events[2].targetToHasTarget &&
           events[2].targetToHasPosition && events[2].targetToAssign &&
           events[2].targetToPosition.z == 3.0f);
    assert(events[3].hasSensorGroupColor && events[3].sensorColorGroup == 3 &&
           events[3].sensorColors[2] == 0x44332211u);
    assert(events[4].hasSensorGroup && events[4].sensorGroup == 7);
    assert(events[5].hasAudio && events[5].audioProfileId == 77);
}

static void testInteriorOutsideAndPortalRules() {
    const DTSShape::InteriorPlane plane{{1, 0, 0}, 0};
    assert(interiorPortalAllowsTraversal(0, 0, 1, 0, {1, 0, 0}, plane));
    assert(!interiorPortalAllowsTraversal(0, 0, 1, 0, {-1, 0, 0}, plane));
    assert(interiorPortalAllowsTraversal(0x8000, 0, 1, 0, {-1, 0, 0}, plane));
    assert(!interiorPortalAllowsTraversal(0, 2, 2, 2, {0, 0, 0}, plane));
}

static void testInteriorCollisionSelectionAndContacts() {
    CollisionMesh mesh;
    const float vertices[] = {
        -2, 0, -2,  2, 0, -2,  2, 0, 2,
        -2, 10, -2, 2, 10, 2, -2, 10, 2,
        0, 0, -2, 0, 2, -2, 0, 0, 2
    };
    const uint32_t indices[] = {0, 1, 2, 3, 5, 4, 6, 7, 8};
    mesh.addMesh(vertices, 27, indices, 9);
    mesh.build();

    // A floor query must not select the interior ceiling above the actor.
    assert(mesh.getHeight(0, 0) == 10.0f);
    assert(mesh.getFloorHeight(0, 5, 0) > -0.01f && mesh.getFloorHeight(0, 5, 0) < 0.01f);

    // Starting outside the grid must still enter it and find the wall.
    float t = 0.0f;
    Point3F hit{}, normal{};
    assert(mesh.raycast({-600, 1, 0}, {1, 0, 0}, 700, t, hit, normal));
    assert(t > 599.0f && t < 601.0f);

    // Player-sized contacts against an edge/vertex are solid, not just
    // contacts whose projection falls inside the triangle.
    Point3F push{};
    assert(mesh.sphereCollide({0.25f, 0.3f, 0}, 0.5f, push));
    assert(push.x > 0.0f);

    CollisionMesh room;
    const float roomVertices[] = {
        -2, 0, -2,  2, 0, -2,  0, 0, 2,
        -2, 10, -2, 2, 10, -2, 0, 10, 2
    };
    const uint32_t roomIndices[] = {0, 2, 1, 3, 4, 5};
    room.addMesh(roomVertices, 18, roomIndices, 6);
    room.build();
    // A floor query above a room must skip the downward-facing ceiling.
    assert(room.getFloorHeight(0, 15, 0) > -0.01f &&
           room.getFloorHeight(0, 15, 0) < 0.01f);
}

static void testLineOfSightEndpoint() {
    CollisionMesh mesh;
    const float vertices[] = {
        1.05f, -1.0f, -1.0f,  1.05f, 1.0f, -1.0f,  1.05f, 1.0f, 1.0f,
    };
    const uint32_t indices[] = {0, 1, 2};
    mesh.addMesh(vertices, 9, indices, 3);
    mesh.build();

    assert(mesh.lineOfSight({0, 0, 0}, {1, 0, 0}));
    assert(!mesh.lineOfSight({0, 0, 0}, {2, 0, 0}));
}

static void testMovementParity() {
    assert(Movement::fallDamage(12.0f) == 0.0f);
    assert(Movement::fallDamage(10.0f) == 0.0f);
    assert(Movement::fallDamage(17.0f) == 20.0f);
    assert(Movement::fallDamage(100.0f) == 100.0f);
    assert(Movement::healthAfterLanding(100.0f, false, true, 17.0f) == 80.0f);
    assert(Movement::healthAfterLanding(100.0f, true, true, 17.0f) == 100.0f);
    assert(Movement::healthAfterLanding(100.0f, false, false, 17.0f) == 100.0f);

    MovementState slopeContact{{0, 0.3f, 0}, {0, 0, 0}, 100, false, false};
    MovementEnvironment slopeFloor;
    slopeFloor.floorY = 0.0f;
    slopeFloor.floorNormal = {0.0f, 0.8f, 0.6f};
    slopeFloor.collisionRadius = 0.5f;
    assert(std::fabs(Movement::contactHeight(0.0f, 0.5f,
                                             slopeFloor.floorNormal) - 0.4f) < 1.0e-5f);
    assert(!Movement::isGroundedAtFloor(0.2f, 0.0f, 0.5f, {0.0f, 0.4f, 0.9f}));
    Movement::step(slopeContact, {}, slopeFloor, 1.0f / 60.0f);
    // The center rests 0.4 units above an 0.8-upward ramp, not 0.5.
    assert(std::fabs(slopeContact.position.y - 0.4f) < 1.0e-5f);
    assert(slopeContact.onGround);

    MovementEnvironment floor{0.0f, {0, 1, 0}, false};
    MovementState paused{{1, 2, 3}, {4, 5, 6}, 73, false, true};
    MovementState pausedBefore = paused;
    MovementEnvironment pausedZone = floor;
    pausedZone.velocityMod = 0.5f;
    Movement::step(paused, {}, pausedZone, 0.0f);
    assert(paused.position.x == pausedBefore.position.x &&
           paused.position.y == pausedBefore.position.y &&
           paused.position.z == pausedBefore.position.z);
    assert(paused.velocity.x == pausedBefore.velocity.x &&
           paused.velocity.y == pausedBefore.velocity.y &&
           paused.velocity.z == pausedBefore.velocity.z &&
           paused.energy == pausedBefore.energy &&
           paused.onGround == pausedBefore.onGround &&
           paused.jumpWasDown == pausedBefore.jumpWasDown);

    MovementState accelerated{{0, 0, 0}, {0, 0, 0}, 100, true, false};
    MovementInput forward; forward.forward = 1.0f;
    Movement::step(accelerated, forward, floor, 1.0f / 60.0f);
    assert(accelerated.velocity.z > 0.9f && accelerated.velocity.z < 1.1f);
    for (int i = 0; i < 120; ++i) Movement::step(accelerated, {}, floor, 1.0f / 60.0f);
    assert(accelerated.velocity.z < 0.01f);

    MovementState jumped{{0, 0.5f, 0}, {0, 0, 0}, 100, true, false};
    MovementInput jump; jump.jump = true;
    Movement::step(jumped, jump, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y > 9.5f && !jumped.onGround);
    const float jumpVelocity = jumped.velocity.y;
    Movement::step(jumped, jump, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y < jumpVelocity); // held jump does not retrigger in air
    MovementState held{{0, 0.5f, 0}, {0, 0, 0}, 100, true, false};
    int heldJumps = 0;
    for (int i = 0; i < 360; ++i) {
        const bool wasGrounded = held.onGround;
        Movement::step(held, jump, floor, 1.0f / 60.0f);
        if (wasGrounded && !held.onGround && held.velocity.y > 9.0f)
            ++heldJumps;
    }
    assert(heldJumps >= 2); // held jump fires again after the native delay.
    MovementInput jet; jet.jet = true;
    const float energy = jumped.energy;
    Movement::step(jumped, jet, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y > jumpVelocity - 0.5f && jumped.energy < energy);

    MovementState falling{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    Movement::step(falling, {}, floor, 1.0f / 60.0f);
    assert(falling.velocity.y < 0.0f);

    // Contact resolution clears vertical velocity, but landing damage must use
    // the impact speed from the same tick rather than the previous tick.
    MovementState hardLanding{{0, 0.1f, 0}, {0, -20.0f, 0}, 100, false, false};
    Movement::step(hardLanding, {}, floor, 1.0f / 60.0f);
    assert(hardLanding.onGround && hardLanding.velocity.y == 0.0f);
    assert(hardLanding.position.y == 0.5f);
    assert(hardLanding.landingSpeed > 20.0f - 0.5f);
    assert(Movement::fallDamage(hardLanding.landingSpeed) > 30.0f);

    // Jump cooldown elapses while airborne; landing must not add another
    // nearly-one-second wait before the next jump.
    MovementState landed{{0, 0.5f, 0}, {0, 0, 0}, 100, true, false};
    Movement::step(landed, jump, floor, 1.0f / 60.0f);
    for (int i = 0; i < 120; ++i) Movement::step(landed, {}, floor, 1.0f / 60.0f);
    assert(landed.onGround && landed.jumpDelay == 0.0f);
    Movement::step(landed, jump, floor, 1.0f / 60.0f);
    assert(!landed.onGround && landed.velocity.y > 9.5f);

    MovementState swimming{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    Movement::step(swimming, {}, {0.0f, {0, 1, 0}, true}, 1.0f / 60.0f);
    assert(swimming.velocity.y > falling.velocity.y);

    MovementEnvironment zone{0.0f, {0, 1, 0}, false, 0.5f, 2.0f, {4, 0, 0}};
    MovementState zoned{{0, 2, 0}, {2, 0, 0}, 100, false, false};
    Movement::step(zoned, {}, zone, 1.0f / 60.0f);
    assert(zoned.velocity.x > 0.9f && zoned.velocity.x < 1.1f);
    assert(zoned.velocity.y < -0.6f && zoned.velocity.y > -0.8f);

    // Malformed PhysicalZone fields must not turn a player state into NaN.
    MovementEnvironment invalidZone = floor;
    invalidZone.velocityMod = NAN;
    invalidZone.gravityMod = NAN;
    invalidZone.gravity = NAN;
    invalidZone.appliedForce = {NAN, 4.0f, NAN};
    MovementState safe{{0, 2, 0}, {1, 0, 0}, 100, false, false};
    Movement::step(safe, {}, invalidZone, 1.0f / 60.0f);
    assert(std::isfinite(safe.position.x) && std::isfinite(safe.position.y) &&
           std::isfinite(safe.position.z));
    assert(std::isfinite(safe.velocity.x) && std::isfinite(safe.velocity.y) &&
           std::isfinite(safe.velocity.z));
    assert(safe.velocity.y < 0.0f);

    Point3F wallVelocity{3, 0, -2};
    Movement::projectOnPlane(wallVelocity, {1, 0, 0});
    assert(wallVelocity.x == 3.0f); // separating velocity is preserved
    wallVelocity = {-3, 0, -2};
    Movement::projectOnPlane(wallVelocity, {1, 0, 0});
    assert(wallVelocity.x == 0.0f && wallVelocity.z == -2.0f);
    Point3F slopeVelocity{0, -1, 2};
    Movement::projectOnPlane(slopeVelocity, {0, 0.7071067f, 0.7071067f});
    assert(slopeVelocity.y < 0.0f && slopeVelocity.z > 0.0f);
    MovementState ramp{{0, 0.35f, 0}, {0, -2, 1}, 100, true, false};
    MovementEnvironment rampFloor{0.0f, {0, 0.7071067f, 0.7071067f}, false};
    Movement::step(ramp, {}, rampFloor, 1.0f / 60.0f);
    assert(ramp.velocity.y == 0.0f && ramp.velocity.z > 0.0f);
    assert(ramp.velocity.y + ramp.velocity.z > -0.001f);

    MovementEnvironment tuned = floor;
    tuned.maxGroundSpeed = 6.0f;
    tuned.jumpSpeed = 4.0f;
    tuned.jetAcceleration = 12.0f;
    MovementState tunedRun{{0, 0.5f, 0}, {0, 0, 0}, 100, true, false};
    MovementInput tunedForward; tunedForward.forward = 1.0f;
    Movement::step(tunedRun, tunedForward, tuned, 1.0f / 60.0f);
    assert(tunedRun.velocity.z > 0.9f && tunedRun.velocity.z < 1.1f);
    MovementState tunedJump{{0, 0.5f, 0}, {0, 0, 0}, 100, true, false};
    MovementInput tunedJumpInput; tunedJumpInput.jump = true;
    Movement::step(tunedJump, tunedJumpInput, tuned, 1.0f / 60.0f);
    assert(tunedJump.velocity.y > 3.6f && tunedJump.velocity.y < 4.0f);

    // PlayerData::maxEnergy changes the cap used by both recharge and the HUD
    // path; custom armor must not silently fall back to the stock 100 units.
    MovementEnvironment highEnergy = floor;
    highEnergy.maxEnergy = 150.0f;
    MovementState recharging{{0, 0.5f, 0}, {0, 0, 0}, 149.0f, true, false};
    for (int i = 0; i < 20; ++i) Movement::step(recharging, {}, highEnergy, 1.0f);
    assert(recharging.energy == 150.0f);
    // Interior sphere resolution places the player at the normal-relative
    // contact height; a horizontal floor still uses the full radius.
    assert(Movement::isGroundedAtFloor(0.5f, 0.0f, 0.5f, {0, 1, 0}));
    assert(!Movement::isGroundedAtFloor(0.7f, 0.0f, 0.5f, {0, 1, 0}));
    assert(!Movement::isGroundedAtFloor(0.5f, 0.0f, 0.5f, {0, 0.4f, 0}));

    MovementState authoritative{{0, 0, 0}, {0, 0, 0}, 100, true, false};
    MovementState replay = authoritative;
    for (int i = 0; i < 30; ++i) Movement::step(authoritative, forward, floor, 1.0f / 60.0f);
    for (int i = 0; i < 10; ++i) Movement::step(replay, forward, floor, 1.0f / 60.0f);
    for (int i = 10; i < 30; ++i) Movement::step(replay, forward, floor, 1.0f / 60.0f);
    assert(replay.position.x == authoritative.position.x && replay.position.z == authoritative.position.z);
    assert(replay.energy == authoritative.energy);

    MovementState customGravity{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    MovementState defaultGravity{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    Movement::step(defaultGravity, {}, floor, 1.0f / 60.0f);
    assert(defaultGravity.velocity.y < 0.0f && defaultGravity.velocity.y > -0.35f);
    MovementEnvironment lowGravity = floor;
    lowGravity.gravity = -5.0f;
    Movement::step(customGravity, {}, lowGravity, 1.0f / 60.0f);
    assert(customGravity.velocity.y < 0.0f && customGravity.velocity.y > -0.1f);

    MovementState invertedGravity{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    MovementEnvironment inverted = floor;
    inverted.gravityMod = -1.0f;
    Movement::step(invertedGravity, {}, inverted, 1.0f / 60.0f);
    assert(invertedGravity.velocity.y > 0.0f);

    MovementState reversedVelocity{{0, 10, 0}, {4, 0, -2}, 100, false, false};
    MovementEnvironment conveyor = floor;
    conveyor.velocityMod = -1.0f;
    Movement::step(reversedVelocity, {}, conveyor, 1.0f / 60.0f);
    assert(reversedVelocity.velocity.x < 0.0f && reversedVelocity.velocity.z > 0.0f);

    const Point3F terrainNormal = terrainNormalFromHeights(0.0f, 2.0f, 0.0f, 0.0f);
    assert(terrainNormal.x < 0.0f && terrainNormal.y > 0.0f &&
           terrainNormal.y < 1.0f);
}

static void testSlowProjectileSweepParity() {
    assert(projectileHasSweepLength(0.00001f));
    assert(!projectileHasSweepLength(0.0f));
    assert(!projectileHasSweepLength(-0.01f));
    assert(!projectileHasSweepLength(std::numeric_limits<float>::quiet_NaN()));
}

static void testCtfFlagReturnClearsCarrier() {
    CtfRuntime::Flag flag{1, CtfRuntime::FlagState::Dropped, 23};
    assert(CtfRuntime::returnHome(flag, 1));
    assert(flag.state == CtfRuntime::FlagState::Home && flag.carrier == -1);

    // Replicated dropped flags do not carry the local return countdown.  They
    // still need the native thirty-second grace period rather than returning
    // immediately when the first server tick arrives.
    CtfRuntime::Flag replicated{1, CtfRuntime::FlagState::Dropped, -1, 0.0};
    assert(!CtfRuntime::tickFlag(replicated, 1.0));
    assert(replicated.state == CtfRuntime::FlagState::Dropped);
    assert(replicated.returnTimer == 29.0);
    assert(CtfRuntime::tickFlag(replicated, 29.0));
    assert(replicated.state == CtfRuntime::FlagState::Home);
}

static void testCtfStatusParsingIsLocaleTolerant() {
    using CtfRuntime::StatusToken;
    assert(CtfRuntime::classifyStatusToken("<At Base>") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken(" at   base ") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("Home") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("<BASE>") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("Returned") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("return") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("<IN THE FIELD>") == StatusToken::Field);
    assert(CtfRuntime::classifyStatusToken("Field") == StatusToken::Field);
    assert(CtfRuntime::classifyStatusToken("Dropped") == StatusToken::Field);
    assert(CtfRuntime::classifyStatusToken("Flag at Base") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("Your flag returned") == StatusToken::Home);
    assert(CtfRuntime::classifyStatusToken("Your flag is in the field") == StatusToken::Field);
    assert(CtfRuntime::classifyStatusToken("PlayerOne") == StatusToken::Held);
}

static void testProjectileSlopeBounceParity() {
    Point3F velocity{0.0f, -10.0f, 2.0f};
    reflectProjectileVelocity(velocity, {0.0f, 0.7071067f, 0.7071067f});
    // A ramp redirects part of the downward velocity along the slope.
    assert(velocity.y < 0.0f);
    assert(velocity.z > 0.0f);
    assert(std::fabs(velocity.y + 4.0f) < 0.01f);
    assert(std::fabs(velocity.z - 8.0f) < 0.01f);
}

static void testWaterVolumeParity() {
    assert(waterBodySubmergesCamera(2.0f, 3.0f, 1));
    assert(!waterBodySubmergesCamera(2.0f, 3.0f, 4));
    assert(!waterBodySubmergesCamera(2.0f, 3.0f, -1));
    assert(!waterBodySubmergesCamera(3.0f, 3.0f, 1));
    assert(activeWaterBodySubmergesCamera(true, 2.0f, 3.0f, 1));
    assert(!activeWaterBodySubmergesCamera(false, -100.0f, 100.0f, 1));
    // The material alpha is the authored WaterBlock opacity, not a second
    // factor to multiply into it.
    assert(waterSurfaceOpacity(0.5f) == 0.5f);
    assert(waterSurfaceOpacity(2.0f) == 1.0f);
    assert(waterSurfaceOpacity(std::numeric_limits<float>::quiet_NaN()) == 0.5f);
    assert(waterSurfaceOpacity(std::numeric_limits<float>::infinity()) == 0.5f);
    assert(waterShoreTextureActive(true, true, 1.0f));
    assert(!waterShoreTextureActive(true, false, 1.0f));
    assert(!waterShoreTextureActive(false, true, 1.0f));
    assert(!waterShoreTextureActive(true, true, 0.0f));
    assert(waterBodyCanAffectSurface(true, 1));
    assert(!waterBodyCanAffectSurface(false, 1));
    // Lava and quicksand are authored WaterBlocks with visible surfaces, but
    // they intentionally do not apply the normal underwater camera filter.
    assert(waterBodyCanAffectSurface(true, 4));
    assert(waterBodyCanAffectSurface(true, 7));
    assert(!waterBodyCanAffectSurface(true, -1));
    // An overlapping higher lava volume must not suppress the normal-water
    // camera filter from a lower volume.
    assert(waterBodySubmergesCamera(1.0f, 2.0f, 1));
    assert(!waterBodySubmergesCamera(1.0f, 3.0f, 4));
    assert(waterSurfaceLevel(2.0f, true, 1, 5.0f) == 5.0f);
    assert(waterSurfaceLevel(2.0f, true, 4, 100.0f) == 100.0f);
    assert(waterSurfaceLevel(2.0f, false, 1, 100.0f) == 2.0f);
}

static void testWaterBoundsParity() {
    // A WaterBlock authored at y=10 with a 20-unit y extent covers
    // renderer z=-30..-10 after the Z-up to Y-up conversion.
    assert(waterOriginZ(10.0f, 20.0f) == -30.0f);
    assert(waterOriginZ(-4.0f, 8.0f) == -4.0f);
    // A below-origin WaterBlock must remain below origin; using zero as the
    // accumulator would incorrectly report it as a surface at y=0.
    assert(waterSurfaceLevel(-100.0f, true, 1, -5.0f) == -5.0f);
}

static void testWaterRenderDistanceParity() {
    assert(waterQuadWithinRenderDistance(399.0f, 1000.0f));
    assert(waterQuadWithinRenderDistance(999.0f, 1000.0f));
    assert(waterQuadWithinRenderDistance(1000.0f, 1000.0f));
    assert(!waterQuadWithinRenderDistance(1000.1f, 1000.0f));
    assert(!waterQuadWithinRenderDistance(-1.0f, 1000.0f));
    assert(!waterQuadWithinRenderDistance(10.0f, -1.0f));
    assert(!waterQuadWithinRenderDistance(
        std::numeric_limits<float>::quiet_NaN(), 1000.0f));
}

static void testPrecipitationEnableRestoresCoverage() {
    World world;
    assert(world.setPrecipitation(3, 0.25f));
    assert(world.precipitation.active && world.precipitation.percentage == 0.25f);
    assert(world.setPrecipitationEnabled(false));
    assert(!world.precipitation.active && world.precipitation.percentage == 0.0f);
    assert(world.setPrecipitationEnabled(true));
    assert(world.precipitation.active && world.precipitation.percentage == 0.25f);
}

static void testProjectileDirectHitParity() {
    float hitT = 0.0f;
    assert(segmentSphereHit({0, 0, 0}, {10, 0, 0}, {5, 0, 0}, 1.0f, hitT));
    assert(hitT > 0.39f && hitT < 0.41f);
    assert(!segmentSphereHit({0, 0, 0}, {10, 0, 0}, {5, 2.1f, 0}, 1.0f, hitT));
    assert(!segmentSphereHit({0, 0, 0}, {10, 0, 0}, {5, 0, 0}, -1.0f, hitT));
    assert(segmentSphereHit({5, 0, 0}, {5, 0, 0}, {5, 0, 0}, 1.0f, hitT));
    assert(hitT == 0.0f);
    // A fast target sweep may begin within the hit volume; impact is immediate,
    // rather than being reported at the sphere's exit point.
    assert(segmentSphereHit({4.5f, 0, 0}, {10, 0, 0}, {5, 0, 0}, 1.0f, hitT));
    assert(hitT == 0.0f);
    assert(segmentSurfaceCrossing(10.0f, 0.0f, -10.0f, 0.0f, hitT));
    assert(hitT > 0.49f && hitT < 0.51f);
    assert(!segmentSurfaceCrossing(1.0f, 0.0f, 2.0f, 0.0f, hitT));
    // A projectile already intersecting terrain must still impact when its
    // velocity points upward, rather than escaping through the surface.
    assert(projectileTouchesSurface(-0.01f, 0.0f));
    assert(!projectileTouchesSurface(0.01f, 0.0f));
    assert(projectileExitsSurface(0.0f, 1.0f, 2.0f, 1.5f));
    assert(projectileExitsSurface(1.0f, 1.0f, 2.0f, 1.5f));
    assert(!projectileExitsSurface(1.1f, 1.0f, 2.0f, 1.5f));
}

static void testProjectileTerrainCrossingParity() {
    float hitT = 0.0f;
    // Uphill terrain can intercept a projectile that is moving upward.
    assert(segmentSurfaceCrossing(5.0f, 0.0f, 6.0f, 8.0f, hitT));
    assert(hitT > 0.7f && hitT < 0.8f);
}

static void testProjectileGravityParity() {
    assert(projectileGravityAcceleration(-20.0f, 1.0f) == -20.0f);
    assert(projectileGravityAcceleration(-5.0f, 0.5f) == -2.5f);
    // PhysicalZone gravityMod is signed, matching player movement.
    assert(projectileGravityAcceleration(-20.0f, -1.0f) == 20.0f);
    const Point3F force = projectileZoneAcceleration(-20.0f, 0.5f, {3.0f, 4.0f, -2.0f});
    assert(force.x == 3.0f && force.y == -6.0f && force.z == -2.0f);
    const Point3F malformed = projectileZoneAcceleration(NAN, NAN, {NAN, 4.0f, INFINITY});
    assert(std::isfinite(malformed.x) && std::isfinite(malformed.y) &&
           std::isfinite(malformed.z) && malformed.x == 0.0f &&
           malformed.y == 4.0f && malformed.z == 0.0f);
}

static float testGroundSample(float, float, void* context) {
    return *static_cast<float*>(context);
}

static void testServerGroundHeightParity() {
    float authoredHeight = -5.0f;
    assert(serverGroundHeight(2.0f, testGroundSample, &authoredHeight, 0, 0) == -5.0f);
    authoredHeight = -1.0e10f;
    assert(serverGroundHeight(2.0f, testGroundSample, &authoredHeight, 0, 0) == 2.0f);
    assert(serverGroundHeight(2.0f, nullptr, nullptr, 0, 0) == 2.0f);
}

static void testItemPickupParity() {
    assert(itemWithinPickupRange(2.0f));
    assert(!itemWithinPickupRange(2.001f));
    assert(!itemWithinPickupRange(NAN));
    const Point3F itemPosition = itemWorldPosition({2.0f, 3.0f, 7.0f});
    assert(itemPosition.x == 2.0f && itemPosition.y == 7.0f && itemPosition.z == -3.0f);
    assert(classifyItemKind("HealthKit") == ItemKind::Health);
    assert(classifyItemKind("EnergyPack") == ItemKind::Energy);
    assert(classifyItemKind("AmmoPack") == ItemKind::Ammo);
    assert(classifyItemKind("Flag") == ItemKind::None);
    assert(applyItemAmount(ItemKind::Health, 90.0f, 25.0f, 100.0f) == 100.0f);
    assert(applyItemAmount(ItemKind::Energy, 10.0f, 25.0f, 100.0f) == 35.0f);
    assert(applyItemAmount(ItemKind::Ammo, 4.0f, 15.0f, 0.0f) == 19.0f);
    assert(applyItemAmount(ItemKind::Ammo, -1.0f, 15.0f, 0.0f) == -1.0f);
    assert(applyItemAmount(ItemKind::Ammo, 35.0f, 15.0f, 40.0f) == 40.0f);
    assert(itemPickupWouldApply(ItemKind::Health, 75.0f, 25.0f, 100.0f));
    assert(!itemPickupWouldApply(ItemKind::Health, 100.0f, 25.0f, 100.0f));
    assert(!itemPickupWouldApply(ItemKind::Energy, 100.0f, 10.0f, 100.0f));
    assert(!itemPickupWouldApply(ItemKind::Ammo, 40.0f, 15.0f, 40.0f));
    assert(!itemPickupWouldApply(ItemKind::Ammo, -1.0f, 15.0f, 0.0f));
    assert(itemRespawnReady(std::numeric_limits<float>::quiet_NaN(), 0.1f));
    assert(itemRespawnReady(0.1f, 0.1f));
    assert(!itemRespawnReady(0.2f, 0.1f));
}

static void testWeaponSelectionAndStateParity() {
    const WeaponData infiniteAmmo{"", ProjectileType::Bolt, 0, 0, 0, 0, 0, -1, 0, false, false, nullptr, nullptr};
    const WeaponData finiteAmmo{"", ProjectileType::Bolt, 0, 1.0f, 0, 0, 0, 40, 0, false, false, nullptr, nullptr};
    assert(weaponInitialAmmo(infiniteAmmo) == -1);
    assert(weaponInitialAmmo(finiteAmmo) == 40);
    assert(weaponNeedsReload(finiteAmmo, 0));
    assert(!weaponNeedsReload(infiniteAmmo, 0));
    std::vector<Weapon> weapons(4);
    for (int i = 0; i < 4; ++i) weapons[i].type = i;
    weapons[0].ammo = 10;
    weapons[1].ammo = 0;
    weapons[2].ammo = -1;
    weapons[3].ammo = 0;
    assert(weaponIsSelectable(weapons[0]));
    assert(!weaponIsSelectable(weapons[1]));
    assert(weaponIsSelectable(weapons[2]));
    assert(nextSelectableWeapon(weapons, 0, 1) == 2);
    // Empty finite-reserve slots remain selectable so selecting one can
    // restart its interrupted reload instead of hiding it permanently.
    assert(nextSelectableWeapon(weapons, 2, 1) == 3);
    assert(nextSelectableWeapon(weapons, 0, -1) == 3);
    assert(weaponTriggerCanFire(infiniteAmmo, true, false));
    assert(!weaponTriggerCanFire(infiniteAmmo, true, true));
    WeaponData automatic = infiniteAmmo;
    automatic.autoFire = true;
    assert(weaponTriggerCanFire(automatic, true, true));
    assert(weaponFireModeAllowed(infiniteAmmo, false));
    assert(!weaponFireModeAllowed(infiniteAmmo, true));
    WeaponData alternate = infiniteAmmo;
    alternate.altFire = true;
    assert(weaponFireModeAllowed(alternate, false));
    assert(weaponFireModeAllowed(alternate, true));
    // A newly connected or respawned client has timestamp zero and can fire
    // immediately; subsequent shots still observe the authored cadence.
    assert(weaponFireCooldownReady(0.1, 0.0, 0.8f));
    assert(!weaponFireCooldownReady(0.8, 0.1, 0.8f));
    assert(weaponFireCooldownReady(0.91, 0.1, 0.8f));
    // A weapon without an authored reload duration cannot be manually
    // reloaded; the native reload action must not instantly refill it.
    const WeaponData noReload{"", ProjectileType::Bolt, 0, 0.0f, 0, 0, 0, 40, 0, false, false, nullptr, nullptr};
    assert(!weaponNeedsReload(noReload, 0));
    assert(!weaponCanStartReload(noReload, 0));
    assert(weaponCanStartReload(finiteAmmo, 0));

    // Completing a reload must return the image state to its idle sentinel;
    // retaining a negative timer can make later reload transitions diverge.
    Weapon reloading;
    reloading.type = 0;
    reloading.ammo = 0;
    reloading.reloading = true;
    reloading.reloadTimer = 0.1f;
    reloading.updateTimers(0.1f);
    assert(!reloading.reloading && reloading.reloadTimer == 0.0f &&
           reloading.ammo == gWeaponTable[0].maxAmmo);

    const Point3F trace = hitscanEndpoint({1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -1.0f}, 100.0f);
    assert(trace.x == 1.0f && trace.y == 2.0f && trace.z == -97.0f);
    const Point3F scaledTrace = hitscanEndpoint({1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -4.0f}, 100.0f);
    assert(scaledTrace.x == 1.0f && scaledTrace.y == 2.0f && scaledTrace.z == -97.0f);
    // Hitscan collision must sweep from the muzzle to the trace endpoint;
    // replacing previousPos with the endpoint would make the segment empty.
    assert(!projectileAdvancesPosition(ProjectileType::Hitscan));
    assert(projectileAdvancesPosition(ProjectileType::Disc));

    // Authoritative projectile collision must sweep between server ticks;
    // checking only the endpoint lets a fast disc tunnel through a player.
    float hitT = -1.0f;
    assert(segmentPlayerHit({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f},
                            {0.0f, 0.0f, 0.0f}, hitT));
    assert(hitT >= 0.0f && hitT < 1.0f);
    assert(!segmentPlayerHit({-5.0f, 3.0f, 0.0f}, {5.0f, 3.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f}, hitT));
    // The authored player volume includes its top and bottom planes; an exact
    // boundary hit must not tunnel through the player.
    assert(segmentPlayerHit({-5.0f, 2.0f, 0.0f}, {5.0f, 2.0f, 0.0f},
                            {0.0f, 0.0f, 0.0f}, hitT));

}

static void testHudStateAndLifecycleParity() {
    // Tribes 2 team sensor groups are 1 and 2; zero means unassigned.
    assert(HudParity::sensorGroupInRange(1, 2));
    assert(HudParity::sensorGroupInRange(2, 2));
    assert(!HudParity::sensorGroupInRange(0, 2));
    assert(!HudParity::sensorGroupInRange(3, 2));
    assert(std::strcmp(HudParity::teamName(1), "Storm") == 0);
    assert(std::strcmp(HudParity::teamName(2), "Inferno") == 0);
    assert(std::strcmp(HudParity::teamName(0), "Observer") == 0);
    // Stock Tribes 2 assigns Storm (team 1) blue and Inferno (team 2) red.
    assert(HudParity::teamColor(1).b > HudParity::teamColor(1).r);
    assert(HudParity::teamColor(2).r > HudParity::teamColor(2).b);
    assert(HudParity::messageAlpha(0.0, 3.0) == 1.0f);
    assert(HudParity::messageAlpha(1.5, 3.0) > 0.49f && HudParity::messageAlpha(1.5, 3.0) < 0.51f);
    assert(HudParity::messageAlpha(3.0, 3.0) == 0.0f);
    assert(HudParity::scoreboardHeaderY(100.0f, 2) == 218.0f);
    assert(HudParity::targetFinderWindowStart(0, 20, 13) == 0);
    assert(HudParity::targetFinderWindowStart(12, 20, 13) == 0);
    assert(HudParity::targetFinderWindowStart(13, 20, 13) == 1);
    assert(HudParity::targetFinderWindowStart(19, 20, 13) == 7);
    DemoParser parser;
    parser.handleHudRemoteCommand("setWeaponsHudItem", {"setWeaponsHudItem", "2", "40", "1"});
    parser.handleHudRemoteCommand("setWeaponsHudBitmap", {"setWeaponsHudBitmap", "2", "", "gui/disc"});
    parser.handleHudRemoteCommand("setWeaponsHudActive", {"setWeaponsHudActive", "2"});
    parser.handleHudRemoteCommand("setAmmoHudCount", {"setAmmoHudCount", "7"});
    assert(parser.getWeaponsHud().slots.at(2) == 40);
    assert(parser.getWeaponsHud().bitmaps.at(2) == "gui/disc");
    assert(parser.getWeaponsHud().activeIndex == 2);
    assert(parser.getAmmoHud().count == 7);

    parser.handleHudRemoteCommand("setInventoryHudItem", {"setInventoryHudItem", "4", "3", "1"});
    parser.handleHudRemoteCommand("setInventoryHudAmount", {"setInventoryHudAmount", "4", "9"});
    assert(parser.getInventoryHud().slots.at(4) == 9);

    parser.handleHudRemoteCommand("setBackpackHudItem", {"setBackpackHudItem", "6", "1"});
    parser.handleHudRemoteCommand("updatePackText", {"updatePackText", "2"});
    assert(parser.getBackpackHud().packIndex == 6 && parser.getBackpackHud().active);
    assert(parser.getBackpackHud().text == "2");
    parser.handleHudRemoteCommand("setRepairPackIconOff", {"setRepairPackIconOff"});
    assert(!parser.getBackpackHud().active && parser.getBackpackHud().text.empty());

    // Mission transitions and replay seeks must not carry HUD state forward.
    parser.handleHudRemoteCommand("setWeaponsHudActive", {"setWeaponsHudActive", "2"});
    parser.resetHudState();
    assert(parser.getWeaponsHud().slots.empty());
    assert(parser.getInventoryHud().slots.empty());
    assert(parser.getBackpackHud().packIndex == -1 && !parser.getBackpackHud().active);
    assert(parser.getAmmoHud().count == -1);
}

static void testDeathRespawnParity() {
    assert(DeathRespawn::RespawnDelay == 3.0f);
    assert(DeathRespawn::crossesIntoDeath(10.0f, 10.0f));
    assert(DeathRespawn::crossesIntoDeath(100.0f, 101.0f));
    assert(!DeathRespawn::crossesIntoDeath(0.0f, 100.0f));
    assert(!DeathRespawn::crossesIntoDeath(10.0f, 9.0f));
    assert(DeathRespawn::crossesHealthBoundary(1.0f, 0.0f));
    assert(!DeathRespawn::crossesHealthBoundary(0.0f, 0.0f));
    assert(!DeathRespawn::crossesHealthBoundary(10.0f, 1.0f));
    assert(!DeathRespawn::respawnDue(2.99f, 3.0f));
    assert(DeathRespawn::respawnDue(3.0f, 3.0f));
    assert(!DeathRespawn::respawnDue(3.0f, 0.0f));
    assert(DeathRespawn::nextSpawn(0, 3) == 0);
    assert(DeathRespawn::nextSpawn(3, 3) == 0);
    assert(DeathRespawn::nextSpawn(4, 3) == 1);
    assert(DeathRespawn::nextSpawn(0, 0) == 0);
}

static void testStockMissionRules() {
    for (int i = 1; i <= 5; ++i) {
        const MissionRules rules = stockMissionRules("Training" + std::to_string(i));
        assert(rules.type == MissionGameType::Training);
        assert(!rules.respawn && !rules.teamBased && rules.scoreLimit == 0 && rules.objectives);
    }
    assert(isStockTrainingMission("base/missions/Training5.mis"));
    assert(!isStockTrainingMission("Training10.mis"));
    const MissionRules authored = stockMissionRules(
        "base/missions/Training3.mis", "new AIObjective(TrainingGoal) {}");
    assert(authored.objectives && !authored.matchClock && !authored.scoreHud &&
           !authored.debrief);
    const MissionRules ctf = stockMissionRules("Minotaur");
    assert(ctf.type == MissionGameType::CaptureTheFlag && ctf.teamBased &&
           ctf.objectives && ctf.scoreLimit == 5);
    assert(stockMissionRules("Damnation").type == MissionGameType::CaptureTheFlag);
    assert(stockMissionRules("base/missions/Raindance.mis").type ==
           MissionGameType::CaptureTheFlag);
    assert(stockMissionRules("Scarabrae").type == MissionGameType::CaptureTheFlag);
    assert(stockMissionRules("base\\missions\\Minotaur.mis").type ==
           MissionGameType::CaptureTheFlag);
    assert(stockMissionRules("Katabatic", "// MissionTypes = Team Deathmatch\n").type ==
           MissionGameType::TeamDeathmatch);
    assert(stockMissionRules("custom", "missionTypes = \"CTF\";\n").type ==
           MissionGameType::CaptureTheFlag);
    assert(stockMissionRules("custom", "missionTypes=TeamDeathmatch;\n").type ==
           MissionGameType::TeamDeathmatch);
    assert(stockMissionRules("Arena").type == MissionGameType::Deathmatch);
    assert(missionRulesTeamSpawn(1, 1) && missionRulesTeamSpawn(0, 2));
    assert(!missionRulesTeamSpawn(2, 1));
}

static void testObjectivePresentationParity() {
    AuthoredMissionObjective objective;
    objective.marker.teamId = 1;
    objective.description = "Destroy the generator";
    assert(objectiveVisibleForTeam(1, 1, false));
    assert(!objectiveVisibleForTeam(2, 1, false));
    assert(objectiveVisibleForTeam(2, 0, false));
    assert(objectiveVisibleForTeam(2, 1, true));
    const ColorF observerStorm = objectiveMarkerColor(1, 0, false);
    const ColorF observerInferno = objectiveMarkerColor(2, 0, false);
    assert(observerStorm.b > observerStorm.r && observerStorm.b > observerStorm.g);
    assert(observerInferno.r > observerInferno.g && observerInferno.r > observerInferno.b);
    const auto friendly = presentObjective(objective, 1, false);
    assert(friendly.visible && friendly.label == objective.description);
    assert(friendly.color.g > friendly.color.r);
    const auto mapper = presentObjective(objective, 2, true);
    assert(mapper.visible && mapper.color.b > mapper.color.r);
    objective.marker.teamId = 2;
    const auto inferno = presentObjective(objective, 1, true);
    assert(inferno.visible && inferno.color.r > inferno.color.b);
    assert(objectiveTaskText("Capture tower", "Eliminate enemies") ==
           "Capture tower\nEliminate enemies");
    assert(objectiveTaskText("", "Second") == "Second");
    assert(stockTrainingInitialObjective("Training4").first ==
           "Stay alert for enemy presence.");
    assert(stockTrainingInitialObjective("Training4").second ==
           "Repair sensor at waypoint.");
    assert(stockTrainingInitialObjective("base/missions/training4.mis").first ==
           "Stay alert for enemy presence.");
    assert(stockTrainingInitialObjective("Training10").first.empty());
    assert(stockTrainingInitialObjective("Arena").first.empty());
}

static void testCtfRuntimeTransitions() {
    assert(CtfRuntime::DroppedFlagReturnSeconds == 30.0);
    CtfRuntime::Match match;
    match.reset(1000);
    match.scoreLimit = 2;
    match.start();
    CtfRuntime::Flag red{1}, blue{2};
    assert(!CtfRuntime::take(blue, 0, 7));
    assert(blue.state == CtfRuntime::FlagState::Home);
    assert(CtfRuntime::take(blue, 1, 7));
    assert(blue.state == CtfRuntime::FlagState::Held && blue.carrier == 7);
    assert(!CtfRuntime::take(blue, 1, 8));
    CtfRuntime::Flag dropped{2, CtfRuntime::FlagState::Dropped, -1, 12.0};
    assert(CtfRuntime::take(dropped, 1, 8));
    assert(dropped.returnTimer == 0.0);
    CtfRuntime::Flag ownFlag{1, CtfRuntime::FlagState::Held, 7};
    assert(!CtfRuntime::canCapture(ownFlag, red, 1, 7));
    CtfRuntime::Flag unresolved = blue;
    unresolved.carrier = -1;
    assert(!CtfRuntime::canCapture(unresolved, red, 1, -1));
    assert(CtfRuntime::drop(blue, 7));
    assert(blue.returnTimer == CtfRuntime::DroppedFlagReturnSeconds);
    assert(!CtfRuntime::tickFlag(blue, 29.9));
    assert(blue.state == CtfRuntime::FlagState::Dropped);
    assert(CtfRuntime::tickFlag(blue, 0.11));
    assert(blue.state == CtfRuntime::FlagState::Home && blue.carrier == -1);
    assert(!CtfRuntime::tickFlag(blue, 1.0));
    assert(CtfRuntime::take(blue, 1, 7));
    assert(CtfRuntime::drop(blue, 7));
    assert(CtfRuntime::returnHome(blue, 2));
    assert(blue.state == CtfRuntime::FlagState::Home && blue.returnTimer == 0.0);
    CtfRuntime::Flag unassigned{0, CtfRuntime::FlagState::Dropped};
    assert(!CtfRuntime::returnHome(unassigned, 0));
    assert(unassigned.state == CtfRuntime::FlagState::Dropped);
    assert(CtfRuntime::take(blue, 1, 7));
    assert(CtfRuntime::canCapture(blue, red, 1, 7));
    assert(match.capture(1) && match.score[1] == 1 && !match.ended);
    assert(!match.tick(999) && match.clockMs == 1);
    assert(match.tick(1) == true && match.ended && match.clockMs == 0);
    assert(!match.capture(1));

    CtfRuntime::Match invalidLimit;
    invalidLimit.scoreLimit = 0;
    invalidLimit.reset(1000);
    invalidLimit.start();
    assert(!invalidLimit.capture(1));

    CtfRuntime::Match expired;
    expired.reset(0);
    expired.start();
    assert(expired.started && expired.ended);
    assert(!expired.capture(1));
}

static void testMatchRuntimeParity() {
    assert(MatchRuntime::balancedTeam(0, 0) == 1);
    assert(MatchRuntime::balancedTeam(1, 0) == 2);
    assert(MatchRuntime::balancedTeam(1, 1) == 1);
    assert(MatchRuntime::joiningTeam(false, 4, 0) == 0);
    assert(MatchRuntime::joiningTeam(true, 1, 0) == 2);
    assert(MatchRuntime::joiningTeam(true, 1, 1) == 1);

    MatchRuntime::Clock clock;
    clock.start();
    assert(clock.started && !clock.ended);
    assert(clock.tick(1000) && clock.remainingMs == MatchRuntime::MatchDurationMs - 1000);
    assert(!clock.tick(0));
    clock.tick(MatchRuntime::MatchDurationMs);
    assert(clock.ended && clock.remainingMs == 0);

    MatchRuntime::Clock emptyClock;
    emptyClock.reset();
    emptyClock.remainingMs = 0;
    emptyClock.start();
    assert(emptyClock.ended && !emptyClock.started);
    int emptyPlayerScore = 0;
    int emptyTeamScore = 0;
    assert(!MatchRuntime::scoreKill(emptyClock, emptyPlayerScore, emptyTeamScore,
                                    false, 1));
    assert(emptyPlayerScore == 0 && emptyTeamScore == 0);

    clock.reset();
    int playerScore = 0;
    int teamScore = 0;
    assert(!MatchRuntime::scoreKill(clock, playerScore, teamScore, false, 2));
    assert(playerScore == 0 && teamScore == 0 && !clock.ended);
    clock.start();
    assert(!MatchRuntime::scoreKill(clock, playerScore, teamScore, false, 2));
    assert(MatchRuntime::scoreKill(clock, playerScore, teamScore, false, 2));
    assert(clock.ended && playerScore == 2 && teamScore == 0);
    assert(!MatchRuntime::scoreKill(clock, playerScore, teamScore, false, 2));
}

static void testSparseScreenEffectParity() {
    V12::ServerGameState live;
    V12BitWriter writer;
    writer.writeUnsigned(0, 2); // rate fields
    for (int i = 0; i < 4; ++i) writer.writeUnsigned(0, 7);
    writer.writeUnsigned(0, 32); // last move ack
    writer.writeFlag(true);      // effects section
    writer.writeFlag(true); writer.writeUnsigned(64, 7);
    writer.writeFlag(false);     // no whiteout
    writer.writeFlag(false);     // no self lock
    writer.writeFlag(false);     // no seeker tracking
    writer.writeFlag(false); writer.writeFlag(false); // not pinged/jammed
    writer.writeFlag(false);     // no control object update
    writer.writeFlag(true);      // target visibility entry
    writer.writeUnsigned(3, 4);  // listener sensor group
    writer.writeUnsigned((1u << 2) | (1u << 7), 32);
    writer.writeFlag(false);     // end target visibility entries
    writer.writeFlag(false);     // no camera FOV
    writer.writeFlag(false);     // no unguaranteed events
    writer.writeFlag(false);     // no guaranteed events
    auto input = stream(writer);
    std::vector<V12::ServerEvent> events;
    V12::NetStringTable strings;
    assert(V12::readServerPacketEvents(input, strings, events, &live));
    assert(live.hasDamageFlash && !live.hasWhiteOut && live.damageFlash > 0.49f &&
           live.damageFlash < 0.51f);
    assert(live.sensorGroupListenMasks.at(3) == ((1u << 2) | (1u << 7)));

    // Demo and live packet defaults must be distinguishable from an explicit
    // zero, otherwise sparse packets erase an effect before client decay.
    GameState demo;
    assert(!demo.hasDamageFlash && !demo.hasWhiteOut);
}

static void testLinkBeamGeometry() {
    const Point3F start{0, 0, 0}, end{10, 0, 0};
    const auto straight = linkBeamPoints(start, end, false);
    assert(straight.size() == 2 && straight.front().x == 0 && straight.back().x == 10);
    const auto elf = linkBeamPoints(start, end, true);
    assert(elf.size() == 9 && elf.front().x == 0 && elf.back().x == 10);
    assert(elf[4].y > 0.0f);

    const auto quad = projectileBeamQuad(start, end, {5, 4, 0}, 2.0f);
    assert(quad.size() == 4 && quad[0].z > quad[1].z);
    const auto parallel = projectileBeamQuad(start, end, {5, 0, 0}, 2.0f);
    assert(parallel.size() == 4);
    const auto vertical = projectileBeamQuad(start, {0, 10, 0}, {3, 5, 0}, 2.0f);
    assert(vertical.size() == 4);
    assert(vertical[0].x != vertical[1].x || vertical[0].z != vertical[1].z);
    const auto verticalLightning = shockLancePoints(start, {0, 10, 0}, 0.0f);
    assert(verticalLightning.size() >= 2);
    const auto degenerate = projectileBeamQuad(start, start, {0, 1, 0}, 2.0f);
    assert(degenerate.size() == 2);
}

static void testHudCrosshairVisibility() {
    assert(HudParity::crosshairVisible(false, false, false));
    assert(!HudParity::crosshairVisible(true, false, false));
    assert(!HudParity::crosshairVisible(false, true, false));
    assert(!HudParity::crosshairVisible(false, false, true));
}

static void testParticleKeyClamping() {
    const std::vector<float> times{0.25f, 0.75f};
    const auto beforeFirst = particleKeyInterpolation(0.1f, times);
    assert(beforeFirst.lower == 0 && beforeFirst.upper == 0 && beforeFirst.fraction == 0.0f);
    const auto between = particleKeyInterpolation(0.5f, times);
    assert(between.lower == 0 && between.upper == 1 && between.fraction == 0.5f);
    const auto afterLast = particleKeyInterpolation(1.0f, times);
    assert(afterLast.lower == 1 && afterLast.upper == 1 && afterLast.fraction == 0.0f);
    const auto invalidAge = particleKeyInterpolation(
        std::numeric_limits<float>::quiet_NaN(), times);
    assert(invalidAge.lower == 0 && invalidAge.upper == 0 && invalidAge.fraction == 0.0f);
    const auto infiniteAge = particleKeyInterpolation(
        std::numeric_limits<float>::infinity(), times);
    assert(infiniteAge.lower == 0 && infiniteAge.upper == 0 && infiniteAge.fraction == 0.0f);
    // Bad authored tracks are held at their last valid key instead of jumping
    // to a descending or non-finite entry during rendering.
    const auto descending = particleKeyInterpolation(1.0f, {0.0f, 0.75f, 0.5f});
    assert(descending.lower == 1 && descending.upper == 1 && descending.fraction == 0.0f);
    const auto nonFinite = particleKeyInterpolation(
        1.0f, {0.0f, 0.75f, std::numeric_limits<float>::quiet_NaN()});
    assert(nonFinite.lower == 1 && nonFinite.upper == 1 && nonFinite.fraction == 0.0f);
}

static void testRespawnResetsAnimationParity() {
    Player player;
    player.applyDamage(1000.0f);
    player.updateAnimation(1.0f / 60.0f, false);
    assert(player.isDead() && player.animState() == Player::Death);
    player.respawn();
    assert(!player.isDead() && player.animState() == Player::Stand);
}

static void testMissionObjectNameParity() {
    assert(missionObjectNameIs("BaseDoor", "BaseDoor"));
    assert(missionObjectNameIs("BaseDoor", "basedoor"));
    assert(missionObjectNameIs("BASEDOOR", "BaseDoor"));
    assert(!missionObjectNameIs("BaseDoor", "BaseDoor2"));
}

static void testConsoleAssignmentWhitespaceParity() {
    Console console;
    console.setVariable("$parityConsoleValue", "old");
    console.execute("$parityConsoleValue = new value");
    assert(std::string(console.getStringVariable("$parityConsoleValue", "")) ==
           "new value");
}

int main() {
    testDataBlocks(); testProjectileImpact(); testTerrainRaycastNormalParity(); testProjectileDatablockReferences(); testProjectileVisualDefaults(); testTerrainHoles();
    testTerrainSplitInterpolation();
    testSparseVehicleStateMerge(); testSparseAppearanceStateMerge();
    testSparseDamageAndUnmountMerge();
    testVehicleStateMergeAndControl();
    testMountedImageWireOrder();
    testShapeBaseV12OrderAndReset();
    testDemoClockMath();
    testInputParity();
    testTextureFrameTiming();
    testDemoCameraMath(); testDemoGhostHudCoordinateParity();
    testDemoAudioIdentity();
    testLiveAudioEventPayload();
    testLiveEventOrderingAndPayloads();
    testInteriorOutsideAndPortalRules();
    testInteriorCollisionSelectionAndContacts();
    testLineOfSightEndpoint();
    testMovementParity(); testHudCrosshairVisibility(); testSlowProjectileSweepParity(); testProjectileSlopeBounceParity(); testItemPickupRejectsNonFiniteAmounts(); testDamageRejectsNonFiniteAmounts(); testPlayerStateRejectsNonFiniteMutations(); testWaterVolumeParity(); testWaterBoundsParity(); testWaterRenderDistanceParity(); testProjectileDirectHitParity();
    testProjectileTerrainCrossingParity();
    testPrecipitationEnableRestoresCoverage();
    testProjectileGravityParity();
    testServerGroundHeightParity();
    testItemPickupParity(); testWeaponSelectionAndStateParity();
    testHudStateAndLifecycleParity();
    testDeathRespawnParity();
    testStockMissionRules();
    testObjectivePresentationParity();
    testObserverTargetParity();
    testMissionHiddenObjectParity();
    testGhostClassParity();
    testFreeCameraSpeedParity();
    testCameraPitchParity();
    testAnimationDeltaParity();
    testDtsThreadTimingParity();
    testLoopingAnimationReverseParity();
    testParticleTickParity();
    testBotRespawnClearsReactionState();
    testWindRejectsInvalidComponents();
    testSpawnSphereFacingParity();
    testDamageFlashHealthCapParity();
    testFiniteAmmoSentinelParity();
    testVerticalProjectilePlayerHit();
    testProjectileLifetimeParity();
    testHitscanResolvesInOneTick();
    testPhysicalZoneInvalidValues();
    testDamageLevelUsesAuthoredHealthCap();
    testRepairRateParity();
    testMissionClassNamesAreCaseInsensitive();
    testTeamSpawnSelection();
    testObserverTeamLabel(); testResourceBarFractions();
    testScoreboardTeamRowsOnlyReserveVisibleStrip();
    testArmorCannotAbsorbMoreThanRemains();
    testLocalBotKillsUpdateScoreboard();
    testPlayerUsesAuthoredMaximumHealth();
    testJetHeatUsesActualJetting();
    testMovementRejectsInvalidInput(); testWheelStepOverflowParity(); testMovementRejectsInvalidReplicatedState(); testRayCastRejectsInvalidViewInput(); testWorldTickDeltaIsBounded(); testDroppedFlagRejectsInvalidTime(); testDroppedFlagRecoversInvalidDeadline(); testMissionFlagTeamNameIsCaseInsensitive();
    testCtfRuntimeTransitions(); testCtfFlagReturnClearsCarrier(); testCtfStatusParsingIsLocaleTolerant();
    testMatchRuntimeParity();
    testSparseScreenEffectParity();
    testLinkBeamGeometry();
    testParticleKeyClamping();
    testMissionObjectNameParity();
    testConsoleAssignmentWhitespaceParity();
    testRespawnResetsAnimationParity();
    return 0;
}
