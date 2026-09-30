#include "sim/player.h"
#include "sim/force_field.h"
#include "sim/datablock_pack.h"
#include "game/player_animation.h"
#include "render/dts_loader.h"
#include "core/engine.h"
#include <map>
#include <strings.h>
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
    if (m.exact) {
        PlayerPrediction::Move move;
        move.x = m.fx; move.y = m.fy; move.z = m.fz;
        move.yaw = m.fyaw; move.pitch = m.fpitch; move.roll = m.froll;
        move.freeLook = m.freeLook;
        for (int i = 0; i < 6; ++i) move.trigger[i] = triggers[i];
        return move;
    }
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
bool PlayerObject::canJump() const {
    return state.actionState == PlayerPrediction::MoveState && damageState == ShapeBase::Enabled && mount.empty() &&
           !state.jumpDelay && energy >= dataFloat("minJumpEnergy", 0) &&
           state.jumpSurfaceLastContact < PlayerPrediction::JumpSkipContactsMax;
}

float PlayerObject::getJetAbility(float& thrust, float& duration, float& jumpSpeed) const {
    const float mass = dataFloat("mass", 1.0f);
    thrust = dataFloat("jetForce", 0) * PlayerPrediction::TickSec / mass;
    const float drain = waterCoverage < 0.9f ? dataFloat("jetEnergyDrain", 0) : dataFloat("underwaterJetEnergyDrain", 0);
    const float net = std::max(drain - rechargeRate, 0.01f);
    duration = maxEnergy() / net;
    jumpSpeed = dataFloat("jumpForce", 0) / mass;
    return energy / net;
}

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

Point3F PlayerObject::getMomentum() const {
    const float m = mass();
    return {state.velocity.x * m, state.velocity.y * m, state.velocity.z * m};
}

void PlayerObject::setMomentum(const Point3F& momentum) {
    const float m = mass();
    state.velocity = {momentum.x / m, momentum.y / m, momentum.z / m};
    setMaskBits(MoveMask);
}

bool PlayerObject::displaceObject(const Point3F& displacement) {
    // LH_HACK (the Training crash): displacement recursion is bounded.
    static uint32_t sBalance = 0;
    const float vellen = PlayerPrediction::length(state.velocity);
    if (vellen < 0.001f || sBalance > 16) {
        state.velocity = {0, 0, 0};
        return false;
    }
    const PlayerPrediction::Data* data = physics();
    if (!data) return false;
    const float dt = PlayerPrediction::length(displacement) / vellen;
    sBalance++;
    const ServerCollision& world = serverCollision();
    const PlayerPrediction::GatherTriangles gather = [&](const Point3F& min, const Point3F& max,
                                                         std::vector<PlayerPrediction::Triangle>& out) {
        if (world.triangles) world.triangles(min, max, out);
        ForceFields::gather(this, min, max, out);
    };
    const Point3F initial = state.position;
    collision.prepare(gather, state.position, data->boxSize, PlayerPrediction::mul(state.velocity, dt),
                      data->maxStepHeight);
    const bool result = PlayerPrediction::updatePos(state, *data, collision, initial, dt);
    sBalance--;
    syncTransform();
    setMaskBits(MoveMask);
    return result;
}

void PlayerObject::applyImpulse(const Point3F& impulse) {
    const float mass = dataFloat("mass", 1.0f);
    if (mass <= 0) return;
    setVelocity({state.velocity.x + impulse.x / mass, state.velocity.y + impulse.y / mass,
                 state.velocity.z + impulse.z / mass});
}

