#include "net/network.h"
#include "net/master_query.h"
#include "net/v12_protocol.h"
#include "core/console.h"
#include "core/config.h"
#include "core/engine.h"
#include "game/ctf_runtime.h"
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <deque>
#include <chrono>
#include <map>
#include <utility>
#include <sstream>
#include <random>
#include <curl/curl.h>

static const char* nativeFlagStatus(const std::string& status) {
    switch (CtfRuntime::classifyStatusToken(status)) {
        case CtfRuntime::StatusToken::Home: return "home";
        case CtfRuntime::StatusToken::Field: return "field";
        case CtfRuntime::StatusToken::Held: return "held";
    }
    return "home";
}

static bool parseMessageIndex(const std::string& text, int& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < 0 || parsed >= 1024) return false;
    value = (int)parsed;
    return true;
}

// ─── Connection ───────────────────────────────────────────────────

struct Connection::Impl {
    int sock = -1;
    sockaddr_in addr{};

    // Connection timers
    double connectTime = 0;
    double lastPing = 0;
    uint32_t clientConnectSequence = 0;
    uint32_t serverConnectSequence = 0;
    std::vector<uint8_t> connectRequest;
    V12::ProtocolState nativeProtocol;
    V12::NetStringTable nativeStrings;
    V12::GhostTracker nativeGhosts;
    std::map<uint16_t, V12::PlayerGhostState> nativePlayerStates;
    std::map<uint16_t, V12::PlayerGhostState> pendingNativeStats;
    std::map<uint16_t, uint16_t> nativeGhostDatablocks;
    // A stock projectile reports its hidden/explosion transition once.
    std::set<uint16_t> nativeProjectileImpacts;
    std::map<uint16_t, V12::ServerEvent::TargetInfo> nativeTargets;
    uint32_t nativeMissionCrc = 0;
    std::map<uint16_t, std::string> nativeDatablockShapes;
    std::map<uint16_t, V12::DecodedDataBlock> nativeDatablocks;
    std::deque<std::vector<uint8_t>> injectedObserverPackets;
    std::map<int, Connection::ObserverSnapshot::TeamState> nativeTeamScores;
    std::map<int, int> nativePlayerScores;
    std::map<int, int> nativePlayerKills;
    std::map<int, int> nativePlayerDeaths;
    std::map<int, int> nativePlayerPings;
    std::map<int, int> nativePlayerPacketLoss;
    std::map<int, int> nativeClientTargets;
    std::map<int, int> nativeClientTeams;
    std::map<int, std::string> nativeClientNames;
    std::map<std::pair<int, uint32_t>, uint32_t> nativeSensorGroupColors;
    std::array<uint32_t, 16> nativeTargetVisible{};
    V12Vec3 nativeCompressionPoint{};
    uint16_t nativeControlGhost = 0;
    bool nativeControlAssigned = false;
    uint32_t nativeLastMoveAck = 0;
    uint8_t nativePlayerSensorGroup = 0;
    bool nativeHasCameraFov = false;
    uint8_t nativeCameraFov = 0;
    bool nativeMatchStarted = false;
    bool nativeMatchEnded = false;
    bool nativeGhosting = false;
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

    void clearNativeState() {
        nativeProtocol.reset(nativeProtocol.connectionSequence());
        nativeStrings.clear();
        nativeGhosts.clear();
        nativePlayerStates.clear();
        pendingNativeStats.clear();
        nativeGhostDatablocks.clear();
        nativeProjectileImpacts.clear();
        nativeTargets.clear();
        nativeMissionCrc = 0;
        nativeDatablockShapes.clear();
        nativeDatablocks.clear();
        nativeTeamScores.clear();
        nativePlayerScores.clear();
        nativePlayerKills.clear();
        nativePlayerDeaths.clear();
        nativePlayerPings.clear();
        nativePlayerPacketLoss.clear();
        nativeClientTargets.clear();
        nativeClientTeams.clear();
        nativeClientNames.clear();
        nativeSensorGroupColors.clear();
        nativeTargetVisible.fill(0);
        nativeCompressionPoint = {};
        nativeControlGhost = 0;
        nativeControlAssigned = false;
        nativeLastMoveAck = 0;
        nativePlayerSensorGroup = 0;
        nativeHasCameraFov = false;
        nativeCameraFov = 0;
        nativeMatchStarted = false;
        nativeMatchEnded = false;
        nativeGhosting = false;
        nativeClockRemainingMs = 0;
        nativeLoadInfoLines.clear();
        sentNativeEventPackets.clear();
        pendingNativeEvents.clear();
        pendingNativeMove = false;
        nativeRateAdvertised = false;
        lastNativeDataSend = 0;
        lastPing = 0;
    }

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
    impl->clearNativeState();
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
    ++epoch;
}

void Connection::sendVoiceEvent(uint8_t sequence, uint8_t codec, uint8_t stream,
                                bool endOfStream,
                                const std::vector<std::array<uint8_t, 33>>& frames) {
    if (!isConnected() || codec != 3 || frames.size() > 6 ||
        (!endOfStream && frames.size() != 1)) return;
    impl->pendingNativeEvents.push_back(
        V12::makeVoiceStreamEvent(sequence, codec, stream, endOfStream, frames));
    impl->flushNativeMove();
}

