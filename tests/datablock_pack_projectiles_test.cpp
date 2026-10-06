// Round trip of the 'projectiles' datablock writers through Torch's reader.
#include "sim/datablock_pack.h"
#include "tests/datablock_pack_harness.h"
#include <cmath>

using Fields = std::map<std::string, std::string>;
using Refs = std::map<std::string, uint32_t>;

// Torch's reader keeps datablock references as the raw 11-bit wire value,
// id - DataBlockObjectIdFirst (as it does SimDataBlockEvent object ids).
static uint32_t wire(uint32_t id) { return id - DataBlockPack::ObjectIdFirst; }

static bool near(float a, float b, float eps = 1.0f / 255.0f) { return std::fabs(a - b) <= eps; }

static PackRoundTrip check(const char* className, const Fields& fields = {}, const Refs& refs = {}) {
    PackRoundTrip r = packRoundTrip(className, fields, refs);
    if (!r.exact()) fprintf(stderr, "FAIL %s (%zu fields)\n", className, fields.size());
    assert(r.exact());
    return r;
}

// Every retail datablock name the realistic sets reference.
static const Refs kRefs = {
    {"discProjectileSound", 10}, {"DiscExplosion", 11}, {"UnderwaterDiscExplosion", 12}, {"DiscSplash", 13},
    {"ChaingunExplosion", 20}, {"ChaingunSplash", 21}, {"ChaingunProjectile", 22},
    {"ChaingunDecal1", 23}, {"ChaingunDecal2", 24}, {"ChaingunDecal3", 25},
    {"ChaingunDecal4", 26}, {"ChaingunDecal5", 27}, {"ChaingunDecal6", 28},
    {"MortarExplosion", 30}, {"UnderwaterMortarExplosion", 31}, {"MortarSplash", 32},
    {"MortarSmokeEmitter", 33}, {"MortarBubbleEmitter", 34}, {"MortarProjectileSound", 35},
    {"MissileExplosion", 40}, {"MissileSplash", 41}, {"MissileSmokeEmitter", 42}, {"MissileFireEmitter", 43},
    {"MissilePuffEmitter", 44}, {"GrenadeBubbleEmitter", 45}, {"MissileLauncherExhaustEmitter", 46},
    {"MissileProjectileSound", 47}, {"FlechetteDebris", 48},
    {"ElfGunFireSound", 50}, {"ElfFireWetSound", 51}, {"ELFSparksEmitter", 52},
    {"SniperRifleProjectileSound", 60}, {"SniperExplosion", 61}, {"SniperSplash", 62},
    {"ShocklanceHit", 70}, {"ShockParticleEmitter", 71},
    {"BlasterProjectileSound", 80}, {"BlasterExplosion", 81}, {"BlasterSplash", 82},
    {"PlasmaBoltExplosion", 90}, {"PlasmaSplash", 91}, {"PlasmaProjectileSound", 92},
    {"PlasmaFireSound", 93}, {"PlasmaFireWetSound", 94},
    {"VehicleBombExplosion", 100}, {"BomberBombProjectileSound", 101},
    {"FlareGrenadeBurnSound", 110}, {"FlareGrenadeExplosion", 111}, {"FlareEmitter", 112},
    {"RepairPackFireSound", 120},
    {"GrenadeProjectileSound", 130}, {"GrenadeExplosion", 131}, {"UnderwaterGrenadeExplosion", 132},
    {"GrenadeSplash", 133}, {"GrenadeSmokeEmitter", 134},
};

// weapons/disc.cs DiscProjectile
static const Fields kDisc = {
    {"projectileShapeName", "disc.dts"}, {"emitterDelay", "-1"}, {"directDamage", "0.0"},
    {"hasDamageRadius", "true"}, {"indirectDamage", "0.50"}, {"damageRadius", "7.5"},
    {"kickBackStrength", "2000"}, {"sound", "discProjectileSound"}, {"explosion", "DiscExplosion"},
    {"underwaterExplosion", "UnderwaterDiscExplosion"}, {"splash", "DiscSplash"},
    {"dryVelocity", "95"}, {"wetVelocity", "55"}, {"velInheritFactor", "0.75"},
    {"fizzleTimeMS", "5000"}, {"lifetimeMS", "5000"}, {"explodeOnDeath", "true"},
    {"reflectOnWaterImpactAngle", "15.0"}, {"explodeOnWaterImpact", "true"},
    {"deflectionOnWaterImpact", "20.0"}, {"fizzleUnderwaterMS", "5000"}, {"activateDelayMS", "200"},
    {"hasLight", "true"}, {"lightRadius", "6.0"}, {"lightColor", "0.175 0.175 0.5"},
};

