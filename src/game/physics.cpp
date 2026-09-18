#include "game/physics.h"
#include "game/game.h"
#include "game/movement.h"
#include "core/engine.h"
#include <cmath>

Physics::Physics() {}
Physics::~Physics() {}

void Physics::update(Player* player, float dt, const Game::InputMove& input) {
    if (!player) return;

    Point3F pos = player->position();
    Point3F rot = player->rotation();
    Point3F vel = player->velocity();

    float yaw = rot.z;
    float groundHeight = Engine::instance().game().world().getFloorHeight(pos.x, pos.y, pos.z);
    MovementState state{pos, vel, player->energy(), player->isOnGround(), player->jumpWasDown()};
    MovementInput movement;
    movement.forward = (input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f);
    movement.strafe = (input.right ? 1.0f : 0.0f) - (input.left ? 1.0f : 0.0f);
    movement.jump = input.jump; movement.jet = input.jet; movement.yaw = yaw;
    const auto zone = Engine::instance().game().world().physicalZoneEffect(pos);
    MovementEnvironment environment{groundHeight, {0, 1, 0},
                                    Engine::instance().game().world().isUnderwater(pos),
                                    zone.velocityMod, zone.gravityMod, zone.appliedForce,
                                    Engine::instance().game().getGravity()};
    Movement::step(state, movement, environment, dt);
    player->setJumpWasDown(state.jumpWasDown);
    pos = state.position; vel = state.velocity;
    bool onGround = state.onGround;
    float energy = state.energy;
    float heat = Math::clamp(player->heat() + (input.jet ? 45.0f : -25.0f) * dt, 0.0f, 100.0f);

    // Resolve interior collision (push player out of walls/floors)
    resolveCollision(player, pos, vel, dt);

    // Re-check ground after collision resolve. Do not snap to a ceiling or to
    // an unwalkable slope; the contact normal determines grounded state.
    groundHeight = Engine::instance().game().world().getFloorHeight(pos.x, pos.y, pos.z);
    Point3F floorNormal{0, 1, 0};
    float floorT = 0.0f;
    Point3F floorPoint{};
    const auto& collision = Engine::instance().game().world().collision();
    if (collision.loaded && collision.raycast({pos.x, pos.y + 0.2f, pos.z},
                                               {0, -1, 0}, 0.4f, floorT,
                                               floorPoint, floorNormal)) {
        groundHeight = floorPoint.y;
    }
    if (pos.y < groundHeight) { pos.y = groundHeight; if (vel.y < 0) vel.y = 0; }
    onGround = pos.y <= groundHeight + 0.1f && floorNormal.y >= 0.55f;
    if (onGround) Movement::projectOnPlane(vel, floorNormal);

    // Update rotation from look input
    rot.x -= input.lookDelta.x;
    rot.z += input.lookDelta.y;
    rot.x = Math::clamp(rot.x, -Math::PI * 0.45f, Math::PI * 0.45f);

    player->setPosition(pos);
    player->setRotation(rot);
    player->setVelocity(vel);
    player->setEnergy(energy);
    player->setHeat(heat);
    player->setOnGround(onGround);
}

void Physics::resolveCollision(Player* player, Point3F& pos, Point3F& vel, float dt) {
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
        const float length = std::sqrt(pushOut.x * pushOut.x + pushOut.y * pushOut.y + pushOut.z * pushOut.z);
        if (length > 0.0001f) {
            const Point3F normal{pushOut.x / length, pushOut.y / length, pushOut.z / length};
            const float intoWall = vel.x * normal.x + vel.y * normal.y + vel.z * normal.z;
            if (intoWall < 0.0f) {
                vel.x -= normal.x * intoWall;
                vel.y -= normal.y * intoWall;
                vel.z -= normal.z * intoWall;
            }
        }
    }
}

Physics::RayCastResult Physics::rayCast(const Point3F& origin, const Point3F& dir, float maxDist) {
    RayCastResult result;
    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length <= 0.0001f || maxDist <= 0.0f) return result;
    const Point3F normalized{dir.x / length, dir.y / length, dir.z / length};

    const auto& collision = Engine::instance().game().world().collision();
    if (collision.loaded) {
        float hitDistance = 0.0f;
        Point3F hitPoint{}, hitNormal{};
        if (collision.raycast(origin, normalized, maxDist, hitDistance, hitPoint, hitNormal)) {
            result.hit = true;
            result.point = hitPoint;
            result.normal = hitNormal;
            result.distance = hitDistance;
        }
    }

    // Simple ground test
    float h = Engine::instance().game().world().getFloorHeight(origin.x, origin.y, origin.z);
    if (origin.y > h && normalized.y < 0) {
        float t = (origin.y - h) / (-normalized.y);
        if (t >= 0 && t <= maxDist) {
            if (!result.hit || t < result.distance) {
                result.hit = true;
                result.point = {origin.x + normalized.x * t, h, origin.z + normalized.z * t};
                result.normal = {0, 1, 0};
                result.distance = t;
            }
        }
    }

    return result;
}