// PlayerData::preload: the shape's sequences, then the TSShapeConstructor's
// sequence0.. DSQs (aliases rename), as the retail action table
// (PlayerAnimation::buildActionTable, the client's same table).
const std::vector<std::string>& PlayerObject::actionNames() {
    static std::map<std::string, std::vector<std::string>> cache;
    static const std::vector<std::string> none;
    ScriptObject* data = ScriptEngine::instance().findObject(dataBlock().c_str());
    std::string shapeFile = Fields::string(data, "shapeFile");
    if (shapeFile.empty() || !Engine::instance().filesys) return none;
    std::string key = shapeFile;
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<std::string> names;
    const auto bytes = Engine::instance().fs().read(("shapes/" + shapeFile).c_str());
    if (!bytes.empty()) {
        DTSLoadResult shape = loadDTS(bytes.data(), bytes.size(), shapeFile.c_str());
        std::vector<DTSShape::Animation> animations = shape.animations;
        // The constructor whose baseShape is this shape.
        ScriptObject* ctor = nullptr;
        for (auto& [name, object] : ScriptEngine::instance().objects)
            if (object && strcasecmp(object->className.c_str(), "TSShapeConstructor") == 0 &&
                strcasecmp(Fields::string(object, "baseShape").c_str(), shapeFile.c_str()) == 0) {
                ctor = object;
                break;
            }
        for (int i = 0; ctor && i < 127; ++i) {
            const std::string entry = Fields::string(ctor, ("sequence" + std::to_string(i)).c_str());
            if (entry.empty()) continue;
            const size_t split = entry.find_first_of(" \t");
            const std::string file = entry.substr(0, split);
            std::string alias;
            if (split != std::string::npos) {
                const size_t start = entry.find_first_not_of(" \t", split);
                if (start != std::string::npos) alias = entry.substr(start);
                while (!alias.empty() && std::isspace((unsigned char)alias.back())) alias.pop_back();
            }
            const auto dsq = Engine::instance().fs().read(("shapes/" + file).c_str());
            if (dsq.empty() || importDSQ(dsq.data(), dsq.size(), shape.nodes, alias, animations) < 0) continue;
        }
        std::vector<std::string> sequenceNames;
        for (const auto& a : animations) sequenceNames.push_back(a.name);
        for (int index : PlayerAnimation::buildActionTable(sequenceNames))
            names.push_back(index >= 0 && index < (int)sequenceNames.size() ? sequenceNames[index] : std::string());
    }
    return cache.emplace(key, std::move(names)).first->second;
}

// Player::setActionThread(name, hold, wait, fsp): the first match after the
// root action.
bool PlayerObject::setActionThread(const std::string& name, bool hold, bool firstPerson) {
    const auto& names = actionNames();
    for (size_t i = 1; i < names.size(); ++i) {
        if (names[i].empty() || strcasecmp(names[i].c_str(), name.c_str()) != 0) continue;
        if (action != (int)i) {
            action = (int)i;
            actionHold = hold;
            actionFirstPerson = firstPerson;
        }
        setMaskBits(ActionMask);
        return true;
    }
    return false;
}

bool PlayerObject::setArmThread(const std::string& name) {
    const auto& names = actionNames();
    for (size_t i = 0; i < names.size(); ++i)
        if (!names[i].empty() && strcasecmp(names[i].c_str(), name.c_str()) == 0) {
            if (armAction != (int)i) {
                armAction = (int)i;
                setMaskBits(ActionMask);
            }
            return true;
        }
    return false;
}

const char* PlayerObject::stateName() const {
    if (damageState != Enabled) return "Dead";
    if (state.mounted) return "Mounted";
    if (state.actionState == PlayerPrediction::RecoverState) return "Recover";
    return "Move";
}

void PlayerObject::setControlObject(const std::string& object) {
    const std::string self = script ? ScriptEngine::instance().objectKey(script) : std::string();
    if (auto* old = controlObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(controlObject)) {
        old->controllingObject.clear();
        old->controllingClient.clear();
    }
    controlObject.clear();
    auto* shape = object.empty() || object == self ? nullptr : EngineObjects::get<ShapeBase>(object);
    if (!shape) return;
    if (auto* other = EngineObjects::get<PlayerObject>(shape->controllingObject)) other->setControlObject("");
    if (auto* client = EngineObjects::get<GameConnection>(shape->controllingClient);
        client && client->controlObject() == object)
        client->setControlObject("");
    controlObject = object;
    shape->controllingObject = self;
    shape->controllingClient = controllingClient;
}

