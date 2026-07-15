#include "net/network.h"
#include "net/protocol.h"
#include "core/console.h"
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
Connection::~Connection() { disconnect(); delete impl; }

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

    // Generate random challenge
    srand((unsigned int)(time(nullptr) + (uintptr_t)this));
    impl->challenge[0] = (uint32_t)rand();
    impl->challenge[1] = (uint32_t)rand();

    // Actually send the Connect packet with protocol info
    T2Protocol::ConnectMessage connMsg;
    connMsg.protocol = T2Protocol::PROTOCOL_VERSION;
    connMsg.challenge = impl->challenge[0];
    memset(connMsg.version, 0, sizeof(connMsg.version));
    snprintf(connMsg.version, sizeof(connMsg.version), "torch 0.1");
    memset(connMsg.gameType, 0, sizeof(connMsg.gameType));
    snprintf(connMsg.gameType, sizeof(connMsg.gameType), "TRIBES2");

    uint8_t payload[sizeof(T2Protocol::ConnectMessage)];
    memcpy(payload, &connMsg, sizeof(connMsg));
    impl->sendFramed(PacketType::Connect, payload, sizeof(payload), false);

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host, port);
    return true;
}

void Connection::disconnect() {
    if (impl->sock >= 0) {
        sendPacket(PacketType::Disconnect, nullptr, 0);
        close(impl->sock);
        impl->sock = -1;
    }
    connState = Disconnected;
    impl->sentPackets.clear();
}

void Connection::update() {
    if (impl->sock < 0) return;

    double now = Engine::instance().timer().now();

    // ── Receive packets ──────────────────────────────────────────
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;
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

    // ── Send Connect retry (no response yet) ────────────────────
    if (connState == Connecting && (now - impl->connectTime) > 1.0) {
        impl->connectTime = now;
        T2Protocol::ConnectMessage connMsg;
        connMsg.protocol = T2Protocol::PROTOCOL_VERSION;
        connMsg.challenge = impl->challenge[0];
        memset(connMsg.version, 0, sizeof(connMsg.version));
        snprintf(connMsg.version, sizeof(connMsg.version), "torch 0.1");
        memset(connMsg.gameType, 0, sizeof(connMsg.gameType));
        snprintf(connMsg.gameType, sizeof(connMsg.gameType), "TRIBES2");
        uint8_t payload[sizeof(T2Protocol::ConnectMessage)];
        memcpy(payload, &connMsg, sizeof(connMsg));
        impl->sendFramed(PacketType::Connect, payload, sizeof(payload), false);
    }

    // ── Ping ─────────────────────────────────────────────────────
    if (connState >= Connected && (now - impl->lastPing) > 5.0) {
        impl->lastPing = now;
        sendPacket(PacketType::Ping, nullptr, 0);
    }
}

void Connection::sendPacket(PacketType type, const uint8_t* data, size_t size) {
    impl->sendFramed(type, data, size, false);
}

void Connection::sendGamePacket(const uint8_t* data, size_t size, bool reliable) {
    // Reliable: track for retransmission (handled by sendFramed with reliable=true)
    impl->sendFramed(PacketType::GameData, data, size, reliable);
}

void Connection::sendCommandPacket(const char* command) {
    if (!command || connState < Connected) return;
    // Format: [GDT_Command][len:2][command_string]
    size_t len = strlen(command);
    if (len > 1023) len = 1023;
    uint8_t buf[1026];
    buf[0] = T2Protocol::GDT_Command;
    buf[1] = (uint8_t)(len & 0xFF);
    buf[2] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(buf + 3, command, len);
    sendGamePacket(buf, 3 + len, true);
}

// ── NetworkManager ────────────────────────────────────────────────

struct NetworkManager::Impl {
    int broadcastSock = -1;
    bool querying = false;
    double queryStartTime = 0;
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

void NetworkManager::update() {
    // Receive query responses
    if (impl->broadcastSock < 0) return;

    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->broadcastSock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;

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

    // Send broadcast query on LAN query port (28002) and standard game port (28000)
    uint8_t queryPacket[] = { (uint8_t)PacketType::QueryServers, 'T','2','L','A','N','Q' };
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(T2Protocol::LAN_QUERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, queryPacket, sizeof(queryPacket), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    // Also broadcast on standard T2 port
    broadcastAddr.sin_port = htons(T2Protocol::DEFAULT_PORT);
    sendto(sock, queryPacket, sizeof(queryPacket), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    impl->querying = true;
    impl->queryStartTime = Engine::instance().timer().now();

    if (serverListCb) serverListCb();
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    Console::instance().printf(LogLevel::Info, "Querying master: %s", masterUrl);
}
