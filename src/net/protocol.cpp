#include "net/protocol.h"
#include "net/network.h"
#include "core/console.h"
#include "core/engine.h"
#include "game/mission_parser.h"
#include "game/demo.h"
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sys/time.h>
#include <chrono>

// ─── Performance Stats ────────────────────────────────────────────

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
    uint16_t stored = 0;
    memcpy(&stored, data + size - 2, 2);
    return calculateChecksum(data, size - 2) == stored;
}

// ─── Move Message ─────────────────────────────────────────────────

size_t T2Protocol::encodeMove(uint8_t* buf, size_t bufSize, const MoveMessage& msg) {
    if (bufSize < 34) return 0;
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
    memcpy(buf + pos, &msg.seq, 4); pos += 4;
    return pos;
}

bool T2Protocol::decodeMove(const uint8_t* data, size_t size, MoveMessage& msg) {
    if (size < 34 || data[0] != GDT_Move) return false;
    uint32_t pos = 1;
    memcpy(&msg.posX, data + pos, 4); pos += 4;
    memcpy(&msg.posY, data + pos, 4); pos += 4;
    memcpy(&msg.posZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotZ, data + pos, 4); pos += 4;
    memcpy(&msg.rotX, data + pos, 4); pos += 4;
    msg.flags = data[pos++];
    memcpy(&msg.lookX, data + pos, 4); pos += 4;
    memcpy(&msg.lookY, data + pos, 4); pos += 4;
    memcpy(&msg.seq, data + pos, 4); pos += 4;
    return true;
}

// ─── Update Message ───────────────────────────────────────────────

bool T2Protocol::encodeUpdate(uint8_t* buf, size_t bufSize, const UpdateMessage& msg) {
    if (bufSize < 40) return false;
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
    float hc = msg.health; if (hc < 0) hc = 0; else if (hc > 100) hc = 100;
    float ec = msg.energy; if (ec < 0) ec = 0; else if (ec > 100) ec = 100;
    buf[pos++] = (uint8_t)(hc * 2);
    buf[pos++] = (uint8_t)(ec * 2);
    buf[pos++] = msg.flags;
    memcpy(buf + pos, &msg.lastMoveSeq, 4); pos += 4;
    return true;
}

bool T2Protocol::decodeUpdate(const uint8_t* data, size_t size, UpdateMessage& msg) {
    if (size < 40 || data[0] != GDT_Update) return false;
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
    memcpy(&msg.lastMoveSeq, data + pos, 4); pos += 4;
    return true;
}

// ─── Datablock Messages ───────────────────────────────────────────

size_t T2Protocol::encodeDatablock(uint8_t* buf, size_t bufSize,
                                    const DatablockHeader& hdr,
                                    const uint8_t* payload, size_t payloadLen) {
    size_t needed = 1 + 4*4 + payloadLen;
    if (bufSize < needed) return 0;
    uint32_t pos = 0;
    buf[pos++] = GDT_Datablock;
    memcpy(buf + pos, &hdr.classId,  4); pos += 4;
    memcpy(buf + pos, &hdr.objectId, 4); pos += 4;
    memcpy(buf + pos, &hdr.index,    4); pos += 4;
    memcpy(buf + pos, &hdr.total,    4); pos += 4;
    if (payload && payloadLen > 0) {
        memcpy(buf + pos, payload, payloadLen); pos += payloadLen;
    }
    return pos;
}

// Codec functions moved to protocol.h as inline definitions so they can be
// fuzzed (and reused) without linking GameServer/Engine.

// ─── Helpers ──────────────────────────────────────────────────────
// Map className from mission to classId using the NetObjectClassNames table
static int32_t classNameToClassId(const std::string& name) {
    for (int i = 0; i < T2Demo::NetObjectClassCount; i++) {
        if (name == T2Demo::NetObjectClassNames[i])
            return i;
    }
    return -1;
}

// ─── Server ───────────────────────────────────────────────────────

struct GameServer::Impl {
    int sock = -1;
    bool running = false;

    // Server-side ghost
    struct ServerGhost {
        uint32_t index{};
        int32_t classId{};
        float posX{}, posY{}, posZ{};
        float rotX{}, rotZ{};
        float health{100}, energy{100};
        bool active{true};
        // Item pickup fields
        int itemType = -1;
        double respawnTime = 0;
        // Vehicle fields
        int passengerCi = -1; // client index driving this vehicle, -1 if none
        float vehSpeed{};     // current forward speed
    };

    struct Client {
        sockaddr_in addr;
        socklen_t addrLen;
        double lastReceive;
        uint16_t moveSeq;
        T2Protocol::MoveMessage lastMove;
        float posX, posY, posZ;
        float velX, velY, velZ;
        float rotZ, rotX;
        float health, energy;
        int kills{}, deaths{};
        float playTime = 0;
        int teamId = 0;
        int flagCarried = 0;
        double lastFireTime = 0;            // per-client projectile spawn cadence
        double acLastFire = 0;              // per-client anti-cheat fire timestamp
        double respawnAt = 0;               // >0 while waiting to respawn after death
        std::map<uint32_t, double> ghostFirstSent; // ghost idx -> time first sent as Create
        bool active;
        bool isBot = false;
        std::string playerName = "Player";
        // Position history for lag compensation (timestamped)
        struct PosSample { double time; float x, y, z; };
        std::vector<PosSample> posHistory;
        static constexpr int maxPosHistory = 64;
        uint32_t expectedResp[2]{};
        // Ghost tracking
        std::set<uint32_t> knownGhosts;
        uint32_t playerGhostIndex = 0; // index of this client's player ghost
        bool datablocksSent = false;
    };
    std::vector<Client> clients;

    // Ghost registry
    std::vector<ServerGhost> serverGhosts;
    uint32_t nextGhostIndex = 1;

    // Admin
    std::vector<uint32_t> bannedIPs; // IPs in network byte order

    // Recording
    std::ofstream recFile;
    double recStartTime = 0;

    // NAT relay
    bool natRelayEnabled = false;

    // Query socket for server info API
    int querySock = -1;

    // Server-side projectiles (linked to ghost indices)
    struct ServerProjectile {
        uint32_t ghostIndex{};
        float posX{}, posY{}, posZ{};
        float velX{}, velY{}, velZ{};
        float lifetime{5.0f};
        float damage{30.0f};
        float splashRadius{3.0f};
        int32_t ownerCi{-1}; // which client fired it
        bool active{true};
    };
    // Per-weapon damage/splash profile (0 = default disc; 1 = sniper, etc.)
    struct WeaponProfile { float damage; float splash; };
    static WeaponProfile weaponProfile(int id) {
        switch (id) {
            case 1: return {80.0f, 1.0f};   // sniper-style: high damage, tiny splash
            default: return {30.0f, 3.0f};  // disc: moderate damage, splash radius
        }
    }
    std::vector<ServerProjectile> projectiles;

    // Spawn points loaded from mission
    struct SpawnPoint { float x, y, z; };
    std::vector<SpawnPoint> spawnPoints;

    // Bot AI state per bot
    struct BotState {
        float thinkTimer = 0;
        float strafeTimer = 0;
        int strafeDir = 0;
    };
    std::map<uint32_t, BotState> botStates;     // keyed by player ghost index
    std::map<uint32_t, double> botLastFire;      // keyed by player ghost index
    std::map<uint32_t, bool> vehUsePrev;         // keyed by player ghost index

    // Game mode
    int gameMode = 0; // 0=Deathmatch, 1=Team Deathmatch
    int scoreLimit = 25;
    int teamScore[3] = {}; // index 0=unused, 1=red, 2=blue

    // Known datablock classes (simple list for transmission)
    struct DatablockInfo {
        uint32_t classId;
        uint32_t objectId;
        uint32_t index;
        uint32_t total;
        std::vector<uint8_t> payload; // class-specific data
    };
    std::vector<DatablockInfo> datablocks;