void PlayerObject::processMove(const ClientMoveIn* move) {
    // The control object gets the move, less the jump trigger while mounted
    // and the view while free-looking; the player keeps those.
    ClientMoveIn pMove;
    if (auto* control = controlObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(controlObject)) {
        control->controllingClient = controllingClient;
        if (!move) {
            control->processMove(nullptr);
        } else {
            ClientMoveIn cMove = *move;
            if (!mount.empty()) {
                pMove.trigger[2] = move->trigger[2];
                cMove.trigger[2] = false;
            }
            if (move->freeLook) {
                pMove.yaw = move->yaw;
                pMove.pitch = move->pitch;
                pMove.roll = move->roll;
                pMove.freeLook = true;
                cMove.freeLook = false;
                cMove.yaw = cMove.pitch = cMove.roll = 0;
            }
            control->processMove(damageState == Enabled ? &cMove : nullptr);
            move = &pMove;
        }
    }
    ShapeBase::processMove(move);
    const PlayerPrediction::Data* data = physics();
    if (!data) return;
    PlayerPrediction::Move m;
    if (move) m = unclamp(*move);
    state.energy = energy;
    state.damageState = (int)damageState;
    // Mounted: the mount places the player (followMount in ShapeBase).
    state.mounted = !mount.empty();
    if (state.mounted) {
        state.position = {transform[3], transform[7], transform[11]};
        state.velocity = {0, 0, 0};
    }
    state.predictionCount = PlayerPrediction::MaxPredictionTicks; // the server always ticks
    const Point3F before = state.position;
    const float yawBefore = state.yaw;
    const ServerCollision& world = serverCollision();
    // The static world and the force fields not permeable to this player.
    const PlayerPrediction::GatherTriangles gather = [&](const Point3F& min, const Point3F& max,
                                                         std::vector<PlayerPrediction::Triangle>& out) {
        if (world.triangles) world.triangles(min, max, out);
        ForceFields::gather(this, min, max, out);
    };
    PlayerPrediction::processTick(state, *data, SimState::server().gravity, &m, 0.0f, collision, gather, world.water);
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
    // ActionMask: an action beyond the movement table (deaths, taunts).
    if (w.writeFlag((mask & ActionMask) && action >= PlayerAnimation::NumTableActions)) {
        w.writeInt(action, 8);
        w.writeFlag(actionHold);
        w.writeFlag(false); // not at its end (the server does not animate)
        w.writeFlag(actionFirstPerson);
        w.writeFlag(false); // from the start
    }
    // The arm thread, unless it is the look action on the initial update.
    int lookAction = -1;
    {
        const auto& names = actionNames();
        for (size_t i = 0; i < names.size(); ++i)
            if (strcasecmp(names[i].c_str(), "look") == 0) { lookAction = (int)i; break; }
    }
    if (w.writeFlag((mask & ActionMask) && armAction >= 0 && (!(mask & InitialUpdateMask) || armAction != lookAction)))
        w.writeInt(armAction, 8);
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
    bool result = ret;
    if (auto* control = controlObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(controlObject)) {
        const int index = connection.ghostIndex(controlObject);
        if (w.writeFlag(index != -1)) {
            w.writeInt(index, 10);
            result = control->writePacketData(connection, w);
        } else {
            result = false; // the control object is not on the other side yet
        }
    } else {
        w.writeFlag(false);
    }
    w.writeFlag(state.disableMove);
    w.writeFlag(pilot);
    return result;
}

void registerPlayerNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Player", [] { return std::make_shared<PlayerObject>(); });
    using Args = std::vector<VMValue>;
    auto player = [](const Args& args) -> PlayerObject* {
        return args.empty() ? nullptr : EngineObjects::get<PlayerObject>(args[0].toString());
    };
    // Player::setControlObject(obj) / getControlObject / clearControlObject.
    ts.registerNative("Player::setControlObject", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p || args.size() < 2) return VMValue(0);
        ScriptObject* object = ScriptEngine::instance().findObject(args[1].toString().c_str());
        p->setControlObject(object ? ScriptEngine::instance().objectKey(object) : std::string());
        return VMValue(1);
    });
    ts.registerNative("Player::getControlObject", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        ScriptObject* object = p && !p->controlObject.empty()
            ? ScriptEngine::instance().findObject(p->controlObject.c_str()) : nullptr;
        return VMValue(object ? ScriptEngine::instance().objectId(object) : 0);
    });
    ts.registerNative("Player::clearControlObject", [player](const Args& args) -> VMValue {
        if (auto* p = player(args)) p->setControlObject("");
        return VMValue("");
    });
    ts.registerNative("Player::setPilot", [player](const Args& args) -> VMValue {
        if (auto* p = player(args); p && args.size() > 1) p->pilot = args[1].toBool();
        return VMValue("");
    });
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
    // Player::setMoveState: a disabled player ignores its moves.
    ts.registerNative("Player::disableMove", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (p) {
            p->state.disableMove = args.size() > 1 && args[1].toBool();
            if (!p->controllingClient.empty())
                if (auto* c = EngineObjects::get<GameConnection>(p->controllingClient)) c->setControlObjectDirty();
        }
        return VMValue("");
    });
    ts.registerNative("Player::setArmThread", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        return VMValue(p && args.size() > 1 && p->setArmThread(args[1].toString()) ? 1 : 0);
    });
    // setActionThread(sequenceName, <hold>, <fsp>).
    ts.registerNative("Player::setActionThread", [player](const Args& args) -> VMValue {
        auto* p = player(args);
        if (!p || args.size() < 2) return VMValue(0);
        const bool hold = args.size() > 2 && args[2].toBool();
        const bool fsp = args.size() > 3 ? args[3].toBool() : true;
        return VMValue(p->setActionThread(args[1].toString(), hold, fsp) ? 1 : 0);
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
