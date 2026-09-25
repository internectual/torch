#include "game/weapon.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>

int main() {
    const Point3F camera{10.0f, 20.0f, 30.0f};
    const Point3F direction{1.0f, 0.0f, 0.0f};

    std::srand(1);
    const float r1 = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f);
    const float r2 = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f);
    std::srand(1);
    const Point3F spawn = computeProjectileSpawn(camera, direction, 1.0f);

    // For an X-facing shot, the horizontal spread basis points along Z.  The
    // muzzle offset must preserve that component instead of dropping it.
    assert(std::fabs(spawn.x - 10.5f) < 1.0e-6f);
    assert(std::fabs(spawn.y - (20.0f + r2)) < 1.0e-6f);
    assert(std::fabs(spawn.z - (30.0f - r1)) < 1.0e-6f);

    // Vertical aim must still apply both spread dimensions.  The old
    // horizontal-only basis discarded r1 for straight-up shots.
    std::srand(1);
    const Point3F vertical = computeProjectileSpawn(camera, {0.0f, 1.0f, 0.0f}, 1.0f);
    assert(std::fabs(vertical.x - (10.0f + r1)) < 1.0e-6f);
    assert(std::fabs(vertical.y - 20.5f) < 1.0e-6f);
    assert(std::fabs(vertical.z - (30.0f + r2)) < 1.0e-6f);

    // Aim vectors from script/GUI paths are not guaranteed to be normalized;
    // the native muzzle offset remains a fixed half-unit in that case.
    const Point3F unnormalized = computeProjectileSpawn(camera, {0.0f, 0.0f, 4.0f});
    assert(std::fabs(unnormalized.x - 10.0f) < 1.0e-6f);
    assert(std::fabs(unnormalized.y - 20.0f) < 1.0e-6f);
    assert(std::fabs(unnormalized.z - 30.5f) < 1.0e-6f);

    // Moving players impart their momentum to physical projectiles.  This is
    // what lets a skiing/jetting player preserve the native projectile range.
    const Point3F launch = projectileLaunchVelocity(
        {0.0f, 0.0f, 1.0f}, 50.0f, {3.0f, -2.0f, 4.0f});
    assert(launch.x == 3.0f && launch.y == -2.0f && launch.z == 54.0f);
    const Point3F invalidLaunch = projectileLaunchVelocity(
        {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f}, 50.0f,
        {3.0f, -2.0f, 4.0f});
    assert(invalidLaunch.x == 0.0f && invalidLaunch.y == 0.0f &&
           invalidLaunch.z == 0.0f);

    // A malformed camera target must not turn the muzzle transform into NaNs.
    const Point3F invalidAim = computeProjectileSpawn(
        camera, {std::numeric_limits<float>::infinity(), 0.0f, 1.0f});
    assert(invalidAim.x == camera.x && invalidAim.y == camera.y &&
           invalidAim.z == camera.z);
    assert(!weaponAimUsable({std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f}));
    assert(!weaponAimUsable({0.0f, 0.0f, 0.0f}));
    assert(weaponAimUsable({0.0f, 0.0f, 1.0f}));

    Projectile directHit;
    markProjectileImpact(directHit);
    assert(!directHit.active && directHit.hasImpacted);
    Projectile bounce;
    bounce.hasImpacted = true;
    markProjectileBounce(bounce);
    assert(bounce.active && !bounce.hasImpacted);

    std::vector<Weapon> loadout(3);
    for (int i = 0; i < 3; ++i) {
        loadout[i].type = i;
        loadout[i].ammo = 1;
    }
    loadout[2].type = gWeaponCount;
    assert(!weaponIsSelectable(loadout[2]));
    loadout[2].type = 2;
    loadout[2].reloading = true;
    loadout[2].ammo = 0;
    assert(weaponIsSelectable(loadout[2]));
    // Interrupting an image reload while switching weapons must not make an
    // otherwise valid finite-reserve slot disappear from the loadout.
    loadout[2].reloading = false;
    assert(weaponIsSelectable(loadout[2]));
    assert(weaponNeedsReloadOnSelect(loadout[2]));
    beginWeaponReload(loadout[2].reloading, loadout[2].reloadTimer,
                      loadout[2].firing, loadout[2].fireTimer,
                      gWeaponTable[loadout[2].type].reloadTime);
    assert(loadout[2].reloading && loadout[2].reloadTimer ==
           gWeaponTable[loadout[2].type].reloadTime);
    // No selected slot is a valid transient state while changing loadouts.
    // Cycling backward must begin at the last slot, not produce a negative
    // modulo index or skip slot two.
    assert(nextSelectableWeapon(loadout, -1, -1) == 2);
    assert(nextSelectableWeapon(loadout, 3, 1) == 0);

    // A grenade resting on a surface must bounce at the start of the next
    // tick, rather than first penetrating and being corrected at its endpoint.
    float surfaceT = -1.0f;
    assert(segmentSurfaceCrossing(0.0f, 0.0f, -0.1f, 0.0f, surfaceT));
    assert(surfaceT == 0.0f);
    assert(!segmentSurfaceCrossing(-0.1f, 0.0f, 0.1f, 0.0f, surfaceT));
    assert(!projectileVelocityIntoSurface({3.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
    assert(projectileVelocityIntoSurface({0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));

    // Players and bots at the same distance from an explosion receive the
    // same authored splash damage; player damage was previously halved.
    assert(std::fabs(projectileSplashDamage(80.0f, 2.0f, 8.0f) - 60.0f) < 1.0e-6f);
    assert(projectileSplashDamage(80.0f, 8.0f, 8.0f) == 0.0f);
    // Invalid negative distances must not amplify a blast past its authored
    // damage or produce an outward impulse.
    assert(projectileSplashDamage(80.0f, -1.0f, 8.0f) == 0.0f);
    assert(projectileSplashDamage(-80.0f, 2.0f, 8.0f) == 0.0f);
    assert(projectileSplashEffect(-80.0f, -1.0f, 8.0f) == 0.0f);
    assert(std::fabs(projectileSplashEffect(-80.0f, 2.0f, 8.0f) + 60.0f) < 1.0e-6f);
    // Interior walls block explosive splash, but scenes without an interior
    // collision mesh retain the open-world radius behavior.
    assert(projectileSplashCanReach(true, true));
    assert(!projectileSplashCanReach(true, false));
    assert(projectileSplashCanReach(false, false));

    // Local explosive impacts must apply the same outward impulse as the
    // authoritative server, rather than only reducing health.
    const Point3F impulse = projectileSplashImpulse({3.0f, 0.0f, 0.0f},
                                                     {0.0f, 0.0f, 0.0f},
                                                     3.0f, 6.0f);
    assert(std::fabs(impulse.x - 5.0f) < 1.0e-6f);
    assert(impulse.y == 0.0f);
    assert(impulse.z == 0.0f);
    const Point3F centeredImpulse = projectileSplashImpulse(
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 6.0f);
    assert(centeredImpulse.x == 0.0f && centeredImpulse.y == 10.0f &&
           centeredImpulse.z == 0.0f);
    assert(projectileSplashImpulse({3.0f, 0.0f, 0.0f},
                                   {0.0f, 0.0f, 0.0f}, -1.0f, 6.0f).x == 0.0f);

    // Projectile effects use the datablock type, not the reordered loadout
    // slot that selected the weapon.
    loadout[0].type = 6;
    assert(weaponEffectType(loadout[0]) == 6);

    WeaponData finiteAmmo = gWeaponTable[0];
    assert(weaponAcceptsAmmoPickup(finiteAmmo));
    finiteAmmo.maxAmmo = -1;
    assert(!weaponAcceptsAmmoPickup(finiteAmmo));
    finiteAmmo.maxAmmo = 0;
    assert(!weaponAcceptsAmmoPickup(finiteAmmo));

    // Primary and alternate trigger inputs are resolved as one native weapon
    // action, with primary taking precedence when both are pressed.
    const WeaponData& alternateWeapon = gWeaponTable[7];
    assert(weaponTriggerMode(alternateWeapon, true, true, false, false) == 1);
    assert(weaponTriggerMode(alternateWeapon, false, true, false, false) == 2);
    assert(weaponTriggerMode(finiteAmmo, false, true, false, false) == 0);

    // Invalid/paused frame deltas must not poison weapon timers.  NaN used to
    // leave both timers non-comparable forever, making the weapon unusable.
    Weapon timed;
    timed.type = 0;
    timed.fireTimer = 0.25f;
    timed.reloading = true;
    timed.reloadTimer = 1.0f;
    timed.updateTimers(std::numeric_limits<float>::quiet_NaN());
    assert(timed.fireTimer == 0.25f && timed.reloadTimer == 1.0f);
    timed.updateTimers(0.0f);
    assert(timed.fireTimer == 0.25f && timed.reloadTimer == 1.0f);

    // An invalid reload timer must recover to an idle weapon instead of
    // leaving the weapon permanently unable to fire.
    timed.reloadTimer = std::numeric_limits<float>::quiet_NaN();
    timed.updateTimers(1.0f / 60.0f);
    assert(!timed.reloading && timed.reloadTimer == 0.0f);

    // A malformed replicated fire timer must recover to ready instead of
    // leaving the weapon permanently unable to fire.
    timed.fireTimer = std::numeric_limits<float>::quiet_NaN();
    timed.firing = true;
    timed.updateTimers(1.0f / 60.0f);
    assert(timed.fireTimer == 0.0f && !timed.firing);

    // Starting a reload immediately leaves the native weapon in its reload
    // presentation instead of showing a stale fire animation.
    bool reloading = false;
    float reloadTimer = 0.0f;
    bool firing = true;
    float fireTimer = 0.5f;
    beginWeaponReload(reloading, reloadTimer, firing, fireTimer, 1.5f);
    assert(reloading && reloadTimer == 1.5f && !firing && fireTimer == 0.0f);
    cancelWeaponReload(reloading, reloadTimer);
    assert(!reloading && reloadTimer == 0.0f);

    // Switching images clears the old weapon's transient firing presentation;
    // its cooldown may remain, but it must not replay the fire animation when
    // selected again.
    bool oldFiring = true;
    cancelWeaponFirePresentation(oldFiring);
    assert(!oldFiring);

    // Invalid replicated energy/cooldown state must not pass the fire gate.
    Weapon guarded;
    guarded.type = 0;
    guarded.ammo = 1;
    guarded.fireTimer = std::numeric_limits<float>::quiet_NaN();
    assert(!guarded.canFire(100.0f));
    guarded.fireTimer = 0.0f;
    assert(!guarded.canFire(std::numeric_limits<float>::quiet_NaN()));
    assert(!guarded.canFire(std::numeric_limits<float>::infinity()));

    return 0;
}
