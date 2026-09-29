#pragma once
// game/targetManager.cc (server half): the target table (name, skin, voice,
// type tags, sensor group, ShapeBaseData, render flags and voice pitch per
// target id), the sensor-group masks and colours, and the events that keep
// every client's copy current (TargetInfoEvent, TargetFreeEvent,
// ResetClientTargetsEvent, SensorGroupColorEvent, SimTargetAudioEvent,
// RemoveClientTargetTypeEvent). Also the connection's target state from
// game/gameConnection.cc: its sensor group (SetSensorGroupEvent), the
// retail received-datablocks flag, and its task target (TargetToEvent);
// and GameBase::setTarget / getTarget.
//
// Target events are posted as the table changes, like the engine does:
// no per-packet hook is needed. Targets are sent only to connections whose
// datablocks have arrived (%client.setReceivedDataBlocks(true) in
// GameConnection::dataBlocksDone, then sendTargetsToClient).
#include <array>
#include <cstdint>
#include <string>

class GameConnection;
class TorqueScript;

namespace ServerTargets {

enum {
    MaxTargets = 512,
    TargetIdBitSize = 9,
    TargetFreeMaskSize = MaxTargets >> 5,
    // TargetInfo render flags.
    HudRenderStart = 0,
    NumHudRenderImages = 8,
    CommanderListRender = 1 << NumHudRenderImages,
    NumRenderBits = NumHudRenderImages + 1,
};

// Engine event class indices (NetEventClassFirst-relative).
enum EventClass {
    RemoveClientTargetTypeEvent = 10,
    ResetClientTargetsEvent = 11,
    SensorGroupColorEvent = 12,
    SetSensorGroupEvent = 15,
    SimTargetAudioEvent = 20,
    TargetFreeEvent = 23,
    TargetInfoEvent = 24,
    TargetToEvent = 25,
};

// TargetInfo, server fields.
struct Target {
    uint32_t nameTag = 0, skinTag = 0, skinPrefTag = 0, voiceTag = 0, typeTag = 0;
    float voicePitch = 1.0f;
    uint32_t sensorGroup = 0;
    uint32_t renderFlags = 0;
    bool allocated = false;
    int targetObject = 0;  // SimObject id of the GameBase using it (targets >= 32), 0 = none
    int shapeBaseData = 0; // ShapeBaseData datablock id, 0 = none
    uint32_t sensorVisMask = 0, sensorAlwaysVisMask = 0, sensorNeverVisMask = 0, sensorFriendlyMask = 0;
    uint32_t sensorFlags = 0;
    int sensorData = 0; // SensorData datablock id, 0 = none
    void clear(bool clearFlags = true);
};

// TargetManager::clear: every target free, team targets 0..31 reserved,
// default sensor-group masks and colours.
void clear();
// TargetManager::reset (resetTargets()): every client clears its targets.
void reset();
// TargetManager::newClient (sendTargetsToClient()).
void newClient(GameConnection& connection);
int allocTarget(uint32_t nameTag, uint32_t skinTag, uint32_t voiceTag, uint32_t typeTag, uint32_t sensorGroup,
                uint32_t dataBlockId, float voicePitch, uint32_t prefSkin);
void freeTarget(int target);
// A target by id (0..511), or nullptr when out of range.
const Target* serverTarget(int target);
bool isTargetVisible(int target, uint32_t sensorGroup);
bool isTargetFriendly(int target, uint32_t sensorGroup);
uint32_t sensorGroupListenMask(uint32_t sensorGroup);
// RGBA of colour group `colorGroup` as seen by `sensorGroup`.
std::array<uint8_t, 4> sensorGroupColor(uint32_t sensorGroup, uint32_t colorGroup);

// GameConnection target state (gameConnection.cc).
uint32_t connectionSensorGroup(const GameConnection& connection);
void setConnectionSensorGroup(GameConnection& connection, uint32_t group);
bool receivedDataBlocks(const GameConnection& connection);
void setReceivedDataBlocks(GameConnection& connection, bool received);

} // namespace ServerTargets

void registerTargetManagerNatives(TorqueScript& ts);
