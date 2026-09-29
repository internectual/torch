#include "sim/static_shapes.h"
#include "sim/torque_math.h"
#include "sim/game_connection.h"
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

void registerStaticShapeNatives(TorqueScript& ts) {
    EngineObjects::registerClass("StaticShape", [] { return std::make_shared<StaticShapeObject>(); });
    EngineObjects::registerClass("ScopeAlwaysShape",
                                 [] { return std::make_shared<StaticShapeObject>("ScopeAlwaysShape", true); });
    EngineObjects::registerClass("Turret", [] { return std::make_shared<TurretObject>(); });
    EngineObjects::registerClass("Item", [] { return std::make_shared<ItemObject>(); });
    using Args = std::vector<VMValue>;
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
