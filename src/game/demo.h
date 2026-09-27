#pragma once
#include "game/shape_lighting.h"
#include "game/weapon_image_state.h"
#include "game/vehicle_jets.h"
#include "game/flare_spikes.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <limits>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <cctype>

#include "core/math.h"
#include "net/v12_datablocks.h"

// ─── Vec3 / Quat helpers ───────────────────────────────────────
struct Vec3 { float x{}, y{}, z{}; };
struct Vec4 { float x{}, y{}, z{}, w{}; };

// ─── BitStream ──────────────────────────────────────────────────
// Bit-level reader, ported from Torque/V12 BitStream (LE, LSB-first)
class BitStream {
public:
    BitStream(const uint8_t* data, size_t size, size_t bitOffset = 0);

    bool     readFlag();
    int      readInt(int bitCount);
    int      readSignedInt(int bitCount);
    float    readFloat(int bitCount);
    float    readSignedFloat(int bitCount);
    int      readRangedU32(int rangeStart, int rangeEnd);
    uint8_t  readU8();
    uint16_t readU16();
    uint32_t readU32();
    int32_t  readS32();
    float    readF32();
    bool     readBool();
    Vec3     readPoint3F();
    Vec3     readNormalVector(int bitCount);
    Vec3     readCompressedPoint(const Vec3& compressionPoint, float scale = 0.01f);
    struct AffineTransform {
        Vec3 position;
        Vec4 rotation; // quaternion
    };
    AffineTransform readAffineTransform(const Vec3& cp = Vec3{});
    int*       readMatrixF(Vec3* outPos = nullptr); // returns elements[16] (internal)
    std::string readString();
    std::string readRawString();
    std::string unpackNetString();
    void setStringBufferEnabled(bool en) { stringBufferEnabled = en; if (!en) stringBuffer.clear(); }

    int  getCurPos() const { return bitNum; }
    void setCurPos(int pos) {
        if (pos < 0 || pos > maxReadBitNum) { error = true; return; }
        bitNum = pos;
    }
    int  getBytePosition() const { return (bitNum + 7) >> 3; }
    void fail() { error = true; }
    bool isError() const { return error; }
    int  getRemainingBits() const { return maxReadBitNum - bitNum; }
    int  getMaxPos() const { return maxReadBitNum; }
    int  savePos() const { return bitNum; }
    void restorePos(int pos) {
        if (pos < 0 || pos > maxReadBitNum) { error = true; return; }
        bitNum = pos;
        error = false;
    }
    void skipBits(int count) {
        if (count < 0 || count > maxReadBitNum - bitNum) { error = true; return; }
        bitNum += count;
    }
    const uint8_t* getBuffer() const { return data; }
    size_t getBufferSize() const { return dataLen; }

private:
    const uint8_t* data;
    size_t dataLen;
    int bitNum;
    int maxReadBitNum;
    bool error;
    std::string stringBuffer;
    bool stringBufferEnabled = false;

    struct HuffNode { int pop, index0, index1; };
    struct HuffLeaf { int pop, symbol, numBits, code; };
    static HuffNode s_huffNodes[512];
    static HuffLeaf s_huffLeaves[256];
    static int s_huffNodeCount;
    static bool s_huffBuilt;
    static void buildHuffmanTables();
    std::string readHuffBuffer();
};

// ─── T2 Protocol Constants ──────────────────────────────────────
namespace T2Demo {
    constexpr int MaxGhostCount = 1024;
    constexpr int GhostIdBitSize = 10;
    constexpr int NetEventClassBitSize = 6;
    constexpr int NetEventClassFirst = 255;
    constexpr int NetEventClassCount = 26;
    inline static const char* NetEventClassNames[NetEventClassCount] __attribute__((unused)) = {
        "CRCChallengeEvent",       // 0
        "CRCChallengeResponseEvent",// 1
        "FogChallengeEvent",       // 2
        "GhostAlwaysObjectEvent",   // 3
        "GhostingMessageEvent",    // 4
        "GravityEvent",            // 5
        "LightningStrikeEvent",    // 6
        "NetStringEvent",          // 7
        "PathManagerEvent",        // 8
        "RemoteCommandEvent",      // 9
        "RemoveClientTargetTypeEvent", // 10
        "ResetClientTargetsEvent", // 11
        "SensorGroupColorEvent",   // 12
        "SetMissionCRCEvent",      // 13
        "SetObjectActiveImageEvent",// 14
        "SetSensorGroupEvent",     // 15
        "SetServerTargetEvent",    // 16
        "Sim2DAudioEvent",         // 17
        "Sim3DAudioEvent",         // 18
        "SimDataBlockEvent",       // 19
        "SimTargetAudioEvent",     // 20
        "SimVoiceStreamEvent",     // 21
        "SimpleMessageEvent",      // 22
        "TargetFreeEvent",         // 23
        "TargetInfoEvent",         // 24
        "TargetToEvent",           // 25
    };
    constexpr int NetObjectClassBitSize = 7;
    constexpr int NetObjectClassFirst = 0;
    constexpr int MaxTriggerKeys = 6;
    constexpr int DataBlockClassFirst = 128;
    constexpr int SimDBEventObjectIdBits = 11;
    constexpr int SimDBEventClassIdBits = 7;
    constexpr int SimDBEventIndexBits = 11;
    constexpr int SimDBEventTotalBits = 12;

