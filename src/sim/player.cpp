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
