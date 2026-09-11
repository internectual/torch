#pragma once
#include "net/v12_protocol.h"
#include "net/v12_ghost_packet.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <map>

enum class PacketType : uint8_t {
    Connect = 0x01,
    ConnectOK = 0x02,
    ConnectReject = 0x03,
    Challenge = 0x04,
    ChallengeResponse = 0x05,
    GameData = 0x06,
    GameDataReliable = 0x07,
    Disconnect = 0x08,
    Ping = 0x09,
    Pong = 0x0A,
    Ack = 0x0B,
    QueryServers = 0x0C,
    QueryResponse = 0x0D,
};

inline bool isObserverSetupCommand(const std::vector<std::string>& argv) {
    if (argv.size() != 2) return false;
    return (argv[0] == "setPlayerTeam" && argv[1] == "0") ||
           (argv[0] == "ScopeCommanderMap" && argv[1] == "1") ||
           (argv[0] == "WatchOnly" && argv[1] == "ImaWatcher");
}

#pragma pack(push, 1)
struct WireHeader {
    uint32_t sequence{};
    uint32_t ack{};
    uint32_t ackMask{};
    uint8_t type{};
    uint16_t checksum{};
};
#pragma pack(pop)
static_assert(sizeof(WireHeader) == 15, "WireHeader must use the 15-byte wire layout");

inline void encodeWireHeader(uint8_t* out, const WireHeader& header) {
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(header.sequence >> (i * 8));
    for (int i = 0; i < 4; ++i) out[4 + i] = (uint8_t)(header.ack >> (i * 8));
    for (int i = 0; i < 4; ++i) out[8 + i] = (uint8_t)(header.ackMask >> (i * 8));
    out[12] = header.type;
    out[13] = (uint8_t)(header.checksum & 0xff);
    out[14] = (uint8_t)(header.checksum >> 8);
}

inline WireHeader decodeWireHeader(const uint8_t* data) {
    WireHeader header{};
    for (int i = 0; i < 4; ++i) header.sequence |= (uint32_t)data[i] << (i * 8);
    for (int i = 0; i < 4; ++i) header.ack |= (uint32_t)data[4 + i] << (i * 8);
    for (int i = 0; i < 4; ++i) header.ackMask |= (uint32_t)data[8 + i] << (i * 8);
    header.type = data[12];
    header.checksum = (uint16_t)data[13] | ((uint16_t)data[14] << 8);
    return header;
}

struct NetAddress {
    uint32_t ip{};
    uint16_t port{};

    bool operator==(const NetAddress& o) const {
        return ip == o.ip && port == o.port;
    }

    std::string toString() const {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d:%d",
            (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
            (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port);
        return buf;
    }
};

class Connection {
public:
    enum State {
        Disconnected,
        Connecting,
        Challenging,
        Connected,
        Game
    };

    Connection();
    ~Connection();

    bool connect(const char* host, uint16_t port);
    void setObserverMode(bool observer) { observerMode = observer; }
    bool isObserverMode() const { return observerMode; }
    void resetProtocolEpoch();
    void setPlayerName(const char* name) { playerName = name ? name : "Observer"; }
    void setJoinPassword(const char* password) { joinPassword = password ? password : ""; }
    void disconnect();
    void update();
    bool ingestObserverPacket(const uint8_t* data, size_t size);

    State state() const { return connState; }
    void setState(State s) { connState = s; }
    NetAddress address() const { return remoteAddr; }
    uint32_t ping() const { return currentPing; }
    uint64_t sentPacketCount() const;
    uint64_t receivedPacketCount() const;
    uint64_t sentByteCount() const;
    uint64_t receivedByteCount() const;
    uint64_t protocolEpoch() const { return epoch; }

    void sendPacket(PacketType type, const uint8_t* data, size_t size);
    void sendGamePacket(const uint8_t* data, size_t size, bool reliable = false);
    void sendNativeMove(uint32_t moveStart, const V12::ClientMove& move);
    void sendCommandPacket(const char* command);

    using PacketCallback = std::function<void(PacketType type, const uint8_t* data, size_t size)>;
    void setPacketCallback(PacketCallback cb) { packetCb = cb; }

    using CommandCallback = std::function<void(const std::string&)>;
    void setCommandCallback(CommandCallback cb) { commandCb = cb; }
    using ClientCommandCallback = std::function<void(const std::vector<std::string>&)>;
    void setClientCommandCallback(ClientCommandCallback cb) { clientCommandCb = std::move(cb); }

