#include "net/network.h"
#include "net/master_query.h"
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
#include <deque>
#include <chrono>
#include <map>
#include <utility>
#include <sstream>
#include <random>
#include <curl/curl.h>

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
    uint32_t recvMask = 0;
    bool haveReceivedSequence = false;

    // Sent reliable packets keyed by sequence number
    struct SentPacket {
        std::vector<uint8_t> data;
        PacketType type;
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
    std::map<uint16_t, V12::PlayerGhostState> nativePlayerStates;
    std::map<uint16_t, V12::ServerEvent::TargetInfo> nativeTargets;
    uint32_t nativeMissionCrc = 0;
    std::map<uint16_t, std::string> nativeDatablockShapes;
    std::deque<std::vector<uint8_t>> injectedObserverPackets;
    std::map<int, Connection::ObserverSnapshot::TeamState> nativeTeamScores;
    std::map<int, int> nativePlayerScores;
    std::map<int, int> nativePlayerPings;
    std::map<int, int> nativePlayerPacketLoss;
    std::map<int, int> nativeClientTargets;
    std::map<int, int> nativeClientTeams;
    std::map<int, std::string> nativeClientNames;
    V12Vec3 nativeCompressionPoint{};
    uint16_t nativeControlGhost = 0;
    uint32_t nativeLastMoveAck = 0;
    uint8_t nativePlayerSensorGroup = 0;
    bool nativeMatchStarted = false;
    bool nativeMatchEnded = false;
    uint32_t nativeClockRemainingMs = 0;
    std::vector<std::string> nativeLoadInfoLines;
    std::map<uint32_t, std::vector<V12::ClientEvent>> sentNativeEventPackets;
    uint8_t nextNativeEventSequence = 0;
    bool nativeRateAdvertised = false;
    bool pendingNativeMove = false;
    uint32_t pendingNativeMoveStart = 0;
    V12::ClientMove pendingNativeMoveData;
    std::vector<V12::ClientEvent> pendingNativeEvents;
    double lastNativeDataSend = 0;
    std::vector<uint8_t> disconnectPacket;
    uint64_t sentPacketCount = 0;
    uint64_t receivedPacketCount = 0;
    uint64_t sentByteCount = 0;
    uint64_t receivedByteCount = 0;
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
        ++sentPacketCount;
        sentByteCount += packet.size();
    }

    void sendFramedAt(PacketType ptype, uint32_t sequence,
                      const uint8_t* payload, size_t payloadLen) {
        constexpr size_t kMaxUdpPayload = 60 * 1024;
        if (sock < 0 || payloadLen > kMaxUdpPayload || (payloadLen > 0 && !payload)) return;

        // Build wire header
        WireHeader hdr;
        hdr.sequence = sequence;
        hdr.ack = recvSeq;
        // Advertise only packets actually received in the preceding 32 slots.
        hdr.ackMask = recvMask;
        hdr.type = (uint8_t)ptype;
        hdr.checksum = 0; // placeholder

        // Assemble full packet: header + payload
        std::vector<uint8_t> packet;
        packet.resize(sizeof(WireHeader) + payloadLen);
        encodeWireHeader(packet.data(), hdr);
        if (payload && payloadLen > 0)
            memcpy(packet.data() + sizeof(WireHeader), payload, payloadLen);

        // Calculate checksum over header + payload (with checksum=0)
        uint16_t csum = wireChecksum(packet.data(), packet.size());
        packet[13] = (uint8_t)(csum & 0xff);
        packet[14] = (uint8_t)(csum >> 8);

        sendto(sock, packet.data(), packet.size(), 0, (sockaddr*)&addr, sizeof(addr));
        ++sentPacketCount;
        sentByteCount += packet.size();
    }

    // Send a pre-built payload (without wire header) with proper framing.
    void sendFramed(PacketType ptype, const uint8_t* payload, size_t payloadLen, bool reliable) {
        const uint32_t sequence = sendSeq++;
        sendFramedAt(ptype, sequence, payload, payloadLen);

        // Track reliable packets for retransmission
        if (reliable) {
            sentPackets[sequence] = {
                payloadLen == 0 ? std::vector<uint8_t>{}
                                : std::vector<uint8_t>(payload, payload + payloadLen),
                ptype,
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

uint64_t Connection::sentPacketCount() const { return impl->sentPacketCount; }
uint64_t Connection::receivedPacketCount() const { return impl->receivedPacketCount; }
uint64_t Connection::sentByteCount() const { return impl->sentByteCount; }
uint64_t Connection::receivedByteCount() const { return impl->receivedByteCount; }

void Connection::resetProtocolEpoch() {
    impl->nativeProtocol.reset(impl->nativeProtocol.connectionSequence());
    impl->nativeStrings.clear();
    impl->nativeGhosts.clear();
    impl->nativePlayerStates.clear();
    impl->nativeTargets.clear();
    impl->nativeMissionCrc = 0;
    impl->nativeDatablockShapes.clear();
    impl->nativeTeamScores.clear();
    impl->nativePlayerScores.clear();
    impl->nativePlayerPings.clear();
    impl->nativePlayerPacketLoss.clear();
    impl->nativeClientTargets.clear();
    impl->nativeClientTeams.clear();
    impl->nativeClientNames.clear();
    impl->nativeCompressionPoint = {};
    impl->nativeControlGhost = 0;
    impl->nativeLastMoveAck = 0;
    impl->nativePlayerSensorGroup = 0;
    impl->nativeMatchStarted = false;
    impl->nativeMatchEnded = false;
    impl->nativeClockRemainingMs = 0;
    impl->nativeLoadInfoLines.clear();
    impl->connectRequest.clear();
    impl->disconnectPacket.clear();
    impl->disconnectAttempts = 0;
    impl->sentPacketCount = 0;
    impl->receivedPacketCount = 0;
    impl->sentByteCount = 0;
    impl->receivedByteCount = 0;
    impl->nextNativeEventSequence = 0;
    impl->nativeRateAdvertised = false;
    impl->pendingNativeMove = false;
    impl->pendingNativeEvents.clear();
    impl->sentNativeEventPackets.clear();
    impl->lastNativeDataSend = 0;
    impl->recvSeq = 0;
    impl->recvMask = 0;
    impl->haveReceivedSequence = false;
    ++epoch;
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

    // Resolve an IPv4 hostname without the obsolete process-global resolver.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host, service.c_str(), &hints, &resolved) != 0 || !resolved) {
        Console::instance().printf(LogLevel::Error, "Cannot resolve: %s", host);
        close(impl->sock);
        impl->sock = -1;
        return false;
    }

    memcpy(&impl->addr, resolved->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(resolved);

    remoteAddr.ip = impl->addr.sin_addr.s_addr;
    remoteAddr.port = port;

    connState = Connecting;
    impl->connectTime = Engine::instance().timer().now();
    resetProtocolEpoch();

    // Generate random challenge
    std::random_device random;
    impl->challenge[0] = random();
    impl->challenge[1] = random();

    impl->clientConnectSequence = impl->challenge[0];
    impl->serverConnectSequence = 0;
    impl->sendRaw(V12::buildConnectChallengeRequest(
        V12::ProtocolVersion, impl->clientConnectSequence, joinPassword));

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host, port);
    return true;
}

Connection::ObserverSnapshot Connection::observerSnapshot() const {
    ObserverSnapshot snapshot;
    snapshot.epoch = epoch;
    snapshot.missionCrc = impl->nativeMissionCrc;
    snapshot.compressionPoint = impl->nativeCompressionPoint;
    snapshot.controlGhost = impl->nativeControlGhost;
    snapshot.lastMoveAck = impl->nativeLastMoveAck;
    snapshot.playerSensorGroup = impl->nativePlayerSensorGroup;
    snapshot.matchStarted = impl->nativeMatchStarted;
    snapshot.matchEnded = impl->nativeMatchEnded;
    snapshot.clockRemainingMs = impl->nativeClockRemainingMs;
    snapshot.loadInfoLines = impl->nativeLoadInfoLines;
    snapshot.protocol = impl->nativeProtocol.snapshot();
    snapshot.strings = impl->nativeStrings.entries();
    snapshot.datablockShapes = impl->nativeDatablockShapes;
    snapshot.players.reserve(impl->nativePlayerStates.size());
    for (const auto& [index, state] : impl->nativePlayerStates)
        snapshot.players.emplace_back(index, state);
    snapshot.ghostClasses.reserve(impl->nativePlayerStates.size());
    for (const auto& [index, state] : impl->nativePlayerStates) {
        if (const auto* ghost = impl->nativeGhosts.get(index))
            snapshot.ghostClasses.emplace_back(index, ghost->classId);
    }
    snapshot.targets.reserve(impl->nativeTargets.size());
    for (const auto& [id, target] : impl->nativeTargets)
        snapshot.targets.push_back(target);
    for (const auto& [id, team] : impl->nativeTeamScores)
        snapshot.teams.push_back(team);
    snapshot.playerScores = impl->nativePlayerScores;
    snapshot.playerPings = impl->nativePlayerPings;
    snapshot.playerPacketLoss = impl->nativePlayerPacketLoss;
    snapshot.clientTargets = impl->nativeClientTargets;
    snapshot.clientTeams = impl->nativeClientTeams;
    snapshot.clientNames = impl->nativeClientNames;
    return snapshot;
}

bool Connection::seedObserverSnapshot(const ObserverSnapshot& snapshot) {
    if (snapshot.strings.size() > 4096 || snapshot.datablockShapes.size() > 2048 ||
        snapshot.players.size() > 1024 || snapshot.targets.size() > 512)
        return false;
    for (const auto& [id, value] : snapshot.strings)
        if (id >= 4096 || value.size() > 255) return false;
    for (const auto& [id, value] : snapshot.datablockShapes)
        if (id >= 2048 || value.size() > 1024) return false;
    for (const auto& [index, state] : snapshot.players)
        if (index >= 1024) return false;
    for (size_t i = 0; i < snapshot.players.size(); ++i)
        for (size_t j = i + 1; j < snapshot.players.size(); ++j)
            if (snapshot.players[i].first == snapshot.players[j].first) return false;
    for (const auto& [index, classId] : snapshot.ghostClasses)
        if (index >= 1024 || classId >= V12::GhostClassCount) return false;
    for (const auto& target : snapshot.targets)
        if (target.targetId >= 512) return false;

    impl->nativeStrings.clear();
    epoch = snapshot.epoch;
    impl->nativeProtocol.restore(snapshot.protocol);
    impl->nativeGhosts.clear();
    impl->nativePlayerStates.clear();
    impl->nativeTargets.clear();
    impl->nativeDatablockShapes = snapshot.datablockShapes;
    impl->nativeMissionCrc = snapshot.missionCrc;
    impl->nativeCompressionPoint = snapshot.compressionPoint;
    impl->nativeControlGhost = snapshot.controlGhost;
    impl->nativeLastMoveAck = snapshot.lastMoveAck;
    impl->nativePlayerSensorGroup = snapshot.playerSensorGroup;
    impl->nativeMatchStarted = snapshot.matchStarted;
    impl->nativeMatchEnded = snapshot.matchEnded;
    impl->nativeClockRemainingMs = snapshot.clockRemainingMs;
    impl->nativeLoadInfoLines = snapshot.loadInfoLines;
    impl->nativeTeamScores.clear();
    for (const auto& team : snapshot.teams)
        if (team.teamId > 0 && team.teamId < 64) impl->nativeTeamScores[team.teamId] = team;
    impl->nativePlayerScores = snapshot.playerScores;
    impl->nativePlayerPings = snapshot.playerPings;
    impl->nativePlayerPacketLoss = snapshot.playerPacketLoss;
    impl->nativeClientTargets = snapshot.clientTargets;
    impl->nativeClientTeams = snapshot.clientTeams;
    impl->nativeClientNames = snapshot.clientNames;
    for (const auto& [id, value] : snapshot.strings)
        impl->nativeStrings.set(id, value);
    for (const auto& target : snapshot.targets)
        impl->nativeTargets[target.targetId] = target;
    for (const auto& [index, state] : snapshot.players) {
        uint16_t classId = 25;
        for (const auto& [classIndex, listedClass] : snapshot.ghostClasses) {
            if (classIndex == index) { classId = listedClass; break; }
        }
        if (!impl->nativeGhosts.create(index, classId)) return false;
        impl->nativePlayerStates[index] = state;
    }
    if (targetCb) {
        for (const auto& target : snapshot.targets)
            targetCb(&impl->nativeTargets[target.targetId], target.targetId);
    }
    if (serverMessageCb) {
        if (snapshot.matchStarted) serverMessageCb({"ServerMessage", "MsgMissionStart"});
        if (snapshot.matchEnded) serverMessageCb({"ServerMessage", "MsgDebriefResult"});
        if (snapshot.clockRemainingMs > 0)
            serverMessageCb({"ServerMessage", "MsgSystemClock", "", std::to_string(snapshot.clockRemainingMs)});
        if (!snapshot.loadInfoLines.empty()) {
            serverMessageCb({"ServerMessage", "MsgLoadInfo"});
            for (const auto& line : snapshot.loadInfoLines)
                serverMessageCb({"ServerMessage", "MsgLoadObjectiveLine", line});
            serverMessageCb({"ServerMessage", "MsgLoadInfoDone"});
        }
        for (const auto& team : snapshot.teams) {
            serverMessageCb({"ServerMessage", "MsgCTFAddTeam",
                             std::to_string(team.teamId), team.name,
                             team.flagStatus == "home" ? "<At Base>" :
                             team.flagStatus == "field" ? "<In the Field>" : team.flagCarrier,
                             std::to_string(team.score)});
        }
        for (const auto& [clientId, targetId] : snapshot.clientTargets) {
            const auto name = snapshot.clientNames.find(clientId);
            serverMessageCb({"ServerMessage", "MsgClientJoin",
                             name == snapshot.clientNames.end() ? "" : name->second,
                             std::to_string(clientId), std::to_string(targetId)});
            const auto score = snapshot.playerScores.find(clientId);
            if (score != snapshot.playerScores.end())
            serverMessageCb({"ServerMessage", "MsgPlayerScore",
                                 std::to_string(clientId), std::to_string(score->second),
                                 std::to_string(snapshot.playerPings.contains(clientId)
                                     ? snapshot.playerPings.at(clientId) : 0),
                                 std::to_string(snapshot.playerPacketLoss.contains(clientId)
                                     ? snapshot.playerPacketLoss.at(clientId) : 0)});
            const auto team = snapshot.clientTeams.find(clientId);
            if (team != snapshot.clientTeams.end())
                serverMessageCb({"ServerMessage", "MsgClientJoinTeam", "", "",
                                 std::to_string(clientId), std::to_string(team->second)});
        }
    }
    if (ghostCb) {
        for (const auto& [index, state] : snapshot.players) {
            const auto* ghost = impl->nativeGhosts.get(index);
            V12::GhostUpdate update;
            update.operation = V12::GhostUpdate::Operation::Create;
            update.index = index;
            update.classId = ghost ? ghost->classId : 25;
            ghostCb(update, &impl->nativePlayerStates[index]);
        }
    }
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
    impl->injectedObserverPackets.clear();
}

void Connection::update() {
    if (impl->sock < 0 && impl->injectedObserverPackets.empty()) return;

    double now = Engine::instance().timer().now();

    if ((connState == Connecting || connState == Challenging) &&
        now - impl->connectTime > 10.0) {
        Console::instance().printf(LogLevel::Warn, "Connection timeout");
        if (connectCb) connectCb(false);
        disconnect();
        return;
    }
    if (connState >= Connected && impl->lastReceive > 0 &&
        now - impl->lastReceive > 15.0) {
        Console::instance().printf(LogLevel::Warn, "Connection receive timeout");
        disconnect();
        return;
    }

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
        int n = 0;
        const bool injected = !impl->injectedObserverPackets.empty();
        if (injected) {
            const auto packet = std::move(impl->injectedObserverPackets.front());
            impl->injectedObserverPackets.pop_front();
            n = (int)packet.size();
            memcpy(buf, packet.data(), packet.size());
        } else if (impl->sock >= 0) {
            n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if (n <= 0) break;
        } else {
            break;
        }
        if (!injected && (from.sin_addr.s_addr != impl->addr.sin_addr.s_addr ||
            from.sin_port != impl->addr.sin_port)
        )
            continue;
        ++impl->receivedPacketCount;
        impl->receivedByteCount += (size_t)n;

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
                        {observerMode ? "ImaWatcher" : playerName,
                         "Male Human", "beagle", "male1", "1.0"});
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
                        bool endGhosting = false;
                        impl->nativeLastMoveAck = gameState.lastMoveAck;
                        if (gameState.hasCompressionPoint)
                            impl->nativeCompressionPoint = gameState.compressionPoint;
                        if (gameState.controlPresent && !gameState.controlDirty)
                            impl->nativeControlGhost = gameState.controlGhost;
                        if (stateCb) stateCb(gameState);
                        for (const auto& event : events) {
                            if (event.hasSensorGroup)
                                impl->nativePlayerSensorGroup = event.sensorGroup;
                            if (event.classId == 9 && commandCb)
                                commandCb(event.message);
                            if (event.classId == 9 && clientCommandCb)
                                clientCommandCb(event.arguments);
                            if (event.classId == 9 && event.arguments.size() >= 2 &&
                                event.arguments[0] == "ServerMessage") {
                                const auto& args = event.arguments;
                                const std::string& type = args[1];
                                if (type == "MsgMissionStart") {
                                    impl->nativeMatchStarted = true;
                                    impl->nativeMatchEnded = false;
                                } else if (type == "MsgClientReady") {
                                    impl->nativeMatchStarted = false;
                                    impl->nativeMatchEnded = false;
                                } else if (type == "MsgClearDebrief" || type == "MsgDebriefResult") {
                                    impl->nativeMatchEnded = true;
                                } else if (type == "MsgSystemClock" && args.size() >= 4) {
                                    impl->nativeClockRemainingMs = (uint32_t)std::max(0, atoi(args[3].c_str()));
                                } else if (type == "MsgLoadInfo") {
                                    impl->nativeLoadInfoLines.clear();
                                } else if ((type == "MsgLoadQuoteLine" || type == "MsgLoadObjectiveLine" ||
                                            type == "MsgLoadRulesLine") && args.size() >= 3 &&
                                           impl->nativeLoadInfoLines.size() < 128) {
                                    impl->nativeLoadInfoLines.push_back(args[2]);
                                }
                                if ((type == "MsgTeamScoreIs" || type == "MsgTeamScore") &&
                                    args.size() >= 5) {
                                    const int teamId = atoi(args[2].c_str());
                                    if (teamId > 0 && teamId < 64) {
                                        auto& team = impl->nativeTeamScores[teamId];
                                        team.teamId = teamId;
                                        team.score = atoi(args[3].c_str());
                                    }
                                } else if (type == "MsgCTFAddTeam" && args.size() >= 6) {
                                    const int teamId = atoi(args[2].c_str());
                                    if (teamId > 0 && teamId < 64) {
                                        auto& team = impl->nativeTeamScores[teamId];
                                        team.teamId = teamId;
                                        team.name = args[3];
                                        team.flagStatus = args[4].find("At Base") == 0 ? "home" :
                                            args[4].find("In the Field") == 0 ? "field" : "held";
                                        team.flagCarrier = team.flagStatus == "held" ? args[4] : "";
                                        team.score = atoi(args[5].c_str());
                                    }
                                } else if ((type == "MsgCTFFlagTaken" ||
                                            type == "MsgCTFFlagDropped" ||
                                            type == "MsgCTFFlagReturned" ||
                                            type == "MsgCTFFlagCapped") && args.size() >= 6) {
                                    const int teamId = atoi(args[4].c_str());
                                    if (teamId > 0 && teamId < 64) {
                                        auto& team = impl->nativeTeamScores[teamId];
                                        team.teamId = teamId;
                                        team.flagStatus = type == "MsgCTFFlagTaken" ? "held" :
                                            type == "MsgCTFFlagDropped" ? "field" : "home";
                                        team.flagCarrier = team.flagStatus == "held" ? args[2] : "";
                                    }
                                } else if (type == "MsgPlayerScore" && args.size() >= 5) {
                                    const int clientId = atoi(args[2].c_str());
                                    if (clientId >= 0 && clientId < 1024)
                                        impl->nativePlayerScores[clientId] = atoi(args[3].c_str());
                                    if (clientId >= 0 && clientId < 1024 && args.size() >= 6) {
                                        impl->nativePlayerPings[clientId] = atoi(args[4].c_str());
                                        impl->nativePlayerPacketLoss[clientId] = atoi(args[5].c_str());
                                    }
                                } else if (type == "MsgClientJoin" && args.size() >= 5) {
                                    const int clientId = atoi(args[3].c_str());
                                    const int targetId = atoi(args[4].c_str());
                                    if (clientId >= 0 && clientId < 1024 &&
                                        targetId >= 0 && targetId < 1024) {
                                        impl->nativeClientTargets[clientId] = targetId;
                                        impl->nativeClientNames[clientId] = args[2];
                                    }
                                } else if (type == "MsgClientDrop" && args.size() >= 4) {
                                    const int clientId = atoi(args[3].c_str());
                                    impl->nativeClientTargets.erase(clientId);
                                    impl->nativeClientNames.erase(clientId);
                                    impl->nativePlayerScores.erase(clientId);
                                    impl->nativePlayerPings.erase(clientId);
                                    impl->nativePlayerPacketLoss.erase(clientId);
                                } else if (type == "MsgClientNameChanged" && args.size() >= 5) {
                                    const int clientId = atoi(args[4].c_str());
                                    if (clientId >= 0 && clientId < 1024)
                                        impl->nativeClientNames[clientId] = args[3];
                                } else if (type == "MsgClientJoinTeam" && args.size() >= 6) {
                                    const int clientId = atoi(args[4].c_str());
                                    const int teamId = atoi(args[5].c_str());
                                    if (clientId >= 0 && clientId < 1024 && teamId >= 0 && teamId < 64)
                                        impl->nativeClientTeams[clientId] = teamId;
                                }
                            }
                            if (event.classId == 9 && serverMessageCb && !event.arguments.empty())
                                serverMessageCb(event.arguments);
                            if (event.hasTargetInfo && targetCb)
                                targetCb(&event.targetInfo, event.targetInfo.targetId);
                            if (event.hasTargetInfo)
                                impl->nativeTargets[event.targetInfo.targetId] = event.targetInfo;
                            if (event.hasTargetFree && targetCb)
                                targetCb(nullptr, event.targetFreeId);
                            if (event.hasTargetFree)
                                impl->nativeTargets.erase(event.targetFreeId);
                            if (event.hasMissionCrc && missionCb)
                                missionCb(event.missionCrc);
                            if (event.hasMissionCrc)
                                impl->nativeMissionCrc = event.missionCrc;
                            if (event.hasDatablock) {
                                if (!event.datablockData.shapeFile.empty())
                                    impl->nativeDatablockShapes[event.datablockObject] =
                                        event.datablockData.shapeFile;
                                if (datablockCb)
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
                            if (event.hasGhostingMessage && event.ghostMessage == 2)
                                endGhosting = true;
                        }
                        std::vector<V12::GhostUpdate> updates;
                        std::map<uint16_t, V12::PlayerGhostState> playerStates;
                        const bool ghostsOk = V12::readGhostUpdates(
                            payload, impl->nativeGhosts, updates,
                            [&](V12BitStream& ghost, uint16_t index, uint16_t classId, bool initial) {
                                V12::PlayerGhostState state;
                                const bool ok = V12::readGhostPayload(
                                    ghost, classId, initial, compressionPoint, &state);
                                if (ok && (classId == 16 || classId == 22 || classId == 25 || classId == 29 ||
                                           classId == 37 || classId == 39 || classId == 51))
                                    playerStates[index] = state;
                                return ok;
                            });
                        if (ghostsOk) {
                            for (const auto& update : updates) {
                                if (update.operation == V12::GhostUpdate::Operation::Delete) {
                                    impl->nativePlayerStates.erase(update.index);
                                    if (ghostCb) ghostCb(update, nullptr);
                                } else {
                                    auto state = playerStates.find(update.index);
                                    if (state == playerStates.end()) {
                                        if (ghostCb) ghostCb(update, nullptr);
                                        continue;
                                    }
                                    if (update.operation == V12::GhostUpdate::Operation::Create) {
                                        impl->nativePlayerStates[update.index] = state->second;
                                    } else {
                                        auto previous = impl->nativePlayerStates.find(update.index);
                                        if (previous == impl->nativePlayerStates.end())
                                            impl->nativePlayerStates[update.index] = state->second;
                                        else
                                            previous->second = V12::mergePlayerGhostState(
                                                previous->second, state->second);
                                    }
                                    if (ghostCb)
                                        ghostCb(update, &impl->nativePlayerStates[update.index]);
                                }
                            }
                        }
                        if (endGhosting) {
                            resetProtocolEpoch();
                            if (epochCb) epochCb(epoch);
                        }
                    }
                }
                continue;
            }
        }

        if ((size_t)n < sizeof(WireHeader)) continue;

        // Parse wire header
        WireHeader hdr;
        hdr = decodeWireHeader(buf);

        // Verify checksum
        uint16_t savedCsum = hdr.checksum;
        WireHeader hdrNoCsum = hdr;
        hdrNoCsum.checksum = 0;
        std::vector<uint8_t> tmp(sizeof(WireHeader) + n - sizeof(WireHeader));
        encodeWireHeader(tmp.data(), hdrNoCsum);
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

        // Update the actual receive window used for future acknowledgements.
        if (!impl->haveReceivedSequence) {
            impl->recvSeq = hdr.sequence;
            impl->recvMask = 0;
            impl->haveReceivedSequence = true;
        } else if (hdr.sequence > impl->recvSeq) {
            const uint32_t diff = hdr.sequence - impl->recvSeq;
            if (diff < 32)
                impl->recvMask = (impl->recvMask << diff) | (1u << (diff - 1));
            else
                impl->recvMask = 0;
            impl->recvSeq = hdr.sequence;
        } else if (hdr.sequence < impl->recvSeq) {
            const uint32_t diff = impl->recvSeq - hdr.sequence;
            if (diff <= 32)
                impl->recvMask |= 1u << (diff - 1);
        }

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
                impl->sendFramedAt(sp.type, seq, sp.data.data(), sp.data.size());
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