    int findClient(const sockaddr_in& from) const {
        for (size_t i = 0; i < clients.size(); i++)
            if (clients[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                clients[i].addr.sin_port == from.sin_port)
                return (int)i;
        return -1;
    }

    // Helper: send raw buffer to a client
    void sendTo(const sockaddr_in& addr, const uint8_t* data, size_t len) {
        sendto(sock, data, len, 0, (sockaddr*)&addr, sizeof(addr));
    }

    // Helper: send datablock headers to a client
    void sendDatablocksTo(int ci) {
        if (ci < 0 || ci >= (int)clients.size()) return;
        auto& cl = clients[ci];
        if (cl.datablocksSent) return;
        cl.datablocksSent = true;

        WireHeader whdr{};
        whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
        whdr.type = (uint8_t)PacketType::GameData;
        whdr.checksum = 0;

        for (auto& db : datablocks) {
            T2Protocol::DatablockHeader hdr{db.classId, db.objectId, db.index, db.total};
            uint8_t dbBuf[512];
            size_t dbLen = T2Protocol::encodeDatablock(dbBuf, sizeof(dbBuf), hdr,
                db.payload.empty() ? nullptr : db.payload.data(), db.payload.size());
            if (dbLen == 0) continue;
            std::vector<uint8_t> pkt(sizeof(WireHeader) + dbLen);
            memcpy(pkt.data(), &whdr, sizeof(WireHeader));
            memcpy(pkt.data() + sizeof(WireHeader), dbBuf, dbLen);
            sendTo(cl.addr, pkt.data(), pkt.size());
            Console::instance().printf(LogLevel::Debug, "Server: sent datablock class=%u obj=%u idx=%u tot=%u",
                (unsigned)db.classId, (unsigned)db.objectId, (unsigned)db.index, (unsigned)db.total);
        }
    }

    // Helper: broadcast ghost delete to all clients except the originator
    void broadcastGhostDelete(uint32_t ghostIndex, int excludeCi) {
        WireHeader whdr{};
        whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
        whdr.type = (uint8_t)PacketType::GameData;
        whdr.checksum = 0;

        T2Protocol::GhostMessage gm;
        gm.index = ghostIndex;
        gm.type = T2Protocol::Ghost_Delete;
        uint8_t ghBuf[16];
        size_t ghLen = T2Protocol::encodeGhostHeader(ghBuf, sizeof(ghBuf), gm);
        if (ghLen == 0) return;

        std::vector<uint8_t> pkt(sizeof(WireHeader) + ghLen);
        memcpy(pkt.data(), &whdr, sizeof(WireHeader));
        memcpy(pkt.data() + sizeof(WireHeader), ghBuf, ghLen);

        for (size_t i = 0; i < clients.size(); i++) {
            if ((int)i == excludeCi) continue;
            if (!clients[i].active || clients[i].isBot) continue;
            clients[i].knownGhosts.erase(ghostIndex);
            clients[i].ghostFirstSent.erase(ghostIndex);
            sendTo(clients[i].addr, pkt.data(), pkt.size());
        }
    }

    // Helper: send ghost creates/updates for all server ghosts to a client
    void sendGhostsTo(int ci) {
        if (ci < 0 || ci >= (int)clients.size()) return;
        auto& cl = clients[ci];
        static const double GHOST_CREATE_WINDOW = 1.0; // seconds to retransmit Creates
        double nowGhost = Engine::instance().timer().now();
        const float scopeRange = 200.0f;
        const float scopeRangeSq = scopeRange * scopeRange;

        // Collect currently visible ghosts
        std::set<uint32_t> visible;
        for (auto& sg : serverGhosts) {
            if (!sg.active) continue;
            // Player ghosts are always visible
            if (sg.classId == 31) { visible.insert(sg.index); continue; }
            // Distance check
            float dx = sg.posX - cl.posX;
            float dz = sg.posZ - cl.posZ;
            if (dx*dx + dz*dz <= scopeRangeSq)
                visible.insert(sg.index);
        }

        // Send deletes for ghosts that are no longer visible
        for (auto it = cl.knownGhosts.begin(); it != cl.knownGhosts.end(); ) {
            if (visible.find(*it) == visible.end()) {
                // Out of range → send delete to this client only
                WireHeader whdr{};
                whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                whdr.type = (uint8_t)PacketType::GameData;
                whdr.checksum = 0;
                T2Protocol::GhostMessage gm;
                gm.index = *it;
                gm.type = T2Protocol::Ghost_Delete;
                uint8_t ghBuf[16];
                size_t ghLen = T2Protocol::encodeGhostHeader(ghBuf, sizeof(ghBuf), gm);
                if (ghLen > 0) {
                    std::vector<uint8_t> pkt(sizeof(WireHeader) + ghLen);
                    memcpy(pkt.data(), &whdr, sizeof(WireHeader));
                    memcpy(pkt.data() + sizeof(WireHeader), ghBuf, ghLen);
                    sendTo(cl.addr, pkt.data(), pkt.size());
                }
                cl.ghostFirstSent.erase(*it);
                cl.knownGhosts.erase(it++);
            } else {
                ++it;
            }
        }

        // Batch all ghost creates/updates into a single packet
        WireHeader whdr{};
        whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
        whdr.type = (uint8_t)PacketType::GameData;
        whdr.checksum = 0;
        std::vector<uint8_t> batchPkt(sizeof(WireHeader));
        memcpy(batchPkt.data(), &whdr, sizeof(WireHeader));
        int batched = 0;

        for (auto& sg : serverGhosts) {
            if (!sg.active) continue;
            if (visible.find(sg.index) == visible.end()) continue;
            bool known = cl.knownGhosts.find(sg.index) != cl.knownGhosts.end();

            T2Protocol::GhostMessage gm;
            gm.index = sg.index;
            gm.type = known ? T2Protocol::Ghost_Update : T2Protocol::Ghost_Create;
            // Reliability: re-send full Create (idempotent on client) for a short
            // window after first send so a dropped Create packet is retried.
            if (known && (nowGhost - cl.ghostFirstSent[sg.index]) < GHOST_CREATE_WINDOW)
                gm.type = T2Protocol::Ghost_Create;
            gm.classId = sg.classId;

            uint8_t ghBuf[32];
            size_t ghLen = T2Protocol::encodeGhostHeader(ghBuf, sizeof(ghBuf), gm);
            bool always = (sg.classId == 31);
            if (always) ghBuf[0] = T2Protocol::GDT_GhostAlways;
            if (ghLen == 0) continue;

            uint8_t posData[64];
            uint32_t p = 0;
            memcpy(posData + p, &sg.posX, 4); p += 4;
            memcpy(posData + p, &sg.posY, 4); p += 4;
            memcpy(posData + p, &sg.posZ, 4); p += 4;
            memcpy(posData + p, &sg.rotX, 4); p += 4;
            memcpy(posData + p, &sg.rotZ, 4); p += 4;
            memcpy(posData + p, &sg.health, 4); p += 4;
            float fKills = 0, fDeaths = 0, fTeam = 0;
            std::string pname = "Player";
            if (sg.classId == 31) {
                for (auto& c : clients)
                    if (c.playerGhostIndex == sg.index) { fKills = (float)c.kills; fDeaths = (float)c.deaths; fTeam = (float)c.teamId; pname = c.playerName; break; }
            }
            memcpy(posData + p, &fKills, 4); p += 4;
            memcpy(posData + p, &fDeaths, 4); p += 4;
            memcpy(posData + p, &fTeam, 4); p += 4; // teamId (for scoreboard)
            // Append player name (null-terminated, max 28 chars + null)
            size_t nameLen = std::min(pname.size(), (size_t)27);
            memcpy(posData + p, pname.c_str(), nameLen);
            posData[p + nameLen] = 0;
            p += (uint32_t)(nameLen + 1);

            size_t old = batchPkt.size();
            batchPkt.resize(old + ghLen + p);
            memcpy(batchPkt.data() + old, ghBuf, ghLen);
            memcpy(batchPkt.data() + old + ghLen, posData, p);
            batched++;

            if (!known) {
                cl.knownGhosts.insert(sg.index);
                cl.ghostFirstSent[sg.index] = nowGhost;
                Console::instance().printf(LogLevel::Debug, "Server: ghost create idx=%u class=%d for client %d",
                    (unsigned)sg.index, sg.classId, ci);
            }
        }
        if (batched > 0)
            sendTo(cl.addr, batchPkt.data(), batchPkt.size());
    }
};

GameServer::GameServer() : impl(new Impl) {}
GameServer::~GameServer() { stop(); delete impl; }

bool GameServer::start(uint16_t port) {
    // Load previous stats
    std::ifstream ifs("server_stats.txt");
    if (ifs.is_open()) {
        int totalPlayers = 0;
        int k, d, t; unsigned ip;
        while (ifs >> k >> d >> t >> ip) { totalPlayers++; }
        ifs.close();
        Console::instance().printf(LogLevel::Info, "Loaded stats: %d previous players", totalPlayers);
    }
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

    // Set up query socket on port+1
    impl->querySock = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->querySock >= 0) {
        sockaddr_in qaddr{};
        qaddr.sin_family = AF_INET;
        qaddr.sin_port = htons(port + 1);
        qaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(impl->querySock, (sockaddr*)&qaddr, sizeof(qaddr)) == 0) {
            int qflags = fcntl(impl->querySock, F_GETFL, 0);
            fcntl(impl->querySock, F_SETFL, qflags | O_NONBLOCK);
            Console::instance().printf(LogLevel::Info, "Server query API on port %d", port + 1);
        } else {
            close(impl->querySock);
            impl->querySock = -1;
        }
    }

    // Populate default datablocks for game objects with payload data
    struct DefaultDB { uint32_t classId; uint32_t objectId; uint32_t index; uint32_t total; const char* name; float health; };
    static const DefaultDB defaultDBs[] = {
        {128, 1, 0, 1, "GameBaseData", 100},
        {129, 2, 0, 1, "PlayerData", 100},
        {130, 3, 0, 1, "ItemData", 50},
        {131, 4, 0, 1, "CameraData", 50},
        {132, 5, 0, 1, "ProjectileData", 1},
        {133, 6, 0, 1, "ExplosionData", 1},
        {134, 7, 0, 1, "VehicleData", 200},
        {135, 8, 0, 1, "TurretData", 150},
        {136, 9, 0, 1, "BeaconData", 50},
        {137, 10, 0, 1, "FlagData", 1},
    };
    for (auto& d : defaultDBs) {
        Impl::DatablockInfo info;
        info.classId = d.classId;
        info.objectId = d.objectId;
        info.index = d.index;
        info.total = d.total;
        // Build simple payload: [health:4][shapeNameLen:2][shapeName...]
        float hp = d.health;
        std::string shapeName = d.name;
        shapeName += "_shape";
        uint16_t nameLen = (uint16_t)shapeName.size();
        info.payload.resize(4 + 2 + nameLen);
        memcpy(info.payload.data(), &hp, 4);
        memcpy(info.payload.data() + 4, &nameLen, 2);
        memcpy(info.payload.data() + 6, shapeName.data(), nameLen);
        impl->datablocks.push_back(std::move(info));
    }
    // Spawn a variety of ghost types with different shapes
    struct SpawnEntry { int32_t classId; const char* desc; float x, y, z; int itemType; };
    static const SpawnEntry ghostSpawns[] = {
        // Items (classId 20 = Item) with pickups
        {20, "Health",      10, 2, 10, 0},
        {20, "Energy",     -10, 2, 10, 1},
        {20, "Ammo",        10, 2, -10, 2},
        {20, "Health",     -10, 2, -10, 0},
        {20, "Energy",      20, 2, 0, 1},
        {20, "Ammo",       -20, 2, 0, 2},
        {20, "Health",      0, 2, 20, 0},
        {20, "Energy",      0, 2, -20, 1},
        // CTF Flags (classId 100 = RedFlag, 101 = BlueFlag)
        {100, "RedFlag",    10, 2, 30, -1},
        {101, "BlueFlag",  -10, 2, 30, -1},
        // Player shapes (classId 31 = Player)
        {31, "Player",      20, 2, 0, -1},
        {31, "Player",     -20, 2, 0, -1},
        // Turrets (classId 57 = Turret)
        {57, "Turret",      0, 2, 20, -1},
        {57, "Turret",      0, 2, -20, -1},
        // Vehicles
        {13, "FlyingVehicle",   15, 4, 15, -1},
        {17, "HoverVehicle",  -15, 3, 15, -1},
        {62, "WheeledVehicle", 15, 2, -15, -1},
        // Cameras (classId 5 = Camera)
        {5,  "Camera",     -15, 2, -15, -1},
        // Beacons (classId 2 = BeaconObject)
        {2,  "Beacon",      25, 2, 0, -1},
        // Debris (classId 6 = Debris)
        {6,  "Debris",      0, 2, 25, -1},
        // Bomb projectiles (classId 3 = BombProjectile)
        {3,  "Bomb",        8, 2, 8, -1},
        // Turret/Sentry
        {46, "SpawnSphere", 12, 2, -12, -1},
        {24, "Marker",     -12, 2, 12, -1},
    };
    for (auto& sp : ghostSpawns) {
        Impl::ServerGhost sg;
        sg.index = impl->nextGhostIndex++;
        sg.classId = sp.classId;
        sg.posX = sp.x; sg.posY = sp.y; sg.posZ = sp.z;
        sg.itemType = sp.itemType;
        impl->serverGhosts.push_back(sg);
    }
    // Try loading a mission file (default: missions/test mis named via sv_mission)
    const char* missionVar = Console::instance().getStringVariable("sv_mission");
    std::string mission = missionVar ? missionVar : "";
    if (!mission.empty()) {
        std::string fullPath = std::string("missions/") + mission;
        if (!fullPath.ends_with(".mis")) fullPath += ".mis";
        Console::instance().printf(LogLevel::Info, "Server: loading mission '%s'", fullPath.c_str());
        if (loadMission(fullPath.c_str())) {
            Console::instance().printf(LogLevel::Info, "Server: loaded %zu ghosts from mission", impl->serverGhosts.size());
        } else {
            Console::instance().printf(LogLevel::Warn, "Server: failed to load mission '%s', using defaults", fullPath.c_str());
        }
    }
    // Read game mode variables
    const char* modeVar = Console::instance().getStringVariable("sv_gamemode");
    if (modeVar) {
        if (strcmp(modeVar, "tdm") == 0 || strcmp(modeVar, "team") == 0)
            impl->gameMode = 1;
        else
            impl->gameMode = 0;
    }
    const char* limitVar = Console::instance().getStringVariable("sv_scorelimit");
    if (limitVar) impl->scoreLimit = atoi(limitVar);
    if (impl->scoreLimit < 1) impl->scoreLimit = 25;
    Console::instance().printf(LogLevel::Info, "Server: game mode %s, score limit %d",
        impl->gameMode == 1 ? "Team Deathmatch" : "Deathmatch", impl->scoreLimit);
    Console::instance().printf(LogLevel::Info, "Server started on port %d (%zu datablocks, %zu ghosts)",
        port, impl->datablocks.size(), impl->serverGhosts.size());
    return true;
}