    constexpr int BlockTypePacket = 0;
    constexpr int BlockTypeSendPacket = 1;
    constexpr int BlockTypeMove = 2;
    constexpr int BlockTypeInfo = 3;

    // Each Move block is one fixed 32 ms simulation tick; packets between
    // Move blocks belong to the tick they follow. ticksBefore[i] is the number
    // of Move blocks preceding block i (size = block count + 1).
    constexpr double TickSeconds = 0.032;
    inline int playbackTargetTick(float time) {
        if (!std::isfinite(time) || time <= 0.0f) return 0;
        // Round away float error so 0.032 * n maps back to tick n.
        const double tick = std::floor((double)time / TickSeconds + 1e-6);
        return tick >= (double)std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max() : (int)tick;
    }
    // Blocks to process to reach `time`: through the Move block that
    // completes the target tick, as a tick-stepped playback would.
    inline int playbackTargetBlock(float time, const std::vector<int>& ticksBefore) {
        if (ticksBefore.size() < 2) return 0;
        const int tick = playbackTargetTick(time);
        const auto it = std::lower_bound(ticksBefore.begin(), ticksBefore.end(), tick);
        return std::min((int)(it - ticksBefore.begin()), (int)ticksBefore.size() - 1);
    }
    inline float playbackBlockTime(int blockIndex, const std::vector<int>& ticksBefore) {
        if (ticksBefore.empty()) return 0.0f;
        const int index = std::clamp(blockIndex, 0, (int)ticksBefore.size() - 1);
        return (float)(ticksBefore[index] * TickSeconds);
    }
    inline float playbackProgress(float time, float totalTime) {
        if (!std::isfinite(time) || !std::isfinite(totalTime) || totalTime <= 0.0f)
            return 0.0f;
        return std::clamp(time / totalTime, 0.0f, 1.0f);
    }

    // A failed replacement remains pending until the asset becomes available.
    // Keeping this state separate from the parser's timeline prevents a
    // missing mission from being mistaken for the successfully loaded scene.
    struct MissionReplacementState {
        std::string loadedMission;
        std::string pendingMission;

        static bool sameMission(const std::string& left, const std::string& right) {
            if (left.size() != right.size()) return false;
            for (size_t i = 0; i < left.size(); ++i)
                if (std::tolower((unsigned char)left[i]) !=
                    std::tolower((unsigned char)right[i])) return false;
            return true;
        }

        bool defer(const std::string& mission) {
            if (mission.empty() || sameMission(mission, loadedMission)) {
                pendingMission.clear();
                return false;
            }
            pendingMission = mission;
            return true;
        }

        bool commit(const std::string& mission) {
            if (mission.empty()) return false;
            if (sameMission(mission, loadedMission) && pendingMission.empty()) return true;
            if (pendingMission.empty() || !sameMission(pendingMission, mission)) return false;
            loadedMission = mission;
            pendingMission.clear();
            return true;
        }
    };

