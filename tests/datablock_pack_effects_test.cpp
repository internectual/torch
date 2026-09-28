// Round trip of the 'effects' datablock writers through Torch's reader.
#include "sim/datablock_pack.h"
#include "tests/datablock_pack_harness.h"
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// The reader returns datablock references as the raw 11-bit value (id - 3).
uint32_t wire(uint32_t id) { return id - DataBlockPack::ObjectIdFirst; }

using Fields = std::map<std::string, std::string>;
using Refs = std::map<std::string, uint32_t>;

PackRoundTrip exact(const std::string& className, const Fields& fields = {}, const Refs& refs = {}) {
    PackRoundTrip r = packRoundTrip(className, fields, refs);
    if (!r.exact()) {
        fprintf(stderr, "not exact: %s (%zu fields)\n", className.c_str(), fields.size());
        ++failures;
    }
    return r;
}

const Refs refs = {
    {"PlasmaBarrelExpSound", 40},
    {"FireballAtmosphereExplosionEmitter", 41},
    {"FireballAtmosphereCrescentEmitter", 42},
    {"FireballAtmosphereSubExplosion1", 43},
    {"FireballAtmosphereSubExplosion2", 44},
    {"FireballAtmosphereSubExplosion3", 45},
    {"TurretShockwave", 46},
    {"FireballAtmosphereEmitter", 47},
    {"FireballAtmosphereBoltExplosion", 48},
    {"PlayerSplashParticle", 49},
    {"PlayerSplashEmitter", 50},
    {"PlayerSplashMistEmitter", 51},
    {"Universal_Rain_Light_1", 52},
    {"LightningHitSound", 53},
    {"thunderCrash1", 54},
    {"thunderCrash2", 55},
    {"thunderCrash3", 56},
    {"thunderCrash4", 57},
    {"FireballAtmosphereDebris", 58},
    {"FireballAtmosphereParticle", 59},
    {"HumanArmorJetParticle", 60},
    {"FireballAtmosphereDebris2", 2050},
};