bool Connection::ingestObserverPacket(const uint8_t* data, size_t size) {
    constexpr size_t MaxObserverPacket = 2048;
    constexpr size_t MaxQueuedObserverPackets = 256;
    if (!data || size == 0 || size > MaxObserverPacket ||
        connState < Connected || impl->injectedObserverPackets.size() >= MaxQueuedObserverPackets)
        return false;
    impl->injectedObserverPackets.emplace_back(data, data + size);
    return true;
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
    if (observerMode && !isObserverSetupCommand(argv)) {
        Console::instance().printf(LogLevel::Warn,
            "Ignored observer command: %s", argv.front().c_str());
        return;
    }
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
    std::vector<sockaddr_in> queryTargets;
    int queryAttempts = 0;
    double queryLastSent = 0;

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

static size_t masterResponseWrite(char* data, size_t size, size_t count, void* user) {
    auto* response = static_cast<std::string*>(user);
    const size_t bytes = size * count;
    constexpr size_t kMaxMasterResponse = 1024 * 1024;
    if (bytes > kMaxMasterResponse || response->size() > kMaxMasterResponse - bytes)
        return 0;
    response->append(data, bytes);
    return bytes;
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
    info.numBots = stream.readU8();
    if (stream.failed()) return false;
    info.password = (status & 0x02) != 0;
    info.tournament = (status & 0x04) != 0;
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
        if (!impl->queryTargets.empty() && impl->queryAttempts < 3 &&
            now - impl->queryLastSent >= 0.75) {
            const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
            for (const auto& target : impl->queryTargets)
                sendto(impl->broadcastSock, queryPacket.data(), queryPacket.size(), 0,
                       (const sockaddr*)&target, sizeof(target));
            ++impl->queryAttempts;
            impl->queryLastSent = now;
        }
        if (now - impl->queryStartTime > 3.0) {
            impl->querying = false;
            impl->queryTargets.clear();
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
    impl->queryTargets.clear();
    impl->queryAttempts = 0;

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
    impl->queryLastSent = impl->querySentAt;

    if (serverListCb) serverListCb();
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    const std::string configured = masterUrl && *masterUrl
        ? masterUrl : "http://master.tribesnext.com/list";
    std::string url = configured;
    if (url.find("http://") != 0 && url.find("https://") != 0)
        url = "http://" + url;
    if (url.find("/list", url.find("://") + 3) == std::string::npos)
        url += "/list";

    Console::instance().printf(LogLevel::Info, "Querying master: %s", url.c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        Console::instance().printf(LogLevel::Error, "Master query: libcurl unavailable");
        return;
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, masterResponseWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Torch/0.1 Tribes2 client");
    const CURLcode result = curl_easy_perform(curl);
    long httpStatus = 0;
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || httpStatus < 200 || httpStatus >= 300) {
        if (result == CURLE_OK)
            Console::instance().printf(LogLevel::Warn,
                "Master query failed with HTTP status %ld", httpStatus);
        else
        Console::instance().printf(LogLevel::Warn, "Master query failed: %s", curl_easy_strerror(result));
        return;
    }

    servers.clear();
    impl->seenServers.clear();
    impl->nativeInfoRequested.clear();
    int queried = 0;
    if (impl->ensureBroadcastSock() < 0) {
        Console::instance().printf(LogLevel::Error, "Master query: unable to create UDP query socket");
        return;
    }
    std::istringstream lines(body);
    std::string line;
    const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
    while (std::getline(lines, line) && queried < 512) {
        std::string host;
        uint16_t port = 0;
        if (!TorchMaster::parseAddressLine(line, host, port)) continue;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* resultAddr = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &resultAddr) != 0 || !resultAddr)
            continue;
        auto* target = reinterpret_cast<sockaddr_in*>(resultAddr->ai_addr);
        impl->queryTargets.push_back(*target);
        sendto(impl->broadcastSock, queryPacket.data(), queryPacket.size(), 0,
               reinterpret_cast<sockaddr*>(target), sizeof(sockaddr_in));
        freeaddrinfo(resultAddr);
        queried++;
    }
    impl->querying = queried > 0;
    impl->queryAttempts = queried > 0 ? 1 : 0;
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;
    if (serverListCb) serverListCb();
    Console::instance().printf(LogLevel::Info, "Master query dispatched %d server probes", queried);
}

void NetworkManager::stopServerQuery() {
    impl->querying = false;
    impl->nativeInfoRequested.clear();
    impl->queryTargets.clear();
    impl->queryAttempts = 0;
    if (serverListCb) serverListCb();
}

void NetworkManager::querySingleServer(const char* address) {
    if (!address || !*address) return;
    std::string host;
    uint16_t port = 0;
    if (!TorchMaster::parseAddressLine(address, host, port)) {
        Console::instance().printf(LogLevel::Warn, "Invalid server address: %s", address);
        return;
    }
    if (impl->ensureBroadcastSock() < 0) return;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0 || !resolved)
        return;
    const auto packet = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
    impl->queryTargets.clear();
    impl->queryTargets.push_back(*reinterpret_cast<sockaddr_in*>(resolved->ai_addr));
    sendto(impl->broadcastSock, packet.data(), packet.size(), 0,
           resolved->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(resolved);
    impl->querying = true;
    impl->queryAttempts = 1;
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;
    impl->queryLastSent = impl->querySentAt;
    if (serverListCb) serverListCb();
}

bool NetworkManager::isServerQueryActive() const {
    return impl->querying;
}
