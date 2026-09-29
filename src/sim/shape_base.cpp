#include "sim/shape_base.h"
#include "sim/game_connection.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <strings.h>

static const char* sDamageStateName[] = {"Enabled", "Disabled", "Destroyed"};

float ShapeBase::getEnergyValue() const {
    const float max = maxEnergy();
    return max > 0.0f ? energy / max : 0.0f;
}

void ShapeBase::setEnergyLevel(float level) {
    if (damageState == Enabled) energy = std::clamp(level, 0.0f, std::max(0.0f, maxEnergy()));
}

void ShapeBase::setDamageLevel(float level) {
    if (invincible() || level == damage) return;
    damage = std::clamp(level, 0.0f, maxDamage());
    callDataBlock("onDamage");
}

void ShapeBase::applyRepair(float amount) {
    if (amount > 0 && (repairReserve += amount) > damage) repairReserve = damage;
}

float ShapeBase::getDamageValue() const {
    const float max = maxDamage();
    return max > 0.0f ? damage / max : 0.0f;
}

const char* ShapeBase::damageStateName() const { return sDamageStateName[damageState]; }

void ShapeBase::setDamageState(DamageState state) {
    if (damageState == state) return;
    const char* callback = nullptr;
    const std::string lastState = damageStateName();
    switch (state) {
        case Destroyed:
            if (damageState == Enabled) setDamageState(Disabled);
            callback = "onDestroyed";
            break;
        case Disabled:
            if (damageState == Enabled) callback = "onDisabled";
            break;
        case Enabled:
            callback = "onEnabled";
            break;
    }
    damageState = state;
    if (damageState != Enabled) {
        repairReserve = 0;
        energy = 0;
    }
    if (callback) callDataBlock(callback, {lastState});
}

bool ShapeBase::setDamageState(const std::string& name) {
    for (int i = 0; i < 3; ++i)
        if (strcasecmp(name.c_str(), sDamageStateName[i]) == 0) {
            setDamageState(DamageState(i));
            return true;
        }
    return false;
}

void ShapeBase::processMove(const ClientMoveIn* move) {
    processShapeTick();
    // Script on trigger state changes: %data.onTrigger(%obj, %trigger, %state).
    if (move)
        for (int i = 0; i < 6; ++i)
            if (move->trigger[i] != trigger[i]) {
                trigger[i] = move->trigger[i];
                callDataBlock("onTrigger", {std::to_string(i), trigger[i] ? "1" : "0"});
            }
}

void ShapeBase::processShapeTick() {
    // Energy management
    if (damageState == Enabled && !dataBool("inheritEnergyFromMount", false))
        energy = std::clamp(energy + rechargeRate, 0.0f, std::max(0.0f, maxEnergy()));
    // Repair management
    if (!invincible()) {
        const float store = damage;
        damage = std::clamp(damage - repairRate, 0.0f, maxDamage());
        if (repairReserve > damage) repairReserve = damage;
        if (repairReserve > 0.0f) {
            const float rate = std::min(dataFloat("repairRate", 0.0033f), repairReserve);
            damage -= rate;
            repairReserve -= rate;
        }
        if (store != damage) callDataBlock("onDamage");
    }
}

