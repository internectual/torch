// game/netDispatch.cc, sim/netConnection.cc (checkTimeout / keepAlive),
// game/gameConnection.cc (onRemove, setDisconnectReason), game/serverQuery.cc
// (handleGamePingRequest / handleGameInfoRequest) and platformLinux/linuxNet.cc
// (openPort, stringToAddress, addressToString), as the retail protocol 0x33
// engine runs them.
#include "sim/net_interface.h"
#include "sim/game_connection.h"
#include "sim/sim_state.h"
#include "net/v12_bitstream.h"
#include "net/v12_protocol.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include "core/timer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <strings.h>
#include <vector>

namespace {

enum : uint32_t {
    MaxConnectArgs = 16,
    MaxPendingConnects = 20,
    PendingConnectTimeout = 7000,
    ChallengeRetryCount = 4,
    ChallengeRetryTime = 2500,
    ConnectRetryCount = 4,
    ConnectRetryTime = 2500,
    TimeoutCheckInterval = 1500,
    MaxAuthInfoSize = 1024,
    MaxPasswordLength = 16,
    // netConnection.cc
    PingTimeout = 4500,
    DefaultPingRetryCount = 15,
};
constexpr uint32_t CurrentProtocolVersion = V12::ProtocolVersion;
constexpr uint32_t MinRequiredProtocolVersion = V12::ProtocolVersion;
constexpr uint16_t DefaultPort = 28000; // linuxNet.cc defaultPort
constexpr uint32_t BuildVersion = 25034; // getVersionNumber()
const char* const VersionString = "VER5";

// NetAddress (IP only).
struct Address {
    uint32_t ip = 0; // network order
    uint16_t port = 0;
    bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
};

Address fromSockaddr(const sockaddr_in& sa) { return {sa.sin_addr.s_addr, ntohs(sa.sin_port)}; }

sockaddr_in toSockaddr(const Address& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = a.ip;
    sa.sin_port = htons(a.port);
    return sa;
}

// Net::addressToString.
std::string addressToString(const Address& a) {
    if (a.ip == htonl(INADDR_BROADCAST)) return "IP:Broadcast:" + std::to_string(a.port);
    const uint32_t ip = a.ip;
    char text[64];
    std::snprintf(text, sizeof text, "IP:%u.%u.%u.%u:%u", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 24) & 0xff, (unsigned)a.port);
    return text;
}

// Net::stringToAddress: "IP:" optional, "host[:port]", the port 28000 when
// left out; "broadcast"; a dotted address or a host name.
bool stringToAddress(std::string text, Address& out) {
    if (text.size() >= 3 && strncasecmp(text.c_str(), "ip:", 3) == 0) text.erase(0, 3);
    if (text.size() >= 4 && strncasecmp(text.c_str(), "ipx:", 4) == 0) return false;
    if (text.size() > 255) return false;
    std::string host = text, portString;
    bool hasPort = false;
    if (const size_t colon = text.find(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        portString = text.substr(colon + 1);
        hasPort = true;
    }
    Address address;
    if (strcasecmp(host.c_str(), "broadcast") == 0) {
        address.ip = htonl(INADDR_BROADCAST);
    } else {
        in_addr parsed{};
        if (inet_aton(host.c_str(), &parsed)) {
            address.ip = parsed.s_addr;
        } else {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return false;
            address.ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(result);
        }
    }
    address.port = hasPort ? (uint16_t)std::atoi(portString.c_str()) : DefaultPort;
    out = address;
    return true;
}

// ---------------------------------------------------------------------------
// Sockets. The server's port is the one setNetPort opens (Net::openPort);
// a client's connection to a server sends from a socket of its own.

int gPortSocket = -1;
int gClientSocket = -1;

int openUdpSocket(int port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)port);
    int size = 32768, on = 1;
    bool ok = bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0;
    ok = ok && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0;
    ok = ok && setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) == 0;
    ok = ok && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0;
    if (!ok) {
        close(fd);
        return -1;
    }
    return fd;
}

void sendPacket(int fd, const Address& to, const std::vector<uint8_t>& packet) {
    if (fd < 0 || packet.empty()) return;
    const sockaddr_in sa = toSockaddr(to);
    ::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
}

void sendFromPort(const Address& to, const std::vector<uint8_t>& packet) { sendPacket(gPortSocket, to, packet); }
void sendFromClient(const Address& to, const std::vector<uint8_t>& packet) { sendPacket(gClientSocket, to, packet); }

