#include "sim/player.h"
#include "sim/datablock_pack.h"
#include "sim/game_connection.h"
#include "sim/torque_math.h"
#include "sim/sim_state.h"
#include "net/v12_bitstream.h"
#include "net/v12_datablocks.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <cmath>
#include <cstdio>

namespace {
constexpr float TwoPi = 6.28318530717958647692f;

uint32_t dataBlockId(const std::string& handle) {
    ScriptObject* object = ScriptEngine::instance().findObject(handle.c_str());
    if (!object || !object->internals.count("__datablockKey")) return 0;
    const int id = ScriptEngine::instance().objectId(object);
    return id >= (int)DataBlockPack::ObjectIdFirst && id <= (int)DataBlockPack::ObjectIdLast ? (uint32_t)id : 0;
}

// Move::unclamp of a network move.
PlayerPrediction::Move unclamp(const ClientMoveIn& m) {
    bool triggers[6];
    for (int i = 0; i < 6; ++i) triggers[i] = m.trigger[i];
    return PlayerPrediction::unclampMove(m.x, m.y, m.z, (uint16_t)m.yaw, (uint16_t)m.pitch, (uint16_t)m.roll,
                                         m.freeLook, triggers);
}
} // namespace

ServerCollision& serverCollision() {
    static ServerCollision collision;
    return collision;
}

PlayerObject::PlayerObject() { ghostable = true; }

void PlayerObject::readFields() {
    ShapeBase::readFields();
    setTransform(transform);
}

// The PlayerData as the client decodes it (packData, then the reader).
const PlayerPrediction::Data* PlayerObject::physics() {
    const std::string block = dataBlock();
    if (block.empty()) return nullptr;
    if (physicsValid && block == physicsBlock) return &physicsData;
    ScriptObject* data = ScriptEngine::instance().findObject(block.c_str());
    const DataBlockPack::PackFn* pack = data ? DataBlockPack::find(data->className) : nullptr;
    const int classIndex = data ? DataBlockPack::classIndex(data->className) : -1;
    if (!pack || classIndex < 0) return nullptr;
    TorqueBitWriter w;
    DataBlockPack::Context context(data, w, dataBlockId);
    (*pack)(context);
    static const uint8_t none = 0;
    const auto& bytes = w.data();
    V12BitStream stream(bytes.empty() ? &none : bytes.data(), bytes.size());
    V12::DecodedDataBlock decoded;
    if (!V12::readDataBlockPayload(stream, (size_t)classIndex + 128, &decoded) || !decoded.isPlayerData) return nullptr;
    physicsData = decoded.playerPhysics;
    physicsBlock = block;
    physicsValid = true;
    return &physicsData;
}

// Player::setPosition: the rotation about z only.
void PlayerObject::syncTransform() {
    const float cz = std::cos(state.yaw), sz = std::sin(state.yaw);
    transform = {cz, sz, 0, state.position.x,
                 -sz, cz, 0, state.position.y,
                 0, 0, 1, state.position.z,
                 0, 0, 0, 1};
}

// PlayerObject::setTransform: position, and the yaw of the matrix's forward axis.
void PlayerObject::setTransform(const std::array<float, 16>& m) {
    state.position = {m[3], m[7], m[11]};
    state.yaw = -std::atan2(-m[1], m[5]);
    state.velocity = {0, 0, 0};
    state.initialized = true;
    syncTransform();
    setMaskBits(MoveMask | NoWarpMask);
}

void PlayerObject::updateDamageLevel() {
    setDamageState(damage >= maxDamage() ? Disabled : Enabled);
}

// The PlayerData box, feet at the origin.
bool PlayerObject::worldBox(float lo[3], float hi[3]) const {
    ScriptObject* data = ScriptEngine::instance().findObject(dataBlock().c_str());
    const auto box = Fields::point(data, "boxSize", {1, 1, 2.3f});
    const float x = transform[3], y = transform[7], z = transform[11];
    lo[0] = x - box[0] * 0.5f; hi[0] = x + box[0] * 0.5f;
    lo[1] = y - box[1] * 0.5f; hi[1] = y + box[1] * 0.5f;
    lo[2] = z; hi[2] = z + box[2];
    return true;
}

void PlayerObject::setVelocity(const Point3F& velocity) {
    state.velocity = velocity;
    setMaskBits(MoveMask);
}

void PlayerObject::applyImpulse(const Point3F& impulse) {
    const float mass = dataFloat("mass", 1.0f);
    if (mass <= 0) return;
    setVelocity({state.velocity.x + impulse.x / mass, state.velocity.y + impulse.y / mass,
                 state.velocity.z + impulse.z / mass});
}

const char* PlayerObject::stateName() const {
    if (damageState != Enabled) return "Dead";
    if (state.mounted) return "Mounted";
    if (state.actionState == PlayerPrediction::RecoverState) return "Recover";
    return "Move";
}