bool Connection::connect(const char* host, uint16_t port) {
    if (impl->sock >= 0) {
        close(impl->sock);
        impl->sock = -1;
    }
    impl->disconnectPacket.clear();
    impl->serverConnectSequence = 0;
    connState = Disconnected;
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

    std::random_device random;
    impl->clientConnectSequence = random();
    impl->serverConnectSequence = 0;
    impl->sendRaw(V12::buildConnectChallengeRequest(
        V12::ProtocolVersion, impl->clientConnectSequence));

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host, port);
    return true;
}

Connection::ObserverSnapshot Connection::observerSnapshot() const {
    ObserverSnapshot snapshot;
    snapshot.epoch = epoch;
    snapshot.missionCrc = impl->nativeMissionCrc;
    snapshot.compressionPoint = impl->nativeCompressionPoint;
    snapshot.hasCameraFov = impl->nativeHasCameraFov;
    snapshot.cameraFov = impl->nativeCameraFov;
    snapshot.controlGhost = impl->nativeControlGhost;
    snapshot.controlAssigned = impl->nativeControlAssigned;
    snapshot.lastMoveAck = impl->nativeLastMoveAck;
    snapshot.playerSensorGroup = impl->nativePlayerSensorGroup;
    snapshot.matchStarted = impl->nativeMatchStarted;
    snapshot.matchEnded = impl->nativeMatchEnded;
    snapshot.ghosting = impl->nativeGhosting;
    snapshot.clockRemainingMs = impl->nativeClockRemainingMs;
    snapshot.loadInfoLines = impl->nativeLoadInfoLines;
    snapshot.protocol = impl->nativeProtocol.snapshot();
    snapshot.strings = impl->nativeStrings.entries();
    snapshot.datablockShapes = impl->nativeDatablockShapes;
    snapshot.datablocks = impl->nativeDatablocks;
    snapshot.players.reserve(impl->nativePlayerStates.size());
    for (const auto& [index, state] : impl->nativePlayerStates)
        snapshot.players.emplace_back(index, state);
    snapshot.ghostClasses.reserve(impl->nativeGhosts.size());
    for (const auto& ghost : impl->nativeGhosts.entries())
        snapshot.ghostClasses.emplace_back(ghost.index, ghost.classId);
    snapshot.targets.reserve(impl->nativeTargets.size());
    for (const auto& [id, target] : impl->nativeTargets)
        snapshot.targets.push_back(target);
    for (const auto& [id, team] : impl->nativeTeamScores)
        snapshot.teams.push_back(team);
    snapshot.playerScores = impl->nativePlayerScores;
    snapshot.playerKills = impl->nativePlayerKills;
    snapshot.playerDeaths = impl->nativePlayerDeaths;
    snapshot.playerPings = impl->nativePlayerPings;
    snapshot.playerPacketLoss = impl->nativePlayerPacketLoss;
    snapshot.clientTargets = impl->nativeClientTargets;
    snapshot.clientTeams = impl->nativeClientTeams;
    snapshot.clientNames = impl->nativeClientNames;
    snapshot.sensorGroupColors = impl->nativeSensorGroupColors;
    snapshot.targetVisible = impl->nativeTargetVisible;
    // The packet state is folded into the observer snapshot by the game
    // callback; keep the map available for snapshot/seek users as well.
    return snapshot;
}

