#include "game/weapon.h"
#include "audio/audio_system.h"
#include "core/engine.h"
#include "game/game.h"
#include "game/collision.h"
#include <cmath>

#define WEAPON(_name, _proj, _rate, _reload, _dmg, _speed, _energy, _ammo, _splash, _auto, _alt, _fireSnd, _expSnd) \
    { _name, _proj, _rate, _reload, _dmg, _speed, _energy, _ammo, _splash, _auto, _alt, _fireSnd, _expSnd }

#define SND(path) "audio/fx/weapons/" path ".wav"

const WeaponData gWeaponTable[] = {
    WEAPON("Spinfusor",    ProjectileType::Disc,    0.8f,  1.5f, 60.0f, 50.0f,  5.0f,  40, 5.0f, false, false, SND("spinfusor/spinfusor_fire"), SND("spinfusor/spinfusor_impact")),
    WEAPON("Blaster",      ProjectileType::Bolt,    0.2f,  0.0f, 20.0f, 80.0f,  3.0f,  -1, 2.0f, true,  false, SND("blaster/blaster_fire"),        SND("blaster/blaster_impact")),
    WEAPON("Chaingun",     ProjectileType::Hitscan, 0.08f, 2.0f, 10.0f,  0.0f,  0.0f, 200, 0.0f, true,  false, SND("chaingun/chaingun_fire"),      nullptr),
    WEAPON("GrenadeLauncher", ProjectileType::Grenade, 0.6f, 1.2f, 80.0f, 25.0f, 10.0f,  15, 6.0f, false, false, SND("grenade_launcher/grenade_launcher_fire"), SND("grenade_launcher/grenade_explosion")),
    WEAPON("SniperRifle",  ProjectileType::Hitscan, 0.5f,  2.5f, 100.0f, 0.0f,  0.0f,  10, 0.0f, false, false, SND("sniper_rifle/sniper_rifle_fire"), nullptr),
    WEAPON("Mortar",       ProjectileType::Mortar,  1.2f,  2.5f, 150.0f, 35.0f, 20.0f,   8, 8.0f, false, false, SND("mortar/mortar_fire"),          SND("mortar/mortar_explosion")),
    WEAPON("PlasmaGun",    ProjectileType::Bolt,    0.4f,  1.0f, 35.0f, 30.0f,  4.0f,  30, 4.0f, true,  false, SND("plasma_gun/plasma_gun_fire"),  SND("plasma_gun/plasma_gun_impact")),
    WEAPON("Shocklance",   ProjectileType::Hitscan, 0.6f,  0.0f, 90.0f,  0.0f,  0.0f,  -1, 0.0f, false, true,  SND("shocklance/shocklance_fire"),  nullptr),
    WEAPON("RepairTool",   ProjectileType::Bolt,    0.3f,  0.0f, -15.0f,30.0f,  2.0f,  -1, 2.0f, true,  false, SND("repair_tool/repair_tool_fire"), nullptr),
    WEAPON("ELF",          ProjectileType::Bolt,    0.5f,  1.5f,  5.0f, 35.0f,  0.0f,  20, 3.0f, false, false, SND("elf/elf_fire"),               SND("elf/elf_impact")),
};

const int gWeaponCount = sizeof(gWeaponTable) / sizeof(gWeaponTable[0]);

bool Weapon::canFire(float energy) const {
    if (type < 0 || type >= gWeaponCount) return false;
    if (reloading) return false;
    // Native weapon state is rejected when a decoded/script value is not a
    // real number.  Comparisons with NaN would otherwise pass both cooldown
    // and energy checks, and the subsequent subtraction would poison the
    // player's energy and projectile state.
    if (!std::isfinite(energy) || !std::isfinite(fireTimer) || fireTimer > 0)
        return false;
    if (energy < gWeaponTable[type].energyCost) return false;
    // A negative count is the native unlimited-ammo sentinel, but only for
    // datablocks that do not define a finite reserve.  Treating a malformed
    // finite reserve as unlimited lets an empty weapon fire forever.
    if (gWeaponTable[type].maxAmmo > 0 && ammo < 0) return false;
    if (ammo == 0) return false;
    return true;
}

