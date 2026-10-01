#include "sim/target_manager.h"
#include "sim/containers.h"
#include "sim/static_shapes.h"
#include "sim/vehicle.h"
#include "sim/player.h"
#include "sim/engine_classes.h"
#include "sim/game_base.h"
#include "sim/game_connection.h"
#include "sim/net_object.h"
#include "sim/net_string_table.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <strings.h>
#include <vector>

namespace ServerTargets {

namespace {

// DataBlockObjectIdFirst/Last as the retail TargetInfoEvent packs them (11
// bits, like GameBase::packUpdate's datablock).
constexpr uint32_t DataBlockIdFirst = 3, DataBlockIdLast = 2050;
// SimTargetAudioEvent's description range and file tag width, as the
// client's readers (DemoParser, V12::readServerEvents) decode them.
constexpr uint32_t AudioDescriptionIdLast = 1026;
constexpr int AudioFileTagBits = 12;
constexpr int StringIdBitSize = 10;
constexpr int NetStringEventClass = 7;

// ClientTarget types (RemoveClientTargetTypeEvent).
enum { AssignedTask = 0, PotentialTask, Waypoint, NumClientTargetTypes };

using Color = std::array<uint8_t, 4>;
constexpr Color DefaultColor{255, 0, 0, 255}; // SensorInfo::smDefaultColor

struct Manager {
    Target targets[MaxTargets];
    uint32_t freeMask[TargetFreeMaskSize]{};
    uint32_t freeCount = MaxTargets;
    uint32_t sensorGroupCount = 0;
    uint32_t alwaysVisMask[32]{}, neverVisMask[32]{}, friendlyMask[32]{}, listenMask[32]{};
    Color groupColor[32][32];
    // SensorInfo::targetPingMask: the targets each sensor group sees.
    uint32_t pingMask[32][TargetFreeMaskSize]{};
    uint32_t lastSensedObject = 0;
    Manager() { reset(); }
    void reset();
};

void Manager::reset() {
    freeCount = MaxTargets;
    sensorGroupCount = 0;
    for (auto& mask : freeMask) mask = 0;
    for (auto& group : pingMask) for (auto& mask : group) mask = 0;
    for (auto& target : targets) target.clear();
    // Team targets are reserved.
    freeCount -= 32;
    freeMask[0] = 0xffffffffu;
    for (uint32_t i = 0; i < 32; ++i) {
        for (auto& color : groupColor[i]) color = DefaultColor;
        targets[i].sensorGroup = i;
        // Teams are friendly to, visible to and can listen to themselves.
        listenMask[i] = alwaysVisMask[i] = friendlyMask[i] = 1u << i;
        neverVisMask[i] = 0;
    }
}

Manager& manager() {
    static Manager instance;
    return instance;
}

// GameConnection state the Torch connection does not carry.
struct ConnectionState {
    int scriptId = -1;
    bool receivedDataBlocks = false;
    uint32_t sensorGroup = 0;
    int targetId = -1;
    float targetPos[3]{};
    std::bitset<NetStrings::MaxStrings> stringSent; // NetStringEvents this module posted
};

std::map<const GameConnection*, ConnectionState>& connectionStates() {
    static std::map<const GameConnection*, ConnectionState> states;
    return states;
}

int scriptIdOf(const GameConnection& connection) {
    if (!connection.script || !ScriptEngine::exists()) return 0;
    return ScriptEngine::instance().objectId(connection.script);
}

ConnectionState& stateOf(const GameConnection& connection) {
    ConnectionState& state = connectionStates()[&connection];
    const int id = scriptIdOf(connection);
    if (state.scriptId != id) {
        state = ConnectionState{};
        state.scriptId = id;
    }
    return state;
}

void error(const char* format, int value) {
    Console::instance().printf(LogLevel::Error, format, value);
}

// dAtoi.
int32_t argInt(const VMValue& value) {
    const std::string text = value.toString();
    return (int32_t)std::strtol(text.c_str(), nullptr, 10);
}

// A tag argument: "\x01<id>" or the bare id (the prefix may already be gone).
uint32_t argTag(const VMValue& value) {
    const std::string text = value.toString();
    if (text.empty()) return 0;
    return (uint32_t)std::strtol(text.c_str() + (NetStrings::isTag(text) ? 1 : 0), nullptr, 10);
}

ScriptObject* objectById(int id) {
    if (id <= 0 || !ScriptEngine::exists()) return nullptr;
    auto& ids = ScriptEngine::instance().objectsById;
    auto it = ids.find(id);
    return it == ids.end() ? nullptr : it->second;
}

// Sim::findObject(handle) typed as a datablock of `base` (dynamic_cast).
ScriptObject* findDataBlock(const std::string& handle, const char* base) {
    if (handle.empty() || !ScriptEngine::exists()) return nullptr;
    ScriptObject* object = ScriptEngine::instance().findObject(handle.c_str());
    if (!object || !EngineClasses::isA(object->className, base)) return nullptr;
    return object;
}

int liveDataBlock(int id, const char* base) {
    ScriptObject* object = objectById(id);
    return object && EngineClasses::isA(object->className, base) ? id : 0;
}

bool isAIControlled(const GameConnection& connection) {
    return connection.script && EngineClasses::isA(connection.script->className, "AIConnection");
}

// NetConnection::getConnectionList, less the client's own ServerConnection.
std::vector<GameConnection*> serverConnections() {
    std::vector<std::pair<int, GameConnection*>> found;
    if (!ScriptEngine::exists()) return {};
    auto& engine = ScriptEngine::instance();
    for (auto& [key, object] : engine.objects) {
        auto* connection = object ? dynamic_cast<GameConnection*>(object->engine.get()) : nullptr;
        if (!connection || !connection->isServer) continue;
        found.push_back({engine.objectId(object), connection});
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::vector<GameConnection*> out;
    for (auto& [id, connection] : found) out.push_back(connection);
    return out;
}

GameConnection* findConnection(const VMValue& handle) {
    const std::string text = handle.toString();
    return text.empty() ? nullptr : EngineObjects::get<GameConnection>(text);
}

// NetConnection::checkString: the tag's NetStringEvent ahead of the event
// that names it.
void checkString(GameConnection& connection, uint32_t id) {
    if (id == 0 || id >= NetStrings::MaxStrings) return;
    ConnectionState& state = stateOf(connection);
    if (state.stringSent[id]) return;
    state.stringSent[id] = true;
    const std::string* text = NetStrings::lookup(id);
    const bool hasText = text != nullptr;
    const std::string copy = hasText ? *text : std::string();
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = NetStringEventClass;
    event->pack = [id, copy, hasText](TorqueBitWriter& w) {
        w.writeInt((int32_t)id, StringIdBitSize);
        if (w.writeFlag(hasText)) w.writeString(copy);
    };
    connection.postEvent(event);
}

void post(GameConnection& connection, int classIndex, std::function<void(TorqueBitWriter&)> pack) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = classIndex;
    event->pack = std::move(pack);
    connection.postEvent(event);
}

// TargetInfoEvent: -1 leaves a field unchanged on the client.
struct TargetInfoFields {
    int target = 0;
    int nameTag = -1, skinTag = -1, voiceTag = -1, typeTag = -1, skinPrefTag = -1;
    int sensorGroup = -1, dataBlockId = -1, renderFlags = -1;
    float voicePitch = -1.0f;
};

void postTargetInfo(GameConnection& connection, TargetInfoFields f) {
    post(connection, TargetInfoEvent, [f](TorqueBitWriter& w) {
        w.writeInt(f.target, TargetIdBitSize);
        auto tag = [&](int value) {
            if (w.writeFlag(value != -1))
                if (w.writeFlag(value != 0)) w.writeInt(value, StringIdBitSize);
        };
        tag(f.nameTag);
        tag(f.skinTag);
        tag(f.skinPrefTag);
        tag(f.voiceTag);
        tag(f.typeTag);
        if (w.writeFlag(f.sensorGroup != -1)) w.writeInt(f.sensorGroup, 5);
        if (w.writeFlag(f.dataBlockId != -1))
            if (w.writeFlag(f.dataBlockId != 0))
                w.writeRangedU32((uint32_t)f.dataBlockId, DataBlockIdFirst, DataBlockIdLast);
        if (w.writeFlag(f.renderFlags != -1)) w.writeInt(f.renderFlags, NumRenderBits);
        if (w.writeFlag(f.voicePitch != -1.0f)) {
            // [0.5, 2.0] packed as [0, 1].
            float pitch = f.voicePitch;
            if (pitch < 0.5f || pitch > 2.0f) pitch = 1.0f;
            w.writeFloat((pitch - 0.5f) / 1.5f, 7);
        }
    });
}

// Targets go only to connections that have their datablocks (the retail
// received-datablocks gate; sendTargetsToClient catches them up).
bool receivesTargets(const GameConnection& connection) {
    return !isAIControlled(connection) && stateOf(connection).receivedDataBlocks;
}

// TargetManager::updateTarget.
void updateTarget(const TargetInfoFields& fields) {
    for (GameConnection* connection : serverConnections()) {
        if (!receivesTargets(*connection)) continue;
        for (int tag : {fields.nameTag, fields.skinTag, fields.voiceTag, fields.typeTag, fields.skinPrefTag})
            if (tag != -1) checkString(*connection, (uint32_t)tag);
        postTargetInfo(*connection, fields);
    }
}

TargetInfoFields only(int target) {
    TargetInfoFields fields;
    fields.target = target;
    return fields;
}

void postSensorGroupColors(GameConnection& connection, uint32_t sensorGroup, uint32_t updateMask) {
    std::array<Color, 32> colors;
    for (int i = 0; i < 32; ++i) colors[i] = manager().groupColor[sensorGroup][i];
    post(connection, SensorGroupColorEvent, [sensorGroup, updateMask, colors](TorqueBitWriter& w) {
        w.writeInt((int32_t)sensorGroup, 5);
        w.writeU32(updateMask);
        for (int i = 0; i < 32; ++i)
            if (updateMask & (1u << i))
                if (w.writeFlag(colors[i] != DefaultColor))
                    for (uint8_t channel : colors[i]) w.writeInt(channel, 8);
    });
}

void postResetClientTargets(GameConnection& connection, bool clientTargetsOnly) {
    // (pack also resets the connection's visible target masks; Torch keeps
    // none: its packets always report them unchanged.)
    post(connection, ResetClientTargetsEvent,
         [clientTargetsOnly](TorqueBitWriter& w) { w.writeFlag(clientTargetsOnly); });
}

// The SimObject id or name handle of a target's object.
GameBase* targetObjectOf(const Target& target, std::string* key = nullptr) {
    ScriptObject* object = objectById(target.targetObject);
    if (!object) return nullptr;
    auto* base = dynamic_cast<GameBase*>(object->engine.get());
    if (base && key) *key = ScriptEngine::instance().objectKey(object);
    return base;
}

// getValidTarget.
int validTarget(int target, const char* function, bool excludeTeam) {
    if (target < 0 || target >= MaxTargets) {
        Console::instance().printf(LogLevel::Error, "TargetManager::%s: invalid target index [%d]", function, target);
        return -1;
    }
    if (excludeTeam && target < 32) {
        Console::instance().printf(LogLevel::Error,
            "TargetManager::%s: cannot change attribute on team target [%d]", function, target);
        return -1;
    }
    if (!manager().targets[target].allocated && target >= 32) {
        Console::instance().printf(LogLevel::Error,
            "TargetManager::%s: cannot change attribute on unallocated target [%d]", function, target);
        return -1;
    }
    return target;
}

int validSensorGroup(int group, const char* function) {
    if (group < 0 || group >= 32) {
        Console::instance().printf(LogLevel::Error, "TargetManager::%s: invalid sensor group [%d]", function, group);
        return -1;
    }
    return group;
}

float validVoicePitch(float pitch, const char* function) {
    if (pitch < 0.5f || pitch > 2.0f) {
        Console::instance().printf(LogLevel::Error, "TargetManager::%s: Invalid pitch [%d]", function, (int)pitch);
        return 1.0f;
    }
    return pitch;
}

enum MaskType { AlwaysVisMaskType, NeverVisMaskType, FriendlyMaskType };

// All targets in the group inherit a group mask change.
void updateSensorGroupMask(uint32_t sensorGroup, uint32_t mask, MaskType type) {
    Manager& m = manager();
    for (int i = 0; i < MaxTargets; ++i) {
        if (!(m.freeMask[i >> 5] & (1u << (i & 31)))) continue;
        Target& target = m.targets[i];
        if (target.sensorGroup != sensorGroup) continue;
        if (type == NeverVisMaskType) target.sensorNeverVisMask = mask;
        else if (type == FriendlyMaskType) target.sensorFriendlyMask = mask;
        else target.sensorAlwaysVisMask = mask;
    }
}

} // namespace

void Target::clear(bool clearFlags) {
    nameTag = skinTag = skinPrefTag = voiceTag = typeTag = 0;
    voicePitch = 1.0f;
    sensorGroup = 0;
    allocated = false;
    renderFlags = 0;
    sensorVisMask = sensorAlwaysVisMask = sensorNeverVisMask = sensorFriendlyMask = 0;
    // A freed target can be reallocated and sensed before a connection
    // has been updated: its flags stay.
    if (clearFlags) sensorFlags = 0;
    targetObject = 0;
    sensorData = 0;
    shapeBaseData = 0;
}

void clear() { manager().reset(); }

void reset() {
    for (GameConnection* connection : serverConnections())
        if (!isAIControlled(*connection)) postResetClientTargets(*connection, false);
    manager().reset();
}

void newClient(GameConnection& connection) {
    if (isAIControlled(connection)) return;
    Manager& m = manager();
    for (int i = 0; i < MaxTargets; ++i) {
        if (!(m.freeMask[i >> 5] & (1u << (i & 31)))) continue;
        const Target& target = m.targets[i];
        for (uint32_t tag : {target.nameTag, target.skinTag, target.voiceTag, target.typeTag})
            checkString(connection, tag);
        TargetInfoFields fields;
        fields.target = i;
        fields.nameTag = (int)target.nameTag;
        fields.skinTag = (int)target.skinTag;
        fields.voiceTag = (int)target.voiceTag;
        fields.typeTag = (int)target.typeTag;
        fields.sensorGroup = (int)target.sensorGroup;
        fields.dataBlockId = liveDataBlock(target.shapeBaseData, "ShapeBaseData");
        fields.renderFlags = (int)target.renderFlags;
        fields.voicePitch = target.voicePitch;
        postTargetInfo(connection, fields);
    }
}

int allocTarget(uint32_t nameTag, uint32_t skinTag, uint32_t voiceTag, uint32_t typeTag, uint32_t sensorGroup,
                uint32_t dataBlockId, float voicePitch, uint32_t prefSkin) {
    Manager& m = manager();
    if (sensorGroup >= 32) {
        error("TargetManager::allocTarget: invalid sensorGroup [%d]", (int)sensorGroup);
        return -1;
    }
    if (m.freeCount == 0) return -1;
    int id = -1;
    for (int i = 0; i < TargetFreeMaskSize && id == -1; ++i) {
        if (m.freeMask[i] == 0xffffffffu) continue;
        for (int j = 0; j < 32; ++j)
            if (!(m.freeMask[i] & (1u << j))) {
                id = (i << 5) + j;
                m.freeMask[i] |= 1u << j;
                break;
            }
    }
    if (id == -1) return -1;
    --m.freeCount;
    Target& target = m.targets[id];
    target.allocated = true;
    target.nameTag = nameTag;
    target.skinTag = skinTag;
    target.voiceTag = voiceTag;
    target.typeTag = typeTag;
    target.skinPrefTag = prefSkin;
    target.sensorGroup = sensorGroup;
    target.renderFlags = 0;
    target.voicePitch = voicePitch;
    // Vis/friend masks default to the team's.
    target.sensorAlwaysVisMask = m.alwaysVisMask[sensorGroup];
    target.sensorNeverVisMask = m.neverVisMask[sensorGroup];
    target.sensorFriendlyMask = m.friendlyMask[sensorGroup];
    target.sensorFlags = 0;
    target.sensorVisMask = 0;
    target.shapeBaseData = dataBlockId ? liveDataBlock((int)dataBlockId, "ShapeBaseData") : 0;
    TargetInfoFields fields;
    fields.target = id;
    fields.nameTag = (int)nameTag;
    fields.skinTag = (int)skinTag;
    fields.voiceTag = (int)voiceTag;
    fields.typeTag = (int)typeTag;
    fields.skinPrefTag = (int)prefSkin;
    fields.sensorGroup = (int)sensorGroup;
    // The engine sends the id it was given; one the client cannot hold
    // (not a datablock id) goes as none.
    fields.dataBlockId = dataBlockId >= DataBlockIdFirst && dataBlockId <= DataBlockIdLast ? (int)dataBlockId : 0;
    fields.renderFlags = 0;
    fields.voicePitch = voicePitch;
    updateTarget(fields);
    return id;
}

void freeTarget(int target) {
    if (target < 32 || target >= MaxTargets) return;
    for (GameConnection* connection : serverConnections()) {
        if (!receivesTargets(*connection)) continue;
        post(*connection, TargetFreeEvent, [target](TorqueBitWriter& w) { w.writeInt(target, TargetIdBitSize); });
    }
    Manager& m = manager();
    m.targets[target].clear(false);
    const uint32_t index = (uint32_t)target >> 5, bit = (uint32_t)target & 31;
    for (auto& group : m.pingMask) group[index] &= ~(1u << bit);
    if (!(m.freeMask[index] & (1u << bit))) return;
    m.freeMask[index] &= ~(1u << bit);
    ++m.freeCount;
}

const Target* serverTarget(int target) {
    return target >= 0 && target < MaxTargets ? &manager().targets[target] : nullptr;
}

bool isTargetFriendly(int target, uint32_t sensorGroup) {
    if (!serverTarget(target) || sensorGroup >= 32) return false;
    return (manager().targets[target].sensorFriendlyMask & (1u << sensorGroup)) != 0;
}

bool isTargetVisible(int target, uint32_t sensorGroup) {
    if (!serverTarget(target) || sensorGroup >= 32) return false;
    const Target& t = manager().targets[target];
    const uint32_t groupMask = 1u << sensorGroup;
    if (t.sensorNeverVisMask & groupMask) return false;
    if (t.sensorAlwaysVisMask & groupMask) return true;
    return (t.sensorVisMask & groupMask) != 0;
}

uint32_t sensorGroupListenMask(uint32_t sensorGroup) {
    return sensorGroup < 32 ? manager().listenMask[sensorGroup] : 0;
}

std::array<uint8_t, 4> sensorGroupColor(uint32_t sensorGroup, uint32_t colorGroup) {
    if (sensorGroup >= 32 || colorGroup >= 32) return DefaultColor;
    return manager().groupColor[sensorGroup][colorGroup];
}

uint32_t connectionSensorGroup(const GameConnection& connection) { return stateOf(connection).sensorGroup; }

// GameConnection::setSensorGroup: the client learns its group and the
// group's colours (TargetManager::clientSensorGroupChanged).
void setConnectionSensorGroup(GameConnection& connection, uint32_t group) {
    ConnectionState& state = stateOf(connection);
    if (group == state.sensorGroup) return;
    if (group >= 32) {
        error("GameConnection::setSensorGroup: invalid sensor group [%d]", (int)group);
        return;
    }
    if (connection.isServer) {
        if (!isAIControlled(connection)) postSensorGroupColors(connection, group, 0xffffffffu);
        post(connection, SetSensorGroupEvent, [group](TorqueBitWriter& w) { w.writeInt((int32_t)group, 5); });
    }
    stateOf(connection).sensorGroup = group;
}

bool receivedDataBlocks(const GameConnection& connection) { return stateOf(connection).receivedDataBlocks; }

void setReceivedDataBlocks(GameConnection& connection, bool received) {
    stateOf(connection).receivedDataBlocks = received;
}

void setServerTarget(GameConnection& connection, int targetId, const float pos[3]) {
    ConnectionState& state = stateOf(connection);
    state.targetId = targetId;
    std::copy(pos, pos + 3, state.targetPos);
}

const uint32_t* sensorGroupPingMask(uint32_t sensorGroup) {
    return manager().pingMask[sensorGroup & 31];
}

namespace {

// The target's GameBase (TargetInfo::targetObject).
ShapeBase* targetShape(const Target& t) {
    if (!t.targetObject) return nullptr;
    ScriptObject* object = ScriptEngine::instance().findObject(std::to_string(t.targetObject).c_str());
    return object ? dynamic_cast<ShapeBase*>(object->engine.get()) : nullptr;
}

// A player's eye, anything else its box centre.
Point3F sensePosition(ShapeBase& shape) {
    if (dynamic_cast<PlayerObject*>(&shape)) {
        const auto eye = shape.getEyeTransform();
        return {eye[3], eye[7], eye[11]};
    }
    return SimContainer::worldBoxCenter(shape.script);
}

Point3F velocityOf(ShapeBase& shape) {
    if (auto* p = dynamic_cast<PlayerObject*>(&shape)) return p->state.velocity;
    if (auto* v = dynamic_cast<VehicleObject*>(&shape)) return v->getVelocity();
    if (auto* i = dynamic_cast<ItemObject*>(&shape)) return {i->velocity[0], i->velocity[1], i->velocity[2]};
    return {0, 0, 0};
}

bool testLOS(ShapeBase& sensor, const Point3F& sensorPos, ShapeBase& target, const Point3F& targetPos) {
    std::vector<ScriptObject*> exempt{sensor.script, target.script};
    if (!target.mount.empty())
        if (ScriptObject* mount = ScriptEngine::instance().findObject(target.mount.c_str())) exempt.push_back(mount);
    SimContainer::RayInfo info;
    return !SimContainer::castRay(sensorPos, targetPos, SimContainer::TerrainObjectType | SimContainer::InteriorObjectType |
                                                            SimContainer::ShapeBaseObjectType, info, exempt);
}

// SensorData, with the values SensorData::onAdd derives.
struct Sensor {
    bool detects, detectsUsingLOS, detectsPassiveJammed, detectsActiveJammed, detectsCloaked, detectionPings;
    bool detectsFOVOnly, useObjectFOV, jams, jamsOnlyGroup, jamsUsingLOS;
    float detectRSquared, detectMinVSquared, halfFovCos, detectFOVPercent, jamRSquared;
    explicit Sensor(ScriptObject* d) {
        detects = Fields::boolean(d, "detects", true);
        detectsUsingLOS = Fields::boolean(d, "detectsUsingLOS", true);
        detectsPassiveJammed = Fields::boolean(d, "detectsPassiveJammed", false);
        detectsActiveJammed = Fields::boolean(d, "detectsActiveJammed", false);
        detectsCloaked = Fields::boolean(d, "detectsCloaked", false);
        detectionPings = Fields::boolean(d, "detectionPings", true);
        detectsFOVOnly = Fields::boolean(d, "detectsFOVOnly", false);
        useObjectFOV = Fields::boolean(d, "useObjectFOV", false);
        jams = Fields::boolean(d, "jams", false);
        jamsOnlyGroup = Fields::boolean(d, "jamsOnlyGroup", false);
        jamsUsingLOS = Fields::boolean(d, "jamsUsingLOS", false);
        const float r = Fields::f32(d, "detectRadius", 250), v = Fields::f32(d, "detectMinVelocity", 0),
                    j = Fields::f32(d, "jamRadius", 0);
        detectRSquared = r * r;
        detectMinVSquared = v * v;
        jamRSquared = j * j;
        halfFovCos = std::cos(Fields::f32(d, "detectFOV", 90.0f) / 2.0f * (float)M_PI / 180.0f);
        detectFOVPercent = Fields::f32(d, "detectFOVPercent", 0);
    }
};

} // namespace

// TargetManager::tickSensorState: one 32nd of the targets a tick, each
// against every sensor: what can detect it (per jam/cloak class), whether
// it is jammed or pinged, and so which sensor groups see it.
void tickSensorState() {
    Manager& m = manager();
    const uint32_t totalCount = MaxTargets - m.freeCount;
    const uint32_t pingCount = (totalCount >> 5) + 1; // ping everything once a second
    uint32_t objectCount = 0, lastSensed = m.lastSensedObject;
    std::map<int, std::unique_ptr<Sensor>> sensors;
    auto sensorOf = [&](int id) -> Sensor* {
        auto it = sensors.find(id);
        if (it == sensors.end()) {
            ScriptObject* data = id ? ScriptEngine::instance().findObject(std::to_string(id).c_str()) : nullptr;
            it = sensors.emplace(id, data ? std::make_unique<Sensor>(data) : nullptr).first;
        }
        return it->second.get();
    };
    for (uint32_t i = m.lastSensedObject + 1; i - m.lastSensedObject < MaxTargets; ++i) {
        const uint32_t index = i & (MaxTargets - 1);
        if (!(m.freeMask[index >> 5] & (1u << (index & 31)))) continue;
        Target& targetInfo = m.targets[index];
        ShapeBase* target = targetShape(targetInfo);
        if (!target) continue;
        uint32_t baseVisMask = 0, activeJamVisMask = 0, passiveJamVisMask = 0, cloakVisMask = 0;
        bool pinged = false, jammed = false, enemyJammed = false;
        const Point3F targetPos = sensePosition(*target);
        for (uint32_t sens = 0; sens < MaxTargets; ++sens) {
            if (!(m.freeMask[sens >> 5] & (1u << (sens & 31)))) continue;
            const Target& sensorInfo = m.targets[sens];
            ShapeBase* sensor = targetShape(sensorInfo);
            const Sensor* data = sensor ? sensorOf(sensorInfo.sensorData) : nullptr;
            if (!sensor || !data) continue;
            // can't detect its own bad self, but can jam...
            if (sens == index) {
                if (data->jams) jammed = true;
                continue;
            }
            if (sensor->damageState != ShapeBase::Enabled) continue;
            bool testedLOS = false, hasLOS = false;
            const uint32_t sensorMask = 1u << sensorInfo.sensorGroup;
            bool noDetect = false;
            // jams? and is/not always visible?
            if ((targetInfo.sensorAlwaysVisMask | targetInfo.sensorNeverVisMask) & sensorMask) {
                if (!data->jams) continue;
                noDetect = true;
            }
            const Point3F sensorPos = sensePosition(*sensor);
            Point3F targetVec = SimContainer::sub(targetPos, sensorPos);
            if (!noDetect && data->detects) {
                uint32_t* mask = data->detectsActiveJammed ? &activeJamVisMask
                               : data->detectsCloaked ? &cloakVisMask
                               : data->detectsPassiveJammed ? &passiveJamVisMask : &baseVisMask;
                do {
                    // see if we can skip this one:
                    if ((*mask & sensorMask) && pinged == data->detectionPings) break;
                    if (targetVec.x == 0 && targetVec.y == 0 && targetVec.z == 0) break;
                    // uncapped cylinder
                    if (targetVec.x * targetVec.x + targetVec.y * targetVec.y > data->detectRSquared) break;
                    if (data->detectMinVSquared != 0.f) {
                        const Point3F v = velocityOf(*target);
                        if (SimContainer::dot(v, v) < data->detectMinVSquared) break;
                    }
                    if (data->detectsFOVOnly) {
                        const auto cam = sensor->getEyeTransform();
                        const Point3F camDir{cam[1], cam[5], cam[9]};
                        const float d = std::clamp(SimContainer::dot(SimContainer::normalize(targetVec), camDir), -1.f, 1.f);
                        if (data->useObjectFOV) {
                            const float objectFov = sensor->cameraFov * (float)M_PI / 180.0f;
                            if (d < std::cos(objectFov / 2.f)) break;
                            if (data->detectFOVPercent != 0.f) {
                                Point3F lo, hi;
                                SimContainer::worldBox(target->script, lo, hi);
                                const float objRadius = SimContainer::len(SimContainer::sub(hi, lo)) * 0.5f;
                                const float projRadius = SimContainer::len(targetVec) * std::tan(objectFov / 2.f);
                                if ((objRadius / projRadius) * 100.f < data->detectFOVPercent) break;
                            }
                        } else if (d < data->halfFovCos) {
                            break;
                        }
                    }
                    if (data->detectsUsingLOS) {
                        testedLOS = true;
                        hasLOS = testLOS(*sensor, sensorPos, *target, targetPos);
                        if (!hasLOS) break;
                    }
                    // it's detected
                    *mask |= sensorMask;
                    // friendly do not ping
                    if (data->detectionPings && !(sensorInfo.sensorFriendlyMask & (1u << targetInfo.sensorGroup)))
                        pinged = true;
                } while (false);
            }
            // early out?
            if (!data->jams || (jammed && enemyJammed)) continue;
            if (sensorInfo.sensorGroup == targetInfo.sensorGroup) {
                if (jammed) continue;
            } else if (enemyJammed && data->jamsOnlyGroup) {
                continue;
            }
            if (SimContainer::dot(targetVec, targetVec) > data->jamRSquared) continue;
            if (data->jamsUsingLOS) {
                if (!testedLOS) hasLOS = testLOS(*sensor, sensorPos, *target, targetPos);
                if (!hasLOS) continue;
            }
            // set the jammed state
            if (sensorInfo.sensorGroup == targetInfo.sensorGroup) {
                jammed = true;
            } else {
                jammed = !data->jamsOnlyGroup;
                enemyJammed = true;
            }
        }
        // check what could detect it: active->cloaked->passive->base
        uint32_t visMask;
        if (jammed) visMask = activeJamVisMask;
        else if (target->cloaked) visMask = activeJamVisMask | cloakVisMask;
        else if (target->passiveJammed) visMask = activeJamVisMask | cloakVisMask | passiveJamVisMask;
        else visMask = activeJamVisMask | cloakVisMask | passiveJamVisMask | baseVisMask;
        visMask |= targetInfo.sensorAlwaysVisMask;
        visMask &= ~targetInfo.sensorNeverVisMask;
        targetInfo.sensorVisMask = visMask;
        targetInfo.sensorFlags = 0;
        if (pinged) targetInfo.sensorFlags |= SensorPinged;
        if (jammed || enemyJammed) {
            if (jammed) targetInfo.sensorFlags |= SensorJammed;
            if (enemyJammed) {
                targetInfo.sensorFlags |= EnemySensorJammed;
                // ShapeBase::forceUncloak
                if (target->cloaked) target->callDataBlock("onForceUncloak", {"jammed"});
            }
        }
        for (uint32_t j = 0; j < m.sensorGroupCount && j < 32; ++j, visMask >>= 1) {
            if (visMask & 1) m.pingMask[j][index >> 5] |= 1u << (index & 31);
            else m.pingMask[j][index >> 5] &= ~(1u << (index & 31));
        }
        ++objectCount;
        lastSensed = i;
        if (objectCount >= pingCount) break;
    }
    m.lastSensedObject = lastSensed;
}

} // namespace ServerTargets