void explosion() {
    auto d = exact("ExplosionData");
    CHECK(d.decoded.hasExplosion);
    CHECK(near(d.decoded.explosion.playSpeed, 1.0f));
    CHECK(d.decoded.explosion.lifetimeMS == 992); // 1000 >> 5 << 5
    CHECK(d.decoded.explosion.times.size() == 2);   // {0, 1}
    CHECK(d.decoded.explosion.debrisThetaMax == 90);
    CHECK(d.decoded.explosion.particleDensity == 10);

    // weather.cs FireballAtmosphereBoltExplosion.
    auto r = exact("ExplosionData", {
        {"soundProfile", "PlasmaBarrelExpSound"},
        {"particleEmitter", "FireballAtmosphereExplosionEmitter"},
        {"particleDensity", "250"},
        {"particleRadius", "1.25"},
        {"faceViewer", "true"},
        {"emitter[0]", "FireballAtmosphereCrescentEmitter"},
        {"subExplosion[0]", "FireballAtmosphereSubExplosion1"},
        {"subExplosion[1]", "FireballAtmosphereSubExplosion2"},
        {"subExplosion[2]", "FireballAtmosphereSubExplosion3"},
        {"shakeCamera", "true"},
        {"camShakeFreq", "10.0 9.0 9.0"},
        {"camShakeAmp", "70.0 70.0 70.0"},
        {"camShakeDuration", "1.3"},
        {"camShakeRadius", "15.0"},
    }, refs);
    const auto& e = r.decoded.explosion;
    CHECK(e.soundProfileRef == wire(40));
    CHECK(e.particleEmitterRef == wire(41));
    CHECK(e.particleDensity == 250);
    CHECK(near(e.particleRadius, 1.25f));
    CHECK(e.faceViewer);
    CHECK(e.shakeCamera);
    CHECK(near(e.shakeFrequency[1], 9.0f));
    CHECK(near(e.shakeAmplitude[0], 70.0f));
    CHECK(near(e.shakeDuration, 1.3f));
    CHECK(e.emitterRefs.size() == 4 && e.emitterRefs[0] == wire(42) && e.emitterRefs[1] == 0);
    CHECK(e.subExplosionRefs.size() == 5 && e.subExplosionRefs[2] == wire(45) && e.subExplosionRefs[3] == 0);

    // weather.cs FireballAtmosphereSubExplosion3 (+ turret.cs shockwave, scale).
    auto s = exact("ExplosionData", {
        {"explosionShape", "effect_plasma_explosion.dts"},
        {"faceViewer", "true"},
        {"delayMS", "0"},
        {"offset", "0.0"},
        {"playSpeed", "0.7"},
        {"sizes[0]", "1.0 1.0 1.0"},
        {"sizes[1]", "2.0 2.0 2.0"},
        {"times[0]", "0.0"},
        {"times[1]", "1.0"},
        {"shockwave", "TurretShockwave"},
        {"explosionScale", "2 2 2"},
        {"lifetimeMS", "1500"},
        {"debrisVelocity", "12.5"},
        {"debris", "FireballAtmosphereDebris2"},
    }, refs);
    const auto& x = s.decoded.explosion;
    CHECK(x.shape == "effect_plasma_explosion.dts");
    CHECK(near(x.playSpeed, 0.7f, 0.05f));
    CHECK(x.times.size() == 2 && near(x.times[1], 1.0f));
    CHECK(x.sizes.size() == 2 && near(x.sizes[1][0], 2.0f));
    CHECK(x.shockwaveRef == wire(46));
    CHECK(x.debrisRef == wire(2050));
    CHECK(near(x.scale[2], 2.0f));
    CHECK(x.lifetimeMS == (1500 >> 5 << 5));
    CHECK(near(x.debrisVelocity, 12.5f));

    // Four keys, none reaching 1.
    exact("ExplosionData", {{"times[0]", "0"}, {"times[1]", "0.2"}, {"times[2]", "0.5"}, {"times[3]", "0.9"}});
}

void debris() {
    exact("DebrisData");
    // weather.cs FireballAtmosphereDebris.
    exact("DebrisData", {
        {"emitters[0]", "FireballAtmosphereEmitter"},
        {"explosion", "FireballAtmosphereBoltExplosion"},
        {"explodeOnMaxBounce", "true"},
        {"elasticity", "0.0"},
        {"friction", "1.0"},
        {"lifetime", "100.0"},
        {"lifetimeVariance", "0.0"},
        {"numBounces", "0"},
        {"bounceVariance", "0"},
        {"ignoreWater", "false"},
    }, refs);
    // player.cs PlayerDebris (+ a shape and texture).
    auto player = exact("DebrisData", {
        {"explodeOnMaxBounce", "false"},
        {"elasticity", "0.35"},
        {"friction", "0.5"},
        {"lifetime", "4.0"},
        {"minSpinSpeed", "60"},
        {"maxSpinSpeed", "600"},
        {"numBounces", "5"},
        {"staticOnMaxBounce", "true"},
        {"gravModifier", "1.0"},
        {"useRadiusMass", "true"},
        {"baseRadius", "1"},
        {"velocity", "18.0"},
        {"velocityVariance", "12.0"},
        {"shapeName", "debris_player.dts"},
        {"texture", "special/debris"},
    }, refs);
    CHECK(player.decoded.debris.shape == "debris_player.dts");
    CHECK(player.decoded.debris.lifetimeMS == 4000);
    CHECK(player.decoded.debris.numBounces == 5 && player.decoded.debris.staticOnMaxBounce);
    CHECK(near(player.decoded.debris.velocity, 18.0f) && near(player.decoded.debris.velocityVariance, 12.0f));
    CHECK(near(player.decoded.debris.minSpin, 60.0f) && near(player.decoded.debris.maxSpin, 600.0f));
}

