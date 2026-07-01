#include "net/protocol.h"
#include "net/network.h"
#include "core/console.h"
#include "core/engine.h"
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

uint16_t T2Protocol::calculateChecksum(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++)
        sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

bool T2Protocol::verifyChecksum(const uint8_t* data, size_t size) {
    if (size < 2) return false;
    uint16_t stored = *(uint16_t*)(data + size - 2);
    return calculateChecksum(data, size - 2) == stored;
}

// ─── Move Message ─────────────────────────────────────────────────

bool T2Protocol::encodeMove(uint8_t* buf, size_t bufSize, const MoveMessage& msg) {
    if (bufSize < 33) return false;
    uint32_t pos = 0;
    buf[pos++] = GDT_Move;
    memcpy(buf + pos, &msg.posX, 4); pos += 4;
    memcpy(buf + pos, &msg.posY, 4); pos += 4;
    memcpy(buf + pos, &msg.posZ, 4); pos += 4;
    memcpy(buf + pos, &msg.rotZ, 4); pos += 4;
    memcpy(buf + pos, &msg.rotX, 4); pos += 4;
    buf[pos++] = msg.flags;
    memcpy(buf + pos, &msg.lookX, 4); pos += 4;
    memcpy(buf + pos, &msg.lookY, 4); pos += 4;
    return true;
}

bool T2Protocol::decodeMove(const uint8_t* data, size_t size, MoveMessage& msg) {
    if (size < 33 || data[0] != GDT_Move) return false;
    uint32_t pos = 1;
    memcpy(&msg.posX, data + pos, 4); pos += 4;
    memcpy(&msg.posY, data + pos, 4); pos += 4;
    memcpy(&msg.posZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotX, data + pos, 4); pos += 4;
    msg.flags = data[pos++];
    memcpy(&msg.lookX, data + pos, 4); pos += 4;
    memcpy(&msg.lookY, data + pos, 4); pos += 4;
    return true;
}

// ─── Update Message ───────────────────────────────────────────────

bool T2Protocol::encodeUpdate(uint8_t* buf, size_t bufSize, const UpdateMessage& msg) {
    if (bufSize < 45) return false;
    uint32_t pos = 0;
    buf[pos++] = GDT_Update;
    memcpy(buf + pos, &msg.posX, 4); pos += 4;
    memcpy(buf + pos, &msg.posY, 4); pos += 4;
    memcpy(buf + pos, &msg.posZ, 4); pos += 4;
    memcpy(buf + pos, &msg.rotZ, 4); pos += 4;
    memcpy(buf + pos, &msg.rotX, 4); pos += 4;
    memcpy(buf + pos, &msg.velX, 4); pos += 4;
    memcpy(buf + pos, &msg.velY, 4); pos += 4;
    memcpy(buf + pos, &msg.velZ, 4); pos += 4;
    buf[pos++] = (uint8_t)(msg.health * 2);
    buf[pos++] = (uint8_t)(msg.energy * 2);
    buf[pos++] = msg.flags;
    return true;
}

bool T2Protocol::decodeUpdate(const uint8_t* data, size_t size, UpdateMessage& msg) {
    if (size < 45 || data[0] != GDT_Update) return false;
    uint32_t pos = 1;
    memcpy(&msg.posX, data + pos, 4); pos += 4;
    memcpy(&msg.posY, data + pos, 4); pos += 4;
    memcpy(&msg.posZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotX, data + pos, 4); pos += 4;
    memcpy(&msg.velX, data + pos, 4); pos += 4;
    memcpy(&msg.velY, data + pos, 4); pos += 4;
    memcpy(&msg.velZ, data + pos, 4); pos += 4;
    msg.health = data[pos++] / 2.0f;
    msg.energy = data[pos++] / 2.0f;
    msg.flags = data[pos++];
    return true;
}

// ─── Server ───────────────────────────────────────────────────────

struct GameServer::Impl {
    int sock = -1;
    bool running = false;

