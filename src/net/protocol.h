#pragma once
#include "net/network.h"
#include <cstdint>
#include <vector>

// ─── Torch Network Protocol ──────────────────────────────────────────
//
// Wire Protocol (UDP):
//   All packets are prefixed with a WireHeader (sequence, ack, ackMask,
//   type, checksum). Game data payloads begin with a GameDataType byte.
//
// Game Data Types:
//   GDT_Move      (0x10) – Client→Server: player input (movement + fire)
//   GDT_Update    (0x11) – Server→Client: authoritative state reply
//   GDT_Ghost     (0x12) – Ghost create/update/delete (best-effort)
//   GDT_GhostAlways (0x13) – Same as Ghost but reliable delivery
//   GDT_Datablock (0x14) – Data block definitions (sent at connect)
//   GDT_ChatMessage (0x20) – Chat text (relayed to all clients)
//   GDT_GameState (0x60) – Server→Client: control object, mode info
//   GDT_Command   (0x50) – Client→Server: arbitrary console command
//
// Ghost Payload Format (32-64 bytes per ghost):
//   [posX:4][posY:4][posZ:4][rotX:4][rotZ:4][health:4]
//   [kills:4][deaths:4][playerName:0..28+null]
//   Multiple ghosts are batched into a single packet after the header.
//
// Datablock Payload Format:
//   [classId:4][objectId:4][index:4][total:4][payload...]
//   Payload is class-specific (e.g. health, shape name).
//
// Connection Flow:
//   1. Client sends GDT_Connect    2. Server replies GDT_Challenge
//   3. Client replies GDT_ChallengeResponse  4. Server sends GameState
//   5. Client enters game loop with periodic Move messages
//   6. Server responds with ghost updates, sends datablocks once.
//
// Server Architecture:
//   GameServer owns an Impl with Client[], ServerGhost[], projectiles[].
//   update() processes incoming packets, simulates movement, then sends
//   batched ghost/datablock updates to each client.
//
// ─── End Protocol Overview ─────────────────────────────────────────
namespace T2Protocol {
    constexpr uint16_t DEFAULT_PORT = 28000;
    constexpr uint32_t PROTOCOL_VERSION = 3;
    constexpr uint32_t CHALLENGE_SIZE = 8;

    // Game data types
    enum GameDataType : uint8_t {
        // Connection
        GDT_Connect = 0x00,
        GDT_Challenge = 0x01,
        GDT_ChallengeResponse = 0x02,
        GDT_ConnectReject = 0x03,

        // Game messages
        GDT_Move = 0x10,
        GDT_Update = 0x11,
        GDT_Ghost = 0x12,
        GDT_GhostAlways = 0x13,
        GDT_Datablock = 0x14,

        // Chat
        GDT_ChatMessage = 0x20,
        GDT_TeamMessage = 0x21,

        // Game state
        GDT_GameStart = 0x30,
        GDT_GameEnd = 0x31,
        GDT_PlayerList = 0x32,
        GDT_Score = 0x33,
        GDT_Item = 0x34,

        // Voice
        GDT_Voice = 0x40,

        // Command routing
        GDT_Command = 0x50,

        // Game state
        GDT_GameState = 0x60,
    };

    struct PacketHeader {
        uint8_t sequence{};
        uint8_t ack{};
        uint16_t ackMask{};
        PacketType type{};
        uint16_t checksum{};
    };

    struct GamePacket {
        uint32_t sequence{};
        uint32_t ack{};
        uint32_t ackMask{};
        uint8_t data[1]; // variable-length data follows header
    };

    // Helper functions
    uint16_t calculateChecksum(const uint8_t* data, size_t size);
    bool verifyChecksum(const uint8_t* data, size_t size);

    // TribesNext RSA authentication
    struct RSAKey {
        uint8_t modulus[256];  // 2048-bit RSA modulus
        uint8_t exponent[4];   // public exponent (usually 65537)
    };

    // Challenge/response messages
    struct AuthChallenge {
        uint8_t data[64];      // random challenge bytes
    };

    struct AuthResponse {
        uint8_t signature[256]; // RSA-encrypted challenge hash
    };

    // Load the TribesNext public key
    const RSAKey& getTribesNextPublicKey();

    // Connection protocol messages
    struct ConnectMessage {
        uint32_t protocol;
        uint32_t challenge;
        char version[32];
        char gameType[32];
    };