    using TargetCallback = std::function<void(const V12::ServerEvent::TargetInfo*,
                                              uint16_t)>;
    void setTargetCallback(TargetCallback cb) { targetCb = std::move(cb); }

    using MissionCallback = std::function<void(uint32_t)>;
    void setMissionCallback(MissionCallback cb) { missionCb = std::move(cb); }

    using ServerMessageCallback = std::function<void(const std::vector<std::string>&)>;
    void setServerMessageCallback(ServerMessageCallback cb) { serverMessageCb = std::move(cb); }

    struct ObserverSnapshot {
        struct TeamState {
            int teamId = 0;
            std::string name;
            int score = 0;
            std::string flagStatus = "home";
            std::string flagCarrier;
        };
        uint64_t epoch = 0;
        uint32_t missionCrc = 0;
        V12Vec3 compressionPoint{};
        uint16_t controlGhost = 0;
        uint32_t lastMoveAck = 0;
        uint8_t playerSensorGroup = 0;
        bool matchStarted = false;
        bool matchEnded = false;
        uint32_t clockRemainingMs = 0;
        std::vector<std::string> loadInfoLines;
        V12::ProtocolStateSnapshot protocol;
        std::vector<std::pair<uint16_t, std::string>> strings;
        std::map<uint16_t, std::string> datablockShapes;
        std::vector<std::pair<uint16_t, V12::PlayerGhostState>> players;
        std::vector<std::pair<uint16_t, uint16_t>> ghostClasses;
        std::vector<V12::ServerEvent::TargetInfo> targets;
        std::vector<TeamState> teams;
        std::map<int, int> playerScores;
        std::map<int, int> playerPings;
        std::map<int, int> playerPacketLoss;
        std::map<int, int> clientTargets;
        std::map<int, int> clientTeams;
        std::map<int, std::string> clientNames;
    };
    ObserverSnapshot observerSnapshot() const;
    bool seedObserverSnapshot(const ObserverSnapshot& snapshot);

    using GhostCallback = std::function<void(
        const V12::GhostUpdate&, const V12::PlayerGhostState*)>;
    void setGhostCallback(GhostCallback cb) { ghostCb = cb; }

    using DatablockCallback = std::function<void(
        uint16_t objectId, uint8_t classId, uint16_t index, uint16_t total,
        const std::string& className,
        const V12::DecodedDataBlock& data)>;
    void setDatablockCallback(DatablockCallback cb) { datablockCb = std::move(cb); }

    using StateCallback = std::function<void(const V12::ServerGameState&)>;
    void setStateCallback(StateCallback cb) { stateCb = std::move(cb); }
    void setEpochCallback(std::function<void(uint64_t)> cb) { epochCb = std::move(cb); }

    void setConnectCallback(std::function<void(bool)> cb) { connectCb = cb; }

    bool isConnected() const { return connState >= Connected; }

private:
    struct Impl;
    Impl* impl;
    State connState = Disconnected;
    NetAddress remoteAddr;
    uint32_t currentPing = 0;
    PacketCallback packetCb;
    CommandCallback commandCb;
    ClientCommandCallback clientCommandCb;
    TargetCallback targetCb;
    MissionCallback missionCb;
    ServerMessageCallback serverMessageCb;
    GhostCallback ghostCb;
    DatablockCallback datablockCb;
    StateCallback stateCb;
    std::function<void(uint64_t)> epochCb;
    std::function<void(bool)> connectCb;
    std::string playerName = "Observer";
    std::string joinPassword;
    uint64_t epoch = 0;
    bool observerMode = false;
};

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    bool init();
    void shutdown();
    void update();

    Connection* createConnection();
    void destroyConnection(Connection* conn);

    // Server browser
    struct ServerInfo {
        NetAddress addr;
        std::string name;
        std::string map;
        std::string gameType;
        int32_t numPlayers{};
        int32_t maxPlayers{};
        int32_t ping{};
        int32_t numBots{};
        bool password{};
        bool tournament{};
    };

    void queryLanServers();
    void queryMasterServer(const char* masterUrl);
    void querySingleServer(const char* address);
    void stopServerQuery();
    bool isServerQueryActive() const;

    std::vector<ServerInfo> getServerList() const { return servers; }

    void setServerListCallback(std::function<void()> cb) { serverListCb = cb; }

private:
    struct Impl;
    Impl* impl;
    std::vector<ServerInfo> servers;
    std::function<void()> serverListCb;
};
