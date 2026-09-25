#include "game/physics.h"
#include "game/game.h"
#include "game/movement.h"
#include "game/weapon.h"
#include "game/collision.h"
#include "core/engine.h"
#include "core/input_parity.h"
#include <cmath>

Physics::Physics() {}
Physics::~Physics() {}

static Point3F terrainContactNormal(const World& world, float x, float z) {
    constexpr float sampleSpacing = 0.25f;
    const float left = world.getHeight(x - sampleSpacing, z);
    const float right = world.getHeight(x + sampleSpacing, z);
    const float back = world.getHeight(x, z - sampleSpacing);
    const float front = world.getHeight(x, z + sampleSpacing);
    if (left <= -1.0e9f || right <= -1.0e9f ||
        back <= -1.0e9f || front <= -1.0e9f)
        return {0.0f, 1.0f, 0.0f};
    return terrainNormalFromHeights(left, right, back, front, sampleSpacing);
}

void Physics::update(Player* player, float dt, const Game::InputMove& input) {
    if (!player) return;

    // Movement::step caps its own integration delta, but this wrapper also
    // performs collision resolution, landing damage, heat, and rotation.
    // Keep every part of the native tick on the same bounded step so a frame
    // hitch cannot teleport the player or apply a full frame of extra damage.
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    dt = std::min(dt, 0.05f);

    constexpr float playerRadius = 0.5f;

    Point3F pos = player->position();
    Point3F rot = player->rotation();
    Point3F vel = player->velocity();

    // Apply the current look sample before translating input.  Using the
    // previous yaw makes a sharp turn move one frame in the old direction,
    // which is visible immediately when strafing or skiing.
    float yaw = rot.z + input.lookDelta.y;
    auto& world = Engine::instance().game().world();
    float groundHeight = world.getFloorHeight(pos.x, pos.y, pos.z);
    Point3F floorNormal{0, 1, 0};
    const auto& collision = world.collision();
    if (collision.loaded) {
        float floorT = 0.0f;
        Point3F floorPoint{};
        // Feed the actual contact plane to prediction.  Treating every
        // interior floor as horizontal makes movement stick and snap on ramps.
        if (collision.raycast({pos.x, pos.y + 0.2f, pos.z}, {0, -1, 0},
                              20000.0f, floorT, floorPoint, floorNormal) &&
            floorNormal.y >= 0.55f) {
            // A downward ray can also hit the top of an interior ceiling when
            // an actor is above the room.  Only upward-facing surfaces are
            // valid floors; otherwise a ceiling becomes a false ground plane.
            groundHeight = floorPoint.y;
        } else {
            floorNormal = terrainContactNormal(world, pos.x, pos.z);
        }
    } else {
        floorNormal = terrainContactNormal(world, pos.x, pos.z);
    }
    MovementState state{pos, vel, player->energy(), player->isOnGround(), player->jumpWasDown()};
    const bool wasOnGround = state.onGround;
    const float previousDownwardSpeed = std::max(0.0f, -state.velocity.y);
    state.jumpDelay = player->jumpDelay();
    MovementInput movement;
    movement.forward = (input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f);
    movement.strafe = (input.right ? 1.0f : 0.0f) - (input.left ? 1.0f : 0.0f);
    movement.jump = input.jump; movement.jet = input.jet; movement.yaw = yaw;
    const bool jetting = Movement::isJetting(movement, player->energy());
    const auto zone = Engine::instance().game().world().physicalZoneEffect(pos);
    const auto& config = Engine::instance().game().config();
    MovementEnvironment environment{groundHeight, floorNormal,
                                    world.isUnderwater(pos), zone.velocityMod,
                                    zone.gravityMod, zone.appliedForce,
                                    Engine::instance().game().getGravity(),
                                    0.5f, config.moveSpeed, config.jumpSpeed,
                                    config.jetSpeed};
    environment.maxEnergy = player->maxEnergy();
    Movement::step(state, movement, environment, dt);
    player->setJumpWasDown(state.jumpWasDown);
    player->setJumpDelay(state.jumpDelay);
    pos = state.position; vel = state.velocity;
    bool onGround = state.onGround;
    float energy = state.energy;
    float heat = Math::clamp(player->heat() + (jetting ? 45.0f : -25.0f) * dt, 0.0f, 100.0f);

    // Resolve interior collision (push player out of walls/floors)
    resolveCollision(player, pos, vel, dt);

    // Re-check ground after collision resolve. Do not snap to a ceiling or to
    // an unwalkable slope; the contact normal determines grounded state.
    groundHeight = Engine::instance().game().world().getFloorHeight(pos.x, pos.y, pos.z);
    float floorT = 0.0f;
    Point3F floorPoint{};
    // Probe from above the player's center far enough to reach the authored
    // floor while the collision sphere is resting on it.  A 0.4-unit ray
    // stops above a horizontal floor when the native player radius is 0.5.
    const bool hasInteriorFloor = collision.loaded &&
        collision.raycast({pos.x, pos.y + 0.2f, pos.z}, {0, -1, 0},
                          playerRadius + 0.25f,
                          floorT, floorPoint, floorNormal) &&
        floorNormal.y >= 0.55f;
    if (hasInteriorFloor) {
        groundHeight = floorPoint.y;
    } else {
        floorNormal = terrainContactNormal(world, pos.x, pos.z);
    }
    // groundHeight is the authored surface, while the player position is the
    // sphere center.  Snapping the center to the surface embeds the player by
    // its radius and causes visible floor jitter on the next collision pass.
    // A sphere resting on a ramp is offset vertically by radius * normal.y,
    // not by the full radius used for a horizontal floor.
    const float contactY = groundHeight +
        std::max(0.0f, playerRadius * floorNormal.y);
    if (pos.y < contactY) { pos.y = contactY; if (vel.y < 0) vel.y = 0; }
    onGround = Movement::isGroundedAtFloor(pos.y, groundHeight, playerRadius, floorNormal);
    if (onGround) Movement::projectOnPlane(vel, floorNormal);

    // Apply landing damage once, at the transition from airborne to grounded.
    // Doing this after collision resolution also handles a fast fall that
    // crosses the floor between simulation ticks.
    if (!wasOnGround && onGround) {
        const float damage = Movement::fallDamage(
            std::max(state.landingSpeed, previousDownwardSpeed));
        if (damage > 0.0f) player->applyDamage(damage);
    }

    // Update rotation from look input
    rot.x -= input.lookDelta.x;
    rot.z += input.lookDelta.y;
    rot.x = clampCameraPitch(rot.x);

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
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) || !rayCastInputUsable(dir, maxDist))
        return result;
    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length <= 0.0001f) return result;
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

    // Sweep the authored floor instead of testing only the ray origin.  The
    // old check missed sloped terrain and rays that crossed a ridge between
    // frames, making targeting and interaction rays pass through the ground.
    auto& world = Engine::instance().game().world();
    const Point3F end{origin.x + normalized.x * maxDist,
                      origin.y + normalized.y * maxDist,
                      origin.z + normalized.z * maxDist};
    const float startSurface = world.getFloorHeight(origin.x, origin.y, origin.z);
    const float endSurface = world.getFloorHeight(end.x, end.y, end.z);
    // A horizontal trace can still enter a rising terrain surface.  Restricting
    // this check to downward rays lets targeting and interaction traces pass
    // through ridges when their vertical component is zero (or points up).
    if (startSurface > -1.0e9f && endSurface > -1.0e9f) {
        float t = 0.0f;
        const bool startsInside = origin.y <= startSurface;
        const bool crossesSurface = segmentSurfaceCrossing(
            origin.y, startSurface, end.y, endSurface, t);
        if (startsInside || crossesSurface) {
            const float distance = startsInside ? 0.0f : t * maxDist;
            const float surface = startsInside ? startSurface :
                startSurface + (endSurface - startSurface) * t;
            if (!result.hit || distance < result.distance) {
                result.hit = true;
                result.point = {origin.x + normalized.x * distance, surface,
                                origin.z + normalized.z * distance};
                // Terrain ray hits need the authored slope normal.  Returning
                // an up vector makes decals, interaction traces, and impact
                // effects lie flat whenever the trace meets a ramp.
                result.normal = terrainContactNormal(
                    world, result.point.x, result.point.z);
                result.distance = distance;
            }
        }
    }

    return result;
}