bool Connection::seedObserverSnapshot(const ObserverSnapshot& snapshot) {
    if (snapshot.strings.size() > 4096 || snapshot.datablockShapes.size() > 2048 ||
        snapshot.datablocks.size() > 2048 || snapshot.players.size() > 1024 ||
        snapshot.targets.size() > 512)
        return false;
    for (const auto& [id, value] : snapshot.strings)
        if (id >= 4096 || value.size() > 255) return false;
    for (const auto& [id, value] : snapshot.datablockShapes)
        if (id >= 2048 || value.size() > 1024) return false;
    for (const auto& [id, value] : snapshot.datablocks)
        if (id >= 2048 || value.audioFilename.size() > 1024) return false;
    for (const auto& [index, state] : snapshot.players)
        if (index >= 1024) return false;
    for (size_t i = 0; i < snapshot.players.size(); ++i)
        for (size_t j = i + 1; j < snapshot.players.size(); ++j)
            if (snapshot.players[i].first == snapshot.players[j].first) return false;
    for (const auto& [index, classId] : snapshot.ghostClasses)
        if (index >= 1024 || classId >= V12::GhostClassCount) return false;
    if (snapshot.ghostClasses.size() > 1024) return false;
    for (size_t i = 0; i < snapshot.ghostClasses.size(); ++i)
        for (size_t j = i + 1; j < snapshot.ghostClasses.size(); ++j)
            if (snapshot.ghostClasses[i].first == snapshot.ghostClasses[j].first) return false;
    for (const auto& target : snapshot.targets)
        if (target.targetId >= 512) return false;

    impl->nativeStrings.clear();
    epoch = snapshot.epoch;
    impl->nativeProtocol.restore(snapshot.protocol);
    impl->nativeGhosts.clear();
    impl->nativePlayerStates.clear();
    impl->pendingNativeStats.clear();
    impl->nativeGhostDatablocks.clear();
    impl->nativeTargets.clear();
    impl->nativeDatablockShapes = snapshot.datablockShapes;
    impl->nativeDatablocks = snapshot.datablocks;
    impl->nativeMissionCrc = snapshot.missionCrc;
    impl->nativeCompressionPoint = snapshot.compressionPoint;
    impl->nativeHasCameraFov = snapshot.hasCameraFov;
    impl->nativeCameraFov = snapshot.cameraFov;
    impl->nativeControlGhost = snapshot.controlGhost;
    impl->nativeControlAssigned = snapshot.controlAssigned;
    impl->nativeLastMoveAck = snapshot.lastMoveAck;
    impl->nativePlayerSensorGroup = snapshot.playerSensorGroup;
    impl->nativeMatchStarted = snapshot.matchStarted;
    impl->nativeMatchEnded = snapshot.matchEnded;
    impl->nativeGhosting = snapshot.ghosting;
    impl->nativeClockRemainingMs = snapshot.clockRemainingMs;
    impl->nativeLoadInfoLines = snapshot.loadInfoLines;
    impl->nativeTeamScores.clear();
    for (const auto& team : snapshot.teams)
        if (team.teamId > 0 && team.teamId < 64) impl->nativeTeamScores[team.teamId] = team;
    impl->nativePlayerScores = snapshot.playerScores;
    impl->nativePlayerKills = snapshot.playerKills;
    impl->nativePlayerDeaths = snapshot.playerDeaths;
    impl->nativePlayerPings = snapshot.playerPings;
    impl->nativePlayerPacketLoss = snapshot.playerPacketLoss;
    impl->nativeClientTargets = snapshot.clientTargets;
    impl->nativeClientTeams = snapshot.clientTeams;
    impl->nativeClientNames = snapshot.clientNames;
    impl->nativeSensorGroupColors = snapshot.sensorGroupColors;
    impl->nativeTargetVisible = snapshot.targetVisible;
    impl->nativeProjectileImpacts.clear();
    impl->sentNativeEventPackets.clear();
    impl->pendingNativeEvents.clear();
    for (const auto& [id, value] : snapshot.strings)
        impl->nativeStrings.set(id, value);
    for (const auto& target : snapshot.targets)
        impl->nativeTargets[target.targetId] = target;
    for (const auto& [index, classId] : snapshot.ghostClasses)
        if (!impl->nativeGhosts.create(index, classId)) return false;
    for (const auto& [index, state] : snapshot.players) {
        if (!impl->nativeGhosts.get(index)) return false;
        impl->nativePlayerStates[index] = state;
        if (state.hasDatablock)
            impl->nativeGhostDatablocks[index] = state.datablockId;
    }
    if (serverMessageCb) {
        if (snapshot.matchStarted) serverMessageCb({"ServerMessage", "MsgMissionStart"});
        if (snapshot.matchEnded) serverMessageCb({"MissionEnd"});
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
    // The game callback applies target metadata to the matching live ghost,
    // so restore ghosts before targets and the control assignment.
    if (targetCb) {
        for (const auto& target : snapshot.targets)
            targetCb(&impl->nativeTargets[target.targetId], target.targetId);
    }
    if (targetControlCb && snapshot.controlAssigned)
        targetControlCb(true, snapshot.controlGhost, false, {}, true);
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
    impl->injectedObserverPackets.clear();
    // A disconnected browser/observer must not continue displaying the prior
    // mission while the graceful disconnect packet is retried.
    impl->clearNativeState();
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
    // Keep one byte of headroom so an oversized UDP datagram is observable
    // instead of being silently truncated to the native limit.
    uint8_t buf[V12::MaxPacketDataSize + 1];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = 0;
        const bool injected = !impl->injectedObserverPackets.empty();
        if (injected) {
            const auto packet = std::move(impl->injectedObserverPackets.front());
            impl->injectedObserverPackets.pop_front();
            n = (int)packet.size();
            if (packet.size() > V12::MaxPacketDataSize) continue;
            memcpy(buf, packet.data(), packet.size());
        } else if (impl->sock >= 0) {
            n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if (n <= 0) break;
        } else {
            break;
        }
        if ((size_t)n > V12::MaxPacketDataSize) continue;
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
                        {"ImaWatcher",
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
                impl->serverConnectSequence = 0;
                impl->connectRequest.clear();
                 impl->clearNativeState();
                connState = Disconnected;
                if (connectCb) connectCb(false);
                continue;
            }
            continue;
        }

        // Native dnet packets are bit-packed after the first discriminator.
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
                    V12::NetStringTable packetStrings = impl->nativeStrings;
                    V12::GhostTracker packetGhosts = impl->nativeGhosts;
                    if (V12::readServerPacketEvents(payload, packetStrings, events,
                                                     &gameState,
                                                     &compressionPoint, nullptr)) {
                        bool endGhosting = false;
                        impl->nativeLastMoveAck = gameState.lastMoveAck;
                          if (gameState.hasCompressionPoint)
                              impl->nativeCompressionPoint = gameState.compressionPoint;
                          if (gameState.hasCameraFov) {
                              impl->nativeHasCameraFov = true;
                              impl->nativeCameraFov = gameState.cameraFov;
                          }
                         for (const auto& [index, mask] : gameState.targetVisibleToggles)
                             impl->nativeTargetVisible[index & 15] ^= mask;
                        if (gameState.controlPresent && !gameState.controlDirty)
                            impl->nativeControlGhost = gameState.controlGhost;
                        if (stateCb) stateCb(gameState);
                        for (const auto& event : events) {
                             if (event.hasSensorGroup)
                                 impl->nativePlayerSensorGroup = event.sensorGroup;
                             if (event.hasSensorGroupColor) {
                                 for (int targetGroup = 0; targetGroup < 32; ++targetGroup) {
                                     if ((event.sensorColorUpdateMask & (uint32_t(1) << targetGroup)) != 0)
                                         impl->nativeSensorGroupColors[{event.sensorColorGroup,
                                             uint32_t(1) << targetGroup}] = event.sensorColors[targetGroup];
                                 }
                             }
                            if (event.classId == 9 && commandCb)
                                commandCb(event.message);
                            if (event.classId == 9 && clientCommandCb)
                                clientCommandCb(event.rawArguments, event.taggedArguments);
                             if (event.classId == 9 && !event.arguments.empty() &&
                                  (event.arguments[0] == "ServerMessage" ||
                                   !event.arguments[0].empty())) {
                                  const auto& args = event.arguments;
                                  const bool wrapped = args[0] == "ServerMessage";
                                  const size_t base = wrapped ? 1 : 0;
                                  if (base >= args.size()) continue;
                                  const std::string& type = args[base];
                                 const auto value = [&](size_t index) -> const std::string& {
                                     return args[index + base];
                                 };
                                 const size_t valueCount = args.size() - base;
                                 if (type == "MsgMissionStart") {
                                    impl->nativeMatchStarted = true;
                                    impl->nativeMatchEnded = false;
                                } else if (type == "MsgClientReady") {
                                    impl->nativeMatchStarted = false;
                                    impl->nativeMatchEnded = false;
                                } else if (!wrapped && type == "MissionEnd") {
                                    // GameConnection::endMission; debrief
                                    // messages can arrive during play.
                                    impl->nativeMatchEnded = true;
                                 } else if (type == "MsgSystemClock" && valueCount >= 3) {
                                     impl->nativeClockRemainingMs = (uint32_t)std::max(0, atoi(value(2).c_str()));
                                } else if (type == "MsgLoadInfo") {
                                    impl->nativeLoadInfoLines.clear();
                                 } else if ((type == "MsgLoadQuoteLine" || type == "MsgLoadObjectiveLine" ||
                                             type == "MsgLoadRulesLine") && valueCount >= 2 &&
                                            impl->nativeLoadInfoLines.size() < 128) {
                                     impl->nativeLoadInfoLines.push_back(value(1));
                                }
                                 if ((type == "MsgTeamScoreIs" || type == "MsgTeamScore") &&
                                     valueCount >= 4) {
                                      int teamId = 0;
                                      if (parseMessageIndex(value(1), teamId) && teamId > 0 && teamId < 64) {
                                         auto& team = impl->nativeTeamScores[teamId];
                                         team.teamId = teamId;
                                         team.score = atoi(value(2).c_str());
                                     }
                                 } else if (type == "MsgCTFAddTeam" && valueCount >= 5) {
                                     const int teamId = atoi(value(1).c_str());
                                     if (teamId > 0 && teamId < 64) {
                                         auto& team = impl->nativeTeamScores[teamId];
                                         team.teamId = teamId;
                                         team.name = value(2);
                                          team.flagStatus = nativeFlagStatus(value(3));
                                          team.flagCarrier = team.flagStatus == "held" && !value(3).empty() ? value(3) : "";
                                         team.score = atoi(value(4).c_str());
                                     }
                                 } else if ((type == "MsgCTFFlagTaken" ||
                                            type == "MsgCTFFlagDropped" ||
                                            type == "MsgCTFFlagReturned" ||
                                             type == "MsgCTFFlagCapped") && valueCount >= 5) {
                                      int teamId = 0;
                                      if (parseMessageIndex(value(3), teamId) && teamId > 0 && teamId < 64) {
                                         auto& team = impl->nativeTeamScores[teamId];
                                         team.teamId = teamId;
                                         team.flagStatus = type == "MsgCTFFlagTaken" ? "held" :
                                              type == "MsgCTFFlagDropped" ? "field" : "home";
                                          team.flagCarrier = team.flagStatus == "held" && value(1) != "0" ? value(1) : "";
                                     }
                                  } else if (type == "MsgPlayerScore" && valueCount >= 4) {
                                     const int clientId = atoi(value(1).c_str());
                                     if (clientId >= 0 && clientId < 1024)
                                         impl->nativePlayerScores[clientId] = atoi(value(2).c_str());
                                     if (clientId >= 0 && clientId < 1024 && valueCount >= 5) {
                                         impl->nativePlayerPings[clientId] = atoi(value(3).c_str());
                                          impl->nativePlayerPacketLoss[clientId] = atoi(value(4).c_str());
                                      }
                                 } else if (type == "MsgPlayerStats" && valueCount >= 6) {
                                      int ghostId = 0;
                                      if (parseMessageIndex(value(1), ghostId)) {
                                          const int kills = atoi(value(2).c_str());
                                          const int deaths = atoi(value(3).c_str());
                                          const int score = atoi(value(4).c_str());
                                          const int team = atoi(value(5).c_str());
                                         impl->nativePlayerKills[ghostId] = kills;
                                         impl->nativePlayerDeaths[ghostId] = deaths;
                                         auto state = impl->nativePlayerStates.find((uint16_t)ghostId);
                                         if (state != impl->nativePlayerStates.end()) {
                                             state->second.kills = kills;
                                             state->second.deaths = deaths;
                                             state->second.score = score;
                                             state->second.team = team;
                                             state->second.hasStats = true;
                                         } else {
                                             V12::PlayerGhostState pending;
                                             pending.kills = kills;
                                             pending.deaths = deaths;
                                             pending.score = score;
                                             pending.team = team;
                                             pending.hasStats = true;
                                             impl->pendingNativeStats[(uint16_t)ghostId] = pending;
                                         }
                                     }
                                 } else if (type == "MsgClientJoin" && valueCount >= 4) {
                                     const int clientId = atoi(value(2).c_str());
                                     const int targetId = atoi(value(3).c_str());
                                     if (clientId >= 0 && clientId < 1024 &&
                                         targetId >= 0 && targetId < 1024) {
                                         impl->nativeClientTargets[clientId] = targetId;
                                         impl->nativeClientNames[clientId] = value(1);
                                     }
                                 } else if (type == "MsgClientDrop" && valueCount >= 3) {
                                      const int clientId = atoi(value(2).c_str());
                                     impl->nativeClientTargets.erase(clientId);
                                     impl->nativeClientNames.erase(clientId);
                                     impl->nativeClientTeams.erase(clientId);
                                     impl->nativePlayerScores.erase(clientId);
                                     impl->nativePlayerPings.erase(clientId);
                                     impl->nativePlayerPacketLoss.erase(clientId);
                                 } else if (type == "MsgClientNameChanged" && valueCount >= 4) {
                                     const int clientId = atoi(value(3).c_str());
                                     if (clientId >= 0 && clientId < 1024)
                                         impl->nativeClientNames[clientId] = value(2);
                                 } else if (type == "MsgClientJoinTeam" && valueCount >= 4) {
                                      const int clientId = atoi(value(2).c_str());
                                      const int teamId = atoi(value(3).c_str());
                                    if (clientId >= 0 && clientId < 1024 && teamId >= 0 && teamId < 64)
                                        impl->nativeClientTeams[clientId] = teamId;
                                }
                            }
                            if (event.classId == 9 && serverMessageCb && !event.arguments.empty())
                                serverMessageCb(event.arguments);
                            if (event.classId == 22 && serverMessageCb && !event.message.empty())
                                serverMessageCb({"ChatMessage", event.message});
                            if (event.hasTargetInfo && targetCb)
                                targetCb(&event.targetInfo, event.targetInfo.targetId);
                             if ((event.hasAudio || event.hasVoiceStream) && audioCb)
                                 audioCb(event);
                             if (event.hasTargetInfo) {
                                auto& target = impl->nativeTargets[event.targetInfo.targetId];
                                target.targetId = event.targetInfo.targetId;
                                if (event.targetInfo.hasName) {
                                    target.hasName = true;
                                    target.name = event.targetInfo.name;
                                }
                                if (event.targetInfo.hasSkin) {
                                    target.hasSkin = true;
                                    target.skin = event.targetInfo.skin;
                                }
                                if (event.targetInfo.hasSkinPreference) {
                                    target.hasSkinPreference = true;
                                    target.skinPreference = event.targetInfo.skinPreference;
                                }
                                if (event.targetInfo.hasVoice) {
                                    target.hasVoice = true;
                                    target.voice = event.targetInfo.voice;
                                }
                                if (event.targetInfo.hasType) {
                                    target.hasType = true;
                                    target.type = event.targetInfo.type;
                                }
                                if (event.targetInfo.hasSensorGroup) {
                                    target.hasSensorGroup = true;
                                    target.sensorGroup = event.targetInfo.sensorGroup;
                                }
                                if (event.targetInfo.hasDataBlockId) {
                                    target.hasDataBlockId = true;
                                    target.dataBlockId = event.targetInfo.dataBlockId;
                                }
                                if (event.targetInfo.hasRenderFlags) {
                                    target.hasRenderFlags = true;
                                    target.renderFlags = event.targetInfo.renderFlags;
                                }
                                if (event.targetInfo.hasVoicePitch) {
                                    target.hasVoicePitch = true;
                                    target.voicePitch = event.targetInfo.voicePitch;
                                }
                             }
                             if (event.hasTargetTo) {
                                 if (event.targetToAssign) {
                                     impl->nativeControlGhost = event.targetToHasTarget
                                         ? event.targetToId : 0;
                                     impl->nativeControlAssigned = event.targetToHasTarget;
                                 }
                                 if (targetControlCb)
                                     targetControlCb(event.targetToHasTarget,
                                         event.targetToHasTarget ? event.targetToId : 0,
                                         event.targetToHasPosition,
                                         event.targetToPosition,
                                         event.targetToAssign);
                             }
                             if (event.hasTargetFree && targetCb)
                                 targetCb(nullptr, event.targetFreeId);
                             if (event.hasTargetFree) {
                                if (impl->nativeControlGhost == event.targetFreeId) {
                                    impl->nativeControlGhost = 0;
                                    impl->nativeControlAssigned = false;
                                    if (targetControlCb)
                                        targetControlCb(false, 0, false, {}, true);
                                }
                                 impl->nativeTargets.erase(event.targetFreeId);
                                for (auto it = impl->nativeClientTargets.begin();
                                     it != impl->nativeClientTargets.end();) {
                                    if (it->second == event.targetFreeId)
                                        it = impl->nativeClientTargets.erase(it);
                                    else
                                        ++it;
                                }
                            }
                             if (event.hasMissionCrc && event.missionCrc != impl->nativeMissionCrc) {
                                // Mission-scoped UI and target state must not cross the CRC boundary.
                                impl->nativeGhosts.clear();
                                 impl->nativePlayerStates.clear();
                                 impl->pendingNativeStats.clear();
                                impl->nativeGhostDatablocks.clear();
                                impl->nativeProjectileImpacts.clear();
                                 impl->nativeTargets.clear();
                                 impl->nativeControlGhost = 0;
                                 impl->nativeControlAssigned = false;
                                 if (targetControlCb)
                                     targetControlCb(false, 0, false, {}, true);
                                 impl->nativeDatablockShapes.clear();
                                 impl->nativeDatablocks.clear();
                                impl->nativeTeamScores.clear();
                                 impl->nativePlayerScores.clear();
                                 impl->nativePlayerKills.clear();
                                 impl->nativePlayerDeaths.clear();
                                impl->nativePlayerPings.clear();
                                impl->nativePlayerPacketLoss.clear();
                                impl->nativeClientTargets.clear();
                                impl->nativeClientTeams.clear();
                                impl->nativeClientNames.clear();
                                 impl->nativeSensorGroupColors.clear();
                                 impl->nativeTargetVisible.fill(0);
                                 impl->nativeLoadInfoLines.clear();
                                impl->nativeMatchStarted = false;
                                impl->nativeMatchEnded = false;
                                 impl->nativeClockRemainingMs = 0;
                                 impl->nativeHasCameraFov = false;
                                 impl->nativeCameraFov = 0;
                                if (missionCb) missionCb(event.missionCrc);
                                impl->nativeMissionCrc = event.missionCrc;
                            }
                              if (event.hasDatablock) {
                                 impl->nativeDatablocks[event.datablockObject] = event.datablockData;
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
                                // The server is beginning a fresh world pass.
                                // Clear the transactional tracker before its
                                // creates are decoded below.
                                 packetGhosts.clear();
                                  impl->nativeGhosting = true;
                                 impl->nativeControlGhost = 0;
                                 impl->nativeControlAssigned = false;
                                 if (targetControlCb)
                                     targetControlCb(false, 0, false, {}, true);
                                 if (targetCb) {
                                     for (const auto& [targetId, target] : impl->nativeTargets)
                                         targetCb(nullptr, targetId);
                                 }
                                 impl->nativeTargets.clear();
                                impl->nativePlayerStates.clear();
                                 impl->nativeGhostDatablocks.clear();
                                 impl->nativeProjectileImpacts.clear();
                                 impl->nativeDatablocks.clear();
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
                             if (event.hasGhostingMessage && event.ghostMessage == 2) {
                                 endGhosting = true;
                                 impl->nativeGhosting = false;
                             }
                        }
                        std::vector<V12::GhostUpdate> updates;
                        std::map<uint16_t, V12::PlayerGhostState> playerStates;
                        std::vector<std::pair<uint16_t, V12::ProjectileImpact>> projectileImpacts;
                        const bool ghostsOk = V12::readGhostUpdates(
                            payload, packetGhosts, updates,
                            [&](V12BitStream& ghost, uint16_t index, uint16_t classId, bool initial) {
                                V12::PlayerGhostState state;
                                std::vector<V12::ProjectileImpact> impacts;
                                const bool ok = V12::readGhostPayload(
                                    ghost, classId, initial, compressionPoint, &state,
                                    projectileImpactCb ? &impacts : nullptr);
                                if (ok && projectileImpactCb) {
                                    for (const auto& impact : impacts)
                                        projectileImpacts.emplace_back(index, impact);
                                }
                                 if (ok && (classId == 3 || classId == 6 || classId == 7 || classId == 9 ||
                                            classId == 13 || classId == 15 || classId == 16 || classId == 18 || classId == 19 ||
                                             classId == 10 || classId == 14 || classId == 22 || classId == 25 || classId == 27 || classId == 28 || classId == 38 ||
                                             classId == 30 || classId == 32 || classId == 36 || classId == 37 ||
                                             classId == 39 || classId == 44 || classId == 46 || classId == 51 || classId == 52))
                                     playerStates[index] = state;
                                if (ok && state.hasDatablock)
                                    impl->nativeGhostDatablocks[index] = state.datablockId;
                                return ok;
                            });
                        if (ghostsOk) {
                            impl->nativeStrings = std::move(packetStrings);
                            impl->nativeGhosts = std::move(packetGhosts);
                            if (projectileImpactCb) {
                                for (const auto& [index, impact] : projectileImpacts) {
                                    if (!impl->nativeProjectileImpacts.insert(index).second)
                                        continue;
                                    const auto* entry = impl->nativeGhosts.get(index);
                                    V12::ProjectileImpact resolved = impact;
                                    if (!resolved.hasDatablock) {
                                        auto datablock = impl->nativeGhostDatablocks.find(index);
                                        if (datablock != impl->nativeGhostDatablocks.end()) {
                                            resolved.datablockId = datablock->second;
                                            resolved.hasDatablock = true;
                                        }
                                    }
                                    projectileImpactCb(index, entry ? entry->classId : 0, resolved);
                                }
                            }
                            for (const auto& update : updates) {
                                if (update.operation == V12::GhostUpdate::Operation::Delete) {
                                    impl->nativePlayerStates.erase(update.index);
                                    impl->nativeGhostDatablocks.erase(update.index);
                                    impl->nativeProjectileImpacts.erase(update.index);
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
                                     const auto pending = impl->pendingNativeStats.find(update.index);
                                     if (pending != impl->pendingNativeStats.end()) {
                                         impl->nativePlayerStates[update.index] =
                                             V12::mergePlayerGhostState(impl->nativePlayerStates[update.index],
                                                                         pending->second);
                                         impl->pendingNativeStats.erase(pending);
                                     }
                                    if (state->second.hasDatablock)
                                        impl->nativeGhostDatablocks[update.index] = state->second.datablockId;
                                    if (ghostCb)
                                        ghostCb(update, &impl->nativePlayerStates[update.index]);
                                }
                            }
                        }
                        // Ghosting completion is a state transition, not a new
                        // protocol epoch. Clearing here discards the ghosts,
                        // mission CRC, and datablocks received in this packet.
                        (void)endGhosting;
                    }
                }
                continue;
            }
        }
    }

    // ── Timeout ──────────────────────────────────────────────────
    if ((connState == Connecting || connState == Challenging) &&
        (now - impl->connectTime) > 10.0) {
        Console::instance().printf(LogLevel::Warn, "Connection timeout");
        if (connectCb) connectCb(false);
        disconnect();
        return;
    }

    impl->flushNativeMove();

    // ── Send Connect retry (no response yet) ────────────────────
    if (connState == Connecting && (now - impl->connectTime) > 1.0) {
        impl->connectTime = now;
        impl->sendRaw(V12::buildConnectChallengeRequest(
            V12::ProtocolVersion, impl->clientConnectSequence));
    } else if (connState == Challenging && !impl->connectRequest.empty() &&
               (now - impl->connectTime) > 1.0) {
        impl->connectTime = now;
        impl->sendRaw(impl->connectRequest);
    }

    // ── Ping ─────────────────────────────────────────────────────
    if (connState >= Connected && (now - impl->lastPing) > 5.0) {
        impl->lastPing = now;
        if (impl->serverConnectSequence != 0)
            impl->sendRaw(impl->nativeProtocol.buildPacket(V12::PacketType::Ping));
    }
}