uint32_t virtualMilliseconds() { return (uint32_t)(uint64_t)(Timer::now() * 1000.0); }

TorqueScript* script() { return ScriptEngine::exists() ? ScriptEngine::instance().ts() : nullptr; }

std::string global(const char* name) {
    TorqueScript* ts = script();
    return ts ? ts->getGlobal(name).toString() : std::string();
}
int globalInt(const char* name) {
    TorqueScript* ts = script();
    return ts ? ts->getGlobal(name).toInt() : 0;
}
bool globalBool(const char* name) {
    TorqueScript* ts = script();
    return ts ? ts->getGlobal(name).toBool() : false;
}

// Con::executef on a global function.
void execute(const std::string& function, const std::vector<VMValue>& args = {}) {
    TorqueScript* ts = script();
    if (ts && ts->isFunction(function)) ts->callFunction(function, args);
}

// Authentication removed (V12): Torch has no WON login, so it is never an
// online server or client.
bool isServerOnline() { return false; }

// ---------------------------------------------------------------------------
// Network connections (NetConnection's address table).

struct NetLink {
    std::shared_ptr<EngineObject> engine; // keeps the GameConnection's state alive
    GameConnection* connection = nullptr;
    std::string key;
    Address address;
    uint32_t clientConnectSequence = 0;
    uint32_t serverConnectSequence = 0;
    bool serverConnection = false; // NetConnection::isServerConnection: the client's link to its server
    uint32_t lastPingSendTime = 0;
    uint32_t pingSendCount = 0;
    uint32_t pingRetryCount = DefaultPingRetryCount;
};
std::vector<NetLink> gLinks;
// The client's last ServerConnection, kept past its deletion: the game's
// live client holds it until the next connection replaces it.
std::shared_ptr<EngineObject> gRetiredServerConnection;

NetLink* findLink(const Address& address, bool serverConnection) {
    for (auto& link : gLinks)
        if (link.serverConnection == serverConnection && link.address == address) return &link;
    return nullptr;
}

NetLink* findLink(const GameConnection* connection) {
    for (auto& link : gLinks)
        if (link.connection == connection) return &link;
    return nullptr;
}

void eraseLink(const GameConnection* connection) {
    gLinks.erase(std::remove_if(gLinks.begin(), gLinks.end(),
                                [&](const NetLink& l) { return l.connection == connection; }),
                 gLinks.end());
}

// The script object still carrying this connection's state.
ScriptObject* linkObject(const NetLink& link) {
    if (!ScriptEngine::exists()) return nullptr;
    auto& engine = ScriptEngine::instance();
    auto it = engine.objects.find(link.key);
    if (it == engine.objects.end() || !it->second || it->second->engine.get() != link.connection) return nullptr;
    return it->second;
}

void sendDisconnect(const NetLink& link, const std::string& reason) {
    Console::instance().printf(LogLevel::Info, "Issuing Disconnect packet.");
    const auto packet = V12::buildDisconnectPacket(link.serverConnectSequence, link.clientConnectSequence, reason);
    if (link.serverConnection) sendFromClient(link.address, packet);
    else sendFromPort(link.address, packet);
}

void retireLink(const NetLink& link) {
    if (link.serverConnection) {
        link.connection->deliver = nullptr;
        gRetiredServerConnection = link.engine;
    }
    eraseLink(link.connection);
}

std::string disconnectReason(ScriptObject* object) {
    auto it = object->internals.find("__disconnectReason");
    return it == object->internals.end() ? std::string() : it->second.toString();
}

// SimObject::deleteObject on a GameConnection: GameConnection::onRemove
// issues the Disconnect packet of a network connection and calls onDrop on
// a client's connection on the server, then the object goes.
void deleteConnection(ScriptObject* object) {
    if (!object || object->internals["__deleting"].toBool()) return;
    auto& engine = ScriptEngine::instance();
    const std::string key = engine.objectKey(object);
    auto* connection = dynamic_cast<GameConnection*>(object->engine.get());
    if (connection) {
        const std::string reason = disconnectReason(object);
        std::shared_ptr<EngineObject> keep = object->engine;
        if (NetLink* link = findLink(connection)) {
            sendDisconnect(*link, reason);
            const NetLink copy = *link;
            retireLink(copy);
        }
        if (connection->isServer)
            if (TorqueScript* ts = script()) ts->callObjectMethod(key, "onDrop", {VMValue(reason)});
    }
    if (engine.findObject(key.c_str()) == object) engine.deleteScriptObject(key);
}

