#include "sim/projectiles.h"
#include "sim/shape_base.h"
#include "sim/player.h"
#include "sim/static_shapes.h"
#include "sim/game_connection.h"
#include "sim/sim_state.h"
#include "sim/engine_classes.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace SimContainer;

namespace {

constexpr uint32_t TickMs = 32;
constexpr float TickSec = 0.032f;
constexpr float DegToRad = 3.14159265358979323846f / 180.0f;

int tickRound(int ms) { return (ms + (int)TickMs - 1) & ~((int)TickMs - 1); }

std::string formatPoint(const Point3F& p) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%f %f %f", p.x, p.y, p.z);
    return buffer;
}

Point3F toPoint(const std::array<float, 3>& a) { return {a[0], a[1], a[2]}; }

ScriptObject* findScript(const std::string& handle) {
    if (handle.empty() || !ScriptEngine::exists()) return nullptr;
    return ScriptEngine::instance().findObject(handle.c_str());
}

std::string keyOf(ScriptObject* object) { return object ? ScriptEngine::instance().objectKey(object) : std::string(); }

int idOf(const std::string& key) {
    ScriptObject* object = findScript(key);
    return object ? ScriptEngine::instance().objectId(object) : 0;
}

// GameBase::getVelocity for the classes the server simulates.
Point3F objectVelocity(ScriptObject* object) {
    EngineObject* engine = object ? object->engine.get() : nullptr;
    if (auto* player = dynamic_cast<PlayerObject*>(engine)) return player->state.velocity;
    if (auto* item = dynamic_cast<ItemObject*>(engine)) return {item->velocity[0], item->velocity[1], item->velocity[2]};
    if (auto* projectile = dynamic_cast<ProjectileObject*>(engine)) return projectile->getVelocity();
    return {0, 0, 0};
}

// BitStream::dumbDownNormal: the vector as writeNormalVector/readNormalVector
// leave it.
Point3F dumbDownNormal(const Point3F& v, int bits) {
    auto quantize = [](float f, int b) {
        const int32_t max = (1 << b) - 1;
        const int32_t i = (int32_t)(((f + 1.0f) * 0.5f) * (float)max);
        return ((float)i * 2.0f) / (float)max - 1.0f;
    };
    const float phi = quantize(std::atan2(v.x, v.y) / 3.14159265358979323846f, bits + 1) * 3.14159265358979323846f;
    const float theta = quantize(std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) / (3.14159265358979323846f / 2.0f), bits) *
                        (3.14159265358979323846f / 2.0f);
    return {std::sin(phi) * std::cos(theta), std::cos(phi) * std::cos(theta), std::sin(theta)};
}

// Rodrigues: v about the unit axis k by angle (radians).
Point3F rotate(const Point3F& v, const Point3F& k, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0f - c)));
}

void writePoint(TorqueBitWriter& w, const Point3F& p) { w.writePoint({p.x, p.y, p.z}); }

bool controlledBy(const ShapeBase& object, GameConnection& connection) {
    return !object.controllingClient.empty() && connection.script &&
           object.controllingClient == ScriptEngine::instance().objectKey(connection.script);
}

} // namespace

// ─── Projectile ──────────────────────────────────────────────

ProjectileObject::ProjectileObject(const char* netClass) : netClass(netClass) { ghostable = true; }

ShapeBase* ProjectileObject::source() const { return sourceKey.empty() ? nullptr : EngineObjects::get<ShapeBase>(sourceKey); }
ShapeBase* ProjectileObject::vehicle() const { return vehicleKey.empty() ? nullptr : EngineObjects::get<ShapeBase>(vehicleKey); }
ScriptObject* ProjectileObject::sourceScript() const { return source() ? findScript(sourceKey) : nullptr; }

void ProjectileObject::setPosition(const Point3F& p) {
    transform = {1, 0, 0, p.x, 0, 1, 0, p.y, 0, 0, 1, p.z, 0, 0, 0, 1};
}

bool ProjectileObject::alive() const {
    if (!ScriptEngine::exists()) return false;
    auto& objects = ScriptEngine::instance().objects;
    auto it = objects.find(selfKey);
    return it != objects.end() && it->second && it->second->engine.get() == this;
}

void ProjectileObject::deleteSelf() {
    if (alive()) ScriptEngine::instance().deleteScriptObject(selfKey);
}

std::vector<ScriptObject*> ProjectileObject::timeoutExempt() const {
    std::vector<ScriptObject*> exempt;
    if (sourceIdTimeoutTicks) {
        if (source()) exempt.push_back(findScript(sourceKey));
        if (vehicle()) exempt.push_back(findScript(vehicleKey));
    }
    return exempt;
}

int ProjectileObject::ghostOf(GameConnection& connection, const std::string& key) const {
    return key.empty() || !findScript(key) ? -1 : connection.ghostIndex(key);
}

void ProjectileObject::readFields() {
    GameBase::readFields();
    auto& engine = ScriptEngine::instance();
    selfKey = engine.objectKey(script);
    selfId = engine.objectId(script);
    initialPosition = toPoint(Fields::point(script, "initialPosition", {0, 0, 0}));
    initialDirection = toPoint(Fields::point(script, "initialDirection", {0, 0, 1}));
    sourceSlot = Fields::s32(script, "sourceSlot", -1);
    // Projectile::onAdd: resolve the source and vehicle ShapeBases.
    const std::string sourceField = Fields::string(script, "sourceObject", "-1");
    ScriptObject* src = findScript(sourceField);
    if (src && dynamic_cast<ShapeBase*>(src->engine.get())) {
        sourceKey = keyOf(src);
        sourceObjectId = engine.objectId(src);
    } else {
        sourceObjectId = Fields::s32(script, "sourceObject", -1);
        if (sourceObjectId != -1)
            Console::instance().printf(LogLevel::Error, "Projectile::onAdd: mSourceObjectId is invalid");
    }
    ScriptObject* veh = findScript(Fields::string(script, "vehicleObject", "-1"));
    if (veh && dynamic_cast<ShapeBase*>(veh->engine.get())) {
        vehicleKey = keyOf(veh);
        vehicleObjectId = engine.objectId(veh);
    }
    if (dataBlock().empty()) return;
    // The source's velocity (the vehicle's when mounted) x velInheritFactor.
    Point3F sourceVel{0, 0, 0};
    if (src && !sourceKey.empty()) {
        sourceVel = veh && !vehicleKey.empty() ? objectVelocity(veh) : objectVelocity(src);
        sourceVel = mul(sourceVel, std::clamp(data("velInheritFactor", 1.0f), 0.0f, 1.0f));
    }
    const float l = len(sourceVel);
    excessVel = (uint32_t)(l + 0.5f);
    excessDir = excessVel != 0 ? mul(sourceVel, 1.0f / l) : Point3F{0, 0, 1};
    excessDirDumb = dumbDownNormal(excessDir, ExcessVelDirBits);
    currTick = 0;
    sourceIdTimeoutTicks = SourceIdTimeoutTicks;
    setPosition(initialPosition);
    added = true;
    onAddServer();
}