void splash() {
    auto d = exact("SplashData");
    CHECK(d.decoded.hasSplash);
    CHECK(near(d.decoded.splash.width, 4.0f));
    CHECK(d.decoded.splash.numSegments == 10);
    CHECK(near(d.decoded.splash.startRadius, 0.5f));
    CHECK(d.decoded.splash.colors.size() == 4 && d.decoded.splash.colors[0] == 0xffffffffu);

    // player.cs PlayerSplash.
    auto r = exact("SplashData", {
        {"numSegments", "15"},
        {"ejectionFreq", "15"},
        {"ejectionAngle", "40"},
        {"ringLifetime", "0.5"},
        {"lifetimeMS", "300"},
        {"velocity", "4.0"},
        {"startRadius", "0.0"},
        {"acceleration", "-3.0"},
        {"texWrap", "5.0"},
        {"texture", "special/water2"},
        {"emitter[0]", "PlayerSplashEmitter"},
        {"emitter[1]", "PlayerSplashMistEmitter"},
        {"colors[0]", "0.7 0.8 1.0 0.0"},
        {"colors[1]", "0.7 0.8 1.0 0.3"},
        {"colors[2]", "0.7 0.8 1.0 0.7"},
        {"colors[3]", "0.7 0.8 1.0 0.0"},
        {"times[0]", "0.0"},
        {"times[1]", "0.4"},
        {"times[2]", "0.8"},
        {"times[3]", "1.0"},
    }, refs);
    const auto& s = r.decoded.splash;
    CHECK(s.numSegments == 15);
    CHECK(near(s.ejectionAngle, 40.0f));
    CHECK(s.lifetimeMS == 300);
    CHECK(near(s.acceleration, -3.0f));
    CHECK(s.emitterRefs.size() == 3 && s.emitterRefs[0] == wire(50) && s.emitterRefs[1] == wire(51) &&
          s.emitterRefs[2] == 0);
    CHECK(s.explosionRef == 0);
    // ColorI bytes r, g, b, a: 0.7*255+0.5 = 179, 0.8 -> 204, 1.0 -> 255, 0.3 -> 77.
    CHECK(s.colors[1] == (179u | 204u << 8 | 255u << 16 | 77u << 24));
    CHECK(s.times.size() == 4 && near(s.times[1], 0.4f));
    CHECK(s.textures.size() == 2 && s.textures[0] == "special/water2" && s.textures[1].empty());
}

void shockwave() {
    auto d = exact("ShockwaveData");
    CHECK(d.decoded.hasShockwave);
    CHECK(near(d.decoded.shockwave.velocity, 30.0f));
    CHECK(d.decoded.shockwave.mapToTerrain);
    CHECK(d.decoded.shockwave.numVertSegments == 1);

    // turret.cs TurretShockwave.
    auto r = exact("ShockwaveData", {
        {"width", "6.0"},
        {"numSegments", "20"},
        {"numVertSegments", "2"},
        {"velocity", "8"},
        {"acceleration", "20.0"},
        {"lifetimeMS", "1500"},
        {"height", "1.0"},
        {"verticalCurve", "0.5"},
        {"mapToTerrain", "false"},
        {"renderBottom", "true"},
        {"texture[0]", "special/shockwave4"},
        {"texture[1]", "special/gradient"},
        {"texWrap", "6.0"},
        {"times[0]", "0.0"},
        {"times[1]", "0.5"},
        {"times[2]", "1.0"},
        {"colors[0]", "0.8 0.8 0.8 1.00"},
        {"colors[1]", "0.8 0.5 0.2 0.20"},
        {"colors[2]", "1.0 0.5 0.5 0.0"},
    }, refs);
    const auto& s = r.decoded.shockwave;
    CHECK(near(s.width, 6.0f));
    CHECK(s.numSegments == 20 && s.numVertSegments == 2);
    CHECK(s.lifetimeMS == 1500);
    CHECK(near(s.verticalCurve, 0.5f));
    CHECK(!s.mapToTerrain && s.renderBottom && !s.is2D);
    CHECK(s.textures.size() == 2 && s.textures[0] == "special/shockwave4" && s.textures[1] == "special/gradient");
    CHECK(s.times.size() == 4 && near(s.times[2], 1.0f) && near(s.times[3], 1.0f));
}