// ---------------------------------------------------------------------------
// Connect request and server response structures.

struct ConnectRequest {
    enum State { NotConnecting, Challenging, Connecting } state = NotConnecting;
    uint32_t clientConnectSequence = 0;
    uint32_t serverConnectSequence = 0;
    uint32_t protocolVersion = 0;
    uint32_t lastSendTime = 0;
    uint32_t sendCount = 0;
    Address serverAddress;
    bool authenticated = false;
    std::string password;
    std::vector<std::string> argv;
} gConnectRequest;

struct PendingConnect {
    Address sourceAddress;
    uint32_t clientProtocolVersion = 0;
    uint32_t clientConnectSequence = 0;
    uint32_t serverConnectSequence = 0;
    uint32_t lastRecvTime = 0;
};
std::vector<PendingConnect> gPendingConnects;

bool ensureClientSocket() {
    if (gClientSocket < 0) gClientSocket = openUdpSocket(0);
    return gClientSocket >= 0;
}

void sendConnectChallengeRequest() {
    gConnectRequest.sendCount++;
    gConnectRequest.lastSendTime = virtualMilliseconds();
    Console::instance().printf(LogLevel::Info, "Connect Challenge Request: %u", gConnectRequest.sendCount);
    ensureClientSocket();
    sendFromClient(gConnectRequest.serverAddress,
                   V12::buildConnectChallengeRequest(CurrentProtocolVersion, gConnectRequest.clientConnectSequence,
                                                     gConnectRequest.password));
}

void sendConnectChallengeResponse(PendingConnect& p) {
    p.lastRecvTime = virtualMilliseconds();
    Console::instance().printf(LogLevel::Info, "Sent challenge response");
    sendFromPort(p.sourceAddress, V12::buildConnectChallengeResponse(p.serverConnectSequence, p.clientConnectSequence,
                                                                     CurrentProtocolVersion, false));
}

void rejectConnectChallengeRequest(const Address& source, uint32_t clientConnectSequence, const std::string& reason) {
    sendFromPort(source, V12::buildConnectChallengeReject(clientConnectSequence, reason));
}

void handleConnectChallengeRequest(const Address& source, V12BitStream& stream) {
    if (!SimState::server().allowConnections) return;
    uint32_t protoVersion = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    if (stream.failed()) return;
    if (protoVersion < MinRequiredProtocolVersion) {
        rejectConnectChallengeRequest(source, clientConnectSequence, "CHR_PROTOCOL");
        return;
    }
    if (protoVersion > CurrentProtocolVersion) protoVersion = CurrentProtocolVersion;

    // First time out any outstanding challenge requests.
    const uint32_t time = virtualMilliseconds();
    for (size_t i = 0; i < gPendingConnects.size();) {
        PendingConnect& p = gPendingConnects[i];
        if (p.sourceAddress == source && p.clientConnectSequence == clientConnectSequence) {
            sendConnectChallengeResponse(p); // just up the time and ping back
            return;
        }
        if (time > p.lastRecvTime + PendingConnectTimeout) {
            p = gPendingConnects.back();
            gPendingConnects.pop_back();
        } else {
            ++i;
        }
    }

    // Retail: disableCyclingConnections(true) while the mission cycles.
    if (SimState::server().cyclingConnectionsDisabled) {
        rejectConnectChallengeRequest(source, clientConnectSequence,
                                      "Server is cycling missions.  Please try to connect in a moment.");
        return;
    }

    // Test the password.
    const std::string joinPassword = stream.readHuffmanString();
    const std::string password = global("$Host::Password");
    if (!password.empty() &&
        std::strncmp(password.c_str(), joinPassword.c_str(), MaxPasswordLength) != 0) {
        // IMPORTANT! Do NOT change the message here, it is used by the game.
        rejectConnectChallengeRequest(source, clientConnectSequence, "PASSWORD");
        return;
    }

    if (stream.readFlag() && !stream.failed()) {
        const uint32_t size = stream.readUnsigned(16);
        if (size > MaxAuthInfoSize) {
            rejectConnectChallengeRequest(source, clientConnectSequence, "CHR_INVALID_CHALLENGE_PACKET");
            return;
        }
        // (WON certificate data: no authentication.)
    }

    size_t index;
    if (gPendingConnects.size() == MaxPendingConnects) {
        static std::mt19937 random{std::random_device{}()};
        index = std::uniform_int_distribution<size_t>(0, MaxPendingConnects - 1)(random);
    } else {
        index = gPendingConnects.size();
        gPendingConnects.emplace_back();
    }
    PendingConnect& p = gPendingConnects[index];
    p.clientConnectSequence = clientConnectSequence;
    p.sourceAddress = source;
    p.clientProtocolVersion = protoVersion;
    p.serverConnectSequence = virtualMilliseconds();
    Console::instance().printf(LogLevel::Info, "Got challenge request");
    sendConnectChallengeResponse(p);
}