void ProjectileObject::processTick() {
    if (!script || !added) return;
    const std::shared_ptr<EngineObject> keep = script->engine; // callbacks may delete us
    ++currTick;
    if (sourceIdTimeoutTicks) --sourceIdTimeoutTicks;
    tick();
}

void ProjectileObject::onCollision(const Point3F& p, const Point3F& n, ScriptObject* hit) {
    if (!hit || !alive()) return;
    char fade[32];
    std::snprintf(fade, sizeof(fade), "%f", fadeValue);
    callDataBlock("onCollision", {std::to_string(ScriptEngine::instance().objectId(hit)), fade, formatPoint(p), formatPoint(n)});
}

void ProjectileObject::scriptOnExplode(const Point3F& p) {
    if (!alive()) return;
    char fade[32];
    std::snprintf(fade, sizeof(fade), "%f", fadeValue);
    callDataBlock("onExplode", {formatPoint(p), fade});
}

// Retail Projectile::packUpdate (no retail class ghosts as a bare Projectile).
uint32_t ProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        const Point3F p = getPosition(), v = getVelocity();
        w.writeCompressedPoint({p.x, p.y, p.z});
        w.writeCompressedPoint({v.x, v.y, v.z});
        const int s = ghostOf(connection, sourceKey);
        if (w.writeFlag(s != -1)) w.writeInt(s, 10);
        const int h = ghostOf(connection, vehicleKey);
        if (w.writeFlag(h != -1)) w.writeInt(h, 10);
    }
    return ret;
}

// ─── LinearProjectile ────────────────────────────────────────

void LinearProjectileObject::onAddServer() {
    // LinearProjectileData::onAdd.
    dryVelocity = std::max(data("dryVelocity", 5.0f), 0.1f);
    wetVelocity = data("wetVelocity", 5.0f);
    if (wetVelocity < 0.1f && wetVelocity != -1.0f) wetVelocity = 0.1f;
    lifetimeMS = tickRound(std::clamp(dataS32("lifetimeMS", 1000), (int)TickMs, (MaxLivingTicks - 1) * (int)TickMs));
    explodeOnDeath = dataBool("explodeOnDeath", false);
    explodeOnWaterImpact = dataBool("explodeOnWaterImpact", false);
    reflectOnWaterImpactAngle = std::clamp(data("reflectOnWaterImpactAngle", 0.0f), 0.0f, 90.0f);
    setPosition(initialPosition);
    createSegments();
}

void LinearProjectileObject::createSegments() {
    wetStart = pointInWater(initialPosition);
    const Point3F dir = dumbDownNormal(initialDirection, InitialDirectionBits);
    Point3F velocity = add(mul(dir, wetStart ? wetVelocity : dryVelocity), mul(excessDirDumb, (float)excessVel));
    const float lifetime = (float)lifetimeMS / 1000.0f;
    Segment& s0 = segments[0];
    s0.start = initialPosition;
    s0.end = add(initialPosition, mul(velocity, lifetime));
    s0.segmentVel = velocity;
    RayInfo info;
    if (!castRay(s0.start, s0.end, StaticCollisionMask, info)) {
        s0.msStart = 0;
        s0.msEnd = (uint32_t)lifetimeMS;
        s0.cutShort = false;
        s0.endNormal = mul(normalize(velocity), -1.0f);
    } else {
        s0.msStart = 0;
        s0.msEnd = (uint32_t)(lifetimeMS * info.t);
        s0.end = info.point;
        s0.cutShort = true;
        s0.endNormal = info.normal;
        s0.endTypeMask = info.objectType;
        s0.hitObj = keyOf(info.object);
    }
    numSegments = 1;
    RayInfo water;
    if (!wetStart && castRay(s0.start, s0.end, WaterObjectType, water)) {
        if (explodeOnWaterImpact) {
            const Point3F normVel = normalize(velocity);
            if (std::fabs(dot(normVel, water.normal)) >= std::cos((90.0f - reflectOnWaterImpactAngle) * DegToRad)) {
                hitWater = true;
                s0.msEnd = (uint32_t)(s0.msEnd * water.t);
                s0.end = water.point;
                s0.cutShort = true;
                s0.endNormal = water.normal;
                s0.endTypeMask = WaterObjectType;
                s0.hitObj = keyOf(water.object);
                deleteTick = s0.msEnd / TickMs + DeleteWaitTicks;
            } else {
                // Reflect off the surface.
                s0.msEnd = (uint32_t)(s0.msEnd * water.t);
                s0.end = water.point;
                s0.endNormal = water.normal;
                s0.cutShort = false;
                const Point3F n = water.normal;
                const Point3F reflected = sub(velocity, mul(n, 2.0f * dot(velocity, n)));
                Segment& s1 = segments[1];
                s1.msStart = s0.msEnd;
                s1.start = s0.end;
                s1.segmentVel = reflected;
                const Point3F newEnd = add(s1.start, mul(reflected, (float)(lifetimeMS - (int)s0.msEnd) / 1000.0f));
                if (!castRay(s1.start, newEnd, StaticCollisionMask, info)) {
                    s1.msEnd = (uint32_t)lifetimeMS;
                    s1.end = newEnd;
                    s1.cutShort = false;
                    s1.endNormal = mul(normalize(reflected), -1.0f);
                } else {
                    s1.msEnd = (uint32_t)(s1.msStart + (lifetimeMS - (int)s0.msEnd) * info.t);
                    s1.end = info.point;
                    s1.cutShort = true;
                    s1.endNormal = info.normal;
                    s1.endTypeMask = info.objectType;
                    s1.hitObj = keyOf(info.object);
                }
                deleteTick = s1.msEnd / TickMs + DeleteWaitTicks;
                numSegments = 2;
            }
        } else {
            // Continue through at wetVelocity.
            Segment& s1 = segments[1];
            s1.start = water.point;
            s0.end = water.point;
            s0.msEnd = (uint32_t)(s0.msEnd * water.t);
            s0.endNormal = water.normal;
            s0.cutShort = false;
            velocity = add(mul(dir, wetVelocity), mul(excessDirDumb, (float)excessVel));
            const Point3F newEnd = add(s1.start, mul(velocity, (float)(lifetimeMS - (int)s0.msEnd) / 1000.0f));
            s1.msStart = s0.msEnd;
            s1.hitObj.clear();
            if (!castRay(s1.start, newEnd, StaticCollisionMask, info)) {
                s1.msEnd = (uint32_t)lifetimeMS;
                s1.end = newEnd;
                s1.cutShort = false;
                s1.endNormal = mul(normalize(velocity), -1.0f);
            } else {
                s1.msEnd = (uint32_t)(s1.msStart + (lifetimeMS - (int)s0.msEnd) * info.t);
                s1.end = info.point;
                s1.cutShort = true;
                s1.endNormal = info.normal;
                s1.endTypeMask = info.objectType;
                s1.hitObj = keyOf(info.object);
            }
            s1.segmentVel = velocity;
            numSegments = 2;
            deleteTick = s1.msEnd / TickMs + DeleteWaitTicks;
        }
    } else {
        deleteTick = s0.msEnd / TickMs + DeleteWaitTicks;
    }
}