void Weapon::updateTimers(float dt) {
    // Timer state is frame-driven.  A paused frame must not mutate it, and a
    // malformed delta must not turn a usable weapon into a permanently
    // cooling-down or reloading weapon through NaN propagation.
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (type < 0 || type >= gWeaponCount) {
        reloading = false;
        reloadTimer = 0.0f;
        firing = false;
        return;
    }
    // A malformed replicated cooldown must recover to the native idle state;
    // comparing or subtracting NaN would otherwise leave the weapon stuck.
    if (!std::isfinite(fireTimer)) {
        fireTimer = 0.0f;
        firing = false;
    }
    if (fireTimer > 0) fireTimer -= dt;
    if (fireTimer <= 0) {
        fireTimer = 0;
        firing = false;
    }
    if (reloading) {
        // A malformed replicated reload timer must not leave the weapon stuck
        // in the reloading state forever; native weapon state falls back to
        // idle when its timer is not a usable number.
        if (!std::isfinite(reloadTimer)) {
            reloading = false;
            reloadTimer = 0.0f;
            return;
        }
        reloadTimer -= dt;
        if (reloadTimer <= 0) {
            reloading = false;
            reloadTimer = 0.0f;
            int maxAmmo = gWeaponTable[type].maxAmmo;
            if (maxAmmo > 0) ammo = maxAmmo;
        }
    }
}

Point3F computeProjectileSpawn(const Point3F& cameraPos, const Point3F& targetDir, float spread) {
    // The native muzzle offset is measured in world units, so callers passing
    // an unnormalised aim vector must not scale the offset (or spread basis).
    const float directionLength = std::sqrt(targetDir.x * targetDir.x +
                                             targetDir.y * targetDir.y +
                                             targetDir.z * targetDir.z);
    // A malformed camera target can produce an infinite vector.  Dividing an
    // infinite component by its infinite length yields NaN and poisons the
    // muzzle position, so treat non-finite aim vectors as zero-length.
    const Point3F direction = std::isfinite(directionLength) && directionLength > 1.0e-6f &&
        std::isfinite(targetDir.x) && std::isfinite(targetDir.y) &&
        std::isfinite(targetDir.z)
        ? Point3F{targetDir.x / directionLength, targetDir.y / directionLength,
                  targetDir.z / directionLength}
        : Point3F{0, 0, 0};
    Point3F spawn = cameraPos;
    spawn.x += direction.x * 0.5f;
    spawn.y += direction.y * 0.5f;
    spawn.z += direction.z * 0.5f;

    if (spread > 0) {
        float r1 = ((float)rand() / RAND_MAX - 0.5f) * spread;
        float r2 = ((float)rand() / RAND_MAX - 0.5f) * spread;
        Point3F right = {direction.z, 0, -direction.x};
        float rlen = sqrtf(right.x * right.x + right.z * right.z);
        // The horizontal basis collapses when aiming straight up or down.
        // Keep weapon spread two-dimensional instead of silently dropping r1.
        const bool hasHorizontalBasis = rlen > 1e-6f;
        if (hasHorizontalBasis) {
            right.x /= rlen;
            right.z /= rlen;
        } else {
            right = {1, 0, 0};
        }
        Point3F up = hasHorizontalBasis ? Point3F{0, 1, 0} : Point3F{0, 0, 1};
        Point3F spreadOffset = {right.x * r1 + up.x * r2, right.y * r1 + up.y * r2,
                                right.z * r1 + up.z * r2};
        spawn.x += spreadOffset.x;
        spawn.y += spreadOffset.y;
        spawn.z += spreadOffset.z;
    }

    return spawn;
}

