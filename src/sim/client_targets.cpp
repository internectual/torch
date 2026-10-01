// ClientTarget and HUDTargetList (game/targetManager.cc), client side.
#include "sim/client_targets.h"
#include "sim/game_connection.h"
#include "sim/sim_state.h"
#include "core/console.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <strings.h>
#include <vector>

namespace {

constexpr int TargetManagerMaxTargets = 512;
constexpr int TargetIdBitSize = 9;
// SetServerTargetEvent's class index (NetEventClassFirst-relative).
constexpr int SetServerTargetEventClass = 16;

struct List {
    uint32_t numAssignedTasks = 0, numPotentialTasks = 0, numWaypoints = 0;
    HUDTargetList::Entry list[HUDTargetList::MaxTargets];
    uint32_t count = 0;
    int handle[HUDTargetList::MaxTargets];
    std::vector<HUDTargetList::Notify*> notify;
    List() { std::fill(std::begin(handle), std::end(handle), (int)HUDTargetList::FreeHandle); }
};

List& targetList() {
    static List list;
    return list;
}

uint32_t simTimeMs() { return (uint32_t)(SimState::simTime() * 1000.0); }

uint32_t targetTimeout() {
    auto* ts = ScriptEngine::exists() ? ScriptEngine::instance().ts() : nullptr;
    return ts ? (uint32_t)ts->getGlobal("$clientTargetTimeout").toInt() : (uint32_t)HUDTargetList::DefaultTimeout;
}

std::string idOf(const ClientTargetObject& target) {
    return target.script ? std::to_string(ScriptEngine::instance().objectId(target.script)) : std::string();
}

void deleteTarget(ClientTargetObject* target) {
    if (target && target->script) ScriptEngine::instance().deleteScriptObject(idOf(*target));
}

void notifyAdded(uint32_t id) {
    for (auto* n : std::vector<HUDTargetList::Notify*>(targetList().notify)) n->hudTargetAdded(id);
}
void notifyRemoved(uint32_t id) {
    for (auto* n : std::vector<HUDTargetList::Notify*>(targetList().notify)) n->hudTargetRemoved(id);
}
void notifyCleared() {
    for (auto* n : std::vector<HUDTargetList::Notify*>(targetList().notify)) n->hudTargetsCleared();
}

int findOldestEntry(int type) {
    List& l = targetList();
    uint32_t oldTime = 0xffffffffu;
    int oldest = -1;
    for (uint32_t i = 0; i < l.count; ++i)
        if (l.list[i].target->type == type && l.list[i].doneTime < oldTime) {
            oldest = (int)i;
            oldTime = l.list[i].doneTime;
        }
    return oldest;
}

int getAddSlot(const ClientTargetObject* target) {
    List& l = targetList();
    int slot = (int)l.count;
    switch (target->type) {
        case ClientTargetObject::AssignedTask:
            if (l.numAssignedTasks == HUDTargetList::MaxAssignedTasks) slot = findOldestEntry(ClientTargetObject::AssignedTask);
            break;
        case ClientTargetObject::PotentialTask:
            if (l.numPotentialTasks == HUDTargetList::MaxPotentialTasks) slot = findOldestEntry(ClientTargetObject::PotentialTask);
            break;
        case ClientTargetObject::Waypoint:
            if (l.numWaypoints == HUDTargetList::MaxWaypoints) slot = findOldestEntry(ClientTargetObject::Waypoint);
            break;
        default: slot = -1; break;
    }
    return slot;
}

int getFreeHandle() {
    List& l = targetList();
    for (int i = 0; i < HUDTargetList::MaxTargets; ++i)
        if (l.handle[i] == HUDTargetList::FreeHandle) return i;
    return HUDTargetList::MaxTargets;
}

// A new ClientTarget registered and added to the server connection
// (SimObject: no script onAdd).
ClientTargetObject* newClientTarget(int type, int targetId, const float pos[3]) {
    auto& engine = ScriptEngine::instance();
    auto* object = new ScriptObject;
    object->className = "ClientTarget";
    engine.addObject(object);
    object->internals["__added"] = VMValue(1);
    EngineObjects::attach(object);
    auto* target = dynamic_cast<ClientTargetObject*>(object->engine.get());
    if (!target) return nullptr;
    target->type = type;
    target->targetId = targetId;
    std::copy(pos, pos + 3, target->lastTargetPos);
    if (auto* connection = engine.findObject("ServerConnection"); connection && engine.isSimSet(connection))
        engine.addToSet(connection, object);
    return target;
}

ClientTargetObject* clientTarget(const std::vector<VMValue>& args) {
    return args.empty() ? nullptr : EngineObjects::get<ClientTargetObject>(args[0].toString());
}

} // namespace