Point3F LinearProjectileObject::deriveExactPosition(uint32_t tick) const {
    const uint32_t ms = tick * TickMs;
    if (ms == 0) return segments[0].start;
    if (ms <= segments[0].msEnd) return add(segments[0].start, mul(segments[0].segmentVel, (float)ms / 1000.0f));
    if (numSegments > 1 && ms <= segments[1].msEnd)
        return add(segments[1].start, mul(segments[1].segmentVel, (float)(ms - segments[1].msStart) / 1000.0f));
    return segments[numSegments - 1].end;
}

Point3F LinearProjectileObject::deriveExactVelocity(uint32_t tick) const {
    const uint32_t ms = tick * TickMs;
    if (ms < segments[0].msEnd) return segments[0].segmentVel;
    if (numSegments > 1 && ms < segments[1].msEnd) return segments[1].segmentVel;
    return {0, 0, 0};
}

// csmDecalMask: terrain and interiors.
static constexpr uint32_t DecalMask = TerrainObjectType | InteriorObjectType;

void LinearProjectileObject::explode(const Point3F& p, const Point3F& n, bool dynamicObject) {
    if (hitWater && !explodeOnWaterImpact) {
        hidden = true;
        return;
    }
    if (hidden) return;
    hidden = true;
    explosionPosition = add(p, mul(n, 0.01f));
    explosionNormal = n;
    if (dynamicObject) setMaskBits(ExplosionMask);
    scriptOnExplode(explosionPosition);
}

void LinearProjectileObject::tick() {
    if (currTick >= deleteTick) {
        deleteSelf();
        return;
    }
    if (hidden) return;
    const Segment& last = segments[numSegments - 1];
    if ((currTick - 1) * TickMs >= last.msEnd) {
        if (last.cutShort && (last.endTypeMask & DecalMask)) endedWithDecal = true;
        if (last.cutShort || explodeOnDeath) {
            if (ScriptObject* so = findScript(last.hitObj)) onCollision(last.end, last.endNormal, so);
            if (!alive()) return;
            explode(last.end, last.endNormal, false);
        } else {
            // End of the line without an explosion: gone at once (a hidden
            // initial update means "exploded" to the client).
            hidden = true;
            deleteSelf();
        }
        return;
    }
    const Point3F oldPos = getPosition();
    const Point3F newPos = deriveExactPosition(currTick);
    RayInfo hit;
    if (castRay(oldPos, newPos, DynamicCollisionMask, hit, timeoutExempt())) {
        setPosition(hit.point);
        onCollision(hit.point, hit.normal, hit.object);
        if (!alive()) return;
        if (hit.objectType & DecalMask) endedWithDecal = true;
        explode(hit.point, hit.normal, true);
    } else {
        setPosition(newPos);
    }
}

// Retail LinearProjectile::packUpdate.
uint32_t LinearProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    auto writeExplosion = [&] {
        w.writeCompressedPoint({explosionPosition.x, explosionPosition.y, explosionPosition.z});
        w.writeNormalVector({explosionNormal.x, explosionNormal.y, explosionNormal.z}, InitialDirectionBits);
        w.writeFlag(endedWithDecal);
    };
    if (w.writeFlag(mask & InitialUpdateMask)) {
        if (!w.writeFlag(hidden)) {
            w.writeCompressedPoint({initialPosition.x, initialPosition.y, initialPosition.z});
            w.writeNormalVector({initialDirection.x, initialDirection.y, initialDirection.z}, InitialDirectionBits);
            w.writeRangedU32(std::min(currTick, (uint32_t)MaxLivingTicks), 0, MaxLivingTicks);
            const int s = source() ? ghostOf(connection, sourceKey) : -1;
            if (w.writeFlag(s != -1)) {
                w.writeRangedU32((uint32_t)s, 0, 1023);
                w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
                if (w.writeFlag(excessVel != 0)) {
                    w.writeRangedU32(std::min(excessVel, 255u), 0, 255);
                    w.writeNormalVector({excessDir.x, excessDir.y, excessDir.z}, ExcessVelDirBits);
                }
            }
            const int v = vehicle() ? ghostOf(connection, vehicleKey) : -1;
            if (w.writeFlag(v != -1)) w.writeRangedU32((uint32_t)v, 0, 1023);
        } else {
            writeExplosion();
        }
    } else {
        writeExplosion();
    }
    return ret;
}

// ─── GrenadeProjectile ───────────────────────────────────────

void GrenadeProjectileObject::onAddServer() {
    // GrenadeProjectileData::onAdd.
    const int armingDelayMS = tickRound(std::max(dataS32("armingDelayMS", 3000), 250));
    const float muzzleVelocity = std::max(data("muzzleVelocity", 75.0f), 0.1f);
    elasticity = data("grenadeElasticity", 0.999f);
    if (elasticity < 0.0f || elasticity > 0.999f) elasticity = elasticity < 0.0f ? 0.0f : 0.999f;
    friction = std::clamp(data("grenadeFriction", 0.3f), 0.0f, 1.0f);
    drag = std::max(data("drag", 0.0f), 0.0f);
    density = std::clamp(data("density", 1.0f), 0.1f, 10.0f);
    gravityMod = data("gravityMod", 1.0f);
    int lifetimeMS = dataS32("lifetimeMS", 20000);
    if (lifetimeMS == 0) lifetimeMS = 1000;
    // GrenadeProjectile::onAdd: the delete tick starts at the lifetime.
    armTick = (uint32_t)armingDelayMS / TickMs;
    deleteTick = (uint32_t)lifetimeMS / TickMs;
    velocity = add(mul(initialDirection, muzzleVelocity), mul(excessDir, (float)excessVel));
    // Start a little back along the flight.
    const Point3F back = len(velocity) > 1e-4f ? normalize(velocity) : velocity;
    setPosition(sub(initialPosition, mul(back, 0.15f)));
}

void GrenadeProjectileObject::computeNewState(Point3F& newPosition) {
    Point3F accel{0, 0, SimState::server().gravity * gravityMod * 0.4905f};
    if ((drag != 0.0f || density != 1.0f) && pointInWater(getPosition()) && drag > 0.0f)
        accel = sub(accel, mul(velocity, drag * 15.0f));
    velocity = add(velocity, mul(accel, TickSec));
    newPosition = add(getPosition(), mul(velocity, TickSec));
}

