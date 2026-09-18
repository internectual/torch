#include "net/v12_datablocks.h"
#include "net/v12_ghosts.h"
#include "net/v12_events.h"
#include "render/renderer.h"
#include "game/demo.h"
#include "game/collision.h"
#include "render/texture_frames.h"
#include "core/input_parity.h"
#include "game/movement.h"
#include "game/item_parity.h"
#include "game/weapon.h"
#include "game/death_respawn.h"
#include "game/hud_parity.h"
#include "game/mission_rules.h"
#include "game/ctf_runtime.h"
#include "game/match_runtime.h"
#include "game/objective_parity.h"
#include "game/link_beam.h"

#include <cassert>
#include <cstring>

static void f32(V12BitWriter& w, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    w.writeUnsigned(bits, 32);
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
    assert(weaponSlotForScancode(30) == 0);
    assert(weaponSlotForScancode(38) == 8);
    assert(weaponSlotForScancode(39) == 9);
    assert(weaponSlotForScancode(29) == -1);
    assert(observerCyclePressed(false, false) == false);
    assert(observerCyclePressed(true, false) == true);
    assert(observerCyclePressed(false, true) == true);
}

static void testTextureFrameTiming() {
    const std::vector<float> durations{0.1f, 0.2f, 0.3f};
    assert(textureFrameIndex(durations, 3, 0.0f) == 0);
    assert(textureFrameIndex(durations, 3, 0.1f) == 1);
    assert(textureFrameIndex(durations, 3, 0.29f) == 1);
    assert(textureFrameIndex(durations, 3, 0.3f) == 2);
    assert(textureFrameIndex(durations, 3, 0.6f) == 0);
    assert(textureFrameIndex({}, 3, 0.5f) == 1);
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
}

