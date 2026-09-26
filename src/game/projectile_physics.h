#pragma once
// Client flight of ballistic projectiles between server updates
// (GrenadeProjectile::processTick): gravity x gravityMod, swept against the
// static world, bouncing while unarmed and stopping at the first armed
// contact (the server sends the explosion). Y-up space.
#include "core/math.h"
#include <functional>

namespace ProjectilePhysics {

inline constexpr float TickSeconds = 0.032f;

struct RayHit {
    float t = 0.0f;       // fraction of the segment
    Point3F point{};
    Point3F normal{0, 1, 0};
};
// Cast a -> b; true on a hit.
using CastRay = std::function<bool(const Point3F& a, const Point3F& b, RayHit& hit)>;

// projGrenade.cc bounce response: mirror reflection, minus friction times
// the tangential part, all scaled by elasticity.
inline Point3F bounceVelocity(const Point3F& v, const Point3F& n, float friction, float elasticity) {
    const float dot = v.x * n.x + v.y * n.y + v.z * n.z;
    const Point3F b{v.x - n.x * dot * 2.0f, v.y - n.y * dot * 2.0f, v.z - n.z * dot * 2.0f};
    const float bDot = b.x * n.x + b.y * n.y + b.z * n.z;
    const Point3F tangent{b.x - n.x * bDot, b.y - n.y * bDot, b.z - n.z * bDot};
    return {(b.x - tangent.x * friction) * elasticity, (b.y - tangent.y * friction) * elasticity,
            (b.z - tangent.z * friction) * elasticity};
}

// One tick. Returns false once the projectile stops at an armed contact.
inline bool stepBallistic(Point3F& pos, Point3F& vel, float gravity, float elasticity,
                          float friction, bool armed, const CastRay& cast) {
    constexpr int MaxBounces = 5;      // sMaxBounceCount
    constexpr float Backoff = 0.05f;   // off the surface after a bounce
    vel.y += gravity * TickSeconds;
    Point3F start = pos;
    float timeLeft = 1.0f;
    Point3F end{start.x + vel.x * TickSeconds, start.y + vel.y * TickSeconds, start.z + vel.z * TickSeconds};
    for (int i = 0; i < MaxBounces; ++i) {
        RayHit hit;
        if (!cast(start, end, hit)) { pos = end; return true; }
        if (armed) {
            pos = hit.point;
            vel = {0, 0, 0};
            return false;
        }
        vel = bounceVelocity(vel, hit.normal, friction, elasticity);
        timeLeft *= 1.0f - hit.t;
        start = {hit.point.x + hit.normal.x * Backoff, hit.point.y + hit.normal.y * Backoff,
                 hit.point.z + hit.normal.z * Backoff};
        const float dt = timeLeft * TickSeconds;
        end = {start.x + vel.x * dt, start.y + vel.y * dt, start.z + vel.z * dt};
    }
    pos = start;
    return true;
}

} // namespace ProjectilePhysics