void sendConnectRequest() {
    gConnectRequest.sendCount++;
    gConnectRequest.lastSendTime = virtualMilliseconds();
    Console::instance().printf(LogLevel::Info, "Sent connect request");
    sendFromClient(gConnectRequest.serverAddress,
                   V12::buildConnectRequest(gConnectRequest.serverConnectSequence,
                                            gConnectRequest.clientConnectSequence, gConnectRequest.protocolVersion,
                                            gConnectRequest.authenticated, gConnectRequest.argv));
}

void challengeRequestRejected(const std::string& reason) { execute("onChallengeRequestRejected", {VMValue(reason)}); }

void handleConnectChallengeResponse(const Address& address, V12BitStream& stream) {
    if (gConnectRequest.state != ConnectRequest::Challenging) return;
    if (!(gConnectRequest.serverAddress == address)) return;
    const uint32_t serverProtocol = stream.readU32();
    const uint32_t serverConnectSequence = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    if (stream.failed()) return;
    // Retail: an older server's protocol is refused here.
    if (serverProtocol < MinRequiredProtocolVersion) {
        gConnectRequest.state = ConnectRequest::NotConnecting;
        challengeRequestRejected("CHR_PROTOCOL_SERVER");
        return;
    }
    // Must be an old connect request...
    if (clientConnectSequence != gConnectRequest.clientConnectSequence) return;

    gConnectRequest.authenticated = stream.readFlag() && !stream.failed();
    if (gConnectRequest.authenticated) {
        // Retail: an authenticated (online) server wants a logged-in client.
        gConnectRequest.state = ConnectRequest::NotConnecting;
        challengeRequestRejected(isServerOnline() ? "CHR_INVALID_SERVER_PACKET" : "CHR_NOT_AUTHENTICATED");
        return;
    }

    gConnectRequest.state = ConnectRequest::Connecting;
    gConnectRequest.serverConnectSequence = serverConnectSequence;
    gConnectRequest.sendCount = 0;
    gConnectRequest.protocolVersion = std::min(serverProtocol, CurrentProtocolVersion);
    Console::instance().printf(LogLevel::Info, "Got challenge response");
    sendConnectRequest();
}

void sendConnectAccept(const NetLink& link) {
    auto& engine = ScriptEngine::instance();
    ScriptObject* object = linkObject(link);
    const uint32_t id = object ? (uint32_t)engine.objectId(object) : 0;
    Console::instance().printf(LogLevel::Info, "Sending connect accept: %u", id);
    sendFromPort(link.address, V12::buildConnectAccept(link.serverConnectSequence, link.clientConnectSequence,
                                                       CurrentProtocolVersion, id));
}

