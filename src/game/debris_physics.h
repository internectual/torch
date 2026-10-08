#pragma once
// Client Debris (DebrisData): launch, onAdd's per-instance variation and the
// 32 ms processTick flight with its bounce rules. Y-up space; rotation angles
// are the Torque object-space X/Z spins in degrees.
#include "core/math.h"
#include "game/projectile_physics.h"
#include "game/timeline_random.h"
#include "net/v12_datablocks.h"
#include <algorithm>
#include <cmath>

namespace DebrisPhysics {

inline constexpr float TickSeconds = 0.032f;

inline Point3F rotateAbout(const Point3F& v, const Point3F& axis, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    const float d = v.x * axis.x + v.y * axis.y + v.z * axis.z;
    const Point3F cross{axis.y * v.z - axis.z * v.y, axis.z * v.x - axis.x * v.z, axis.x * v.y - axis.y * v.x};
    return {v.x * c + cross.x * s + axis.x * d * (1.0f - c),
            v.y * c + cross.y * s + axis.y * d * (1.0f - c),
            v.z * c + cross.z * s + axis.z * d * (1.0f - c)};
}

// MathUtils::randomDir: `axis` tipped by a uniform theta (degrees from the
// axis), then turned by a uniform phi about it.
inline Point3F randomDir(Point3F axis, float thetaMin, float thetaMax, float phiMin, float phiMax,
                         TimelineRandom& random) {
    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = length > 1e-6f ? Point3F{axis.x / length, axis.y / length, axis.z / length} : Point3F{0, 1, 0};
    const Point3F ref = std::fabs(axis.y) < 0.999f ? Point3F{0, 1, 0} : Point3F{0, 0, 1};
    Point3F perpendicular{axis.y * ref.z - axis.z * ref.y, axis.z * ref.x - axis.x * ref.z,
                          axis.x * ref.y - axis.y * ref.x};
    const float pl = std::sqrt(perpendicular.x * perpendicular.x + perpendicular.y * perpendicular.y +
                               perpendicular.z * perpendicular.z);
    perpendicular = {perpendicular.x / pl, perpendicular.y / pl, perpendicular.z / pl};
    const float theta = (thetaMin + random() * (thetaMax - thetaMin)) * Math::PI / 180.0f;
    const float phi = (phiMin + random() * (phiMax - phiMin)) * Math::PI / 180.0f;
    return rotateAbout(rotateAbout(axis, perpendicular, theta), axis, phi);
}

struct Body {
    Point3F pos{}, prevPos{}, vel{};
    float rotX = 0.0f, rotZ = 0.0f, prevRotX = 0.0f, prevRotZ = 0.0f; // degrees
    float spinX = 0.0f, spinZ = 0.0f;                                 // degrees/s
    float radius = 0.2f;
    float elasticity = 0.3f, friction = 0.2f;
    float lifetime = 3.0f, age = 0.0f;
    int bounces = 0;
    bool stationary = false;
};

// Debris::onAdd: lifetime, bounce count, spin, the datablock's own launch
// speed (when non-zero it replaces the launcher's speed) and useRadiusMass.
// `radius` is the part's radius, else the default 0.2.
inline Body init(const V12::DecodedDataBlock::DebrisData& data, const Point3F& pos, Point3F vel,
                 float radius, TimelineRandom& random) {
    Body b;
    b.pos = b.prevPos = pos;
    b.bounces = data.numBounces + random.intInclusive(data.bounceVariance);
    // V12 keeps the asymmetric variance: lifetime + v * (2 * rand(-1, 1)) - v.
    const float variance = data.lifetimeVarianceMS / 1000.0f;
    b.lifetime = std::max(0.0f, data.lifetimeMS / 1000.0f + variance * (4.0f * random() - 3.0f));
    b.spinX = data.minSpin + random() * (data.maxSpin - data.minSpin);
    b.spinZ = (data.minSpin + random() * (data.maxSpin - data.minSpin)) * (0.1f + random() * 0.4f);
    b.radius = radius;
    b.elasticity = data.elasticity;
    b.friction = data.friction;
    if (data.velocity != 0.0f) {
        const float speed = data.velocity + (random() * 2.0f - 1.0f) * data.velocityVariance;
        const float length = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        const float scale = length > 1e-6f ? speed / length : 0.0f;
        vel = {vel.x * scale, vel.y * scale, vel.z * scale};
    }
    b.vel = vel;
    if (data.useRadiusMass) {
        b.radius = std::max(b.radius, data.baseRadius);
        const float factor = b.radius > 0.0f ? data.baseRadius / b.radius : 1.0f;
        b.elasticity *= factor;
        b.friction *= factor;
        b.spinX *= factor;
        b.spinZ *= factor;
    }
    return b;
}

enum class StepResult { None, MaxBounce };

// Debris::processTick: spin, terminal velocity or gravity, then a ray from
// the position to the next position extended by the radius. A hit reflects
// the velocity (minus friction times its tangential part, scaled by
// elasticity) and places the body at the radius-adjusted hit fraction.
inline StepResult step(Body& b, const V12::DecodedDataBlock::DebrisData& data, float gravity,
                       const ProjectilePhysics::CastRay& cast) {
    b.prevPos = b.pos;
    b.prevRotX = b.rotX;
    b.prevRotZ = b.rotZ;
    if (b.stationary) return StepResult::None;
    b.rotX += b.spinX * TickSeconds;
    b.rotZ += b.spinZ * TickSeconds;
    const float speed = std::sqrt(b.vel.x * b.vel.x + b.vel.y * b.vel.y + b.vel.z * b.vel.z);
    if (data.terminalVelocity > 0.0001f && speed > data.terminalVelocity) {
        const float scale = data.terminalVelocity / speed;
        b.vel = {b.vel.x * scale, b.vel.y * scale, b.vel.z * scale};
    } else {
        b.vel.y += gravity * data.gravModifier * TickSeconds;
    }
    const Point3F next{b.pos.x + b.vel.x * TickSeconds, b.pos.y + b.vel.y * TickSeconds,
                       b.pos.z + b.vel.z * TickSeconds};
    const Point3F delta{next.x - b.pos.x, next.y - b.pos.y, next.z - b.pos.z};
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    ProjectilePhysics::RayHit hit;
    if (distance <= 0.0f) return StepResult::None;
    const Point3F dir{delta.x / distance, delta.y / distance, delta.z / distance};
    const Point3F extent{next.x + dir.x * b.radius, next.y + dir.y * b.radius, next.z + dir.z * b.radius};
    if (!cast || !cast(b.pos, extent, hit)) {
        b.pos = next;
        return StepResult::None;
    }
    b.vel = ProjectilePhysics::bounceVelocity(b.vel, hit.normal, b.friction, b.elasticity);
    // Debris::bounce's radius-adjusted fraction (not a swept sphere): the
    // hit fraction of the extended ray times distance / (distance + radius).
    const float along = hit.t * distance / (distance + b.radius);
    b.pos = {b.pos.x + dir.x * along + b.vel.x * TickSeconds, b.pos.y + dir.y * along + b.vel.y * TickSeconds,
             b.pos.z + dir.z * along + b.vel.z * TickSeconds};
    b.spinX *= b.elasticity;
    b.spinZ *= b.elasticity;
    if (--b.bounces > 0) return StepResult::None;
    if (data.staticOnMaxBounce) b.stationary = true;
    if (data.snapOnMaxBounce) {
        // Lie flat: drop the X tumble, keep the heading, lift off the ground.
        b.rotX = b.prevRotX = 0.0f;
        b.pos.y += 0.1f;
    }
    return StepResult::MaxBounce;
}

} // namespace DebrisPhysics
