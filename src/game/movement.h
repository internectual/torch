#pragma once

#include "core/math.h"
#include <algorithm>
#include <cmath>

// The player controller is deliberately free of World/Engine dependencies so
// prediction and authority run the same arithmetic.
struct MovementInput {
    float forward = 0.0f;
    float strafe = 0.0f;
    bool jump = false;
    bool jet = false;
    float yaw = 0.0f;
};

struct MovementState {
    Point3F position{};
    Point3F velocity{};
    float energy = 100.0f;
    bool onGround = false;
    bool jumpWasDown = false;
    float jumpDelay = 0.0f;
    // Downward speed captured before floor contact zeroes vertical velocity.
    float landingSpeed = 0.0f;
};

struct MovementEnvironment {
    float floorY = -1.0e10f;
    Point3F floorNormal{0.0f, 1.0f, 0.0f};
    bool water = false;
    float velocityMod = 1.0f;
    float gravityMod = 1.0f;
    Point3F appliedForce{};
    float gravity = -20.0f;
    // The movement position is the player's center, while floorY is the
    // authored contact surface.
    float collisionRadius = 0.5f;
    float maxGroundSpeed = 15.0f;
    float jumpSpeed = 10.0f;
    float jetAcceleration = 35.0f;
    float maxEnergy = 100.0f;
};

