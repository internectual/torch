#pragma once
// Item::processTick on the client (Item::updateVelocity and updatePos), in
// Torch's Y-up space. The engine casts a ray from the top centre of the
// object box at the tick's start to its bottom centre at the end for contact,
// then sweeps the box (up to three times) against the world. Here the sweep
// is a ray from the box's bottom centre; the collision response, rest test
// and constants are the engine's.
#include "game/projectile_physics.h"
#include <cmath>

namespace ItemPhysics {

constexpr float TickSeconds = 0.032f;
constexpr float Gravity = -20.0f;          // Item::mGravity
constexpr float AtRestVelocity = 0.15f;    // sAtRestVelocity
constexpr float Backoff = 0.01f;           // off the contact surface

struct Params {
    float gravityMod = 1.0f;
    float maxVelocity = -1.0f;  // <= 0: unlimited
    float friction = 0.0f;
    float elasticity = 0.0f;
    bool sticky = false;
    // The object box's top and bottom centres relative to the item origin.
    Point3F topCentre{}, bottomCentre{};
};

inline float length(const Point3F& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Subtract the velocity into the surface and friction; bounce by elasticity.
inline void respond(Point3F& vel, const Point3F& normal, float bd, const Params& params) {
    Point3F fv{vel.x + normal.x * bd, vel.y + normal.y * bd, vel.z + normal.z * bd};
    const float fvl = length(fv);
    if (fvl > 0.0f) {
        const float ff = bd * params.friction;
        if (ff < fvl) {
            const float scale = ff / fvl;
            fv = {fv.x * scale, fv.y * scale, fv.z * scale};
        }
    }
    const float push = bd * (1.0f + params.elasticity) + 0.002f;
    vel = {vel.x + normal.x * push - fv.x, vel.y + normal.y * push - fv.y,
           vel.z + normal.z * push - fv.z};
}

// One tick. Returns true when the item comes to rest.
inline bool step(Point3F& pos, Point3F& vel, const Params& params,
                 const ProjectilePhysics::CastRay& cast) {
    // updateVelocity: gravity, then pull back 10% of any excess over
    // 1.05 x maxVelocity.
    vel.y += Gravity * params.gravityMod * TickSeconds;
    if (params.maxVelocity > 0.0f) {
        const float len = length(vel);
        if (len > params.maxVelocity * 1.05f) {
            const float k = (1.0f - params.maxVelocity / len) * 0.1f;
            vel = {vel.x - vel.x * k, vel.y - vel.y * k, vel.z - vel.z * k};
        }
    }

    bool contact = false;
    float time = TickSeconds;
    {
        const Point3F end{pos.x + vel.x * time, pos.y + vel.y * time, pos.z + vel.z * time};
        const Point3F a{pos.x + params.topCentre.x, pos.y + params.topCentre.y, pos.z + params.topCentre.z};
        const Point3F b{end.x + params.bottomCentre.x, end.y + params.bottomCentre.y,
                        end.z + params.bottomCentre.z};
        ProjectilePhysics::RayHit hit;
        if (cast(a, b, hit)) {
            const float bd = -(vel.x * hit.normal.x + vel.y * hit.normal.y + vel.z * hit.normal.z);
            if (bd >= 0.0f) {
                if (params.sticky) {
                    vel = {0, 0, 0};
                    return true;
                }
                respond(vel, hit.normal, bd, params);
                contact = true;
            }
        }
    }

    int count = 0;
    for (; count < 3; ++count) {
        const Point3F a{pos.x + params.bottomCentre.x, pos.y + params.bottomCentre.y,
                        pos.z + params.bottomCentre.z};
        const Point3F move{vel.x * time, vel.y * time, vel.z * time};
        const Point3F b{a.x + move.x, a.y + move.y, a.z + move.z};
        ProjectilePhysics::RayHit hit;
        if (!cast(a, b, hit)) {
            pos = {pos.x + move.x, pos.y + move.y, pos.z + move.z};
            break;
        }
        // To the contact point, off the surface.
        pos = {hit.point.x - params.bottomCentre.x + hit.normal.x * Backoff,
               hit.point.y - params.bottomCentre.y + hit.normal.y * Backoff,
               hit.point.z - params.bottomCentre.z + hit.normal.z * Backoff};
        time -= time * hit.t;
        const float bd = -(vel.x * hit.normal.x + vel.y * hit.normal.y + vel.z * hit.normal.z);
        if (bd > 0.0f) {
            if (params.sticky) {
                vel = {0, 0, 0};
                return true;
            }
            respond(vel, hit.normal, bd, params);
            contact = true;
        }
    }
    if (count == 3) vel = {0, 0, 0}; // couldn't move

    if (contact && length(vel) < AtRestVelocity) {
        vel = {0, 0, 0};
        return true;
    }
    return false;
}

} // namespace ItemPhysics