// weapons/chaingun.cs ChaingunBullet
static const Fields kChaingun = {
    {"doDynamicClientHits", "true"}, {"directDamage", "0.0842"}, {"explosion", "ChaingunExplosion"},
    {"splash", "ChaingunSplash"}, {"kickBackStrength", "0.0"}, {"sound", "ChaingunProjectile"},
    {"dryVelocity", "700.0"}, {"wetVelocity", "100.0"}, {"velInheritFactor", "1.0"},
    {"fizzleTimeMS", "3000"}, {"lifetimeMS", "3000"}, {"explodeOnDeath", "false"},
    {"reflectOnWaterImpactAngle", "0.0"}, {"explodeOnWaterImpact", "false"},
    {"deflectionOnWaterImpact", "0.0"}, {"fizzleUnderwaterMS", "3000"},
    {"tracerLength", "15.0"}, {"tracerAlpha", "false"}, {"tracerMinPixels", "6"},
    {"tracerColor", "0.827451 0.843137 0.470588 0.75"},
    {"tracerTex[0]", "special/tracer00"}, {"tracerTex[1]", "special/tracercross"},
    {"tracerWidth", "0.10"}, {"crossSize", "0.20"}, {"crossViewAng", "0.990"}, {"renderCross", "true"},
    {"decalData[0]", "ChaingunDecal1"}, {"decalData[1]", "ChaingunDecal2"}, {"decalData[2]", "ChaingunDecal3"},
    {"decalData[3]", "ChaingunDecal4"}, {"decalData[4]", "ChaingunDecal5"}, {"decalData[5]", "ChaingunDecal6"},
};

// weapons/mortar.cs MortarShot
static const Fields kMortar = {
    {"projectileShapeName", "mortar_projectile.dts"}, {"emitterDelay", "-1"}, {"directDamage", "0.0"},
    {"hasDamageRadius", "true"}, {"indirectDamage", "1.0"}, {"damageRadius", "19.0"},
    {"kickBackStrength", "2500"}, {"explosion", "MortarExplosion"},
    {"underwaterExplosion", "UnderwaterMortarExplosion"}, {"velInheritFactor", "0.5"},
    {"splash", "MortarSplash"}, {"depthTolerance", "10.0"}, {"baseEmitter", "MortarSmokeEmitter"},
    {"bubbleEmitter", "MortarBubbleEmitter"}, {"grenadeElasticity", "0.15"}, {"grenadeFriction", "0.4"},
    {"armingDelayMS", "1500"}, {"gravityMod", "1.1"}, {"muzzleVelocity", "75.95"}, {"drag", "0.1"},
    {"sound", "MortarProjectileSound"}, {"hasLight", "true"}, {"lightRadius", "4"},
    {"lightColor", "0.05 0.2 0.05"}, {"hasLightUnderwaterColor", "true"},
    {"underWaterLightColor", "0.05 0.075 0.2"},
};

// weapons/missilelauncher.cs ShoulderMissile
static const Fields kMissile = {
    {"casingShapeName", "weapon_missile_casement.dts"}, {"projectileShapeName", "weapon_missile_projectile.dts"},
    {"hasDamageRadius", "true"}, {"indirectDamage", "0.8"}, {"damageRadius", "8.0"},
    {"kickBackStrength", "2000"}, {"explosion", "MissileExplosion"}, {"splash", "MissileSplash"},
    {"velInheritFactor", "1.0"}, {"baseEmitter", "MissileSmokeEmitter"}, {"delayEmitter", "MissileFireEmitter"},
    {"puffEmitter", "MissilePuffEmitter"}, {"bubbleEmitter", "GrenadeBubbleEmitter"}, {"bubbleEmitTime", "1.0"},
    {"exhaustEmitter", "MissileLauncherExhaustEmitter"}, {"exhaustTimeMs", "300"},
    {"exhaustNodeName", "muzzlePoint1"}, {"lifetimeMS", "7000"}, {"muzzleVelocity", "10.0"},
    {"maxVelocity", "90.0"}, {"turningSpeed", "110.0"}, {"acceleration", "200.0"}, {"proximityRadius", "3"},
    {"terrainAvoidanceSpeed", "180"}, {"terrainScanAhead", "25"}, {"terrainHeightFail", "12"},
    {"terrainAvoidanceRadius", "100"}, {"flareDistance", "200"}, {"flareAngle", "30"},
    {"sound", "MissileProjectileSound"}, {"hasLight", "true"}, {"lightRadius", "5.0"},
    {"lightColor", "0.2 0.05 0"}, {"useFlechette", "true"}, {"flechetteDelayMs", "550"},
    {"casingDeb", "FlechetteDebris"}, {"explodeOnWaterImpact", "false"},
};