// --- ClientTarget -----------------------------------------------------------

void ClientTargetObject::onRemove() {
    if (type == Waypoint) ScriptEngine::instance().ts()->callObjectMethod(idOf(*this), "waypointRemoved", {});
    HUDTargetList::removeEntryByTarget(this);
}

void ClientTargetObject::onDie() {
    const char* typeStr = nullptr;
    switch (type) {
        case AssignedTask: typeStr = "AssignedTask"; break;
        case PotentialTask: typeStr = "PotentialTask"; break;
        default: deleteTarget(this); return;
    }
    // The console may take possession of the target.
    ScriptObject* self = script;
    const std::string id = idOf(*this);
    ScriptEngine::instance().ts()->callObjectMethod(id, "onDie", {VMValue(typeStr)});
    if (ScriptEngine::instance().findObject(id.c_str()) != self) return;
    HUDTargetList::removeEntryByTarget(this);
    // Deleted unless the script moved it out of the server connection.
    VMValue groupValue;
    ScriptEngine::instance().ts()->callObjectMethod(id, "getGroup", {}, &groupValue);
    const std::string group = groupValue.toString();
    ScriptObject* connection = ScriptEngine::instance().findObject("ServerConnection");
    ScriptObject* owner = group.empty() || group == "0" ? nullptr : ScriptEngine::instance().findObject(group.c_str());
    if (!owner || owner == connection) deleteTarget(this);
}

bool ClientTargetObject::process() {
    const uint32_t time = simTimeMs() + (type == PotentialTask ? targetTimeout() : 0);
    if (HUDTargetList::addTarget(this, type == PotentialTask, time)) {
        ScriptEngine::instance().ts()->callObjectMethod(idOf(*this), "onAdd",
            {VMValue(type == AssignedTask ? "AssignedTask" : "PotentialTask")});
        return true;
    }
    return false;
}

// --- HUDTargetList ------------------------------------------------------------