    inline std::string formatPlaybackClock(float time) {
        const int totalSeconds = std::max(0, (int)std::floor(
            std::isfinite(time) ? time : 0.0f));
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds / 60) % 60;
        const int seconds = totalSeconds % 60;
        char text[32];
        if (hours > 0)
            std::snprintf(text, sizeof(text), "%d:%02d:%02d", hours, minutes, seconds);
        else
            std::snprintf(text, sizeof(text), "%02d:%02d", minutes, seconds);
        return text;
    }

    // V12 stores horizontal FOV; the renderer projection takes vertical FOV.
    inline float horizontalFovToVertical(float horizontalDeg, float aspect) {
        return Math::horizontalFovToVertical(horizontalDeg, aspect);
    }

    // Camera::setPosition builds zRot(yaw) * xRot(pitch); its view direction
    // is the transform's local +Y column.
    // Torque view forward (column 1 of rotZ(yaw) * rotX(pitch)); positive
    // pitch looks down.
    inline Vec3 cameraDirectionFromYawPitch(float yaw, float pitch) {
        const float cp = std::cos(pitch);
        return {std::sin(yaw) * cp, std::cos(yaw) * cp, -std::sin(pitch)};
    }
    // Player::updateMove: yaw wraps to [0, 2pi); pitch clamps to the
    // client view limit (t2-mapper MAX_PITCH = 0.494 pi).
    inline void accumulateViewMove(float& yaw, float& pitch, float moveYaw, float movePitch) {
        constexpr float TwoPi = 6.28318530718f, MaxPitch = 3.14159265359f * 0.494f;
        yaw = std::fmod(yaw + moveYaw, TwoPi);
        if (yaw < 0.0f) yaw += TwoPi;
        pitch = std::clamp(pitch + movePitch, -MaxPitch, MaxPitch);
    }

    // Stable replacement for the V12 CameraShake sine channels. The seed is
    // part of the effect state, so replay and seek produce identical frames.
    inline Vec3 cameraShakeOffset(float elapsed, const Vec3& amplitude,
                                  const Vec3& frequency, const Vec3& phase) {
        constexpr float twoPi = 6.28318530717958647692f;
        return {amplitude.x * std::sin(twoPi * (phase.x + elapsed) * frequency.x),
                amplitude.y * std::sin(twoPi * (phase.y + elapsed) * frequency.y),
                amplitude.z * std::sin(twoPi * (phase.z + elapsed) * frequency.z)};
    }

    // Protocol versions
    constexpr uint32_t ProtocolV24834 = 0x00300004;
    constexpr uint32_t ProtocolV25034 = 0x00330004;

    // Tagged strings
    constexpr int TaggedStringCount = 1024;

    // Deterministic ghost class names for T2 (sorted by strcmp order,
    // matching AbstractClassRep::initialize). Index = classId.
    // Includes base classes + known extensions from the V12 engine.
    // Derived from t2-demo-parser + T2 engine source analysis.
    inline const char* const NetObjectClassNames[] = {
        "AIObjective",              // 0
        "AudioEmitter",             // 1
        "BeaconObject",             // 2
        "BombProjectile",           // 3
        "BombSight",                // 4
        "Camera",                   // 5
        "Debris",                   // 6
        "DecalManager",             // 7
        "ELFProjectile",            // 8
        "EnergyProjectile",         // 9
        "Explosion",                // 10
        "FireballAtmosphere",       // 11
        "FlareProjectile",          // 12
        "FlyingVehicle",            // 13
        "ForceFieldBare",           // 14
        "GameBase",                 // 15
        "GrenadeProjectile",        // 16
        "HoverVehicle",             // 17
        "InteriorInstance",         // 18
        "InteriorSubObject",        // 19
        "Item",                     // 20
        "Lightning",                // 21
        "LinearFlareProjectile",    // 22
        "LinearProjectile",         // 23
        "Marker",                   // 24
        "MirrorSubObject",          // 25
        "MissionArea",              // 26
        "MissionMarker",            // 27
        "ParticleEmissionDummy",    // 28
        "ParticleEmitter",          // 29
        "PhysicalZone",             // 30
        "Player",                   // 31
        "Precipitation",            // 32
        "Projectile",               // 33
        "RepairProjectile",         // 34
        "SceneRoot",                // 35
        "ScopeAlwaysShape",         // 36
        "SeekerProjectile",         // 37
        "ShapeBase",                // 38
        "ShockLanceElectricity",    // 39
        "ShockLanceProjectile",     // 40
        "Shockwave",                // 41
        "ShowTSShape",              // 42
        "SimpleNetObject",          // 43
        "Sky",                      // 44
        "SniperProjectile",         // 45
        "SpawnSphere",              // 46
        "Splash",                   // 47
        "StaticShape",              // 48
        "StationFXPersonal",        // 49
        "StationFXVehicle",         // 50
        "Sun",                      // 51
        "TSStatic",                 // 52
        "TargetProjectile",         // 53
        "TerrainBlock",             // 54
        "TracerProjectile",         // 55
        "Trigger",                  // 56
        "Turret",                   // 57
        "Vehicle",                  // 58
        "VehicleBlocker",           // 59
        "WaterBlock",               // 60
        "WayPoint",                 // 61
        "WheeledVehicle",           // 62
    };
    constexpr int NetObjectClassCount = sizeof(NetObjectClassNames) / sizeof(NetObjectClassNames[0]);
}

// ─── Demo Data Structures ──────────────────────────────────────
struct DemoHeader {
    std::string identString;      // "Tribes2 Recording"
    uint32_t protocolVersion{};
    uint32_t demoLengthMs{};      // total recording duration in ms
    uint32_t initialBlockSize{};
};

struct DemoMove {
    int32_t px, py, pz;
    uint32_t pyaw, ppitch, proll;
    float x, y, z;
    float yaw, pitch, roll;
    uint32_t id;
    uint32_t sendCount;
    bool freeLook;
    bool trigger[6];
};

// Zero is a valid authored camera orientation. Only malformed float payloads
// should be ignored when applying a recorded view direction.
inline bool demoMoveOrientationValid(float yaw, float pitch) {
    return std::isfinite(yaw) && std::isfinite(pitch);
}

struct InfoBlock {
    uint32_t value1;
    float value2;
};

struct DataBlockHeader {
    uint32_t objectId, classId, index, total;
    int dataBitsStart;
};

struct ParsedDataBlock {
    uint32_t classId;
    std::string className;
    uint32_t objectId;
    std::map<std::string, std::string> data; // simple key-value for debug
    V12::DecodedDataBlock decoded;
};

// Move::unpack layout: the connection's queued client moves at record time.
struct QueuedMove {
    uint32_t pyaw{}, ppitch{}, proll{};
    uint32_t px{}, py{}, pz{};
    bool freeLook{};
    bool trigger[6]{};
};

// TargetManager slot state, keyed by target id (not ghost index).
struct DemoTargetState {
    std::string name, skin, type;
    int sensorGroup = -1;
    int renderFlags = 0;
    bool hasRenderFlags = false;
};

struct TargetEntry {
    int targetId{};
    float sensorData{};
    float voiceMapData{};
    std::string name, skin, skinPref, voice, typeDescription;
    int sensorGroup{}, targetData{};
    int dataBlockRef{ -1 };
    float damageLevel{};
};