void updateProjectile(Projectile& p, float dt) {
    if (!p.active) return;
    // A paused or malformed tick must not move the projectile backward or
    // extend its lifetime indefinitely.
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (projectileAdvancesPosition(p.type)) p.previousPos = p.pos;
    if (!projectileLifetimeStep(p.lifetime, dt)) {
        p.active = false;
        return;
    }

    if (p.type == ProjectileType::Disc || p.type == ProjectileType::Grenade ||
        p.type == ProjectileType::Mortar) {
        const auto zone = Engine::instance().game().world().physicalZoneEffect(p.pos);
        const Point3F acceleration = projectileZoneAcceleration(
            Engine::instance().game().getGravity(), zone.gravityMod, zone.appliedForce);
        p.vel.x += acceleration.x * dt;
        p.vel.y += acceleration.y * dt;
        p.vel.z += acceleration.z * dt;
    }

    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += p.vel.z * dt;
}

void loadWeaponSounds(Weapon& w) {
    if (w.type < 0 || w.type >= gWeaponCount) return;
    auto& audio = Engine::instance().audio();
    if (!audio.config().enabled) return;
    const WeaponData& wd = gWeaponTable[w.type];
    if (!w.fireSound && wd.fireSoundPath) {
        w.fireSound = audio.loadSound(wd.fireSoundPath);
    }
    if (!w.explosionSound && wd.explosionSoundPath) {
        w.explosionSound = audio.loadSound(wd.explosionSoundPath);
    }
}