bool Connection::ingestObserverPacket(const uint8_t* data, size_t size) {
    constexpr size_t MaxQueuedObserverPackets = 256;
    if (!data || size == 0 || size > V12::MaxPacketDataSize ||
        connState < Connected || impl->injectedObserverPackets.size() >= MaxQueuedObserverPackets)
        return false;
    impl->injectedObserverPackets.emplace_back(data, data + size);
    return true;
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
    if (!isObserverSetupCommand(argv)) {
        Console::instance().printf(LogLevel::Warn,
            "Ignored observer command: %s", argv.front().c_str());
        return;
    }
    sendRemoteCommand(argv.front(), std::vector<std::string>(argv.begin() + 1, argv.end()));
}

void Connection::sendRemoteCommand(const std::string& command,
                                   const std::vector<std::string>& args) {
    if (command.empty() || connState < Connected) return;
    std::vector<std::string> wireArgs = args;
    std::vector<std::string> argv{command};
    argv.insert(argv.end(), wireArgs.begin(), wireArgs.end());
    if (!isObserverSetupCommand(argv)) {
        Console::instance().printf(LogLevel::Warn,
            "Ignored observer command: %s", command.c_str());
        return;
    }
    auto events = V12::buildRemoteCommandEvents(impl->nativeStrings, command, wireArgs);
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
    std::map<std::string, double> querySentTimes;
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
NetworkManager::~NetworkManager() {
    shutdown();
    delete impl;
}

bool NetworkManager::init() {
    Console::instance().printf(LogLevel::Info, "Network initialized");
    return true;
}

void NetworkManager::shutdown() {
    if (impl->broadcastSock >= 0) {
        close(impl->broadcastSock);
        impl->broadcastSock = -1;
    }
    impl->querying = false;
    impl->queryTargets.clear();
    impl->nativeInfoRequested.clear();
    impl->querySentTimes.clear();
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
            const std::string responseKey = NetAddress{from.sin_addr.s_addr,
                ntohs(from.sin_port)}.toString();
            const auto sent = impl->querySentTimes.find(responseKey);
            const double sentAt = sent == impl->querySentTimes.end()
                ? impl->querySentAt : sent->second;
            if (parseNativePingResponse(buf, (size_t)n, info, sentAt)) {
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
    }

    // Auto-stop query after 3 seconds
    if (impl->querying) {
        double now = Engine::instance().timer().now();
        if (!impl->queryTargets.empty() && impl->queryAttempts < 3 &&
            now - impl->queryLastSent >= 0.75) {
            const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
            for (const auto& target : impl->queryTargets) {
                sendto(impl->broadcastSock, queryPacket.data(), queryPacket.size(), 0,
                       (const sockaddr*)&target, sizeof(target));
                impl->querySentTimes[NetAddress{target.sin_addr.s_addr,
                    ntohs(target.sin_port)}.toString()] = now;
            }
            ++impl->queryAttempts;
            impl->queryLastSent = now;
        }
        if (now - impl->queryStartTime > 3.0) {
            impl->querying = false;
            impl->queryTargets.clear();
            impl->querySentTimes.clear();
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
    Console::instance().setVariable("serverQuerySource", "lan");
    Console::instance().setVariable("serverQueryDemoMode",
        Engine::instance().demoMode ? "1" : "0");
    Console::instance().printf(LogLevel::Info, "Querying LAN servers...");
    int sock = impl->ensureBroadcastSock();
    if (sock < 0) return;

    // Clear existing results
    servers.clear();
    impl->seenServers.clear();
    impl->nativeInfoRequested.clear();
    impl->querySentTimes.clear();
    impl->queryTargets.clear();
    impl->queryAttempts = 0;

    // Native V12 servers answer GamePingRequest on both LAN and game ports.
    const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(T2Protocol::LAN_QUERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    // Broadcast on both the LAN query and standard game ports.  Record one
    // timestamp for each destination so a batch reports per-server latency.
    impl->querying = true;
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;
    sendto(sock, queryPacket.data(), queryPacket.size(), 0,
           (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    broadcastAddr.sin_port = htons(T2Protocol::DEFAULT_PORT);
    sendto(sock, queryPacket.data(), queryPacket.size(), 0,
           (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
    impl->querySentTimes[NetAddress{INADDR_BROADCAST, T2Protocol::LAN_QUERY_PORT}.toString()] =
        impl->querySentAt;
    impl->querySentTimes[NetAddress{INADDR_BROADCAST, T2Protocol::DEFAULT_PORT}.toString()] =
        impl->querySentAt;
    impl->queryLastSent = impl->querySentAt;

    if (serverListCb) serverListCb();
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    if (!masterUrl || !*masterUrl) {
        Console::instance().printf(LogLevel::Warn,
            "Master query skipped: no master URL configured; falling back to LAN discovery");
        queryLanServers();
        return;
    }
    const std::string configured = masterUrl;
    std::string url = configured;
    if (url.find("http://") != 0 && url.find("https://") != 0)
        url = "http://" + url;
    if (url.find("/list", url.find("://") + 3) == std::string::npos)
        url += "/list";

    Console::instance().printf(LogLevel::Info, "Querying master: %s", url.c_str());
    Console::instance().setVariable("serverQuerySource", "master");
    Console::instance().setVariable("serverQueryDemoMode",
        Engine::instance().demoMode ? "1" : "0");
    impl->querying = false;
    impl->queryTargets.clear();
    impl->queryAttempts = 0;
    impl->querySentTimes.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        Console::instance().printf(LogLevel::Error, "Master query: libcurl unavailable");
        Console::instance().printf(LogLevel::Warn, "Falling back to LAN discovery");
        queryLanServers();
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
        Console::instance().printf(LogLevel::Warn, "Falling back to LAN discovery");
        queryLanServers();
        return;
    }

    servers.clear();
    impl->seenServers.clear();
    impl->nativeInfoRequested.clear();
    impl->querySentTimes.clear();
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;
    int queried = 0;
    if (impl->ensureBroadcastSock() < 0) {
        Console::instance().printf(LogLevel::Error, "Master query: unable to create UDP query socket");
        return;
    }
    std::istringstream lines(body);
    std::string line;
    const auto queryPacket = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0);
    std::set<std::string> dispatchedAddresses;
    while (std::getline(lines, line) && queried < 512) {
        std::string host;
        uint16_t port = 0;
        if (!TorchMaster::parseAddressLine(line, host, port)) continue;
        const std::string addressKey = host + ":" + std::to_string(port);
        if (!dispatchedAddresses.insert(addressKey).second) continue;
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
        impl->querySentTimes[NetAddress{target->sin_addr.s_addr, port}.toString()] =
            impl->querySentAt;
        freeaddrinfo(resultAddr);
        queried++;
    }
    impl->querying = queried > 0;
    impl->queryAttempts = queried > 0 ? 1 : 0;
    if (serverListCb) serverListCb();
    Console::instance().printf(LogLevel::Info, "Master query dispatched %d server probes", queried);
}

void NetworkManager::stopServerQuery() {
    impl->querying = false;
    impl->nativeInfoRequested.clear();
    impl->queryTargets.clear();
    impl->querySentTimes.clear();
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
    impl->querySentTimes.clear();
    impl->querying = true;
    impl->queryStartTime = Engine::instance().timer().now();
    impl->querySentAt = impl->queryStartTime;
    impl->queryTargets.push_back(*reinterpret_cast<sockaddr_in*>(resolved->ai_addr));
    sendto(impl->broadcastSock, packet.data(), packet.size(), 0,
           resolved->ai_addr, sizeof(sockaddr_in));
    auto* target = reinterpret_cast<sockaddr_in*>(resolved->ai_addr);
    impl->querySentTimes[NetAddress{target->sin_addr.s_addr, port}.toString()] =
        impl->querySentAt;
    freeaddrinfo(resolved);
    impl->queryAttempts = 1;
    impl->queryLastSent = impl->querySentAt;
    if (serverListCb) serverListCb();
}

bool NetworkManager::isServerQueryActive() const {
    return impl->querying;
}