void handleConnectRequest(const Address& address, V12BitStream& stream) {
    if (!SimState::server().allowConnections) return;
    const std::string addressString = addressToString(address);
    Console::instance().printf(LogLevel::Info, "Connection request from: %s", addressString.c_str());

    const uint32_t serverConnectSequence = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    const uint32_t protocolRequestVersion = stream.readU32();
    if (stream.failed()) return;

    std::string rejectString;
    if (protocolRequestVersion < MinRequiredProtocolVersion || protocolRequestVersion > CurrentProtocolVersion)
        rejectString = "CR_INVALID_PROTOCOL_VERSION";

    size_t i = 0;
    for (; i < gPendingConnects.size(); ++i) {
        const PendingConnect& p = gPendingConnects[i];
        if (p.sourceAddress == address && p.clientConnectSequence == clientConnectSequence &&
            p.serverConnectSequence == serverConnectSequence)
            break;
    }
    if (i == gPendingConnects.size()) {
        // If it's not in the pending list, check if we're already connected
        // to this guy.
        if (NetLink* link = findLink(address, false))
            if (link->clientConnectSequence == clientConnectSequence &&
                link->serverConnectSequence == serverConnectSequence)
                sendConnectAccept(*link); // we already accepted this connection
        return;
    }

    // Check the peer auth (no authentication).
    if (stream.readFlag()) rejectString = "CR_INVALID_CONNECT_PACKET";

    // gBanList.isBanned(0, addr): Torch keeps no ban list.
    if (rejectString.empty() && globalInt("$HostGamePlayerCount") >= globalInt("$Host::MaxPlayers"))
        rejectString = "CR_SERVERFULL";

    // Erase the request from the pending list.
    gPendingConnects[i] = gPendingConnects.back();
    gPendingConnects.pop_back();

    std::vector<std::string> argv;
    const uint32_t argc = stream.readU32();
    if (argc > MaxConnectArgs) {
        rejectString = "CR_INVALID_CONNECT_PACKET";
    } else {
        for (uint32_t a = 0; a < argc; ++a) argv.push_back(stream.readHuffmanString());
    }

    if (!rejectString.empty()) {
        sendFromPort(address, V12::buildConnectReject(serverConnectSequence, clientConnectSequence, rejectString));
        return;
    }

    Console::instance().printf(LogLevel::Info, "Accepting connect... CLIENT ID = %d", 0);
    // Let this guy into the game.
    auto& engine = ScriptEngine::instance();
    engine.ensureEngineGroups();
    ScriptObject* object = engine.createEngineObject("GameConnection", "");
    auto* connection = object ? dynamic_cast<GameConnection*>(object->engine.get()) : nullptr;
    if (!connection) return;
    connection->isServer = true;
    connection->local = false;
    connection->address = addressString;
    connection->setConnectSequence(clientConnectSequence ^ serverConnectSequence);
    connection->deliver = [address](const std::vector<uint8_t>& packet) { sendFromPort(address, packet); };
    NetLink link;
    link.engine = object->engine;
    link.connection = connection;
    link.key = engine.objectKey(object);
    link.address = address;
    link.clientConnectSequence = clientConnectSequence;
    link.serverConnectSequence = serverConnectSequence;
    link.serverConnection = false;
    link.lastPingSendTime = virtualMilliseconds();
    gLinks.push_back(link);
    sendConnectAccept(link);

    // GameClientAdded: into ClientGroup, then onConnect(args...).
    if (ScriptObject* group = engine.findObject("ClientGroup")) engine.addToSet(group, object);
    std::vector<VMValue> connectArgs;
    for (const auto& arg : argv) connectArgs.emplace_back(arg);
    if (TorqueScript* ts = script()) ts->callObjectMethod(link.key, "onConnect", connectArgs);
}

void handleConnectAccept(const Address& address, V12BitStream& stream) {
    if (gConnectRequest.state != ConnectRequest::Connecting) return;
    if (!(gConnectRequest.serverAddress == address)) return;
    const uint32_t serverConnectSequence = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    const uint32_t protocol = stream.readU32();
    const uint32_t id = stream.readU32();
    if (stream.failed()) return;
    if (serverConnectSequence != gConnectRequest.serverConnectSequence ||
        clientConnectSequence != gConnectRequest.clientConnectSequence || protocol > CurrentProtocolVersion ||
        protocol < MinRequiredProtocolVersion)
        return;

    // OK, we can connect up now.
    gConnectRequest.state = ConnectRequest::NotConnecting;
    auto& engine = ScriptEngine::instance();
    engine.ensureEngineGroups();
    ScriptObject* object = engine.createEngineObject("GameConnection", "ServerConnection");
    auto* connection = object ? dynamic_cast<GameConnection*>(object->engine.get()) : nullptr;
    if (!connection) return;
    connection->isServer = false;
    connection->local = false;
    connection->address = addressToString(address);
    connection->setConnectSequence(clientConnectSequence ^ serverConnectSequence);
    connection->deliver = [address](const std::vector<uint8_t>& packet) { sendFromClient(address, packet); };
    NetLink link;
    link.engine = object->engine;
    link.connection = connection;
    link.key = engine.objectKey(object);
    link.address = address;
    link.clientConnectSequence = clientConnectSequence;
    link.serverConnectSequence = serverConnectSequence;
    link.serverConnection = true;
    link.lastPingSendTime = virtualMilliseconds();
    gLinks.push_back(link);
    if (gLocalClientStarted) gLocalClientStarted(*connection);
    gRetiredServerConnection.reset();
    execute("ServerConnectionAccepted");
    Console::instance().printf(LogLevel::Info, "Connection accepted - id %u  protocol %u", id, protocol);
}

