#pragma once
// ClientTarget and HUDTargetList (game/targetManager.cc), client side: the
// tasks the server hands the client (TargetToEvent) and the waypoints the
// client makes itself, in the HUD target list the commander map and tree
// show. A list entry's id is TargetManager::MaxTargets + its handle.
#include "sim/engine_object.h"
#include <cstdint>
#include <functional>
#include <string>

class TorqueScript;

class ClientTargetObject : public EngineObject {
public:
    enum Type { AssignedTask = 0, PotentialTask, Waypoint, NumTypes };
    int type = -1;
    int targetId = -1;
    float lastTargetPos[3] = {0, 0, 0};
    std::string text;

    void onRemove() override;
    // Being removed by the target list: script gets onDie for tasks.
    void onDie();
    // ClientTarget::process: a task from a TargetToEvent into the list.
    bool process();
};

namespace HUDTargetList {

enum {
    DefaultTimeout = 2000,
    MaxTargets = 32,
    MaxAssignedTasks = 1,
    MaxPotentialTasks = 15,
    MaxWaypoints = 16,
    FreeHandle = -1,
};

struct Entry {
    ClientTargetObject* target = nullptr;
    bool canTimeout = false;
    uint32_t doneTime = 0;
    int handle = 0;
};

class Notify {
public:
    Notify();
    virtual ~Notify();
    virtual void hudTargetAdded(uint32_t) {}
    virtual void hudTargetRemoved(uint32_t) {}
    virtual void hudTargetsCleared() {}
};

bool addTarget(ClientTargetObject* target, bool canTimeout, uint32_t doneTime);
int entryByTarget(const ClientTargetObject* target);
void removeEntryByTarget(const ClientTargetObject* target);
void removeEntry(int entry);
void removeTargetsOfType(uint32_t type);
// mHandle[handle - TargetManager::MaxTargets]: the entry of a list id.
int handleIndex(int id);
uint32_t count();
Entry* entry(uint32_t index);
// TargetManager notifications.
void targetRemoved(uint32_t target);
void targetsCleared();
// Once per client frame: timeouts, and the last position of each target
// visible to the client's sensors (`center` gives its object's world box
// centre).
void update(uint32_t newTime, const std::function<bool(int targetId, float out[3])>& center);

} // namespace HUDTargetList

namespace ClientTargets {
// new ClientTarget(-1, targetId, pos) registered in the server connection.
ClientTargetObject* create(int targetId, const float pos[3]);
// TargetToEvent::process: a task from the server.
void targetTo(int targetId, const float pos[3], bool assign);
// RemoveClientTargetTypeEvent / ResetClientTargetsEvent.
void removeTargetsOfType(uint32_t type);
void reset(bool clientTargetsOnly);
} // namespace ClientTargets

void registerClientTargetNatives(TorqueScript& ts);