namespace HUDTargetList {

Notify::Notify() { targetList().notify.push_back(this); }
Notify::~Notify() {
    auto& n = targetList().notify;
    n.erase(std::remove(n.begin(), n.end(), this), n.end());
}

bool addTarget(ClientTargetObject* target, bool canTimeout, uint32_t doneTime) {
    List& l = targetList();
    const int slot = getAddSlot(target);
    if (slot == -1) return false;
    // Something there?
    if (slot != (int)l.count) l.list[slot].target->onDie();
    const int handle = getFreeHandle();
    if (handle == MaxTargets) return false;
    l.handle[handle] = (int)l.count;
    // Always add at the end.
    l.list[l.count] = {target, canTimeout, doneTime, handle};
    l.count++;
    if (target->type == ClientTargetObject::AssignedTask) l.numAssignedTasks++;
    else if (target->type == ClientTargetObject::PotentialTask) l.numPotentialTasks++;
    else if (target->type == ClientTargetObject::Waypoint) l.numWaypoints++;
    notifyAdded((uint32_t)(handle + TargetManagerMaxTargets));
    return true;
}

int entryByTarget(const ClientTargetObject* target) {
    if (!target) return -1;
    List& l = targetList();
    for (uint32_t i = 0; i < l.count; ++i)
        if (l.list[i].target == target) return (int)i;
    return -1;
}

void removeEntryByTarget(const ClientTargetObject* target) {
    const int index = entryByTarget(target);
    if (index != -1) removeEntry(index);
}

void removeEntry(int index) {
    List& l = targetList();
    if (index < 0 || index >= (int)l.count) return;
    notifyRemoved((uint32_t)(l.list[index].handle + TargetManagerMaxTargets));
    ClientTargetObject* target = l.list[index].target;
    if (target->type == ClientTargetObject::AssignedTask) l.numAssignedTasks--;
    else if (target->type == ClientTargetObject::PotentialTask) l.numPotentialTasks--;
    else if (target->type == ClientTargetObject::Waypoint) l.numWaypoints--;
    // The notification handles follow the moved entry.
    l.count--;
    if (index != (int)l.count) {
        l.handle[l.list[l.count].handle] = index;
        l.handle[l.list[index].handle] = FreeHandle;
        l.list[index] = l.list[l.count];
    } else {
        l.handle[l.list[index].handle] = FreeHandle;
    }
    l.list[l.count].target = nullptr;
}

void removeTargetsOfType(uint32_t type) {
    List& l = targetList();
    // The count drops as entries go (the moved-in entry is not revisited).
    for (uint32_t i = 0; i < l.count; ++i)
        if (l.list[i].target->type == (int)type) removeEntry((int)i);
}

int handleIndex(int id) {
    const int handle = id - TargetManagerMaxTargets;
    if (handle < 0 || handle >= MaxTargets) return FreeHandle;
    return targetList().handle[handle];
}

uint32_t count() { return targetList().count; }

Entry* entry(uint32_t index) { return index < targetList().count ? &targetList().list[index] : nullptr; }

void targetRemoved(uint32_t target) {
    // Several HUD targets may map to this target.
    List& l = targetList();
    for (int i = (int)l.count - 1; i >= 0; --i)
        if (i < (int)l.count && l.list[i].target->targetId == (int)target) deleteTarget(l.list[i].target);
}

void targetsCleared() {
    List& l = targetList();
    for (int i = (int)l.count - 1; i >= 0; --i)
        if (i < (int)l.count) deleteTarget(l.list[i].target);
    notifyCleared();
}

void update(uint32_t newTime, const std::function<bool(int, float[3])>& center) {
    List& l = targetList();
    for (int i = (int)l.count - 1; i >= 0; --i) {
        if (i >= (int)l.count) continue;
        Entry& e = l.list[i];
        if (e.canTimeout && e.doneTime < newTime) {
            e.target->onDie();
            continue;
        }
        // Location targets: the position is already known.
        if (e.target->targetId == -1) continue;
        if (center) center(e.target->targetId, e.target->lastTargetPos);
    }
}

} // namespace HUDTargetList

// --- Events -------------------------------------------------------------------

namespace ClientTargets {

ClientTargetObject* create(int targetId, const float pos[3]) {
    return ScriptEngine::exists() ? newClientTarget(-1, targetId, pos) : nullptr;
}

void targetTo(int targetId, const float pos[3], bool assign) {
    if (!ScriptEngine::exists()) return;
    ClientTargetObject* target = newClientTarget(-1, targetId, pos);
    if (!target) return;
    target->type = assign ? ClientTargetObject::AssignedTask : ClientTargetObject::PotentialTask;
    target->process();
}

void removeTargetsOfType(uint32_t type) {
    if (type < ClientTargetObject::NumTypes) HUDTargetList::removeTargetsOfType(type);
}

void reset(bool) {
    // Both a full reset (TargetManager::resetClient notifies the list) and a
    // tasks-only reset clear the HUD list.
    HUDTargetList::targetsCleared();
}

} // namespace ClientTargets

// --- Console --------------------------------------------------------------------