// --- Console ------------------------------------------------------------------

void registerTargetManagerNatives(TorqueScript& ts) {
    using namespace ServerTargets;
    using Args = std::vector<VMValue>;
    auto arg = [](const Args& args, size_t i) { return i < args.size() ? args[i] : VMValue(""); };

    ts.setGlobal("$TargetInfo::HudRenderStart", VMValue((int32_t)HudRenderStart));
    ts.setGlobal("$TargetInfo::NumHudRenderImages", VMValue((int32_t)NumHudRenderImages));
    ts.setGlobal("$TargetInfo::CommanderListRender", VMValue((int32_t)CommanderListRender));

    ts.registerNative("resetTargets", [](const Args&) -> VMValue {
        reset();
        return VMValue("");
    });
    ts.registerNative("resetClientTargets", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        if (connection && !isAIControlled(*connection)) postResetClientTargets(*connection, arg(args, 1).toBool());
        return VMValue("");
    });
    ts.registerNative("sendTargetsToClient", [arg](const Args& args) -> VMValue {
        if (GameConnection* connection = findConnection(arg(args, 0))) newClient(*connection);
        return VMValue("");
    });
    // allocTarget(nameTag, skinTag, voiceTag, typeTag, sensorGroup, dataBlockId, voicePitch, [prefskin])
    ts.registerNative("allocTarget", [arg](const Args& args) -> VMValue {
        if (args.size() < 7) return VMValue(-1);
        const uint32_t sensorGroup = (uint32_t)argInt(args[4]);
        // The datablock by id (a name resolves too).
        uint32_t dataBlockId = (uint32_t)argInt(args[5]);
        if (!dataBlockId && !args[5].toString().empty())
            if (ScriptObject* data = ScriptEngine::instance().findObject(args[5].toString().c_str()))
                dataBlockId = (uint32_t)ScriptEngine::instance().objectId(data);
        float voicePitch = (float)std::atof(args[6].toString().c_str());
        if (voicePitch < 0.5f || voicePitch > 2.0f) voicePitch = 1.0f;
        const uint32_t prefSkin = args.size() > 7 ? argTag(args[7]) : 0;
        return VMValue((int32_t)allocTarget(argTag(args[0]), argTag(args[1]), argTag(args[2]), argTag(args[3]),
                                            sensorGroup, dataBlockId, voicePitch, prefSkin));
    });
    ts.registerNative("freeTarget", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cFreeTarget", true);
        if (target != -1) freeTarget(target);
        return VMValue("");
    });

    // Name/type: '_' as the first character leaves a string out.
    ts.registerNative("getTargetGameName", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cGetTargetGameName", false);
        if (target == -1) return VMValue("");
        const Target& t = manager().targets[target];
        if (!t.allocated) return VMValue("");
        const std::string* name = NetStrings::lookup(t.nameTag);
        const std::string* type = NetStrings::lookup(t.typeTag);
        const bool hasName = name && !name->empty() && (*name)[0] != '_';
        const bool hasType = type && !type->empty() && (*type)[0] != '_';
        if (hasName) return VMValue(hasType ? *name + " " + *type : *name);
        return VMValue(hasType ? *type : std::string());
    });

    // Tag fields: get returns the tag id, set takes a tag or an id.
    struct TagField { const char* get; const char* set; uint32_t Target::*field; int TargetInfoFields::*info; };
    for (const TagField& f : {TagField{"getTargetName", "setTargetName", &Target::nameTag, &TargetInfoFields::nameTag},
                              TagField{"getTargetSkin", "setTargetSkin", &Target::skinTag, &TargetInfoFields::skinTag},
                              TagField{"getTargetVoice", "setTargetVoice", &Target::voiceTag, &TargetInfoFields::voiceTag},
                              TagField{"getTargetType", "setTargetType", &Target::typeTag, &TargetInfoFields::typeTag}}) {
        const std::string getName = std::string("c") + (char)toupper(f.get[0]) + (f.get + 1);
        const std::string setName = std::string("c") + (char)toupper(f.set[0]) + (f.set + 1);
        ts.registerNative(f.get, [arg, f, getName](const Args& args) -> VMValue {
            const int target = validTarget(argInt(arg(args, 0)), getName.c_str(), false);
            if (target == -1) return VMValue(-1);
            return VMValue((int32_t)(manager().targets[target].*f.field));
        });
        ts.registerNative(f.set, [arg, f, setName](const Args& args) -> VMValue {
            const int target = validTarget(argInt(arg(args, 0)), setName.c_str(), false);
            if (target == -1) return VMValue("");
            const uint32_t tag = argTag(arg(args, 1));
            Target& t = manager().targets[target];
            if (t.*f.field == tag) return VMValue("");
            t.*f.field = tag;
            TargetInfoFields fields = only(target);
            fields.*f.info = (int)tag;
            updateTarget(fields);
            return VMValue("");
        });
    }

    ts.registerNative("getTargetVoicePitch", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cGetTargetVoicePitch", false);
        if (target == -1) return VMValue(-1.0f);
        return VMValue(manager().targets[target].voicePitch);
    });
    ts.registerNative("setTargetVoicePitch", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cSetTargetVoicePitch", true);
        if (target == -1) return VMValue("");
        const float pitch = validVoicePitch((float)std::atof(arg(args, 1).toString().c_str()), "cSetTargetVoicePitch");
        Target& t = manager().targets[target];
        if (t.voicePitch == pitch) return VMValue("");
        t.voicePitch = pitch;
        TargetInfoFields fields = only(target);
        fields.voicePitch = pitch;
        updateTarget(fields);
        return VMValue("");
    });

    ts.registerNative("getTargetSensorGroup", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cGetTargetSensorGroup", false);
        if (target == -1) return VMValue(-1);
        return VMValue((int32_t)manager().targets[target].sensorGroup);
    });
    ts.registerNative("setTargetSensorGroup", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cSetTargetSensorGroup", true);
        if (target == -1) return VMValue("");
        const int group = validSensorGroup(argInt(arg(args, 1)), "cSetTargetSensorGroup");
        if (group == -1) return VMValue("");
        Manager& m = manager();
        Target& t = m.targets[target];
        if (t.sensorGroup == (uint32_t)group) return VMValue("");
        t.sensorGroup = (uint32_t)group;
        t.sensorAlwaysVisMask = m.alwaysVisMask[group];
        t.sensorNeverVisMask = m.neverVisMask[group];
        t.sensorFriendlyMask = m.friendlyMask[group];
        TargetInfoFields fields = only(target);
        fields.sensorGroup = group;
        updateTarget(fields);
        return VMValue("");
    });

    // Per-target visibility masks (server only: nothing is sent).
    struct TargetMask { const char* get; const char* set; uint32_t Target::*field; };
    for (const TargetMask& f : {TargetMask{"getTargetAlwaysVisMask", "setTargetAlwaysVisMask", &Target::sensorAlwaysVisMask},
                                TargetMask{"getTargetNeverVisMask", "setTargetNeverVisMask", &Target::sensorNeverVisMask},
                                TargetMask{"getTargetFriendlyMask", "setTargetFriendlyMask", &Target::sensorFriendlyMask}}) {
        const std::string getName = std::string("c") + (char)toupper(f.get[0]) + (f.get + 1);
        const std::string setName = std::string("c") + (char)toupper(f.set[0]) + (f.set + 1);
        ts.registerNative(f.get, [arg, f, getName](const Args& args) -> VMValue {
            const int target = validTarget(argInt(arg(args, 0)), getName.c_str(), false);
            if (target == -1) return VMValue(0);
            return VMValue((int32_t)(manager().targets[target].*f.field));
        });
        ts.registerNative(f.set, [arg, f, setName](const Args& args) -> VMValue {
            const int target = validTarget(argInt(arg(args, 0)), setName.c_str(), false);
            if (target != -1) manager().targets[target].*f.field = (uint32_t)argInt(arg(args, 1));
            return VMValue("");
        });
    }

    // Sensor-group masks: always/never visible and friendly pass to every
    // target of the group.
    struct GroupMask { const char* get; const char* set; uint32_t* (*masks)(); int type; };
    for (const GroupMask& f : {
             GroupMask{"getSensorGroupAlwaysVisMask", "setSensorGroupAlwaysVisMask",
                       [] { return manager().alwaysVisMask; }, AlwaysVisMaskType},
             GroupMask{"getSensorGroupNeverVisMask", "setSensorGroupNeverVisMask",
                       [] { return manager().neverVisMask; }, NeverVisMaskType},
             GroupMask{"getSensorGroupFriendlyMask", "setSensorGroupFriendlyMask",
                       [] { return manager().friendlyMask; }, FriendlyMaskType},
             GroupMask{"getSensorGroupListenMask", "setSensorGroupListenMask",
                       [] { return manager().listenMask; }, -1}}) {
        const std::string getName = std::string("c") + (char)toupper(f.get[0]) + (f.get + 1);
        const std::string setName = std::string("c") + (char)toupper(f.set[0]) + (f.set + 1);
        ts.registerNative(f.get, [arg, f, getName](const Args& args) -> VMValue {
            const int group = validSensorGroup(argInt(arg(args, 0)), getName.c_str());
            if (group == -1) return VMValue(0);
            return VMValue((int32_t)f.masks()[group]);
        });
        ts.registerNative(f.set, [arg, f, setName](const Args& args) -> VMValue {
            const int group = validSensorGroup(argInt(arg(args, 0)), setName.c_str());
            if (group == -1) return VMValue("");
            const uint32_t mask = (uint32_t)argInt(arg(args, 1));
            if (f.type >= 0) updateSensorGroupMask((uint32_t)group, mask, (MaskType)f.type);
            f.masks()[group] = mask;
            return VMValue("");
        });
    }

    ts.registerNative("isTargetFriendly", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cIsTargetFriendly", false);
        if (target == -1) return VMValue(0);
        const int group = validSensorGroup(argInt(arg(args, 1)), "cIsTargetFriendly");
        if (group == -1) return VMValue(0);
        return VMValue(isTargetFriendly(target, (uint32_t)group) ? 1 : 0);
    });
    ts.registerNative("isTargetVisible", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cIsTargetVisible", false);
        if (target == -1) return VMValue(0);
        const int group = validSensorGroup(argInt(arg(args, 1)), "cIsTargetVisible");
        if (group == -1) return VMValue(0);
        return VMValue(isTargetVisible(target, (uint32_t)group) ? 1 : 0);
    });

    ts.registerNative("getSensorGroupCount", [](const Args&) -> VMValue {
        return VMValue((int32_t)manager().sensorGroupCount);
    });
    ts.registerNative("setSensorGroupCount", [arg](const Args& args) -> VMValue {
        const int count = argInt(arg(args, 0));
        if (count < 0 || count > 32) {
            error("TargetManager::cSetSensorGroupCount: invalid group count [%d]", count);
            return VMValue("");
        }
        manager().sensorGroupCount = (uint32_t)count;
        return VMValue("");
    });

    ts.registerNative("setTargetSensorData", [arg](const Args& args) -> VMValue {
        const int target = argInt(arg(args, 0));
        if (target < 0 || target >= MaxTargets) return VMValue("");
        ScriptObject* data = findDataBlock(arg(args, 1).toString(), "SensorData");
        manager().targets[target].sensorData = data ? ScriptEngine::instance().objectId(data) : 0;
        return VMValue("");
    });
    ts.registerNative("getTargetSensorData", [arg](const Args& args) -> VMValue {
        const int target = argInt(arg(args, 0));
        if (target < 0 || target >= MaxTargets) return VMValue(-1);
        const int id = liveDataBlock(manager().targets[target].sensorData, "SensorData");
        return VMValue(id ? id : -1);
    });
    ts.registerNative("getTargetObject", [arg](const Args& args) -> VMValue {
        const int target = argInt(arg(args, 0));
        if (target < 0 || target >= MaxTargets) return VMValue("-1");
        const Target& t = manager().targets[target];
        return VMValue(targetObjectOf(t) ? std::to_string(t.targetObject) : std::string("-1"));
    });

    ts.registerNative("getSensorGroupColor", [arg](const Args& args) -> VMValue {
        const int group = validSensorGroup(argInt(arg(args, 0)), "cGetSensorGroupColor");
        if (group == -1) return VMValue("");
        const int colorGroup = validSensorGroup(argInt(arg(args, 1)), "cGetSensorGroupColor");
        if (colorGroup == -1) return VMValue("");
        const auto c = sensorGroupColor((uint32_t)group, (uint32_t)colorGroup);
        return VMValue(std::to_string(c[0]) + " " + std::to_string(c[1]) + " " + std::to_string(c[2]) + " " +
                       std::to_string(c[3]));
    });
    // setSensorGroupColor(sensorGroup, groupMask, "r g b a"): the colour
    // `sensorGroup` sees each masked group in; its members are told.
    ts.registerNative("setSensorGroupColor", [arg](const Args& args) -> VMValue {
        const int group = validSensorGroup(argInt(arg(args, 0)), "cSetSensorGroupColor");
        if (group == -1) return VMValue("");
        const uint32_t updateMask = (uint32_t)argInt(arg(args, 1));
        int r = 0, g = 0, b = 0, a = 0;
        std::sscanf(arg(args, 2).toString().c_str(), "%d %d %d %d", &r, &g, &b, &a);
        const Color color{(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
        for (int i = 0; i < 32; ++i)
            if (updateMask & (1u << i)) manager().groupColor[group][i] = color;
        for (GameConnection* connection : serverConnections()) {
            if (isAIControlled(*connection) || connectionSensorGroup(*connection) != (uint32_t)group) continue;
            postSensorGroupColors(*connection, (uint32_t)group, updateMask);
        }
        return VMValue("");
    });

    ts.registerNative("getTargetDataBlock", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cGetTargetDataBlock", true);
        if (target == -1) return VMValue(-1);
        return VMValue((int32_t)liveDataBlock(manager().targets[target].shapeBaseData, "ShapeBaseData"));
    });
    ts.registerNative("setTargetDataBlock", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cSetTargetDataBlock", true);
        if (target == -1) return VMValue("");
        ScriptObject* data = findDataBlock(arg(args, 1).toString(), "ShapeBaseData");
        const int id = data ? ScriptEngine::instance().objectId(data) : 0;
        manager().targets[target].shapeBaseData = id;
        TargetInfoFields fields = only(target);
        fields.dataBlockId = id;
        updateTarget(fields);
        return VMValue("");
    });

    ts.registerNative("getTargetRenderMask", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cGetTargetRender", false);
        if (target == -1) return VMValue(-1);
        return VMValue((int32_t)manager().targets[target].renderFlags);
    });
    ts.registerNative("setTargetRenderMask", [arg](const Args& args) -> VMValue {
        const int target = validTarget(argInt(arg(args, 0)), "cSetTargetRender", false);
        if (target == -1) return VMValue("");
        Target& t = manager().targets[target];
        t.renderFlags = (uint32_t)argInt(arg(args, 1)) & ((1u << NumRenderBits) - 1);
        TargetInfoFields fields = only(target);
        fields.renderFlags = (int)t.renderFlags;
        updateTarget(fields);
        return VMValue("");
    });

    // playTargetAudio(target, fileTag, description, update): the target's
    // voice file to every client whose control object can hear it; the
    // position goes along when the target is not ghosted to that client.
    ts.registerNative("playTargetAudio", [arg](const Args& args) -> VMValue {
        ScriptObject* description = findDataBlock(arg(args, 2).toString(), "AudioDescription");
        const int descriptionId = description ? ScriptEngine::instance().objectId(description) : 0;
        if (!description || descriptionId < (int)DataBlockIdFirst || descriptionId > (int)AudioDescriptionIdLast) {
            Console::instance().printf(LogLevel::Warn, "Invalid audio description '%s'.",
                                       arg(args, 2).toString().c_str());
            return VMValue("");
        }
        const bool update = args.size() >= 4 && arg(args, 3).toBool();
        const int target = argInt(arg(args, 0));
        const uint32_t fileTag = argTag(arg(args, 1));
        bool sent = false;
        if (target >= 0 && target < MaxTargets && fileTag > 0 && fileTag < NetStrings::MaxStrings) {
            std::string objectKey;
            if (GameBase* object = targetObjectOf(manager().targets[target], &objectKey)) {
                sent = true;
                const TorqueBitWriter::Point pos{object->transform[3], object->transform[7], object->transform[11]};
                const float maxDistance = Fields::f32(description, "maxDistance", 100.0f);
                for (GameConnection* connection : serverConnections()) {
                    if (isAIControlled(*connection) || connection->controlObject().empty()) continue;
                    auto* control = EngineObjects::get<SceneObject>(connection->controlObject());
                    if (!control) continue;
                    const float dx = control->transform[3] - pos.x, dy = control->transform[7] - pos.y,
                                dz = control->transform[11] - pos.z;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) >= maxDistance) continue;
                    const bool sendPos = connection->ghostIndex(objectKey) == -1;
                    checkString(*connection, fileTag);
                    post(*connection, SimTargetAudioEvent,
                         [target, fileTag, descriptionId, sendPos, pos, update](TorqueBitWriter& w) {
                             w.writeInt(target, TargetIdBitSize);
                             w.writeInt((int32_t)fileTag, AudioFileTagBits);
                             w.writeRangedU32((uint32_t)descriptionId, DataBlockIdFirst, AudioDescriptionIdLast);
                             if (w.writeFlag(sendPos)) w.writeCompressedPoint(pos, 0.5f);
                             w.writeFlag(update);
                         });
                }
            }
        }
        if (!sent) Console::instance().printf(LogLevel::Warn, "Failed to send target audio event to clients");
        return VMValue("");
    });

    // removeClientTargetType(client, "AssignedTask" | "PotentialTask" | "Waypoint")
    ts.registerNative("removeClientTargetType", [arg](const Args& args) -> VMValue {
        const std::string name = arg(args, 1).toString();
        const int type = !strcasecmp(name.c_str(), "AssignedTask")  ? AssignedTask
                         : !strcasecmp(name.c_str(), "PotentialTask") ? PotentialTask
                         : !strcasecmp(name.c_str(), "Waypoint")      ? Waypoint
                                                                      : NumClientTargetTypes;
        if (type >= NumClientTargetTypes) {
            Console::instance().printf(LogLevel::Error, "HUDTargetList::removeClientTargetType: invalid type [%s]",
                                       arg(args, 0).toString().c_str());
            return VMValue("");
        }
        GameConnection* connection = findConnection(arg(args, 0));
        if (!connection || !connection->isServer) {
            Console::instance().printf(LogLevel::Error,
                "HUDTargetList::removeClientTargetType: invalid connection [%s]", arg(args, 0).toString().c_str());
            return VMValue("");
        }
        if (isAIControlled(*connection)) return VMValue("");
        post(*connection, RemoveClientTargetTypeEvent, [type](TorqueBitWriter& w) {
            w.writeRangedU32((uint32_t)type, 0, NumClientTargetTypes);
        });
        return VMValue("");
    });

    // --- GameBase ---------------------------------------------------------------
    ts.registerNative("GameBase::setTarget", [arg](const Args& args) -> VMValue {
        const std::string handle = arg(args, 0).toString();
        auto* object = EngineObjects::get<GameBase>(handle);
        if (!object) return VMValue("");
        const int targetId = argInt(arg(args, 1));
        if (targetId < -1 || targetId >= MaxTargets) {
            Console::instance().printf(LogLevel::Error, "GameBase::cSetTargetId: invalid target id [%s]",
                                       arg(args, 1).toString().c_str());
            return VMValue("");
        }
        if (object->targetId == targetId) return VMValue("");
        object->targetId = targetId;
        // Only non-team targets have objects.
        if (targetId >= 32 && object->script)
            manager().targets[targetId].targetObject = ScriptEngine::instance().objectId(object->script);
        object->setMaskBits(GameBase::ExtendedInfoMask);
        return VMValue("");
    });
    ts.registerNative("GameBase::getTarget", [arg](const Args& args) -> VMValue {
        auto* object = EngineObjects::get<GameBase>(arg(args, 0).toString());
        return VMValue((int32_t)(object ? object->targetId : -1));
    });

    // --- GameConnection -----------------------------------------------------------
    ts.registerNative("GameConnection::setSensorGroup", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        if (connection && connection->isServer) setConnectionSensorGroup(*connection, (uint32_t)argInt(arg(args, 1)));
        return VMValue("");
    });
    ts.registerNative("GameConnection::getSensorGroup", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        return VMValue((int32_t)(connection ? connectionSensorGroup(*connection) : 0));
    });
    ts.registerNative("GameConnection::getReceivedDataBlocks", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        return VMValue(connection && receivedDataBlocks(*connection) ? 1 : 0);
    });
    ts.registerNative("GameConnection::setReceivedDataBlocks", [arg](const Args& args) -> VMValue {
        if (GameConnection* connection = findConnection(arg(args, 0)))
            setReceivedDataBlocks(*connection, arg(args, 1).toBool());
        return VMValue("");
    });
    // The task target (set by the client's SetServerTargetEvent, or by script).
    ts.registerNative("GameConnection::getTargetId", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        if (!connection || !connection->isServer) return VMValue(-1);
        return VMValue((int32_t)stateOf(*connection).targetId);
    });
    ts.registerNative("GameConnection::setTargetId", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        const int targetId = argInt(arg(args, 1));
        if (targetId < -1 || targetId >= MaxTargets) {
            error("GameConnection::cSetTargetId: invalid target id [%d]", targetId);
            return VMValue("");
        }
        if (connection) stateOf(*connection).targetId = targetId;
        return VMValue("");
    });
    ts.registerNative("GameConnection::getTargetPos", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        if (!connection || !connection->isServer) return VMValue("");
        const float* p = stateOf(*connection).targetPos;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%f %f %f", p[0], p[1], p[2]);
        return VMValue(std::string(buf));
    });
    ts.registerNative("GameConnection::setTargetPos", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        if (!connection) return VMValue("");
        float p[3] = {0, 0, 0};
        std::sscanf(arg(args, 1).toString().c_str(), "%f %f %f", &p[0], &p[1], &p[2]);
        std::copy(p, p + 3, stateOf(*connection).targetPos);
        return VMValue("");
    });
    // conn.sendTargetTo(dest, assign): this connection's task target to
    // dest as a TargetToEvent; a target ghosted to dest goes without its
    // position.
    ts.registerNative("GameConnection::sendTargetTo", [arg](const Args& args) -> VMValue {
        GameConnection* connection = findConnection(arg(args, 0));
        GameConnection* dest = findConnection(arg(args, 1));
        if (!connection || !dest || !connection->isServer) return VMValue("");
        const ConnectionState& state = stateOf(*connection);
        const int targetId = state.targetId;
        if (targetId < -1 || targetId >= MaxTargets) return VMValue("");
        const TorqueBitWriter::Point pos{state.targetPos[0], state.targetPos[1], state.targetPos[2]};
        const bool assign = arg(args, 2).toBool();
        post(*dest, TargetToEvent, [dest, targetId, pos, assign](TorqueBitWriter& w) {
            int ghost = -1;
            if (targetId != -1) {
                std::string key;
                if (targetObjectOf(manager().targets[targetId], &key)) ghost = dest->ghostIndex(key);
            }
            if (w.writeFlag(targetId != -1)) w.writeInt(targetId, TargetIdBitSize);
            if (w.writeFlag(ghost == -1)) w.writePoint(pos);
            w.writeFlag(assign);
        });
        return VMValue("");
    });
}