bool checkProjectileCollision(Projectile& p, float& groundHeight, Point3F& impactNormal) {
    auto& world = Engine::instance().game().world();
    groundHeight = world.getFloorHeight(p.pos.x, p.pos.y, p.pos.z);
    constexpr float sampleSpacing = 0.25f;
    const float left = world.getHeight(p.pos.x - sampleSpacing, p.pos.z);
    const float right = world.getHeight(p.pos.x + sampleSpacing, p.pos.z);
    const float back = world.getHeight(p.pos.x, p.pos.z - sampleSpacing);
    const float front = world.getHeight(p.pos.x, p.pos.z + sampleSpacing);
    impactNormal = (left > -1.0e9f && right > -1.0e9f &&
                    back > -1.0e9f && front > -1.0e9f)
        ? terrainNormalFromHeights(left, right, back, front, sampleSpacing)
        : Point3F{0, 1, 0};

    // Sweep the full projectile segment against the authored surface. A fast
    // projectile can cross terrain between frames even when its final point
    // has already passed below the surface.
    // Interior columns can contain several upward-facing surfaces (for
    // example, stacked floors).  The unbounded height query returns the top
    // one, which makes a projectile on a lower floor collide with geometry
    // above it.  Resolve each endpoint against the floor at that endpoint's
    // height, while the interior raycast below still handles ceilings and
    // walls.
    const float previousSurface = world.getFloorHeight(
        p.previousPos.x, p.previousPos.y, p.previousPos.z);
    const float currentSurface = world.getFloorHeight(
        p.pos.x, p.pos.y, p.pos.z);
    float crossingT = 0.0f;
    // A projectile can climb into a rising terrain face while its y velocity
    // is positive; collision follows the swept segment, not velocity sign.
    if (previousSurface > -1.0e9f && currentSurface > -1.0e9f &&
        segmentSurfaceCrossing(p.previousPos.y, previousSurface,
                               p.pos.y, currentSurface, crossingT)) {
        p.pos = {p.previousPos.x + (p.pos.x - p.previousPos.x) * crossingT,
                 p.previousPos.y + (p.pos.y - p.previousPos.y) * crossingT,
                 p.previousPos.z + (p.pos.z - p.previousPos.z) * crossingT};
        groundHeight = previousSurface + (currentSurface - previousSurface) * crossingT;
        // The cached normal was sampled at the segment endpoint.  On a slope
        // that can be a different facet from the one actually struck, causing
        // grenades to bounce in the wrong direction.  Sample the authored
        // surface again at the interpolated contact point.
        const float impactLeft = world.getHeight(p.pos.x - sampleSpacing, p.pos.z);
        const float impactRight = world.getHeight(p.pos.x + sampleSpacing, p.pos.z);
        const float impactBack = world.getHeight(p.pos.x, p.pos.z - sampleSpacing);
        const float impactFront = world.getHeight(p.pos.x, p.pos.z + sampleSpacing);
        if (impactLeft > -1.0e9f && impactRight > -1.0e9f &&
            impactBack > -1.0e9f && impactFront > -1.0e9f)
            impactNormal = terrainNormalFromHeights(impactLeft, impactRight,
                                                    impactBack, impactFront,
                                                    sampleSpacing);
        if (p.type == ProjectileType::Grenade && p.bounceCount < 3) {
            if (!projectileVelocityIntoSurface(p.vel, impactNormal)) {
                p.pos.x += impactNormal.x * 0.001f;
                p.pos.y += impactNormal.y * 0.001f;
                p.pos.z += impactNormal.z * 0.001f;
                return false;
            }
            p.pos.y = groundHeight;
            reflectProjectileVelocity(p.vel, impactNormal);
            p.vel.x *= 0.7f;
            p.vel.z *= 0.7f;
            p.bounceCount++;
            markProjectileBounce(p);
            return false;
        }
        p.pos.y = groundHeight;
        p.hasImpacted = true;
        return true;
    }

    // A projectile that starts inside terrain and moves out is already leaving
    // a contact resolved by an earlier frame.  Do not snap it back to the
    // previous embedded point: that would make a bouncing grenade collide
    // again immediately and reverse its newly applied upward velocity.
    if (previousSurface > -1.0e9f && currentSurface > -1.0e9f &&
        projectileExitsSurface(p.previousPos.y, previousSurface,
                               p.pos.y, currentSurface)) {
        return false;
    }

    // Check terrain height at the final position.
    float th = groundHeight;
    // Contact must resolve even when a projectile starts inside the surface
    // or is moving upward after penetrating it in a prior frame.
    if (projectileTouchesSurface(p.pos.y, th)) {
        p.pos.y = th;
        if (p.type == ProjectileType::Grenade && p.bounceCount < 3) {
            if (!projectileVelocityIntoSurface(p.vel, impactNormal)) {
                p.pos.x += impactNormal.x * 0.001f;
                p.pos.y += impactNormal.y * 0.001f;
                p.pos.z += impactNormal.z * 0.001f;
                return false;
            }
            reflectProjectileVelocity(p.vel, impactNormal);
            p.vel.x *= 0.7f;
            p.vel.z *= 0.7f;
            p.bounceCount++;
            markProjectileBounce(p);
            return false; // still bouncing, not yet impacted
        }
        p.hasImpacted = true;
        return true;
    }

    // Check interior collision mesh (raycast)
    const auto& collision = world.collision();
    if (collision.loaded) {
        float t;
        Point3F hitPos, hitNorm;
        Point3F dir = {p.pos.x - p.previousPos.x,
                       p.pos.y - p.previousPos.y,
                       p.pos.z - p.previousPos.z};
        float speed = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (projectileHasSweepLength(speed)) {
            dir.x /= speed; dir.y /= speed; dir.z /= speed;
            const float maxDist = speed;
            const Point3F origin = p.previousPos;
            if (collision.raycast(origin, dir, maxDist, t, hitPos, hitNorm)) {
                p.pos = hitPos;
                impactNormal = hitNorm;
                p.hasImpacted = true;

                if (p.type == ProjectileType::Grenade && p.bounceCount < 3) {
                    if (!projectileVelocityIntoSurface(p.vel, hitNorm)) {
                        p.pos.x += hitNorm.x * 0.001f;
                        p.pos.y += hitNorm.y * 0.001f;
                        p.pos.z += hitNorm.z * 0.001f;
                        p.hasImpacted = false;
                        return false;
                    }
                    float dot = p.vel.x * hitNorm.x + p.vel.y * hitNorm.y + p.vel.z * hitNorm.z;
                    p.vel.x = (p.vel.x - 2 * dot * hitNorm.x) * 0.5f;
                    p.vel.y = (p.vel.y - 2 * dot * hitNorm.y) * 0.5f;
                    p.vel.z = (p.vel.z - 2 * dot * hitNorm.z) * 0.5f;
                    p.bounceCount++;
                    p.hasImpacted = false;
                    return false;
                }
                return true;
            }
        }
    }

    return false;
}
