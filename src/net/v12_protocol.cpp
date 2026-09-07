#include "net/v12_protocol.h"
#include "net/v12_events.h"

#include <string>
#include <cstdlib>
#include <algorithm>
#include <cmath>

namespace V12 {

bool readDnetHeader(V12BitStream& stream, DnetHeader& header) {
    header.gameFlag = stream.readFlag();
    header.connectSequenceBit = stream.readFlag();
    header.sequence = (uint16_t)stream.readUnsigned(9);
    header.highestAck = (uint16_t)stream.readUnsigned(9);
    const uint32_t type = stream.readUnsigned(2);
    header.packetType = type <= 2 ? (PacketType)type : PacketType::Invalid;
    header.ackByteCount = (uint8_t)stream.readUnsigned(3);
    if (header.ackByteCount > sizeof(header.ackMask)) {
        header.packetType = PacketType::Invalid;
        return false;
    }
    header.ackMask = 0;
    for (uint8_t i = 0; i < header.ackByteCount; ++i)
        header.ackMask |= (uint64_t)stream.readUnsigned(8) << (i * 8);
    return !stream.failed() && header.packetType != PacketType::Invalid;
}

void writeDnetHeader(V12BitWriter& writer, const DnetHeader& header) {
    writer.writeFlag(header.gameFlag);
    writer.writeFlag(header.connectSequenceBit);
    writer.writeUnsigned(header.sequence & 0x1ff, 9);
    writer.writeUnsigned(header.highestAck & 0x1ff, 9);
    writer.writeUnsigned((uint32_t)header.packetType & 3, 2);
    writer.writeUnsigned(header.ackByteCount & 7, 3);
    for (uint8_t i = 0; i < header.ackByteCount; ++i)
        writer.writeUnsigned((uint32_t)(header.ackMask >> (i * 8)), 8);
}

bool readRateInfo(V12BitStream& stream, RateInfo& info) {
    info.hasCurrent = stream.readFlag();
    if (info.hasCurrent) {
        info.updateDelay = (uint16_t)stream.readUnsigned(10);
        info.packetSize = (uint16_t)stream.readUnsigned(10);
    }
    info.hasMaximum = stream.readFlag();
    if (info.hasMaximum) {
        info.maxUpdateDelay = (uint16_t)stream.readUnsigned(10);
        info.maxPacketSize = (uint16_t)stream.readUnsigned(10);
    }
    return !stream.failed();
}

void writeRateInfo(V12BitWriter& writer, const RateInfo& info) {
    writer.writeFlag(info.hasCurrent);
    if (info.hasCurrent) {
        writer.writeUnsigned(info.updateDelay, 10);
        writer.writeUnsigned(info.packetSize, 10);
    }
    writer.writeFlag(info.hasMaximum);
    if (info.hasMaximum) {
        writer.writeUnsigned(info.maxUpdateDelay, 10);
        writer.writeUnsigned(info.maxPacketSize, 10);
    }
}

static void appendU32(std::vector<uint8_t>& packet, uint32_t value) {
    packet.push_back((uint8_t)(value & 0xff));
    packet.push_back((uint8_t)((value >> 8) & 0xff));
    packet.push_back((uint8_t)((value >> 16) & 0xff));
    packet.push_back((uint8_t)((value >> 24) & 0xff));
}

std::vector<uint8_t> buildConnectChallengeRequest(uint32_t protocolVersion,
                                                   uint32_t clientSequence,
                                                   const std::string& password) {
    std::vector<uint8_t> packet{OobConnectChallengeRequest};
    appendU32(packet, protocolVersion);
    appendU32(packet, clientSequence);
    V12BitWriter payload;
    payload.writeHuffmanString(password);
    payload.writeFlag(false); // no authentication payload
    packet.insert(packet.end(), payload.data().begin(), payload.data().end());
    return packet;
}

std::vector<uint8_t> buildConnectRequest(uint32_t serverSequence,
                                         uint32_t clientSequence,
                                         uint32_t protocolVersion,
                                         bool authenticated,
                                         const std::vector<std::string>& argv) {
    V12BitWriter payload;
    payload.writeUnsigned(OobConnectRequest, 8);
    payload.writeUnsigned(serverSequence, 32);
    payload.writeUnsigned(clientSequence, 32);
    payload.writeUnsigned(protocolVersion, 32);
    payload.writeFlag(authenticated);
    payload.writeUnsigned((uint32_t)argv.size(), 32);
    for (const std::string& arg : argv)
        payload.writeHuffmanString(arg);
    return payload.data();
}

std::vector<uint8_t> buildConnectChallengeResponse(uint32_t serverSequence,
                                                   uint32_t clientSequence) {
    V12BitWriter payload;
    payload.writeUnsigned(OobConnectChallengeResponse, 8);
    payload.writeUnsigned(ProtocolVersion, 32);
    payload.writeUnsigned(serverSequence, 32);
    payload.writeUnsigned(clientSequence, 32);
    return payload.data();
}

std::vector<uint8_t> buildConnectAccept(uint32_t serverSequence,
                                         uint32_t clientSequence) {
    V12BitWriter payload;
    payload.writeUnsigned(OobConnectAccept, 8);
    payload.writeUnsigned(serverSequence, 32);
    payload.writeUnsigned(clientSequence, 32);
    return payload.data();
}

bool readConnectChallengeResponse(const uint8_t* data, size_t size,
                                  ConnectChallengeResponse& response) {
    if (!data || size < 13 || data[0] != 30) return false;
    V12BitStream stream(data + 1, size - 1);
    response.protocolVersion = stream.readU32();
    response.serverSequence = stream.readU32();
    response.clientSequence = stream.readU32();
    return !stream.failed();
}

bool readConnectAccept(const uint8_t* data, size_t size, ConnectAccept& accept) {
    if (!data || size < 9 || data[0] != OobConnectAccept) return false;
    V12BitStream stream(data + 1, size - 1);
    accept.serverSequence = stream.readU32();
    accept.clientSequence = stream.readU32();
    return !stream.failed();
}

std::vector<uint8_t> buildGameQuery(uint8_t type, uint8_t flags, uint32_t key) {
    std::vector<uint8_t> packet{type, flags};
    appendU32(packet, key);
    return packet;
}

std::vector<uint8_t> buildGamePingResponse(uint8_t flags, uint32_t key,
                                           const std::string& serverName,
                                           uint32_t buildVersion) {
    V12BitWriter payload;
    payload.writeUnsigned(16, 8);
    payload.writeUnsigned(flags, 8);
    payload.writeUnsigned(key, 32);
    payload.writeHuffmanString("VER5");
    payload.writeUnsigned(ProtocolVersion, 32);
    payload.writeUnsigned(ProtocolVersion, 32);
    payload.writeUnsigned(buildVersion, 32);
    payload.writeHuffmanString(serverName);
    return payload.data();
}

std::vector<uint8_t> buildGameInfoResponse(uint8_t flags, uint32_t key,
                                           const std::string& mod,
                                           const std::string& gameType,
                                           const std::string& mapName,
                                           uint8_t status, uint8_t players,
                                           uint8_t maxPlayers, uint8_t bots) {
    V12BitWriter payload;
    payload.writeUnsigned(20, 8);
    payload.writeUnsigned(flags, 8);
    payload.writeUnsigned(key, 32);
    payload.writeHuffmanString(mod);
    payload.writeHuffmanString(gameType);
    payload.writeHuffmanString(mapName);
    payload.writeUnsigned(status, 8);
    payload.writeUnsigned(players, 8);
    payload.writeUnsigned(maxPlayers, 8);
    payload.writeUnsigned(bots, 8);
    payload.writeUnsigned(0, 16); // CPU MHz
    payload.writeHuffmanString(""); // server description
    payload.writeUnsigned(0, 16); // empty status/roster string
    return payload.data();
}

std::vector<uint8_t> buildDisconnectPacket(uint32_t serverSequence,
                                            uint32_t clientSequence) {
    std::vector<uint8_t> packet{OobDisconnect};
    appendU32(packet, serverSequence);
    appendU32(packet, clientSequence);
    V12BitWriter reason;
    reason.writeHuffmanString("");
    packet.insert(packet.end(), reason.data().begin(), reason.data().end());
    return packet;
}

bool ReceiveWindow::accept(uint16_t sequence, bool packetConnectSequenceBit) {
    sequence &= 0x1ff;
    if (packetConnectSequenceBit != connectSequenceBit) return false;
    if (!haveSequence) {
        highest = sequence;
        mask = 0;
        haveSequence = true;
        return true;
    }

    const uint16_t delta = (uint16_t)((sequence - highest) & 0x1ff);
    if (delta == 0 || delta >= 256) return false;

    if (delta >= 64) {
        mask = 0;
    } else {
        mask <<= delta;
        mask |= 1ull << (delta - 1);
    }
    highest = sequence;
    return true;
}

void ReceiveWindow::reset(bool newConnectSequenceBit) {
    connectSequenceBit = newConnectSequenceBit;
    haveSequence = false;
    highest = 0;
    mask = 0;
}

void ProtocolState::noteSentDataPacket() {
    ++lastSentSequence;
}

void ProtocolState::reset(uint32_t newConnectSequence) {
    connectSequence = newConnectSequence;
    lastSeqReceived = 0;
    highestAcked = 0;
    lastSentSequence = 0;
    receiveMask = 0;
    lastReceivedAckAck = 0;
    connectionEstablished = false;
}

void ProtocolState::writePacketHeader(V12BitWriter& writer, PacketType packetType) {
    if (packetType == PacketType::Data)
        noteSentDataPacket();

    uint8_t ackByteCount = 0;
    uint32_t mask = receiveMask;
    if (mask != 0) {
        if (mask & 0xff000000u) ackByteCount = 4;
        else if (mask & 0x00ff0000u) ackByteCount = 3;
        else if (mask & 0x0000ff00u) ackByteCount = 2;
        else ackByteCount = 1;
    }

    DnetHeader header;
    header.gameFlag = true;
    header.connectSequenceBit = (connectSequence & 1) != 0;
    header.sequence = (uint16_t)(lastSentSequence & 0x1ff);
    header.highestAck = (uint16_t)(lastSeqReceived & 0x1ff);
    header.packetType = packetType;
    header.ackByteCount = ackByteCount;
    header.ackMask = mask;

    writeDnetHeader(writer, header);
}

std::vector<uint8_t> ProtocolState::buildPacket(PacketType packetType) {
    V12BitWriter writer;
    writePacketHeader(writer, packetType);
    return writer.data();
}

static void writeClientMove(V12BitWriter& writer, const ClientMove& move) {
    constexpr float TwoPi = 6.28318530717958647692f;
    const int yaw = (int)std::lround((move.yaw / TwoPi) * 65536.0f);
    const int pitch = (int)std::lround((move.pitch / TwoPi) * 65536.0f);
    const int roll = (int)std::lround((move.roll / TwoPi) * 65536.0f);
    for (int value : {yaw, pitch, roll}) {
        writer.writeFlag(value != 0);
        if (value != 0) writer.writeUnsigned((uint32_t)value & 0xffffu, 16);
    }
    const auto axis = [](float value) -> uint32_t {
        return (uint32_t)std::clamp((int)std::lround(value * 16.0f + 16.0f), 0, 32);
    };
    writer.writeUnsigned(axis(move.x), 6);
    writer.writeUnsigned(axis(move.y), 6);
    writer.writeUnsigned(axis(move.z), 6);
    writer.writeFlag(move.freeLook);
    for (bool trigger : move.trigger) writer.writeFlag(trigger);
}

std::vector<uint8_t> ProtocolState::buildClientPacket(const ClientPacketOptions& options) {
    V12BitWriter writer;
    writePacketHeader(writer, PacketType::Data);
    writer.writeFlag(false); // current receive rate unchanged
    writer.writeFlag(options.advertiseMaxRate);
    if (options.advertiseMaxRate) {
        writer.writeUnsigned(options.maxUpdateDelay, 10);
        writer.writeUnsigned(options.maxPacketSize, 10);
    }
    writer.writeFlag(false); // observer is not first-person
    writer.writeUnsigned(0, 32); // control object checksum
    writer.writeUnsigned(options.moveStart, 32);
    writer.writeUnsigned((uint32_t)std::min<size_t>(options.moves.size(), 31), 5);
    for (size_t i = 0; i < options.moves.size() && i < 31; ++i)
        writeClientMove(writer, options.moves[i]);
    writer.writeFlag(false); // FOV unchanged
    writer.writeFlag(false); // end unguaranteed events
    for (const ClientEvent& event : options.events) {
        V12BitWriter encodedEvent;
        encodedEvent.writeFlag(true);
        encodedEvent.writeFlag(false); // explicit event sequence
        encodedEvent.writeUnsigned(event.sequence & 0x7f, 7);
        encodedEvent.writeUnsigned(event.classId, EventClassBits);
        if (event.write) event.write(encodedEvent);
        if (writer.sizeBits() + encodedEvent.sizeBits() + 1 > 450 * 8) break;
        writer.writeBits(encodedEvent.data().data(), encodedEvent.sizeBits());
    }
    writer.writeFlag(false); // end guaranteed events
    return writer.data();
}

std::vector<uint8_t> ProtocolState::buildServerPacket(ServerPacketOptions& options) {
    V12BitWriter writer;
    writePacketHeader(writer, PacketType::Data);
    writer.writeUnsigned(0, 2); // server rate field width: four 7-bit floats
    for (int i = 0; i < 4; ++i) writer.writeUnsigned(0, 7);
    writer.writeUnsigned(options.lastMoveAck, 32);
    writer.writeFlag(false); // damage/whiteout
    writer.writeFlag(false); // self lock state
    writer.writeFlag(false); // seeker state
    writer.writeFlag(false); // pinged
    writer.writeFlag(false); // jammed
    writer.writeFlag(false); // control object
    writer.writeFlag(false); // target visibility terminator
    writer.writeFlag(false); // camera FOV
    writer.writeFlag(false); // no unguaranteed events
    for (const ClientEvent& event : options.events) {
        writer.writeFlag(true);
        writer.writeFlag(false);
        writer.writeUnsigned(event.sequence & 0x7f, 7);
        writer.writeUnsigned(event.classId, EventClassBits);
        if (event.write) event.write(writer);
    }
    writer.writeFlag(false); // end guaranteed events
    writer.writeFlag(!options.ghosts.empty());
    options.emittedGhosts.clear();
    if (!options.ghosts.empty()) {
        writer.writeUnsigned(7, 3); // 10-bit ghost indices
        for (const auto& ghost : options.ghosts) {
            V12BitWriter encodedGhost;
            encodedGhost.writeFlag(true);
            encodedGhost.writeUnsigned(ghost.index, 10);
            encodedGhost.writeFlag(ghost.deleted);
            if (!ghost.deleted) {
                if (ghost.hasClass) encodedGhost.writeUnsigned(ghost.classId, 7);
                if (ghost.write) ghost.write(encodedGhost);
            }
            if (writer.sizeBits() + encodedGhost.sizeBits() + 1 > 450 * 8) break;
            writer.writeBits(encodedGhost.data().data(), encodedGhost.sizeBits());
            options.emittedGhosts.push_back(ghost.index);
        }
        writer.writeFlag(false);
    }
    return writer.data();
}

static void writePackedNetString(V12BitWriter& writer, const std::string& value) {
    if (value.empty()) {
        writer.writeUnsigned(0, 2);
        return;
    }
    if (value[0] == '\x01') {
        writer.writeUnsigned(2, 2);
        writer.writeUnsigned((uint32_t)std::strtoul(value.c_str() + 1, nullptr, 10), 10);
        return;
    }
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (end && *end == '\0') {
        const uint32_t magnitude = (uint32_t)(number < 0 ? -number : number);
        writer.writeUnsigned(3, 2);
        writer.writeFlag(number < 0);
        if (magnitude < 128) {
            writer.writeFlag(true);
            writer.writeUnsigned(magnitude, 7);
        } else if (magnitude < 32768) {
            writer.writeFlag(false);
            writer.writeFlag(true);
            writer.writeUnsigned(magnitude, 15);
        } else {
            writer.writeFlag(false);
            writer.writeFlag(false);
            writer.writeUnsigned(magnitude, 31);
        }
        return;
    }
    writer.writeUnsigned(1, 2);
    writer.writeFlag(false); // no compression-buffer prefix
    writer.writeHuffmanString(value);
}

ClientEvent makeNetStringEvent(uint16_t id, const std::string& value) {
    return {0, 7, [id, value](V12BitWriter& writer) {
        writer.writeUnsigned(id, 10);
        writer.writeFlag(true);
        writer.writeFlag(false); // no compression-buffer prefix
        writer.writeHuffmanString(value);
    }};
}

ClientEvent makeGhostingMessageEvent(uint32_t sequence, uint8_t message,
                                     uint16_t ghostCount) {
    return {0, 4, [sequence, message, ghostCount](V12BitWriter& writer) {
        writer.writeUnsigned(sequence, 32);
        writer.writeUnsigned(message, 3);
        writer.writeUnsigned(ghostCount, 11);
    }};
}

std::vector<ClientEvent> buildRemoteCommandEvents(NetStringTable& strings,
                                                   const std::string& command,
                                                   const std::vector<std::string>& args) {
    std::vector<ClientEvent> events;
    const auto [id, isNew] = strings.getOrAdd(command);
    if (id == 0) return events;
    if (isNew) events.push_back(makeNetStringEvent(id, command));
    std::vector<std::string> values;
    values.push_back(command);
    values.insert(values.end(), args.begin(), args.end());
    if (values.size() > 20) values.resize(20);
    events.push_back({0, 9, [id, values](V12BitWriter& writer) {
        writer.writeUnsigned((uint32_t)values.size(), 5);
        writer.writeUnsigned(2, 2); // tagged string
        writer.writeUnsigned(id, 10);
        for (size_t i = 1; i < values.size(); ++i)
            writePackedNetString(writer, values[i]);
    }});
    return events;
}

ProtocolResult ProtocolState::processReceived(const DnetHeader& header) {
    if (header.connectSequenceBit != (connectSequence & 1)) return {};
    if (header.ackByteCount > 4 || header.packetType == PacketType::Invalid) return {};

    uint32_t sequence = header.sequence | (lastSeqReceived & 0xfffffe00u);
    if (sequence < lastSeqReceived) sequence += MaxPacketSequence;
    if (sequence > lastSeqReceived + 0x1f) return {};

    uint32_t ack = header.highestAck | (highestAcked & 0xfffffe00u);
    if (ack < highestAcked) ack += MaxPacketSequence;
    if (ack > lastSentSequence) return {};

    const uint32_t shift = (sequence - lastSeqReceived) & 0x1f;
    receiveMask = shift >= 32 ? 0 : receiveMask << shift;
    if (header.packetType == PacketType::Data) receiveMask |= 1;

    ProtocolResult result;
    result.accepted = true;
    for (uint32_t sent = highestAcked + 1; sent <= ack; ++sent) {
        const uint32_t distance = (ack - sent) & 0x1f;
        const bool acknowledged = distance < 32 &&
            (header.ackMask & (1u << distance)) != 0;
        result.acknowledgements.push_back({sent, acknowledged});
        if (acknowledged)
            connectionEstablished = true;
    }
    if (sequence > lastReceivedAckAck + 0x20)
        lastReceivedAckAck = sequence - 0x20;
    highestAcked = ack;
    const bool dispatch = lastSeqReceived != sequence && header.packetType == PacketType::Data;
    lastSeqReceived = sequence;
    result.dispatchData = dispatch;
    return result;
}

} // namespace V12