struct ConnectionProtocolState {
    uint32_t lastSeqRecvdAtSend[32]{};
    uint32_t lastSeqRecvd{}, highestAckedSeq{}, lastSendSeq{};
    uint32_t ackMask{}, connectSequence{}, lastRecvAckAck{};
    bool connectionEstablished{};
};

struct DnetHeader {
    bool gameFlag{};
    int connectSeqBit{}, seqNumber{}, highestAck{};
    int packetType{}, ackByteCount{};
    uint64_t ackMask{};
};

struct GameState {
    uint32_t lastMoveAck{};
    float damageFlash{ -1 };
    float whiteOut{ -1 };
    bool hasDamageFlash{};
    bool hasWhiteOut{};
    bool selfLocked{}, selfHomed{};
    bool seekerTracking{};
    int seekerMode{}, seekerObjectGhostIndex{ -1 };
    Vec3 targetPos;
    Vec3 seekerTrackingPos;
    bool pinged{}, jammed{};
    int controlObjectGhostIndex{ -1 };
    bool controlObjectDirty{};
    float energy{}, rechargeRate{};
    Vec3 compressionPoint;
    std::vector<std::pair<int, int>> targetVisibility;
    float cameraFov{ -1 };
    Vec3 cameraPosition{};
    float cameraPitch{};
    float cameraYaw{};
    bool hasCameraTransform{};
    int cameraMode{ -1 };
    int orbitObjectGhostIndex{ -1 };
    float orbitMinDistance{}, orbitMaxDistance{}, orbitDistance{};
    // Player::readPacketData view: mHead.x (pitch), mHead.z, mRot.z (yaw).
    bool hasControlRotation{};
    float controlHeadX{}, controlHeadZ{}, controlRotZ{};
};

struct GhostUpdate {
    int index{};
    enum Type { Create, Update, Delete } type{};
    int classId{};
    int updateBitsStart{}, updateBitsEnd{};
};

struct NetEventInfo {
    int classId{};
    bool guaranteed{};
    int sequenceNumber{};
    int dataBitsStart{}, dataBitsEnd{};
    std::string message;    // parsed text for chat/server messages
    std::vector<std::string> arguments; // decoded remote-command arguments
    std::string eventName;  // class name for display
    int audioProfileId = -1; // for audio events
    bool directAudioProfile = false;
    int targetId = -1;
    Vec3 audioPosition{};
    bool hasAudioPosition = false;
    bool hasTargetInfo = false;
    bool hasTargetFree = false;
    bool hasMissionCrc = false;
    std::string targetName, targetSkin, targetSkinPreference;
    std::string targetVoice, targetType;
    int targetSensorGroup = 0;
    int targetDataBlockId = -2;
    int targetRenderFlags = 0;
    bool hasTargetRenderFlags = false;
    float targetVoicePitch = 1.0f;
    uint32_t missionCrc = 0;
};

// Stable identity for one audio event occurrence. Block and ordinal distinguish
// legitimate repeated sounds while the payload fields suppress parser repeats.
inline uint64_t demoAudioEventKey(int blockIndex, int eventIndex,
                                  const NetEventInfo& event) {
    uint64_t key = static_cast<uint32_t>(blockIndex);
    key = (key << 16) ^ static_cast<uint16_t>(eventIndex);
    key = (key << 16) ^ static_cast<uint16_t>(event.classId);
    key = (key << 11) ^ static_cast<uint16_t>(event.audioProfileId + 1);
    if (event.hasAudioPosition) {
        key ^= static_cast<uint32_t>(event.audioPosition.x * 16.0f);
        key = (key << 7) ^ static_cast<uint32_t>(event.audioPosition.y * 16.0f);
        key = (key << 7) ^ static_cast<uint32_t>(event.audioPosition.z * 16.0f);
    }
    return key;
}

struct DemoTimedEvent {
    double time{};       // seconds into demo
    std::string text;    // display text
    int type{};          // 0=chat, 1=server, 2=system
    int ghostIndex{-1};  // source ghost (player), -1 if server
};

inline bool demoEventVisibleAt(const DemoTimedEvent& event, float playbackTime) {
    return event.time <= (double)playbackTime;
}

struct PacketData {
    DnetHeader dnetHeader;
    GameState gameState;
    std::vector<NetEventInfo> events;
    std::vector<GhostUpdate> ghosts;
};

struct InitialBlockData {
    std::map<int, std::string> taggedStrings;
    std::vector<DataBlockHeader> dataBlockHeaders;
    int dataBlockCount{};
    std::map<uint32_t, ParsedDataBlock> dataBlocks;
    bool firstPerson{};
    // The control object's view from the initial control packet.
    bool hasControlRotation{};
    float controlYaw{}, controlPitch{};
    std::vector<uint32_t> connectionFields;
    std::vector<uint32_t> stateArray;
    std::vector<QueuedMove> queuedMoves;
    std::vector<std::string> demoValues;
    std::vector<TargetEntry> targetEntries;
    ConnectionProtocolState connectionState;
    float roundTripTime{}, packetLoss{};
    uint32_t notifyCount{}, nextRecvEventSeq{}, ghostingSequence{};
    std::vector<GhostUpdate> initialGhosts;
    std::vector<NetEventInfo> initialEvents;
    int controlObjectGhostIndex{ -1 };
    std::string missionName;
    uint32_t missionCRC{};
    bool phase2Valid{};
    std::map<uint32_t, std::string> datablockWeaponShapes; // datablock index → weapon shape path