void particles() {
    auto e = exact("ParticleEmitterData");
    CHECK(e.decoded.hasEmitter);
    CHECK(e.decoded.emitter.ejectionPeriodMS == 100);
    CHECK(e.decoded.emitter.ejectionVelocity == 200);
    CHECK(e.decoded.emitter.thetaMax == 90);
    CHECK(e.decoded.emitter.orientOnVelocity);
    CHECK(e.decoded.emitter.particleRefs.empty());

    // player.cs PlayerSplashEmitter.
    auto r = exact("ParticleEmitterData", {
        {"ejectionPeriodMS", "1"},
        {"periodVarianceMS", "0"},
        {"ejectionVelocity", "3"},
        {"velocityVariance", "1.0"},
        {"ejectionOffset", "0.0"},
        {"thetaMin", "60"},
        {"thetaMax", "80"},
        {"phiReferenceVel", "0"},
        {"phiVariance", "360"},
        {"overrideAdvances", "false"},
        {"orientParticles", "true"},
        {"lifetimeMS", "100"},
        {"particles", "PlayerSplashParticle"},
    }, refs);
    const auto& m = r.decoded.emitter;
    CHECK(m.ejectionPeriodMS == 1);
    CHECK(m.ejectionVelocity == 300 && m.velocityVariance == 100);
    CHECK(m.thetaMin == 60 && m.thetaMax == 80);
    CHECK(m.orientParticles);
    CHECK(m.lifetimeMS == (100 >> 5 << 5));
    CHECK(m.particleRefs.size() == 1 && m.particleRefs[0] == wire(49));

    // Several particles (one unresolved, dropped as onAdd does), offset, phi.
    auto p = exact("ParticleEmitterData", {
        {"ejectionOffset", "1.5"},
        {"phiReferenceVel", "90"},
        {"phiVariance", "20"},
        {"particles", "PlayerSplashParticle Missing\tHumanArmorJetParticle"},
    }, refs);
    CHECK(p.decoded.emitter.ejectionOffset == 150);
    CHECK(p.decoded.emitter.phiReferenceVel == 90 && p.decoded.emitter.phiVariance == 20);
    CHECK(p.decoded.emitter.particleRefs.size() == 2 && p.decoded.emitter.particleRefs[1] == wire(60));

    auto d = exact("ParticleData");
    CHECK(d.decoded.hasParticle);
    CHECK(d.decoded.particle.keys.size() == 2);    // times {0, 1, 2, 2}
    CHECK(d.decoded.particle.lifetimeMS == 992);
    CHECK(near(d.decoded.particle.windCoefficient, 0.0f)); // default: not sent
    CHECK(d.decoded.particle.textures.empty());

    // player.cs PlayerSplashParticle.
    auto s = exact("ParticleData", {
        {"dragCoefficient", "1"},
        {"gravityCoefficient", "0.2"},
        {"inheritedVelFactor", "0.2"},
        {"constantAcceleration", "-0.0"},
        {"lifetimeMS", "600"},
        {"lifetimeVarianceMS", "0"},
        {"textureName", "special/droplet"},
        {"colors[0]", "0.7 0.8 1.0 1.0"},
        {"colors[1]", "0.7 0.8 1.0 0.5"},
        {"colors[2]", "0.7 0.8 1.0 0.0"},
        {"sizes[0]", "0.5"},
        {"sizes[1]", "0.5"},
        {"sizes[2]", "0.5"},
        {"times[0]", "0.0"},
        {"times[1]", "0.5"},
        {"times[2]", "1.0"},
    }, refs);
    const auto& q = s.decoded.particle;
    CHECK(near(q.dragCoefficient, 1.0f, 0.01f));
    CHECK(near(q.gravityCoefficient, 0.2f, 0.01f));
    CHECK(near(q.inheritedVelFactor, 0.2f, 0.01f));
    CHECK(q.lifetimeMS == (600 >> 5 << 5));
    CHECK(q.keys.size() == 3);
    CHECK(near(q.keys[1].alpha, 0.5f, 0.01f) && near(q.keys[1].time, 0.5f, 0.01f));
    CHECK(near(q.keys[2].size * 50.0f, 0.5f, 0.01f));
    CHECK(q.textures.size() == 1 && q.textures[0] == "special/droplet");

    // weather.cs FireballAtmosphereParticle: spin range, animated textures.
    Fields fireball = {
        {"dragCoeffiecient", "0.0"},
        {"gravityCoefficient", "-0.0"},
        {"inheritedVelFactor", "0.85"},
        {"lifetimeMS", "1600"},
        {"textureName", "particleTest"},
        {"useInvAlpha", "false"},
        {"spinRandomMin", "-100.0"},
        {"spinRandomMax", "100.0"},
        {"animateTexture", "true"},
        {"framesPerSec", "15"},
        {"colors[0]", "1.0 0.7 0.5 1.0"},
        {"colors[1]", "1.0 0.5 0.2 1.0"},
        {"colors[2]", "1.0 0.25 0.1 0.0"},
        {"sizes[0]", "10.0"},
        {"sizes[1]", "4.0"},
        {"sizes[2]", "2.0"},
        {"times[0]", "0.0"},
        {"times[1]", "0.2"},
        {"times[2]", "1.0"},
    };
    for (int i = 0; i < 26; ++i) {
        char key[32], value[64];
        snprintf(key, sizeof key, "animTexName[%02d]", i);
        snprintf(value, sizeof value, "special/Explosion/exp_%04d", 2 + 2 * i);
        fireball[key] = value;
    }
    auto f = exact("ParticleData", fireball, refs);
    CHECK(near(f.decoded.particle.spinRandomMin, -100.0f) && near(f.decoded.particle.spinRandomMax, 100.0f));
    CHECK(f.decoded.particle.textures.size() == 26);
    CHECK(f.decoded.particle.textures[0] == "special/Explosion/exp_0002");
    CHECK(f.decoded.particle.textures[25] == "special/Explosion/exp_0052");
    CHECK(near(f.decoded.particle.keys[0].size * 50.0f, 10.0f, 0.01f));

    // Wind, spin speed, invAlpha, four keys.
    auto w = exact("ParticleData", {
        {"windCoefficient", "0.5"}, {"spinSpeed", "30"}, {"useInvAlpha", "true"},
        {"times[1]", "0.3"}, {"times[2]", "0.6"}, {"times[3]", "0.9"},
    });
    CHECK(near(w.decoded.particle.windCoefficient, 0.5f));
    CHECK(near(w.decoded.particle.spinSpeed, 30.0f));
    CHECK(w.decoded.particle.useInvAlpha);
    CHECK(w.decoded.particle.keys.size() == 4);

    exact("ParticleEmissionDummyData");
    exact("ParticleEmissionDummyData", {{"timeMultiple", "0.5"}}); // particledummies.cs halftimeEmissionDummy
}

