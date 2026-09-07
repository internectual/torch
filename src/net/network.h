#pragma once
#include "net/v12_protocol.h"
#include "net/v12_ghost_packet.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <utility>

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
    void setPlayerName(const char* name) { playerName = name ? name : "Observer"; }
    void setJoinPassword(const char* password) { joinPassword = password ? password : ""; }
    void disconnect();
    void update();

    State state() const { return connState; }
    void setState(State s) { connState = s; }
    NetAddress address() const { return remoteAddr; }
    uint32_t ping() const { return currentPing; }

    void sendPacket(PacketType type, const uint8_t* data, size_t size);
    void sendGamePacket(const uint8_t* data, size_t size, bool reliable = false);
    void sendNativeMove(uint32_t moveStart, const V12::ClientMove& move);
    void sendCommandPacket(const char* command);

    using PacketCallback = std::function<void(PacketType type, const uint8_t* data, size_t size)>;
    void setPacketCallback(PacketCallback cb) { packetCb = cb; }

    using CommandCallback = std::function<void(const std::string&)>;
    void setCommandCallback(CommandCallback cb) { commandCb = cb; }

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
    GhostCallback ghostCb;
    DatablockCallback datablockCb;
    StateCallback stateCb;
    std::function<void(bool)> connectCb;
    std::string playerName = "Observer";
    std::string joinPassword;
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
        bool password{};
    };

    void queryLanServers();
    void queryMasterServer(const char* masterUrl);

    std::vector<ServerInfo> getServerList() const { return servers; }

    void setServerListCallback(std::function<void()> cb) { serverListCb = cb; }

private:
    struct Impl;
    Impl* impl;
    std::vector<ServerInfo> servers;
    std::function<void()> serverListCb;
};