    // Match and mission metadata parsed from demoValues (matching t2-mapper)
    std::string missionDisplayName;
    std::string missionTypeDisplayName;
    std::string gameClassName;
    std::string serverDisplayName;
    std::string modName;
    std::string recorderName;
    int recorderClientId = -1;
    std::string recordingDate;
};

// ─── HUD State (ported from t2-mapper StreamEngine) ───────────────
struct WeaponsHudState {
    std::map<int, int> slots; // slot index -> ammo (-1 = none/infinite)
    std::map<int, std::string> bitmaps;
    std::string backgroundBitmap;
    std::string highlightBitmap;
    std::string infiniteAmmoBitmap;
    int activeIndex = -1;
};

struct BackpackHudState {
    int packIndex = -1;
    bool active = false;
    std::string text;
    std::string bitmap;
};

struct InventoryHudState {
    std::map<int, int> slots; // slot index -> amount
    std::map<int, std::string> bitmaps;
    std::string backgroundBitmap;
};

struct VehicleHudState {
    bool dashboardVisible = false;
    int activeWeapon = -1;
    std::string vehicleType;
    int node = -1;
};

struct AmmoHudState {
    int count = -1;
};

struct PathManagerRecord {
    uint32_t field0{}, field1{}, field2{}, auxField{};
};

struct PathManagerEntry {
    uint32_t entryId{};
    std::vector<PathManagerRecord> records;
};

// ─── Ghost Tracker ──────────────────────────────────────────────
struct DTSShape; // forward decl

struct GhostEntry {
    struct ThreadState {
        int sequence = -1;
        int state = 0;
        float timescale = 1.0f;
        float position = 0.0f;
        bool forward = true;
        bool atEnd = false;
        bool valid = false;
    };
    struct SoundThreadState {
        int profileId = -1;
        bool playing = false;
        bool valid = false;
    };
    int classId{};
    std::string className;
    Vec3 position{};
    bool hasPosition{};
    Vec3 velocity{};
    Vec3 linearMomentum{};
    Vec3 renderPos{};
    Vec4 rotation{};
    Vec3 cameraEuler{};
    Vec4 renderRotation{};
    bool hasRotation{};
    bool hasCameraEuler{};
    bool hasVelocity{};
    bool hasSteering{};
    bool frozen{};
    Vec3 beamStart{};
    Vec3 beamEnd{};
    bool hasBeam{};
    bool hasLinearMomentum{};
    int datablockId = -1;
    bool hasDatablock = false;
    Vec3 projectileScale{1.0f, 1.0f, 1.0f};
    bool hasProjectileScale = false;
    std::string skinName;
    std::string shapePath;
    DTSShape* shape{};
    Vec3 prevPosition{};
    float animTime{};
    float threadAnimTime{};
    bool isMoving{};
    float moveYaw{};
    bool hasRendered{};
    bool skinApplied{};
    float health{100.0f};
    float maxHealth{100.0f};
    float steeringYaw{};
    int damageState = 0; // 0 enabled, 1 disabled, 2 destroyed
    float energy{100.0f};
    int32_t kills{};
    int32_t deaths{};
    int32_t score{};
    std::string playerName;
    int teamId{-1};
    int sensorGroup{-1};
    std::string targetType;
    int targetRenderFlags = 0;
    bool isFlag = false;
    int flagTeamId = 0;
    // GameBase TargetMask: this object's TargetManager slot, or -1.
    int targetId = -1;
    // Projectile has exploded; it stays hidden until the ghost is deleted.
    bool exploded = false;
    std::string shapeName; // from datablock
    int linkSourceGhost = -1;
    int linkTargetGhost = -1;
    int linkSourceSlot = -1;
    ThreadState threads[4]{};
    SoundThreadState soundThreads[4]{};

    // Mounted image (weapon) slots
    struct MountedImage {
        int16_t datablockId = -1;
        int mountPoint = 0;
        std::string shapePath;
        bool loaded = false;
        bool isFiring = false;
        // Networked image conditions (ShapeBase ImageMask).
        bool triggerDown = false, ammo = false, wet = false, target = false;
        int fireCount = 0;
        bool forceFire = false; // initial update's firing bit
        // Client-side state machine, rebuilt when the datablock changes.
        WeaponImage::Animation animation;
        int16_t animationDatablock = -1;
    };
    struct WheelState {
        float angularVelocity = 0.0f;
        float suspension = 0.0f;
        float lateral = 0.0f;
        bool valid = false;
    };
    MountedImage mountedImages[8]{};
    WheelState wheels[6]{};
    float wheelRotation[6]{};

    // Turret barrel aiming
    float barrelPitch = 0.0f;
    float barrelYaw = 0.0f;
    bool hasTurretAim = false;
    // Turret::unpackUpdate: phi and theta normalized to [0, 1], activation.
    float turretPhi = 0.0f, turretTheta = 0.0f, turretActivation = 0.0f;

