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

struct Connection::Impl {
    int sock = -1;
    sockaddr_in addr{};
    bool connected = false;

    // Sequence numbers
    uint8_t sendSeq = 0;
    uint8_t recvSeq = 0;

    // Reliable send queue
    struct ReliablePacket {
        std::vector<uint8_t> data;
        uint32_t seq;
        double sendTime;
        int retries = 0;
    };
    std::queue<ReliablePacket> reliableQueue;

    // Connection timers
    double connectTime = 0;
    double lastPing = 0;
    double lastReceive = 0;
    uint32_t challenge[2]{};
};

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

    // Send connect packet
    connState = Connecting;
    impl->connectTime = Engine::instance().timer().now();

    // Generate random challenge
    srand(time(nullptr));
    impl->challenge[0] = rand();
    impl->challenge[1] = rand();

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host, port);
    return true;
}

void Connection::disconnect() {
    if (impl->sock >= 0) {
        close(impl->sock);
        impl->sock = -1;
    }
    connState = Disconnected;
}

void Connection::update() {
    if (impl->sock < 0) return;

    // Receive packets
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;

        impl->lastReceive = Engine::instance().timer().now();

        if (packetCb) {
            PacketType type = (PacketType)buf[0];
            packetCb(type, buf + 1, n - 1);
        }
    }

    // Handle timeouts
    double now = Engine::instance().timer().now();
    if (connState == Connecting && (now - impl->connectTime) > 10.0) {
        Console::instance().printf(LogLevel::Warn, "Connection timeout");
        if (connectCb) connectCb(false);
        disconnect();
    }

    // Ping
    if (connState >= Connected && (now - impl->lastPing) > 5.0) {
        impl->lastPing = now;
        sendPacket(PacketType::Ping, nullptr, 0);
    }
}

void Connection::sendPacket(PacketType type, const uint8_t* data, size_t size) {
    if (impl->sock < 0) return;

    std::vector<uint8_t> packet;
    packet.push_back((uint8_t)type);
    if (data) packet.insert(packet.end(), data, data + size);

    sendto(impl->sock, packet.data(), packet.size(), 0,
           (sockaddr*)&impl->addr, sizeof(impl->addr));
}

void Connection::sendGamePacket(const uint8_t* data, size_t size, bool reliable) {
    if (reliable) {
        impl->reliableQueue.push({
            std::vector<uint8_t>(data, data + size),
            impl->sendSeq++,
            Engine::instance().timer().now(),
            0
        });
    }
    sendPacket(PacketType::GameData, data, size);
}

// NetworkManager
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
    broadcastAddr.sin_port = htons(28002);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, queryPacket, sizeof(queryPacket), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    // Also broadcast on standard T2 port
    broadcastAddr.sin_port = htons(28000);
    sendto(sock, queryPacket, sizeof(queryPacket), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    impl->querying = true;
    impl->queryStartTime = Engine::instance().timer().now();

    if (serverListCb) serverListCb();
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    Console::instance().printf(LogLevel::Info, "Querying master: %s", masterUrl);
}
