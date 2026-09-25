#pragma once
#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

enum class ProjectileType : uint8_t {
    Hitscan,
    Disc,
    Bolt,
    Grenade,
    Mortar
};

// Hitscan traces keep their muzzle position as the segment start while their
// current position is set directly to the end of the trace.
inline bool projectileAdvancesPosition(ProjectileType type) {
    return type != ProjectileType::Hitscan;
}

// Hitscan traces are resolved during their firing tick. Keeping the trace
// alive would render a stationary trail for subsequent frames in open space.
inline bool projectileResolvesThisTick(ProjectileType type) {
    return type == ProjectileType::Hitscan;
}

inline float projectileGravityAcceleration(float worldGravity, float gravityMod) {
    // PhysicalZone fields come from mission data.  Ignore malformed values
    // rather than turning the projectile position into NaN.
    if (!std::isfinite(worldGravity)) worldGravity = 0.0f;
    if (!std::isfinite(gravityMod)) gravityMod = 1.0f;
    return worldGravity * gravityMod;
}

// PhysicalZone force accelerates projectiles just as it does players. Keep
// the force in the same Y-up frame as the projectile simulation.
inline Point3F projectileZoneAcceleration(float worldGravity, float gravityMod,
                                          const Point3F& appliedForce) {
    return {std::isfinite(appliedForce.x) ? appliedForce.x : 0.0f,
            (std::isfinite(appliedForce.y) ? appliedForce.y : 0.0f) +
                projectileGravityAcceleration(worldGravity, gravityMod),
            std::isfinite(appliedForce.z) ? appliedForce.z : 0.0f};
}

// Reflect grenade velocity from the contact plane, including sloped terrain.
inline void reflectProjectileVelocity(Point3F& velocity, const Point3F& normal,
                                      float restitution = 0.5f) {
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                   normal.z * normal.z);
    if (length <= 1.0e-6f) return;
    const Point3F n{normal.x / length, normal.y / length, normal.z / length};
    const float normalSpeed = velocity.x * n.x + velocity.y * n.y + velocity.z * n.z;
    if (normalSpeed >= 0.0f) return;
    const float impulse = (1.0f + std::max(0.0f, restitution)) * normalSpeed;
    velocity.x -= impulse * n.x;
    velocity.y -= impulse * n.y;
    velocity.z -= impulse * n.z;
}

// A grenade that is already resting on a surface can still have tangential
// velocity.  It is sliding, not making another bounce; counting that contact
// consumes the bounce budget and makes grenades detonate early on flat floors.
inline bool projectileVelocityIntoSurface(const Point3F& velocity,
                                           const Point3F& normal) {
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                   normal.z * normal.z);
    if (length <= 1.0e-6f) return false;
    return velocity.x * normal.x + velocity.y * normal.y + velocity.z * normal.z <
        -1.0e-6f * length;
}

// Explosion damage uses the same linear distance falloff for players and
// bots. Keeping this calculation shared avoids different damage for identical
// targets.
inline float projectileSplashDamage(float baseDamage, float distance,
                                     float radius) {
    if (!std::isfinite(baseDamage) || !std::isfinite(distance) ||
        !std::isfinite(radius) || distance < 0.0f || radius <= 0.0f ||
        distance >= radius)
        return 0.0f;
    // Malformed negative damage must not turn an explosion into a healing
    // pulse.  Tribes 2 clamps splash damage at zero before applying falloff.
    return std::max(0.0f, baseDamage) * std::max(0.0f, 1.0f - distance / radius);
}

// RepairTool uses negative projectile damage to heal. Keep the signed effect
// separate from projectileSplashDamage, whose non-negative result is used for
// ordinary damage calculations.
inline float projectileSplashEffect(float baseDamage, float distance,
                                     float radius) {
    if (!std::isfinite(baseDamage) || !std::isfinite(distance) ||
        !std::isfinite(radius) || distance < 0.0f || radius <= 0.0f ||
        distance >= radius)
        return 0.0f;
    return baseDamage * std::max(0.0f, 1.0f - distance / radius);
}

