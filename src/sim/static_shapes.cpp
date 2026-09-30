#include "sim/static_shapes.h"
#include "sim/torque_math.h"
#include "sim/game_connection.h"
#include "sim/nav_graph.h"
#include "sim/sim_state.h"
#include "script/torquescript.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "script/script_engine.h"
#include "script/torquescript.h"

static bool controlledBy(const GameBase& object, GameConnection& connection) {
    return !object.controllingClient.empty() && connection.script &&
           object.controllingClient == ScriptEngine::instance().objectKey(connection.script);
}

uint32_t StaticShapeObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & PositionMask)) {
        writeAffineTransform(w);
        writeScale(w);
    }
    w.writeFlag(powered);
    return ret;
}

// Retail Turret::packUpdate: capacitor energy, the control shortcut, aim.
uint32_t TurretObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = StaticShapeObject::packUpdate(connection, mask, w);
    w.writeFlag(false); // capacitor energy unchanged
    if (w.writeFlag(controlledBy(*this, connection) && !(mask & InitialUpdateMask))) return ret;
    w.writeFlag(false); // aim unchanged
    return ret;
}

void ItemObject::readFields() {
    ShapeBase::readFields();
    rotate = Fields::boolean(script, "rotate", false);
    isStatic = Fields::boolean(script, "static", false);
    collideable = Fields::boolean(script, "collideable", false);
}

uint32_t ItemObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        w.writeFlag(rotate);
        w.writeFlag(isStatic);
        w.writeFlag(collideable);
        if (w.writeFlag(scale[0] != 1 || scale[1] != 1 || scale[2] != 1)) writeScale(w);
    }
    w.writeFlag(false); // no thrower collision object
    if (w.writeFlag((mask & RotationMask) && !rotate)) {
        // Assumes rotation about the z axis.
        const TorqueMath::AngAxis aa = TorqueMath::angAxis(TorqueMath::quat(transform));
        w.writeFlag(aa.z < 0);
        w.writeF32(aa.angle);
    }
    if (w.writeFlag(mask & PositionMask)) {
        w.writePoint({transform[3], transform[7], transform[11]});
        if (!w.writeFlag(atRest)) w.writePoint({velocity[0], velocity[1], velocity[2]});
        w.writeFlag(!(mask & NoWarpMask));
    }
    return ret;
}

void ItemObject::setVelocity(const float v[3]) {
    for (int i = 0; i < 3; ++i) velocity[i] = v[i];
    setMaskBits(PositionMask);
    atRest = false;
    atRestCounter = 0;
}

// Item::processTick, server side.
void ItemObject::processMove(const ClientMoveIn* move) {
    ShapeBase::processMove(move);
    if (!collisionObject.empty() && --collisionTimeout <= 0) collisionObject.clear();
    const bool sticky = dataBool("sticky", false);
    if (atRest && !isStatic && !sticky && ++atRestCounter > 64) {
        atRest = false;
        atRestCounter = 0;
    }
    if (!isStatic && !atRest && !hidden) {
        updateVelocity(0.032f);
        updatePos(0.032f);
    }
}

void ItemObject::updateVelocity(float dt) {
    const float gravityMod = dataFloat("gravityMod", 1.0f);
    velocity[2] += SimState::server().gravity * gravityMod * dt;
    const float maxVelocity = dataFloat("maxVelocity", -1.0f);
    const float len = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    if (maxVelocity > 0 && len > maxVelocity * 1.05f) {
        const float k = (1.0f - maxVelocity / len) * 0.1f;
        for (float& v : velocity) v -= v * k;
    }
    // Container buoyancy & drag
    velocity[2] -= buoyancy * (SimState::server().gravity * gravityMod * this->gravityMod) * dt;
    for (float& v : velocity) v -= v * drag * dt;
}

