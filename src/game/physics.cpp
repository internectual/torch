#include "game/physics.h"
#include "game/game.h"
#include "core/engine.h"
#include <cmath>

Physics::Physics() {}
Physics::~Physics() {}

void Physics::update(Player* player, float dt, const Game::InputMove& input) {
    if (!player) return;

    Point3F pos = player->position();
    Point3F rot = player->rotation();
    Point3F vel = player->velocity();

    // Apply input acceleration
    float speed = 10.0f;
    float yaw = rot.z;
    float sinYaw = std::sin(yaw);
    float cosYaw = std::cos(yaw);
    Point3F moveDir{0,0,0};

    if (input.forward)  { moveDir.x += sinYaw * speed; moveDir.z += cosYaw * speed; }
    if (input.backward) { moveDir.x -= sinYaw * speed; moveDir.z -= cosYaw * speed; }
    if (input.left)     { moveDir.x -= cosYaw * speed; moveDir.z += sinYaw * speed; }
    if (input.right)    { moveDir.x += cosYaw * speed; moveDir.z -= sinYaw * speed; }

    // Apply movement
    vel.x += (moveDir.x - vel.x) * dt * 10.0f;
    vel.z += (moveDir.z - vel.z) * dt * 10.0f;

    // Ground detection with interior collision
    float groundHeight = Engine::instance().game().world().getHeight(pos.x, pos.z);
    bool onGround = pos.y <= groundHeight + 0.1f;

    if (input.jump && onGround) {
        vel.y = 8.0f;
        onGround = false;
    }

    // Jet
    float energy = player->energy();
    if (input.jet && energy > 0) {
        vel.y += 12.0f * dt;
        energy -= 25.0f * dt;
        if (energy < 0) energy = 0;
    } else {
        energy += 15.0f * dt;
        if (energy > 100) energy = 100;
    }

    // Gravity
    if (!onGround) {
        vel.y += gravity * dt;
        vel.x *= airFriction;
        vel.z *= airFriction;
    } else {
        vel.x *= friction;
        vel.z *= friction;
    }

    // Update position
    pos.x += vel.x * dt;
    pos.y += vel.y * dt;
    pos.z += vel.z * dt;

    // Resolve interior collision (push player out of walls/floors)
    resolveCollision(player, pos, dt);

    // Re-check ground after collision resolve
    groundHeight = Engine::instance().game().world().getHeight(pos.x, pos.z);

    // Ground clamp
    if (pos.y < groundHeight) {
        pos.y = groundHeight;
        vel.y = 0;
        onGround = true;
    }

    // Update rotation from look input
    rot.x += input.lookDelta.x;
    rot.z += input.lookDelta.y;
    rot.x = Math::clamp(rot.x, -Math::PI * 0.45f, Math::PI * 0.45f);

    player->setPosition(pos);
    player->setRotation(rot);
    player->setVelocity(vel);
    player->setEnergy(energy);
    player->setOnGround(onGround);
}

void Physics::resolveCollision(Player* player, Point3F& pos, float dt) {
    auto& world = Engine::instance().game().world();
    auto& collision = world.collision();
    if (!collision.loaded) return;

    float radius = 0.5f;

    // Sphere collision push-out
    Point3F pushOut{0,0,0};
    collision.sphereCollide(pos, radius, pushOut);

    // If pushed out, add to position and zero velocity in that direction
    if (pushOut.x != 0 || pushOut.y != 0 || pushOut.z != 0) {
        pos.x += pushOut.x;
        pos.y += pushOut.y;
        pos.z += pushOut.z;
    }
}

Physics::RayCastResult Physics::rayCast(const Point3F& origin, const Point3F& dir, float maxDist) {
    RayCastResult result;

    // Simple ground test
    float h = Engine::instance().game().world().getHeight(origin.x, origin.z);
    if (origin.y > h && dir.y < 0) {
        float t = (origin.y - h) / (-dir.y);
        if (t >= 0 && t <= maxDist) {
            result.hit = true;
            result.point = {origin.x + dir.x * t, h, origin.z + dir.z * t};
            result.normal = {0, 1, 0};
            result.distance = t;
        }
    }

    return result;
}
