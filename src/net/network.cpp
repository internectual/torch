#include "net/network.h"
#include "net/protocol.h"
#include "net/v12_protocol.h"
#include "core/console.h"
#include "core/config.h"
#include "core/engine.h"
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <queue>
#include <chrono>
#include <map>
#include <utility>

// ─── Wire Header ──────────────────────────────────────────────────
// Preprended to every UDP packet (15 bytes)
struct WireHeader {
    uint32_t sequence;
    uint32_t ack;
    uint32_t ackMask;
    uint8_t type;
    uint16_t checksum;
};

static uint16_t wireChecksum(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++)
        sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

// ─── Connection ───────────────────────────────────────────────────

struct Connection::Impl {
    int sock = -1;
    sockaddr_in addr{};

    // Sequence numbers (32-bit for proper ack tracking)
    uint32_t sendSeq = 1;
    uint32_t recvSeq = 0;

    // Sent reliable packets keyed by sequence number
    struct SentPacket {
        std::vector<uint8_t> data;
        double sendTime;
        int retries;
        bool reliable;
    };
    std::map<uint32_t, SentPacket> sentPackets; // unacked sent packets
    static constexpr int MAX_RETRIES = 5;
    static constexpr double RETRY_TIMEOUT = 1.0; // seconds

    // Connection timers
    double connectTime = 0;
    double lastPing = 0;
    double lastReceive = 0;
    uint32_t challenge[2]{};
    uint32_t clientConnectSequence = 0;
    uint32_t serverConnectSequence = 0;
    std::vector<uint8_t> connectRequest;
    V12::ProtocolState nativeProtocol;
    V12::NetStringTable nativeStrings;
    V12::GhostTracker nativeGhosts;
    std::map<uint32_t, std::vector<V12::ClientEvent>> sentNativeEventPackets;
    uint8_t nextNativeEventSequence = 0;
    bool nativeRateAdvertised = false;
    bool pendingNativeMove = false;
    uint32_t pendingNativeMoveStart = 0;
    V12::ClientMove pendingNativeMoveData;
    std::vector<V12::ClientEvent> pendingNativeEvents;
    double lastNativeDataSend = 0;
    std::vector<uint8_t> disconnectPacket;
    int disconnectAttempts = 0;
    double nextDisconnectSend = 0;
    static constexpr uint32_t NativeSendWindow = 30;

    void flushNativeMove() {
        if ((!pendingNativeMove && pendingNativeEvents.empty()) ||
            sock < 0 || serverConnectSequence == 0) return;
        const double now = Engine::instance().timer().now();
        if (now < lastNativeDataSend + 0.032) return;
        if (nativeProtocol.lastSent() - nativeProtocol.highestAcknowledged() >=
            NativeSendWindow) return;
        V12::ClientPacketOptions options;
        if (pendingNativeMove) {
            options.moveStart = pendingNativeMoveStart;
            options.moves.push_back(pendingNativeMoveData);
        }
        options.advertiseMaxRate = !nativeRateAdvertised;
        options.maxUpdateDelay = 32;
        options.maxPacketSize = 450;
        const uint32_t packetSequence = nativeProtocol.lastSent() + 1;
        std::vector<V12::ClientEvent> sentEvents = std::move(pendingNativeEvents);
        options.events = sentEvents;
        nativeRateAdvertised = true;
        sendRaw(nativeProtocol.buildClientPacket(options));
        if (!sentEvents.empty())
            sentNativeEventPackets.emplace(packetSequence, std::move(sentEvents));
        pendingNativeMove = false;
        lastNativeDataSend = now;
    }

    void sendRaw(const std::vector<uint8_t>& packet) {
        if (sock < 0 || packet.empty()) return;
        sendto(sock, packet.data(), packet.size(), 0,
               (sockaddr*)&addr, sizeof(addr));
    }