// Interior geometry blocks explosive splash, while mapper/terrain-only scenes
// without a collision mesh retain the open-world distance behavior.
inline bool projectileSplashCanReach(bool collisionLoaded, bool lineOfSight) {
    return !collisionLoaded || lineOfSight;
}

// Explosive projectiles push ShapeBases away from the impact point as well as
// damaging them. Keep the client-side impulse consistent with server splash
// handling, including the same linear falloff.
inline Point3F projectileSplashImpulse(const Point3F& target,
                                       const Point3F& impact, float distance,
                                       float radius) {
    if (!std::isfinite(distance) || !std::isfinite(radius) || radius <= 0.0f ||
        distance < 0.0f || distance >= radius)
        return {};
    const float dx = target.x - impact.x;
    const float dy = target.y - impact.y;
    const float dz = target.z - impact.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    // A ShapeBase at the blast center still receives an upward impulse.  The
    // native solver uses this stable fallback when no radial direction exists.
    if (!std::isfinite(length)) return {};
    const float strength = std::max(0.0f, 1.0f - distance / radius) * 10.0f;
    if (length <= 1.0e-4f) return {0.0f, strength, 0.0f};
    return {dx / length * strength, dy / length * strength,
            dz / length * strength};
}

// Hitscan weapons resolve their whole trace in one simulation step rather
// than advancing a projectile by the frame's movement distance.
inline Point3F hitscanEndpoint(const Point3F& origin, const Point3F& direction,
                               float range = 20000.0f) {
    const float length = std::sqrt(direction.x * direction.x +
                                   direction.y * direction.y +
                                   direction.z * direction.z);
    if (length <= 1.0e-6f || !std::isfinite(length) || !std::isfinite(range))
        return origin;
    const float scale = range / length;
    return {origin.x + direction.x * scale,
            origin.y + direction.y * scale,
            origin.z + direction.z * scale};
}

// Torque launches physical projectiles from the muzzle in the shooter's frame:
// a moving player imparts their current velocity to the disc, grenade, or
// mortar round.  Hitscan traces do not use this helper because they are
// resolved against the world immediately.
inline Point3F projectileLaunchVelocity(const Point3F& direction, float speed,
                                        const Point3F& shooterVelocity) {
    // Launch data can arrive from a partially initialized ghost or script
    // datablock. Do not let one malformed shot poison the projectile list.
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(speed) ||
        !std::isfinite(shooterVelocity.x) || !std::isfinite(shooterVelocity.y) ||
        !std::isfinite(shooterVelocity.z))
        return {};
    return {direction.x * speed + shooterVelocity.x,
            direction.y * speed + shooterVelocity.y,
            direction.z * speed + shooterVelocity.z};
}

// A view rebuild can briefly provide an incomplete camera ray.  Treat that as
// a failed trigger rather than consuming ammo/energy for a zero-direction shot.
inline bool weaponAimUsable(const Point3F& direction) {
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z))
        return false;
    const float length = std::sqrt(direction.x * direction.x +
                                   direction.y * direction.y +
                                   direction.z * direction.z);
    return std::isfinite(length) && length >= 0.001f;
}

// Interior collision must still be tested after a projectile has nearly come
// to rest. The old 0.1-unit cutoff let slow replicated rounds cross walls.
inline bool projectileHasSweepLength(float length) {
    return std::isfinite(length) && length > 1.0e-6f;
}