    // Cloak state
    bool cloaked = false;
    bool hasCloak = false;

    // Shield state
    float shieldLevel = 0.0f;  // 0-1 normalized shield strength
    bool hasShield = false;

    // Head rotation (aim direction)
    float headPitch = 0.0f;
    float headYaw = 0.0f;
    // Player ActionMask: the server's action index (deaths, taunts, etc.).
    int actionAnim = -1;
    int armAction = -1; // arm thread action index; -1 uses "look"
    bool actionHoldAtEnd = false, actionAtEnd = false;
    float actionAnimPos = 0.0f;
    float actionTime = 0.0f; // demo time the action update arrived
    // Player MoveMask state, in Torque world space.
    bool falling = false, jetting = false;
    Vec3 torqueVelocity{};
    float bodyYaw = 0.0f;
    // ShapeBase MountedMask: object this ghost is mounted on, or -1.
    int mountObject = -1;
    int mountNode = 0;        // parent mount point (ShapeBase::mountObject)
    // CloakMask fade (ShapeBase mFadeVal): a timed fade in or out, or a
    // fixed visible/invisible value.
    bool fading = false, fadeOut = false, fadeFresh = false;
    float fadeTime = 0.0f, fadeVal = 1.0f, fadeStart = 0.0f;
    // Client-derived movement animation, updated on simulation ticks.
    int contactTimer = 0;
    int moveAction = 0;
    float moveTimeScale = 1.0f;
    float moveStartTime = 0.0f;
    bool moveAnimValid = false;
    // Render-side shape lighting probe state.
    ShapeLighting::State shapeLight;
    // Jet flare thread position in [0, 1] (Player::processTick).
    float jetFlarePosition = 0.0f;
    // Vehicle jets: the networked jetting flag and thrust direction, and the
    // client's back/bottom Activate/Maintain thread state.
    float spawnTime = -1.0f; // demo time this ghost first rendered
    float cloakLevel = 0.0f;  // ShapeBase mCloakLevel, 0 -> 1 over 0.5 s
    // Player action transitions (Player::setActionThread, 0.25 s): the last
    // rendered sequence and time, and the frozen outgoing pose.
    int animLastIndex = -1;
    float animLastTime = 0.0f;
    int animPrevIndex = -1;
    float animPrevTime = 0.0f, animChangedAt = -1.0f;
    // getRenderMuzzlePoint/Vector per image slot from the last render (Y-up
    // world): the image's Muzzlepoint node and its native +Y axis.
    Point3F muzzlePos[8]{}, muzzleDir[8]{};
    bool hasMuzzle[8]{};
    // The last render transform (model x upOrientation), for beam raycasts.
    MatrixF renderModel;
    bool hasRenderModel = false;
    // RepairProjectile::advanceTime endpoint: snaps to the first hit, then
    // chases later hits at 2*dt, keeping the last one on a miss.
    Point3F repairCurrent{}, repairDesired{};
    bool repairHasHit = false;
    int repairTarget = -1;
    float repairLastTime = -1.0f;
    // Energy bolt motion blur: recent render positions (Y-up) and times.
    std::vector<std::pair<Point3F, float>> blurTail;
    // LinearProjectile initial state (Torque space) and the client segment:
    // velocity and the flight time until the first world hit or lifetime.
    Vec3 linearStart{}, linearDir{}, linearExcess{};
    int linearCurrTick = 0;
    bool hasLinearFlight = false, linearSegmentValid = false;
    Vec3 linearVelocity{};
    float linearEndTime = 0.0f;
    // GrenadeProjectile family: the latest transmitted state (Torque) and the
    // client flight (Y-up) stepped per 32 ms tick from it.
    Vec3 ballisticSentPos{}, ballisticSentVel{};
    int ballisticCurrTick = 0;
    bool hasBallistic = false, ballisticFresh = false, ballisticStopped = false;
    bool ballisticCoast = false; // seekers: no gravity, stop on any contact
    // ShockLanceProjectile: pinned on a hit object; lightning regeneration.
    bool beamHit = false, shockFresh = false;
    float shockRegenTimer = 0.0f, shockLastTime = -1.0f;
    std::vector<Point3F> shockBolts[2]; // bolt-local points along +X
    std::vector<FlareSpikes::Spike> flareSpikes; // LinearFlareProjectile
    Point3F ballisticPos{}, ballisticVel{};
    float ballisticTime = 0.0f;
    int ballisticAgeTicks = 0;
    bool vehicleJetting = false;
    int thrustDirection = VehicleJets::ThrustForward;
    VehicleJets::Direction jetBack, jetBottom;
};

class GhostTracker {
public:
    bool hasGhost(int index) const;
    const GhostEntry* getGhost(int index) const;
    GhostEntry* getMutableGhost(int index);
    void createGhost(int index, int classId, const std::string& className);
    void deleteGhost(int index);
    void clear();
    int size() const;
    std::vector<int> getAllIndices() const;
private:
    std::map<int, GhostEntry> ghosts;
};

