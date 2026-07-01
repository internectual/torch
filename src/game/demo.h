#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <functional>

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
    std::string unpackNetString();
    void setStringBufferEnabled(bool en) { stringBufferEnabled = en; if (!en) stringBuffer.clear(); }

    int  getCurPos() const { return bitNum; }
    void setCurPos(int pos) { bitNum = pos; }
    int  getBytePosition() const { return (bitNum + 7) >> 3; }
    bool isError() const { return error; }
    int  getRemainingBits() const { return maxReadBitNum - bitNum; }
    int  getMaxPos() const { return maxReadBitNum; }
    int  savePos() const { return bitNum; }
    void restorePos(int pos) { bitNum = pos; error = false; }
    void skipBits(int count) { bitNum += count; }
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
    inline static const char* NetEventClassNames[NetEventClassCount] = {
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

    // Protocol versions
    constexpr uint32_t ProtocolV24834 = 0x00300004;
    constexpr uint32_t ProtocolV25034 = 0x00330004;

    // Tagged strings
    constexpr int TaggedStringCount = 1024;

    // Deterministic ghost class names for T2 25034 (sorted by strcmp order,
    // matching AbstractClassRep::initialize). Index = classId.
    // Derived from t2-demo-parser / machine code analysis of T2 25034 binary.
    inline const char* const NetObjectClassNames[] = {
        "AIObjective",          // 0
        "AudioEmitter",         // 1
        "BeaconObject",         // 2
        "BombProjectile",       // 3
        "Camera",               // 4
        "Debris",               // 5
        "ELFProjectile",        // 6
        "EnergyProjectile",     // 7
        "FireballAtmosphere",   // 8
        "FlareProjectile",      // 9
        "FlyingVehicle",        // 10
        "ForceFieldBare",       // 11
        "GameBase",             // 12
        "GrenadeProjectile",    // 13
        "HoverVehicle",         // 14
        "InteriorInstance",     // 15
        "Item",                 // 16
        "Lightning",            // 17
        "LinearFlareProjectile",// 18
        "LinearProjectile",     // 19
        "Marker",               // 20
        "MissionArea",          // 21
        "MissionMarker",        // 22
        "ParticleEmissionDummy",// 23
        "PhysicalZone",         // 24
        "Player",               // 25
        "Precipitation",        // 26
        "Projectile",           // 27
        "RepairProjectile",     // 28
        "ScopeAlwaysShape",     // 29
        "SeekerProjectile",     // 30
        "ShapeBase",            // 31
        "ShockLanceProjectile", // 32
        "Shockwave",            // 33
        "SimpleNetObject",      // 34
        "Sky",                  // 35
        "SniperProjectile",     // 36
        "SpawnSphere",          // 37
        "Splash",               // 38
        "StaticShape",          // 39
        "StationFXPersonal",    // 40
        "StationFXVehicle",     // 41
        "Sun",                  // 42
        "TSStatic",             // 43
        "TargetProjectile",     // 44
        "TerrainBlock",         // 45
        "TracerProjectile",     // 46
        "Trigger",              // 47
        "Turret",               // 48
        "VehicleBlocker",       // 49
        "WaterBlock",           // 50
        "WayPoint",             // 51
        "WheeledVehicle",       // 52
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
};

struct ScoreEntry {
    uint32_t clientId{}, teamId{}, score{};
    uint32_t field0{}, field1{}, field2{};
    bool isBot{};
    bool triggerFlags[6]{};
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
    int packetType{}, ackByteCount{}, ackMask{};
};

struct GameState {
    uint32_t lastMoveAck{};
    float damageFlash{ -1 };
    float whiteOut{ -1 };
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
    std::string eventName;  // class name for display
};

struct DemoTimedEvent {
    double time{};       // seconds into demo
    std::string text;    // display text
    int type{};          // 0=chat, 1=server, 2=system
    int ghostIndex{-1};  // source ghost (player), -1 if server
};

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
    std::vector<uint32_t> connectionFields;
    std::vector<uint32_t> stateArray;
    std::vector<ScoreEntry> scoreEntries;
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
    std::string phase2Error;
    int phase2TrailingBits{};
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
    int classId{};
    std::string className;
    Vec3 position{};
    Vec3 renderPos{};
    Vec4 rotation{};
    Vec4 renderRotation{};
    bool hasRotation{};
    std::string skinName;
    std::string shapePath;
    DTSShape* shape{};
    Vec3 prevPosition{};
    float animTime{};
    bool isMoving{};
    float moveYaw{};
    bool hasRendered{};
    bool skinApplied{};
    float health{100.0f};
    float energy{100.0f};
    std::string playerName;
    int teamId{-1};
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

// ─── DemoParser ─────────────────────────────────────────────────
class DemoParser {
public:
    DemoParser();
    ~DemoParser();

    bool load(const uint8_t* buffer, size_t size);
    bool loadFile(const char* path);

    const DemoHeader& getHeader() const { return header; }
    const InitialBlockData& getInitialBlock() const { return initialBlock; }
    const GhostTracker& getGhostTracker() const { return ghostTracker; }

    int getBlockCount();
    int getBlockCursor() const { return blockCursor_; }

    DemoBlock* nextBlock();
    void reset();
    int processBlocks(int count);

    // Convenience: parse all blocks into memory
    bool parseFull(std::vector<DemoBlock>& outBlocks);

    // Cross-map mission tracking: scan block stream for .mis paths
    void scanMissionChanges();
    const std::string& currentMission() const { return currentMission_; }
    void setCurrentBlock(int blockIndex);

    // Demo event log
    const std::vector<DemoTimedEvent>& getEventLog() const { return eventLog_; }
    void clearEventLog() { eventLog_.clear(); }

    // Scoreboard data (player names, scores)
    struct PlayerInfo {
        std::string name, skin;
        int teamId{-1};
        float damage{0};
    };
    const std::vector<PlayerInfo>& getPlayerInfo() const { return playerInfo_; }
    const std::string& getPlayerNameForSkin(const std::string& skin) const;

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
    std::string currentMission_;
    int nextChangeIdx_{};

    // Demo event log
    std::vector<DemoTimedEvent> eventLog_;

    // Scoreboard data
    std::vector<PlayerInfo> playerInfo_;
    std::map<std::string, std::string> skinToPlayer_; // skinName → playerName

    // File path for Node.js reference parser fallback
    std::string recFilePath_;
    void extractScoreboardData(const char* recPath);

    // Packet parser state
    Vec3 compressionPoint;
    uint32_t lastSeqRecvdAtSend[32]{};
    uint32_t lastSeqRecvd{}, highestAckedSeq{}, lastSendSeq{};
    uint32_t recvAckMask{}, connectSequence{}, lastRecvAckAck{};
    bool connectionEstablished{};
    uint32_t nextRecvEventSeq{};
    uint32_t packetsParsed{};

    // ─── Internal parsing methods ───
    void readHeader();
    void readInitialBlock(const uint8_t* data, size_t size);
    void readTaggedStrings(BitStream& bs);
    void readDataBlocks(BitStream& bs);
    ScoreEntry readScoreEntry(BitStream& bs);
    std::vector<std::string> readDemoValues(BitStream& bs);
    void readComplexTargetManager(BitStream& bs);
    void readSimpleTargetManager(BitStream& bs);
    void readConnectionProtocol(BitStream& bs);
    std::vector<PathManagerEntry> readPathManager(BitStream& bs);
    void readEventStartBlock(BitStream& bs);
    void readGhostStartBlock(BitStream& bs, bool useIBTracker);
    InfoBlock readInfoBlock(const uint8_t* data, size_t size);

    // Packet parsing
    DnetHeader readDnetHeader(BitStream& bs);
    GameState readGameState(BitStream& bs);
    void readEvents(BitStream& bs, std::vector<NetEventInfo>& outEvents);
    void readGhosts(BitStream& bs, std::vector<GhostUpdate>& outGhosts, int seqNumber, const Vec3* compressionPoint = nullptr);

    // Apply protocol header
    bool applyProtocolHeader(const DnetHeader& dnet, bool& dispatchData);

public:
    DemoMove readRawMove(const uint8_t* data, size_t size);
    PacketData parsePacket(const uint8_t* data, size_t size, int blockIndex = -1);
};
