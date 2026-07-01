#pragma once
#include "core/math.h"
#include <cstdint>
#include <vector>

enum class ProjectileType : uint8_t {
    Hitscan,
    Disc,
    Bolt,
    Grenade,
    Mortar
};

struct Projectile {
    Point3F pos;
    Point3F vel;
    ProjectileType type = ProjectileType::Disc;
    float lifetime = 0;
    float damage = 0;
    float splashRadius = 0;
    int32_t ownerId = -1;
    int32_t weaponType = -1;
    bool active = true;
    int32_t bounceCount = 0;
    bool hasImpacted = false;
};

struct SoundBuffer;

struct WeaponData {
    const char* name;
    ProjectileType projectileType;
    float fireRate;
    float reloadTime;
    float damage;
    float speed;
    float energyCost;
    int32_t maxAmmo;
    float splashRadius;
    bool autoFire;
    bool altFire;
    const char* fireSoundPath;      // path in VL2, e.g. "audio/fx/weapons/spinfusor/spinfusor_fire"
    const char* explosionSoundPath; // e.g. "audio/fx/weapons/spinfusor/spinfusor_explosion"
};

struct Weapon {
    int32_t type = -1;
    int32_t ammo = 0;
    float fireTimer = 0;
    float reloadTimer = 0;
    bool firing = false;
    bool reloading = false;
    SoundBuffer* fireSound = nullptr;
    SoundBuffer* explosionSound = nullptr;

    bool canFire(float energy) const;
    void updateTimers(float dt);
};

void loadWeaponSounds(Weapon& w);

extern const WeaponData gWeaponTable[];
extern const int gWeaponCount;

Point3F computeProjectileSpawn(const Point3F& cameraPos, const Point3F& targetDir, float spread = 0);
void updateProjectile(Projectile& p, float dt);
bool checkProjectileCollision(Projectile& p, float& groundHeight);
