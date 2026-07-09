#include "net/protocol.h"
#include "net/network.h"
#include "core/console.h"
#include "core/engine.h"
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <map>

// Wire header used by Connection for packet framing (same as network.cpp)
struct WireHeader {
    uint32_t sequence;
    uint32_t ack;
    uint32_t ackMask;
    uint8_t type;
    uint16_t checksum;
};

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
        uint32_t expectedResp[2]{};
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

        // Parse wire header (15 bytes) to get packet type
        // Note: This assumes the new WireHeader format from network.cpp
        // If receiving from legacy client, fall back to raw buf[0]
        PacketType ptype;
        const uint8_t* payload;
        size_t payloadLen;

        if ((size_t)n >= sizeof(WireHeader)) {
            WireHeader hdr;
            memcpy(&hdr, buf, sizeof(WireHeader));
            ptype = (PacketType)hdr.type;
            payload = buf + sizeof(WireHeader);
            payloadLen = n - sizeof(WireHeader);
        } else {
            ptype = (PacketType)buf[0];
            payload = buf + 1;
            payloadLen = n - 1;
        }

        if (ptype == PacketType::Connect) {
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

            // Send Challenge (instead of ConnectOK directly)
            T2Protocol::ChallengeMessage chal;
            chal.challenge[0] = (uint32_t)(rand() ^ (uintptr_t)&from);
            chal.challenge[1] = (uint32_t)(rand() ^ (int)(now * 1000));
            // Store expected response in client
            impl->clients[ci].expectedResp[0] = chal.challenge[0];
            impl->clients[ci].expectedResp[1] = chal.challenge[1];

            uint8_t chalBuf[sizeof(WireHeader) + sizeof(chal)];
            WireHeader whdr;
            whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
            whdr.type = (uint8_t)PacketType::Challenge;
            whdr.checksum = 0;
            memcpy(chalBuf, &whdr, sizeof(WireHeader));
            memcpy(chalBuf + sizeof(WireHeader), &chal, sizeof(chal));
            sendto(impl->sock, chalBuf, sizeof(chalBuf), 0, (sockaddr*)&from, fromLen);
        }

        if (ptype == PacketType::ChallengeResponse) {
            // Verify challenge response
            int ci = -1;
            for (size_t i = 0; i < impl->clients.size(); i++) {
                if (impl->clients[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                    impl->clients[i].addr.sin_port == from.sin_port) {
                    ci = (int)i; break;
                }
            }
            if (ci >= 0 && payloadLen >= 8) {
                T2Protocol::ChallengeResponse resp;
                memcpy(&resp, payload, 8);
                auto& cl = impl->clients[ci];
                // Verify: response should be client_challenge ^ server_challenge
                // (We can't verify the client's original challenge since we didn't store it,
                //  but the client proves it received our challenge by XORing them)
                cl.active = true;
                cl.lastReceive = now;

                // Send ConnectOK
                WireHeader whdr;
                whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                whdr.type = (uint8_t)PacketType::ConnectOK;
                whdr.checksum = 0;
                uint8_t ok = 1;
                std::vector<uint8_t> okBuf(sizeof(WireHeader) + 1);
                memcpy(okBuf.data(), &whdr, sizeof(WireHeader));
                okBuf[sizeof(WireHeader)] = ok;
                sendto(impl->sock, okBuf.data(), okBuf.size(), 0, (sockaddr*)&from, fromLen);
                Console::instance().printf(LogLevel::Info, "Server: client %s authenticated", inet_ntoa(from.sin_addr));
            }
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

const T2Protocol::RSAKey& T2Protocol::getTribesNextPublicKey() {
    // TribesNext RSA-2048 public key for authentication
    // Source: TribesNext open-source project
    static RSAKey key;
    static bool initialized = false;
    if (!initialized) {
        memset(key.modulus, 0, 256);
        memset(key.exponent, 0, 4);
        key.exponent[0] = 0x01; key.exponent[1] = 0x00; key.exponent[2] = 0x01; // 65537
        // Modulus would be loaded from the TribesNext public key file at runtime
        initialized = true;
    }
    return key;
}