// weapons/elfgun.cs BasicELF (unindexed "emitter" is element 0)
static const Fields kELF = {
    {"beamRange", "38"}, {"numControlPoints", "8"}, {"restorativeFactor", "3.75"}, {"dragFactor", "4.5"},
    {"mainBeamWidth", "0.1"}, {"mainBeamSpeed", "9.0"}, {"mainBeamRepeat", "0.25"},
    {"lightningWidth", "0.1"}, {"lightningDist", "0.15"}, {"fireSound", "ElfGunFireSound"},
    {"wetFireSound", "ElfFireWetSound"}, {"textures[0]", "special/ELFBeam"},
    {"textures[1]", "special/ELFLightning"}, {"textures[2]", "special/BlueImpact"},
    {"emitter", "ELFSparksEmitter"},
};

// weapons/sniperrifle.cs BasicSniperShot
static const Fields kSniper = {
    {"directDamage", "0.4"}, {"hasDamageRadius", "false"}, {"velInheritFactor", "1.0"},
    {"sound", "SniperRifleProjectileSound"}, {"explosion", "SniperExplosion"}, {"splash", "SniperSplash"},
    {"maxRifleRange", "1000"}, {"rifleHeadMultiplier", "1.3"}, {"beamColor", "1 0.1 0.1"}, {"fadeTime", "1.0"},
    {"startBeamWidth", "0.145"}, {"endBeamWidth", "0.25"}, {"pulseBeamWidth", "0.5"},
    {"beamFlareAngle", "3.0"}, {"minFlareSize", "0.0"}, {"maxFlareSize", "400.0"},
    {"pulseSpeed", "6.0"}, {"pulseLength", "0.150"}, {"lightRadius", "1.0"}, {"lightColor", "0.3 0.0 0.0"},
    {"textureName[0]", "special/flare"}, {"textureName[1]", "special/nonlingradient"},
    {"textureName[2]", "special/laserrip01"}, {"textureName[3]", "special/laserrip02"},
    {"textureName[4]", "special/laserrip03"}, {"textureName[5]", "special/laserrip04"},
    {"textureName[6]", "special/laserrip05"}, {"textureName[7]", "special/laserrip06"},
    {"textureName[8]", "special/laserrip07"}, {"textureName[9]", "special/laserrip08"},
    {"textureName[10]", "special/laserrip09"}, {"textureName[11]", "special/sniper00"},
};

// weapons/shocklance.cs BasicShocker
static const Fields kShocker = {
    {"directDamage", "0.45"}, {"kickBackStrength", "2600"}, {"velInheritFactor", "0"}, {"sound", ""},
    {"zapDuration", "1.0"}, {"impulse", "1800"}, {"boltLength", "16.0"}, {"extension", "16.0"},
    {"lightningFreq", "25.0"}, {"lightningDensity", "3.0"}, {"lightningAmp", "0.25"},
    {"lightningWidth", "0.05"}, {"shockwave", "ShocklanceHit"},
    {"boltSpeed[0]", "2.0"}, {"boltSpeed[1]", "-0.5"}, {"texWrap[0]", "1.5"}, {"texWrap[1]", "1.5"},
    {"startWidth[0]", "0.3"}, {"endWidth[0]", "0.6"}, {"startWidth[1]", "0.3"}, {"endWidth[1]", "0.6"},
    {"texture[0]", "special/shockLightning01"}, {"texture[1]", "special/shockLightning02"},
    {"texture[2]", "special/shockLightning03"}, {"texture[3]", "special/ELFBeam"},
    {"emitter[0]", "ShockParticleEmitter"},
};

