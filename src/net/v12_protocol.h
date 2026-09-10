#pragma once

#include "net/v12_bitstream.h"
#include "net/v12_events.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace V12 {

// Connection protocol version from the Tribes2.exe handshake. The demo file
// header uses a different packed build/version constant.
constexpr uint32_t ProtocolVersion = 0x33;
constexpr int PacketSequenceBits = 9;
constexpr int MaxPacketSequence = 1 << PacketSequenceBits;
constexpr uint8_t OobConnectChallengeRequest = 26;
constexpr uint8_t OobConnectRequest = 32;
constexpr uint8_t OobConnectChallengeResponse = 30;
constexpr uint8_t OobConnectAccept = 36;
constexpr uint8_t OobDisconnect = 38;
constexpr uint8_t OobGamePingRequest = 14;
constexpr uint8_t OobGameInfoRequest = 18;

enum class PacketType : uint32_t {
    Data = 0,
    Ping = 1,
    Ack = 2,
    Invalid = 3,
};

struct DnetHeader {
    bool gameFlag = false;
    bool connectSequenceBit = false;
    uint16_t sequence = 0;
    uint16_t highestAck = 0;
    PacketType packetType = PacketType::Invalid;
    uint8_t ackByteCount = 0;
    uint64_t ackMask = 0;
};

struct RateInfo {
    bool hasCurrent = false;
    uint16_t updateDelay = 0;
    uint16_t packetSize = 0;
    bool hasMaximum = false;
    uint16_t maxUpdateDelay = 0;
    uint16_t maxPacketSize = 0;
};

struct ConnectChallengeResponse {
    uint32_t protocolVersion = 0;
    uint32_t serverSequence = 0;
    uint32_t clientSequence = 0;
};

struct ConnectAccept {
    uint32_t serverSequence = 0;
    uint32_t clientSequence = 0;
};

// Reads the packed header used by Tribes 2 data packets. The stream remains
// positioned at the first game-payload bit on success.
bool readDnetHeader(V12BitStream& stream, DnetHeader& header);

void writeDnetHeader(V12BitWriter& writer, const DnetHeader& header);
bool readRateInfo(V12BitStream& stream, RateInfo& info);
void writeRateInfo(V12BitWriter& writer, const RateInfo& info);
std::vector<uint8_t> buildConnectChallengeRequest(uint32_t protocolVersion,
                                                    uint32_t clientSequence,
                                                    const std::string& password = {});
std::vector<uint8_t> buildConnectRequest(uint32_t serverSequence,
                                         uint32_t clientSequence,
                                         uint32_t protocolVersion,
                                         bool authenticated,
                                         const std::vector<std::string>& argv = {});
std::vector<uint8_t> buildConnectChallengeResponse(uint32_t serverSequence,
                                                   uint32_t clientSequence);
std::vector<uint8_t> buildConnectAccept(uint32_t serverSequence,
                                         uint32_t clientSequence);
bool readConnectChallengeResponse(const uint8_t* data, size_t size,
                                  ConnectChallengeResponse& response);
bool readConnectAccept(const uint8_t* data, size_t size, ConnectAccept& accept);
std::vector<uint8_t> buildGameQuery(uint8_t type, uint8_t flags, uint32_t key);
std::vector<uint8_t> buildGamePingResponse(uint8_t flags, uint32_t key,
                                           const std::string& serverName,
                                           uint32_t buildVersion = 25034);
std::vector<uint8_t> buildGameInfoResponse(uint8_t flags, uint32_t key,
                                           const std::string& mod,
                                           const std::string& gameType,
                                           const std::string& mapName,
                                           uint8_t status, uint8_t players,
                                           uint8_t maxPlayers, uint8_t bots);
std::vector<uint8_t> buildDisconnectPacket(uint32_t serverSequence,
                                            uint32_t clientSequence);

class ReceiveWindow {
public:
    explicit ReceiveWindow(bool connectSequenceBit = false)
        : connectSequenceBit(connectSequenceBit) {}