void GrenadeProjectileObject::explode(const Point3F& p, const Point3F& n) {
    if (hidden) return;
    explosionPosition = add(p, mul(n, 0.01f));
    explosionNormal = n;
    hidden = true;
    setMaskBits(ExplosionMask);
    scriptOnExplode(explosionPosition);
}

void GrenadeProjectileObject::tick() {
    if (deleteTick != -1 && (int64_t)currTick >= deleteTick) {
        deleteSelf();
        return;
    }
    if (hidden) return;
    Point3F oldPos = getPosition(), newPos;
    computeNewState(newPos);
    const auto exempt = timeoutExempt();
    float timeLeft = 1.0f;
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(oldPos.x - newPos.x) < 1e-4f && std::fabs(oldPos.y - newPos.y) < 1e-4f &&
            std::fabs(oldPos.z - newPos.z) < 1e-4f) {
            setPosition(newPos);
            break;
        }
        RayInfo hit;
        if (!castRay(oldPos, newPos, StaticCollisionMask | DynamicCollisionMask, hit, exempt)) {
            setPosition(newPos);
            break;
        }
        // Bouncing off something the client does not predict.
        if (!(hit.objectType & StaticCollisionMask)) setMaskBits(BounceMask);
        if (armTick < currTick) {
            setPosition(hit.point);
            velocity = {0, 0, 0};
            explode(hit.point, hit.normal);
            deleteTick = currTick + DeleteWaitTicks;
            break;
        }
        const Point3F n = hit.normal;
        velocity = sub(velocity, mul(n, 2.0f * dot(velocity, n)));
        const Point3F tangent = sub(velocity, mul(n, dot(velocity, n)));
        velocity = mul(sub(velocity, mul(tangent, friction)), elasticity);
        timeLeft *= 1.0f - hit.t;
        oldPos = add(hit.point, mul(n, 0.05f));
        newPos = add(oldPos, mul(velocity, timeLeft * TickSec));
    }
}

// Retail GrenadeProjectile::packUpdate (also Energy, Flare and Bomb).
uint32_t GrenadeProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    const Point3F p = getPosition();
    if (w.writeFlag(mask & InitialUpdateMask)) {
        writePoint(w, p);
        writePoint(w, velocity);
        w.writeRangedU32(std::min(currTick, 4095u), 0, 4095);
        w.writeFlag(false); // no splash between ticks before this update
        if (w.writeFlag((mask & ExplosionMask) && hidden)) {
            writePoint(w, p);
            writePoint(w, explosionNormal);
        }
        const int s = source() ? ghostOf(connection, sourceKey) : -1;
        if (w.writeFlag(s != -1)) {
            w.writeRangedU32((uint32_t)s, 0, 1024);
            w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
        }
        const int v = vehicle() ? ghostOf(connection, vehicleKey) : -1;
        if (w.writeFlag(v != -1)) w.writeRangedU32((uint32_t)v, 0, 1024);
    } else {
        if (w.writeFlag(mask & BounceMask)) {
            writePoint(w, p);
            writePoint(w, velocity);
        }
        if (w.writeFlag(mask & ExplosionMask)) {
            writePoint(w, p);
            writePoint(w, explosionNormal);
        }
    }
    return ret;
}

// ─── EnergyProjectile ────────────────────────────────────────

void EnergyProjectileObject::tick() {
    if (deleteTick != -1 && (int64_t)currTick >= deleteTick) {
        deleteSelf();
        return;
    }
    if (hidden) return;
    Point3F oldPos = getPosition(), newPos;
    computeNewState(newPos);
    const auto exempt = timeoutExempt();
    float timeLeft = 1.0f;
    for (int i = 0; i < 5; ++i) {
        RayInfo hit;
        if (!castRay(oldPos, newPos, StaticCollisionMask | DynamicCollisionMask, hit, exempt)) {
            setPosition(newPos);
            break;
        }
        const bool dynamicHit = (hit.objectType & DynamicCollisionMask) != 0;
        if (dynamicHit) setMaskBits(BounceMask);
        if (armTick < currTick || dynamicHit) {
            setPosition(add(hit.point, mul(hit.normal, 0.1f)));
            velocity = {0, 0, 0};
            explode(hit.point, hit.normal);
            onCollision(hit.point, hit.normal, hit.object);
            if (!alive()) return;
            // The bolt lingers for its blur.
            deleteTick = (int64_t)std::lround((float)currTick + data("blurLifetime", 0.5f) * 31.25f);
            break;
        }
        // Unarmed: a mirror bounce.
        velocity = sub(velocity, mul(hit.normal, 2.0f * dot(velocity, hit.normal)));
        timeLeft *= 1.0f - hit.t;
        oldPos = add(hit.point, mul(hit.normal, 0.05f));
        newPos = add(oldPos, mul(velocity, timeLeft * TickSec));
    }
}

// ─── BombProjectile ──────────────────────────────────────────

void BombProjectileObject::tick() {
    if (deleteTick != -1 && (int64_t)currTick >= deleteTick) {
        deleteSelf();
        return;
    }
    if (hidden) return;
    const Point3F oldPos = getPosition();
    Point3F newPos;
    computeNewState(newPos);
    RayInfo hit;
    if (castRay(oldPos, newPos, StaticCollisionMask | DynamicCollisionMask, hit, timeoutExempt())) {
        setPosition(hit.point);
        velocity = {0, 0, 0};
        explode(hit.point, hit.normal);
        deleteTick = currTick + DeleteWaitTicks;
    } else {
        setPosition(newPos);
    }
}

// ─── SeekerProjectile ────────────────────────────────────────