    // Send a pre-built payload (without wire header) with proper framing
    void sendFramed(PacketType ptype, const uint8_t* payload, size_t payloadLen, bool reliable) {
        if (sock < 0) return;

        // Build wire header
        WireHeader hdr;
        hdr.sequence = sendSeq++;
        hdr.ack = recvSeq;
        // Build ack mask: which of the last 32 packets before recvSeq did we receive?
        // For simplicity, just set all bits for recent packets
        hdr.ackMask = 0xFFFFFFFF;
        hdr.type = (uint8_t)ptype;
        hdr.checksum = 0; // placeholder

        // Assemble full packet: header + payload
        std::vector<uint8_t> packet;
        packet.resize(sizeof(WireHeader) + payloadLen);
        memcpy(packet.data(), &hdr, sizeof(WireHeader));
        if (payload && payloadLen > 0)
            memcpy(packet.data() + sizeof(WireHeader), payload, payloadLen);

        // Calculate checksum over header + payload (with checksum=0)
        uint16_t csum = wireChecksum(packet.data(), packet.size());
        memcpy(packet.data() + offsetof(WireHeader, checksum), &csum, sizeof(csum));

        sendto(sock, packet.data(), packet.size(), 0, (sockaddr*)&addr, sizeof(addr));

        // Track reliable packets for retransmission
        if (reliable) {
            sentPackets[hdr.sequence] = {
                std::vector<uint8_t>(payload, payload + payloadLen),
                Engine::instance().timer().now(),
                0,
                true
            };
        }
    }
};

// ── Public API ────────────────────────────────────────────────────

Connection::Connection() : impl(new Impl) {}
Connection::~Connection() {
    disconnect();
    if (impl->sock >= 0) close(impl->sock);
    delete impl;
}

bool Connection::connect(const char* host, uint16_t port) {
    impl->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->sock < 0) {
        Console::instance().printf(LogLevel::Error, "Cannot create socket");
        return false;
    }

    // Non-blocking
    int flags = fcntl(impl->sock, F_GETFL, 0);
    fcntl(impl->sock, F_SETFL, flags | O_NONBLOCK);

    // Resolve hostname
    hostent* he = gethostbyname(host);
    if (!he) {
        Console::instance().printf(LogLevel::Error, "Cannot resolve: %s", host);
        close(impl->sock);
        impl->sock = -1;
        return false;
    }

    impl->addr.sin_family = AF_INET;
    impl->addr.sin_port = htons(port);
    memcpy(&impl->addr.sin_addr, he->h_addr, he->h_length);

    remoteAddr.ip = impl->addr.sin_addr.s_addr;
    remoteAddr.port = port;

    connState = Connecting;
    impl->connectTime = Engine::instance().timer().now();
    impl->nativeProtocol.reset();
    impl->nativeStrings.clear();
    impl->nativeGhosts.clear();
    impl->connectRequest.clear();
    impl->disconnectPacket.clear();
    impl->disconnectAttempts = 0;
    impl->nextNativeEventSequence = 0;
    impl->nativeRateAdvertised = false;
    impl->pendingNativeMove = false;
    impl->pendingNativeEvents.clear();
    impl->sentNativeEventPackets.clear();
    impl->lastNativeDataSend = 0;

    // Generate random challenge
    srand((unsigned int)(time(nullptr) + (uintptr_t)this));
    impl->challenge[0] = (uint32_t)rand();
    impl->challenge[1] = (uint32_t)rand();

    impl->clientConnectSequence = impl->challenge[0];
    impl->serverConnectSequence = 0;
    impl->sendRaw(V12::buildConnectChallengeRequest(
        V12::ProtocolVersion, impl->clientConnectSequence, joinPassword));

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host, port);
    return true;
}

void Connection::disconnect() {
    if (impl->sock < 0) return;
    if (impl->serverConnectSequence != 0) {
        if (impl->disconnectPacket.empty()) {
            impl->disconnectPacket = V12::buildDisconnectPacket(
                impl->serverConnectSequence, impl->clientConnectSequence);
            impl->disconnectAttempts = 0;
        }
        impl->sendRaw(impl->disconnectPacket);
        ++impl->disconnectAttempts;
        impl->nextDisconnectSend = Engine::instance().timer().now() + 0.5;
    } else {
        close(impl->sock);
        impl->sock = -1;
    }
    connState = Disconnected;
    impl->sentPackets.clear();
}