// ─── DemoBlock ──────────────────────────────────────────────────
struct DemoBlock {
    int index{};
    int type{};
    int size{};
    std::vector<uint8_t> data;
};

struct DemoPlayerInfo {
    std::string name, skin;
    int teamId{-1};
    float damage{0};
    int clientId{-1};
    int score{0};
    int ping{0};
    int packetLoss{0};
};

struct DemoPendingExplosion {
    Vec3 position;
    Vec3 normal{0, 1, 0};
    float time{};
    int projectileDataBlockId = -1;
};

struct DemoParserSnapshot {
    size_t blockStreamOffset{};
    int blockCount{-1};
    int blockCursor{};
    GhostTracker ghostTracker;
    Vec3 compressionPoint{};
    uint32_t lastSeqRecvdAtSend[32]{};
    uint32_t lastSeqRecvd{}, highestAckedSeq{}, lastSendSeq{};
    uint32_t recvAckMask{}, connectSequence{}, lastRecvAckAck{};
    bool connectionEstablished{};
    uint32_t nextRecvEventSeq{};
    uint32_t packetsParsed{};
    std::string parseFault;
    uint32_t packetsDroppedAfterFault{};
    std::vector<std::pair<int, std::string>> missionChanges;
    std::vector<std::pair<int, uint32_t>> missionCrcChanges;
    std::map<int, std::string> taggedStrings;
    uint32_t currentMissionCrc{};
    std::string currentMission;
    int nextChangeIdx{};
    std::vector<DemoTimedEvent> eventLog;
    std::vector<DemoPlayerInfo> playerInfo;
    std::map<std::string, std::string> skinToPlayer;
    std::map<int, DemoTargetState> targets;
    WeaponsHudState weaponsHud;
    BackpackHudState backpackHud;
    InventoryHudState inventoryHud;
    VehicleHudState vehicleHud;
    AmmoHudState ammoHud;
    std::vector<DemoPendingExplosion> pendingExplosions;
    std::string pendingTerrainFile;
    Vec3 sunDirection{};
    float sunAzimuth{}, sunElevation{};
    int sunR{}, sunG{}, sunB{};
    bool sunValid{};
};

// ─── DemoParser ─────────────────────────────────────────────────
class DemoParser {
public:
    DemoParser();
    ~DemoParser();

    bool load(const uint8_t* buffer, size_t size);
    bool loadData(const uint8_t* buffer, size_t size);
    bool loadFile(const char* path);

    const DemoHeader& getHeader() const { return header; }
    const InitialBlockData& getInitialBlock() const { return initialBlock; }
    const GhostTracker& getGhostTracker() const { return ghostTracker; }
    GhostTracker& getMutableGhostTracker() { return ghostTracker; }

    int getBlockCount();
    int getMoveBlockCount() const;
    // Move ticks preceding each block; size getBlockCount() + 1.
    const std::vector<int>& getMoveTicksBefore();
    uint32_t getPacketsParsed() const { return packetsParsed; }
    // First packet parse fault. Once set, later packets are dropped until
    // reset() or restoring a snapshot taken before the fault.
    const std::string& getParseFault() const { return parseFault_; }
    uint32_t getPacketsDroppedAfterFault() const { return packetsDroppedAfterFault_; }
    float getRoundTripTime() const { return initialBlock.roundTripTime; }
    float getPacketLoss() const { return initialBlock.packetLoss; }
    int getBlockCursor() const { return blockCursor_; }

    DemoBlock* nextBlock();
    void reset();
    void resetMissionState();
    int processBlocks(int count);
    bool seekToBlock(int blockIndex);

    // Convenience: parse all blocks into memory
    bool parseFull(std::vector<DemoBlock>& outBlocks);
    DemoParserSnapshot captureSnapshot() const;
    bool restoreSnapshot(const DemoParserSnapshot& snapshot);

    // Cross-map mission tracking: scan block stream for .mis paths
    void scanMissionChanges();
    const std::string& currentMission() const { return currentMission_; }
    uint32_t currentMissionCrc() const { return currentMissionCrc_; }
    void setCurrentBlock(int blockIndex);

    // Demo event log
    const std::vector<DemoTimedEvent>& getEventLog() const { return eventLog_; }
    void clearEventLog() { eventLog_.clear(); }

    // Scoreboard data (player names, scores)
    using PlayerInfo = DemoPlayerInfo;
    const std::vector<PlayerInfo>& getPlayerInfo() const { return playerInfo_; }
    const std::string& getPlayerNameForSkin(const std::string& skin) const;

    // HUD state (ported from t2-mapper StreamEngine)
    const WeaponsHudState& getWeaponsHud() const { return weaponsHud_; }
    const BackpackHudState& getBackpackHud() const { return backpackHud_; }
    const InventoryHudState& getInventoryHud() const { return inventoryHud_; }
    const VehicleHudState& getVehicleHud() const { return vehicleHud_; }
    const AmmoHudState& getAmmoHud() const { return ammoHud_; }
    const std::map<std::pair<int, uint32_t>, uint32_t>& getSensorGroupColors() const {
        return sensorGroupColors_;
    }
    void handleHudRemoteCommand(const std::string& funcName, const std::vector<std::string>& args);
    void resetHudState();
    void extractMissionInfo();

private:
    const uint8_t* buf{};
    size_t bufSize{};
    size_t offset{};
    bool ownsBuffer{};