    struct Client {
        sockaddr_in addr;
        socklen_t addrLen;
        double lastReceive;
        uint16_t moveSeq;
        T2Protocol::MoveMessage lastMove;
        float posX, posY, posZ;
        float rotZ, rotX;
        float health, energy;
        bool active;
    };
    std::vector<Client> clients;
};

GameServer::GameServer() : impl(new Impl) {}
GameServer::~GameServer() { stop(); delete impl; }

bool GameServer::start(uint16_t port) {
    impl->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->sock < 0) {
        Console::instance().printf(LogLevel::Error, "Server: cannot create socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(impl->sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Console::instance().printf(LogLevel::Error, "Server: bind failed on port %d", port);
        close(impl->sock);
        impl->sock = -1;
        return false;
    }

    int flags = fcntl(impl->sock, F_GETFL, 0);
    fcntl(impl->sock, F_SETFL, flags | O_NONBLOCK);

    impl->running = true;
    Console::instance().printf(LogLevel::Info, "Server started on port %d", port);
    return true;
}

void GameServer::stop() {
    if (impl->sock >= 0) {
        close(impl->sock);
        impl->sock = -1;
    }
    impl->running = false;
    impl->clients.clear();
}

void GameServer::update() {
    if (!impl->running || impl->sock < 0) return;

    uint8_t buf[1024];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;

        double now = Engine::instance().timer().now();
        PacketType ptype = (PacketType)buf[0];

        if (ptype == PacketType::Connect) {
            // Send connect OK with challenge
            Console::instance().printf(LogLevel::Info, "Server: connect from %s",
                inet_ntoa(from.sin_addr));

            // Find or create client
            int ci = -1;
            for (size_t i = 0; i < impl->clients.size(); i++) {
                if (impl->clients[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                    impl->clients[i].addr.sin_port == from.sin_port) {
                    ci = (int)i; break;
                }
            }
            if (ci < 0) {
                Impl::Client c;
                c.addr = from;
                c.addrLen = fromLen;
                c.lastReceive = now;
                c.moveSeq = 0;
                c.posX = 0; c.posY = 5; c.posZ = 0;
                c.rotZ = 0; c.rotX = 0;
                c.health = 100; c.energy = 100;
                c.active = true;
                impl->clients.push_back(c);
                ci = (int)impl->clients.size() - 1;
            }

            // Send ConnectOK
            uint8_t okBuf[2] = { (uint8_t)PacketType::ConnectOK, 0 };
            sendto(impl->sock, okBuf, 2, 0, (sockaddr*)&from, fromLen);
        }

        if (ptype == PacketType::GameData) {
            int ci = -1;
            for (size_t i = 0; i < impl->clients.size(); i++) {
                if (impl->clients[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                    impl->clients[i].addr.sin_port == from.sin_port) {
                    ci = (int)i; break;
                }
            }
            if (ci < 0) continue;

            auto& client = impl->clients[ci];
            client.lastReceive = now;

            T2Protocol::MoveMessage move;
            if (T2Protocol::decodeMove(buf + 1, n - 1, move)) {
                client.lastMove = move;
                client.posX = move.posX;
                client.posY = move.posY;
                client.posZ = move.posZ;
                client.rotZ = move.rotZ;
                client.rotX = move.rotX;

                // Send update back
                T2Protocol::UpdateMessage update;
                update.posX = client.posX;
                update.posY = client.posY;
                update.posZ = client.posZ;
                update.rotZ = client.rotZ;
                update.rotX = client.rotX;
                update.velX = 0; update.velY = 0; update.velZ = 0;
                update.health = client.health;
                update.energy = client.energy;
                update.flags = 0;

                uint8_t upBuf[64];
                upBuf[0] = (uint8_t)PacketType::GameData;
                T2Protocol::encodeUpdate(upBuf + 1, sizeof(upBuf) - 1, update);
                size_t upLen = 46;
                sendto(impl->sock, upBuf, upLen, 0, (sockaddr*)&from, fromLen);
            }
        }

        if (ptype == PacketType::Ping) {
            uint8_t pong = (uint8_t)PacketType::Pong;
            sendto(impl->sock, &pong, 1, 0, (sockaddr*)&from, fromLen);
        }
    }
}