bool GameServer::loadMission(const char* missionPath) {
    if (!impl->running) return false;
    auto& fs = Engine::instance().fs();
    auto data = fs.read(missionPath);
    if (data.empty()) {
        Console::instance().printf(LogLevel::Warn, "Server: mission file not found: %s", missionPath);
        return false;
    }
    std::string content((const char*)data.data(), data.size());
    auto objects = parseMisFile(content);

    // Class name aliases for mission objects
    struct ClassAlias { const char* misName; int32_t classId; };
    static const ClassAlias aliases[] = {
        {"Item", 20}, {"Player", 31}, {"Camera", 5},
        {"Turret", 57}, {"FlyingVehicle", 13}, {"HoverVehicle", 17},
        {"WheeledVehicle", 62}, {"Vehicle", 58}, {"BeaconObject", 2},
        {"Debris", 6}, {"Marker", 24}, {"WayPoint", 61},
        {"SpawnSphere", 46}, {"Flag", -1}, {"Projectile", 33},
        {"EnergyProjectile", 9}, {"Sensor", -1},
    };

    // Also build a quick alias map from classNameToClassId for net classes
    int spawned = 0;
    for (auto& obj : objects) {
        int32_t cid = -1;
        // Try aliases first
        for (auto& a : aliases) {
            if (obj.className == a.misName) { cid = a.classId; break; }
        }
        // Fall back to NetObjectClassNames lookup
        if (cid < 0) cid = classNameToClassId(obj.className);
        if (cid < 0) {
            Console::instance().printf(LogLevel::Debug, "Server: skipping unknown mission class '%s'", obj.className.c_str());
            continue;
        }

        // Parse position and rotation from properties
        std::string posStr = getProp(obj.props, "position");
        Point3F pos = posStr.empty() ? Point3F{0,2,0} : parsePos(posStr);
        std::string rotStr = getProp(obj.props, "rotation");
        float rotX = 0, rotZ = 0;
        if (!rotStr.empty()) {
            float vals[3] = {0,0,0};
            sscanf(rotStr.c_str(), "%f %f %f", &vals[0], &vals[1], &vals[2]);
            rotX = vals[0]; rotZ = vals[1];
        }

        // Scale position to our world (2x factor for typical T2 missions)
        pos.x *= 2.0f; pos.z *= 2.0f;
        if (pos.y < 1) pos.y = 2.0f; // ground clamp

        Impl::ServerGhost sg;
        sg.index = impl->nextGhostIndex++;
        sg.classId = cid;
        sg.posX = pos.x; sg.posY = pos.y; sg.posZ = pos.z;
        sg.rotX = rotX; sg.rotZ = rotZ;
        impl->serverGhosts.push_back(sg);

        // Collect spawn points for respawn
        if (cid == 46) { // SpawnSphere
            Impl::SpawnPoint sp;
            // Use the object's worldPosition with rotation offset if available
            sp.x = pos.x; sp.y = pos.y; sp.z = pos.z;
            impl->spawnPoints.push_back(sp);
        }
        spawned++;
    }
    Console::instance().printf(LogLevel::Info, "Server: spawned %d ghosts from mission '%s'", spawned, missionPath);
    return true;
}

uint32_t GameServer::spawnGhost(int32_t classId, float x, float y, float z) {
    if (!impl->running) return 0;
    Impl::ServerGhost sg;
    sg.index = impl->nextGhostIndex++;
    sg.classId = classId;
    sg.posX = x; sg.posY = y; sg.posZ = z;
    sg.health = 100;
    impl->serverGhosts.push_back(sg);
    return sg.index;
}

bool GameServer::removeGhost(uint32_t index) {
    for (auto it = impl->serverGhosts.begin(); it != impl->serverGhosts.end(); ++it) {
        if (it->index == index) {
            // Broadcast delete to all connected clients
            impl->broadcastGhostDelete(index, -1);
            impl->serverGhosts.erase(it);
            return true;
        }
    }
    return false;
}

size_t GameServer::ghostCount() const {
    return impl->serverGhosts.size();
}

void GameServer::spawnBot() {
    if (!impl->running) return;
    Impl::Client bot;
    memset(&bot.addr, 0, sizeof(bot.addr));
    bot.addrLen = sizeof(bot.addr);
    bot.lastReceive = Engine::instance().timer().now();
    bot.active = true;
    bot.isBot = true;
    bot.health = 100;
    bot.energy = 100;
    // Place bot at a random spawn point
    if (!impl->spawnPoints.empty()) {
        size_t si = (size_t)(rand() % impl->spawnPoints.size());
        bot.posX = impl->spawnPoints[si].x;
        bot.posY = impl->spawnPoints[si].y;
        bot.posZ = impl->spawnPoints[si].z;
    } else {
        bot.posX = (float)(rand() % 40 - 20);
        bot.posY = 5;
        bot.posZ = (float)(rand() % 40 - 20);
    }
    // Create player ghost for the bot
    Impl::ServerGhost sg;
    sg.index = impl->nextGhostIndex++;
    sg.classId = 31; // Player
    sg.posX = bot.posX; sg.posY = bot.posY; sg.posZ = bot.posZ;
    sg.health = 100;
    impl->serverGhosts.push_back(sg);
    bot.playerGhostIndex = sg.index;
    bot.playerName = "Bot_" + std::to_string(sg.index);
    impl->clients.push_back(std::move(bot));
    Console::instance().printf(LogLevel::Info, "Bot spawned at (%.1f, %.1f, %.1f), ghost idx=%u",
        bot.posX, bot.posY, bot.posZ, (unsigned)sg.index);
}

void GameServer::kickClient(int ci) {
    if (ci < 0 || ci >= (int)impl->clients.size() || !impl->clients[ci].active) {
        Console::instance().printf(LogLevel::Warn, "Kick: invalid client %d", ci);
        return;
    }
    if (impl->clients[ci].isBot) {
        impl->clients[ci].active = false;
        Console::instance().printf(LogLevel::Info, "Bot %d kicked", ci);
        return;
    }
    // Send disconnect notification and remove
    impl->clients[ci].active = false;
    impl->clients[ci].lastReceive = 0; // force timeout on next update
    Console::instance().printf(LogLevel::Info, "Client %d kicked", ci);
}

void GameServer::banClient(int ci) {
    if (ci < 0 || ci >= (int)impl->clients.size() || !impl->clients[ci].active) {
        Console::instance().printf(LogLevel::Warn, "Ban: invalid client %d", ci);
        return;
    }
    uint32_t ip = impl->clients[ci].addr.sin_addr.s_addr;
    impl->bannedIPs.push_back(ip);
    impl->clients[ci].active = false;
    impl->clients[ci].lastReceive = 0;
    Console::instance().printf(LogLevel::Info, "Client %d banned (IP %08x)", ci, ip);
}