void registerClientTargetNatives(TorqueScript& ts) {
    EngineObjects::registerClass("ClientTarget", [] { return std::make_shared<ClientTargetObject>(); });
    ts.setGlobal("$clientTargetTimeout", VMValue((int32_t)HUDTargetList::DefaultTimeout));

    // target.sendToServer(): GameConnection::sendTargetToServer.
    ts.registerNative("ClientTarget::sendToServer", [](const std::vector<VMValue>& args) -> VMValue {
        auto* target = clientTarget(args);
        auto* connection = EngineObjects::get<GameConnection>("ServerConnection");
        if (!target || !connection || connection->isServer) return VMValue("");
        const int targetId = target->targetId;
        const float pos[3] = {target->lastTargetPos[0], target->lastTargetPos[1], target->lastTargetPos[2]};
        auto event = std::make_shared<NetEventOut>();
        event->classIndex = SetServerTargetEventClass;
        event->pack = [targetId, pos](TorqueBitWriter& w) {
            if (w.writeFlag(targetId != -1)) w.writeInt(targetId, TargetIdBitSize);
            w.writeF32(pos[0]);
            w.writeF32(pos[1]);
            w.writeF32(pos[2]);
        };
        connection->postEvent(event);
        return VMValue("");
    });
    // Waypoints are made by the client, from untyped targets only.
    ts.registerNative("ClientTarget::createWaypoint", [](const std::vector<VMValue>& args) -> VMValue {
        auto* target = clientTarget(args);
        if (!target || args.size() < 2) return VMValue("");
        if (target->type != -1) {
            Console::instance().printf(LogLevel::Error, "ClientTarget::cCreateWaypoint: target already typed");
            return VMValue("");
        }
        target->text = args[1].toString();
        target->type = ClientTargetObject::Waypoint;
        if (!HUDTargetList::addTarget(target, false, simTimeMs()))
            Console::instance().printf(LogLevel::Error, "ClientTarget::cCreateWaypoint: unable to create waypoint");
        return VMValue("");
    });
    ts.registerNative("ClientTarget::addPotentialTask", [](const std::vector<VMValue>& args) -> VMValue {
        auto* target = clientTarget(args);
        if (!target || target->type != ClientTargetObject::PotentialTask) return VMValue("");
        // May already be in the list.
        if (HUDTargetList::entryByTarget(target) != -1) return VMValue("");
        if (!HUDTargetList::addTarget(target, true, simTimeMs() + targetTimeout()))
            Console::instance().printf(LogLevel::Error, "ClientTarget::cAddPotentialTask: unable to add task");
        return VMValue("");
    });
    ts.registerNative("ClientTarget::setText", [](const std::vector<VMValue>& args) -> VMValue {
        if (auto* target = clientTarget(args); target && args.size() > 1) target->text = args[1].toString();
        return VMValue("");
    });
    ts.registerNative("ClientTarget::getTargetId", [](const std::vector<VMValue>& args) -> VMValue {
        auto* target = clientTarget(args);
        const int index = HUDTargetList::entryByTarget(target);
        if (index == -1) return VMValue(-1);
        return VMValue((int32_t)(TargetManagerMaxTargets + HUDTargetList::entry((uint32_t)index)->handle));
    });
    // createClientTarget(targetId, <x y z>): an untyped target in the server
    // connection.
    ts.registerNative("createClientTarget", [](const std::vector<VMValue>& args) -> VMValue {
        auto* connection = EngineObjects::get<GameConnection>("ServerConnection");
        if (!connection || connection->isServer || args.empty()) return VMValue(-1);
        float pos[3] = {0, 0, 0};
        if (args.size() > 1) std::sscanf(args[1].toString().c_str(), "%f %f %f", &pos[0], &pos[1], &pos[2]);
        ClientTargetObject* target = newClientTarget(-1, std::atoi(args[0].toString().c_str()), pos);
        if (!target) return VMValue(-1);
        return VMValue((int32_t)ScriptEngine::instance().objectId(target->script));
    });
}