// Retail ShapeBase::packUpdate, in the order the client reads it: damage,
// sounds, threads, images, cloak/shield/invincibility, mount.
uint32_t ShapeBase::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    // No sound plays, no script thread runs and no image is mounted yet.
    if (mask & InitialUpdateMask) mask &= ~(SoundMask | ThreadMask | ImageMask);
    if (!w.writeFlag(mask & (DamageMask | SoundMask | ThreadMask | ImageMask | CloakMask | MountedMask |
                             InvincibleMask | ShieldMask)))
        return ret;
    if (w.writeFlag(mask & DamageMask)) {
        const float max = maxDamage();
        w.writeFloat(std::clamp(max > 0.0f ? damage / max : 0.0f, 0.0f, 1.0f), 6);
        w.writeInt((int32_t)damageState, 2);
        w.writeFlag(false); // blowApart
        w.writeNormalVector({0, 0, 1}, 8); // damageDir
    }
    if (w.writeFlag(mask & SoundMask))
        for (int i = 0; i < 4; ++i) w.writeFlag(false);
    if (w.writeFlag(mask & ThreadMask))
        for (int i = 0; i < 4; ++i) w.writeFlag(false);
    if (w.writeFlag(mask & ImageMask))
        for (int i = 0; i < 8; ++i) w.writeFlag(false);
    if (w.writeFlag(mask & (ShieldMask | CloakMask | InvincibleMask))) {
        if (w.writeFlag(mask & CloakMask)) {
            w.writeFlag(cloaked);
            w.writeFlag(!controllingClient.empty());
            w.writeFlag(false); // not fading
            w.writeFlag(true);  // mFadeVal == 1
        }
        if (w.writeFlag(mask & ShieldMask)) {
            w.writeFlag(false);
            w.writeNormalVector({0, 0, 1}, 8);
            w.writeFloat(getEnergyValue(), 5);
        }
        if (w.writeFlag(mask & InvincibleMask)) {
            w.writeF32(invincibleTime);
            w.writeF32(invincibleSpeed);
        }
    }
    // Not mounted: an unmount unless this is the initial update.
    if (w.writeFlag((mask & MountedMask) && !(mask & InitialUpdateMask))) w.writeFlag(false);
    return ret;
}

bool ShapeBase::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = GameBase::writePacketData(connection, w);
    w.writeF32(getEnergyLevel());
    w.writeF32(rechargeRate);
    return ret;
}