void GameServer::clearBans() {
    impl->bannedIPs.clear();
    Console::instance().printf(LogLevel::Info, "Ban list cleared");
}

void GameServer::startRecording(const char* path) {
    if (impl->recFile.is_open()) impl->recFile.close();
    impl->recFile.open(path, std::ios::binary);
    if (impl->recFile.is_open()) {
        // Write header: magic + timestamp
        uint32_t magic = 0x544F5243; // "TORC"
        double start = Engine::instance().timer().now();
        impl->recFile.write((const char*)&magic, 4);
        impl->recFile.write((const char*)&start, sizeof(start));
        impl->recStartTime = start;
        Console::instance().printf(LogLevel::Info, "Recording started: %s", path);
    } else {
        Console::instance().printf(LogLevel::Warn, "Failed to open recording: %s", path);
    }
}

void GameServer::stopRecording() {
    if (impl->recFile.is_open()) {
        impl->recFile.close();
        Console::instance().printf(LogLevel::Info, "Recording stopped");
    }
}

void GameServer::changeMap(const char* mission) {
    if (!impl->running) return;
    Console::instance().printf(LogLevel::Info, "Changing map to: %s", mission);
    // Clear existing ghosts and reset
    impl->serverGhosts.clear();
    impl->projectiles.clear();
    impl->nextGhostIndex = 1;
    // Reset client positions but keep them connected
    for (auto& cl : impl->clients) {
        cl.knownGhosts.clear();
        cl.datablocksSent = false;
        cl.playerGhostIndex = 0;
    }
    // Load new mission
    std::string fullPath = std::string("missions/") + mission;
    if (!fullPath.ends_with(".mis")) fullPath += ".mis";
    loadMission(fullPath.c_str());
    Console::instance().printf(LogLevel::Info, "Map changed, %zu ghosts spawned", impl->serverGhosts.size());
}

void GameServer::setGameMode(int mode) {
    impl->gameMode = mode;
    impl->teamScore[1] = impl->teamScore[2] = 0;
    Console::instance().printf(LogLevel::Info, "Game mode set to %s (0=DM, 1=TDM)",
        mode == 1 ? "Team Deathmatch" : "Deathmatch");
}

void GameServer::stop() {
    // Save stats
    std::ofstream ofs("server_stats.txt");
    if (ofs.is_open()) {
        for (auto& cl : impl->clients) {
            if (!cl.active) continue;
            ofs << cl.kills << " " << cl.deaths << " " << (int)cl.playTime;
            ofs << " " << ntohl(cl.addr.sin_addr.s_addr) << "\n";
        }
        ofs.close();
        Console::instance().printf(LogLevel::Info, "Saved stats for %zu players", impl->clients.size());
    }
    if (impl->sock >= 0) {
        close(impl->sock);
        impl->sock = -1;
    }
    if (impl->querySock >= 0) {
        close(impl->querySock);
        impl->querySock = -1;
    }
    impl->running = false;
    impl->clients.clear();
    impl->serverGhosts.clear();
    impl->datablocks.clear();
}