// packs/repairpack.cs DefaultRepairBeam
static const Fields kRepair = {
    {"sound", "RepairPackFireSound"}, {"beamRange", "10"}, {"beamWidth", "0.15"}, {"numSegments", "20"},
    {"texRepeat", "0.20"}, {"blurFreq", "10.0"}, {"blurLifetime", "1.0"}, {"cutoffAngle", "25.0"},
    {"textures[0]", "special/redbump2"}, {"textures[1]", "special/redflare"},
};

// weapons/targetinglaser.cs BasicTargeter
static const Fields kTargeter = {
    {"directDamage", "0.0"}, {"hasDamageRadius", "false"}, {"velInheritFactor", "1.0"},
    {"maxRifleRange", "1000"}, {"beamColor", "0.1 1.0 0.1"}, {"startBeamWidth", "0.20"},
    {"pulseBeamWidth", "0.15"}, {"beamFlareAngle", "3.0"}, {"minFlareSize", "0.0"},
    {"maxFlareSize", "400.0"}, {"pulseSpeed", "6.0"}, {"pulseLength", "0.150"},
    {"textureName[0]", "special/nonlingradient"}, {"textureName[1]", "special/flare"},
    {"textureName[2]", "special/pulse"}, {"textureName[3]", "special/expFlare"}, {"beacon", "true"},
};

// weapons/blaster.cs EnergyBolt
static const Fields kEnergyBolt = {
    {"emitterDelay", "-1"}, {"directDamage", "0.15"}, {"kickBackStrength", "0.0"}, {"bubbleEmitTime", "1.0"},
    {"sound", "BlasterProjectileSound"}, {"velInheritFactor", "0.5"}, {"explosion", "BlasterExplosion"},
    {"splash", "BlasterSplash"}, {"grenadeElasticity", "0.998"}, {"grenadeFriction", "0.0"},
    {"armingDelayMS", "500"}, {"muzzleVelocity", "90.0"}, {"drag", "0.05"}, {"gravityMod", "0.0"},
    {"dryVelocity", "200.0"}, {"wetVelocity", "150.0"}, {"explodeOnWaterImpact", "false"},
    {"fizzleUnderwaterMS", "3000"}, {"hasLight", "true"}, {"lightRadius", "3.0"},
    {"lightColor", "0.5 0.175 0.175"}, {"scale", "0.25 20.0 1.0"}, {"crossViewAng", "0.99"},
    {"crossSize", "0.55"}, {"lifetimeMS", "3000"}, {"blurLifetime", "0.2"}, {"blurWidth", "0.25"},
    {"blurColor", "0.4 0.0 0.0 1.0"}, {"texture[0]", "special/blasterBolt"},
    {"texture[1]", "special/blasterBoltCross"},
};

// weapons/plasma.cs PlasmaBolt
static const Fields kPlasma = {
    {"projectileShapeName", "plasmabolt.dts"}, {"scale", "2.0 2.0 2.0"}, {"faceViewer", "true"},
    {"directDamage", "0.0"}, {"hasDamageRadius", "true"}, {"indirectDamage", "0.45"}, {"damageRadius", "4.0"},
    {"explosion", "PlasmaBoltExplosion"}, {"splash", "PlasmaSplash"}, {"dryVelocity", "70.0"},
    {"wetVelocity", "-1"}, {"velInheritFactor", "0.3"}, {"fizzleTimeMS", "2000"}, {"lifetimeMS", "3000"},
    {"explodeOnDeath", "false"}, {"reflectOnWaterImpactAngle", "0.0"}, {"explodeOnWaterImpact", "true"},
    {"deflectionOnWaterImpact", "0.0"}, {"fizzleUnderwaterMS", "-1"}, {"activateDelayMS", "-1"},
    {"size[0]", "0.2"}, {"size[1]", "0.5"}, {"size[2]", "0.1"}, {"numFlares", "35"},
    {"flareColor", "1 0.75 0.25"}, {"flareModTexture", "flaremod"}, {"flareBaseTexture", "flarebase"},
    {"sound", "PlasmaProjectileSound"}, {"fireSound", "PlasmaFireSound"}, {"wetFireSound", "PlasmaFireWetSound"},
    {"hasLight", "true"}, {"lightRadius", "3.0"}, {"lightColor", "1 0.75 0.25"},
};