void handleConnectReject(const Address& address, V12BitStream& stream) {
    if (gConnectRequest.state != ConnectRequest::Connecting) return;
    const uint32_t serverConnectSequence = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    if (stream.failed() || !(address == gConnectRequest.serverAddress) ||
        clientConnectSequence != gConnectRequest.clientConnectSequence ||
        serverConnectSequence != gConnectRequest.serverConnectSequence)
        return;
    const std::string reason = stream.readHuffmanString();
    gConnectRequest.state = ConnectRequest::NotConnecting;
    execute("onConnectRequestRejected", {VMValue(reason)});
}

void handleConnectChallengeReject(const Address& address, V12BitStream& stream) {
    if (gConnectRequest.state != ConnectRequest::Challenging) return;
    const uint32_t clientConnectSequence = stream.readU32();
    if (stream.failed() || !(address == gConnectRequest.serverAddress) ||
        clientConnectSequence != gConnectRequest.clientConnectSequence)
        return;
    const std::string reason = stream.readHuffmanString();
    gConnectRequest.state = ConnectRequest::NotConnecting;
    challengeRequestRejected(reason);
}

void handleDisconnect(const Address& address, V12BitStream& stream, bool serverConnection) {
    Console::instance().printf(LogLevel::Info, "Got disconnect packet.");
    NetLink* link = findLink(address, serverConnection);
    const uint32_t serverConnectSequence = stream.readU32();
    const uint32_t clientConnectSequence = stream.readU32();
    const std::string reason = stream.readHuffmanString();
    if (!link || stream.failed()) return;
    if (link->clientConnectSequence != clientConnectSequence ||
        link->serverConnectSequence != serverConnectSequence)
        return;
    ScriptObject* object = linkObject(*link);
    if (!object) return;
    // OK, it's gone.
    if (link->serverConnection) {
        Console::instance().printf(LogLevel::Info, "Connection with server lost.");
        execute("onConnectionToServerLost", {VMValue(reason)});
    } else {
        Console::instance().printf(LogLevel::Info, "Client %d disconnected.",
                                   ScriptEngine::instance().objectId(object));
        object->internals["__disconnectReason"] = VMValue(reason);
    }
    // The callback may have deleted it already.
    if (NetLink* again = findLink(address, serverConnection); again && (object = linkObject(*again)))
        deleteConnection(object);
}

// handleInfoPacket: the queries a server answers.
void handleGamePingRequest(const Address& address, uint32_t key, uint8_t flags) {
    // Do not respond if a mission is not running.
    if (!SimState::server().allowConnections) return;
    // Do not respond if this is a single-player game.
    if (strcasecmp(global("$HostGameType").c_str(), "SinglePlayer") == 0) return;
    // Do not respond to offline queries if this is an online server.
    if (isServerOnline() && (flags & V12::PingOfflineQuery)) return;
    sendFromPort(address, V12::buildGamePingResponse(flags, key, VersionString, CurrentProtocolVersion,
                                                     MinRequiredProtocolVersion, BuildVersion,
                                                     global("$ServerName")));
}

// Retail status byte: Dedicated 1, Passworded 2, Linux 4, Tournament 8,
// NoSmurfs 16, TeamDamageOn 32, HiVisibility 128.
uint8_t serverStatus() {
    uint8_t status = 0;
    if (globalBool("$Host::Dedicated")) status |= 0x01;
    if (!global("$Host::Password").empty()) status |= 0x02;
    status |= 0x04;
    if (globalBool("$Host::TournamentMode")) status |= 0x08;
    if (globalBool("$Host::NoSmurfs")) status |= 0x10;
    if (globalBool("$Host::TeamDamageOn")) status |= 0x20;
    if (globalBool("$Host::HiVisibility")) status |= 0x80;
    return status;
}

void handleGameInfoRequest(const Address& address, uint32_t key, uint8_t flags) {
    // Do not respond unless there is a server running.
    if (!SimState::server().allowConnections) return;
    if (isServerOnline() && (flags & V12::PingOfflineQuery)) return;
    TorqueScript* ts = script();
    V12::GameInfoResponse info;
    std::string paths = ts && ts->isFunction("getModPaths") ? ts->callFunction("getModPaths", {}).toString() : "base";
    // Strip the "base" from the string.
    if (const size_t base = paths.find(";base"); base != std::string::npos) paths.erase(base, 5);
    info.mod = paths;
    info.gameType = global("$MissionTypeDisplayName");
    info.missionName = global("$MissionDisplayName");
    info.status = serverStatus();
    info.players = (uint8_t)globalInt("$HostGamePlayerCount");
    info.maxPlayers = (uint8_t)globalInt("$Host::MaxPlayers");
    info.bots = (uint8_t)globalInt("$HostGameBotCount");
    info.cpuMhz = 0;
    info.info = global("$Host::Info");
    if (ts && ts->isFunction("getServerStatusString"))
        info.statusString = ts->callFunction("getServerStatusString", {}).toString();
    sendFromPort(address, V12::buildGameInfoResponse(flags, key, info));
}