void Connection::update() {
    if (impl->sock < 0) return;

    double now = Engine::instance().timer().now();

    if (!impl->disconnectPacket.empty()) {
        if (impl->disconnectAttempts < 3 && now >= impl->nextDisconnectSend) {
            impl->sendRaw(impl->disconnectPacket);
            ++impl->disconnectAttempts;
            impl->nextDisconnectSend = now + 0.5;
        } else if (impl->disconnectAttempts >= 3 && now >= impl->nextDisconnectSend) {
            impl->disconnectPacket.clear();
            close(impl->sock);
            impl->sock = -1;
        }
        return;
    }

    // ── Receive packets ──────────────────────────────────────────
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;
        if (from.sin_addr.s_addr != impl->addr.sin_addr.s_addr ||
            from.sin_port != impl->addr.sin_port)
            continue;

        // Native V12 OOB packets are byte-discriminated from dnet packets by
        // the low bit: all OOB packet types are even, while data starts with
        // the set gameFlag bit.
        const uint8_t oobType = buf[0];
        const bool handshakeOob = connState < Connected &&
            (oobType == 28 || oobType == 30 || oobType == 34 || oobType == 36);
        const bool disconnectOob = oobType == 38 &&
            impl->serverConnectSequence != 0;
        if ((buf[0] & 1) == 0 && (handshakeOob || disconnectOob)) {
            const uint8_t type = oobType;
            if (type == 30 && connState == Connecting && n >= 13) {
                V12::ConnectChallengeResponse response;
                if (V12::readConnectChallengeResponse(buf, (size_t)n, response) &&
                    response.protocolVersion == V12::ProtocolVersion &&
                    response.clientSequence == impl->clientConnectSequence) {
                    impl->serverConnectSequence = response.serverSequence;
                    impl->connectRequest = V12::buildConnectRequest(
                        impl->serverConnectSequence,
                        impl->clientConnectSequence,
                        V12::ProtocolVersion,
                        false,
                        {playerName, "Male Human", "beagle", "male1", "1.0"});
                    impl->sendRaw(impl->connectRequest);
                    impl->connectTime = now;
                    connState = Challenging;
                }
                continue;
            }
            if (type == 36 && connState == Challenging) {
                V12::ConnectAccept accept;
                if (!V12::readConnectAccept(buf, (size_t)n, accept) ||
                    accept.serverSequence != impl->serverConnectSequence ||
                    accept.clientSequence != impl->clientConnectSequence)
                    continue;
                connState = Connected;
                impl->nativeProtocol.setConnectSequence(
                    impl->clientConnectSequence ^ impl->serverConnectSequence);
                Console::instance().printf(LogLevel::Info, "Connection established");
                if (connectCb) connectCb(true);
                if (packetCb) packetCb(PacketType::ConnectOK, nullptr, 0);
                continue;
            }
            if (type == 28 || type == 34) {
                Console::instance().printf(LogLevel::Warn, "Connection rejected by server");
                if (connectCb) connectCb(false);
                disconnect();
                continue;
            }
            if (type == 38) {
                impl->disconnectPacket.clear();
                close(impl->sock);
                impl->sock = -1;
                continue;
            }
            continue;
        }

        // Native dnet packets are bit-packed after the first discriminator.
        // Do not pass their payload to the legacy byte-oriented callback yet;
        // gameplay event and move decoding is migrated separately.
        if (connState >= Connected) {
            V12BitStream stream(buf, (size_t)n);
            V12::DnetHeader header;
            if (V12::readDnetHeader(stream, header)) {
                const auto result = impl->nativeProtocol.processReceived(header);
                for (const auto& acknowledgement : result.acknowledgements) {
                    auto sent = impl->sentNativeEventPackets.find(
                        acknowledgement.sequence);
                    if (sent == impl->sentNativeEventPackets.end()) continue;
                    if (!acknowledgement.acknowledged) {
                        impl->pendingNativeEvents.insert(
                            impl->pendingNativeEvents.begin(),
                            sent->second.begin(), sent->second.end());
                    }
                    impl->sentNativeEventPackets.erase(sent);
                }
                if (result.accepted && header.packetType == V12::PacketType::Ping)
                    impl->sendRaw(impl->nativeProtocol.buildPacket(V12::PacketType::Ack));
                if (result.accepted && result.dispatchData &&
                    header.packetType == V12::PacketType::Data) {
                    std::vector<V12::ServerEvent> events;
                    V12::ServerGameState gameState;
                    V12Vec3 compressionPoint;
                    V12BitStream payload(buf, (size_t)n, stream.position());
                    if (V12::readServerPacketEvents(payload, impl->nativeStrings, events,
                                                    &gameState,
                                                    &compressionPoint, nullptr)) {
                        if (stateCb) stateCb(gameState);
                        for (const auto& event : events) {
                            if (event.classId == 9 && commandCb)
                                commandCb(event.message);
                            if (event.hasDatablock && datablockCb) {
                                datablockCb(event.datablockObject,
                                            event.datablockClass,
                                            event.datablockIndex,
                                            event.datablockTotal,
                                            event.datablockClassName,
                                            event.datablockData);
                            }
                            if (event.hasGhostingMessage && event.ghostMessage == 0) {
                                V12::ClientPacketOptions responseOptions;
                                responseOptions.events.push_back(
                                    V12::makeGhostingMessageEvent(
                                        event.ghostSequence, 1, event.ghostCount));
                                responseOptions.events.front().sequence =
                                    impl->nextNativeEventSequence++ & 0x7f;
                                impl->pendingNativeEvents.push_back(
                                    std::move(responseOptions.events.front()));
                                impl->flushNativeMove();
                            }
                        }
                        std::vector<V12::GhostUpdate> updates;
                        std::map<uint16_t, V12::PlayerGhostState> playerStates;
                        const bool ghostsOk = V12::readGhostUpdates(
                            payload, impl->nativeGhosts, updates,
                            [&](V12BitStream& ghost, uint16_t index, uint16_t classId, bool initial) {
                                V12::PlayerGhostState state;
                                const bool ok = V12::readGhostPayload(
                                    ghost, classId, initial, compressionPoint, &state);
                                if (ok && (classId == 22 || classId == 25 || classId == 29 ||
                                           classId == 37 || classId == 39 || classId == 51))
                                    playerStates[index] = state;
                                return ok;
                            });
                        if (ghostsOk && ghostCb) {
                            for (const auto& update : updates) {
                                if (update.operation == V12::GhostUpdate::Operation::Delete) {
                                    ghostCb(update, nullptr);
                                } else {
                                    auto state = playerStates.find(update.index);
                                    ghostCb(update, state == playerStates.end()
                                        ? nullptr : &state->second);
                                }
                            }
                        }
                    }
                }
                continue;
            }
        }

        if ((size_t)n < sizeof(WireHeader)) continue;

        // Parse wire header
        WireHeader hdr;
        memcpy(&hdr, buf, sizeof(WireHeader));

        // Verify checksum
        uint16_t savedCsum = hdr.checksum;
        WireHeader hdrNoCsum = hdr;
        hdrNoCsum.checksum = 0;
        std::vector<uint8_t> tmp(sizeof(WireHeader) + n - sizeof(WireHeader));
        memcpy(tmp.data(), &hdrNoCsum, sizeof(WireHeader));
        if (n > (int)sizeof(WireHeader))
            memcpy(tmp.data() + sizeof(WireHeader), buf + sizeof(WireHeader), n - sizeof(WireHeader));
        if (wireChecksum(tmp.data(), tmp.size()) != savedCsum)
            continue; // bad checksum

        impl->lastReceive = now;

        // Update ack tracking — remove acked packets
        uint32_t ackSeq = hdr.ack;
        uint32_t ackMask = hdr.ackMask;
        auto it = impl->sentPackets.begin();
        while (it != impl->sentPackets.end()) {
            // Acknowledge if seq matches or is within ackMask range
            if (it->first == ackSeq) {
                it = impl->sentPackets.erase(it);
                continue;
            }
            // Check ack mask: bits correspond to packets (ack-1, ack-2, ... ack-32)
            if (ackSeq > it->first) {
                uint32_t diff = ackSeq - it->first;
                if (diff > 0 && diff <= 32 && (ackMask & (1 << (diff - 1)))) {
                    it = impl->sentPackets.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Update received sequence tracking
        if (hdr.sequence > impl->recvSeq)
            impl->recvSeq = hdr.sequence;

        PacketType ptype = (PacketType)hdr.type;
        const uint8_t* payload = buf + sizeof(WireHeader);
        size_t payloadLen = n - sizeof(WireHeader);

        // Handle connection protocol packets internally
        if (ptype == PacketType::Challenge && connState == Connecting && payloadLen >= 8) {
            // Server sent a challenge — respond with challenge response
            T2Protocol::ChallengeMessage chal;
            memcpy(&chal, payload, sizeof(uint32_t) * 2);
            T2Protocol::ChallengeResponse resp;
            resp.response[0] = chal.challenge[0] ^ impl->challenge[0];
            resp.response[1] = chal.challenge[1] ^ impl->challenge[1];
            impl->sendFramed(PacketType::ChallengeResponse, (const uint8_t*)&resp, sizeof(resp), true);
            connState = Challenging;
            continue;
        }

        if (ptype == PacketType::ConnectOK && connState == Challenging) {
            connState = Connected;
            Console::instance().printf(LogLevel::Info, "Connection established");
            if (connectCb) connectCb(true);
            continue;
        }

        if (ptype == PacketType::ConnectReject) {
            Console::instance().printf(LogLevel::Warn, "Connection rejected by server");
            if (connectCb) connectCb(false);
            disconnect();
            continue;
        }

        // Forward other packets to callback
        if (packetCb)
            packetCb(ptype, payload, payloadLen);
    }

    // ── Timeout ──────────────────────────────────────────────────
    if ((connState == Connecting || connState == Challenging) &&
        (now - impl->connectTime) > 10.0) {
        Console::instance().printf(LogLevel::Warn, "Connection timeout");
        if (connectCb) connectCb(false);
        disconnect();
        return;
    }

    // ── Reliable retransmission ──────────────────────────────────
    std::vector<uint32_t> toRemove;
    for (auto& [seq, sp] : impl->sentPackets) {
        if (!sp.reliable) continue;
        if (now - sp.sendTime >= Impl::RETRY_TIMEOUT) {
            sp.retries++;
            if (sp.retries > Impl::MAX_RETRIES) {
                Console::instance().printf(LogLevel::Warn, "Reliable send failed (seq=%u, retries=%d)", seq, sp.retries);
                toRemove.push_back(seq);
                if (connectCb && (connState != Connected))
                    connectCb(false);
            } else {
                // Retransmit
                sp.sendTime = now;
                impl->sendFramed(PacketType::GameData, sp.data.data(), sp.data.size(), false);
            }
        }
    }
    for (uint32_t seq : toRemove)
        impl->sentPackets.erase(seq);

    impl->flushNativeMove();

    // ── Send Connect retry (no response yet) ────────────────────
    if (connState == Connecting && (now - impl->connectTime) > 1.0) {
        impl->connectTime = now;
        impl->sendRaw(V12::buildConnectChallengeRequest(
            V12::ProtocolVersion, impl->clientConnectSequence, joinPassword));
    } else if (connState == Challenging && !impl->connectRequest.empty() &&
               (now - impl->connectTime) > 1.0) {
        impl->connectTime = now;
        impl->sendRaw(impl->connectRequest);
    }

    // ── Ping ─────────────────────────────────────────────────────
    if (connState >= Connected && (now - impl->lastPing) > 5.0) {
        impl->lastPing = now;
        sendPacket(PacketType::Ping, nullptr, 0);
    }
}

void Connection::sendPacket(PacketType type, const uint8_t* data, size_t size) {
    if (type == PacketType::Ping && impl->serverConnectSequence != 0) {
        impl->sendRaw(impl->nativeProtocol.buildPacket(V12::PacketType::Ping));
        return;
    }
    impl->sendFramed(type, data, size, false);
}

void Connection::sendGamePacket(const uint8_t* data, size_t size, bool reliable) {
    // Reliable: track for retransmission (handled by sendFramed with reliable=true)
    impl->sendFramed(PacketType::GameData, data, size, reliable);
}

void Connection::sendNativeMove(uint32_t moveStart, const V12::ClientMove& move) {
    if (connState < Connected || impl->serverConnectSequence == 0) return;
    impl->pendingNativeMoveStart = moveStart;
    impl->pendingNativeMoveData = move;
    impl->pendingNativeMove = true;
    impl->flushNativeMove();
}

void Connection::sendCommandPacket(const char* command) {
    if (!command || connState < Connected) return;
    std::vector<std::string> argv;
    std::string token;
    bool quoted = false;
    bool escaped = false;
    for (const char* p = command;; ++p) {
        const char c = *p;
        if (escaped) {
            if (c == '\0') {
                token.push_back('\\');
                if (!token.empty()) argv.push_back(std::move(token));
                break;
            }
            token.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            quoted = !quoted;
        } else if ((c == ' ' || c == '\t' || c == '\0') && !quoted) {
            if (!token.empty()) {
                argv.push_back(std::move(token));
                token.clear();
            }
            if (c == '\0') break;
        } else {
            token.push_back(c);
        }
    }
    if (argv.empty()) return;
    auto events = V12::buildRemoteCommandEvents(
        impl->nativeStrings, argv.front(),
        std::vector<std::string>(argv.begin() + 1, argv.end()));
    for (auto& event : events)
        event.sequence = impl->nextNativeEventSequence++ & 0x7f;
    for (auto& event : events)
        impl->pendingNativeEvents.push_back(std::move(event));
    impl->flushNativeMove();
}

// ── NetworkManager ────────────────────────────────────────────────

struct NetworkManager::Impl {
    int broadcastSock = -1;
    bool querying = false;
    double queryStartTime = 0;
    double querySentAt = 0;
    std::map<std::string, bool> nativeInfoRequested;
    double queryEndTime = 0;
    std::map<std::string, ServerInfo> seenServers; // dedup by addr string

    int ensureBroadcastSock() {
        if (broadcastSock >= 0) return broadcastSock;
        broadcastSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (broadcastSock < 0) return -1;
        int broadcastEnable = 1;
        setsockopt(broadcastSock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
        int flags = fcntl(broadcastSock, F_GETFL, 0);
        fcntl(broadcastSock, F_SETFL, flags | O_NONBLOCK);
        return broadcastSock;
    }
};

NetworkManager::NetworkManager() : impl(new Impl) {}
NetworkManager::~NetworkManager() { delete impl; }

bool NetworkManager::init() {
    Console::instance().printf(LogLevel::Info, "Network initialized");
    return true;
}

void NetworkManager::shutdown() {
    if (impl->broadcastSock >= 0) close(impl->broadcastSock);
}

static void parseServerResponse(const uint8_t* data, size_t size, NetworkManager::ServerInfo& info) {
    // Format: 4B ip | 2B port | 2B nameLen | name | 2B mapLen | map | 2B typeLen | type | 1B players | 1B maxPlayers | 1B password | 2B ping
    size_t off = 0;
    auto r32 = [&]() -> uint32_t {
        if (off + 4 > size) return 0;
        uint32_t v = data[off] | ((uint32_t)data[off+1]<<8) | ((uint32_t)data[off+2]<<16) | ((uint32_t)data[off+3]<<24);
        off += 4; return v;
    };
    auto r16 = [&]() -> uint16_t {
        if (off + 2 > size) return 0;
        uint16_t v = data[off] | ((uint16_t)data[off+1]<<8);
        off += 2; return v;
    };
    auto r8 = [&]() -> uint8_t {
        if (off + 1 > size) return 0;
        return data[off++];
    };
    auto rstr = [&]() -> std::string {
        uint16_t len = r16();
        if (off + len > size) return "";
        std::string s((const char*)data+off, len);
        off += len;
        return s;
    };
    info.addr.ip = r32();
    info.addr.port = r16();
    info.name = rstr();
    info.map = rstr();
    info.gameType = rstr();
    info.numPlayers = r8();
    info.maxPlayers = r8();
    info.password = r8() != 0;
    info.ping = r16();
}

static bool parseNativePingResponse(const uint8_t* data, size_t size,
                                    NetworkManager::ServerInfo& info,
                                    double sentAt) {
    if (!data || size < 7 || data[0] != 16) return false;
    V12BitStream stream(data + 6, size - 6);
    stream.readHuffmanString(); // version string
    stream.readU32();           // protocol version
    stream.readU32();           // minimum protocol version
    const uint32_t build = stream.readU32();
    const std::string name = stream.readHuffmanString();
    if (stream.failed() || build != 25034) return false;
    info.name = name;
    info.ping = (int)((Engine::instance().timer().now() -
                       sentAt) * 1000.0);
    return true;
}

static bool parseNativeInfoResponse(const uint8_t* data, size_t size,
                                    NetworkManager::ServerInfo& info) {
    if (!data || size < 7 || data[0] != 20) return false;
    V12BitStream stream(data + 6, size - 6);
    stream.readHuffmanString(); // mod
    info.gameType = stream.readHuffmanString();
    info.map = stream.readHuffmanString();
    const uint8_t status = stream.readU8();
    info.numPlayers = stream.readU8();
    info.maxPlayers = stream.readU8();
    stream.readU8(); // bot count
    if (stream.failed()) return false;
    info.password = (status & 0x02) != 0;
    return true;
}

void NetworkManager::update() {
    // Receive query responses
    if (impl->broadcastSock < 0) return;

    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->broadcastSock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;

        if (n >= 1 && buf[0] == 16 && impl->querying) {
            ServerInfo info;
            if (parseNativePingResponse(buf, (size_t)n, info, impl->querySentAt)) {
                info.addr.ip = from.sin_addr.s_addr;
                info.addr.port = ntohs(from.sin_port);
                const std::string key = info.addr.toString();
                if (impl->seenServers.find(key) == impl->seenServers.end()) {
                    impl->seenServers[key] = info;
                    servers.push_back(info);
                    if (serverListCb) serverListCb();
                }
                if (!impl->nativeInfoRequested[key]) {
                    const auto request = V12::buildGameQuery(
                        V12::OobGameInfoRequest, 0, 0);
                    sendto(impl->broadcastSock, request.data(), request.size(), 0,
                           (sockaddr*)&from, sizeof(from));
                    impl->nativeInfoRequested[key] = true;
                }
            }
            continue;
        }

        if (n >= 1 && buf[0] == 20 && impl->querying) {
            NetAddress addr;
            addr.ip = from.sin_addr.s_addr;
            addr.port = ntohs(from.sin_port);
            const std::string key = addr.toString();
            auto server = impl->seenServers.find(key);
            if (server != impl->seenServers.end() &&
                parseNativeInfoResponse(buf, (size_t)n, server->second)) {
                for (auto& entry : servers) {
                    if (entry.addr.toString() == key) {
                        entry = server->second;
                        break;
                    }
                }
                if (serverListCb) serverListCb();
            }
            continue;
        }

        if (n >= 1 && buf[0] == (uint8_t)PacketType::QueryResponse) {
            ServerInfo info;
            parseServerResponse(buf + 1, n - 1, info);
            std::string key = info.addr.toString();
            if (impl->seenServers.find(key) == impl->seenServers.end()) {
                impl->seenServers[key] = info;
                servers.push_back(info);
                if (serverListCb) serverListCb();
            }
        }
    }

    // Auto-stop query after 3 seconds
    if (impl->querying) {
        double now = Engine::instance().timer().now();
        if (now - impl->queryStartTime > 3.0) {
            impl->querying = false;
            if (serverListCb) serverListCb();
        }
    }
}

Connection* NetworkManager::createConnection() {
    return new Connection;
}

void NetworkManager::destroyConnection(Connection* conn) {
    delete conn;
}

void NetworkManager::queryLanServers() {
    Console::instance().printf(LogLevel::Info, "Querying LAN servers...");
    int sock = impl->ensureBroadcastSock();
    if (sock < 0) return;

    // Clear existing results
    servers.clear();
    impl->seenServers.clear();
    impl->nativeInfoRequested.clear();

    // Native V12 servers answer GamePingRequest on both LAN and game ports.
    const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(T2Protocol::LAN_QUERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, queryPacket.data(), queryPacket.size(), 0,
           (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    // Also broadcast on standard T2 port
    broadcastAddr.sin_port = htons(T2Protocol::DEFAULT_PORT);
    sendto(sock, queryPacket.data(), queryPacket.size(), 0,
           (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    impl->querying = true;
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;

    if (serverListCb) serverListCb();
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    Console::instance().printf(LogLevel::Info, "Querying master: %s", masterUrl);
}