namespace {
struct SeekerData {
    int lifetimeMS;
    float muzzleVelocity, turningSpeed, proximityRadius, avoidSpeed, scanAhead, heightFail, avoidRadius;
    float flareDistance, flareAngle, maxVelocity, acceleration;
    uint32_t flechetteDelayMs;
    float cosTurn, cosAvoid, cosFlare;
    bool explodeOnWaterImpact;
};

// SeekerProjectileData::onAdd.
SeekerData seekerData(const ProjectileObject& p) {
    SeekerData d{};
    d.lifetimeMS = tickRound(std::clamp((int)p.dataFloat("lifetimeMS", 3000), 500, 30000));
    d.muzzleVelocity = std::max(p.dataFloat("muzzleVelocity", 100.0f), 0.1f);
    d.turningSpeed = std::clamp(p.dataFloat("turningSpeed", 180.0f), 0.0f, 2000.0f);
    d.proximityRadius = p.dataFloat("proximityRadius", 10.0f);
    d.avoidSpeed = std::clamp(p.dataFloat("terrainAvoidanceSpeed", 180.0f), 0.0f, 2000.0f);
    d.scanAhead = std::clamp(p.dataFloat("terrainScanAhead", 25.0f), 0.0f, 200.0f);
    d.heightFail = std::max(p.dataFloat("terrainHeightFail", 2.0f), 0.0f);
    d.avoidRadius = std::max(p.dataFloat("terrainAvoidanceRadius", 50.0f), 0.0f);
    d.flareDistance = std::max(p.dataFloat("flareDistance", 200.0f), 0.0f);
    d.flareAngle = std::max(p.dataFloat("flareAngle", 20.0f), 0.0f);
    d.maxVelocity = p.dataFloat("maxVelocity", 65.0f);
    if (d.maxVelocity < 0.1f) d.maxVelocity = d.muzzleVelocity + 1.0f;
    d.acceleration = p.dataFloat("acceleration", 10.0f);
    if (d.acceleration < 0.0f || d.acceleration > 30000.0f) d.acceleration = 0.0f;
    int flechette = (int)p.dataFloat("flechetteDelayMs", 300);
    if (flechette > 30000) flechette = 500;
    d.flechetteDelayMs = (uint32_t)flechette;
    d.cosTurn = std::cos(d.turningSpeed * TickSec * DegToRad);
    d.cosAvoid = std::cos(d.avoidSpeed * TickSec * DegToRad);
    d.cosFlare = std::cos(d.flareAngle * DegToRad);
    d.explodeOnWaterImpact = p.dataBool("explodeOnWaterImpact", false);
    return d;
}

GrenadeProjectileObject* flareOf(const std::string& key) {
    ScriptObject* object = findScript(key);
    return object && EngineClasses::isA(object->className, "FlareProjectile")
        ? dynamic_cast<GrenadeProjectileObject*>(object->engine.get()) : nullptr;
}
} // namespace

void SeekerProjectileObject::onAddServer() {
    const SeekerData d = seekerData(*this);
    velocity = mul(initialDirection, d.muzzleVelocity);
    // Only the inherited speed along the launch direction is kept.
    projectedExcessVel = (uint32_t)(std::fabs(dot(excessDir, initialDirection)) * (float)excessVel);
    velocity = add(velocity, mul(excessDir, (float)projectedExcessVel));
    lifetimeTicks = (uint32_t)d.lifetimeMS >> 5;
    deleteTick = lifetimeTicks + DeleteWaitTicks;
    setPosition(initialPosition);
}

void SeekerProjectileObject::setObjectTarget(ScriptObject* target) {
    if (!target || !dynamic_cast<GameBase*>(target->engine.get())) return;
    if (auto* flare = flareOf(targetKey)) --flare->lockCount;
    targetKey = originalTargetKey = keyOf(target);
    mode = ObjectTarget;
    if (auto* flare = flareOf(targetKey)) ++flare->lockCount;
}

void SeekerProjectileObject::setPositionTarget(const Point3F& p) {
    if (auto* flare = flareOf(targetKey)) --flare->lockCount;
    targetKey.clear();
    originalTargetKey.clear();
    targetPosition = p;
    mode = PositionTarget;
}

void SeekerProjectileObject::setNoTarget() {
    if (auto* flare = flareOf(targetKey)) --flare->lockCount;
    targetKey.clear();
    originalTargetKey.clear();
    mode = NoTarget;
}

int SeekerProjectileObject::targetObjectId() const {
    ScriptObject* target = findScript(targetKey);
    return target ? ScriptEngine::instance().objectId(target) : -1;
}

void SeekerProjectileObject::clearTarget() {
    if (auto* flare = flareOf(targetKey)) --flare->lockCount;
    targetKey.clear();
}

// The point the missile steers at; flares within flareDistance and
// flareAngle of the flight steal an object lock from its original target.
bool SeekerProjectileObject::getTarget(Point3F& out) {
    if (mode == NoTarget) return false;
    if (mode == PositionTarget) {
        out = targetPosition;
        return true;
    }
    if (!findScript(targetKey)) {
        if (!findScript(originalTargetKey)) return false;
        targetKey = originalTargetKey;
        if (auto* flare = flareOf(targetKey)) ++flare->lockCount;
    }
    const SeekerData d = seekerData(*this);
    if (targetKey == originalTargetKey && d.flareDistance > 0.0f && d.flareAngle > 0.0f) {
        ScriptObject* best = nullptr;
        float bestDist = 1e8f;
        const Point3F pos = getPosition();
        for (auto& [key, object] : ScriptEngine::instance().objects) {
            if (!object || !EngineClasses::isA(object->className, "FlareProjectile")) continue;
            auto* flare = dynamic_cast<GrenadeProjectileObject*>(object->engine.get());
            if (!flare || flare->lockCount != 0) continue;
            const Point3F delta = sub(flare->getPosition(), pos);
            const float dist = len(delta);
            if (dist > d.flareDistance || dist > bestDist) continue;
            if (dot(normalizeSafe(delta), normalizeSafe(velocity)) < d.cosFlare) continue;
            best = object;
            bestDist = dist;
        }
        if (best && keyOf(best) != targetKey) {
            clearTarget();
            targetKey = keyOf(best);
            if (auto* flare = flareOf(targetKey)) ++flare->lockCount;
            setMaskBits(TargetMask);
        }
    }
    out = worldBoxCenter(findScript(targetKey));
    return true;
}

// Turn at most turningSpeed toward the target (limited by the target's
// heat), then pitch up away from terrain ahead. Returns whether the target
// is ahead.
bool SeekerProjectileObject::steer(const Point3F& pos, const Point3F& vel, const Point3F& target, Point3F& out) {
    const SeekerData d = seekerData(*this);
    const Point3F toTarget = sub(target, pos);
    const Point3F dt = len(toTarget) < 1e-9f ? Point3F{0, 0, 1} : normalize(toTarget);
    const Point3F dv = len(vel) < 1e-9f ? Point3F{0, 0, 1} : normalize(vel);
    const float cosAngle = dot(dt, dv);
    const bool ahead = cosAngle >= 0.0f;
    if (cosAngle >= 0.99985f) {
        out = vel;
    } else {
        const Point3F axis = normalizeSafe(cross(dv, dt));
        float angle = std::acos(std::max(cosAngle, d.cosTurn));
        if (mode == ObjectTarget) {
            ScriptObject* object = findScript(targetKey);
            auto* shape = object ? dynamic_cast<ShapeBase*>(object->engine.get()) : nullptr;
            const float limit = (shape ? shape->heat : 1.0f) * d.turningSpeed;
            angle = std::min(angle, limit);
            if (angle == 0.0f) {
                out = vel;
                return ahead;
            }
        }
        out = rotate(vel, axis, angle);
    }
    if (d.scanAhead == 0.0f) return ahead;
    ScriptObject* object = mode == ObjectTarget ? findScript(targetKey) : nullptr;
    const uint32_t mask = (object ? typeMask(object) : 0u) | TerrainObjectType;
    RayInfo info;
    if (len(sub(pos, target)) < d.avoidRadius) {
        // Close in: only avoid terrain standing between the missile and the target.
        if (!castRay(pos, target, mask, info, {findScript(sourceKey)})) return ahead;
        if (!info.object || !EngineClasses::isA(info.object->className, "TerrainBlock")) return ahead;
    }
    const Point3F dir = normalizeSafe(out);
    if (!castRay(pos, add(pos, mul(dir, d.scanAhead)), TerrainObjectType, info) &&
        !castRay(pos, sub(pos, {0, 0, d.heightFail}), TerrainObjectType, info))
        return ahead;
    const Point3F axis = normalizeSafe(cross(dir, {0, 0, 1}), {0, 0, 0});
    if (len(axis) == 0.0f) return ahead;
    out = rotate(out, axis, std::acos(std::max(std::min(dir.z, 1.0f), d.cosAvoid)));
    return ahead;
}

