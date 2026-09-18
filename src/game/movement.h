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
};

struct MovementEnvironment {
    float floorY = -1.0e10f;
    Point3F floorNormal{0.0f, 1.0f, 0.0f};
    bool water = false;
    float velocityMod = 1.0f;
    float gravityMod = 1.0f;
    Point3F appliedForce{};
    float gravity = -25.0f;
};

namespace Movement {
constexpr float maxGroundSpeed = 15.0f;
constexpr float groundAcceleration = 60.0f;
constexpr float airAcceleration = 10.0f;
constexpr float groundFriction = 10.0f;
constexpr float airFriction = 0.5f;
constexpr float gravity = -25.0f;
constexpr float jetAcceleration = 35.0f;
constexpr float jumpSpeed = 10.0f;
constexpr float waterGravityScale = 0.35f;
constexpr float waterDrag = 4.0f;
constexpr float maxEnergy = 100.0f;
constexpr float jetDrain = 20.0f;
constexpr float energyRecharge = 5.0f;

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

inline void step(MovementState& state, const MovementInput& input,
                 const MovementEnvironment& environment, float dt) {
    dt = std::clamp(dt, 0.0f, 0.05f);
    state.velocity.x *= std::max(0.0f, environment.velocityMod);
    state.velocity.y *= std::max(0.0f, environment.velocityMod);
    state.velocity.z *= std::max(0.0f, environment.velocityMod);
    const float inputLength = std::sqrt(input.forward * input.forward + input.strafe * input.strafe);
    const float scale = inputLength > 1.0f ? 1.0f / inputLength : 1.0f;
    const float sinYaw = std::sin(input.yaw), cosYaw = std::cos(input.yaw);
    const float targetX = (input.forward * sinYaw - input.strafe * cosYaw) * scale * maxGroundSpeed;
    const float targetZ = (input.forward * cosYaw + input.strafe * sinYaw) * scale * maxGroundSpeed;
    const float accel = state.onGround ? groundAcceleration : airAcceleration;
    if (inputLength > 0.001f) {
        state.velocity.x = approach(state.velocity.x, targetX, accel * dt);
        state.velocity.z = approach(state.velocity.z, targetZ, accel * dt);
    } else {
        const float drag = state.onGround ? groundFriction : airFriction;
        state.velocity.x = approach(state.velocity.x, 0.0f, drag * dt);
        state.velocity.z = approach(state.velocity.z, 0.0f, drag * dt);
    }

    const bool jumpPressed = input.jump && !state.jumpWasDown;
    if (jumpPressed && state.onGround) {
        state.velocity.y = jumpSpeed;
        state.onGround = false;
    }
    state.jumpWasDown = input.jump;

    const bool jetting = input.jet && state.energy > 0.0f;
    state.velocity.x += environment.appliedForce.x * dt;
    state.velocity.y += environment.appliedForce.y * dt;
    state.velocity.z += environment.appliedForce.z * dt;
    state.velocity.y += environment.gravity * std::max(0.0f, environment.gravityMod) *
        (environment.water ? waterGravityScale : 1.0f) * dt;
    if (jetting) {
        state.velocity.y += jetAcceleration * dt;
        state.energy = std::max(0.0f, state.energy - jetDrain * dt);
    } else {
        state.energy = std::min(maxEnergy, state.energy + energyRecharge * dt);
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
    if (environment.floorY > -1.0e9f && state.position.y <= environment.floorY) {
        state.position.y = environment.floorY;
        if (state.velocity.y < 0.0f) state.velocity.y = 0.0f;
        state.onGround = environment.floorNormal.y >= 0.55f;
        if (state.onGround) projectOnPlane(state.velocity, environment.floorNormal);
    } else {
        state.onGround = false;
    }
}
}