// vehicles/vehicle_bomber.cs BomberBomb (gravityMod = 20 / mabs($Classic::gravSetting), taken as 1)
static const Fields kBomb = {
    {"projectileShapeName", "bomb.dts"}, {"emitterDelay", "-1"}, {"directDamage", "0.0"},
    {"hasDamageRadius", "true"}, {"indirectDamage", "1.1"}, {"damageRadius", "30"},
    {"kickBackStrength", "4500"}, {"explosion", "VehicleBombExplosion"}, {"velInheritFactor", "1.0"},
    {"grenadeElasticity", "0.25"}, {"grenadeFriction", "0.4"}, {"armingDelayMS", "2000"},
    {"muzzleVelocity", "0.1"}, {"drag", "0.3"}, {"gravityMod", "1"}, {"minRotSpeed", "60.0 0.0 0.0"},
    {"maxRotSpeed", "80.0 0.0 0.0"}, {"scale", "1.0 1.0 1.0"}, {"sound", "BomberBombProjectileSound"},
};

// weapons/flaregrenade.cs FlareGrenadeProj
static const Fields kFlare = {
    {"projectileShapeName", "grenade_projectile.dts"}, {"emitterDelay", "-1"}, {"directDamage", "0.0"},
    {"hasDamageRadius", "false"}, {"kickBackStrength", "1500"}, {"useLensFlare", "false"},
    {"sound", "FlareGrenadeBurnSound"}, {"explosion", "FlareGrenadeExplosion"}, {"velInheritFactor", "0.5"},
    {"texture[0]", "special/flare3"}, {"texture[1]", "special/LensFlare/flare00"}, {"size", "4.0"},
    {"baseEmitter", "FlareEmitter"}, {"grenadeElasticity", "0.35"}, {"grenadeFriction", "0.2"},
    {"armingDelayMS", "6000"}, {"muzzleVelocity", "15.0"}, {"drag", "0.1"}, {"gravityMod", "0.15"},
};

// weapons/grenadelauncher.cs BasicGrenade
static const Fields kGrenade = {
    {"projectileShapeName", "grenade_projectile.dts"}, {"emitterDelay", "-1"}, {"directDamage", "0.0"},
    {"hasDamageRadius", "true"}, {"indirectDamage", "0.40"}, {"damageRadius", "15.0"},
    {"kickBackStrength", "1500"}, {"bubbleEmitTime", "1.0"}, {"sound", "GrenadeProjectileSound"},
    {"explosion", "GrenadeExplosion"}, {"underwaterExplosion", "UnderwaterGrenadeExplosion"},
    {"velInheritFactor", "0.85"}, {"splash", "GrenadeSplash"}, {"baseEmitter", "GrenadeSmokeEmitter"},
    {"bubbleEmitter", "GrenadeBubbleEmitter"}, {"grenadeElasticity", "0.30"}, {"grenadeFriction", "0.2"},
    {"armingDelayMS", "650"}, {"muzzleVelocity", "75.00"}, {"gravityMod", "1.9"},
};