void registerShapeBaseNatives(TorqueScript& ts) {
    EngineObjects::registerClass("ShapeBase", [] { return std::make_shared<ShapeBase>(); });
    using Args = std::vector<VMValue>;
    auto self = [](const Args& args) -> ShapeBase* {
        return args.empty() ? nullptr : EngineObjects::get<ShapeBase>(args[0].toString());
    };
    auto arg = [](const Args& args, size_t i) { return i < args.size() ? args[i] : VMValue(""); };
    auto method = [&ts](const char* name, std::function<VMValue(ShapeBase&, const Args&)> body) {
        ts.registerNative(std::string("ShapeBase::") + name, [name, body](const Args& args) -> VMValue {
            ShapeBase* shape = args.empty() ? nullptr : EngineObjects::get<ShapeBase>(args[0].toString());
            if (!shape) {
                Console::instance().printf(LogLevel::Warn, "ShapeBase::%s: '%s' is not a ShapeBase",
                                           name, args.empty() ? "" : args[0].toString().c_str());
                return VMValue("");
            }
            return body(*shape, args);
        });
    };
    (void)self;
    // ShapeBase::getControllingClient: the connection's id, 0 when none.
    method("getControllingClient", [](ShapeBase& s, const Args&) {
        ScriptObject* client = s.controllingClient.empty() ? nullptr
            : ScriptEngine::instance().findObject(s.controllingClient.c_str());
        return VMValue(client ? ScriptEngine::instance().objectId(client) : 0);
    });
    method("setEnergyLevel", [arg](ShapeBase& s, const Args& a) { s.setEnergyLevel(arg(a, 1).toFloat()); return VMValue(""); });
    method("getEnergyLevel", [](ShapeBase& s, const Args&) { return VMValue(s.getEnergyLevel()); });
    method("getEnergyPercent", [](ShapeBase& s, const Args&) { return VMValue(s.getEnergyValue()); });
    method("setDamageLevel", [arg](ShapeBase& s, const Args& a) { s.setDamageLevel(arg(a, 1).toFloat()); return VMValue(""); });
    method("getDamageLevel", [](ShapeBase& s, const Args&) { return VMValue(s.damage); });
    method("getDamagePercent", [](ShapeBase& s, const Args&) { return VMValue(s.getDamageValue()); });
    method("setDamageState", [arg](ShapeBase& s, const Args& a) { return VMValue(s.setDamageState(arg(a, 1).toString()) ? 1 : 0); });
    method("getDamageState", [](ShapeBase& s, const Args&) { return VMValue(s.damageStateName()); });
    method("isDestroyed", [](ShapeBase& s, const Args&) { return VMValue(s.damageState == ShapeBase::Destroyed ? 1 : 0); });
    method("isDisabled", [](ShapeBase& s, const Args&) { return VMValue(s.damageState != ShapeBase::Enabled ? 1 : 0); });
    method("isEnabled", [](ShapeBase& s, const Args&) { return VMValue(s.damageState == ShapeBase::Enabled ? 1 : 0); });
    method("applyDamage", [arg](ShapeBase& s, const Args& a) { s.applyDamage(arg(a, 1).toFloat()); return VMValue(""); });
    method("applyRepair", [arg](ShapeBase& s, const Args& a) { s.applyRepair(arg(a, 1).toFloat()); return VMValue(""); });
    method("setRepairRate", [arg](ShapeBase& s, const Args& a) { s.repairRate = std::max(0.0f, arg(a, 1).toFloat()); return VMValue(""); });
    method("getRepairRate", [](ShapeBase& s, const Args&) { return VMValue(s.repairRate); });
    method("setRechargeRate", [arg](ShapeBase& s, const Args& a) { s.rechargeRate = arg(a, 1).toFloat(); return VMValue(""); });
    method("getRechargeRate", [](ShapeBase& s, const Args&) { return VMValue(s.rechargeRate); });
    method("setDamageFlash", [arg](ShapeBase& s, const Args& a) { s.damageFlash = std::clamp(arg(a, 1).toFloat(), 0.0f, 1.0f); return VMValue(""); });
    method("getDamageFlash", [](ShapeBase& s, const Args&) { return VMValue(s.damageFlash); });
    method("setWhiteOut", [arg](ShapeBase& s, const Args& a) { s.whiteOut = std::clamp(arg(a, 1).toFloat(), 0.0f, 1.5f); return VMValue(""); });
    method("getWhiteOut", [](ShapeBase& s, const Args&) { return VMValue(s.whiteOut); });
    method("setInvincibleMode", [arg](ShapeBase& s, const Args& a) {
        s.invincibleTime = arg(a, 1).toFloat();
        s.invincibleSpeed = arg(a, 2).toFloat();
        return VMValue("");
    });
    method("setCloaked", [arg](ShapeBase& s, const Args& a) { s.cloaked = arg(a, 1).toBool(); return VMValue(""); });
    method("isCloaked", [](ShapeBase& s, const Args&) { return VMValue(s.cloaked ? 1 : 0); });
    method("setPassiveJammed", [arg](ShapeBase& s, const Args& a) { s.passiveJammed = arg(a, 1).toBool(); return VMValue(""); });
    method("isPassiveJammed", [](ShapeBase& s, const Args&) { return VMValue(s.passiveJammed ? 1 : 0); });
    method("setHeat", [arg](ShapeBase& s, const Args& a) {
        const float heat = arg(a, 1).toFloat();
        if (heat < 0.0f || heat > 1.0f) {
            Console::instance().printf(LogLevel::Error, "Heat must be in the range [0, 1]");
            return VMValue("");
        }
        s.heat = heat;
        return VMValue("");
    });
    method("getHeat", [](ShapeBase& s, const Args&) { return VMValue(s.heat); });
    method("hide", [arg](ShapeBase& s, const Args& a) { s.hidden = arg(a, 1).toBool(); return VMValue(""); });
    method("isHidden", [](ShapeBase& s, const Args&) { return VMValue(s.hidden ? 1 : 0); });
    method("getCameraFov", [](ShapeBase& s, const Args&) { return VMValue(s.cameraFov); });
    method("setCameraFov", [arg](ShapeBase& s, const Args& a) {
        s.cameraFov = std::clamp(arg(a, 1).toFloat(), s.dataFloat("cameraMinFov", 5.0f), s.dataFloat("cameraMaxFov", 120.0f));
        return VMValue("");
    });
}
