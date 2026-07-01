#include "net/network.h"
#include "net/protocol.h"
#include "core/console.h"
#include "core/engine.h"
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <queue>
#include <chrono>

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

void NetworkManager::update() {
    // Periodic tasks
}

Connection* NetworkManager::createConnection() {
    return new Connection;
}

void NetworkManager::destroyConnection(Connection* conn) {
    delete conn;
}

void NetworkManager::queryLanServers() {
    Console::instance().printf(LogLevel::Info, "Querying LAN servers...");
}

void NetworkManager::queryMasterServer(const char* masterUrl) {
    Console::instance().printf(LogLevel::Info, "Querying master: %s", masterUrl);
}