bool SeekerProjectileObject::collide(const Point3F& a, const Point3F& b, bool dynamic, RayInfo& hit) {
    uint32_t mask = StaticCollisionMask | (dynamic ? DynamicCollisionMask : 0u);
    if (seekerData(*this).explodeOnWaterImpact) mask |= WaterObjectType;
    std::vector<ScriptObject*> exempt;
    if (source()) exempt.push_back(findScript(sourceKey));
    if (!castRay(a, b, mask, hit, exempt)) return false;
    RayInfo water;
    if (castRay(a, b, WaterObjectType, water)) hitWater = true;
    return true;
}

void SeekerProjectileObject::explode(const Point3F& p, const Point3F& n) {
    if (hidden) return;
    hidden = true;
    // A missile that reaches the flare it chased takes the flare with it.
    const std::string flare = flareOf(targetKey) ? targetKey : std::string();
    scriptOnExplode(add(p, mul(n, 0.01f)));
    if (!flare.empty() && flareOf(flare)) ScriptEngine::instance().deleteScriptObject(flare);
}

void SeekerProjectileObject::tick() {
    if (currTick >= deleteTick) {
        deleteSelf();
        return;
    }
    if (hidden) return;
    const SeekerData d = seekerData(*this);
    const Point3F pos = getPosition();
    if (currTick >= lifetimeTicks) {
        explode(pos, normalizeSafe(mul(velocity, -1.0f)));
        if (!alive()) return;
        clearTarget();
        deleteTick = currTick + DeleteWaitTicks;
        setMaskBits(ExplosionMask);
        return;
    }
    Point3F target;
    if (getTarget(target)) {
        Point3F steered;
        const bool ahead = steer(pos, velocity, target, steered);
        velocity = steered;
        if (!ahead && len(sub(pos, target)) < d.proximityRadius) {
            explode(pos, normalizeSafe(mul(velocity, -1.0f)));
            if (!alive()) return;
            clearTarget();
            deleteTick = currTick + DeleteWaitTicks;
            setMaskBits(ExplosionMask);
            return;
        }
    } else if (mode != NoTarget) {
        mode = NoTarget;
        setMaskBits(TargetMask);
    }
    const bool accelerating = currTick >= (d.flechetteDelayMs >> 5);
    if (accelerating && len(velocity) < d.maxVelocity) {
        const Point3F own = sub(velocity, mul(excessDir, (float)projectedExcessVel));
        velocity = add(velocity, mul(normalizeSafe(own, {0, 0, 0}), d.acceleration * TickSec));
    }
    const Point3F newPos = add(pos, mul(velocity, TickSec));
    RayInfo hit;
    if (collide(pos, newPos, true, hit)) {
        if (!hitWater && accelerating) {
            explode(hit.point, hit.normal);
            if (!alive()) return;
            setMaskBits(ExplosionMask);
        } else {
            hidden = true; // a dud: gone without an explosion
        }
        clearTarget();
        mode = NoTarget;
        deleteTick = currTick + DeleteWaitTicks;
        return;
    }
    setPosition(newPos);
    if (mode == ObjectTarget) setMaskBits(TargetMask);
}

// Retail SeekerProjectile::packUpdate.
uint32_t SeekerProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    const Point3F p = getPosition();
    Point3F targetPoint{0, 0, 0};
    auto writeTarget = [&] {
        const bool known = mode == PositionTarget || (mode == ObjectTarget && findScript(targetKey));
        if (!w.writeFlag(known)) return;
        const int g = mode == ObjectTarget ? ghostOf(connection, targetKey) : -1;
        if (w.writeFlag(g != -1)) {
            w.writeRangedU32((uint32_t)g, 0, 1024);
        } else {
            targetPoint = mode == PositionTarget ? targetPosition : worldBoxCenter(findScript(targetKey));
            writePoint(w, targetPoint);
        }
    };
    if (w.writeFlag(mask & InitialUpdateMask)) {
        writePoint(w, p);
        writePoint(w, velocity);
        writePoint(w, mul(excessDir, (float)projectedExcessVel));
        const int s = source() ? ghostOf(connection, sourceKey) : -1;
        if (w.writeFlag(s != -1)) {
            w.writeRangedU32((uint32_t)s, 0, 1024);
            w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
        }
        writeTarget();
        w.writeFlag(currTick < (seekerData(*this).flechetteDelayMs >> 5));
    } else if (w.writeFlag(mask & ExplosionMask)) {
        writePoint(w, p);
        writePoint(w, normalizeSafe(mul(velocity, -1.0f)));
    } else {
        writePoint(w, p);
        writePoint(w, velocity);
        writeTarget();
    }
    return ret;
}

// ─── SniperProjectile / TargetProjectile ─────────────────────

void BeamProjectileObject::cast(bool callCollision) {
    const float range = std::clamp(data("maxRifleRange", 1000.0f), 10.0f, 2000.0f);
    endPoint = add(initialPosition, mul(initialDirection, range));
    std::vector<ScriptObject*> exempt;
    if (source()) exempt.push_back(findScript(sourceKey));
    RayInfo hit;
    if (castRay(initialPosition, endPoint, StaticCollisionMask | DynamicCollisionMask | WaterObjectType, hit, exempt)) {
        if (callCollision && (hit.objectType & DamageableMask)) onCollision(hit.point, hit.normal, hit.object);
        if (hit.objectType == WaterObjectType) beamHitWater = true;
        endPoint = hit.point;
        truncated = true;
        hitKey = keyOf(hit.object);
    } else {
        truncated = false;
        hitKey.clear();
    }
}