struct Projectile {
    Point3F pos;
    Point3F previousPos;
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

// Direct target hits consume a projectile just like world-surface impacts.
inline void markProjectileImpact(Projectile& projectile) {
    projectile.active = false;
    projectile.hasImpacted = true;
}

// A bounce is a contact, not an explosion. Keep the projectile alive and defer
// splash/explosion effects until a later terminal impact.
inline void markProjectileBounce(Projectile& projectile) {
    projectile.hasImpacted = false;
}

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

// Non-automatic Tribes 2 weapons fire once per trigger press, even when the
// input remains held across multiple simulation ticks.
inline bool weaponTriggerCanFire(const WeaponData& data, bool triggerDown,
                                  bool triggerWasDown) {
    return triggerDown && (data.autoFire || !triggerWasDown);
}

// A weapon simulation tick can resolve only one trigger mode.  Primary wins
// when both inputs arrive together, matching the native image trigger path.
inline int weaponTriggerMode(const WeaponData& data, bool primaryDown,
                             bool alternateDown, bool primaryWasDown,
                             bool alternateWasDown) {
    if (weaponTriggerCanFire(data, primaryDown, primaryWasDown)) return 1;
    if (data.altFire && weaponTriggerCanFire(data, alternateDown, alternateWasDown))
        return 2;
    return 0;
}

// altFire describes whether the datablock has an alternate mode; it does not
// make the weapon's primary trigger unusable.
inline bool weaponFireModeAllowed(const WeaponData& data, bool alt) {
    return !alt || data.altFire;
}

// A zero timestamp is the native "has not fired" sentinel.  Treat it as
// ready instead of making a newly spawned player wait through the fire rate.
inline bool weaponFireCooldownReady(double now, double lastFireTime,
                                    float fireRate) {
    if (!std::isfinite(now) || !std::isfinite(lastFireTime) ||
        !std::isfinite(fireRate) || fireRate < 0.0f)
        return false;
    return lastFireTime <= 0.0 || now - lastFireTime >= fireRate;
}

// Tribes 2 represents weapons without an ammo reserve with a negative count.
// Keep that sentinel in the player state rather than manufacturing a finite
// reserve that can be picked up and displayed.
inline int32_t weaponInitialAmmo(const WeaponData& data) {
    return data.maxAmmo < 0 ? -1 : data.maxAmmo;
}

// Ammo pickups only apply to weapons with a finite authored reserve.  Zero is
// not a usable reserve size; treating it as one consumes pickups for invalid
// or incomplete loadout entries without changing any gameplay state.
inline bool weaponAcceptsAmmoPickup(const WeaponData& data) {
    return data.maxAmmo > 0;
}

inline bool weaponNeedsReload(const WeaponData& data, int32_t ammo) {
    return data.maxAmmo > 0 && ammo == 0 && data.reloadTime > 0.0f;
}

inline bool weaponCanStartReload(const WeaponData& data, int32_t ammo) {
    return data.maxAmmo > 0 && data.reloadTime > 0.0f &&
           ammo >= 0 && ammo < data.maxAmmo;
}

// Reloading interrupts the firing presentation immediately.  Keeping the
// transition in one place prevents manual reloads from leaving the weapon in
// its fire animation until the old cooldown expires.
inline void beginWeaponReload(bool& reloading, float& reloadTimer, bool& firing,
                               float& fireTimer, float reloadTime) {
    if (!std::isfinite(reloadTime) || reloadTime <= 0.0f) return;
    reloading = true;
    reloadTimer = reloadTime;
    firing = false;
    fireTimer = 0.0f;
}

// Changing weapons interrupts the active reload.  The native weapon image is
// transaction-local; completing it after switching away would refill a weapon
// that was never kept in hand.
inline void cancelWeaponReload(bool& reloading, float& reloadTimer) {
    reloading = false;
    reloadTimer = 0.0f;
}

inline void cancelWeaponFirePresentation(bool& firing) {
    firing = false;
}

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

extern const WeaponData gWeaponTable[];
extern const int gWeaponCount;

// Effects are keyed by the datablock type, not the slot containing it.  A
// loadout may reorder weapons without changing their authored effects.
inline int weaponEffectType(const Weapon& weapon) {
    return weapon.type;
}

// Ammo is the supported loadout state: -1 is infinite, zero is unavailable.
inline bool weaponIsSelectable(const Weapon& weapon) {
    // Do not activate a stale loadout entry that would be used as a table
    // index immediately after selection.
    if (weapon.type < 0 || weapon.type >= gWeaponCount) return false;
    // Empty finite-reserve weapons remain valid loadout slots.  Switching away
    // can interrupt their image reload, but it must not make the slot vanish
    // from the native weapon cycle permanently.
    return gWeaponTable[weapon.type].maxAmmo > 0 || weapon.ammo != 0 ||
           weapon.reloading;
}

inline bool weaponNeedsReloadOnSelect(const Weapon& weapon) {
    if (weapon.type < 0 || weapon.type >= gWeaponCount) return false;
    const WeaponData& data = gWeaponTable[weapon.type];
    return !weapon.reloading && weapon.ammo == 0 && data.maxAmmo > 0 &&
           data.reloadTime > 0.0f;
}

// A non-finite projectile lifetime cannot expire through an ordinary
// comparison. Treat it as invalid state and remove the projectile rather than
// leaving a visible trail alive forever.
inline bool projectileLifetimeStep(float& lifetime, float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return true;
    if (!std::isfinite(lifetime)) return false;
    lifetime -= dt;
    return lifetime > 0.0f;
}

inline int32_t nextSelectableWeapon(const std::vector<Weapon>& weapons,
                                     int32_t current, int32_t direction) {
    if (weapons.empty() || direction == 0) return current;
    const int32_t count = (int32_t)weapons.size();
    // A loadout can briefly have no selected slot (-1). Normalize that state
    // to the edge from which the requested direction should start; otherwise
    // reverse cycling can leave a negative modulo result and index out of
    // bounds (or skip the last weapon).
    int32_t candidate = current >= 0 && current < count
        ? current : (direction > 0 ? count - 1 : 0);
    for (int32_t checked = 0; checked < count; ++checked) {
        candidate = (candidate + (direction > 0 ? 1 : -1) + count) % count;
        if (weaponIsSelectable(weapons[candidate])) return candidate;
    }
    return current;
}

inline bool segmentSphereHit(const Point3F& start, const Point3F& end,
                             const Point3F& center, float radius, float& hitT) {
    // ShapeBase collision radii are authored as non-negative extents. A
    // malformed negative radius must not become a valid sphere when squared.
    if (!std::isfinite(radius) || radius < 0.0f) return false;
    const Point3F delta{end.x - start.x, end.y - start.y, end.z - start.z};
    const Point3F offset{start.x - center.x, start.y - center.y, start.z - center.z};
    const float a = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    const float radiusSquared = radius * radius;
    if (a <= 1.0e-8f) {
        // A stationary projectile can still overlap a target.  Report the
        // contact at the start of the segment so callers do not interpolate
        // with an indeterminate hit fraction.
        hitT = 0.0f;
        return offset.x * offset.x + offset.y * offset.y + offset.z * offset.z <= radiusSquared;
    }
    const float b = 2.0f * (offset.x * delta.x + offset.y * delta.y + offset.z * delta.z);
    const float c = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - radiusSquared;
    // A swept projectile that begins inside the target has already impacted;
    // returning the exit root would delay damage until after it crossed out.
    if (c <= 0.0f) {
        hitT = 0.0f;
        return true;
    }
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return false;
    const float root = std::sqrt(discriminant);
    const float first = (-b - root) / (2.0f * a);
    const float second = (-b + root) / (2.0f * a);
    if (first >= 0.0f && first <= 1.0f) { hitT = first; return true; }
    if (second >= 0.0f && second <= 1.0f) { hitT = second; return true; }
    return false;
}

// Player collision uses a horizontal radius and a separate vertical extent.
// Sweep the projectile segment so a fast projectile cannot skip over a player
// between simulation ticks.
inline bool segmentPlayerHit(const Point3F& start, const Point3F& end,
                             const Point3F& center, float& hitT) {
    constexpr float radiusSquared = 2.0f;
    constexpr float halfHeight = 2.0f;
    const float dx = end.x - start.x;
    const float dz = end.z - start.z;
    const float ox = start.x - center.x;
    const float oz = start.z - center.z;
    const float a = dx * dx + dz * dz;
    const auto verticallyInside = [&](float t) {
        const float y = start.y + (end.y - start.y) * t;
        // ShapeBase collision bounds include their caps.  A trace that lands
        // exactly on the top or bottom plane must still register a hit.
        return std::fabs(y - center.y) <= halfHeight;
    };
    if (ox * ox + oz * oz <= radiusSquared && verticallyInside(0.0f)) {
        hitT = 0.0f;
        return true;
    }
    if (a <= 1.0e-8f) {
        // A vertical trace has no horizontal roots to test. If it stays
        // inside the capsule radius, solve the Y slab directly so shots that
        // pass through a player from above or below still register.
        if (ox * ox + oz * oz > radiusSquared) return false;
        const float dy = end.y - start.y;
        if (std::fabs(dy) <= 1.0e-8f) {
            // Bounds are inclusive even for a stationary trace. This is the
            // edge-contact case missed by the quadratic sweep below.
            if (verticallyInside(0.0f)) {
                hitT = 0.0f;
                return true;
            }
            return false;
        }
        const float lower = center.y - halfHeight;
        const float upper = center.y + halfHeight;
        const float candidate = dy > 0.0f
            ? (lower - start.y) / dy : (upper - start.y) / dy;
        if (candidate >= 0.0f && candidate <= 1.0f && verticallyInside(candidate)) {
            hitT = candidate;
            return true;
        }
        return false;
    }
    const float b = 2.0f * (ox * dx + oz * dz);
    const float c = ox * ox + oz * oz - radiusSquared;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return false;
    const float root = std::sqrt(discriminant);
    const float denominator = 2.0f * a;
    const float candidates[] = {(-b - root) / denominator, (-b + root) / denominator};
    for (float candidate : candidates) {
        if (candidate >= 0.0f && candidate <= 1.0f && verticallyInside(candidate)) {
            hitT = candidate;
            return true;
        }
    }
    return false;
}

inline bool segmentSurfaceCrossing(float previousY, float previousSurface,
                                   float currentY, float currentSurface,
                                   float& hitT) {
    const float before = previousY - previousSurface;
    const float after = currentY - currentSurface;
    // A projectile that starts exactly on a surface and moves into it has
    // already contacted the surface at the beginning of this tick.  Waiting
    // for the final-point fallback moves the impact one frame late and makes
    // grenade bounces visibly sink into floors.
    if (std::fabs(before) <= 1.0e-6f && after < -1.0e-6f) {
        hitT = 0.0f;
        return true;
    }
    if (!(before > 0.0f && after <= 0.0f)) return false;
    const float denominator = before - after;
    hitT = denominator > 1.0e-6f ? std::clamp(before / denominator, 0.0f, 1.0f) : 1.0f;
    return true;
}

inline bool projectileTouchesSurface(float projectileY, float surfaceY) {
    return projectileY <= surfaceY;
}

inline bool projectileExitsSurface(float previousY, float previousSurface,
                                   float currentY, float currentSurface) {
    // A bounced projectile commonly starts its next tick exactly on the
    // surface. Treat that contact as an exit too, otherwise the final-point
    // fallback reverses its newly applied upward velocity a second time.
    return previousY <= previousSurface && currentY > currentSurface;
}

void loadWeaponSounds(Weapon& w);

Point3F computeProjectileSpawn(const Point3F& cameraPos, const Point3F& targetDir, float spread = 0);
void updateProjectile(Projectile& p, float dt);
bool checkProjectileCollision(Projectile& p, float& groundHeight, Point3F& impactNormal);