// NetConnection::processRawPacket's keepAlive: a packet of this connection
// epoch within the window.
void routeDataPacket(NetLink& link, const uint8_t* data, size_t size) {
    V12BitStream stream(data, size);
    V12::DnetHeader header;
    if (V12::readDnetHeader(stream, header) &&
        header.connectSequenceBit == (bool)((link.clientConnectSequence ^ link.serverConnectSequence) & 1)) {
        link.lastPingSendTime = virtualMilliseconds();
        link.pingSendCount = 0;
    }
    GameConnection* connection = link.connection;
    if (linkObject(link)) connection->receivePacket(data, size);
}

// TribesGame::processPacketReceiveEvent.
void processPacket(const Address& source, const uint8_t* data, size_t size, bool portSocket) {
    if (size == 0) return;
    if (data[0] & 0x01) {
        // Lookup the connection in the address table.
        if (NetLink* link = findLink(source, !portSocket)) routeDataPacket(*link, data, size);
        return;
    }
    V12BitStream stream(data + 1, size - 1);
    const uint8_t type = data[0];
    if (type <= 22) { // GameHeartbeat and below: handleInfoPacket
        if (!portSocket) return;
        const uint8_t flags = stream.readU8();
        const uint32_t key = stream.readU32();
        if (stream.failed()) return;
        if (type == V12::OobGamePingRequest) handleGamePingRequest(source, key, flags);
        else if (type == V12::OobGameInfoRequest) handleGameInfoRequest(source, key, flags);
        return;
    }
    switch (type) {
    case V12::OobConnectChallengeRequest: if (portSocket) handleConnectChallengeRequest(source, stream); break;
    case V12::OobConnectRequest: if (portSocket) handleConnectRequest(source, stream); break;
    case V12::OobConnectChallengeResponse: if (!portSocket) handleConnectChallengeResponse(source, stream); break;
    case V12::OobConnectAccept: if (!portSocket) handleConnectAccept(source, stream); break;
    case V12::OobDisconnect: handleDisconnect(source, stream, !portSocket); break;
    case V12::OobConnectReject: if (!portSocket) handleConnectReject(source, stream); break;
    case V12::OobConnectChallengeReject: if (!portSocket) handleConnectChallengeReject(source, stream); break;
    default: break;
    }
}