void SniperProjectileObject::onAddServer() {
    cast(true);
    // The beam lingers for twice the fade time.
    lifetimeTicks = (int)std::lround(std::max(data("fadeTime", 1.0f), 0.25f) * 31.25f * 2.0f);
}

void SniperProjectileObject::tick() {
    if (--lifetimeTicks < 1) deleteSelf();
}

void TargetProjectileObject::onAddServer() { cast(false); }

// The beam follows the source's muzzle; clients hear about it every 8 ticks.
void TargetProjectileObject::tick() {
    ShapeBase* src = source();
    if (!src) return;
    float p[3], v[3];
    src->getMuzzlePoint((uint32_t)std::max(sourceSlot, 0), p);
    src->getMuzzleVector((uint32_t)std::max(sourceSlot, 0), v);
    initialPosition = {p[0], p[1], p[2]};
    initialDirection = {v[0], v[1], v[2]};
    cast(false);
    if ((currTick & 7) == 0) setMaskBits(BeamMask);
    setPosition(endPoint);
}

// Retail SniperProjectile / TargetProjectile::packUpdate.
uint32_t BeamProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    ShapeBase* src = source();
    const int s = src ? ghostOf(connection, sourceKey) : -1;
    auto writeSource = [&] {
        w.writeRangedU32((uint32_t)s, 0, 1024);
        w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
        w.writeFlag(controlledBy(*src, connection));
    };
    if (w.writeFlag(mask & InitialUpdateMask)) {
        if (sniper) w.writeFloat(std::clamp(energyPercentage, 0.0f, 1.0f), 7);
        writePoint(w, initialPosition);
        writePoint(w, endPoint);
        w.writeFlag(truncated);
        if (sniper) w.writeFlag(beamHitWater);
        if (w.writeFlag(s != -1)) writeSource();
    } else {
        if (w.writeFlag(s != -1)) writeSource();
        else writePoint(w, initialPosition);
        writePoint(w, endPoint);
        w.writeFlag(truncated);
    }
    return ret;
}

// ─── ShockLanceProjectile ────────────────────────────────────

void ShockLanceProjectileObject::onAddServer() {
    ScriptObject* target = findScript(Fields::string(script, "targetId", ""));
    if (target && dynamic_cast<ShapeBase*>(target->engine.get())) targetKey = keyOf(target);
    float zapDuration = data("zapDuration", 0.5f);
    if (zapDuration < 0.05f || zapDuration >= 30.0f) zapDuration = zapDuration < 0.05f ? 0.05f : 2.0f;
    float boltLength = data("boltLength", 2.0f);
    if (boltLength < 0.5f || boltLength >= 50.0f) boltLength = boltLength < 0.5f ? 0.5f : 5.0f;
    lifetimeTicks = (int)(zapDuration * 31.25f);
    start = initialPosition;
    end = add(start, mul(initialDirection, boltLength));
    RayInfo hit;
    auto* shape = EngineObjects::get<ShapeBase>(targetKey);
    if (castRay(start, end, StaticCollisionMask | DynamicCollisionMask, hit) && (hit.objectType & DamageableMask) &&
        shape && shape->damageState != ShapeBase::Destroyed) {
        end = hit.point;
        hitObject = true;
    }
    setPosition(end);
}

void ShockLanceProjectileObject::tick() {
    if (--lifetimeTicks < 1) deleteSelf();
}

// Retail ShockLanceProjectile::packUpdate.
uint32_t ShockLanceProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    const int t = EngineObjects::get<ShapeBase>(targetKey) ? ghostOf(connection, targetKey) : -1;
    if (w.writeFlag(t != -1)) w.writeRangedU32((uint32_t)t, 0, 1024);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        writePoint(w, start);
        writePoint(w, end);
        w.writeFlag(hitObject);
        const int s = source() ? ghostOf(connection, sourceKey) : -1;
        if (w.writeFlag(s != -1)) {
            w.writeRangedU32((uint32_t)s, 0, 1024);
            w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
        }
    }
    return ret;
}

// ─── ELFProjectile ───────────────────────────────────────────

ELFProjectileObject::~ELFProjectileObject() {
    // ELFProjectile::onRemove: let go of the target.
    if (!targetKey.empty() && added && ScriptEngine::exists() && ScriptEngine::instance().ts() && findScript(targetKey) &&
        findScript(dataKey))
        ScriptEngine::instance().ts()->callObjectMethod(
            dataKey, "unzapTarget",
            {VMValue(selfId), VMValue(idOf(targetKey)), VMValue(std::to_string(sourceObjectId))});
}

void ELFProjectileObject::unzap() {
    if (targetKey.empty()) return;
    const std::string old = targetKey;
    targetKey.clear();
    if (findScript(old))
        callDataBlock("unzapTarget", {std::to_string(idOf(old)), std::to_string(sourceObjectId)});
}

void ELFProjectileObject::zap(ScriptObject* target) {
    const std::string key = keyOf(target);
    if (key == targetKey) return;
    unzap();
    if (!alive()) return;
    setMaskBits(TargetMask);
    targetKey = key;
    callDataBlock("zapTarget", {std::to_string(idOf(key)), std::to_string(sourceObjectId)});
}

// ELFProjectile beam update: the beam from the source's muzzle holds the
// first damageable thing on it, else the best one inside the hit cone.
void ELFProjectileObject::update() {
    dataKey = dataBlock();
    ShapeBase* src = source();
    if (!src) return;
    const uint32_t slot = (uint32_t)std::max(sourceSlot, 0);
    float p[3], v[3];
    src->getMuzzlePoint(slot, p);
    src->getMuzzleVector(slot, v);
    initialPosition = {p[0], p[1], p[2] - 0.4f};
    initialDirection = {v[0], v[1], v[2]};
    const float range = std::max(data("beamRange", 10.0f), 2.0f);
    const float hitWidth = std::clamp(data("beamHitWidth", 20.0f), 0.0f, 90.0f);
    const Point3F start = initialPosition, end = add(start, mul(initialDirection, range));
    setPosition(start);
    const std::vector<ScriptObject*> exempt{findScript(sourceKey)};
    auto zappable = [](ScriptObject* o) {
        auto* shape = o ? dynamic_cast<ShapeBase*>(o->engine.get()) : nullptr;
        return shape && shape->damageState != ShapeBase::Destroyed;
    };
    RayInfo hit;
    if (castRay(start, end, StaticCollisionMask | DynamicCollisionMask, hit, exempt) && (hit.objectType & DamageableMask) &&
        zappable(hit.object)) {
        zap(hit.object);
        return;
    }
    // The cone around the beam.
    const float spread = range * std::sin(hitWidth * DegToRad);
    Point3F lo{std::min(start.x, end.x) - spread, std::min(start.y, end.y) - spread, std::min(start.z, end.z) - spread};
    Point3F hi{std::max(start.x, end.x) + spread, std::max(start.y, end.y) + spread, std::max(start.z, end.z) + spread};
    const float cosWidth = std::cos(hitWidth * DegToRad);
    for (ScriptObject* candidate : findObjects(lo, hi, DamageableMask)) {
        if (candidate == exempt[0]) continue;
        const Point3F center = worldBoxCenter(candidate);
        const Point3F delta = sub(center, start);
        const float dist = len(delta);
        if (dist > range || dist <= 0.0f) continue;
        std::vector<ScriptObject*> skip = exempt;
        skip.push_back(script);
        if (castRay(start, center, StaticCollisionMask | DynamicCollisionMask, hit, skip) && hit.object != candidate) continue;
        if (dot(mul(delta, 1.0f / dist), initialDirection) < cosWidth || !zappable(candidate)) continue;
        zap(candidate);
        return;
    }
    if (!targetKey.empty()) {
        setMaskBits(TargetMask);
        unzap();
    }
}