// Item::updatePos. The engine casts from the box top centre (start) to the
// box bottom centre (end) for contact, then sweeps the box against the
// working set; APPROXIMATION: the sweep is a ray through the box centre.
void ItemObject::updatePos(float dt) {
    float pos[3] = {transform[3], transform[7], transform[11]};
    float lo[3], hi[3];
    worldBox(lo, hi);
    const float top = hi[2] - pos[2], bottom = lo[2] - pos[2], mid = (top + bottom) * 0.5f;
    const float cx = (lo[0] + hi[0]) * 0.5f - pos[0], cy = (lo[1] + hi[1]) * 0.5f - pos[1];
    const float friction = dataFloat("friction", 0.0f), elasticity = dataFloat("elasticity", 0.0f);
    const bool sticky = dataBool("sticky", false);
    bool contact = false, stickyNotify = false;
    auto respond = [&](const Nav::RayHit& hit) {
        float bd = -(velocity[0] * hit.normal.x + velocity[1] * hit.normal.y + velocity[2] * hit.normal.z);
        if (bd < 0) return false;
        if (sticky) {
            velocity[0] = velocity[1] = velocity[2] = 0;
            atRest = true;
            atRestCounter = 0;
            stickyNotify = true;
            stickyPos[0] = hit.point.x; stickyPos[1] = hit.point.y; stickyPos[2] = hit.point.z;
            stickyNormal[0] = hit.normal.x; stickyNormal[1] = hit.normal.y; stickyNormal[2] = hit.normal.z;
            return true;
        }
        const float n[3] = {hit.normal.x, hit.normal.y, hit.normal.z};
        float fv[3] = {velocity[0] + n[0] * bd, velocity[1] + n[1] * bd, velocity[2] + n[2] * bd};
        const float fvl = std::sqrt(fv[0] * fv[0] + fv[1] * fv[1] + fv[2] * fv[2]);
        if (fvl > 0) {
            const float ff = bd * friction;
            if (ff < fvl) for (float& f : fv) f *= ff / fvl;
        }
        bd *= 1 + elasticity;
        for (int i = 0; i < 3; ++i) velocity[i] += n[i] * (bd + 0.002f) - fv[i];
        contact = true;
        return false;
    };
    Nav::RayHit hit;
    const float end0[3] = {pos[0] + velocity[0] * dt, pos[1] + velocity[1] * dt, pos[2] + velocity[2] * dt};
    const bool stuck = Nav::castRay({pos[0] + cx, pos[1] + cy, pos[2] + top},
                                    {end0[0] + cx, end0[1] + cy, end0[2] + bottom}, 0xFFFFFFFFu, hit) &&
                       respond(hit);
    if (!stuck) {
        float time = dt;
        int count = 0;
        for (; count < 3; ++count) {
            const float end[3] = {pos[0] + velocity[0] * time, pos[1] + velocity[1] * time, pos[2] + velocity[2] * time};
            if (!Nav::castRay({pos[0] + cx, pos[1] + cy, pos[2] + mid}, {end[0] + cx, end[1] + cy, end[2] + mid},
                              0xFFFFFFFFu, hit)) {
                for (int i = 0; i < 3; ++i) pos[i] = end[i];
                break;
            }
            // To the collision point, less a margin.
            const float dx = hit.point.x - (pos[0] + cx), dy = hit.point.y - (pos[1] + cy), dz = hit.point.z - (pos[2] + mid);
            const float travel = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]) * time;
            const float t = travel > 0 ? std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) / travel - 0.01f) : 0.0f;
            for (int i = 0; i < 3; ++i) pos[i] += velocity[i] * time * t;
            time -= time * t;
            if (respond(hit)) break;
        }
        if (count == 3) velocity[0] = velocity[1] = velocity[2] = 0;
    }
    transform[3] = pos[0];
    transform[7] = pos[1];
    transform[11] = pos[2];
    updateContainer();
    if (contact) {
        const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
        if (speed < 0.15f) {
            velocity[0] = velocity[1] = velocity[2] = 0;
            atRest = true;
            atRestCounter = 0;
        }
        // Only static geometry is hit here: the client hears of the final
        // rest position.
        if (atRest) setMaskBits(PositionMask);
    }
    notifyCollision();
    if (stickyNotify) callDataBlock("onStickyCollision");
}

uint32_t BeaconObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = StaticShapeObject::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & BeaconMask)) w.writeInt(beaconType, 2);
    return ret;
}