void decal() {
    auto d = exact("DecalData");
    CHECK(d.decoded.hasDecal);
    CHECK(near(d.decoded.decal.sizeX, 1.0f) && near(d.decoded.decal.sizeY, 1.0f));
    // player.cs LightMaleFootprint.
    auto r = exact("DecalData", {
        {"sizeX", "0.125"}, {"sizeY", "0.25"}, {"textureName", "special/footprints/L_male"},
    });
    CHECK(near(r.decoded.decal.sizeX, 0.125f) && near(r.decoded.decal.sizeY, 0.25f));
    CHECK(r.decoded.decal.texture == "special/footprints/L_male");
}

void weather() {
    auto d = exact("PrecipitationData");
    CHECK(d.decoded.hasPrecipitation);
    CHECK(d.decoded.precipitationType == 0 && near(d.decoded.precipitationMaxSize, 1.0f));

    // weather.cs Rain.
    auto r = exact("PrecipitationData", {
        {"type", "0"},
        {"soundProfile", "Universal_Rain_Light_1"},
        {"materialList", "raindrops.dml"},
        {"sizeX", "0.2"},
        {"sizeY", "0.45"},
        {"movingBoxPer", "0.35"},
        {"divHeightVal", "1.5"},
        {"sizeBigBox", "1"},
        {"topBoxSpeed", "20"},
        {"frontBoxSpeed", "30"},
        {"topBoxDrawPer", "0.5"},
        {"bottomDrawHeight", "40"},
        {"skipIfPer", "-0.3"},
        {"bottomSpeedPer", "1.0"},
        {"frontSpeedPer", "1.5"},
        {"frontRadiusPer", "0.5"},
    }, refs);
    CHECK(r.decoded.precipitationMaterialList == "raindrops.dml");
    CHECK(near(r.decoded.precipitationSizeX, 0.2f) && near(r.decoded.precipitationSizeY, 0.45f));
    // weather.cs Sand.
    auto s = exact("PrecipitationData", {{"type", "2"}, {"maxSize", "2"}, {"sizeX", "25"}});
    CHECK(s.decoded.precipitationType == 2 && near(s.decoded.precipitationMaxSize, 2.0f));
    CHECK(near(s.decoded.precipitationSizeX, 1.0f)); // onAdd: out of (0, 20]

    exact("LightningData");
    // lightning.cs DefaultStorm.
    exact("LightningData", {
        {"directDamageType", "13"},
        {"directDamage", "0.4"},
        {"strikeTextures[0]", "special/skyLightning"},
        {"strikeSound", "LightningHitSound"},
        {"thunderSounds[0]", "thunderCrash1"},
        {"thunderSounds[1]", "thunderCrash2"},
        {"thunderSounds[2]", "thunderCrash3"},
        {"thunderSounds[3]", "thunderCrash4"},
        {"thunderSounds[4]", "thunderCrash1"},
        {"thunderSounds[5]", "thunderCrash2"},
        {"thunderSounds[6]", "thunderCrash3"},
        {"thunderSounds[7]", "thunderCrash4"},
    }, refs);

    exact("FireballAtmosphereData");
    exact("FireballAtmosphereData", {{"fireball", "FireballAtmosphereDebris"}}, refs); // weather.cs Fireball
}

