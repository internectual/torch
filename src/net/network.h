#pragma once
#include "net/v12_protocol.h"
#include "net/v12_ghost_packet.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <map>

namespace T2Protocol {
    constexpr uint16_t DEFAULT_PORT = 28000;
    constexpr uint16_t LAN_QUERY_PORT = 28002;
}

inline bool isObserverSetupCommand(const std::vector<std::string>& argv) {
    if (argv.size() != 2) return false;
    return (argv[0] == "setPlayerTeam" && argv[1] == "0") ||
           (argv[0] == "ScopeCommanderMap" && argv[1] == "1") ||
           (argv[0] == "WatchOnly" && argv[1] == "ImaWatcher");
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

// An anonymous observer ("ImaWatcher") of a retail server over the native
// V12 protocol; it sends only the observer setup commands.
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
    void resetProtocolEpoch();
    void disconnect();
    void update();
    bool ingestObserverPacket(const uint8_t* data, size_t size);

    State state() const { return connState; }
    NetAddress address() const { return remoteAddr; }
    uint32_t ping() const { return currentPing; }
    uint64_t sentPacketCount() const;
    uint64_t receivedPacketCount() const;
    uint64_t sentByteCount() const;
    uint64_t receivedByteCount() const;
    uint64_t protocolEpoch() const { return epoch; }

    void sendNativeMove(uint32_t moveStart, const V12::ClientMove& move);
    void sendCommandPacket(const char* command);
    void sendRemoteCommand(const std::string& command,
                           const std::vector<std::string>& args);
    void sendVoiceEvent(uint8_t sequence, uint8_t codec, uint8_t stream,
                        bool endOfStream,
                        const std::vector<std::array<uint8_t, 33>>& frames);

    using CommandCallback = std::function<void(const std::string&)>;
    void setCommandCallback(CommandCallback cb) { commandCb = cb; }
    // Raw arguments and which were tags (RemoteCommandEvent).
    using ClientCommandCallback = std::function<void(const std::vector<std::string>&, const std::vector<bool>&)>;
    void setClientCommandCallback(ClientCommandCallback cb) { clientCommandCb = std::move(cb); }

    using TargetCallback = std::function<void(const V12::ServerEvent::TargetInfo*,
                                               uint16_t)>;
    void setTargetCallback(TargetCallback cb) { targetCb = std::move(cb); }

    using TargetControlCallback = std::function<void(bool, uint16_t, bool,
                                                       const V12Vec3&, bool)>;
    void setTargetControlCallback(TargetControlCallback cb) {
        targetControlCb = std::move(cb);
    }

    using AudioCallback = std::function<void(const V12::ServerEvent&)>;
    void setAudioCallback(AudioCallback cb) { audioCb = std::move(cb); }

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
        bool hasCameraFov = false;
        uint8_t cameraFov = 0;
        uint16_t controlGhost = 0;
        bool controlAssigned = false;
        uint32_t lastMoveAck = 0;
        uint8_t playerSensorGroup = 0;
        bool matchStarted = false;
        bool matchEnded = false;
        bool ghosting = false;
        uint32_t clockRemainingMs = 0;
        std::vector<std::string> loadInfoLines;
        V12::ProtocolStateSnapshot protocol;
        std::vector<std::pair<uint16_t, std::string>> strings;
        std::map<uint16_t, std::string> datablockShapes;
        std::map<uint16_t, V12::DecodedDataBlock> datablocks;
        std::vector<std::pair<uint16_t, V12::PlayerGhostState>> players;
        std::vector<std::pair<uint16_t, uint16_t>> ghostClasses;
        std::vector<V12::ServerEvent::TargetInfo> targets;
        std::vector<TeamState> teams;
        std::map<int, int> playerScores;
        std::map<int, int> playerKills;
        std::map<int, int> playerDeaths;
        std::map<int, int> playerPings;
        std::map<int, int> playerPacketLoss;
        std::map<int, int> clientTargets;
        std::map<int, int> clientTeams;
         std::map<int, std::string> clientNames;
        std::map<std::pair<int, uint32_t>, uint32_t> sensorGroupColors;
        std::array<uint32_t, 16> targetVisible{}; // VisibleToSensor, one bit per target
     };
    ObserverSnapshot observerSnapshot() const;
    bool seedObserverSnapshot(const ObserverSnapshot& snapshot);

    using GhostCallback = std::function<void(
        const V12::GhostUpdate&, const V12::PlayerGhostState*)>;
    void setGhostCallback(GhostCallback cb) { ghostCb = cb; }

    using ProjectileImpactCallback = std::function<void(
        uint16_t ghostIndex, uint16_t classId, const V12::ProjectileImpact&)>;
    void setProjectileImpactCallback(ProjectileImpactCallback cb) {
        projectileImpactCb = std::move(cb);
    }

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
    CommandCallback commandCb;
    ClientCommandCallback clientCommandCb;
    TargetCallback targetCb;
    TargetControlCallback targetControlCb;
    AudioCallback audioCb;
    MissionCallback missionCb;
    ServerMessageCallback serverMessageCb;
    GhostCallback ghostCb;
    ProjectileImpactCallback projectileImpactCb;
    DatablockCallback datablockCb;
    StateCallback stateCb;
    std::function<void(uint64_t)> epochCb;
    std::function<void(bool)> connectCb;
    uint64_t epoch = 0;
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