// Retail ELFProjectile::packUpdate: every update carries the link.
uint32_t ELFProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    if (w.writeFlag(source() && findScript(targetKey))) {
        const int s = ghostOf(connection, sourceKey), t = ghostOf(connection, targetKey);
        if (w.writeFlag(s != -1 && t != -1)) {
            w.writeRangedU32((uint32_t)s, 0, 1024);
            w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
            w.writeRangedU32((uint32_t)t, 0, 1024);
        }
    }
    return ret;
}

// ─── RepairProjectile ────────────────────────────────────────

void RepairProjectileObject::onAddServer() {
    const std::string field = Fields::string(script, "targetObject", "-1");
    ScriptObject* target = findScript(field);
    if (target && dynamic_cast<ShapeBase*>(target->engine.get())) targetKey = keyOf(target);
    else if (field != "-1") Console::instance().printf(LogLevel::Error, "Projectile::onAdd: mRepairingObjectId is invalid");
}

uint32_t RepairProjectileObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        const int s = source() ? ghostOf(connection, sourceKey) : -1;
        const int t = EngineObjects::get<ShapeBase>(targetKey) ? ghostOf(connection, targetKey) : -1;
        if (w.writeFlag(s != -1 && t != -1)) {
            w.writeRangedU32((uint32_t)s, 0, 1024);
            w.writeRangedU32((uint32_t)sourceSlot & 7u, 0, 7);
            w.writeRangedU32((uint32_t)t, 0, 1024);
        }
    }
    return ret;
}

// ─── Registration ────────────────────────────────────────────

void registerProjectileNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Projectile", [] { return std::make_shared<ProjectileObject>(); });
    EngineObjects::registerClass("LinearProjectile", [] { return std::make_shared<LinearProjectileObject>(); });
    EngineObjects::registerClass("TracerProjectile", [] { return std::make_shared<LinearProjectileObject>("TracerProjectile"); });
    EngineObjects::registerClass("LinearFlareProjectile",
                                 [] { return std::make_shared<LinearProjectileObject>("LinearFlareProjectile"); });
    EngineObjects::registerClass("GrenadeProjectile", [] { return std::make_shared<GrenadeProjectileObject>(); });
    EngineObjects::registerClass("FlareProjectile", [] { return std::make_shared<GrenadeProjectileObject>("FlareProjectile"); });
    EngineObjects::registerClass("EnergyProjectile", [] { return std::make_shared<EnergyProjectileObject>(); });
    EngineObjects::registerClass("BombProjectile", [] { return std::make_shared<BombProjectileObject>(); });
    EngineObjects::registerClass("SeekerProjectile", [] { return std::make_shared<SeekerProjectileObject>(); });
    EngineObjects::registerClass("SniperProjectile", [] { return std::make_shared<SniperProjectileObject>(); });
    EngineObjects::registerClass("TargetProjectile", [] { return std::make_shared<TargetProjectileObject>(); });
    EngineObjects::registerClass("ShockLanceProjectile", [] { return std::make_shared<ShockLanceProjectileObject>(); });
    EngineObjects::registerClass("ELFProjectile", [] { return std::make_shared<ELFProjectileObject>(); });
    EngineObjects::registerClass("RepairProjectile", [] { return std::make_shared<RepairProjectileObject>(); });

    using Args = std::vector<VMValue>;
    auto self = [](const Args& args) -> EngineObject* {
        return args.empty() ? nullptr : EngineObjects::find(args[0].toString());
    };
    // SeekerProjectile console methods (projSeeker.cc).
    ts.registerNative("SeekerProjectile::setObjectTarget", [self](const Args& args) -> VMValue {
        if (auto* s = dynamic_cast<SeekerProjectileObject*>(self(args)); s && args.size() > 1)
            s->setObjectTarget(findScript(args[1].toString()));
        return VMValue("");
    });
    ts.registerNative("SeekerProjectile::setPositionTarget", [self](const Args& args) -> VMValue {
        if (auto* s = dynamic_cast<SeekerProjectileObject*>(self(args)); s && args.size() > 1) {
            Point3F p{0, 0, 0};
            std::sscanf(args[1].toString().c_str(), "%f %f %f", &p.x, &p.y, &p.z);
            s->setPositionTarget(p);
        }
        return VMValue("");
    });
    ts.registerNative("SeekerProjectile::setNoTarget", [self](const Args& args) -> VMValue {
        if (auto* s = dynamic_cast<SeekerProjectileObject*>(self(args))) s->setNoTarget();
        return VMValue("");
    });
    ts.registerNative("SeekerProjectile::getTargetObject", [self](const Args& args) -> VMValue {
        auto* s = dynamic_cast<SeekerProjectileObject*>(self(args));
        return VMValue(s ? s->targetObjectId() : -1);
    });
    ts.registerNative("SniperProjectile::setEnergyPercentage", [self](const Args& args) -> VMValue {
        if (auto* s = dynamic_cast<SniperProjectileObject*>(self(args)); s && args.size() > 1)
            s->energyPercentage = args[1].toFloat();
        return VMValue("");
    });
    ts.registerNative("ELFProjectile::hasTarget", [self](const Args& args) -> VMValue {
        auto* e = dynamic_cast<ELFProjectileObject*>(self(args));
        return VMValue(e && findScript(e->targetKey) ? 1 : 0);
    });
    // TargetProjectile::getTargetPoint: "x y z hitId", or "0 0 0 -1".
    ts.registerNative("TargetProjectile::getTargetPoint", [self](const Args& args) -> VMValue {
        auto* t = dynamic_cast<TargetProjectileObject*>(self(args));
        if (!t || !t->truncated) return VMValue("0 0 0 -1");
        ScriptObject* hit = findScript(t->hitKey);
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f %d", t->endPoint.x, t->endPoint.y, t->endPoint.z,
                      hit ? ScriptEngine::instance().objectId(hit) : -1);
        return VMValue(buffer);
    });
}