void retailEffects() {
    exact("EffectProfile");
    // player.cs ArmorJetEffect.
    exact("EffectProfile", {{"effectname", "armor/thrust"}, {"minDistance", "5.0"}, {"maxDistance", "10.0"}});

    exact("JetEffectData");
    // player.cs HumanArmorJetEffect.
    exact("JetEffectData", {
        {"texture", "special/jetExhaust02"},
        {"coolColor", "0.0 0.0 1.0 1.0"},
        {"hotColor", "0.2 0.4 0.7 1.0"},
        {"activateTime", "0.2"},
        {"deactivateTime", "0.05"},
        {"length", "0.75"},
        {"width", "0.2"},
        {"speed", "-15"},
        {"stretch", "2.0"},
        {"yOffset", "0.2"},
    });

    exact("RunningLightData");
    // No retail script defines one; fields per the retail layout.
    exact("RunningLightData", {
        {"radius", "3.0"},
        {"color", "1.0 0.2 0.2 1.0"},
        {"type", "1"},
        {"length", "2.0"},
        {"nodeName", "light0"},
        {"direction", "0 1 0"},
        {"offset", "0 0 0.5"},
        {"texture[0]", "special/runningLight"},
    });
}

} // namespace

int main() {
    DataBlockPack::registerAll();
    explosion();
    debris();
    splash();
    shockwave();
    particles();
    decal();
    weather();
    retailEffects();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("datablock_pack_effects_test: ok\n");
    return 0;
}