    struct ChallengeMessage {
        uint32_t challenge[2];
    };

    struct ChallengeResponse {
        uint32_t response[2];
    };

    // ─── Game Messages ─────────────────────────────────────────────

    struct MoveMessage {
        float posX, posY, posZ;
        float rotZ, rotX;
        uint8_t flags;
        float lookX, lookY;
        uint32_t seq;
    };

    struct UpdateMessage {
        float posX, posY, posZ;
        float rotZ, rotX;
        float velX, velY, velZ;
        float health, energy;
        uint8_t flags;
        uint32_t lastMoveSeq;
    };

    size_t encodeMove(uint8_t* buf, size_t bufSize, const MoveMessage& msg);
    bool decodeMove(const uint8_t* data, size_t size, MoveMessage& msg);
    bool encodeUpdate(uint8_t* buf, size_t bufSize, const UpdateMessage& msg);
    bool decodeUpdate(const uint8_t* data, size_t size, UpdateMessage& msg);

    // ─── Datablock Messages ──────────────────────────────────────────
    struct DatablockHeader {
        uint32_t classId{};
        uint32_t objectId{};
        uint32_t index{};
        uint32_t total{};
    };

    // Encode a datablock header + payload into buffer
    // Returns total bytes written, 0 on failure
    size_t encodeDatablock(uint8_t* buf, size_t bufSize,
                           const DatablockHeader& hdr,
                           const uint8_t* payload, size_t payloadLen);
    // Decode — returns pointer to payload, sets payloadLen
    bool decodeDatablock(const uint8_t* data, size_t size,
                         DatablockHeader& hdr,
                         const uint8_t*& payload, size_t& payloadLen);

    // ─── Ghost Messages ──────────────────────────────────────────────
    enum GhostUpdateType : uint8_t {
        Ghost_Create = 0,
        Ghost_Update = 1,
        Ghost_Delete = 2,
    };

    struct GhostMessage {
        uint32_t index{};
        GhostUpdateType type{};
        int32_t classId{};   // meaningful for Create
        // Variable-length class-specific data follows
    };

    // Encode a ghost header; returns bytes written (payload not included)
    size_t encodeGhostHeader(uint8_t* buf, size_t bufSize, const GhostMessage& msg);
    bool decodeGhostHeader(const uint8_t* data, size_t size, GhostMessage& msg);

    // ─── Game State Message ──────────────────────────────────────────
    struct GameStateMessage {
        uint32_t controlObjectGhostIndex{};
        float energy{}, rechargeRate{};
        uint8_t flags{};
        int32_t gameMode{};
        int32_t scoreLimit{};
    };

    size_t encodeGameState(uint8_t* buf, size_t bufSize, const GameStateMessage& msg);
    bool decodeGameState(const uint8_t* data, size_t size, GameStateMessage& msg);

    // ─── Chat Message ────────────────────────────────────────────
    struct ChatMessage {
        char sender[32]{};
        char text[256]{};
    };

    size_t encodeChat(uint8_t* buf, size_t bufSize, const ChatMessage& msg);
    bool decodeChat(const uint8_t* data, size_t size, ChatMessage& msg);
};

// ─── Game Server ──────────────────────────────────────────────────

class GameServer {
public:
    GameServer();
    ~GameServer();

    bool start(uint16_t port);
    void stop();
    void update();

    // Set a callback for terrain height queries (x,z → height)
    // Returns true if the callback was set
    void setHeightCallback(float (*cb)(float, float, void*), void* userData) {
        heightCB = cb; heightCtx = userData;
    }

    // Load a .mis mission file and spawn ghosts for supported objects
    bool loadMission(const char* missionPath);

    // Ghost management API
    uint32_t spawnGhost(int32_t classId, float x, float y, float z);
    bool removeGhost(uint32_t index);
    size_t ghostCount() const;
    void spawnBot();

    // Admin commands
    void kickClient(int clientIndex);
    void banClient(int clientIndex);
    void changeMap(const char* mission);
    void setGameMode(int mode);
    void clearBans();

    // Recording
    void startRecording(const char* path);
    void stopRecording();

private:
    struct Impl;
    Impl* impl;
    float (*heightCB)(float, float, void*) = nullptr;
    void* heightCtx = nullptr;
};