namespace Movement {
constexpr float maxGroundSpeed = 15.0f;
constexpr float groundAcceleration = 60.0f;
constexpr float airAcceleration = 10.0f;
constexpr float groundFriction = 10.0f;
constexpr float airFriction = 0.5f;
constexpr float gravity = -20.0f;
constexpr float jetAcceleration = 35.0f;
constexpr float jumpSpeed = 10.0f;
constexpr float jumpDelaySeconds = 0.96f; // 30 native 32-ms ticks.
constexpr float waterGravityScale = 0.35f;
constexpr float waterDrag = 4.0f;
constexpr float maxEnergy = 100.0f;
constexpr float jetDrain = 20.0f;
constexpr float energyRecharge = 5.0f;

// Tribes 2 applies damage when a player lands after a sufficiently hard
// fall.  Small drops and jump landings are harmless; cap the result so one
// impact cannot produce a value outside the normal damage range.
inline float fallDamage(float downwardSpeed) {
    constexpr float safeSpeed = 12.0f;
    return std::clamp(std::max(0.0f, downwardSpeed - safeSpeed) * 4.0f,
                      0.0f, 100.0f);
}

// Landing damage is authoritative state, so the client prediction and server
// simulation must apply it at the same airborne-to-ground transition.
inline float healthAfterLanding(float health, bool wasOnGround, bool onGround,
                                float downwardSpeed) {
    if (!std::isfinite(health) || !std::isfinite(downwardSpeed) ||
        wasOnGround || !onGround)
        return health;
    return std::max(0.0f, health - fallDamage(downwardSpeed));
}

inline bool isJetting(const MovementInput& input, float energy) {
    return input.jet && energy > 0.0f;
}

inline float approach(float current, float target, float amount) {
    if (current < target) return std::min(current + amount, target);
    return std::max(current - amount, target);
}

inline void projectOnPlane(Point3F& velocity, const Point3F& normal) {
    const float n2 = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
    if (n2 < 1.0e-8f) return;
    const float into = (velocity.x * normal.x + velocity.y * normal.y + velocity.z * normal.z) / n2;
    if (into < 0.0f) {
        velocity.x -= normal.x * into;
        velocity.y -= normal.y * into;
        velocity.z -= normal.z * into;
    }
}

// Interior collision resolves the player's sphere against the floor. On a
// slope, only the vertical component of the normal contributes to the
// center's Y offset; using the full radius makes the player visibly float.
inline bool isGroundedAtFloor(float positionY, float floorY, float radius,
                              const Point3F& floorNormal) {
    return floorNormal.y >= 0.55f &&
        positionY <= floorY + std::max(0.0f, radius * floorNormal.y) + 0.1f;
}

inline float contactHeight(float floorY, float radius, const Point3F& floorNormal) {
    return floorY + std::max(0.0f, radius * floorNormal.y);
}

inline void step(MovementState& state, const MovementInput& input,
                 const MovementEnvironment& environment, float dt) {
    // A paused frame must not apply zone modifiers or otherwise mutate the
    // movement state.  In particular, velocityMod is multiplicative, so
    // processing a zero/negative timestep would still alter velocity.
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    dt = std::clamp(dt, 0.0f, 0.05f);
    // Replicated movement state can be incomplete while a ghost is being
    // created.  Do not let one invalid component poison the player's world
    // position, velocity, or resource HUD for the rest of the match.
    if (!std::isfinite(state.position.x)) state.position.x = 0.0f;
    if (!std::isfinite(state.position.y)) state.position.y = 0.0f;
    if (!std::isfinite(state.position.z)) state.position.z = 0.0f;
    if (!std::isfinite(state.velocity.x)) state.velocity.x = 0.0f;
    if (!std::isfinite(state.velocity.y)) state.velocity.y = 0.0f;
    if (!std::isfinite(state.velocity.z)) state.velocity.z = 0.0f;
    const float configuredMaxEnergy = std::isfinite(environment.maxEnergy) &&
        environment.maxEnergy > 0.0f ? environment.maxEnergy : maxEnergy;
    if (!std::isfinite(state.energy)) state.energy = configuredMaxEnergy;
    state.energy = std::clamp(state.energy, 0.0f, configuredMaxEnergy);
    if (!std::isfinite(state.jumpDelay)) state.jumpDelay = 0.0f;
    if (!std::isfinite(state.landingSpeed)) state.landingSpeed = 0.0f;
    state.landingSpeed = 0.0f;
    // PhysicalZone velocityMod is a signed multiplier.  Negative values are
    // authored for reversing conveyors and should not silently become zero.
    // Invalid authored zone values are ignored by the native datablock path;
    // allowing NaN here would poison the player's position and HUD state.
    const float velocityMod = std::isfinite(environment.velocityMod)
        ? std::clamp(environment.velocityMod, -40.0f, 40.0f) : 1.0f;
    state.velocity.x *= velocityMod;
    state.velocity.y *= velocityMod;
    state.velocity.z *= velocityMod;
    // Input can arrive from a script or a decoded move packet.  Native move
    // processing treats invalid components as zero; allowing NaN here makes
    // every subsequent position and velocity update non-finite.
    const float forward = std::isfinite(input.forward)
        ? std::clamp(input.forward, -1.0f, 1.0f) : 0.0f;
    const float strafe = std::isfinite(input.strafe)
        ? std::clamp(input.strafe, -1.0f, 1.0f) : 0.0f;
    const float yaw = std::isfinite(input.yaw) ? input.yaw : 0.0f;
    const float inputLength = std::sqrt(forward * forward + strafe * strafe);
    const float scale = inputLength > 1.0f ? 1.0f / inputLength : 1.0f;
    const float sinYaw = std::sin(yaw), cosYaw = std::cos(yaw);
    const float configuredGroundSpeed = std::isfinite(environment.maxGroundSpeed) &&
        environment.maxGroundSpeed > 0.0f ? environment.maxGroundSpeed : maxGroundSpeed;
    const float configuredJumpSpeed = std::isfinite(environment.jumpSpeed) &&
        environment.jumpSpeed > 0.0f ? environment.jumpSpeed : jumpSpeed;
    const float configuredJetAcceleration = std::isfinite(environment.jetAcceleration) &&
        environment.jetAcceleration > 0.0f ? environment.jetAcceleration : jetAcceleration;
    const float targetX = (forward * sinYaw - strafe * cosYaw) * scale * configuredGroundSpeed;
    const float targetZ = (forward * cosYaw + strafe * sinYaw) * scale * configuredGroundSpeed;
    const float accel = state.onGround ? groundAcceleration : airAcceleration;
    if (inputLength > 0.001f) {
        state.velocity.x = approach(state.velocity.x, targetX, accel * dt);
        state.velocity.z = approach(state.velocity.z, targetZ, accel * dt);
    } else {
        const float drag = state.onGround ? groundFriction : airFriction;
        state.velocity.x = approach(state.velocity.x, 0.0f, drag * dt);
        state.velocity.z = approach(state.velocity.z, 0.0f, drag * dt);
    }

    // The native jump delay is elapsed time between jump attempts, not time
    // spent standing on the ground.  Advance it in the air so a normal jump
    // does not impose another full delay after landing.
    state.jumpDelay = std::max(0.0f, state.jumpDelay - dt);
    if (input.jump && state.onGround && state.jumpDelay <= 0.0f) {
        state.velocity.y = configuredJumpSpeed;
        state.onGround = false;
        state.jumpDelay = jumpDelaySeconds;
    }
    state.jumpWasDown = input.jump;

    const bool jetting = isJetting(input, state.energy);
    const float forceX = std::isfinite(environment.appliedForce.x) ? environment.appliedForce.x : 0.0f;
    const float forceY = std::isfinite(environment.appliedForce.y) ? environment.appliedForce.y : 0.0f;
    const float forceZ = std::isfinite(environment.appliedForce.z) ? environment.appliedForce.z : 0.0f;
    state.velocity.x += forceX * dt;
    state.velocity.y += forceY * dt;
    state.velocity.z += forceZ * dt;
    // PhysicalZone gravityMod is signed in Tribes 2; negative values reverse
    // gravity instead of disabling the zone's gravity contribution.
    const float gravity = std::isfinite(environment.gravity) ? environment.gravity : Movement::gravity;
    const float gravityMod = std::isfinite(environment.gravityMod)
        ? std::clamp(environment.gravityMod, -40.0f, 40.0f) : 1.0f;
    state.velocity.y += gravity * gravityMod *
        (environment.water ? waterGravityScale : 1.0f) * dt;
    if (jetting) {
        state.velocity.y += configuredJetAcceleration * dt;
        state.energy = std::max(0.0f, state.energy - jetDrain * dt);
    } else {
        state.energy = std::min(configuredMaxEnergy, state.energy + energyRecharge * dt);
    }
    if (environment.water) {
        const float drag = std::max(0.0f, 1.0f - waterDrag * dt);
        state.velocity.x *= drag;
        state.velocity.y *= drag;
        state.velocity.z *= drag;
    }

    state.position.x += state.velocity.x * dt;
    state.position.y += state.velocity.y * dt;
    state.position.z += state.velocity.z * dt;
    const float contactY = contactHeight(environment.floorY,
                                         environment.collisionRadius,
                                         environment.floorNormal);
    if (environment.floorY > -1.0e9f && state.position.y <= contactY) {
        state.position.y = contactY;
        state.landingSpeed = std::max(0.0f, -state.velocity.y);
        if (state.velocity.y < 0.0f) state.velocity.y = 0.0f;
        state.onGround = environment.floorNormal.y >= 0.55f;
        if (state.onGround) projectOnPlane(state.velocity, environment.floorNormal);
    } else {
        state.onGround = false;
    }
}
}
