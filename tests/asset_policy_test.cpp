#include "fs/asset_policy.h"
#include "net/v12_bitstream.h"
#include "net/v12_protocol.h"
#include "net/v12_registry.h"
#include "net/v12_events.h"
#include "net/v12_datablocks.h"
#include "net/v12_ghosts.h"
#include "net/v12_ghost_packet.h"

#include <cassert>

int main() {
    using TorchAssets::isOriginalRuntimePath;

    assert(isOriginalRuntimePath("shapes/bioderm_light.dts"));
    assert(isOriginalRuntimePath("interiors/base.dif"));
    assert(isOriginalRuntimePath("missions/riverdance.mis"));
    assert(isOriginalRuntimePath("textures/shell/button.png"));
    assert(isOriginalRuntimePath("textures/lush/terrain.tga"));
    assert(isOriginalRuntimePath("textures/lush/terrain.dds"));
    assert(isOriginalRuntimePath("scripts/server.cs"));
    assert(isOriginalRuntimePath("shapes/bioderm_light"));

    assert(!isOriginalRuntimePath("shapes/bioderm_light.glb"));
    assert(!isOriginalRuntimePath("generated/font.ttf"));
    assert(!isOriginalRuntimePath("cache/converted.mesh"));

    const uint8_t packed[] = {0xb5, 0x01};
    V12BitStream bits(packed, sizeof(packed));
    assert(bits.readUnsigned(4) == 5);
    assert(bits.readFlag());
    assert(bits.readSigned(3) == -1);
    assert(bits.position() == 8);
    assert(bits.readRange(10, 13) == 11);

    V12BitWriter floatWriter;
    floatWriter.writeUnsigned(0x3f800000u, 32);
    V12BitStream floatStream(floatWriter.data().data(), floatWriter.data().size());
    assert(floatStream.readF32() == 1.0f);
    V12BitWriter normalizedWriter;
    normalizedWriter.writeUnsigned(3, 3);
    V12BitStream normalizedStream(normalizedWriter.data().data(), normalizedWriter.data().size());
    assert(normalizedStream.readFloat(3) == 3.0f / 7.0f);
    V12BitWriter signedNormalizedWriter;
    signedNormalizedWriter.writeUnsigned(1, 1);
    V12BitStream signedNormalizedStream(signedNormalizedWriter.data().data(), signedNormalizedWriter.data().size());
    assert(signedNormalizedStream.readSignedFloat(1) == 1.0f);
    V12BitWriter pointWriter;
    pointWriter.writeUnsigned(0x3f800000u, 32);
    pointWriter.writeUnsigned(0x40000000u, 32);
    pointWriter.writeUnsigned(0x40400000u, 32);
    V12BitStream pointStream(pointWriter.data().data(), pointWriter.data().size());
    const V12Vec3 point = pointStream.readPoint3F();
    assert(point.x == 1.0f && point.y == 2.0f && point.z == 3.0f);
    V12BitWriter compressedPointWriter;
    compressedPointWriter.writeUnsigned(0, 2); // 16-bit deltas
    compressedPointWriter.writeFlag(false); compressedPointWriter.writeUnsigned(100, 15);
    compressedPointWriter.writeFlag(true); compressedPointWriter.writeUnsigned(200, 15);
    compressedPointWriter.writeFlag(false); compressedPointWriter.writeUnsigned(300, 15);
    V12BitStream compressedPointStream(compressedPointWriter.data().data(),
                                       compressedPointWriter.data().size());
    const V12Vec3 compressedPoint = compressedPointStream.readCompressedPoint({1, 2, 3});
    assert(compressedPoint.x == 2.0f && compressedPoint.y == 0.0f &&
           compressedPoint.z == 6.0f);

    V12BitWriter textWriter;
    textWriter.writeFlag(false); // raw, uncompressed Huffman buffer
    textWriter.writeUnsigned(2, 8);
    textWriter.writeUnsigned('T', 8);
    textWriter.writeUnsigned('2', 8);
    V12BitStream textStream(textWriter.data().data(), textWriter.data().size());
    assert(textStream.readHuffmanString() == "T2");

    V12BitWriter compressedTextWriter;
    assert(compressedTextWriter.writeHuffmanString("Tribes 2 observer"));
    V12BitStream compressedTextStream(compressedTextWriter.data().data(), compressedTextWriter.data().size());
    assert(compressedTextStream.readHuffmanString() == "Tribes 2 observer");
    assert(!compressedTextWriter.writeHuffmanString(std::string(256, 'x')));
    V12BitWriter sharedStringWriter;
    sharedStringWriter.writeFlag(false);
    sharedStringWriter.writeHuffmanString("Player");
    sharedStringWriter.writeFlag(true);
    sharedStringWriter.writeUnsigned(3, 8);
    sharedStringWriter.writeHuffmanString("yer");
    V12BitStream sharedStringStream(sharedStringWriter.data().data(),
                                    sharedStringWriter.data().size());
    sharedStringStream.setStringBuffer(true);
    assert(sharedStringStream.readString() == "Player");
    assert(sharedStringStream.readString() == "Player");
    assert(!sharedStringStream.failed());

    V12BitWriter netStringWriter;
    assert(netStringWriter.writeNetString("RemoteCommand"));
    V12BitStream netStringStream(netStringWriter.data().data(), netStringWriter.data().size());
    assert(netStringStream.unpackNetString() == "RemoteCommand");

    V12BitStream truncated(packed, sizeof(packed));
    assert(truncated.readUnsigned(17) == 0);
    assert(truncated.failed());

    auto writeShapeBase = [](V12BitWriter& w) {
        w.writeFlag(false); w.writeHuffmanString("");
        for (int i = 0; i < 9; ++i) w.writeFlag(false);
        w.writeHuffmanString(""); w.writeFlag(false); w.writeFlag(false);
        w.writeHuffmanString(""); w.writeHuffmanString("");
        for (int i = 0; i < 6; ++i) w.writeFlag(false);
        for (int i = 0; i < 4; ++i) w.writeFlag(false);
        for (int i = 0; i < 3; ++i) w.writeFlag(false);
        w.writeUnsigned(0, 32); w.writeFlag(false);
        for (int i = 0; i < 8; ++i) w.writeFlag(false);
    };
    V12BitWriter gameBase;
    gameBase.writeFlag(true); // sentinel after the zero-bit GameBaseData payload
    V12BitStream gameBaseStream(gameBase.data().data(), gameBase.data().size());
    assert(V12::readDataBlockPayload(gameBaseStream, 18));
    assert(gameBaseStream.readFlag() && !gameBaseStream.failed());

    V12BitWriter shapeBaseWriter;
    writeShapeBase(shapeBaseWriter); shapeBaseWriter.writeFlag(true);
    V12BitStream shapeBaseStream(shapeBaseWriter.data().data(), shapeBaseWriter.data().size());
    assert(V12::readDataBlockPayload(shapeBaseStream, 37));
    assert(shapeBaseStream.readFlag() && !shapeBaseStream.failed());

    V12BitWriter shapeMetadataWriter;
    shapeMetadataWriter.writeFlag(false);
    shapeMetadataWriter.writeHuffmanString("shapes/native.dts");
    for (int i = 0; i < 9; ++i) shapeMetadataWriter.writeFlag(false);
    shapeMetadataWriter.writeHuffmanString("");
    shapeMetadataWriter.writeFlag(false); shapeMetadataWriter.writeFlag(false);
    shapeMetadataWriter.writeHuffmanString("");
    shapeMetadataWriter.writeHuffmanString("");
    for (int i = 0; i < 6; ++i) shapeMetadataWriter.writeFlag(false);
    for (int i = 0; i < 4; ++i) shapeMetadataWriter.writeFlag(false);
    for (int i = 0; i < 3; ++i) shapeMetadataWriter.writeFlag(false);
    shapeMetadataWriter.writeUnsigned(0, 32);
    shapeMetadataWriter.writeFlag(false);
    for (int i = 0; i < 8; ++i) shapeMetadataWriter.writeFlag(false);
    V12BitStream shapeMetadataStream(shapeMetadataWriter.data().data(),
                                     shapeMetadataWriter.data().size());
    V12::DecodedDataBlock shapeMetadata;
    assert(V12::readDataBlockPayload(shapeMetadataStream, 37, &shapeMetadata));
    assert(shapeMetadata.shapeFile == "shapes/native.dts");
    assert(!shapeMetadataStream.failed());

    V12BitWriter playerWriter;
    writeShapeBase(playerWriter); playerWriter.writeFlag(false);
    for (int i = 0; i < 13; ++i) playerWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 2; ++i) playerWriter.writeFlag(false);
    for (int i = 0; i < 9 + 1 + 8; ++i) playerWriter.writeUnsigned(0, 32);
    playerWriter.writeUnsigned(0, 7);
    for (int i = 0; i < 6 + 9 + 1; ++i) playerWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 32; ++i) playerWriter.writeFlag(false);
    for (int i = 0; i < 3; ++i) playerWriter.writeUnsigned(0, 32);
    playerWriter.writeFlag(false); playerWriter.writeUnsigned(0, 32); playerWriter.writeUnsigned(0, 32);
    playerWriter.writeFlag(false); playerWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 2; ++i) playerWriter.writeFlag(false);
    for (int i = 0; i < 3; ++i) playerWriter.writeFlag(false);
    for (int i = 0; i < 11; ++i) playerWriter.writeUnsigned(0, 32);
    playerWriter.writeUnsigned(0x15, 5);
    V12BitStream playerStream(playerWriter.data().data(), playerWriter.data().size());
    assert(V12::readDataBlockPayload(playerStream, 30));
    const uint32_t playerSentinel = playerStream.readUnsigned(5);
    assert(playerSentinel == 0x15 && !playerStream.failed());

    V12BitWriter projectileWriter;
    projectileWriter.writeHuffmanString("");
    projectileWriter.writeUnsigned(0, 32); projectileWriter.writeUnsigned(0, 32);
    projectileWriter.writeFlag(false); projectileWriter.writeFlag(false);
    for (int i = 0; i < 15; ++i) projectileWriter.writeFlag(false);
    projectileWriter.writeFlag(false); projectileWriter.writeFlag(false);
    projectileWriter.writeUnsigned(0, 8); projectileWriter.writeUnsigned(0, 32);
    projectileWriter.writeUnsigned(0x12, 5);
    V12BitStream projectileStream(projectileWriter.data().data(), projectileWriter.data().size());
    assert(V12::readDataBlockPayload(projectileStream, 32));
    assert(projectileStream.readUnsigned(5) == 0x12 && !projectileStream.failed());

    V12::DnetHeader expected;
    expected.gameFlag = true;
    expected.connectSequenceBit = true;
    expected.sequence = 257;
    expected.highestAck = 128;
    expected.packetType = V12::PacketType::Data;
    expected.ackByteCount = 2;
    expected.ackMask = 0x1234;
    V12BitWriter writer;
    V12::writeDnetHeader(writer, expected);
    V12BitStream packet(writer.data().data(), writer.data().size());
    V12::DnetHeader actual;
    assert(V12::readDnetHeader(packet, actual));
    assert(actual.gameFlag == expected.gameFlag);
    assert(actual.connectSequenceBit == expected.connectSequenceBit);
    assert(actual.sequence == expected.sequence);
    assert(actual.highestAck == expected.highestAck);
    assert(actual.packetType == expected.packetType);
    assert(actual.ackByteCount == expected.ackByteCount);
    assert(actual.ackMask == expected.ackMask);

    V12::RateInfo rateExpected;
    rateExpected.hasCurrent = true;
    rateExpected.updateDelay = 32;
    rateExpected.packetSize = 450;
    rateExpected.hasMaximum = true;
    rateExpected.maxUpdateDelay = 125;
    rateExpected.maxPacketSize = 1023; // 10-bit wire field maximum
    V12BitWriter rateWriter;
    V12::writeRateInfo(rateWriter, rateExpected);
    V12BitStream rateStream(rateWriter.data().data(), rateWriter.data().size());
    V12::RateInfo rateActual;
    assert(V12::readRateInfo(rateStream, rateActual));
    assert(rateActual.hasCurrent && rateActual.updateDelay == 32 && rateActual.packetSize == 450);
    assert(rateActual.hasMaximum && rateActual.maxUpdateDelay == 125 && rateActual.maxPacketSize == 1023);

    V12::ReceiveWindow window(false);
    assert(window.accept(10, false));
    assert(window.accept(11, false));
    assert(window.highestSequence() == 11);
    assert((window.acknowledgementMask() & 1u) != 0);
    assert(!window.accept(11, false));
    assert(window.accept(13, false));
    assert((window.acknowledgementMask() & 2u) != 0);
    assert(!window.accept(12, true));

    V12::ProtocolState protocol(2);
    protocol.noteSentDataPacket();
    V12::DnetHeader received;
    received.connectSequenceBit = false;
    received.sequence = 7;
    received.highestAck = 1;
    received.packetType = V12::PacketType::Data;
    received.ackByteCount = 1;
    received.ackMask = 1;
    auto protocolResult = protocol.processReceived(received);
    assert(protocolResult.accepted && protocolResult.dispatchData);
    assert(protocol.established());
    assert(protocolResult.acknowledgements.size() == 1 &&
           protocolResult.acknowledgements[0].sequence == 1 &&
           protocolResult.acknowledgements[0].acknowledged);
    assert(!protocol.processReceived(received).dispatchData);
    protocol.noteSentDataPacket();
    received.highestAck = 2;
    received.ackMask = 0;
    auto lostResult = protocol.processReceived(received);
    assert(lostResult.accepted && lostResult.acknowledgements.size() == 1 &&
           lostResult.acknowledgements[0].sequence == 2 &&
           !lostResult.acknowledgements[0].acknowledged);
    auto nativePing = protocol.buildPacket(V12::PacketType::Ping);
    V12BitStream nativePingStream(nativePing.data(), nativePing.size());
    V12::DnetHeader nativePingHeader;
    assert(V12::readDnetHeader(nativePingStream, nativePingHeader));
    assert(nativePingHeader.gameFlag);
    assert(nativePingHeader.packetType == V12::PacketType::Ping);
    assert(nativePingHeader.sequence == 2);
    assert(nativePingHeader.highestAck == 7);
    V12::ClientPacketOptions clientOptions;
    clientOptions.moveStart = 12;
    clientOptions.advertiseMaxRate = true;
    clientOptions.maxUpdateDelay = 32;
    clientOptions.maxPacketSize = 450;
    V12::ClientMove move;
    move.x = 1.0f;
    move.yaw = 0.25f;
    move.trigger[0] = true;
    clientOptions.moves.push_back(move);
    clientOptions.events.push_back({17, 9, [](V12BitWriter& event) {
        event.writeUnsigned(0x1234, 16);
    }});
    auto clientPacket = protocol.buildClientPacket(clientOptions);
    V12BitStream clientStream(clientPacket.data(), clientPacket.size());
    V12::DnetHeader clientHeader;
    assert(V12::readDnetHeader(clientStream, clientHeader));
    assert(clientHeader.packetType == V12::PacketType::Data);
    assert(clientHeader.sequence == 3);
    assert(clientStream.readFlag() == false); // current rate
    assert(clientStream.readFlag());
    assert(clientStream.readUnsigned(10) == 32);
    assert(clientStream.readUnsigned(10) == 450);
    assert(!clientStream.readFlag()); // first person
    assert(clientStream.readUnsigned(32) == 0);
    assert(clientStream.readUnsigned(32) == 12);
    assert(clientStream.readUnsigned(5) == 1);
    assert(clientStream.readFlag()); // yaw present
    assert(clientStream.readUnsigned(16) != 0);
    assert(!clientStream.readFlag()); // pitch absent
    assert(!clientStream.readFlag()); // roll absent
    assert(clientStream.readUnsigned(6) == 32);
    assert(clientStream.readUnsigned(6) == 16);
    assert(clientStream.readUnsigned(6) == 16);
    assert(!clientStream.readFlag()); // free look
    assert(clientStream.readFlag());
    for (int i = 1; i < 6; ++i) assert(!clientStream.readFlag());
    assert(!clientStream.readFlag()); // FOV unchanged
    assert(!clientStream.readFlag()); // no unguaranteed events
    assert(clientStream.readFlag()); // guaranteed event
    assert(!clientStream.readFlag()); // explicit sequence
    assert(clientStream.readUnsigned(7) == 17);
    assert(clientStream.readUnsigned(6) == 9);
    assert(clientStream.readUnsigned(16) == 0x1234);
    assert(!clientStream.readFlag()); // end guaranteed events
    assert(!clientStream.failed());
    V12::ClientPacketOptions oversizedOptions;
    for (int i = 0; i < 4; ++i) {
        oversizedOptions.events.push_back({(uint8_t)i, 9, [](V12BitWriter& event) {
            event.writeUnsigned(0, 3000);
        }});
    }
    const auto boundedPacket = protocol.buildClientPacket(oversizedOptions);
    assert(boundedPacket.size() <= 450);
    V12BitWriter serverPrefixWriter;
    serverPrefixWriter.writeUnsigned(0, 2); // server rate field width
    for (int i = 0; i < 4; ++i) serverPrefixWriter.writeUnsigned(0, 7);
    serverPrefixWriter.writeUnsigned(42, 32); // last move ack
    serverPrefixWriter.writeFlag(false); // damage/whiteout
    serverPrefixWriter.writeFlag(false); // self lock state
    serverPrefixWriter.writeFlag(false); // seeker state
    serverPrefixWriter.writeFlag(false); // pinged
    serverPrefixWriter.writeFlag(false); // jammed
    serverPrefixWriter.writeFlag(false); // control object
    serverPrefixWriter.writeFlag(false); // target visibility terminator
    serverPrefixWriter.writeFlag(false); // camera FOV
    serverPrefixWriter.writeFlag(false); // no unguaranteed events
    serverPrefixWriter.writeFlag(false); // no guaranteed events
    V12BitStream serverPrefixStream(serverPrefixWriter.data().data(),
                                    serverPrefixWriter.data().size());
    V12::ServerGameState serverState;
    V12::NetStringTable prefixStrings;
    std::vector<V12::ServerEvent> noServerEvents;
    size_t serverEventsEnd = 0;
    assert(V12::readServerPacketEvents(serverPrefixStream, prefixStrings,
                                       noServerEvents, &serverState, nullptr,
                                       &serverEventsEnd));
    assert(noServerEvents.empty() && serverState.lastMoveAck == 42);
    assert(serverEventsEnd == serverPrefixStream.position());
    V12::ProtocolState serverProtocol(5);
    V12::ServerPacketOptions serverPacketOptions;
    serverPacketOptions.ghosts.push_back({3, 25, true, false,
        [](V12BitWriter& writer) {
            writer.writeFlag(false); writer.writeFlag(false); writer.writeFlag(false);
            writer.writeFlag(false); writer.writeFlag(false); writer.writeFlag(false);
            writer.writeFlag(false); writer.writeFlag(false);
            writer.writeUnsigned(31, 5);
        }});
    const auto serverPacket = serverProtocol.buildServerPacket(serverPacketOptions);
    V12BitStream serverPacketStream(serverPacket.data(), serverPacket.size());
    V12::DnetHeader serverPacketHeader;
    assert(V12::readDnetHeader(serverPacketStream, serverPacketHeader));
    std::vector<V12::ServerEvent> serverPacketEvents;
    size_t serverPacketEventsEnd = 0;
    assert(V12::readServerPacketEvents(serverPacketStream, prefixStrings,
                                       serverPacketEvents, nullptr, nullptr,
                                       &serverPacketEventsEnd));
    V12BitStream serverGhostStream(serverPacket.data(), serverPacket.size(),
                                   serverPacketEventsEnd);
    V12::GhostTracker serverPacketGhosts;
    std::vector<V12::GhostUpdate> serverPacketUpdates;
    assert(V12::readGhostUpdates(serverGhostStream, serverPacketGhosts,
        serverPacketUpdates, [](V12BitStream& ghost, uint16_t, uint16_t classId, bool initial) {
            return classId == 25 && V12::readPlayerGhostPayload(ghost, initial, {}, nullptr);
        }));
    assert(serverPacketUpdates.size() == 1 &&
           serverPacketUpdates[0].operation == V12::GhostUpdate::Operation::Create);
    V12::ServerPacketOptions serverUpdateOptions;
    serverUpdateOptions.ghosts.push_back({3, 25, false, false,
        [](V12BitWriter& writer) {
            writer.writeFlag(false); writer.writeFlag(false); writer.writeFlag(false);
            writer.writeFlag(false); writer.writeFlag(false); writer.writeFlag(false);
            writer.writeFlag(false); writer.writeFlag(false);
            writer.writeUnsigned(31, 5);
        }});
    const auto serverUpdatePacket = serverProtocol.buildServerPacket(serverUpdateOptions);
    V12BitStream serverUpdateStream(serverUpdatePacket.data(), serverUpdatePacket.size());
    V12::DnetHeader serverUpdateHeader;
    assert(V12::readDnetHeader(serverUpdateStream, serverUpdateHeader));
    size_t serverUpdateEventsEnd = 0;
    std::vector<V12::ServerEvent> serverUpdateEvents;
    assert(V12::readServerPacketEvents(serverUpdateStream, prefixStrings,
                                       serverUpdateEvents, nullptr, nullptr,
                                       &serverUpdateEventsEnd));
    V12BitStream serverGhostUpdateStream(serverUpdatePacket.data(),
                                         serverUpdatePacket.size(),
                                         serverUpdateEventsEnd);
    serverPacketUpdates.clear();
    assert(V12::readGhostUpdates(serverGhostUpdateStream, serverPacketGhosts,
        serverPacketUpdates, [](V12BitStream& ghost, uint16_t, uint16_t classId, bool initial) {
            return classId == 25 && V12::readPlayerGhostPayload(ghost, initial, {}, nullptr);
        }));
    assert(serverUpdateOptions.emittedGhosts.size() == 1 &&
           serverPacketUpdates.size() == 1 &&
           serverPacketUpdates[0].operation == V12::GhostUpdate::Operation::Update);
    V12::ServerPacketOptions serverDeleteOptions;
    serverDeleteOptions.ghosts.push_back({3, 0, false, true, {}});
    const auto serverDeletePacket = serverProtocol.buildServerPacket(serverDeleteOptions);
    V12BitStream serverDeleteStream(serverDeletePacket.data(), serverDeletePacket.size());
    V12::DnetHeader serverDeleteHeader;
    assert(V12::readDnetHeader(serverDeleteStream, serverDeleteHeader));
    size_t serverDeleteEventsEnd = 0;
    std::vector<V12::ServerEvent> serverDeleteEvents;
    assert(V12::readServerPacketEvents(serverDeleteStream, prefixStrings,
                                       serverDeleteEvents, nullptr, nullptr,
                                       &serverDeleteEventsEnd));
    V12BitStream serverGhostDeleteStream(serverDeletePacket.data(),
                                         serverDeletePacket.size(),
                                         serverDeleteEventsEnd);
    serverPacketUpdates.clear();
    assert(V12::readGhostUpdates(serverGhostDeleteStream, serverPacketGhosts,
        serverPacketUpdates, [](V12BitStream&, uint16_t, uint16_t, bool) {
            return true;
        }));
    assert(serverDeleteOptions.emittedGhosts.size() == 1 &&
           serverPacketUpdates.size() == 1 &&
           serverPacketUpdates[0].operation == V12::GhostUpdate::Operation::Delete &&
           serverPacketGhosts.get(3) == nullptr);
    V12::NetStringTable commandStrings;
    auto commandEvents = V12::buildRemoteCommandEvents(
        commandStrings, "setPlayerTeam", {"0"});
    assert(commandEvents.size() == 2);
    assert(commandEvents[0].classId == 7);
    V12BitWriter netStringPayload;
    commandEvents[0].write(netStringPayload);
    V12BitStream netStringPayloadStream(netStringPayload.data().data(),
                                         netStringPayload.data().size());
    assert(netStringPayloadStream.readUnsigned(10) == 1);
    assert(netStringPayloadStream.readFlag());
    assert(!netStringPayloadStream.readFlag());
    assert(netStringPayloadStream.readHuffmanString() == "setPlayerTeam");
    V12BitWriter commandPayload;
    commandEvents[1].write(commandPayload);
    V12BitStream commandPayloadStream(commandPayload.data().data(),
                                      commandPayload.data().size());
    assert(commandPayloadStream.readUnsigned(5) == 2);
    assert(commandPayloadStream.readUnsigned(2) == 2);
    assert(commandPayloadStream.readUnsigned(10) == 1);
    assert(commandPayloadStream.readUnsigned(2) == 3);
    assert(!commandPayloadStream.readFlag());
    assert(commandPayloadStream.readFlag());
    assert(commandPayloadStream.readUnsigned(7) == 0);
    assert(!commandPayloadStream.failed());
    auto repeatedCommand = V12::buildRemoteCommandEvents(
        commandStrings, "setPlayerTeam", {"1"});
    assert(repeatedCommand.size() == 1);
    V12::NetStringTable serverStrings;
    V12BitWriter serverEventsWriter;
    serverEventsWriter.writeFlag(false); // no unguaranteed events
    serverEventsWriter.writeFlag(true);  // guaranteed list continues
    V12::EventHeader netEvent{true, false, 7, 1};
    V12::writeEventHeader(serverEventsWriter, true, netEvent);
    serverEventsWriter.writeUnsigned(1, 10);
    serverEventsWriter.writeFlag(true);
    serverEventsWriter.writeHuffmanString("setPlayerTeam");
    serverEventsWriter.writeFlag(true); // another guaranteed event
    V12::EventHeader remoteEvent{true, false, 9, 2};
    V12::writeEventHeader(serverEventsWriter, true, remoteEvent);
    commandEvents[1].write(serverEventsWriter);
    serverEventsWriter.writeFlag(false); // end guaranteed list
    V12BitStream serverEventsStream(serverEventsWriter.data().data(),
                                    serverEventsWriter.data().size());
    std::vector<V12::ServerEvent> decodedEvents;
    assert(V12::readServerEvents(serverEventsStream, serverStrings, decodedEvents));
    assert(decodedEvents.size() == 2);
    assert(decodedEvents[0].classId == 7);
    assert(decodedEvents[1].classId == 9);
    assert(decodedEvents[1].message == "setPlayerTeam 0");
    V12BitWriter mixedEventsWriter;
    mixedEventsWriter.writeFlag(false);
    mixedEventsWriter.writeFlag(true);
    V12::EventHeader datablockEvent{true, false, 19, 1};
    V12::writeEventHeader(mixedEventsWriter, true, datablockEvent);
    mixedEventsWriter.writeFlag(false); // SimDataBlockEvent mProcess
    mixedEventsWriter.writeFlag(true);
    V12::EventHeader mixedNetEvent{true, false, 7, 2};
    V12::writeEventHeader(mixedEventsWriter, true, mixedNetEvent);
    mixedEventsWriter.writeUnsigned(1, 10);
    mixedEventsWriter.writeFlag(true);
    mixedEventsWriter.writeHuffmanString("setPlayerTeam");
    mixedEventsWriter.writeFlag(true);
    V12::EventHeader mixedRemoteEvent{true, false, 9, 3};
    V12::writeEventHeader(mixedEventsWriter, true, mixedRemoteEvent);
    commandEvents[1].write(mixedEventsWriter);
    mixedEventsWriter.writeFlag(false);
    V12BitStream mixedEventsStream(mixedEventsWriter.data().data(),
                                   mixedEventsWriter.data().size());
    V12::NetStringTable mixedStrings;
    std::vector<V12::ServerEvent> mixedEvents;
    assert(V12::readServerEvents(mixedEventsStream, mixedStrings, mixedEvents));
    assert(mixedEvents.size() == 3 && mixedEvents[0].classId == 19 &&
           mixedEvents[2].message == "setPlayerTeam 0");
    assert(!mixedEvents[0].datablockProcess && !mixedEvents[0].hasDatablock);
    V12BitWriter processedDbWriter;
    processedDbWriter.writeFlag(false);
    processedDbWriter.writeFlag(true);
    V12::EventHeader processedDbHeader{true, false, 19, 1};
    V12::writeEventHeader(processedDbWriter, true, processedDbHeader);
    processedDbWriter.writeFlag(true); // mProcess
    processedDbWriter.writeUnsigned(4, 11);
    processedDbWriter.writeUnsigned(18, 7); // GameBaseData
    processedDbWriter.writeUnsigned(0, 11);
    processedDbWriter.writeUnsigned(1, 12);
    processedDbWriter.writeFlag(true);
    V12::EventHeader processedRemoteHeader{true, false, 9, 2};
    V12::writeEventHeader(processedDbWriter, true, processedRemoteHeader);
    commandEvents[1].write(processedDbWriter);
    processedDbWriter.writeFlag(false);
    V12BitStream processedDbStream(processedDbWriter.data().data(),
                                   processedDbWriter.data().size());
    V12::NetStringTable processedDbStrings;
    processedDbStrings.set(1, "setPlayerTeam");
    std::vector<V12::ServerEvent> processedDbEvents;
    assert(V12::readServerEvents(processedDbStream, processedDbStrings,
                                 processedDbEvents));
    assert(processedDbEvents.size() == 2 && processedDbEvents[0].hasDatablock &&
           processedDbEvents[0].datablockClassName == "GameBaseData" &&
           processedDbEvents[1].message == "setPlayerTeam 0");
    V12BitWriter ghostingEventWriter;
    ghostingEventWriter.writeFlag(false);
    ghostingEventWriter.writeFlag(true);
    V12::EventHeader ghostingHeader{true, false, 4, 8};
    V12::writeEventHeader(ghostingEventWriter, true, ghostingHeader);
    auto ghostingEvent = V12::makeGhostingMessageEvent(0x12345678, 0, 321);
    ghostingEvent.write(ghostingEventWriter);
    ghostingEventWriter.writeFlag(false);
    V12BitStream ghostingEventStream(ghostingEventWriter.data().data(),
                                     ghostingEventWriter.data().size());
    std::vector<V12::ServerEvent> ghostingEvents;
    assert(V12::readServerEvents(ghostingEventStream, mixedStrings, ghostingEvents));
    assert(ghostingEvents.size() == 1 && ghostingEvents[0].hasGhostingMessage);
    assert(ghostingEvents[0].ghostSequence == 0x12345678 &&
           ghostingEvents[0].ghostMessage == 0 && ghostingEvents[0].ghostCount == 321);
    V12BitWriter alwaysEventWriter;
    alwaysEventWriter.writeFlag(false);
    alwaysEventWriter.writeFlag(true);
    V12::EventHeader alwaysHeader{true, false, 3, 4};
    V12::writeEventHeader(alwaysEventWriter, true, alwaysHeader);
    alwaysEventWriter.writeUnsigned(77, 10);
    alwaysEventWriter.writeFlag(false);
    alwaysEventWriter.writeFlag(false);
    V12BitStream alwaysEventStream(alwaysEventWriter.data().data(),
                                   alwaysEventWriter.data().size());
    std::vector<V12::ServerEvent> alwaysEvents;
    assert(V12::readServerEvents(alwaysEventStream, mixedStrings, alwaysEvents));
    assert(alwaysEvents.size() == 1 && alwaysEvents[0].hasGhostAlways &&
           alwaysEvents[0].ghostAlwaysIndex == 77 &&
           !alwaysEvents[0].ghostAlwaysHasData);

    static_assert(V12::GhostClassCount == 53);
    static_assert(V12::DataBlockClassCount == 54);
    static_assert(V12::EventClassCount == 26);
    static_assert(V12::GhostClassNames[12] == "GameBase");
    static_assert(V12::GhostClassNames[31] == "ShapeBase");
    static_assert(V12::GhostClassNames[47] == "Trigger");
    static_assert(V12::GhostClassNames[49] == "VehicleBlocker");
    static_assert(V12::GhostClassNames[50] == "WaterBlock");
    static_assert(V12::GhostClassNames[51] == "WayPoint");
    static_assert(V12::GhostClassNames[52] == "WheeledVehicle");
    assert(std::string_view(V12::ghostClassName(25)) == "Player");
    assert(std::string_view(V12::dataBlockClassName(30)) == "PlayerData");
    assert(std::string_view(V12::eventClassName(9)) == "RemoteCommandEvent");
    assert(V12::ghostClassName(53) == nullptr);

    V12::EventHeader eventExpected;
    eventExpected.guaranteed = true;
    eventExpected.classId = 9;
    eventExpected.sequence = 73;
    V12BitWriter eventWriter;
    eventWriter.writeFlag(true); // guaranteed-list continuation
    V12::writeEventHeader(eventWriter, true, eventExpected);
    V12BitStream eventStream(eventWriter.data().data(), eventWriter.data().size());
    V12::EventHeader eventActual;
    assert(eventStream.readFlag());
    assert(V12::readEventHeader(eventStream, true, eventActual));
    assert(eventActual.guaranteed);
    assert(eventActual.classId == 9);
    assert(eventActual.sequence == 73);

    V12::NetStringTable strings;
    assert(strings.set(12, "PlayerName"));
    assert(strings.get(12) && *strings.get(12) == "PlayerName");
    assert(!strings.set(4096, "out-of-range"));
    strings.clear();
    assert(strings.get(12) == nullptr);

    V12::GhostTracker ghosts;
    assert(ghosts.create(4, 25));
    assert(!ghosts.create(4, 25));
    assert(ghosts.update(4));
    assert(ghosts.get(4) != nullptr);
    assert(std::string_view(ghosts.get(4)->className) == "Player");
    assert(ghosts.erase(4));
    assert(!ghosts.update(4));
    assert(!ghosts.create(1024, 25));
    assert(!ghosts.create(4, 53));

    V12BitWriter ghostWriter;
    ghostWriter.writeFlag(true); // ghost section present
    ghostWriter.writeUnsigned(0, 3); // ID width = 3 + 0
    ghostWriter.writeFlag(true); // one ghost
    ghostWriter.writeUnsigned(3, 3); // index
    ghostWriter.writeFlag(false); // create
    ghostWriter.writeUnsigned(25, 7); // Player
    ghostWriter.writeFlag(true); // payload callback consumes one bit
    ghostWriter.writeFlag(false); // end ghost list
    V12BitStream ghostStream(ghostWriter.data().data(), ghostWriter.data().size());
    V12::GhostTracker packetGhosts;
    std::vector<V12::GhostUpdate> updates;
    assert(V12::readGhostUpdates(ghostStream, packetGhosts, updates,
        [](V12BitStream& stream, uint16_t, uint16_t, bool) {
            stream.readFlag();
            return !stream.failed();
        }));
    assert(updates.size() == 1 && updates[0].operation == V12::GhostUpdate::Operation::Create);
    assert(packetGhosts.get(3) && packetGhosts.get(3)->classId == 25);
    V12BitWriter ghostUpdateWriter;
    ghostUpdateWriter.writeFlag(true);
    ghostUpdateWriter.writeUnsigned(0, 3);
    ghostUpdateWriter.writeFlag(true);
    ghostUpdateWriter.writeUnsigned(3, 3);
    ghostUpdateWriter.writeFlag(false); // update
    ghostUpdateWriter.writeFlag(false); // payload value
    ghostUpdateWriter.writeFlag(false); // end ghost list
    V12BitStream ghostUpdateStream(ghostUpdateWriter.data().data(),
                                   ghostUpdateWriter.data().size());
    updates.clear();
    assert(V12::readGhostUpdates(ghostUpdateStream, packetGhosts, updates,
        [](V12BitStream& stream, uint16_t, uint16_t, bool) {
            stream.readFlag();
            return !stream.failed();
        }));
    assert(updates.size() == 1 &&
           updates[0].operation == V12::GhostUpdate::Operation::Update);
    V12BitWriter ghostDeleteWriter;
    ghostDeleteWriter.writeFlag(true);
    ghostDeleteWriter.writeUnsigned(0, 3);
    ghostDeleteWriter.writeFlag(true);
    ghostDeleteWriter.writeUnsigned(3, 3);
    ghostDeleteWriter.writeFlag(true); // delete
    V12BitStream ghostDeleteStream(ghostDeleteWriter.data().data(),
                                   ghostDeleteWriter.data().size());
    updates.clear();
    assert(V12::readGhostUpdates(ghostDeleteStream, packetGhosts, updates));
    assert(updates.size() == 1 &&
           updates[0].operation == V12::GhostUpdate::Operation::Delete);
    assert(packetGhosts.get(3) == nullptr);
    V12BitWriter playerPayloadWriter;
    playerPayloadWriter.writeFlag(false); // GameBase mask 1
    playerPayloadWriter.writeFlag(false); // GameBase mask 2
    playerPayloadWriter.writeFlag(false); // ShapeBase mask
    playerPayloadWriter.writeFlag(false); // ImpactMask
    playerPayloadWriter.writeFlag(false); // ActionMask
    playerPayloadWriter.writeFlag(false); // ArmAction
    playerPayloadWriter.writeFlag(false); // control-object shortcut
    playerPayloadWriter.writeFlag(false); // MoveMask
    playerPayloadWriter.writeUnsigned(31, 5); // full energy
    V12BitStream playerPayloadStream(playerPayloadWriter.data().data(),
                                     playerPayloadWriter.data().size());
    V12::PlayerGhostState playerState;
    assert(V12::readPlayerGhostPayload(playerPayloadStream, false, {}, &playerState));
    assert(playerState.energy == 100.0f && !playerState.hasRotation);

    V12BitWriter staticShapeWriter;
    staticShapeWriter.writeFlag(false); // GameBase datablock
    staticShapeWriter.writeFlag(false); // GameBase target
    staticShapeWriter.writeFlag(false); // ShapeBase masks
    staticShapeWriter.writeFlag(false); // StaticShape position
    staticShapeWriter.writeFlag(true);  // powered
    staticShapeWriter.writeUnsigned(0x15, 5);
    V12BitStream staticShapeStream(staticShapeWriter.data().data(),
                                   staticShapeWriter.data().size());
    assert(V12::readGhostPayload(staticShapeStream, 39, false, {}));
    assert(staticShapeStream.readUnsigned(5) == 0x15 && !staticShapeStream.failed());

    V12BitWriter aiObjectiveWriter;
    aiObjectiveWriter.writeFlag(false); // GameBase datablock
    aiObjectiveWriter.writeFlag(false); // GameBase target
    aiObjectiveWriter.writeFlag(false); // ShapeBase masks
    aiObjectiveWriter.writeFlag(false); // MissionMarker position
    aiObjectiveWriter.writeFlag(false); // AIObjective sphere update
    aiObjectiveWriter.writeUnsigned(0x15, 5);
    V12BitStream aiObjectiveStream(aiObjectiveWriter.data().data(),
                                   aiObjectiveWriter.data().size());
    assert(V12::readGhostPayload(aiObjectiveStream, 0, true, {}));
    assert(aiObjectiveStream.readUnsigned(5) == 0x15 && !aiObjectiveStream.failed());

    V12BitWriter turretWriter;
    turretWriter.writeFlag(false); // GameBase datablock
    turretWriter.writeFlag(false); // GameBase target
    turretWriter.writeFlag(false); // ShapeBase masks
    turretWriter.writeFlag(false); // StaticShape position
    turretWriter.writeFlag(false); // powered
    turretWriter.writeFlag(false); // not controlled by this client
    turretWriter.writeFlag(true);  // turret orientation update
    turretWriter.writeUnsigned(0x155, 10);
    turretWriter.writeUnsigned(0x2aa, 10);
    turretWriter.writeUnsigned(0x55, 8);
    turretWriter.writeUnsigned(0x16, 5);
    V12BitStream turretStream(turretWriter.data().data(), turretWriter.data().size());
    assert(V12::readGhostPayload(turretStream, 48, false, {}));
    assert(turretStream.readUnsigned(5) == 0x16 && !turretStream.failed());

    V12BitWriter forceFieldWriter;
    forceFieldWriter.writeFlag(false); forceFieldWriter.writeFlag(false);
    forceFieldWriter.writeFlag(true); // initial transform
    forceFieldWriter.writeUnsigned(0, 2); // compressed-point delta size
    for (int i = 0; i < 3; ++i) {
        forceFieldWriter.writeFlag(false);
        forceFieldWriter.writeUnsigned(0, 15);
    }
    for (int i = 0; i < 3; ++i) forceFieldWriter.writeUnsigned(0, 32);
    forceFieldWriter.writeFlag(false); // quaternion sign
    for (int i = 0; i < 3; ++i) forceFieldWriter.writeUnsigned(0, 32); // scale
    forceFieldWriter.writeFlag(false); // state unchanged
    forceFieldWriter.writeUnsigned(0x15, 5);
    V12BitStream forceFieldStream(forceFieldWriter.data().data(), forceFieldWriter.data().size());
    assert(V12::readGhostPayload(forceFieldStream, 11, true, {}));
    assert(forceFieldStream.readUnsigned(5) == 0x15 && !forceFieldStream.failed());

    V12BitWriter forceFieldUpdateWriter;
    forceFieldUpdateWriter.writeFlag(false); forceFieldUpdateWriter.writeFlag(false);
    forceFieldUpdateWriter.writeFlag(false); // no initial transform
    forceFieldUpdateWriter.writeFlag(false); // no transform update
    forceFieldUpdateWriter.writeFlag(true); // state change
    forceFieldUpdateWriter.writeUnsigned(1, 2); // opening
    forceFieldUpdateWriter.writeUnsigned(0, 32); // position
    forceFieldUpdateWriter.writeUnsigned(0x16, 5);
    V12BitStream forceFieldUpdateStream(forceFieldUpdateWriter.data().data(),
                                        forceFieldUpdateWriter.data().size());
    assert(V12::readGhostPayload(forceFieldUpdateStream, 11, false, {}));
    assert(forceFieldUpdateStream.readUnsigned(5) == 0x16 &&
           !forceFieldUpdateStream.failed());

    V12BitWriter precipitationWriter;
    precipitationWriter.writeFlag(false); precipitationWriter.writeFlag(false);
    precipitationWriter.writeFlag(true); // initial state
    precipitationWriter.writeUnsigned(0, 32); // percentage
    precipitationWriter.writeUnsigned(0, 32); // color count
    for (int i = 0; i < 3; ++i) precipitationWriter.writeUnsigned(0, 32);
    precipitationWriter.writeUnsigned(0, 32); // max drops
    precipitationWriter.writeUnsigned(0, 32); // max radius
    precipitationWriter.writeFlag(false); // storm initialization
    precipitationWriter.writeFlag(false); // storm show
    precipitationWriter.writeFlag(false); // storm
    precipitationWriter.writeFlag(false); // percentage update
    precipitationWriter.writeUnsigned(0x15, 5);
    V12BitStream precipitationStream(precipitationWriter.data().data(),
                                     precipitationWriter.data().size());
    assert(V12::readGhostPayload(precipitationStream, 26, true, {}));
    assert(precipitationStream.readUnsigned(5) == 0x15 && !precipitationStream.failed());

    V12BitWriter precipitationUpdateWriter;
    precipitationUpdateWriter.writeFlag(false); precipitationUpdateWriter.writeFlag(false);
    precipitationUpdateWriter.writeFlag(false); // no initial state
    precipitationUpdateWriter.writeFlag(false); // storm show
    precipitationUpdateWriter.writeFlag(false); // storm
    precipitationUpdateWriter.writeFlag(false); // percentage update
    precipitationUpdateWriter.writeUnsigned(0x16, 5);
    V12BitStream precipitationUpdateStream(precipitationUpdateWriter.data().data(),
                                           precipitationUpdateWriter.data().size());
    assert(V12::readGhostPayload(precipitationUpdateStream, 26, false, {}));
    assert(precipitationUpdateStream.readUnsigned(5) == 0x16 &&
           !precipitationUpdateStream.failed());

    V12BitWriter baseGhostWriter;
    baseGhostWriter.writeFlag(false); baseGhostWriter.writeFlag(false);
    baseGhostWriter.writeUnsigned(0x15, 5);
    V12BitStream baseGhostStream(baseGhostWriter.data().data(), baseGhostWriter.data().size());
    assert(V12::readGhostPayload(baseGhostStream, 12, false, {}));
    assert(baseGhostStream.readUnsigned(5) == 0x15 && !baseGhostStream.failed());

    V12BitWriter shapeGhostWriter;
    shapeGhostWriter.writeFlag(false); shapeGhostWriter.writeFlag(false);
    shapeGhostWriter.writeFlag(false); shapeGhostWriter.writeUnsigned(0x15, 5);
    V12BitStream shapeGhostStream(shapeGhostWriter.data().data(), shapeGhostWriter.data().size());
    assert(V12::readGhostPayload(shapeGhostStream, 31, false, {}));
    assert(shapeGhostStream.readUnsigned(5) == 0x15 && !shapeGhostStream.failed());

    V12BitWriter triggerWriter;
    triggerWriter.writeUnsigned(0x12345678, 32); triggerWriter.writeUnsigned(0x15, 5);
    V12BitStream triggerStream(triggerWriter.data().data(), triggerWriter.data().size());
    assert(V12::readGhostPayload(triggerStream, 47, false, {}));
    assert(triggerStream.readUnsigned(5) == 0x15 && !triggerStream.failed());

    V12BitWriter blockerWriter;
    for (int i = 0; i < 22; ++i) blockerWriter.writeUnsigned(0, 32);
    blockerWriter.writeUnsigned(0x15, 5);
    V12BitStream blockerStream(blockerWriter.data().data(), blockerWriter.data().size());
    assert(V12::readGhostPayload(blockerStream, 49, false, {}));
    assert(blockerStream.readUnsigned(5) == 0x15 && !blockerStream.failed());

    V12BitWriter waterWriter;
    waterWriter.writeUnsigned(0, 2);
    for (int i = 0; i < 3; ++i) {
        waterWriter.writeFlag(false); waterWriter.writeUnsigned(0, 15);
    }
    for (int i = 0; i < 3; ++i) waterWriter.writeUnsigned(0, 32);
    waterWriter.writeFlag(false);
    for (int i = 0; i < 3; ++i) waterWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 4; ++i) {
        waterWriter.writeHuffmanString("");
    }
    waterWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 4; ++i) waterWriter.writeUnsigned(0, 32);
    waterWriter.writeUnsigned(0, 8); waterWriter.writeFlag(false);
    waterWriter.writeUnsigned(0x15, 5);
    V12BitStream waterStream(waterWriter.data().data(), waterWriter.data().size());
    assert(V12::readGhostPayload(waterStream, 50, false, {}));
    assert(waterStream.readUnsigned(5) == 0x15 && !waterStream.failed());

    V12BitWriter stationFXPersonalInitialWriter;
    stationFXPersonalInitialWriter.writeFlag(false); // GameBase datablock
    stationFXPersonalInitialWriter.writeFlag(false); // GameBase target
    stationFXPersonalInitialWriter.writeFlag(true);  // initial update
    stationFXPersonalInitialWriter.writeFlag(true);  // station object ghost
    stationFXPersonalInitialWriter.writeUnsigned(0x155, 11);
    stationFXPersonalInitialWriter.writeUnsigned(0x15, 5);
    V12BitStream stationFXPersonalInitialStream(
        stationFXPersonalInitialWriter.data().data(),
        stationFXPersonalInitialWriter.data().size());
    assert(V12::readGhostPayload(stationFXPersonalInitialStream, 40, true, {}));
    assert(stationFXPersonalInitialStream.readUnsigned(5) == 0x15 &&
           !stationFXPersonalInitialStream.failed());

    V12BitWriter stationFXVehicleUpdateWriter;
    stationFXVehicleUpdateWriter.writeFlag(false); // GameBase datablock
    stationFXVehicleUpdateWriter.writeFlag(false); // GameBase target
    stationFXVehicleUpdateWriter.writeUnsigned(0x16, 5);
    V12BitStream stationFXVehicleUpdateStream(
        stationFXVehicleUpdateWriter.data().data(),
        stationFXVehicleUpdateWriter.data().size());
    assert(V12::readGhostPayload(stationFXVehicleUpdateStream, 41, false, {}));
    assert(stationFXVehicleUpdateStream.readUnsigned(5) == 0x16 &&
           !stationFXVehicleUpdateStream.failed());

    V12BitWriter stationFXVehicleInitialWriter;
    stationFXVehicleInitialWriter.writeFlag(false); // GameBase datablock
    stationFXVehicleInitialWriter.writeFlag(false); // GameBase target
    stationFXVehicleInitialWriter.writeFlag(true);  // initial update
    stationFXVehicleInitialWriter.writeFlag(false); // no station object ghost
    stationFXVehicleInitialWriter.writeUnsigned(0x17, 5);
    V12BitStream stationFXVehicleInitialStream(
        stationFXVehicleInitialWriter.data().data(),
        stationFXVehicleInitialWriter.data().size());
    assert(V12::readGhostPayload(stationFXVehicleInitialStream, 41, true, {}));
    assert(stationFXVehicleInitialStream.readUnsigned(5) == 0x17 &&
           !stationFXVehicleInitialStream.failed());

    V12BitWriter fireballAtmosphereInitialWriter;
    fireballAtmosphereInitialWriter.writeFlag(false); // GameBase datablock
    fireballAtmosphereInitialWriter.writeFlag(false); // GameBase target
    fireballAtmosphereInitialWriter.writeFlag(true);  // initial update
    for (int i = 0; i < 9; ++i)
        fireballAtmosphereInitialWriter.writeUnsigned(0, 32);
    fireballAtmosphereInitialWriter.writeUnsigned(0x18, 5);
    V12BitStream fireballAtmosphereInitialStream(
        fireballAtmosphereInitialWriter.data().data(),
        fireballAtmosphereInitialWriter.data().size());
    assert(V12::readGhostPayload(fireballAtmosphereInitialStream, 8, true, {}));
    assert(fireballAtmosphereInitialStream.readUnsigned(5) == 0x18 &&
           !fireballAtmosphereInitialStream.failed());

    V12BitWriter fireballAtmosphereUpdateWriter;
    fireballAtmosphereUpdateWriter.writeFlag(false); // GameBase datablock
    fireballAtmosphereUpdateWriter.writeFlag(false); // GameBase target
    fireballAtmosphereUpdateWriter.writeUnsigned(0x19, 5);
    V12BitStream fireballAtmosphereUpdateStream(
        fireballAtmosphereUpdateWriter.data().data(),
        fireballAtmosphereUpdateWriter.data().size());
    assert(V12::readGhostPayload(fireballAtmosphereUpdateStream, 8, false, {}));
    assert(fireballAtmosphereUpdateStream.readUnsigned(5) == 0x19 &&
           !fireballAtmosphereUpdateStream.failed());

    V12BitWriter sunWriter;
    sunWriter.writeFlag(true); // Sun update mask
    for (int i = 0; i < 3; ++i) sunWriter.writeUnsigned(0, 32); // direction
    sunWriter.writeUnsigned(0, 8); // azimuth
    sunWriter.writeUnsigned(0, 8); // elevation
    for (int i = 0; i < 3; ++i) sunWriter.writeUnsigned(0, 8); // color
    sunWriter.writeFlag(false);
    sunWriter.writeFlag(false);
    sunWriter.writeUnsigned(0x15, 5); // unaligned sentinel
    V12BitStream sunStream(sunWriter.data().data(), sunWriter.data().size());
    assert(V12::readGhostPayload(sunStream, 42, false, {}));
    assert(sunStream.readUnsigned(5) == 0x15 && !sunStream.failed());

    V12BitWriter skyWriter;
    skyWriter.writeFlag(true); // initial update
    skyWriter.writeHuffmanString("sky/materials");
    for (int i = 0; i < 3; ++i) skyWriter.writeUnsigned(0, 32); // fog color
    skyWriter.writeUnsigned(1, 32); // one fog volume
    skyWriter.writeUnsigned(1, 8); // use sky textures
    skyWriter.writeUnsigned(0, 8); // render bottom texture
    for (int i = 0; i < 3; ++i) skyWriter.writeUnsigned(0, 32); // solid color
    skyWriter.writeUnsigned(1, 8); // effect precipitation
    for (int i = 0; i < 6; ++i) skyWriter.writeUnsigned(0, 32); // fog volume
    for (int i = 0; i < 3; ++i) {
        skyWriter.writeHuffmanString("");
        skyWriter.writeUnsigned(0, 32); // cloud height
        skyWriter.writeUnsigned(0, 32); // cloud speed
    }
    for (int i = 0; i < 3; ++i) skyWriter.writeUnsigned(0, 32); // wind velocity
    skyWriter.writeUnsigned(0, 32); // storm current
    skyWriter.writeFlag(true); // storm initialization
    for (int i = 0; i < 5; ++i) skyWriter.writeUnsigned(0, 32);
    skyWriter.writeFlag(true); skyWriter.writeUnsigned(1, 8); // storm clouds
    skyWriter.writeFlag(true); skyWriter.writeUnsigned(0, 8); // storm fog
    skyWriter.writeFlag(true); skyWriter.writeUnsigned(0, 32); skyWriter.writeUnsigned(0, 32);
    skyWriter.writeFlag(true); skyWriter.writeUnsigned(0, 32); skyWriter.writeUnsigned(0, 32);
    skyWriter.writeFlag(true); for (int i = 0; i < 3; ++i) skyWriter.writeUnsigned(0, 32);
    skyWriter.writeFlag(true); for (int i = 0; i < 4; ++i) skyWriter.writeUnsigned(0, 32);
    skyWriter.writeFlag(true); for (int i = 0; i < 3; ++i) skyWriter.writeUnsigned(0, 32);
    skyWriter.writeUnsigned(0x15, 5);
    V12BitStream skyStream(skyWriter.data().data(), skyWriter.data().size());
    assert(V12::readGhostPayload(skyStream, 35, true, {}));
    assert(skyStream.readUnsigned(5) == 0x15 && !skyStream.failed());

    V12BitWriter terrainWriter;
    terrainWriter.writeFlag(true); // initial update
    terrainWriter.writeUnsigned(0, 32); // CRC
    terrainWriter.writeHuffmanString("terrain.ter");
    terrainWriter.writeHuffmanString("detail");
    terrainWriter.writeUnsigned(0, 32); // square size
    terrainWriter.writeUnsigned(2, 32); // empty-square run count
    terrainWriter.writeUnsigned(0, 32); terrainWriter.writeUnsigned(1, 32);
    terrainWriter.writeUnsigned(0x16, 5);
    V12BitStream terrainStream(terrainWriter.data().data(), terrainWriter.data().size());
    assert(V12::readGhostPayload(terrainStream, 45, true, {}));
    assert(terrainStream.readUnsigned(5) == 0x16 && !terrainStream.failed());

    V12BitWriter terrainUpdateWriter;
    terrainUpdateWriter.writeFlag(false); // not initial
    terrainUpdateWriter.writeFlag(true); // empty mask
    terrainUpdateWriter.writeUnsigned(1, 32);
    terrainUpdateWriter.writeUnsigned(0, 32);
    terrainUpdateWriter.writeUnsigned(0x17, 5);
    V12BitStream terrainUpdateStream(terrainUpdateWriter.data().data(),
                                     terrainUpdateWriter.data().size());
    assert(V12::readGhostPayload(terrainUpdateStream, 45, false, {}));
    assert(terrainUpdateStream.readUnsigned(5) == 0x17 && !terrainUpdateStream.failed());

    V12BitWriter terrainBadCountWriter;
    terrainBadCountWriter.writeFlag(false);
    terrainBadCountWriter.writeFlag(true);
    terrainBadCountWriter.writeUnsigned(0xffffffffu, 32);
    V12BitStream terrainBadCountStream(terrainBadCountWriter.data().data(),
                                       terrainBadCountWriter.data().size());
    assert(!V12::readGhostPayload(terrainBadCountStream, 45, false, {}));

    V12BitWriter tsStaticWriter;
    for (int i = 0; i < 16; ++i) tsStaticWriter.writeUnsigned(0, 32); // MatrixF
    for (int i = 0; i < 3; ++i) tsStaticWriter.writeUnsigned(0, 32); // position
    tsStaticWriter.writeUnsigned(0x15, 5); // aligned sentinel
    V12BitStream tsStaticStream(tsStaticWriter.data().data(),
                                tsStaticWriter.data().size());
    assert(V12::readGhostPayload(tsStaticStream, 43, false, {}));
    assert(tsStaticStream.readUnsigned(5) == 0x15 && !tsStaticStream.failed());

    V12BitWriter bombWriter;
    bombWriter.writeFlag(false); // GameBase datablock
    bombWriter.writeFlag(false); // GameBase target
    bombWriter.writeFlag(false); // non-full bomb state
    bombWriter.writeFlag(false); // no bounce position
    bombWriter.writeFlag(false); // no endpoint
    bombWriter.writeUnsigned(0x12, 5);
    V12BitStream bombStream(bombWriter.data().data(), bombWriter.data().size());
    assert(V12::readGhostPayload(bombStream, 3, false, {}));
    assert(bombStream.readUnsigned(5) == 0x12 && !bombStream.failed());
    V12BitWriter seekerWriter;
    seekerWriter.writeFlag(false); // GameBase datablock
    seekerWriter.writeFlag(false); // GameBase target
    seekerWriter.writeFlag(false); // non-full state
    seekerWriter.writeFlag(false); // no explosion
    for (int i = 0; i < 6; ++i) seekerWriter.writeUnsigned(0, 32);
    seekerWriter.writeFlag(false); // no target info
    seekerWriter.writeUnsigned(0x12, 5);
    V12BitStream seekerStream(seekerWriter.data().data(), seekerWriter.data().size());
    assert(V12::readGhostPayload(seekerStream, 30, false, {}));
    assert(seekerStream.readUnsigned(5) == 0x12 && !seekerStream.failed());
    V12BitWriter elfWriter;
    elfWriter.writeFlag(false); elfWriter.writeFlag(false);
    elfWriter.writeFlag(false); elfWriter.writeUnsigned(0x12, 5);
    V12BitStream elfStream(elfWriter.data().data(), elfWriter.data().size());
    assert(V12::readGhostPayload(elfStream, 6, false, {}));
    assert(elfStream.readUnsigned(5) == 0x12 && !elfStream.failed());
    V12BitWriter repairWriter;
    repairWriter.writeFlag(false); repairWriter.writeFlag(false);
    repairWriter.writeFlag(false); repairWriter.writeUnsigned(0x12, 5);
    V12BitStream repairStream(repairWriter.data().data(), repairWriter.data().size());
    assert(V12::readGhostPayload(repairStream, 28, false, {}));
    assert(repairStream.readUnsigned(5) == 0x12 && !repairStream.failed());
    V12BitWriter targetWriter;
    targetWriter.writeFlag(false); targetWriter.writeFlag(false);
    targetWriter.writeFlag(false); // no source object
    for (int i = 0; i < 6; ++i) targetWriter.writeUnsigned(0, 32);
    targetWriter.writeFlag(false); // not truncated
    targetWriter.writeUnsigned(0x12, 5);
    V12BitStream targetStream(targetWriter.data().data(), targetWriter.data().size());
    assert(V12::readGhostPayload(targetStream, 44, false, {}));
    assert(targetStream.readUnsigned(5) == 0x12 && !targetStream.failed());

    V12BitWriter shockwaveWriter;
    shockwaveWriter.writeFlag(false); // GameBase datablock
    shockwaveWriter.writeFlag(false); // GameBase target
    shockwaveWriter.writeFlag(true);  // position/normal present
    for (int i = 0; i < 6; ++i) shockwaveWriter.writeUnsigned(0, 32);
    shockwaveWriter.writeUnsigned(0x12, 5);
    V12BitStream shockwaveStream(shockwaveWriter.data().data(),
                                 shockwaveWriter.data().size());
    assert(V12::readGhostPayload(shockwaveStream, 33, false, {}));
    assert(shockwaveStream.readUnsigned(5) == 0x12 && !shockwaveStream.failed());

    V12BitWriter splashWriter;
    splashWriter.writeFlag(false); // GameBase datablock
    splashWriter.writeFlag(false); // GameBase target
    splashWriter.writeFlag(true);  // position present
    for (int i = 0; i < 3; ++i) splashWriter.writeUnsigned(0, 32);
    splashWriter.writeUnsigned(0x12, 5);
    V12BitStream splashStream(splashWriter.data().data(), splashWriter.data().size());
    assert(V12::readGhostPayload(splashStream, 38, false, {}));
    assert(splashStream.readUnsigned(5) == 0x12 && !splashStream.failed());

    V12BitWriter missionAreaWriter;
    missionAreaWriter.writeFlag(true); // area update
    for (int i = 0; i < 4; ++i) missionAreaWriter.writeUnsigned(0, 32);
    missionAreaWriter.writeUnsigned(0, 32); // flight ceiling
    missionAreaWriter.writeUnsigned(0, 32); // flight ceiling range
    missionAreaWriter.writeUnsigned(0x12, 5);
    V12BitStream missionAreaStream(missionAreaWriter.data().data(),
                                   missionAreaWriter.data().size());
    assert(V12::readGhostPayload(missionAreaStream, 21, false, {}));
    assert(missionAreaStream.readUnsigned(5) == 0x12 && !missionAreaStream.failed());

    V12BitWriter simpleNetObjectWriter;
    simpleNetObjectWriter.writeHuffmanString("Hello World!");
    simpleNetObjectWriter.writeUnsigned(0x12, 5);
    V12BitStream simpleNetObjectStream(simpleNetObjectWriter.data().data(),
                                       simpleNetObjectWriter.data().size());
    assert(V12::readGhostPayload(simpleNetObjectStream, 34, false, {}));
    assert(simpleNetObjectStream.readUnsigned(5) == 0x12 &&
           !simpleNetObjectStream.failed());

    V12BitWriter linearFlareInitialWriter;
    linearFlareInitialWriter.writeFlag(false); // GameBase datablock
    linearFlareInitialWriter.writeFlag(false); // GameBase target
    linearFlareInitialWriter.writeFlag(true);  // InitialUpdateMask
    linearFlareInitialWriter.writeFlag(false); // live projectile
    linearFlareInitialWriter.writeUnsigned(0, 2); // compressed-point delta size
    for (int i = 0; i < 3; ++i) {
        linearFlareInitialWriter.writeFlag(false);
        linearFlareInitialWriter.writeUnsigned(0, 15);
    }
    linearFlareInitialWriter.writeUnsigned(0, 15); // direction phi
    linearFlareInitialWriter.writeUnsigned(0, 14); // direction theta
    linearFlareInitialWriter.writeUnsigned(0, 9); // current tick
    linearFlareInitialWriter.writeFlag(false); // no source
    linearFlareInitialWriter.writeFlag(false); // no vehicle
    linearFlareInitialWriter.writeUnsigned(0x12, 5);
    V12BitStream linearFlareInitialStream(linearFlareInitialWriter.data().data(),
                                           linearFlareInitialWriter.data().size());
    assert(V12::readGhostPayload(linearFlareInitialStream, 18, true, {}));
    assert(linearFlareInitialStream.readUnsigned(5) == 0x12 &&
           !linearFlareInitialStream.failed());

    V12BitWriter linearFlareUpdateWriter;
    linearFlareUpdateWriter.writeFlag(false); // GameBase datablock
    linearFlareUpdateWriter.writeFlag(false); // GameBase target
    linearFlareUpdateWriter.writeFlag(false); // non-initial update
    linearFlareUpdateWriter.writeUnsigned(0, 2); // compressed-point delta size
    for (int i = 0; i < 3; ++i) {
        linearFlareUpdateWriter.writeFlag(false);
        linearFlareUpdateWriter.writeUnsigned(0, 15);
    }
    linearFlareUpdateWriter.writeUnsigned(0, 15); // direction phi
    linearFlareUpdateWriter.writeUnsigned(0, 14); // direction theta
    linearFlareUpdateWriter.writeFlag(false); // no decal
    linearFlareUpdateWriter.writeUnsigned(0x12, 5);
    V12BitStream linearFlareUpdateStream(linearFlareUpdateWriter.data().data(),
                                          linearFlareUpdateWriter.data().size());
    assert(V12::readGhostPayload(linearFlareUpdateStream, 18, false, {}));
    assert(linearFlareUpdateStream.readUnsigned(5) == 0x12 &&
           !linearFlareUpdateStream.failed());

    V12BitWriter sniperInitialWriter;
    sniperInitialWriter.writeFlag(false); // GameBase datablock
    sniperInitialWriter.writeFlag(false); // GameBase target
    sniperInitialWriter.writeFlag(true);  // InitialUpdateMask
    sniperInitialWriter.writeUnsigned(0, 7); // energy percentage
    for (int i = 0; i < 6; ++i) sniperInitialWriter.writeUnsigned(0, 32);
    sniperInitialWriter.writeFlag(false); // not truncated
    sniperInitialWriter.writeFlag(false); // did not hit water
    sniperInitialWriter.writeFlag(false); // no source
    sniperInitialWriter.writeUnsigned(0x12, 5);
    V12BitStream sniperInitialStream(sniperInitialWriter.data().data(),
                                     sniperInitialWriter.data().size());
    assert(V12::readGhostPayload(sniperInitialStream, 36, true, {}));
    assert(sniperInitialStream.readUnsigned(5) == 0x12 &&
           !sniperInitialStream.failed());

    V12BitWriter sniperUpdateWriter;
    sniperUpdateWriter.writeFlag(false); // GameBase datablock
    sniperUpdateWriter.writeFlag(false); // GameBase target
    sniperUpdateWriter.writeFlag(false); // non-initial update
    sniperUpdateWriter.writeFlag(false); // no source
    for (int i = 0; i < 6; ++i) sniperUpdateWriter.writeUnsigned(0, 32);
    sniperUpdateWriter.writeFlag(false); // not truncated
    sniperUpdateWriter.writeUnsigned(0x12, 5);
    V12BitStream sniperUpdateStream(sniperUpdateWriter.data().data(),
                                    sniperUpdateWriter.data().size());
    assert(V12::readGhostPayload(sniperUpdateStream, 36, false, {}));
    assert(sniperUpdateStream.readUnsigned(5) == 0x12 &&
           !sniperUpdateStream.failed());

    V12BitWriter flyingVehicleWriter;
    flyingVehicleWriter.writeFlag(false); // GameBase datablock
    flyingVehicleWriter.writeFlag(false); // GameBase target
    flyingVehicleWriter.writeFlag(false); // ShapeBase masks
    flyingVehicleWriter.writeFlag(false); // jetting
    flyingVehicleWriter.writeFlag(true);  // Vehicle control shortcut
    flyingVehicleWriter.writeFlag(true);  // FlyingVehicle control shortcut
    flyingVehicleWriter.writeUnsigned(0x12, 5);
    V12BitStream flyingVehicleStream(flyingVehicleWriter.data().data(),
                                     flyingVehicleWriter.data().size());
    assert(V12::readGhostPayload(flyingVehicleStream, 10, false, {}));
    assert(flyingVehicleStream.readUnsigned(5) == 0x12 && !flyingVehicleStream.failed());

    for (const uint16_t classId : {uint16_t(7), uint16_t(9), uint16_t(13)}) {
        V12BitWriter grenadeWriter;
        grenadeWriter.writeFlag(false); // GameBase datablock
        grenadeWriter.writeFlag(false); // GameBase target
        grenadeWriter.writeFlag(false); // non-initial update
        grenadeWriter.writeFlag(false); // no bounce
        grenadeWriter.writeFlag(false); // no explosion
        grenadeWriter.writeUnsigned(0x11, 5);
        V12BitStream grenadeStream(grenadeWriter.data().data(),
                                   grenadeWriter.data().size());
        assert(V12::readGhostPayload(grenadeStream, classId, false, {}));
        assert(grenadeStream.readUnsigned(5) == 0x11 && !grenadeStream.failed());
    }

    for (const uint16_t classId : {uint16_t(7), uint16_t(9), uint16_t(13)}) {
        V12BitWriter grenadeInitialWriter;
        grenadeInitialWriter.writeFlag(false); // GameBase datablock
        grenadeInitialWriter.writeFlag(false); // GameBase target
        grenadeInitialWriter.writeFlag(true);  // InitialUpdateMask
        for (int i = 0; i < 6; ++i) grenadeInitialWriter.writeUnsigned(0, 32);
        grenadeInitialWriter.writeUnsigned(0, 12); // current tick
        grenadeInitialWriter.writeFlag(false); // quick splash
        grenadeInitialWriter.writeFlag(false); // no explosion
        grenadeInitialWriter.writeFlag(false); // no source
        grenadeInitialWriter.writeFlag(false); // no vehicle
        grenadeInitialWriter.writeUnsigned(0x12, 5);
        V12BitStream grenadeInitialStream(grenadeInitialWriter.data().data(),
                                           grenadeInitialWriter.data().size());
        assert(V12::readGhostPayload(grenadeInitialStream, classId, true, {}));
        assert(grenadeInitialStream.readUnsigned(5) == 0x12 &&
               !grenadeInitialStream.failed());
    }

    for (const uint16_t classId : {uint16_t(18), uint16_t(19), uint16_t(46)}) {
        V12BitWriter linearInitialWriter;
        linearInitialWriter.writeFlag(false); // GameBase datablock
        linearInitialWriter.writeFlag(false); // GameBase target
        linearInitialWriter.writeFlag(true);  // InitialUpdateMask
        linearInitialWriter.writeFlag(false); // live projectile
        linearInitialWriter.writeUnsigned(0, 2); // compressed-point delta size
        for (int i = 0; i < 3; ++i) {
            linearInitialWriter.writeFlag(false);
            linearInitialWriter.writeUnsigned(0, 15);
        }
        linearInitialWriter.writeUnsigned(0, 15); // direction phi
        linearInitialWriter.writeUnsigned(0, 14); // direction theta
        linearInitialWriter.writeUnsigned(0, 9); // current tick
        linearInitialWriter.writeFlag(false); // no source
        linearInitialWriter.writeFlag(false); // no vehicle
        linearInitialWriter.writeUnsigned(0x13, 5);
        V12BitStream linearInitialStream(linearInitialWriter.data().data(),
                                          linearInitialWriter.data().size());
        assert(V12::readGhostPayload(linearInitialStream, classId, true, {}));
        assert(linearInitialStream.readUnsigned(5) == 0x13 &&
               !linearInitialStream.failed());
    }

    V12BitWriter lightningWriter;
    lightningWriter.writeFlag(false); // GameBase datablock
    lightningWriter.writeFlag(false); // GameBase target
    lightningWriter.writeFlag(false); // no initial update
    lightningWriter.writeUnsigned(0x12, 5);
    V12BitStream lightningStream(lightningWriter.data().data(),
                                 lightningWriter.data().size());
    assert(V12::readGhostPayload(lightningStream, 17, false, {}));
    assert(lightningStream.readUnsigned(5) == 0x12 && !lightningStream.failed());

    V12BitWriter particleWriter;
    particleWriter.writeFlag(false); // GameBase datablock
    particleWriter.writeFlag(false); // GameBase target
    for (int i = 0; i < 19; ++i) particleWriter.writeUnsigned(0, 32);
    particleWriter.writeFlag(false); // no emitter datablock
    particleWriter.writeUnsigned(0x13, 5);
    V12BitStream particleStream(particleWriter.data().data(),
                                particleWriter.data().size());
    assert(V12::readGhostPayload(particleStream, 23, false, {}));
    assert(particleStream.readUnsigned(5) == 0x13 && !particleStream.failed());

    V12BitWriter scopeAlwaysWriter;
    scopeAlwaysWriter.writeFlag(false); // ShapeBase GameBase datablock
    scopeAlwaysWriter.writeFlag(false); // ShapeBase GameBase target
    scopeAlwaysWriter.writeFlag(false); // ShapeBase masks
    scopeAlwaysWriter.writeFlag(false); // no position update
    scopeAlwaysWriter.writeFlag(false); // not powered
    scopeAlwaysWriter.writeUnsigned(0x14, 5);
    V12BitStream scopeAlwaysStream(scopeAlwaysWriter.data().data(),
                                   scopeAlwaysWriter.data().size());
    assert(V12::readGhostPayload(scopeAlwaysStream, 29, false, {}));
    assert(scopeAlwaysStream.readUnsigned(5) == 0x14 && !scopeAlwaysStream.failed());

    V12BitWriter tracerWriter;
    tracerWriter.writeFlag(false); // GameBase datablock
    tracerWriter.writeFlag(false); // GameBase target
    tracerWriter.writeFlag(false); // non-initial update
    tracerWriter.writeUnsigned(0, 2); // compressed-point delta size
    for (int i = 0; i < 3; ++i) {
        tracerWriter.writeFlag(false);
        tracerWriter.writeUnsigned(0, 15);
    }
    tracerWriter.writeUnsigned(0, 15); // direction phi
    tracerWriter.writeUnsigned(0, 14); // direction theta
    tracerWriter.writeFlag(false); // no decal
    tracerWriter.writeUnsigned(0x15, 5);
    V12BitStream tracerStream(tracerWriter.data().data(), tracerWriter.data().size());
    assert(V12::readGhostPayload(tracerStream, 46, false, {}));
    assert(tracerStream.readUnsigned(5) == 0x15 && !tracerStream.failed());

    V12BitWriter audioEmitterWriter;
    audioEmitterWriter.writeFlag(false); // initial update
    audioEmitterWriter.writeFlag(false); // no transform
    audioEmitterWriter.writeFlag(false); // no profile
    audioEmitterWriter.writeFlag(false); // no description
    audioEmitterWriter.writeFlag(true);  // filename
    audioEmitterWriter.writeHuffmanString("audio.wav");
    for (int i = 0; i < 15; ++i) audioEmitterWriter.writeFlag(false);
    audioEmitterWriter.writeUnsigned(0x14, 5);
    V12BitStream audioEmitterStream(audioEmitterWriter.data().data(),
                                    audioEmitterWriter.data().size());
    assert(V12::readGhostPayload(audioEmitterStream, 1, false, {}));
    assert(audioEmitterStream.readUnsigned(5) == 0x14 &&
           !audioEmitterStream.failed());

    V12BitWriter beaconWriter;
    beaconWriter.writeFlag(false); beaconWriter.writeFlag(false);
    beaconWriter.writeFlag(false); // ShapeBase masks
    beaconWriter.writeFlag(false); // no position update
    beaconWriter.writeFlag(false); // not powered
    beaconWriter.writeFlag(true); beaconWriter.writeUnsigned(2, 2);
    beaconWriter.writeUnsigned(0x16, 5);
    V12BitStream beaconStream(beaconWriter.data().data(), beaconWriter.data().size());
    assert(V12::readGhostPayload(beaconStream, 2, false, {}));
    assert(beaconStream.readUnsigned(5) == 0x16 && !beaconStream.failed());

    V12BitWriter debrisWriter;
    debrisWriter.writeFlag(false); debrisWriter.writeFlag(false); // GameBase
    for (int i = 0; i < 6; ++i) debrisWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 4; ++i) debrisWriter.writeUnsigned(0, 8);
    for (int i = 0; i < 6; ++i) debrisWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 2; ++i) debrisWriter.writeUnsigned(0, 8);
    for (int i = 0; i < 3; ++i) debrisWriter.writeUnsigned(0, 32);
    debrisWriter.writeUnsigned(0, 8);
    debrisWriter.writeFlag(false); debrisWriter.writeHuffmanString("debris");
    debrisWriter.writeFlag(true); debrisWriter.writeUnsigned(0, 8);
    debrisWriter.writeHuffmanString("_variant");
    for (int i = 0; i < 3; ++i) debrisWriter.writeFlag(false);
    debrisWriter.writeUnsigned(0x17, 5);
    V12BitStream debrisStream(debrisWriter.data().data(), debrisWriter.data().size());
    debrisStream.setStringBuffer(true);
    assert(V12::readGhostPayload(debrisStream, 5, false, {}));
    assert(debrisStream.readUnsigned(5) == 0x17 && !debrisStream.failed());

    V12BitWriter physicalZoneInitialWriter;
    physicalZoneInitialWriter.writeFlag(true); // initial update
    for (int i = 0; i < 16; ++i) physicalZoneInitialWriter.writeUnsigned(0, 32);
    for (int i = 0; i < 3; ++i) physicalZoneInitialWriter.writeUnsigned(0, 32); // scale
    physicalZoneInitialWriter.writeUnsigned(0, 32); // point count
    physicalZoneInitialWriter.writeUnsigned(0, 32); // plane count
    physicalZoneInitialWriter.writeUnsigned(0, 32); // edge count
    physicalZoneInitialWriter.writeUnsigned(0, 32); // velocity mod
    physicalZoneInitialWriter.writeUnsigned(0, 32); // gravity mod
    for (int i = 0; i < 3; ++i) physicalZoneInitialWriter.writeUnsigned(0, 32);
    physicalZoneInitialWriter.writeFlag(false); // inactive
    physicalZoneInitialWriter.writeUnsigned(0x18, 5);
    V12BitStream physicalZoneInitialStream(physicalZoneInitialWriter.data().data(),
                                           physicalZoneInitialWriter.data().size());
    assert(V12::readGhostPayload(physicalZoneInitialStream, 24, true, {}));
    assert(physicalZoneInitialStream.readUnsigned(5) == 0x18 &&
           !physicalZoneInitialStream.failed());

    V12BitWriter physicalZoneUpdateWriter;
    physicalZoneUpdateWriter.writeFlag(false); // non-initial update
    physicalZoneUpdateWriter.writeFlag(true); // active
    physicalZoneUpdateWriter.writeUnsigned(0x19, 5);
    V12BitStream physicalZoneUpdateStream(physicalZoneUpdateWriter.data().data(),
                                          physicalZoneUpdateWriter.data().size());
    assert(V12::readGhostPayload(physicalZoneUpdateStream, 24, false, {}));
    assert(physicalZoneUpdateStream.readUnsigned(5) == 0x19 &&
           !physicalZoneUpdateStream.failed());

    V12BitWriter shockLanceInitialWriter;
    shockLanceInitialWriter.writeFlag(false); // GameBase datablock
    shockLanceInitialWriter.writeFlag(false); // GameBase target
    shockLanceInitialWriter.writeFlag(false); // no target
    shockLanceInitialWriter.writeFlag(true); // initial update
    for (int i = 0; i < 6; ++i) shockLanceInitialWriter.writeUnsigned(0, 32);
    shockLanceInitialWriter.writeFlag(false); // no hit object
    shockLanceInitialWriter.writeFlag(false); // no source object
    shockLanceInitialWriter.writeUnsigned(0x1a, 5);
    V12BitStream shockLanceInitialStream(shockLanceInitialWriter.data().data(),
                                         shockLanceInitialWriter.data().size());
    assert(V12::readGhostPayload(shockLanceInitialStream, 32, true, {}));
    assert(shockLanceInitialStream.readUnsigned(5) == 0x1a &&
           !shockLanceInitialStream.failed());

    V12BitWriter shockLanceUpdateWriter;
    shockLanceUpdateWriter.writeFlag(false); // GameBase datablock
    shockLanceUpdateWriter.writeFlag(false); // GameBase target
    shockLanceUpdateWriter.writeFlag(false); // no target
    shockLanceUpdateWriter.writeFlag(false); // no initial update
    shockLanceUpdateWriter.writeUnsigned(0x1b, 5);
    V12BitStream shockLanceUpdateStream(shockLanceUpdateWriter.data().data(),
                                        shockLanceUpdateWriter.data().size());
    assert(V12::readGhostPayload(shockLanceUpdateStream, 32, false, {}));
    assert(shockLanceUpdateStream.readUnsigned(5) == 0x1b &&
           !shockLanceUpdateStream.failed());

    auto challenge = V12::buildConnectChallengeRequest(V12::ProtocolVersion, 0x12345678);
    assert(challenge.size() >= 10 && challenge[0] == V12::OobConnectChallengeRequest);
    assert(challenge[1] == 0x33 && challenge[2] == 0x00 && challenge[3] == 0x00);
    assert(challenge[5] == 0x78 && challenge[8] == 0x12);
    auto passwordChallenge = V12::buildConnectChallengeRequest(
        V12::ProtocolVersion, 17, "secret");
    V12BitStream passwordStream(passwordChallenge.data() + 9,
                                passwordChallenge.size() - 9);
    assert(passwordStream.readHuffmanString() == "secret");
    assert(!passwordStream.readFlag() && !passwordStream.failed());
    auto connect = V12::buildConnectRequest(7, 9, V12::ProtocolVersion, false,
        {"Observer", "Male Human", "beagle", "male1", "1.0"});
    assert(connect.size() > 18 && connect[0] == V12::OobConnectRequest);
    V12BitStream connectStream(connect.data() + 1, connect.size() - 1);
    assert(connectStream.readU32() == 7);
    assert(connectStream.readU32() == 9);
    assert(connectStream.readU32() == V12::ProtocolVersion);
    assert(!connectStream.readFlag());
    assert(connectStream.readU32() == 5);
    assert(connectStream.readHuffmanString() == "Observer");
    assert(connectStream.readHuffmanString() == "Male Human");
    assert(connectStream.readHuffmanString() == "beagle");
    assert(connectStream.readHuffmanString() == "male1");
    assert(connectStream.readHuffmanString() == "1.0");
    assert(!connectStream.failed());
    const uint8_t challengeResponse[] = {
        30, 0x33, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
        0xef, 0xcd, 0xab, 0x90,
    };
    V12::ConnectChallengeResponse parsedChallenge;
    assert(V12::readConnectChallengeResponse(
        challengeResponse, sizeof(challengeResponse), parsedChallenge));
    assert(parsedChallenge.protocolVersion == V12::ProtocolVersion);
    assert(parsedChallenge.serverSequence == 0x12345678);
    assert(parsedChallenge.clientSequence == 0x90abcdef);
    assert(!V12::readConnectChallengeResponse(
        challengeResponse, sizeof(challengeResponse) - 1, parsedChallenge));
    const auto acceptPacket = V12::buildConnectAccept(7, 9);
    V12::ConnectAccept parsedAccept;
    assert(V12::readConnectAccept(acceptPacket.data(), acceptPacket.size(), parsedAccept));
    assert(parsedAccept.serverSequence == 7 && parsedAccept.clientSequence == 9);
    auto ping = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0xabcdef01);
    assert(ping.size() == 6 && ping[0] == 14 && ping[2] == 0x01 && ping[5] == 0xab);
    auto pingResponse = V12::buildGamePingResponse(0, 0xabcdef01, "Torch Server");
    assert(pingResponse[0] == 16 && pingResponse.size() > 20);
    V12BitStream pingResponseStream(pingResponse.data() + 6,
                                    pingResponse.size() - 6);
    assert(pingResponseStream.readHuffmanString() == "VER5");
    assert(pingResponseStream.readU32() == V12::ProtocolVersion);
    assert(pingResponseStream.readU32() == V12::ProtocolVersion);
    assert(pingResponseStream.readU32() == 25034);
    assert(pingResponseStream.readHuffmanString() == "Torch Server");
    auto infoResponse = V12::buildGameInfoResponse(
        0, 0xabcdef01, "base", "Deathmatch", "riverdance", 0, 3, 32, 1);
    assert(infoResponse[0] == 20 && infoResponse.size() > 20);
    V12BitStream infoStream(infoResponse.data() + 6, infoResponse.size() - 6);
    assert(infoStream.readHuffmanString() == "base");
    assert(infoStream.readHuffmanString() == "Deathmatch");
    assert(infoStream.readHuffmanString() == "riverdance");
    assert(infoStream.readU8() == 0 && infoStream.readU8() == 3);
    assert(infoStream.readU8() == 32 && infoStream.readU8() == 1);
    auto disconnect = V12::buildDisconnectPacket(7, 9);
    assert(disconnect.size() >= 10 && disconnect[0] == V12::OobDisconnect);
    V12BitStream disconnectStream(disconnect.data() + 1, disconnect.size() - 1);
    assert(disconnectStream.readU32() == 7);
    assert(disconnectStream.readU32() == 9);
    assert(disconnectStream.readHuffmanString().empty());
    assert(!disconnectStream.failed());

    return 0;
}
