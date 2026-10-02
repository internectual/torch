#pragma once
// Client Player::processTick / updateMove / updatePos / unpackUpdate for
// demo ghosts (as ported by t2-mapper stream/playerPrediction.ts and
// collision/playerCollision.ts). Torque space (Z up). Collision is a working
// set of triangles gathered once per tick, swept with a box/triangle SAT.
#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace PlayerPrediction {

// Retail physics use 1/32 s ticks (not TickMs / 1000).
constexpr float TickSec = 1.0f / 32.0f;
constexpr int MaxPredictionTicks = 30;
constexpr float MinWarpTicks = 0.5f;
constexpr int MaxWarpTicks = 3;
constexpr int MoveState = 1, RecoverState = 2;
constexpr int JumpSkipContactsMax = 8;

inline Point3F add(const Point3F& a, const Point3F& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Point3F sub(const Point3F& a, const Point3F& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Point3F mul(const Point3F& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Point3F cross(const Point3F& a, const Point3F& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Point3F& a) { return std::sqrt(dot(a, a)); }
inline Point3F normalize(const Point3F& a) {
    const float l = length(a);
    return l > 0.0f ? mul(a, 1.0f / l) : Point3F{0, 0, 0};
}
inline float angleDifference(float a, float b) {
    constexpr float TwoPi = 6.28318530717958647692f, Pi = 3.14159265358979323846f;
    float d = std::fmod(a - b + Pi, TwoPi);
    if (d < 0.0f) d += TwoPi;
    return d - Pi;
}

struct Move {
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
    bool freeLook = false;
    bool trigger[6]{};
};

// Move::unclamp for the packed ghost/demo move.
inline Move unclampMove(int px, int py, int pz, uint32_t pyaw, uint32_t ppitch, uint32_t proll,
                        bool freeLook, const bool* triggers) {
    constexpr float Scale = 6.28318530717958647692f / 65536.0f;
    Move move;
    move.x = px / 16.0f - 1.0f;
    move.y = py / 16.0f - 1.0f;
    move.z = pz / 16.0f - 1.0f;
    move.yaw = (float)(int16_t)(pyaw & 0xffff) * Scale;
    move.pitch = (float)(int16_t)(ppitch & 0xffff) * Scale;
    move.roll = (float)(int16_t)(proll & 0xffff) * Scale;
    move.freeLook = freeLook;
    for (int i = 0; i < 6; ++i) move.trigger[i] = triggers[i];
    return move;
}

// PlayerData fields the client simulation reads.
struct Data {
    bool renderFirstPerson = false;
    float mass = 1, drag = 0, density = 1, maxEnergy = 0;
    float minLookAngle = -1.5708f, maxLookAngle = 1.5708f, maxFreelookAngle = 1.5708f;
    float maxStepHeight = 0;
    float jetForce = 0, underwaterJetForce = 0, underwaterVertJetFactor = 0;
    float jetEnergyDrain = 0, underwaterJetEnergyDrain = 0, minJetEnergy = 0;
    float maxJetForwardSpeed = 0, maxJetHorizontalPercentage = 0;
    float runForce = 0, runEnergyDrain = 0, minRunEnergy = 0;
    float maxForwardSpeed = 0, maxBackwardSpeed = 0, maxSideSpeed = 0;
    float maxUnderwaterForwardSpeed = 0, maxUnderwaterBackwardSpeed = 0, maxUnderwaterSideSpeed = 0;
    float runSurfaceAngle = 0;
    float recoverDelay = 0, recoverRunForceScale = 1;
    float jumpForce = 0, jumpEnergyDrain = 0, minJumpEnergy = 0, minJumpSpeed = 0, maxJumpSpeed = 0;
    float jumpSurfaceAngle = 0;
    int jumpDelay = 0;
    float horizMaxSpeed = std::numeric_limits<float>::infinity();
    float horizResistSpeed = std::numeric_limits<float>::infinity();
    float horizResistFactor = 0;
    float upMaxSpeed = std::numeric_limits<float>::infinity();
    float upResistSpeed = std::numeric_limits<float>::infinity();
    float upResistFactor = 0;
    float minImpactSpeed = std::numeric_limits<float>::infinity();
    Point3F boxSize{1, 1, 2};
};

struct Triangle {
    Point3F a, b, c, n;
};

// Gathers the world triangles overlapping a box (Torque space), outward
// normals toward free space.
using GatherTriangles = std::function<void(const Point3F& min, const Point3F& max, std::vector<Triangle>& out)>;
// The water surface height at an XY, or NaN where there is no water.
using WaterLevel = std::function<float(float x, float y)>;

struct Box { Point3F min, max; };

inline Box playerBox(const Point3F& position, const Point3F& size) {
    return {{position.x - size.x / 2, position.y - size.y / 2, position.z},
            {position.x + size.x / 2, position.y + size.y / 2, position.z + size.z}};
}

// Box/triangle overlap by the separating axis theorem (Akenine-Moller).
inline bool boxIntersectsTriangle(const Box& box, const Triangle& t) {
    const Point3F c = mul(add(box.min, box.max), 0.5f);
    const Point3F e = mul(sub(box.max, box.min), 0.5f);
    const Point3F v[3] = {sub(t.a, c), sub(t.b, c), sub(t.c, c)};
    auto separated = [&](const Point3F& axis) {
        const float p0 = dot(v[0], axis), p1 = dot(v[1], axis), p2 = dot(v[2], axis);
        const float r = e.x * std::fabs(axis.x) + e.y * std::fabs(axis.y) + e.z * std::fabs(axis.z);
        return std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r;
    };
    if (separated({1, 0, 0}) || separated({0, 1, 0}) || separated({0, 0, 1})) return false;
    const Point3F edges[3] = {sub(v[1], v[0]), sub(v[2], v[1]), sub(v[0], v[2])};
    if (separated(cross(edges[0], edges[1]))) return false;
    for (const auto& edge : edges) {
        if (separated({0, edge.z, -edge.y}) || separated({-edge.z, 0, edge.x}) || separated({edge.y, -edge.x, 0}))
            return false;
    }
    return true;
}

// A PhysicalZone near the player: its faces (outward normals) and the
// factor its faces apply to the velocity of a box that meets them.
struct Zone {
    std::vector<Triangle> triangles;
    float velocityMod = 1.0f;
};
using GatherZones = std::function<void(const Point3F& min, const Point3F& max, std::vector<Zone>& out)>;

struct Collision {
    std::vector<Triangle> triangles;
    // The active physical zones (the server's; empty without them).
    std::vector<Zone> zones;
    GatherZones gatherZones;
    float hitTime = 1.0f;
    float hitHeight = -std::numeric_limits<float>::infinity();
    Point3F hitNormal{0, 0, 1};

    // Player::updateWorkingCollisionSet: the box grown by the tick's travel
    // plus acceleration headroom, and the step height upward.
    void prepare(const GatherTriangles& gather, const Point3F& position, const Point3F& size,
                 const Point3F& travel, float maxStep) {
        Box box = playerBox(position, size);
        const float grow = (length(travel) + 10.0f / 32.0f) * 1.1f + 0.1f;
        box.min = sub(box.min, {grow, grow, grow});
        box.max = add(box.max, {grow, grow, grow + maxStep});
        triangles.clear();
        if (gather) gather(box.min, box.max, triangles);
        zones.clear();
        if (gatherZones) gatherZones(box.min, box.max, zones);
    }

    bool findContact(const Point3F& position, const Point3F& size, Point3F& out) const {
        Box box = playerBox(position, size);
        box.min.z = position.z - 0.03f;
        box.max.z = position.z + 0.03f;
        float best = 0.0f;
        for (const auto& t : triangles) {
            if (t.n.z > best && boxIntersectsTriangle(box, t)) {
                best = t.n.z;
                out = t.n;
            }
        }
        return best > 0.0f;
    }

    // Continuous box/triangle SAT against the faces the box travels into.
    bool sweep(const Point3F& position, const Point3F& size, const Point3F& travel) {
        const Box box = playerBox(position, size);
        const Point3F center = mul(add(box.min, box.max), 0.5f);
        const Point3F extent = mul(size, 0.5f);
        hitTime = 1.0f;
        hitHeight = -std::numeric_limits<float>::infinity();
        float bestDot = -std::numeric_limits<float>::infinity();
        for (const auto& t : triangles) {
            const float faceDot = -dot(t.n, travel);
            if (faceDot <= 1e-10f) continue;
            const Point3F a = sub(t.a, center), b = sub(t.b, center), c = sub(t.c, center);
            float enter = -std::numeric_limits<float>::infinity(), leave = std::numeric_limits<float>::infinity();
            auto axis = [&](float x, float y, float z) {
                const float p = a.x * x + a.y * y + a.z * z, q = b.x * x + b.y * y + b.z * z,
                            r = c.x * x + c.y * y + c.z * z;
                const float radius = std::fabs(x) * extent.x + std::fabs(y) * extent.y + std::fabs(z) * extent.z;
                const float lo = std::min({p, q, r}) - radius, hi = std::max({p, q, r}) + radius;
                const float speed = x * travel.x + y * travel.y + z * travel.z;
                if (std::fabs(speed) < 1e-12f) return lo <= 1e-9f && hi >= -1e-9f;
                const float t0 = lo / speed, t1 = hi / speed;
                enter = std::max(enter, std::min(t0, t1));
                leave = std::min(leave, std::max(t0, t1));
                return enter <= leave + 1e-9f;
            };
            if (!axis(1, 0, 0) || !axis(0, 1, 0) || !axis(0, 0, 1) || !axis(t.n.x, t.n.y, t.n.z)) continue;
            bool separated = false;
            const Point3F verts[3] = {a, b, c};
            for (int j = 0; j < 3 && !separated; ++j) {
                const Point3F edge = sub(verts[(j + 1) % 3], verts[j]);
                if (!axis(0, edge.z, -edge.y) || !axis(-edge.z, 0, edge.x) || !axis(edge.y, -edge.x, 0))
                    separated = true;
            }
            // A box already penetrating a surface must be able to escape it.
            if (separated || enter < -1e-7f || enter > hitTime + 1e-8f || leave < 0.0f) continue;
            const float time = std::max(0.0f, enter);
            if (time < hitTime - 1e-8f) {
                bestDot = -std::numeric_limits<float>::infinity();
                hitHeight = -std::numeric_limits<float>::infinity();
            }
            hitTime = time;
            hitHeight = std::max({hitHeight, t.a.z, t.b.z, t.c.z});
            if (faceDot > bestDot) {
                bestDot = faceDot;
                hitNormal = t.n;
            }
        }
        return hitTime < 1.0f;
    }

    // Player::step: clip the polygons against the box at the destination.
    float stepHeight(const Point3F& position, const Point3F& size, const Point3F& travel, float maxStep) const {
        Box box = playerBox(add(position, travel), size);
        box.max.z += maxStep + 0.01f;
        float height = position.z - 0.01f;
        for (const auto& t : triangles) {
            if (!boxIntersectsTriangle(box, t)) continue;
            std::vector<Point3F> vertices = {t.a, t.b, t.c}, clipped;
            for (int axis = 0; axis < 3 && !vertices.empty(); ++axis) {
                for (int sign : {-1, 1}) {
                    auto comp = [&](const Point3F& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
                    const float limit = sign < 0 ? comp(box.min) : comp(box.max);
                    clipped.clear();
                    Point3F previous = vertices.back();
                    float pd = sign * (comp(previous) - limit);
                    for (const auto& vertex : vertices) {
                        const float d = sign * (comp(vertex) - limit);
                        if ((d <= 0) != (pd <= 0)) {
                            const float f = pd / (pd - d);
                            clipped.push_back(add(previous, mul(sub(vertex, previous), f)));
                        }
                        if (d <= 0) clipped.push_back(vertex);
                        previous = vertex;
                        pd = d;
                    }
                    vertices = clipped;
                    if (vertices.empty()) break;
                }
            }
            for (const auto& v : vertices) height = std::max(height, v.z + 0.01f);
        }
        const float rise = height - position.z;
        return rise > 0.0f && rise < maxStep ? rise : 0.0f;
    }
};

// One ghost's client simulation state.
struct State {
    bool initialized = false;
    Point3F position{}, velocity{}, posVec{};
    float yaw = 0, rotVec = 0, headPitch = 0, headYaw = 0;
    float energy = 0;
    bool jetting = false, falling = false, mounted = false, allowFreelook = false, disableMove = false;
    int damageState = 0;
    int predictionCount = 0, warpTicks = 0;
    int actionState = MoveState, recoverTicks = 0, jumpDelay = 0, jumpSurfaceLastContact = 0, contactTimer = 0;
    Point3F warpOffset{};
    float rotOffset = 0;
    Point3F jumpSurfaceNormal{0, 0, 1};
    Move move;
};

// A ghost MoveMask (or control packet) update.
struct Update {
    bool hasPosition = false;
    Point3F position{}, velocity{};
    bool hasMove = false;
    Move move;
    int actionState = MoveState, recoverTicks = 0;
    float headX = 0, headZ = 0; // ghost: [-1, 1] of maxLookAngle; packet: radians
    float rotZ = 0;
    bool falling = false, jetting = false;
    bool allowWarp = true;
    bool headInRadians = false; // control packets carry radians
};

// Player::unpackUpdate: packet values are authoritative; a warp spreads a
// small correction over at most three ticks instead of snapping.
inline void unpackUpdate(State& s, const Data& d, const Update& u, bool initial, bool headInRadians) {
    if (!u.hasPosition) return;
    const float oldSpeed = length(s.velocity);
    s.predictionCount = MaxPredictionTicks;
    s.velocity = u.velocity;
    if (u.hasMove) s.move = u.move;
    s.actionState = u.actionState;
    s.recoverTicks = u.recoverTicks;
    s.headPitch = headInRadians ? u.headX : u.headX * d.maxLookAngle;
    s.headYaw = headInRadians ? u.headZ : u.headZ * d.maxLookAngle;
    s.falling = u.falling;
    s.jetting = u.jetting;
    s.warpTicks = 0;
    if (s.initialized && !initial && u.allowWarp) {
        s.warpOffset = sub(u.position, s.position);
        const float distancePerTick = (oldSpeed + length(s.velocity)) * 0.5f * TickSec;
        const float ticks = distancePerTick > 0.00001f ? length(s.warpOffset) / distancePerTick : (float)MaxWarpTicks;
        if (ticks > MinWarpTicks) {
            s.warpTicks = std::min(MaxWarpTicks, std::max(1, (int)std::floor(ticks + 0.5f)));
            s.warpOffset = mul(s.warpOffset, 1.0f / s.warpTicks);
            s.rotOffset = angleDifference(u.rotZ, s.yaw) / s.warpTicks;
            return;
        }
    }
    s.initialized = true;
    s.position = u.position;
    s.yaw = u.rotZ;
    s.posVec = {0, 0, 0};
    s.rotVec = 0;
}

inline bool isRunSurface(const Data& d, const Point3F& contactNormal) {
    return contactNormal.z > std::cos(d.runSurfaceAngle * 3.14159265358979f / 180.0f);
}

inline void updateMove(State& s, const Data& d, float gravity, Collision& collision, const WaterLevel& water,
                       float gravityMod, const Point3F& appliedForce) {
    const Move& move = s.move;
    const float dt = TickSec, mass = d.mass > 0 ? d.mass : 1.0f;
    if (!s.damageState) {
        const float oldYaw = s.yaw;
        const float pitch = angleDifference(move.pitch, 0), yaw = angleDifference(move.yaw, 0);
        s.headPitch = std::clamp(s.headPitch + pitch, d.minLookAngle, d.maxLookAngle);
        if (move.freeLook && s.allowFreelook) {
            s.headYaw = std::clamp(s.headYaw + yaw, -d.maxFreelookAngle, d.maxFreelookAngle);
        } else {
            constexpr float TwoPi = 6.28318530717958647692f;
            s.yaw = std::fmod(std::fmod(s.yaw + yaw, TwoPi) + TwoPi, TwoPi);
            s.headYaw *= 0.5f;
        }
        s.rotVec = angleDifference(oldYaw, s.yaw);
    }
    float coverage = 0.0f;
    if (water) {
        const float surface = water(s.position.x, s.position.y);
        if (std::isfinite(surface)) coverage = std::clamp((surface - s.position.z) / d.boxSize.z, 0.0f, 1.0f);
    }
    const bool underwater = coverage >= 0.9f;
    const float drag = coverage >= 0.1f ? d.drag * 15.0f * coverage : 0.0f;
    const float buoyancy = coverage >= 0.1f ? coverage / (d.density != 0 ? d.density : 1.0f) : 0.0f;
    const bool moving = s.actionState == MoveState && !s.damageState && !s.disableMove;
    const float x = moving ? move.x : 0.0f, y = moving ? move.y : 0.0f;
    const float sn = std::sin(s.yaw), cs = std::cos(s.yaw);
    const Point3F moveVec{cs * x + sn * y, -sn * x + cs * y, 0.0f};
    const float forward = underwater ? d.maxUnderwaterForwardSpeed : d.maxForwardSpeed;
    const float backward = underwater ? d.maxUnderwaterBackwardSpeed : d.maxBackwardSpeed;
    const float side = underwater ? d.maxUnderwaterSideSpeed : d.maxSideSpeed;
    const float moveSpeed = std::max((y > 0 ? forward : backward) * std::fabs(y), side * std::fabs(x));
    Point3F acceleration{appliedForce.x / mass * dt, appliedForce.y / mass * dt,
                         gravity * gravityMod * dt + appliedForce.z / mass * dt};
    Point3F contactNormal{0, 0, 0};
    const bool contacted = collision.findContact(s.position, d.boxSize, contactNormal);
    const bool run = contacted && isRunSurface(d, contactNormal);
    s.contactTimer = run ? 0 : s.contactTimer + 1;
    const bool jump = contacted && contactNormal.z > std::cos(d.jumpSurfaceAngle * 3.14159265358979f / 180.0f);
    if (jump) s.jumpSurfaceNormal = contactNormal;
    if (run) {
        const float into = -dot(acceleration, contactNormal);
        if (into > 0) acceleration = add(acceleration, mul(contactNormal, into + 0.002f));
        if (length(acceleration) < 0.0001f) acceleration = {0, 0, 0};
        Point3F requested = moveVec;
        if (s.energy < d.minRunEnergy) requested = {0, 0, 0};
        else s.energy -= d.runEnergyDrain;
        if (dot(requested, requested) > 0) {
            const Point3F across = normalize({requested.y, -requested.x, 0});
            const Point3F along = sub(contactNormal, mul(across, dot(across, contactNormal)));
            requested = sub(requested, mul(along, dot(requested, along)));
            requested = mul(normalize(requested), moveSpeed);
        }
        const Point3F current = add(s.velocity, acceleration);
        // The ski branch (renderFirstPerson, MoveState, jump held) keeps the
        // existing speed along the requested direction; no input coasts.
        if (d.renderFirstPerson && s.actionState != RecoverState && move.trigger[2]) {
            if (dot(requested, requested) == 0) requested = current;
            else {
                const float speed = length(requested);
                const Point3F dir = mul(requested, 1.0f / speed);
                requested = mul(dir, std::max(speed, dot(current, dir)));
            }
        }
        requested = sub(requested, current);
        const float maxAcc = d.runForce / mass * dt * (s.actionState == RecoverState ? d.recoverRunForceScale : 1.0f);
        const float len = length(requested);
        if (len > maxAcc) requested = mul(requested, maxAcc / len);
        acceleration = add(acceleration, requested);
    }
    s.jetting = moving && move.trigger[3] && s.energy >= d.minJetEnergy;
    if (s.jetting) {
        s.energy -= underwater ? d.underwaterJetEnergyDrain : d.jetEnergyDrain;
        const float force = underwater ? d.underwaterJetForce : d.jetForce;
        Point3F requested{0, 0, force};
        if (dot(moveVec, moveVec) > 0 && s.jumpSurfaceLastContact >= 8) {
            const Point3F dir = normalize(moveVec);
            const float speed = dot(s.velocity, dir), max = d.maxJetForwardSpeed;
            const float fraction = std::min(d.maxJetHorizontalPercentage,
                                            speed <= 0 ? 1.0f : speed > max ? 0.0f : 1.0f - speed / max);
            requested = mul(dir, force * fraction);
            requested.z = force * (1.0f - fraction);
        }
        if (drag != 0 && coverage > 0.25f && d.underwaterJetForce != 0)
            requested.z *= d.jetForce / d.underwaterJetForce * d.underwaterVertJetFactor;
        acceleration = add(acceleration, mul(requested, dt / mass));
    }
    if (move.trigger[2] && moving && !s.jumpDelay && s.energy >= d.minJumpEnergy &&
        s.jumpSurfaceLastContact < JumpSkipContactsMax && s.velocity.z <= d.maxJumpSpeed) {
        const float scale = s.velocity.z <= d.minJumpSpeed ? 1.0f
            : 1.0f - (s.velocity.z - d.minJumpSpeed) / (d.maxJumpSpeed - d.minJumpSpeed);
        const Point3F dir = normalize(moveVec);
        const float along = dot(dir, s.jumpSurfaceNormal), impulse = d.jumpForce / mass;
        if (along > 0) acceleration = add(acceleration, mul(dir, impulse * along));
        acceleration.z += s.jumpSurfaceNormal.z * impulse * scale;
        s.jumpDelay = d.jumpDelay;
        s.energy -= d.jumpEnergyDrain;
        s.jumpSurfaceLastContact = 8;
    } else {
        s.jumpSurfaceLastContact = jump ? 0 : s.jumpSurfaceLastContact + 1;
    }
    if (s.jumpDelay > 0) s.jumpDelay--;
    s.velocity = add(s.velocity, acceleration);
    const float horizontal = std::hypot(s.velocity.x, s.velocity.y);
    if (horizontal > d.horizResistSpeed) {
        float cap = std::min(horizontal, d.horizMaxSpeed);
        cap -= d.horizResistFactor * dt * (cap - d.horizResistSpeed);
        s.velocity.x *= cap / horizontal;
        s.velocity.y *= cap / horizontal;
    }
    if (s.velocity.z > d.upResistSpeed) {
        s.velocity.z = std::min(s.velocity.z, d.upMaxSpeed);
        s.velocity.z -= d.upResistFactor * dt * (s.velocity.z - d.upResistSpeed);
    }
    if (buoyancy != 0 && (buoyancy > 1 || dot(s.velocity, s.velocity) > 0.0001f || !run))
        s.velocity.z -= buoyancy * gravity * gravityMod * dt;
    s.velocity = mul(s.velocity, 1.0f - drag * dt);
    if (s.disableMove) s.velocity.x = s.velocity.y = 0;
    s.falling = !run && s.velocity.z < -10.0f;
    s.energy = std::max(0.0f, s.energy);
}

// Player::updatePos: true when the player moved at least a thousandth of
// its speed (false: blocked).
inline bool updatePos(State& s, const Data& d, Collision& collision, const Point3F& initial,
                      float travelTime = TickSec) {
    float time = travelTime, maxStep = d.maxStepHeight;
    const float initialSpeed = length(s.velocity);
    float totalMotion = 0;
    Point3F firstNormal{0, 0, 0};
    for (int retry = 0; retry < 5; ++retry) {
        const float speed = length(s.velocity);
        if (speed == 0) return totalMotion >= 0.001f * initialSpeed;
        const Point3F travel = mul(s.velocity, time);
        // The physical zones' faces the swept box meets (a separate poly
        // list) scale the velocity; a free move still covers the travel.
        for (const auto& zone : collision.zones) {
            Collision faces;
            faces.triangles = zone.triangles;
            if (faces.sweep(s.position, d.boxSize, travel)) s.velocity = mul(s.velocity, zone.velocityMod);
        }
        if (!collision.sweep(s.position, d.boxSize, travel)) {
            s.position = add(s.position, travel);
            totalMotion += speed * time;
            return totalMotion >= 0.001f * initialSpeed;
        }
        const float velLen = length(s.velocity);
        const float dt = time * collision.hitTime;
        const float backOff = velLen > 0.0f ? std::min(0.01f / velLen, dt) : 0.0f;
        s.position = add(s.position, mul(s.velocity, dt - backOff));
        totalMotion += velLen * (dt - backOff);
        time -= dt;
        s.falling = false;
        const Point3F normal = collision.hitNormal;
        if (collision.hitHeight < s.position.z + d.maxStepHeight && std::fabs(normal.z) < 0.173f) {
            const float rise = collision.stepHeight(s.position, d.boxSize, mul(s.velocity, time), maxStep);
            if (rise > 0) {
                s.position.z += rise;
                maxStep -= rise;
                continue;
            }
        }
        const float into = -dot(s.velocity, normal);
        if (into > d.minImpactSpeed && !s.damageState && s.actionState != RecoverState) {
            s.actionState = RecoverState;
            const float value = into - d.minImpactSpeed, range = d.minImpactSpeed * 0.9f;
            const int delay = (int)d.recoverDelay;
            s.recoverTicks = value < range ? 1 + (int)std::floor(delay * value / range) : delay;
        }
        const Point3F push = mul(normal, into + 0.01f);
        s.velocity = add(s.velocity, push);
        if (retry == 0) firstNormal = normal;
        else if (retry == 1 && dot(push, firstNormal) < 0 && dot(normal, firstNormal) < 0) {
            Point3F crease = cross(normal, firstNormal);
            if (dot(crease, crease) > 0) {
                crease = mul(normalize(crease), length(s.velocity) * (dot(crease, s.velocity) < 0 ? -1.0f : 1.0f));
                s.velocity = crease;
            }
        }
    }
    s.position = initial;
    s.velocity = {0, 0, 0};
    return totalMotion >= 0.001f * initialSpeed;
}

// Player::processTick. `move` is the controlling client's move for this
// tick, else null: other ghosts keep predicting their last move for up to
// MaxPredictionTicks.
inline void processTick(State& s, const Data& d, float gravity, const Move* move, float rechargeRate,
                        Collision& collision, const GatherTriangles& gather, const WaterLevel& water,
                        float gravityMod = 1.0f, const Point3F& appliedForce = {}) {
    s.posVec = {0, 0, 0};
    s.rotVec = 0;
    if (!s.initialized) return;
    if (s.warpTicks > 0) {
        s.warpTicks--;
        s.position = add(s.position, s.warpOffset);
        s.yaw += s.rotOffset;
        s.posVec = mul(s.warpOffset, -1.0f);
        s.rotVec = -s.rotOffset;
        return;
    }
    if (!move && s.predictionCount-- <= 0) return;
    if (move) s.move = *move;
    // Mounted players cannot find a run surface; their mount owns placement.
    if (s.mounted) {
        s.contactTimer++;
        return;
    }
    s.energy = std::min(d.maxEnergy, s.energy + rechargeRate);
    if (s.actionState == RecoverState && (s.recoverTicks-- == 0 || dot(s.velocity, s.velocity) > 1.69f))
        s.actionState = MoveState;
    const Point3F initial = s.position;
    collision.prepare(gather, s.position, d.boxSize, mul(s.velocity, TickSec), d.maxStepHeight);
    updateMove(s, d, gravity, collision, water, gravityMod, appliedForce);
    updatePos(s, d, collision, initial);
    s.posVec = sub(initial, s.position);
}

// Player::interpolateTick: `backDelta` is the fraction of a tick still to
// run (0 at the tick, 1 a whole tick back).
inline Point3F renderPosition(const State& s, float backDelta) {
    return add(s.position, mul(s.posVec, backDelta));
}
inline float renderYaw(const State& s, float backDelta) { return s.yaw + s.rotVec * backDelta; }

} // namespace PlayerPrediction