void GameServer::update() {
    if (!impl->running || impl->sock < 0) return;

    // Profile timing
    using clock = std::chrono::steady_clock;
    static clock::time_point lastProfile = clock::now();
    static int profileFrames = 0;
    profileFrames++;
    auto t0 = clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - lastProfile).count();
    if (elapsed >= 5000) {
        float fps = profileFrames / (elapsed / 1000.0f);
        Console::instance().printf(LogLevel::Debug, "Server perf: %.1f ticks/sec, %zu ghosts, %zu clients",
            fps, impl->serverGhosts.size(), impl->clients.size());
        profileFrames = 0;
        lastProfile = t0;
    }

    // Quick check: if no active clients, skip most work (packets still processed)
    bool hasActive = false;
    for (auto& cl : impl->clients) { if (cl.active) { hasActive = true; break; } }

    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(impl->sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;

        double now = Engine::instance().timer().now();

        // Parse wire header (15 bytes) to get packet type
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

            int ci = impl->findClient(from);
            if (ci < 0) {
                // Check ban list
                uint32_t ip = from.sin_addr.s_addr;
                bool banned = false;
                for (auto b : impl->bannedIPs) { if (b == ip) { banned = true; break; } }
                if (banned) {
                    Console::instance().printf(LogLevel::Info, "Server: rejected banned connection from %s",
                        inet_ntoa(from.sin_addr));
                    continue;
                }
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
                // Assign team in TDM mode (alternate)
                if (impl->gameMode == 1) {
                    int redCount = 0, blueCount = 0;
                    for (auto& cc : impl->clients) { if (!cc.active) continue; if (cc.teamId == 1) redCount++; if (cc.teamId == 2) blueCount++; }
                    impl->clients[ci].teamId = (redCount <= blueCount) ? 1 : 2;
                }
            }

            // Send Challenge
            T2Protocol::ChallengeMessage chal;
            chal.challenge[0] = (uint32_t)(rand() ^ (uintptr_t)&from);
            chal.challenge[1] = (uint32_t)(rand() ^ (int)(now * 1000));
            impl->clients[ci].expectedResp[0] = chal.challenge[0];
            impl->clients[ci].expectedResp[1] = chal.challenge[1];

            uint8_t chalBuf[sizeof(WireHeader) + sizeof(chal)];
            WireHeader whdr{};
            whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
            whdr.type = (uint8_t)PacketType::Challenge;
            whdr.checksum = 0;
            memcpy(chalBuf, &whdr, sizeof(WireHeader));
            memcpy(chalBuf + sizeof(WireHeader), &chal, sizeof(chal));
            impl->sendTo(from, chalBuf, sizeof(chalBuf));
        }

        if (ptype == PacketType::ChallengeResponse) {
            int ci = impl->findClient(from);
            if (ci >= 0 && payloadLen >= 8) {
                T2Protocol::ChallengeResponse resp;
                memcpy(&resp, payload, 8);
                auto& cl = impl->clients[ci];
                cl.active = true;
                cl.lastReceive = now;

                // Send ConnectOK
                WireHeader whdr{};
                whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                whdr.type = (uint8_t)PacketType::ConnectOK;
                whdr.checksum = 0;
                uint8_t ok = 1;
                std::vector<uint8_t> okBuf(sizeof(WireHeader) + 1);
                memcpy(okBuf.data(), &whdr, sizeof(WireHeader));
                okBuf[sizeof(WireHeader)] = ok;
                impl->sendTo(from, okBuf.data(), okBuf.size());

                // Create a ghost entry for this new player
                Impl::ServerGhost sg;
                sg.index = impl->nextGhostIndex++;
                sg.classId = 31; // Player class
                sg.posX = cl.posX; sg.posY = cl.posY; sg.posZ = cl.posZ;
                sg.rotX = cl.rotX; sg.rotZ = cl.rotZ;
                sg.health = cl.health; sg.energy = cl.energy;
                impl->serverGhosts.push_back(sg);
                cl.playerGhostIndex = sg.index;

                // Send game state with control object ghost index
                {
                    T2Protocol::GameStateMessage gs;
                    gs.controlObjectGhostIndex = sg.index;
                    gs.energy = 100.0f;
                    gs.flags = 0;
                    gs.gameMode = impl->gameMode;
                    gs.scoreLimit = impl->scoreLimit;
                    uint8_t gsBuf[32];
                    size_t gsLen = T2Protocol::encodeGameState(gsBuf, sizeof(gsBuf), gs);
                    if (gsLen > 0) {
                        WireHeader gsWhdr{};
                        gsWhdr.sequence = 1; gsWhdr.ack = 0; gsWhdr.ackMask = 0;
                        gsWhdr.type = (uint8_t)PacketType::GameData;
                        gsWhdr.checksum = 0;
                        std::vector<uint8_t> gsPkt(sizeof(WireHeader) + gsLen);
                        memcpy(gsPkt.data(), &gsWhdr, sizeof(WireHeader));
                        memcpy(gsPkt.data() + sizeof(WireHeader), gsBuf, gsLen);
                        impl->sendTo(from, gsPkt.data(), gsPkt.size());
                    }
                }
                Console::instance().printf(LogLevel::Info, "Server: client %s authenticated, ghost idx=%u",
                    inet_ntoa(from.sin_addr), (unsigned)sg.index);
            }
        }

        if (ptype == PacketType::GameData) {
            int ci = impl->findClient(from);
            if (ci < 0) continue;

            auto& client = impl->clients[ci];
            client.lastReceive = now;
            if (!client.active) continue; // ignore GameData from inactive slots

            // Check for GDT_Command
            if (payloadLen > 0 && payload[0] == T2Protocol::GDT_Command && payloadLen >= 3) {
                uint16_t cmdLen = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
                if (cmdLen > 0 && (size_t)(3 + cmdLen) <= payloadLen) {
                    std::string cmd((const char*)payload + 3, cmdLen);
                    // Handle sv_name specially (set player name for this client)
                    if (cmd.rfind("sv_name ", 0) == 0 && cmd.size() > 8) {
                        client.playerName = cmd.substr(8, 255);
                        Console::instance().printf(LogLevel::Info, "Client %d set name: %s", ci, client.playerName.c_str());
                    } else {
                        Console::instance().execute(cmd.c_str());
                    }
                }
                continue;
            }

            // Handle chat messages: broadcast to all clients
            if (payloadLen > 0 && payload[0] == T2Protocol::GDT_ChatMessage) {
                T2Protocol::ChatMessage chat;
                if (T2Protocol::decodeChat(payload, payloadLen, chat)) {
                    WireHeader whdr{};
                    whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                    whdr.type = (uint8_t)PacketType::GameData;
                    whdr.checksum = 0;
                    uint8_t chatBuf[512];
                    if (T2Protocol::encodeChat(chatBuf, sizeof(chatBuf), chat)) {
                        std::vector<uint8_t> pkt(sizeof(WireHeader) + 4 + strlen(chat.sender) + strlen(chat.text));
                        memcpy(pkt.data(), &whdr, sizeof(WireHeader));
                        memcpy(pkt.data() + sizeof(WireHeader), chatBuf, pkt.size() - sizeof(WireHeader));
                        for (auto& c : impl->clients)
                            if (c.active && !c.isBot) impl->sendTo(c.addr, pkt.data(), pkt.size());
                    }
                    Console::instance().printf(LogLevel::Info, "[CHAT] %s: %s", chat.sender, chat.text);
                }
                continue;
            }

            T2Protocol::MoveMessage move;
            if (payloadLen > 0 && T2Protocol::decodeMove(payload, payloadLen, move)) {
                client.lastMove = move;
                // Anti-cheat: validate rotation
                float rotDiffZ = fabsf(move.rotZ - client.rotZ);
                float rotDiffX = fabsf(move.rotX - client.rotX);
                if (rotDiffZ > 6.28318f) { move.rotZ = client.rotZ; } // full spin in one frame?
                if (rotDiffX > 3.14159f) { move.rotX = client.rotX; }
                // Anti-cheat: cap fire rate (per-client)
                if (move.flags & 8) {
                    double now = Engine::instance().timer().now();
                    if (now - client.acLastFire < 0.05) move.flags &= ~8;
                    else client.acLastFire = now;
                }
                // Use client orientation only; position is server-authoritative
                client.rotZ = move.rotZ;
                client.rotX = move.rotX;

                // Dead players don't process movement input (frozen until respawn)
                if (client.respawnAt <= 0) {
                // Compute velocity from input flags based on client orientation
                float forward = 0, strafe = 0;
                if (move.flags & 1) forward += 1.0f;
                if (move.flags & 32) strafe -= 1.0f; // strafe left
                if (move.flags & 64) strafe += 1.0f; // strafe right
                float yaw = client.rotZ;
                float speed = 15.0f;
                if (move.flags & 4) speed = 30.0f; // jet boost
                float cosYaw = cosf(yaw), sinYaw = sinf(yaw);
                client.velX = (forward * sinYaw + strafe * cosYaw) * speed;
                client.velZ = (forward * cosYaw - strafe * sinYaw) * speed;

                // Jump: apply upward impulse if on ground
                if ((move.flags & 2) && client.posY <= 2.1f) {
                    client.velY = 10.0f;
                }

                // Jet: upward force, consumes energy
                if (move.flags & 4) {
                    client.velY += 8.0f * 0.05f; // gentle upward push per tick
                    client.energy -= 20.0f * 0.05f; // consume energy
                    if (client.energy < 0) client.energy = 0;
                } else {
                    // Recharge energy when not jetting
                    client.energy += 5.0f * 0.05f;
                    if (client.energy > 100) client.energy = 100;
                }
                }

                // Handle fire input: spawn a projectile ghost (per-client cadence)
                double now = Engine::instance().timer().now();
                if (client.respawnAt <= 0 && (move.flags & 8) && (now - client.lastFireTime) > 0.35) {
                    client.lastFireTime = now;
                    float pitch = client.rotX;
                    float pyaw = client.rotZ;
                    // Compute direction from pitch/yaw
                    float dirX = sinf(pyaw) * cosf(pitch);
                    float dirY = sinf(pitch);
                    float dirZ = cosf(pyaw) * cosf(pitch);
                    float projSpeed = 40.0f;

                    Impl::ServerProjectile sp;
                    sp.ghostIndex = impl->nextGhostIndex++;
                    sp.posX = client.posX + dirX * 2.0f;
                    sp.posY = client.posY + 1.5f;
                    sp.posZ = client.posZ + dirZ * 2.0f;
                    sp.velX = client.velX + dirX * projSpeed;
                    sp.velY = client.velY + dirY * projSpeed;
                    sp.velZ = client.velZ + dirZ * projSpeed;
                    sp.ownerCi = (int)ci;
                    auto wp = Impl::weaponProfile(0);
                    sp.damage = wp.damage; sp.splashRadius = wp.splash;
                    impl->projectiles.push_back(sp);

                    // Spawn visual ghost for this projectile
                    Impl::ServerGhost sg;
                    sg.index = sp.ghostIndex;
                    sg.classId = 33; // Projectile
                    sg.posX = sp.posX; sg.posY = sp.posY; sg.posZ = sp.posZ;
                    impl->serverGhosts.push_back(sg);
                    Console::instance().printf(LogLevel::Debug, "Server: projectile spawned idx=%u", (unsigned)sg.index);
                }

                // Update the player ghost's rotation immediately
                if (client.playerGhostIndex > 0) {
                    for (auto& sg : impl->serverGhosts) {
                        if (sg.index == client.playerGhostIndex) {
                            sg.rotX = client.rotX;
                            sg.rotZ = client.rotZ;
                            break;
                        }
                    }
                }

                // Send update back with the move's seq echoed
                T2Protocol::UpdateMessage update;
                update.posX = client.posX;
                update.posY = client.posY;
                update.posZ = client.posZ;
                update.rotZ = client.rotZ;
                update.rotX = client.rotX;
                update.velX = client.velX;
                update.velY = client.velY;
                update.velZ = client.velZ;
                update.health = client.health;
                update.energy = client.energy;
                update.flags = 0;
                update.lastMoveSeq = move.seq;

                uint8_t upBuf[64];
                WireHeader whdr{};
                whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                whdr.type = (uint8_t)PacketType::GameData;
                whdr.checksum = 0;
                memcpy(upBuf, &whdr, sizeof(WireHeader));
                // Update message is exactly 40 bytes; only send if encode succeeded.
                if (T2Protocol::encodeUpdate(upBuf + sizeof(WireHeader), sizeof(upBuf) - sizeof(WireHeader), update))
                    impl->sendTo(from, upBuf, sizeof(WireHeader) + 40);
            }
        }

        if (ptype == PacketType::Ping) {
            uint8_t pong = (uint8_t)PacketType::Pong;
            impl->sendTo(from, &pong, 1);
        }
    }

    // Handle query API requests
    if (impl->querySock >= 0) {
        sockaddr_in qfrom{};
        socklen_t qfromLen = sizeof(qfrom);
        uint8_t qbuf[256];
        int qn = recvfrom(impl->querySock, qbuf, sizeof(qbuf), 0, (sockaddr*)&qfrom, &qfromLen);
        if (qn > 0) {
            if (qn >= (int)sizeof(qbuf)) qn = (int)sizeof(qbuf) - 1;
            qbuf[qn] = 0;
            std::string reply;
            auto jsonEscape = [](const std::string& s) {
                std::string out;
                out.reserve(s.size() + 8);
                for (char c : s) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default: out += c;
                    }
                }
                return out;
            };
            if (strcmp((const char*)qbuf, "QUERY") == 0) {
                // Build JSON-like response
                reply = "{";
                reply += "\"name\":\"Torch Server\",";
                reply += "\"map\":\"test\",";
                reply += "\"gamemode\":0,";
                reply += "\"numplayers\":" + std::to_string((int)impl->clients.size()) + ",";
                reply += "\"maxplayers\":32,";
                reply += "\"players\":[";
                bool first = true;
                for (auto& cl : impl->clients) {
                    if (!cl.active) continue;
                    if (!first) reply += ",";
                    first = false;
                    reply += "{\"name\":\"" + jsonEscape(cl.playerName) + "\",";
                    reply += "\"kills\":" + std::to_string(cl.kills) + ",";
                    reply += "\"deaths\":" + std::to_string(cl.deaths) + "}";
                }
                reply += "]}";
            } else {
                reply = "UNKNOWN";
            }
            sendto(impl->querySock, reply.data(), reply.size(), 0, (sockaddr*)&qfrom, qfromLen);
        }
    }

    // After processing incoming packets, simulate movement for all clients
    // then send ghost/datablock updates and remove stale clients
    if (!hasActive) return; // skip simulation when no clients
    double now = Engine::instance().timer().now();
    static double lastSimTime = 0;
    if (lastSimTime == 0) lastSimTime = now;
    float simDt = (float)(now - lastSimTime);
    if (simDt > 0.05f) simDt = 0.05f; // cap to avoid spiral of death
    lastSimTime = now;

    for (auto& cl : impl->clients) {
        if (!cl.active) continue;
        cl.playTime += simDt;
        // Position history for lag compensation (every ~100ms)
        if (cl.posHistory.empty() || (now - cl.posHistory.back().time) > 0.1) {
            decltype(cl.posHistory)::value_type ps = {now, cl.posX, cl.posY, cl.posZ};
            cl.posHistory.push_back(ps);
            if ((int)cl.posHistory.size() > 64)
                cl.posHistory.erase(cl.posHistory.begin());
        }
        // Integrate position from velocity
        cl.posX += cl.velX * simDt;
        cl.posY += cl.velY * simDt;
        cl.posZ += cl.velZ * simDt;
        // Apply gravity if not jetting
        float groundY = 2.0f;
        if (heightCB) {
            float h = heightCB(cl.posX, cl.posZ, heightCtx);
            if (h > groundY) groundY = h;
        }
        bool onGround = (cl.posY <= groundY + 0.1f);
        if (!(cl.lastMove.flags & 4) && !onGround) {
            cl.velY -= 25.0f * simDt; // gravity
        }
        // Ground clamp
        if (cl.posY < groundY) { cl.posY = groundY; cl.velY = 0; }
        // Friction - higher on ground, lower in air
        float friction = onGround ? 5.0f : 0.5f;
        cl.velX *= (1.0f - friction * simDt);
        cl.velZ *= (1.0f - friction * simDt);

        // Push simulated position + vitals into player ghost
        if (cl.playerGhostIndex > 0) {
            for (auto& sg : impl->serverGhosts) {
                if (sg.index == cl.playerGhostIndex) {
                    sg.posX = cl.posX;
                    sg.posY = cl.posY;
                    sg.posZ = cl.posZ;
                    sg.health = cl.health;
                    sg.energy = cl.energy;
                    break;
                }
            }
        }

        // Drop flag if carrier died this frame
        if (cl.health <= 0 && cl.flagCarried != 0) {
            for (auto& sg : impl->serverGhosts) {
                if (sg.index == (uint32_t)cl.flagCarried) {
                    sg.posX = cl.posX; sg.posY = cl.posY + 1; sg.posZ = cl.posZ;
                    sg.passengerCi = -1;
                    sg.respawnTime = Engine::instance().timer().now() + 30.0;
                    cl.flagCarried = 0;
                    Console::instance().printf(LogLevel::Info, "Flag %d dropped on carrier death", sg.classId);
                    break;
                }
            }
        }

        // Death check & respawn (with delay so death is meaningful)
        double tnow = Engine::instance().timer().now();
        const float RESPAWN_DELAY = 3.0f;
        if (cl.health <= 0 && cl.respawnAt <= 0) {
            // Just died: freeze in place, schedule respawn. kills/deaths are
            // already credited at hit time; pin health to 0 so clients show death.
            cl.health = 0;
            cl.velX = cl.velY = cl.velZ = 0;
            cl.respawnAt = tnow + RESPAWN_DELAY;
            for (auto& sg : impl->serverGhosts)
                if (sg.index == cl.playerGhostIndex) { sg.health = 0; break; }
            Console::instance().printf(LogLevel::Info, "Client %d died (respawning in %.1fs)", (int)(&cl - &impl->clients[0]), RESPAWN_DELAY);
        }
        if (cl.respawnAt > 0 && tnow >= cl.respawnAt) {
            cl.health = 100; cl.energy = 100;
            cl.respawnAt = 0;
            cl.velX = cl.velY = cl.velZ = 0;
            if (!impl->spawnPoints.empty()) {
                size_t idx = (size_t)(rand() % impl->spawnPoints.size());
                auto& rsp = impl->spawnPoints[idx];
                cl.posX = rsp.x; cl.posY = rsp.y; cl.posZ = rsp.z;
                Console::instance().printf(LogLevel::Info, "Client %d respawned at spawn %zu", (int)(&cl - &impl->clients[0]), idx);
            } else {
                cl.posX = 0; cl.posY = 5; cl.posZ = 0;
            }
            for (auto& sg : impl->serverGhosts) {
                if (sg.index == cl.playerGhostIndex) {
                    sg.posX = cl.posX; sg.posY = cl.posY; sg.posZ = cl.posZ;
                    sg.health = 100; sg.energy = 100;
                    break;
                }
            }
        }
    }

    // Item pickup detection
    double pickupNow = Engine::instance().timer().now();
    const float pickupRadius = 2.0f;
    const float pickupRadiusSq = pickupRadius * pickupRadius;
    for (auto& cl : impl->clients) {
        if (!cl.active || cl.playerGhostIndex == 0) continue;
        for (auto& sg : impl->serverGhosts) {
            if (!sg.active || sg.itemType < 0) continue;
            if (sg.index == cl.playerGhostIndex) continue;
            if (sg.respawnTime > pickupNow) continue;
            float dx = cl.posX - sg.posX;
            float dz = cl.posZ - sg.posZ;
            if (dx*dx + dz*dz > pickupRadiusSq) continue;
            float dy = cl.posY - sg.posY;
            if (dy > 3.0f || dy < -3.0f) continue;
            // Pickup!
            switch (sg.itemType) {
                case 0: cl.health = std::min(100.0f, cl.health + 25.0f); break;
                case 1: cl.energy = std::min(100.0f, cl.energy + 25.0f); break;
                case 2: break;
                default: break;
            }
            sg.active = false;
            sg.respawnTime = pickupNow + 20.0;
            WireHeader whdr{};
            whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
            whdr.type = (uint8_t)PacketType::GameData;
            whdr.checksum = 0;
            T2Protocol::GhostMessage gm;
            gm.index = sg.index;
            gm.type = T2Protocol::Ghost_Delete;
            uint8_t ghBuf[16];
            size_t ghLen = T2Protocol::encodeGhostHeader(ghBuf, sizeof(ghBuf), gm);
            if (ghLen > 0) {
                std::vector<uint8_t> pkt(sizeof(WireHeader) + ghLen);
                memcpy(pkt.data(), &whdr, sizeof(WireHeader));
                memcpy(pkt.data() + sizeof(WireHeader), ghBuf, ghLen);
                for (auto& c : impl->clients)
                    if (c.active && !c.isBot) impl->sendTo(c.addr, pkt.data(), pkt.size());
            }
            Console::instance().printf(LogLevel::Debug, "Item %u picked up by client %d (type %d)",
                (unsigned)sg.index, (int)(&cl - &impl->clients[0]), sg.itemType);
        }
    }

    // Respawn timed-out items
    for (auto& sg : impl->serverGhosts) {
        if (!sg.active && sg.itemType >= 0 && sg.respawnTime > 0 && pickupNow >= sg.respawnTime) {
            sg.active = true;
            sg.respawnTime = 0;
            Console::instance().printf(LogLevel::Debug, "Item %u respawned", (unsigned)sg.index);
        }
    }

    // CTF flag handling
    if (impl->gameMode == 1) {
        for (auto& cl : impl->clients) {
            if (!cl.active) continue;
            for (auto& sg : impl->serverGhosts) {
                if (!sg.active || (sg.classId != 100 && sg.classId != 101)) continue;
                int flagTeam = (sg.classId == 100) ? 1 : 2; // RedFlag=team1, BlueFlag=team2

                // Flag carried by someone: sync to carrier
                if (sg.passengerCi >= 0) {
                    auto& carrier = impl->clients[sg.passengerCi];
                    if (!carrier.active || carrier.health <= 0) {
                        // Carrier died: drop flag
                        sg.posX = carrier.posX; sg.posY = carrier.posY + 1; sg.posZ = carrier.posZ;
                        sg.passengerCi = -1;
                        sg.respawnTime = Engine::instance().timer().now() + 30.0;
                        carrier.flagCarried = 0;
                        Console::instance().printf(LogLevel::Info, "Flag %d dropped by dead carrier", sg.classId);
                    } else {
                        // Update flag position to carrier
                        sg.posX = carrier.posX; sg.posY = carrier.posY + 3.0f; sg.posZ = carrier.posZ;
                        // Check for capture: carrier at own base (near original flag spawn)
                        for (auto& baseSg : impl->serverGhosts) {
                            if (baseSg.classId == sg.classId && baseSg.index != sg.index) continue;
                            if (baseSg.classId == sg.classId && baseSg.index == sg.index) {
                                float dx = carrier.posX - baseSg.posX;
                                float dz = carrier.posZ - baseSg.posZ;
                                if (dx*dx + dz*dz < 16.0f) {
                                    // Captured!
                                    impl->teamScore[carrier.teamId] += 5;
                                    Console::instance().printf(LogLevel::Info, "Team %d captured the flag! (Score: %d)",
                                        carrier.teamId, impl->teamScore[carrier.teamId]);
                                    sg.active = false;
                                    sg.passengerCi = -1;
                                    carrier.flagCarried = 0;
                                    sg.respawnTime = Engine::instance().timer().now() + 10.0;
                                    // Broadcast flag delete
                                    WireHeader whdr{};
                                    whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                                    whdr.type = (uint8_t)PacketType::GameData;
                                    whdr.checksum = 0;
                                    T2Protocol::GhostMessage gm;
                                    gm.index = sg.index; gm.type = T2Protocol::Ghost_Delete;
                                    uint8_t ghBuf[16];
                                    size_t ghLen = T2Protocol::encodeGhostHeader(ghBuf, sizeof(ghBuf), gm);
                                    if (ghLen > 0) {
                                        std::vector<uint8_t> pkt(sizeof(WireHeader) + ghLen);
                                        memcpy(pkt.data(), &whdr, sizeof(WireHeader));
                                        memcpy(pkt.data() + sizeof(WireHeader), ghBuf, ghLen);
                                        for (auto& c : impl->clients)
                                            if (c.active && !c.isBot) impl->sendTo(c.addr, pkt.data(), pkt.size());
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (!sg.active) continue;
                }

                // Near a flag on the ground (not being carried): pick it up
                if (sg.passengerCi >= 0) continue;
                if (sg.respawnTime > Engine::instance().timer().now()) continue;

                float dx = cl.posX - sg.posX;
                float dz = cl.posZ - sg.posZ;
                if (dx*dx + dz*dz < 9.0f) {
                    float dy = cl.posY - sg.posY;
                    if (dy > -3.0f && dy < 3.0f && cl.teamId != flagTeam) {
                        // Pick up enemy flag
                        sg.passengerCi = (int)(&cl - &impl->clients[0]);
                        cl.flagCarried = (int)sg.index;
                        Console::instance().printf(LogLevel::Info, "Player %d picked up flag %d",
                            sg.passengerCi, sg.classId);
                        break;
                    }
                }
            }
        }

        // Respawn timed-out flags
        for (auto& sg : impl->serverGhosts) {
            if ((sg.classId == 100 || sg.classId == 101) && !sg.active && sg.respawnTime > 0 &&
                Engine::instance().timer().now() >= sg.respawnTime) {
                sg.active = true;
                sg.respawnTime = 0;
                sg.passengerCi = -1;
                Console::instance().printf(LogLevel::Debug, "Flag %u respawned at base", (unsigned)sg.index);
            }
        }
    }

    // Vehicle enter/exit
    for (auto& cl : impl->clients) {
        if (!cl.active) continue;
        bool wantUse = (cl.lastMove.flags & 16) != 0;
        bool& prev = impl->vehUsePrev[cl.playerGhostIndex];
        bool justPressed = wantUse && !prev;
        prev = wantUse;
        if (!justPressed) continue;

        // Check if already in a vehicle
        bool inVeh = false;
        for (auto& sg : impl->serverGhosts) {
            if (sg.passengerCi >= 0 && &cl - &impl->clients[0] == sg.passengerCi) {
                // Exit vehicle
                // Place player at an offset from the vehicle
                float exitOffset = 3.0f;
                cl.posX = sg.posX + cosf(sg.rotZ) * exitOffset;
                cl.posZ = sg.posZ - sinf(sg.rotZ) * exitOffset;
                cl.posY = sg.posY + 1.0f;
                sg.passengerCi = -1;
                Console::instance().printf(LogLevel::Debug, "Client %d exited vehicle %u",
                    (int)(&cl - &impl->clients[0]), (unsigned)sg.index);
                break;
            }
        }
        if (inVeh) continue;

        // Try to enter a nearby vehicle
        for (auto& sg : impl->serverGhosts) {
            if (!sg.active || sg.passengerCi >= 0) continue;
            bool isVeh = (sg.classId == 13 || sg.classId == 17 || sg.classId == 62 || sg.classId == 58);
            if (!isVeh) continue;
            float dx = cl.posX - sg.posX;
            float dz = cl.posZ - sg.posZ;
            if (dx*dx + dz*dz < 9.0f) {
                sg.passengerCi = (int)(&cl - &impl->clients[0]);
                Console::instance().printf(LogLevel::Debug, "Client %d entered vehicle %u",
                    sg.passengerCi, (unsigned)sg.index);
                break;
            }
        }
    }

    // Vehicle physics: move vehicles with passengers
    for (auto& sg : impl->serverGhosts) {
        if (sg.passengerCi < 0 || sg.passengerCi >= (int)impl->clients.size()) continue;
        auto& driver = impl->clients[sg.passengerCi];
        if (!driver.active) continue;
        float forward = (driver.lastMove.flags & 1) ? 1.0f : 0;
        float backward = 0; // could add flag later
        bool isFlying = (sg.classId == 13);
        bool isHover = (sg.classId == 17);
        float accel = isFlying ? 25.0f : isHover ? 20.0f : 15.0f;
        float maxSpeed = isFlying ? 35.0f : isHover ? 25.0f : 20.0f;
        float braking = isFlying ? 1.0f : isHover ? 2.0f : 3.0f;

        // Turn vehicle toward driver's yaw
        float yawDiff = driver.rotZ - sg.rotZ;
        while (yawDiff > 3.14159f) yawDiff -= 6.28318f;
        while (yawDiff < -3.14159f) yawDiff += 6.28318f;
        sg.rotZ += yawDiff * 0.1f; // smooth turn

        // Accelerate/brake
        if (forward > 0)
            sg.vehSpeed = std::min(maxSpeed, sg.vehSpeed + accel * simDt);
        else if (backward > 0)
            sg.vehSpeed = std::max(-maxSpeed * 0.5f, sg.vehSpeed - accel * simDt);
        else
            sg.vehSpeed *= (1.0f - braking * simDt);

        // Move vehicle in direction it's facing
        sg.posX += sinf(sg.rotZ) * sg.vehSpeed * simDt;
        sg.posZ += cosf(sg.rotZ) * sg.vehSpeed * simDt;
        // Ground/hover height
        float groundY = 2.0f;
        if (heightCB) groundY = heightCB(sg.posX, sg.posZ, heightCtx);
        if (isFlying) {
            sg.posY = groundY + 10.0f;
        } else if (isHover) {
            sg.posY = groundY + 3.0f;
        } else {
            sg.posY = groundY + 0.5f;
        }

        // Sync driver position to vehicle
        driver.posX = sg.posX;
        driver.posY = sg.posY + 1.5f;
        driver.posZ = sg.posZ;
        driver.velX = 0; driver.velY = 0; driver.velZ = 0;
        driver.rotZ = sg.rotZ;
    }

    // Bot AI with improved behavior
    for (auto& bot : impl->clients) {
        if (!bot.active || !bot.isBot) continue;

        // Per-bot state (Impl member map keyed by ghost index, cleared on disconnect)
        auto& state = impl->botStates[bot.playerGhostIndex];
        state.strafeTimer -= simDt;
        state.thinkTimer -= simDt;
        if (state.thinkTimer <= 0) {
            state.thinkTimer = 0.1f + (rand() % 200) / 1000.0f; // think 5-10 times/sec

            // Find nearest enemy (human only), track Y for vertical aim
            float nearestDist = 1e9f;
            float targetX = bot.posX, targetZ = bot.posZ, targetY = bot.posY;
            bool hasTarget = false;
            for (auto& other : impl->clients) {
                if (&other == &bot || !other.active || other.isBot) continue;
                if (impl->gameMode == 1 && other.teamId == bot.teamId) continue;
                float dx = other.posX - bot.posX;
                float dz = other.posZ - bot.posZ;
                float d = dx*dx + dz*dz;
                if (d < nearestDist) { nearestDist = d; targetX = other.posX; targetZ = other.posZ; targetY = other.posY; hasTarget = true; }
            }

            if (!hasTarget) {
                // Wander toward items or random
                float nearestItem = 1e9f;
                float itemX = bot.posX, itemZ = bot.posZ;
                for (auto& sg : impl->serverGhosts) {
                    if (!sg.active || sg.itemType < 0) continue;
                    float dx = sg.posX - bot.posX, dz = sg.posZ - bot.posZ;
                    float d = dx*dx + dz*dz;
                    bool needItem = (sg.itemType == 0 && bot.health < 50) || (sg.itemType == 1 && bot.energy < 30);
                    if (needItem && d < nearestItem) { nearestItem = d; itemX = sg.posX; itemZ = sg.posZ; }
                }
                if (nearestItem < 1e8f) {
                    float dx = itemX - bot.posX, dz = itemZ - bot.posZ;
                    bot.rotZ = atan2f(dx, dz);
                } else {
                    bot.rotZ += (rand() % 200 - 100) * 0.003f;
                }
                bot.lastMove.flags = 1;
                state.strafeDir = 0;
            } else {
                // Chase enemy with strafing
                float dx = targetX - bot.posX;
                float dz = targetZ - bot.posZ;
                float dist = sqrtf(nearestDist);
                bot.rotZ = atan2f(dx, dz);
                bot.lastMove.flags = 1;
                if (dist < 60.0f) {
                    state.strafeDir = (state.strafeTimer <= 0) ? (rand() % 3 - 1) : state.strafeDir;
                    if (state.strafeTimer <= 0) state.strafeTimer = 0.5f + (rand() % 1000) / 1000.0f;
                } else {
                    state.strafeDir = 0;
                }
                if (dist < 30.0f && (rand() % 100) < 20) bot.lastMove.flags |= 2;
                if (dist < 20.0f && (rand() % 100) < 10) bot.lastMove.flags |= 4;
                // Fire with vertical aim
                if (nearestDist < 2500.0f) {
                    double now = Engine::instance().timer().now();
                    float fireInterval = (dist < 20.0f) ? 0.25f : (dist < 50.0f) ? 0.5f : 0.8f;
                    if (now - impl->botLastFire[bot.playerGhostIndex] > fireInterval) {
                        bot.lastMove.flags |= 8;
                        impl->botLastFire[bot.playerGhostIndex] = now;
                        float pitch = atan2f(targetY - bot.posY, sqrtf(dx*dx + dz*dz));
                        float dirX = sinf(bot.rotZ) * cosf(pitch);
                        float dirY = sinf(pitch);
                        float dirZ = cosf(bot.rotZ) * cosf(pitch);
                        Impl::ServerProjectile sp;
                        sp.ghostIndex = impl->nextGhostIndex++;
                        sp.posX = bot.posX + dirX * 2.0f; sp.posY = bot.posY + 1.5f; sp.posZ = bot.posZ + dirZ * 2.0f;
                        sp.velX = bot.velX + dirX * 40.0f; sp.velY = bot.velY + dirY * 40.0f; sp.velZ = bot.velZ + dirZ * 40.0f;
                        sp.ownerCi = (int)(&bot - &impl->clients[0]);
                        auto bwp = Impl::weaponProfile(0);
                        sp.damage = bwp.damage; sp.splashRadius = bwp.splash;
                        impl->projectiles.push_back(sp);
                        Impl::ServerGhost pghost;
                        pghost.index = sp.ghostIndex; pghost.classId = 33;
                        pghost.posX = sp.posX; pghost.posY = sp.posY; pghost.posZ = sp.posZ;
                        impl->serverGhosts.push_back(pghost);
                    }
                }
            }
        }

        // Velocity: combine forward + strafe
        float speed = 15.0f;
        if (bot.lastMove.flags & 4) speed = 30.0f;
        float cosYaw = cosf(bot.rotZ), sinYaw = sinf(bot.rotZ);
        float fwd = (bot.lastMove.flags & 1) ? 1.0f : 0;
        float strafe = (float)state.strafeDir * 0.6f;
        bot.velX = (fwd * sinYaw + strafe * cosYaw) * speed;
        bot.velZ = (fwd * cosYaw - strafe * sinYaw) * speed;

        // Ground clamp
        float groundY = 2.0f;
        if (heightCB) groundY = heightCB(bot.posX, bot.posZ, heightCtx);
        if (bot.posY < groundY) { bot.posY = groundY; bot.velY = 0; }
    }

    // Ghost-ghost collision: simple cylinder push-apart
    const float ghostRadius = 1.5f;
    const float ghostRadiusSq = ghostRadius * ghostRadius;
    for (size_t i = 0; i < impl->clients.size(); i++) {
        auto& ci = impl->clients[i];
        if (!ci.active || ci.playerGhostIndex == 0) continue;
        for (auto& sg : impl->serverGhosts) {
            if (sg.index == ci.playerGhostIndex) continue;
            // Only collide with player-shaped or item ghosts
            float dx = ci.posX - sg.posX;
            float dz = ci.posZ - sg.posZ;
            float distSq = dx * dx + dz * dz;
            if (distSq < ghostRadiusSq && distSq > 0.0001f) {
                float dist = sqrtf(distSq);
                float overlap = ghostRadius - dist;
                float pushX = (dx / dist) * overlap * 0.5f;
                float pushZ = (dz / dist) * overlap * 0.5f;
                ci.posX += pushX;
                ci.posZ += pushZ;
                sg.posX -= pushX;
                sg.posZ -= pushZ;
                // Ground clamp after push
                float groundY = 2.0f;
                if (heightCB) {
                    float h = heightCB(ci.posX, ci.posZ, heightCtx);
                    if (h > groundY) groundY = h;
                }
                if (ci.posY < groundY) ci.posY = groundY;
            }
        }
    }

    // Projectile simulation
    for (auto& sp : impl->projectiles) {
        if (!sp.active) continue;
        sp.posX += sp.velX * simDt;
        sp.posY += sp.velY * simDt;
        sp.posZ += sp.velZ * simDt;
        sp.velY -= 15.0f * simDt;

        Impl::ServerGhost* projGhost = nullptr;
        for (auto& sg : impl->serverGhosts)
            if (sg.index == sp.ghostIndex) { projGhost = &sg; break; }

        float groundY = 2.0f;
        if (heightCB) {
            float h = heightCB(sp.posX, sp.posZ, heightCtx);
            if (h > groundY) groundY = h;
        }
        if (sp.posY < groundY) {
            sp.active = false;
            if (projGhost) projGhost->active = false;
            for (auto& client : impl->clients) {
                if (!client.active || client.respawnAt > 0) continue;
                if (sp.ownerCi >= 0 && sp.ownerCi < (int)impl->clients.size()) {
                    auto& own = impl->clients[sp.ownerCi];
                    if (impl->gameMode == 1 && own.teamId == client.teamId && client.teamId != 0)
                        continue; // friendly fire: no splash damage
                }
                float dx = client.posX - sp.posX, dy = client.posY - sp.posY, dz = client.posZ - sp.posZ;
                float dist2 = dx*dx + dy*dy + dz*dz;
                if (dist2 < sp.splashRadius * sp.splashRadius && dist2 > 0.01f) {
                    float frac = 1.0f - sqrtf(dist2) / sp.splashRadius;
                    client.health -= sp.damage * frac;
                    float invDist = 1.0f / sqrtf(dist2);
                    client.velX += dx * invDist * frac * 10.0f;
                    client.velY += dy * invDist * frac * 10.0f;
                    client.velZ += dz * invDist * frac * 10.0f;
                }
            }
            continue;
        }
        // Hit a player? (with lag compensation: check rewind position too)
        bool impacted = false;
        for (auto& client : impl->clients) {
            if (!client.active || client.respawnAt > 0) continue;
            if (sp.ownerCi >= 0 && &client == &impl->clients[sp.ownerCi]) continue;
            // Current position check
            float dx = client.posX - sp.posX, dz = client.posZ - sp.posZ;
            bool hit = (dx*dx + dz*dz < 2.0f);
            // Rewind position check (~100ms ago for lag compensation)
            if (!hit && !client.posHistory.empty()) {
                double rewindTime = Engine::instance().timer().now() - 0.1;
                auto best = client.posHistory[0];
                for (auto& ps : client.posHistory)
                    if (ps.time <= rewindTime && ps.time > best.time) best = ps;
                float rdx = best.x - sp.posX, rdz = best.z - sp.posZ;
                hit = (rdx*rdx + rdz*rdz < 2.0f);
            }
            if (hit) {
                float dy = client.posY - sp.posY;
                if (dy > -2.0f && dy < 2.0f) {
                    sp.active = false; if (projGhost) projGhost->active = false;
                    bool friendlyFire = false;
                    if (sp.ownerCi >= 0 && sp.ownerCi < (int)impl->clients.size()) {
                        auto& killer = impl->clients[sp.ownerCi];
                        friendlyFire = (impl->gameMode == 1 && killer.teamId == client.teamId && client.teamId != 0);
                    }
                    if (!friendlyFire) {
                        client.health -= sp.damage;
                        // Track kills/deaths with game mode
                        if (sp.ownerCi >= 0 && sp.ownerCi < (int)impl->clients.size()) {
                            auto& killer = impl->clients[sp.ownerCi];
                            killer.kills++;
                            client.deaths++;
                            if (impl->gameMode == 1) {
                                impl->teamScore[killer.teamId]++;
                                if (impl->teamScore[killer.teamId] >= impl->scoreLimit) {
                                    Console::instance().printf(LogLevel::Info, "Team %d wins!", killer.teamId);
                                }
                            } else if (killer.kills >= impl->scoreLimit) {
                                Console::instance().printf(LogLevel::Info, "Player %d wins with %d kills!",
                                    sp.ownerCi, killer.kills);
                            }
                            // Kill feed (E)
                            {
                                T2Protocol::ChatMessage kf{};
                                std::string txt = killer.playerName + " fragged " + client.playerName;
                                strncpy(kf.sender, "[KILL]", sizeof(kf.sender) - 1);
                                snprintf(kf.text, sizeof(kf.text), "%s", txt.c_str());
                                uint8_t kfBuf[512];
                                if (T2Protocol::encodeChat(kfBuf, sizeof(kfBuf), kf)) {
                                    WireHeader whdr{};
                                    whdr.sequence = 1; whdr.ack = 0; whdr.ackMask = 0;
                                    whdr.type = (uint8_t)PacketType::GameData; whdr.checksum = 0;
                                    std::vector<uint8_t> pkt(sizeof(WireHeader) + 4 + strlen(kf.sender) + 2 + strlen(kf.text));
                                    memcpy(pkt.data(), &whdr, sizeof(WireHeader));
                                    memcpy(pkt.data() + sizeof(WireHeader), kfBuf, pkt.size() - sizeof(WireHeader));
                                    for (auto& c : impl->clients)
                                        if (c.active && !c.isBot) impl->sendTo(c.addr, pkt.data(), pkt.size());
                                }
                            }
                        }
                    }
                    float dist = sqrtf(dx*dx + dz*dz);
                    if (dist > 0.01f && !friendlyFire) { client.velX += (dx/dist)*8.0f; client.velZ += (dz/dist)*8.0f; client.velY += 4.0f; }
                    Console::instance().printf(LogLevel::Debug, "Projectile %u hit! health=%.0f ff=%d", (unsigned)sp.ghostIndex, client.health, (int)friendlyFire);
                    impacted = true; break;
                }
            }
        }
        if (impacted) continue;
        sp.lifetime -= simDt;
        if (sp.lifetime <= 0) { sp.active = false; if (projGhost) projGhost->active = false; }
        if (projGhost) { projGhost->posX = sp.posX; projGhost->posY = sp.posY; projGhost->posZ = sp.posZ; }
    }
    // Record frame if recording is active
    if (impl->recFile.is_open()) {
        double frameTime = Engine::instance().timer().now();
        uint32_t numGhosts = 0;
        for (auto& sg : impl->serverGhosts) if (sg.active) numGhosts++;
        impl->recFile.write((const char*)&frameTime, sizeof(frameTime));
        impl->recFile.write((const char*)&numGhosts, sizeof(numGhosts));
        for (auto& sg : impl->serverGhosts) {
            if (!sg.active) continue;
            uint32_t idx = sg.index, cid = (uint32_t)sg.classId;
            impl->recFile.write((const char*)&idx, 4);
            impl->recFile.write((const char*)&cid, 4);
            impl->recFile.write((const char*)&sg.posX, 4);
            impl->recFile.write((const char*)&sg.posY, 4);
            impl->recFile.write((const char*)&sg.posZ, 4);
            impl->recFile.write((const char*)&sg.rotX, 4);
            impl->recFile.write((const char*)&sg.rotZ, 4);
            impl->recFile.write((const char*)&sg.health, 4);
        }
    }
    // Clean up inactive projectiles (their ghost deletes are sent with the other ghosts below)
    impl->projectiles.erase(std::remove_if(impl->projectiles.begin(), impl->projectiles.end(),
        [](auto& sp) { return !sp.active; }), impl->projectiles.end());
    for (auto it = impl->serverGhosts.begin(); it != impl->serverGhosts.end(); ) {
        if (!it->active) { impl->broadcastGhostDelete(it->index, -1); it = impl->serverGhosts.erase(it); }
        else ++it;
    }

    for (size_t ci = 0; ci < impl->clients.size(); ) {
        auto& cl = impl->clients[ci];
        bool disconnected = !cl.active || (now - cl.lastReceive) > 30.0;
        if (disconnected) {
            // Clean up the player's ghost once (idempotent via playerGhostIndex reset).
            uint32_t oldG = cl.playerGhostIndex;
            if (oldG > 0) {
                impl->broadcastGhostDelete(oldG, (int)ci);
                for (auto it = impl->serverGhosts.begin(); it != impl->serverGhosts.end(); ++it) {
                    if (it->index == oldG) {
                        impl->serverGhosts.erase(it);
                        break;
                    }
                }
            }
            // Drop per-client state maps keyed by this player's ghost index.
            impl->vehUsePrev.erase(oldG);
            impl->botLastFire.erase(oldG);
            impl->botStates.erase(oldG);
            cl.playerGhostIndex = 0;
            cl.active = false; // keep the slot so other clients' stored indices stay valid
            ci++;
            continue;
        }

        // Send datablocks/ghost updates (skip bots — they have no socket)
        if (!cl.isBot) {
            impl->sendDatablocksTo((int)ci);
            impl->sendGhostsTo((int)ci);
        }

        ci++;
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
