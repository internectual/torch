#include "net/network.h"
#include "net/protocol.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>

int main() {
    GameServer server;
    assert(server.start(0));
    assert(server.port() != 0);
    assert(server.queryPort() != 0);

    const int querySocket = socket(AF_INET, SOCK_DGRAM, 0);
    assert(querySocket >= 0);
    sockaddr_in queryAddress{};
    queryAddress.sin_family = AF_INET;
    queryAddress.sin_port = htons(server.queryPort());
    assert(inet_pton(AF_INET, "127.0.0.1", &queryAddress.sin_addr) == 1);
    const char query[] = "QUERY";
    assert(sendto(querySocket, query, sizeof(query) - 1, 0,
                  (sockaddr*)&queryAddress, sizeof(queryAddress)) == (ssize_t)(sizeof(query) - 1));
    server.update();
    char json[4096]{};
    sockaddr_in responseAddress{};
    socklen_t responseLength = sizeof(responseAddress);
    const ssize_t jsonLength = recvfrom(querySocket, json, sizeof(json) - 1, MSG_DONTWAIT,
                                        (sockaddr*)&responseAddress, &responseLength);
    assert(jsonLength > 0);
    json[jsonLength] = 0;
    assert(std::strstr(json, "\"map\":\"test\"") != nullptr);
    assert(std::strstr(json, "\"gamemode\":0") != nullptr);
    assert(std::strstr(json, "\"numplayers\":0") != nullptr);
    assert(std::strstr(json, "\"numbots\":0") != nullptr);
    close(querySocket);
    // Keep the packet small enough that this test reaches its fixture in the
    // first native ghost batch, independent of UDP scheduling.
    for (uint32_t index = 1; index <= 23; ++index)
        assert(server.removeGhost(index));

    Connection client;
    client.setObserverMode(true);
    client.setPlayerName("LoopbackObserver");

    int connected = 0;
    int creates = 0;
    int updates = 0;
    int deletes = 0;
    int createDamageState = -1;
    int updateDamageState = -1;
    int scoreMessages = 0;
    int teamMessages = 0;
    uint32_t dynamicGhost = 0;
    client.setConnectCallback([&](bool ok) { if (ok) ++connected; });
    client.setGhostCallback([&](const V12::GhostUpdate& update,
                                const V12::PlayerGhostState* state) {
        if (update.operation == V12::GhostUpdate::Operation::Create) {
            if (update.index == dynamicGhost) {
                ++creates;
                if (state && state->hasDamageState) createDamageState = state->damageState;
            }
        } else if (update.operation == V12::GhostUpdate::Operation::Update) {
            if (update.index == dynamicGhost) {
                ++updates;
                if (state && state->hasDamageState) updateDamageState = state->damageState;
            }
        } else if (update.operation == V12::GhostUpdate::Operation::Delete) {
            if (update.index == dynamicGhost) ++deletes;
        }
    });
    client.setServerMessageCallback([&](const std::vector<std::string>& args) {
        if (!args.empty() && args[0] == "MsgPlayerScore") ++scoreMessages;
        if (!args.empty() && args[0] == "MsgClientJoinTeam") ++teamMessages;
    });
    assert(client.connect("127.0.0.1", server.port()));
    for (int i = 0; i < 100 && connected == 0; ++i) {
        server.update();
        client.update();
        std::this_thread::yield();
    }
    assert(connected == 1);
    assert(client.isConnected() && client.isObserverMode());

    dynamicGhost = server.spawnGhost(T2Protocol::CLASS_PLAYER, 4, 5, 6);
    assert(dynamicGhost != 0);
    V12::ClientMove move;
    client.sendNativeMove(1, move);
    for (int i = 0; i < 500 && creates == 0; ++i) {
        server.update();
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(creates > 0);
    assert(createDamageState == 0);
    const auto createdSnapshot = client.observerSnapshot();
    assert(!createdSnapshot.players.empty());
    assert(createdSnapshot.players.front().second.hasMaxHealth);
    assert(createdSnapshot.matchStarted);
    assert(createdSnapshot.clockRemainingMs > 0);
    assert(createdSnapshot.clockRemainingMs <= 20u * 60u * 1000u);
    assert(scoreMessages > 0 && teamMessages > 0);
    assert(client.observerSnapshot().protocol.highestAcknowledged >= 1);
    assert(client.observerSnapshot().protocol.established);

    client.sendNativeMove(2, move);
    for (int i = 0; i < 500 && updates == 0; ++i) {
        server.update();
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(updates > 0);
    assert(updateDamageState == 0);

    assert(server.removeGhost(dynamicGhost));
    for (int i = 0; i < 100 && deletes == 0; ++i) {
        server.update();
        client.update();
        std::this_thread::yield();
    }
    assert(deletes == 1);
    client.disconnect();
    for (int i = 0; i < 100 && client.state() != Connection::Disconnected; ++i) {
        server.update();
        client.update();
        std::this_thread::yield();
    }
    assert(client.state() == Connection::Disconnected);
    assert(client.observerSnapshot().players.empty());
    assert(client.observerSnapshot().missionCrc == 0);

    // Reusing the same Connection must start a fresh native epoch and not
    // inherit the prior ghost tracker or acknowledgement state.
    connected = 0;
    assert(client.connect("127.0.0.1", server.port()));
    for (int i = 0; i < 100 && connected == 0; ++i) {
        server.update();
        client.update();
        std::this_thread::yield();
    }
    assert(connected == 1);
    assert(client.observerSnapshot().epoch > 1);
    server.stop();
    return 0;
}
