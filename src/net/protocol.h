#pragma once
#include "net/network.h"
#include <cstdint>
#include <vector>

// T2 Protocol constants
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
    };

    struct UpdateMessage {
        float posX, posY, posZ;
        float rotZ, rotX;
        float velX, velY, velZ;
        float health, energy;
        uint8_t flags;
    };

    bool encodeMove(uint8_t* buf, size_t bufSize, const MoveMessage& msg);
    bool decodeMove(const uint8_t* data, size_t size, MoveMessage& msg);
    bool encodeUpdate(uint8_t* buf, size_t bufSize, const UpdateMessage& msg);
    bool decodeUpdate(const uint8_t* data, size_t size, UpdateMessage& msg);
};

// ─── Game Server ──────────────────────────────────────────────────

class GameServer {
public:
    GameServer();
    ~GameServer();

    bool start(uint16_t port);
    void stop();
    void update();

private:
    struct Impl;
    Impl* impl;
};