    // Accept a packet sequence and update the acknowledgement mask. Returns
    // false for a duplicate, stale packet, or the wrong connection epoch.
    bool accept(uint16_t sequence, bool packetConnectSequenceBit);
    void reset(bool newConnectSequenceBit);

    bool initialized() const { return haveSequence; }
    uint16_t highestSequence() const { return highest; }
    uint64_t acknowledgementMask() const { return mask; }
    bool connectBit() const { return connectSequenceBit; }

private:
    bool connectSequenceBit;
    bool haveSequence = false;
    uint16_t highest = 0;
    uint64_t mask = 0;
};

struct ProtocolResult {
    bool accepted = false;
    bool dispatchData = false;
    struct AckNotification {
        uint32_t sequence = 0;
        bool acknowledged = false;
    };
    std::vector<AckNotification> acknowledgements;
};

struct ProtocolStateSnapshot {
    uint32_t connectSequence = 0;
    uint32_t lastReceived = 0;
    uint32_t highestAcknowledged = 0;
    uint32_t lastSent = 0;
    uint32_t receiveAckMask = 0;
    bool established = false;
};

struct ClientMove {
    float x = 0;
    float y = 0;
    float z = 0;
    float yaw = 0;
    float pitch = 0;
    float roll = 0;
    bool freeLook = false;
    bool trigger[6]{};
};

struct ClientEvent {
    uint8_t sequence = 0;
    uint8_t classId = 0;
    std::function<void(V12BitWriter&)> write;
};

struct ClientPacketOptions {
    uint32_t moveStart = 0;
    std::vector<ClientMove> moves;
    bool advertiseMaxRate = false;
    uint16_t maxUpdateDelay = 0;
    uint16_t maxPacketSize = 0;
    std::vector<ClientEvent> events;
};

struct ServerPacketOptions {
    uint32_t lastMoveAck = 0;
    std::vector<ClientEvent> events;
    struct Ghost {
        uint16_t index = 0;
        uint8_t classId = 0;
        bool hasClass = true;
        bool deleted = false;
        std::function<void(V12BitWriter&)> write;
    };
    std::vector<Ghost> ghosts;
    std::vector<uint16_t> emittedGhosts;
};

ClientEvent makeNetStringEvent(uint16_t id, const std::string& value);
ClientEvent makeGhostingMessageEvent(uint32_t sequence, uint8_t message,
                                     uint16_t ghostCount);
std::vector<ClientEvent> buildRemoteCommandEvents(NetStringTable& strings,
                                                   const std::string& command,
                                                   const std::vector<std::string>& args = {});

class ProtocolState {
public:
    ProtocolState(uint32_t connectSequence = 0)
        : connectSequence(connectSequence) {}

    void reset(uint32_t newConnectSequence = 0);
    void setConnectSequence(uint32_t sequence) { connectSequence = sequence; }
    void noteSentDataPacket();
    void writePacketHeader(V12BitWriter& writer, PacketType packetType);
    std::vector<uint8_t> buildPacket(PacketType packetType);
    std::vector<uint8_t> buildClientPacket(const ClientPacketOptions& options);
    std::vector<uint8_t> buildServerPacket(ServerPacketOptions& options);
    ProtocolResult processReceived(const DnetHeader& header);

    uint32_t lastReceived() const { return lastSeqReceived; }
    uint32_t highestAcknowledged() const { return highestAcked; }
    uint32_t lastSent() const { return lastSentSequence; }
    uint32_t receiveAckMask() const { return receiveMask; }
    uint32_t connectionSequence() const { return connectSequence; }
    bool established() const { return connectionEstablished; }
    ProtocolStateSnapshot snapshot() const;
    void restore(const ProtocolStateSnapshot& state);

private:
    uint32_t connectSequence;
    uint32_t lastSeqReceived = 0;
    uint32_t highestAcked = 0;
    uint32_t lastSentSequence = 0;
    uint32_t receiveMask = 0;
    uint32_t lastReceivedAckAck = 0;
    bool connectionEstablished = false;
};

} // namespace V12