void registerStaticShapeNatives(TorqueScript& ts) {
    EngineObjects::registerClass("StaticShape", [] { return std::make_shared<StaticShapeObject>(); });
    EngineObjects::registerClass("ScopeAlwaysShape",
                                 [] { return std::make_shared<StaticShapeObject>("ScopeAlwaysShape", true); });
    EngineObjects::registerClass("Turret", [] { return std::make_shared<TurretObject>(); });
    EngineObjects::registerClass("BeaconObject", [] { return std::make_shared<BeaconObject>(); });
    static const char* const beaconTypes[] = {"enemy", "friend", "vehicle"};
    ts.registerNative("BeaconObject::setBeaconType", [](const std::vector<VMValue>& args) -> VMValue {
        auto* beacon = args.empty() ? nullptr : EngineObjects::get<BeaconObject>(args[0].toString());
        const std::string type = args.size() > 1 ? args[1].toString() : std::string();
        for (int i = 0; i < 3; ++i)
            if (strcasecmp(type.c_str(), beaconTypes[i]) == 0) {
                if (beacon) {
                    beacon->beaconType = i;
                    beacon->setMaskBits(BeaconObject::BeaconMask);
                }
                return VMValue("");
            }
        Console::instance().printf(LogLevel::Error, "BeaconObject::cGetBeaconType: invalid beacon type [%s]", type.c_str());
        return VMValue("");
    });
    ts.registerNative("BeaconObject::getBeaconType", [](const std::vector<VMValue>& args) -> VMValue {
        auto* beacon = args.empty() ? nullptr : EngineObjects::get<BeaconObject>(args[0].toString());
        return VMValue(beacon ? beaconTypes[std::clamp(beacon->beaconType, 0, 2)] : "");
    });
    EngineObjects::registerClass("Item", [] { return std::make_shared<ItemObject>(); });
    using Args = std::vector<VMValue>;
    auto item = [](const Args& args) -> ItemObject* {
        return args.empty() ? nullptr : EngineObjects::get<ItemObject>(args[0].toString());
    };
    auto vec = [](const VMValue& v, float out[3]) {
        out[0] = out[1] = out[2] = 0;
        std::sscanf(v.toString().c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
    };
    auto fmt = [](const float v[3]) {
        char b[100];
        std::snprintf(b, sizeof(b), "%g %g %g", v[0], v[1], v[2]);
        return VMValue(b);
    };
    // Item::applyImpulse: items ignore angular velocity; the new velocity
    // replaces the old (impulse / mass).
    ts.registerNative("Item::applyImpulse", [item, vec](const Args& args) -> VMValue {
        auto* i = item(args);
        if (!i || args.size() < 3) return VMValue("");
        float v[3];
        vec(args[2], v);
        const float mass = i->dataFloat("mass", 1.0f);
        if (mass > 0) for (float& c : v) c /= mass;
        i->setVelocity(v);
        return VMValue("");
    });
    ts.registerNative("Item::setVelocity", [item, vec](const Args& args) -> VMValue {
        auto* i = item(args);
        if (!i || args.size() < 2) return VMValue(0);
        float v[3];
        vec(args[1], v);
        i->setVelocity(v);
        return VMValue(1);
    });
    ts.registerNative("Item::getVelocity", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float zero[3] = {0, 0, 0};
        return fmt(i ? i->velocity : zero);
    });
    ts.registerNative("Item::isStatic", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        return VMValue(i && i->isStatic ? 1 : 0);
    });
    ts.registerNative("Item::isRotating", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        return VMValue(i && i->rotate ? 1 : 0);
    });
    ts.registerNative("Item::setCollisionTimeout", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        ScriptObject* obj = args.size() > 1 ? ScriptEngine::instance().findObject(args[1].toString().c_str()) : nullptr;
        if (!i || !obj || !EngineObjects::get<ShapeBase>(args[1].toString())) return VMValue(0);
        i->collisionObject = std::to_string(ScriptEngine::instance().objectId(obj));
        i->collisionTimeout = 15;
        i->setMaskBits(ItemObject::ThrowSrcMask);
        return VMValue(1);
    });
    ts.registerNative("Item::getLastStickyPos", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float zero[3] = {0, 0, 0};
        return fmt(i ? i->stickyPos : zero);
    });
    ts.registerNative("Item::getLastStickyNormal", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float up[3] = {0, 0, 1};
        return fmt(i ? i->stickyNormal : up);
    });
    ts.registerNative("StaticShape::setPoweredState", [](const Args& args) -> VMValue {
        if (auto* s = args.empty() ? nullptr : EngineObjects::get<StaticShapeObject>(args[0].toString()))
            if (args.size() > 1) s->powered = args[1].toBool();
        return VMValue("");
    });
    ts.registerNative("StaticShape::getPoweredState", [](const Args& args) -> VMValue {
        auto* s = args.empty() ? nullptr : EngineObjects::get<StaticShapeObject>(args[0].toString());
        return VMValue(s && s->powered ? 1 : 0);
    });
}