void receiveAll(int& fd, bool portSocket) {
    uint8_t buffer[V12::MaxPacketDataSize];
    while (fd >= 0) {
        sockaddr_in from{};
        socklen_t length = sizeof from;
        const ssize_t bytes = recvfrom(fd, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (bytes < 0) break;
        if (from.sin_family != AF_INET || bytes <= 0) continue;
        processPacket(fromSockaddr(from), buffer, (size_t)bytes, portSocket);
    }
}

// dispatchCheckTimeouts.
void checkTimeouts() {
    static uint32_t lastTimeoutCheckTime = 0;
    const uint32_t time = virtualMilliseconds();
    if (time <= lastTimeoutCheckTime + TimeoutCheckInterval) return;

    // Check the connection state.
    if (gConnectRequest.state == ConnectRequest::Challenging &&
        time > gConnectRequest.lastSendTime + ChallengeRetryTime) {
        if (gConnectRequest.sendCount > ChallengeRetryCount) {
            gConnectRequest.state = ConnectRequest::NotConnecting;
            execute("onConnectRequestTimedOut");
        } else {
            sendConnectChallengeRequest();
        }
    } else if (gConnectRequest.state == ConnectRequest::Connecting &&
               time > gConnectRequest.lastSendTime + ConnectRetryTime) {
        if (gConnectRequest.sendCount > ConnectRetryCount) {
            gConnectRequest.state = ConnectRequest::NotConnecting;
            execute("onConnectRequestTimedOut");
        } else {
            sendConnectRequest();
        }
    }
    lastTimeoutCheckTime = time;

    // NetConnection::checkTimeout on every network connection. (The ping
    // itself is not sent: a GameConnection writes a packet every update, so
    // a live peer is never silent that long.)
    std::vector<GameConnection*> timedOut;
    for (auto& link : gLinks) {
        if (time > link.lastPingSendTime + PingTimeout) {
            if (link.pingSendCount >= link.pingRetryCount) {
                timedOut.push_back(link.connection);
                continue;
            }
            link.lastPingSendTime = time;
            link.pingSendCount++;
        }
    }
    for (GameConnection* connection : timedOut) {
        NetLink* link = findLink(connection);
        ScriptObject* object = link ? linkObject(*link) : nullptr;
        if (!object) continue;
        if (link->serverConnection) {
            Console::instance().printf(LogLevel::Info, "Connection to server timed out");
            execute("onConnectionToServerTimedOut");
        } else {
            Console::instance().printf(LogLevel::Info, "Client %d timed out.",
                                       ScriptEngine::instance().objectId(object));
            object->internals["__disconnectReason"] = VMValue("TimedOut");
        }
        if ((link = findLink(connection)) && (object = linkObject(*link))) deleteConnection(object);
    }
}

// A connection deleted without going through GameConnection::delete (its
// group went, or C++ deleted it) still issues its Disconnect.
void sweepRemovedConnections() {
    std::vector<NetLink> gone;
    for (const auto& link : gLinks)
        if (!linkObject(link)) gone.push_back(link);
    for (const auto& link : gone) {
        sendDisconnect(link, std::string());
        retireLink(link);
    }
}

} // namespace

void netInterfaceProcess(double) {
    if (!ScriptEngine::exists()) return;
    sweepRemovedConnections();
    receiveAll(gPortSocket, true);
    receiveAll(gClientSocket, false);
    checkTimeouts();
}

void registerNetInterfaceNatives(TorqueScript& ts) {
    using Args = std::vector<VMValue>;
    // main.cc cSetNetPort: Net::openPort(port) closes the port and binds
    // the new one on every interface.
    ts.registerNative("setNetPort", [](const Args& args) -> VMValue {
        const int port = args.empty() ? 0 : args[0].toInt();
        if (gPortSocket >= 0) {
            close(gPortSocket);
            gPortSocket = -1;
        }
        gPortSocket = openUdpSocket(port);
        if (gPortSocket >= 0) Console::instance().printf(LogLevel::Info, "UDP initialized on port %d", port);
        else Console::instance().printf(LogLevel::Info, "Unable to initialize UDP -- error %d", errno);
        return VMValue("");
    });
    // cConnect(addr, password, args...).
    ts.registerNative("connect", [](const Args& args) -> VMValue {
        if (args.empty()) return VMValue("");
        gConnectRequest.state = ConnectRequest::Challenging;
        gConnectRequest.clientConnectSequence = virtualMilliseconds();
        if (!stringToAddress(args[0].toString(), gConnectRequest.serverAddress)) return VMValue("");
        gConnectRequest.protocolVersion = CurrentProtocolVersion;
        gConnectRequest.password = args.size() > 1 ? args[1].toString().substr(0, MaxPasswordLength) : std::string();
        gConnectRequest.argv.clear();
        size_t bufPos = 0;
        for (size_t i = 2; i < args.size() && gConnectRequest.argv.size() < MaxConnectArgs; ++i) {
            const std::string arg = args[i].toString();
            if (bufPos + arg.size() + 1 > V12::MaxPacketDataSize) break;
            gConnectRequest.argv.push_back(arg);
            bufPos += arg.size() + 1;
        }
        gConnectRequest.sendCount = 0;
        gConnectRequest.authenticated = false;
        sendConnectChallengeRequest();
        return VMValue("");
    });
    // SimObject::delete on a connection runs GameConnection::onRemove.
    ts.registerNative("GameConnection::delete", [](const Args& args) -> VMValue {
        if (args.empty()) return VMValue("");
        deleteConnection(ScriptEngine::instance().findObject(args[0].toString().c_str()));
        return VMValue("");
    });
    ts.registerNative("GameConnection::setDisconnectReason", [](const Args& args) -> VMValue {
        ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (object && dynamic_cast<GameConnection*>(object->engine.get()))
            object->internals["__disconnectReason"] = VMValue(args.size() > 1 ? args[1].toString().substr(0, 255)
                                                                              : std::string());
        return VMValue("");
    });
}
