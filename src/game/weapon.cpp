#include "game/weapon.h"
#include "audio/audio_system.h"
#include "core/engine.h"
#include "game/game.h"
#include <cmath>

#define WEAPON(_name, _proj, _rate, _reload, _dmg, _speed, _energy, _ammo, _splash, _auto, _alt, _fireSnd, _expSnd) \
    { _name, _proj, _rate, _reload, _dmg, _speed, _energy, _ammo, _splash, _auto, _alt, _fireSnd, _expSnd }

#define SND(path) "audio/fx/weapons/" path ".wav"

const WeaponData gWeaponTable[] = {
    WEAPON("Spinfusor",    ProjectileType::Disc,    0.8f,  1.5f, 60.0f, 50.0f,  5.0f,  40, 5.0f, false, false, SND("spinfusor/spinfusor_fire"), SND("spinfusor/spinfusor_explosion")),
    WEAPON("Blaster",      ProjectileType::Bolt,    0.2f,  0.0f, 20.0f, 80.0f,  3.0f,  -1, 2.0f, true,  false, SND("blaster/blaster_fire"),        SND("blaster/blaster_impact")),
    WEAPON("Chaingun",     ProjectileType::Hitscan, 0.08f, 2.0f, 10.0f,  0.0f,  0.0f, 200, 0.0f, true,  false, SND("chaingun/chaingun_fire"),      nullptr),
    WEAPON("GrenadeLauncher", ProjectileType::Grenade, 0.6f, 1.2f, 80.0f, 25.0f, 10.0f,  15, 6.0f, false, false, SND("grenade/grenade_fire"),       SND("grenade/grenade_explosion")),
    WEAPON("SniperRifle",  ProjectileType::Hitscan, 0.5f,  2.5f, 100.0f, 0.0f,  0.0f,  10, 0.0f, false, false, SND("sniper/sniper_fire"),          nullptr),
    WEAPON("Mortar",       ProjectileType::Mortar,  1.0f,  2.0f, 120.0f, 30.0f, 15.0f,  10, 7.0f, false, false, SND("mortar/mortar_fire"),          SND("mortar/mortar_explosion")),
};

const int gWeaponCount = sizeof(gWeaponTable) / sizeof(gWeaponTable[0]);

bool Weapon::canFire(float energy) const {
    if (type < 0 || type >= gWeaponCount) return false;
    if (reloading) return false;
    if (fireTimer > 0) return false;
    if (energy < gWeaponTable[type].energyCost) return false;
    if (ammo == 0) return false;
    return true;
}

void Weapon::updateTimers(float dt) {
    if (fireTimer > 0) fireTimer -= dt;
    if (fireTimer < 0) fireTimer = 0;
    if (reloading) {
        reloadTimer -= dt;
        if (reloadTimer <= 0) {
            reloading = false;
            int maxAmmo = gWeaponTable[type].maxAmmo;
            if (maxAmmo > 0) ammo = maxAmmo;
        }
    }
}

Point3F computeProjectileSpawn(const Point3F& cameraPos, const Point3F& targetDir, float spread) {
    Point3F spawn = cameraPos;
    spawn.x += targetDir.x * 0.5f;
    spawn.y += targetDir.y * 0.5f;
    spawn.z += targetDir.z * 0.5f;

    if (spread > 0) {
        float r1 = ((float)rand() / RAND_MAX - 0.5f) * spread;
        float r2 = ((float)rand() / RAND_MAX - 0.5f) * spread;
        Point3F right = {targetDir.z, 0, -targetDir.x};
        float rlen = sqrtf(right.x * right.x + right.z * right.z);
        if (rlen > 1e-6f) { right.x /= rlen; right.z /= rlen; }
        Point3F up = {0, 1, 0};
        Point3F spreadOffset = {right.x * r1 + up.x * r2, right.y * r1 + up.y * r2, 0};
        spawn.x += spreadOffset.x;
        spawn.y += spreadOffset.y;
    }

    return spawn;
}

void updateProjectile(Projectile& p, float dt) {
    p.lifetime -= dt;
    if (p.lifetime <= 0 || !p.active) {
        p.active = false;
        return;
    }

    if (p.type == ProjectileType::Disc || p.type == ProjectileType::Grenade ||
        p.type == ProjectileType::Mortar) {
        p.vel.y += -20.0f * dt;
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

bool checkProjectileCollision(Projectile& p, float& groundHeight) {
    auto& world = Engine::instance().game().world();

    // Check terrain height
    float th = world.getHeight(p.pos.x, p.pos.z);
    if (p.pos.y <= th && p.vel.y < 0) {
        p.pos.y = th;
        p.hasImpacted = true;

        if (p.type == ProjectileType::Grenade && p.bounceCount < 3) {
            p.vel.y = -p.vel.y * 0.5f;
            p.vel.x *= 0.7f;
            p.vel.z *= 0.7f;
            p.bounceCount++;
            if (p.vel.y < 2.0f) p.hasImpacted = true;
            return false;
        }
        return true;
    }

    // Check interior collision mesh (raycast)
    const auto& collision = world.collision();
    if (collision.loaded) {
        float t;
        Point3F hitPos, hitNorm;
        Point3F dir = p.vel;
        float speed = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (speed > 0.1f) {
            dir.x /= speed; dir.y /= speed; dir.z /= speed;
            float maxDist = speed * 0.1f; // Check next frame's travel
            Point3F origin = {
                p.pos.x - dir.x * 0.05f,
                p.pos.y - dir.y * 0.05f,
                p.pos.z - dir.z * 0.05f
            };
            if (collision.raycast(origin, dir, maxDist + 0.2f, t, hitPos, hitNorm)) {
                p.pos = hitPos;
                p.hasImpacted = true;

                if (p.type == ProjectileType::Grenade && p.bounceCount < 3) {
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