    DemoHeader header;
    InitialBlockData initialBlock;
    GhostTracker ghostTracker;
    GhostTracker ibGhostTracker; // tracker used during initial block parsing

    // Decompressed block stream
    uint8_t* decompressed{};
    size_t decompressedSize{};
    int blockStreamOffset{};
    int blockCount_{ -1 };
    int blockCursor_{};

    // Mission change tracking
    std::vector<std::pair<int, std::string>> missionChanges_;
    std::vector<std::pair<int, uint32_t>> missionCrcChanges_;
    std::string currentMission_;
    uint32_t currentMissionCrc_{};
    int nextChangeIdx_{};
    int parsingBlockIndex_{-1};

    // Demo event log
    std::vector<DemoTimedEvent> eventLog_;

    // Scoreboard data
    std::vector<PlayerInfo> playerInfo_;
    std::map<int, std::string> initialTaggedStrings_;
    std::vector<PlayerInfo> initialPlayerInfo_;
    std::map<std::string, std::string> skinToPlayer_; // skinName → playerName

    // HUD state
    WeaponsHudState weaponsHud_;
    BackpackHudState backpackHud_;
    InventoryHudState inventoryHud_;
    VehicleHudState vehicleHud_;
    AmmoHudState ammoHud_;
    // HUD state saved in the demo values at record start (recordings.cs
    // saveDemoSettings); every HUD reset starts from it.
    WeaponsHudState initialWeaponsHud_;
    BackpackHudState initialBackpackHud_;
    InventoryHudState initialInventoryHud_;
    AmmoHudState initialAmmoHud_;
    std::map<std::pair<int, uint32_t>, uint32_t> sensorGroupColors_;

    // Packet parser state
    Vec3 compressionPoint;
    uint32_t lastSeqRecvdAtSend[32]{};
    uint32_t lastSeqRecvd{}, highestAckedSeq{}, lastSendSeq{};
    uint32_t recvAckMask{}, connectSequence{}, lastRecvAckAck{};
    bool connectionEstablished{};
    uint32_t nextRecvEventSeq{};
    std::map<int, DemoTargetState> targets_, initialTargets_;
    void applyTarget(GhostEntry& ghost) const;
    std::vector<int> moveTicksBefore_;
    uint32_t packetsParsed{};
    std::string parseFault_;
    uint32_t packetsDroppedAfterFault_{};

    // ─── Internal parsing methods ───
    void readHeader();
    bool readInitialBlock(const uint8_t* data, size_t size, uint32_t protocolVersion);
    void readTaggedStrings(BitStream& bs);
    bool readDataBlocks(BitStream& bs);
    QueuedMove readQueuedMove(BitStream& bs);
    std::vector<std::string> readDemoValues(BitStream& bs);
    void readComplexTargetManager(BitStream& bs);
    void readSimpleTargetManager(BitStream& bs);
    void readConnectionProtocol(BitStream& bs);
    std::vector<PathManagerEntry> readPathManager(BitStream& bs);
    void readEventStartBlock(BitStream& bs);
    bool readGhostStartBlock(BitStream& bs, bool useIBTracker);
    InfoBlock readInfoBlock(const uint8_t* data, size_t size);

    // Packet parsing
    DnetHeader readDnetHeader(BitStream& bs);
    GameState readGameState(BitStream& bs);
    void readEvents(BitStream& bs, std::vector<NetEventInfo>& outEvents, const Vec3& compressionPoint);
    bool readEventPayload(BitStream& bs, NetEventInfo& ev, const Vec3& compressionPoint,
                          bool applyEffects);
    void readGhosts(BitStream& bs, std::vector<GhostUpdate>& outGhosts, int seqNumber, const Vec3* compressionPoint = nullptr);

    // Apply protocol header
    bool applyProtocolHeader(const DnetHeader& dnet, bool& dispatchData);

    // Audio profile mapping
    static std::vector<std::string> s_audioProfilePaths;
    static std::string soundPathForProfile(int profileId);
    static void scanAudioProfiles();

public:
    DemoMove readRawMove(const uint8_t* data, size_t size);
    PacketData parsePacket(const uint8_t* data, size_t size, int blockIndex = -1);
    void recordParseFault(const char* stage, int blockIndex);
    void onSendPacketTrigger();

    // Pending explosion events from projectile parsers
    using PendingExplosion = DemoPendingExplosion;
    static std::vector<PendingExplosion> s_pendingExplosions;
    // Demo time of the packet being parsed, for timestamping ghost updates.
    static float s_packetTime;
    std::vector<PendingExplosion> consumeExplosions() { auto r = std::move(s_pendingExplosions); s_pendingExplosions.clear(); return r; }

    // Terrain file from ghost data (for when .mis doesn't have it)
    static std::string s_pendingTerrainFile;

    // Sun data extracted from demo stream (for when .mis is unavailable)
    struct SunData {
        Vec3 direction{};
        float azimuth{}, elevation{};
        int r{}, g{}, b{};
        bool valid = false;
    };
    static SunData s_sunData;
};