void PlayerObject::processMove(const ClientMoveIn* move) {
    ShapeBase::processMove(move);
    const PlayerPrediction::Data* data = physics();
    if (!data) return;
    PlayerPrediction::Move m;
    if (move) m = unclamp(*move);
    state.energy = energy;
    state.damageState = (int)damageState;
    state.predictionCount = PlayerPrediction::MaxPredictionTicks; // the server always ticks
    const Point3F before = state.position;
    const float yawBefore = state.yaw;
    const ServerCollision& world = serverCollision();
    PlayerPrediction::processTick(state, *data, SimState::server().gravity, &m, 0.0f, collision, world.triangles, world.water);
    energy = state.energy;
    syncTransform();
    // The items and corpses the player touches: onCollision both ways.
    if (damageState == Enabled) {
        PlayerContacts::queue(*this);
        notifyCollision();
    }
    if (before.x != state.position.x || before.y != state.position.y || before.z != state.position.z ||
        yawBefore != state.yaw || move)
        setMaskBits(MoveMask);
}

// Retail PlayerObject::packUpdate, in the order the client reads it.
uint32_t PlayerObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    w.writeFlag(false); // ImpactMask
    w.writeFlag(false); // ActionMask: no action animation
    w.writeFlag(false); // arm action
    const bool controlledHere = !controllingClient.empty() && connection.script &&
                                controllingClient == ScriptEngine::instance().objectKey(connection.script);
    if (w.writeFlag(controlledHere && !(mask & InitialUpdateMask))) return ret;
    const PlayerPrediction::Data* data = physics();
    const float maxLook = data && data->maxLookAngle != 0 ? data->maxLookAngle : 1.0f;
    if (w.writeFlag(mask & MoveMask)) {
        w.writeInt(state.actionState, 3);
        if (w.writeFlag(state.actionState == PlayerPrediction::RecoverState)) w.writeInt(state.recoverTicks, 7);
        w.writeFlag(state.falling);
        w.writeFlag(state.jetting);
        w.writeCompressedPoint({state.position.x, state.position.y, state.position.z});
        const float len = std::sqrt(state.velocity.x * state.velocity.x + state.velocity.y * state.velocity.y +
                                    state.velocity.z * state.velocity.z);
        if (w.writeFlag(len > 0.02f)) {
            w.writeInt((int32_t)std::min(len * 32.0f, 8191.0f), 13);
            w.writeNormalVector({state.velocity.x / len, state.velocity.y / len, state.velocity.z / len}, 10);
        }
        w.writeSignedFloat(std::clamp(state.headPitch / maxLook, -1.0f, 1.0f), 6);
        w.writeSignedFloat(std::clamp(state.headYaw / maxLook, -1.0f, 1.0f), 6);
        float yaw = std::fmod(state.yaw, TwoPi);
        if (yaw < 0) yaw += TwoPi;
        w.writeFloat(yaw / TwoPi, 7);
        // delta.move: the last move, packed.
        const PlayerPrediction::Move& m = state.move;
        auto angle = [](float a) { return (uint32_t)((int64_t)((a / TwoPi) * 0x10000)) & 0xFFFF; };
        auto axis = [](float v) { return v < -1 ? 0 : v > 1 ? 32 : (int)((v + 1) * 16); };
        const uint32_t pyaw = angle(m.yaw), ppitch = angle(m.pitch), proll = angle(m.roll);
        if (w.writeFlag(pyaw != 0)) w.writeInt((int32_t)pyaw, 16);
        if (w.writeFlag(ppitch != 0)) w.writeInt((int32_t)ppitch, 16);
        if (w.writeFlag(proll != 0)) w.writeInt((int32_t)proll, 16);
        w.writeInt(axis(m.x), 6);
        w.writeInt(axis(m.y), 6);
        w.writeInt(axis(m.z), 6);
        w.writeFlag(m.freeLook);
        for (bool t : m.trigger) w.writeFlag(t);
        w.writeFlag(!(mask & NoWarpMask));
    }
    const float max = maxEnergy();
    w.writeFloat(std::clamp(max > 0 ? energy / max : 0.0f, 0.0f, 1.0f), 5);
    return ret;
}

// Retail PlayerObject::writePacketData.
bool PlayerObject::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = ShapeBase::writePacketData(connection, w);
    w.writeInt(state.actionState, 3);
    if (w.writeFlag(state.actionState == PlayerPrediction::RecoverState)) w.writeInt(state.recoverTicks, 7);
    if (w.writeFlag(state.jumpDelay > 0)) w.writeInt(state.jumpDelay, 7);
    if (w.writeFlag(!state.mounted)) {
        w.writePoint({state.position.x, state.position.y, state.position.z});
        w.writePoint({state.velocity.x, state.velocity.y, state.velocity.z});
        w.writeInt(std::min(state.jumpSurfaceLastContact, 15), 4);
        w.setCompressionPoint({state.position.x, state.position.y, state.position.z});
    }
    w.writeF32(state.headPitch);
    w.writeF32(state.headYaw);
    w.writeF32(state.yaw);
    w.writeFlag(false); // no controlled (piloted) object
    w.writeFlag(state.disableMove);
    w.writeFlag(false); // pilot
    return ret;
}

void registerPlayerNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Player", [] { return std::make_shared<PlayerObject>(); });
    using Args = std::vector<VMValue>;
    auto player = [](const Args& args) -> PlayerObject* {
        return args.empty() ? nullptr : EngineObjects::get<PlayerObject>(args[0].toString());
    };
    ts.registerNative("Player::getState", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        return VMValue(p ? p->stateName() : "");
    });
    ts.registerNative("Player::setTransform", [player](const Args& args) -> VMValue {
        if (auto* p = player(args); p && args.size() > 1) p->setTransform(TorqueMath::parse(args[1].toString()));
        return VMValue("");
    });
    // Player::getDamageLocation(pos): "legs|torso|head <quadrant>".
    ts.registerNative("Player::getDamageLocation", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p || args.size() < 2) return VMValue("");
        float w[3] = {0, 0, 0};
        std::sscanf(args[1].toString().c_str(), "%f %f %f", &w[0], &w[1], &w[2]);
        // mWorldToObj: the inverse of the yaw-only transform.
        const auto& m = p->transform;
        const float d[3] = {w[0] - m[3], w[1] - m[7], w[2] - m[11]};
        const float x = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
        const float y = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
        const float z = m[2] * d[0] + m[6] * d[1] + m[10] * d[2];
        ScriptObject* data = ScriptEngine::instance().findObject(p->dataBlock().c_str());
        const auto box = Fields::point(data, "boxSize", {1, 1, 2.3f});
        const float torso = Fields::f32(data, "boxTorsoPercentage", 0.55f) * box[2];
        const float head = Fields::f32(data, "boxHeadPercentage", 0.85f) * box[2];
        const char* vert = z <= torso ? "legs" : z <= head ? "torso" : "head";
        const char* quad;
        if (std::string(vert) != "head") {
            quad = y >= 0 ? (x <= 0 ? "front_left" : "front_right") : (x <= 0 ? "back_left" : "back_right");
        } else {
            // The head fractions are TypeS32 fields in the engine.
            const float backPoint = box[0] * (Fields::s32(data, "boxHeadBackPercentage", 0) - 0.5f);
            const float frontPoint = box[0] * (Fields::s32(data, "boxHeadFrontPercentage", 1) - 0.5f);
            const float leftPoint = box[1] * (Fields::s32(data, "boxHeadLeftPercentage", 0) - 0.5f);
            const float rightPoint = box[1] * (Fields::s32(data, "boxHeadRightPercentage", 1) - 0.5f);
            int index = y < backPoint ? 0 : y <= frontPoint ? 3 : 6;
            index += x < leftPoint ? 0 : x <= rightPoint ? 1 : 2;
            static const char* names[9] = {"left_back", "middle_back", "right_back", "left_middle", "middle_middle",
                                           "right_middle", "left_front", "middle_front", "right_front"};
            quad = names[index];
        }
        return VMValue(std::string(vert) + " " + quad);
    });
    ts.registerNative("Player::applyImpulse", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p || args.size() < 3) return VMValue("");
        Point3F v{};
        std::sscanf(args[2].toString().c_str(), "%f %f %f", &v.x, &v.y, &v.z);
        p->applyImpulse(v);
        return VMValue("");
    });
    ts.registerNative("Player::setVelocity", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p || args.size() < 2) return VMValue("");
        Point3F v{};
        std::sscanf(args[1].toString().c_str(), "%f %f %f", &v.x, &v.y, &v.z);
        p->setVelocity(v);
        return VMValue(1);
    });
    ts.registerNative("Player::getVelocity", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p) return VMValue("0 0 0");
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", p->state.velocity.x, p->state.velocity.y, p->state.velocity.z);
        return VMValue(buffer);
    });
    // SceneObject transform methods for engine scene objects.
    auto scene = [](const Args& args) -> SceneObject* {
        return args.empty() ? nullptr : EngineObjects::get<SceneObject>(args[0].toString());
    };
    ts.registerNative("SceneObject::getTransform", [scene](const Args& args) -> VMValue {
        auto* o = scene(args);
        return VMValue(o ? TorqueMath::format(o->transform) : std::string());
    });
    ts.registerNative("SceneObject::getPosition", [scene](const Args& args) -> VMValue {
        auto* o = scene(args);
        if (!o) return VMValue("");
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", o->transform[3], o->transform[7], o->transform[11]);
        return VMValue(buffer);
    });
    ts.registerNative("SceneObject::setTransform", [scene](const Args& args) -> VMValue {
        if (auto* o = scene(args); o && args.size() > 1) {
            o->transform = TorqueMath::parse(args[1].toString());
            o->setMaskBits(0xFFFFFFFFu & ~1u);
        }
        return VMValue("");
    });
}