static void testMovementParity() {
    MovementEnvironment floor{0.0f, {0, 1, 0}, false};
    MovementState accelerated{{0, 0, 0}, {0, 0, 0}, 100, true, false};
    MovementInput forward; forward.forward = 1.0f;
    Movement::step(accelerated, forward, floor, 1.0f / 60.0f);
    assert(accelerated.velocity.z > 0.9f && accelerated.velocity.z < 1.1f);
    for (int i = 0; i < 120; ++i) Movement::step(accelerated, {}, floor, 1.0f / 60.0f);
    assert(accelerated.velocity.z < 0.01f);

    MovementState jumped{{0, 0, 0}, {0, 0, 0}, 100, true, false};
    MovementInput jump; jump.jump = true;
    Movement::step(jumped, jump, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y > 9.5f && !jumped.onGround);
    const float jumpVelocity = jumped.velocity.y;
    Movement::step(jumped, jump, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y < jumpVelocity); // held jump does not retrigger
    MovementInput jet; jet.jet = true;
    const float energy = jumped.energy;
    Movement::step(jumped, jet, floor, 1.0f / 60.0f);
    assert(jumped.velocity.y > jumpVelocity - 0.5f && jumped.energy < energy);

    MovementState falling{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    Movement::step(falling, {}, floor, 1.0f / 60.0f);
    assert(falling.velocity.y < 0.0f);
    MovementState swimming{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    Movement::step(swimming, {}, {0.0f, {0, 1, 0}, true}, 1.0f / 60.0f);
    assert(swimming.velocity.y > falling.velocity.y);

    MovementEnvironment zone{0.0f, {0, 1, 0}, false, 0.5f, 2.0f, {4, 0, 0}};
    MovementState zoned{{0, 2, 0}, {2, 0, 0}, 100, false, false};
    Movement::step(zoned, {}, zone, 1.0f / 60.0f);
    assert(zoned.velocity.x > 0.9f && zoned.velocity.x < 1.1f);
    assert(zoned.velocity.y < -0.7f && zoned.velocity.y > -1.0f);

    Point3F wallVelocity{3, 0, -2};
    Movement::projectOnPlane(wallVelocity, {1, 0, 0});
    assert(wallVelocity.x == 3.0f); // separating velocity is preserved
    wallVelocity = {-3, 0, -2};
    Movement::projectOnPlane(wallVelocity, {1, 0, 0});
    assert(wallVelocity.x == 0.0f && wallVelocity.z == -2.0f);
    Point3F slopeVelocity{0, -1, 2};
    Movement::projectOnPlane(slopeVelocity, {0, 0.7071067f, 0.7071067f});
    assert(slopeVelocity.y < 0.0f && slopeVelocity.z > 0.0f);

    MovementState authoritative{{0, 0, 0}, {0, 0, 0}, 100, true, false};
    MovementState replay = authoritative;
    for (int i = 0; i < 30; ++i) Movement::step(authoritative, forward, floor, 1.0f / 60.0f);
    for (int i = 0; i < 10; ++i) Movement::step(replay, forward, floor, 1.0f / 60.0f);
    for (int i = 10; i < 30; ++i) Movement::step(replay, forward, floor, 1.0f / 60.0f);
    assert(replay.position.x == authoritative.position.x && replay.position.z == authoritative.position.z);
    assert(replay.energy == authoritative.energy);

    MovementState customGravity{{0, 10, 0}, {0, 0, 0}, 100, false, false};
    MovementEnvironment lowGravity = floor;
    lowGravity.gravity = -5.0f;
    Movement::step(customGravity, {}, lowGravity, 1.0f / 60.0f);
    assert(customGravity.velocity.y < 0.0f && customGravity.velocity.y > -0.1f);
}

static void testItemPickupParity() {
    assert(classifyItemKind("HealthKit") == ItemKind::Health);
    assert(classifyItemKind("EnergyPack") == ItemKind::Energy);
    assert(classifyItemKind("AmmoPack") == ItemKind::Ammo);
    assert(classifyItemKind("Flag") == ItemKind::None);
    assert(applyItemAmount(ItemKind::Health, 90.0f, 25.0f, 100.0f) == 100.0f);
    assert(applyItemAmount(ItemKind::Energy, 10.0f, 25.0f, 100.0f) == 35.0f);
    assert(applyItemAmount(ItemKind::Ammo, 4.0f, 15.0f, 0.0f) == 19.0f);
}

static void testWeaponSelectionAndStateParity() {
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
    assert(nextSelectableWeapon(weapons, 2, 1) == 0);
    assert(nextSelectableWeapon(weapons, 0, -1) == 2);
}

static void testHudStateAndLifecycleParity() {
    assert(HudParity::messageAlpha(0.0, 3.0) == 1.0f);
    assert(HudParity::messageAlpha(1.5, 3.0) > 0.49f && HudParity::messageAlpha(1.5, 3.0) < 0.51f);
    assert(HudParity::messageAlpha(3.0, 3.0) == 0.0f);
    assert(HudParity::scoreboardHeaderY(100.0f, 2) == 218.0f);
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
    assert(DeathRespawn::crossesIntoDeath(10.0f, 10.0f));
    assert(DeathRespawn::crossesIntoDeath(100.0f, 101.0f));
    assert(!DeathRespawn::crossesIntoDeath(0.0f, 100.0f));
    assert(!DeathRespawn::crossesIntoDeath(10.0f, 9.0f));
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
    assert(stockMissionRules("Katabatic", "// MissionTypes = Team Deathmatch\n").type ==
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
    assert(objectiveVisibleForTeam(2, 1, true));
    const auto friendly = presentObjective(objective, 1, false);
    assert(friendly.visible && friendly.label == objective.description);
    assert(friendly.color.g > friendly.color.r);
    const auto mapper = presentObjective(objective, 2, true);
    assert(mapper.visible && mapper.color.r > mapper.color.b);
    assert(objectiveTaskText("Capture tower", "Eliminate enemies") ==
           "Capture tower\nEliminate enemies");
    assert(objectiveTaskText("", "Second") == "Second");
    assert(stockTrainingInitialObjective("Training4").first ==
           "Stay alert for enemy presence.");
    assert(stockTrainingInitialObjective("Training4").second ==
           "Repair sensor at waypoint.");
    assert(stockTrainingInitialObjective("Arena").first.empty());
}

static void testCtfRuntimeTransitions() {
    CtfRuntime::Match match;
    match.reset(1000);
    match.scoreLimit = 2;
    match.start();
    CtfRuntime::Flag red{1}, blue{2};
    assert(CtfRuntime::take(blue, 1, 7));
    assert(blue.state == CtfRuntime::FlagState::Held && blue.carrier == 7);
    assert(!CtfRuntime::take(blue, 1, 8));
    assert(CtfRuntime::drop(blue, 7));
    assert(CtfRuntime::returnHome(blue, 2));
    assert(blue.state == CtfRuntime::FlagState::Home);
    assert(CtfRuntime::take(blue, 1, 7));
    assert(CtfRuntime::canCapture(blue, red, 1, 7));
    assert(match.capture(1) && match.score[1] == 1 && !match.ended);
    assert(!match.tick(999) && match.clockMs == 1);
    assert(match.tick(1) == true && match.ended && match.clockMs == 0);
    assert(!match.capture(1));
}

static void testMatchRuntimeParity() {
    assert(MatchRuntime::balancedTeam(0, 0) == 1);
    assert(MatchRuntime::balancedTeam(1, 0) == 2);
    assert(MatchRuntime::balancedTeam(1, 1) == 1);

    MatchRuntime::Clock clock;
    clock.start();
    assert(clock.started && !clock.ended);
    assert(clock.tick(1000) && clock.remainingMs == MatchRuntime::MatchDurationMs - 1000);
    assert(!clock.tick(0));
    clock.tick(MatchRuntime::MatchDurationMs);
    assert(clock.ended && clock.remainingMs == 0);

    clock.reset();
    clock.start();
    int playerScore = 0;
    int teamScore = 0;
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
    const auto degenerate = projectileBeamQuad(start, start, {0, 1, 0}, 2.0f);
    assert(degenerate.size() == 2);
}

int main() {
    testDataBlocks(); testProjectileImpact(); testProjectileDatablockReferences(); testProjectileVisualDefaults(); testTerrainHoles();
    testTerrainSplitInterpolation();
    testSparseVehicleStateMerge(); testSparseAppearanceStateMerge();
    testSparseDamageAndUnmountMerge();
    testVehicleStateMergeAndControl();
    testMountedImageWireOrder();
    testShapeBaseV12OrderAndReset();
    testDemoClockMath();
    testInputParity();
    testTextureFrameTiming();
    testDemoCameraMath();
    testDemoAudioIdentity();
    testLiveAudioEventPayload();
    testLiveEventOrderingAndPayloads();
    testInteriorOutsideAndPortalRules();
    testInteriorCollisionSelectionAndContacts();
    testMovementParity(); testItemPickupParity(); testWeaponSelectionAndStateParity();
    testHudStateAndLifecycleParity();
    testDeathRespawnParity();
    testStockMissionRules();
    testObjectivePresentationParity();
    testCtfRuntimeTransitions();
    testMatchRuntimeParity();
    testSparseScreenEffectParity();
    testLinkBeamGeometry();
    return 0;
}