int main() {
    DataBlockPack::registerAll();

    const char* classes[] = {
        "ProjectileData", "LinearProjectileData", "GrenadeProjectileData", "SeekerProjectileData",
        "SniperProjectileData", "ShockLanceProjectileData", "ELFProjectileData", "RepairProjectileData",
        "TargetProjectileData", "TracerProjectileData", "EnergyProjectileData", "LinearFlareProjectileData",
        "BombProjectileData", "FlareProjectileData"};
    for (const char* name : classes) {
        assert(DataBlockPack::find(name));
        check(name); // constructor defaults
    }

    // Defaults as the engine sends them.
    {
        auto r = check("ProjectileData");
        assert(r.decoded.emitterDelayMS == -1 && near(r.decoded.bubbleEmitTime, 0.5f));
        assert(!r.decoded.hasProjectileScale && !r.decoded.projectileHasLight);
        assert(r.decoded.projectileDepthTolerance == 5.0f);
    }
    {
        auto r = check("LinearProjectileData");
        assert(r.decoded.projectileDryVelocity == 5.0f && r.decoded.projectileLifetimeMS == 1024);
        assert(r.decoded.projectileFizzleTimeMS == 1024);
    }
    {
        auto r = check("GrenadeProjectileData");
        assert(r.decoded.grenadeArmingDelayMS == 3008 && r.decoded.projectileLifetimeMS == 20000);
        assert(r.decoded.grenadeElasticity == 0.999f && r.decoded.grenadeFriction == 0.3f);
    }
    {
        auto r = check("SniperProjectileData");
        assert(r.decoded.sniperBeam.valid && r.decoded.sniperBeam.textures.size() == 12);
        assert(r.decoded.sniperBeam.textures[11] == "special/sniper00");
        assert(near(r.decoded.projectileLightColor[0], 0.4f));
    }
    {
        auto r = check("RepairProjectileData");
        assert(r.decoded.linkBeam.texture == "special/redbump2" && r.decoded.linkBeam.cutoffAngle == 40.0f);
    }
    {
        auto r = check("FlareProjectileData");
        assert(r.decoded.projectileMaterialSizes[0] == 50.0f && r.decoded.projectileUseLensFlare);
    }

    // Retail datablocks.
    {
        auto r = check("LinearProjectileData", kDisc, kRefs);
        const auto& d = r.decoded;
        assert(d.shapeFile == "disc.dts");
        assert(d.projectileExplosionRef == wire(11) && d.projectileUnderwaterExplosionRef == wire(12));
        assert(d.projectileSplashRef == wire(13) && d.projectileSoundRef == wire(10) && d.projectileBaseEmitterRef == 0);
        assert(d.projectileDryVelocity == 95.0f && d.projectileWetVelocity == 55.0f);
        assert(d.projectileLifetimeMS == 5024 && d.projectileFizzleTimeMS == 5024); // tick rounded
        assert(d.projectileExplodeOnDeath && d.projectileExplodeOnWaterImpact);
        assert(d.projectileHasLight && near(d.projectileLightRadius, 6.0f, 20.0f / 255.0f));
        assert(near(d.projectileLightColor[2], 0.5f, 1.0f / 127.0f));
    }
    {
        auto r = check("TracerProjectileData", kChaingun, kRefs);
        const auto& d = r.decoded;
        assert(d.projectileDryVelocity == 700.0f && d.projectileLifetimeMS == 3008);
        assert(d.projectileTracerLength == 15.0f && d.projectileTracerWidth == 0.10f);
        assert(d.projectileTracerMinPixels == 6.0f && !d.projectileTracerAlpha && d.projectileRenderCross);
        assert(d.projectileCrossViewAngle == 0.990f && d.projectileCrossSize == 0.20f);
        assert(near(d.projectileMaterialColor[0], 211.0f / 255.0f) && near(d.projectileMaterialColor[3], 0.75f));
        assert(d.projectileMaterialTextures.size() == 2 && d.projectileMaterialTextures[1] == "special/tracercross");
        assert(d.projectileDecalRefs.size() == 6 && d.projectileDecalRefs[0] == wire(23) && d.projectileDecalRefs[5] == wire(28));
    }
    {
        auto r = check("GrenadeProjectileData", kMortar, kRefs);
        const auto& d = r.decoded;
        assert(d.shapeFile == "mortar_projectile.dts" && d.projectileDepthTolerance == 10.0f);
        assert(d.projectileBaseEmitterRef == wire(33) && d.projectileBubbleEmitterRef == wire(34));
        assert(d.grenadeArmingDelayMS == 1504 && d.grenadeElasticity == 0.15f);
        assert(d.grenadeFriction == 0.4f && d.grenadeGravityMod == 1.1f);
        assert(d.projectileHasUnderwaterLightColor && near(d.projectileUnderwaterLightColor[2], 0.2f, 1.0f / 127.0f));
    }
    check("GrenadeProjectileData", kGrenade, kRefs);
    {
        auto r = check("SeekerProjectileData", kMissile, kRefs);
        const auto& d = r.decoded;
        assert(d.shapeFile == "weapon_missile_projectile.dts" && d.bubbleEmitTime == 1.0f);
        assert(d.projectileDelayEmitterRef == wire(43) && d.seekerUseFlechette && d.seekerFlechetteDelayMS == 550);
    }
    {
        auto r = check("ELFProjectileData", kELF, kRefs);
        const auto& l = r.decoded.linkBeam;
        assert(l.valid && l.elf && l.beamRange == 38.0f && near(l.width, 0.2f, 1e-6f));
        assert(l.texture == "special/ELFBeam" && l.lightningTexture == "special/ELFLightning");
        assert(l.flareTexture == "special/BlueImpact" && l.lightningDist == 0.15f);
        assert(r.decoded.projectileFireSoundRef == wire(50) && r.decoded.projectileWetFireSoundRef == wire(51));
    }
    {
        auto r = check("SniperProjectileData", kSniper, kRefs);
        const auto& b = r.decoded.sniperBeam;
        assert(b.valid && near(b.color[0], 1.0f) && near(b.color[1], 0.1f) && b.fadeTime == 1.0f);
        assert(b.startWidth == 0.145f && b.endWidth == 0.25f && b.pulseLength == 0.150f);
        assert(b.textures[2] == "special/laserrip01" && b.textures[11] == "special/sniper00");
    }
    {
        auto r = check("ShockLanceProjectileData", kShocker, kRefs);
        const auto& s = r.decoded.shockLance;
        assert(s.valid && s.zapDuration == 1.0f && s.lightningFreq == 25.0f && s.lightningWidth == 0.05f);
        assert(s.startWidth[1] == 0.3f && s.endWidth[0] == 0.6f && s.boltSpeed[1] == -0.5f && s.texWrap[0] == 1.5f);
        assert(s.textures.size() == 4 && s.textures[3] == "special/ELFBeam");
        assert(r.decoded.projectileSoundRef == 0);
    }
    {
        auto r = check("RepairProjectileData", kRepair, kRefs);
        const auto& l = r.decoded.linkBeam;
        assert(l.valid && !l.elf && l.beamRange == 10.0f && l.texRepeat == 0.20f && l.cutoffAngle == 25.0f);
        assert(l.flareTexture == "special/redflare");
    }
    {
        auto r = check("TargetProjectileData", kTargeter, kRefs);
        const auto& beam = r.decoded.targetBeam;
        assert(beam.valid && beam.maxRange == 1000.0f);
        assert(near(beam.color[0], 0.1f, 1.0f / 255.0f) && beam.color[1] == 1.0f);
        assert(beam.startWidth == 0.20f && beam.pulseWidth == 0.15f);
        assert(beam.flareAngle == 3.0f && beam.maxFlareSize == 400.0f);
        assert(beam.pulseSpeed == 6.0f && beam.pulseLength == 0.150f);
        assert(beam.textures.size() == 4 && beam.textures[0] == "special/nonlingradient" &&
               beam.textures[2] == "special/pulse");
    }
    {
        auto r = check("EnergyProjectileData", kEnergyBolt, kRefs);
        const auto& d = r.decoded;
        assert(d.projectileIsEnergyBolt && d.hasProjectileScale && d.projectileScale.y == 20.0f);
        assert(d.projectileCrossViewAngle == 0.99f && d.projectileCrossSize == 0.55f);
        assert(d.projectileBlurLifetime == 0.2f && d.projectileBlurColor[0] == 0.4f);
        assert(d.grenadeArmingDelayMS == 512 && d.projectileLifetimeMS == 3000 && d.grenadeGravityMod == 0.0f);
        assert(d.projectileMaterialTextures[0] == "special/blasterBolt");
    }
    {
        auto r = check("LinearFlareProjectileData", kPlasma, kRefs);
        const auto& d = r.decoded;
        assert(d.faceViewer && d.projectileScale.x == 2.0f && d.projectileWetVelocity == -1.0f);
        assert(d.projectileFlareCount == 35 && d.projectileMaterialSizes[1] == 0.5f);
        assert(d.projectileMaterialTextures[0] == "flaremod" && d.projectileMaterialTextures[1] == "flarebase");
        assert(near(d.projectileMaterialColor[1], 0.75f));
        assert(d.projectileLifetimeMS == 3008 && d.projectileFizzleTimeMS == 2016);
    }
    {
        auto r = check("BombProjectileData", kBomb, kRefs);
        assert(r.decoded.shapeFile == "bomb.dts" && !r.decoded.hasProjectileScale);
        assert(r.decoded.grenadeArmingDelayMS == 2016 && r.decoded.grenadeElasticity == 0.25f);
    }
    {
        auto r = check("FlareProjectileData", kFlare, kRefs);
        const auto& d = r.decoded;
        assert(d.projectileMaterialSizes[0] == 4.0f && !d.projectileUseLensFlare);
        assert(d.projectileMaterialTextures[1] == "special/LensFlare/flare00" && d.projectileBaseEmitterRef == wire(112));
    }
    // ProjectileData itself has no retail datablock; reuse the disc's base fields.
    {
        auto r = check("ProjectileData", kDisc, kRefs);
        assert(r.decoded.shapeFile == "disc.dts" && r.decoded.projectileExplosionRef == wire(11));
    }

    printf("datablock_pack_projectiles: ok\n");
    return 0;
}
