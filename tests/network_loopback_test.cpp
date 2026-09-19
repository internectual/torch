#include "net/network.h"
#include "net/protocol.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "game/game.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>

int main() {
    World objectiveWorld;
    World::WorldObject objective;
    objective.className = "AIObjective";
    objective.objectName = "ObjectiveA";
    objective.missionObjective = true;
    objectiveWorld.addObject(objective);
    assert(objectiveWorld.setObjectiveActive("ObjectiveA", false));
    assert(objectiveWorld.setObjectiveState("ObjectiveA", 1));
    assert(objectiveWorld.setObjectiveTarget("ObjectiveA", "Generator", 12));
    assert(objectiveWorld.setObjectiveWeight("ObjectiveA", 2, 7.5f));
    assert(objectiveWorld.setObjectiveScore("ObjectiveA", 4.0f));
    assert(objectiveWorld.setObjectiveTeam("ObjectiveA", 2));
    assert(!objectiveWorld.setObjectiveState("ObjectiveA", 4));
    assert(!objectiveWorld.setObjectiveTeam("ObjectiveA", -1));
    const auto& objectiveState = objectiveWorld.objects().front();
    assert(objectiveState.objectiveActive && objectiveState.objectiveState == 1);
    assert(objectiveState.objectiveTarget == "Generator" && objectiveState.objectiveTargetId == 12);
    assert(objectiveState.objectiveWeights[2] == 7.5f && objectiveState.objectiveScore == 4.0f);
    assert(objectiveState.teamId == 2);

    GameServer server;
    assert(server.start(0));
    assert(server.isRunning());
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

    // Stock scripts must be able to distinguish live sessions from replay
    // playback; the native is backed by an injectable provider for tests.
    ScriptEngine script;
    assert(script.init());
    std::vector<std::string> objectiveCallbacks;
    script.ts()->registerNative("AIObjective::onComplete", [&](const auto& args) {
        objectiveCallbacks.push_back("complete:" + args[0].toString());
        return VMValue(1);
    });
    script.ts()->registerNative("AIObjective::onFail", [&](const auto& args) {
        objectiveCallbacks.push_back("fail:" + args[0].toString());
        return VMValue(1);
    });
    std::vector<std::string> damageCallbacks;
    script.ts()->registerNative("Player::onDamage", [&](const auto& args) {
        damageCallbacks.push_back("damage:" + args[0].toString());
        return VMValue(1);
    });
    script.ts()->registerNative("Player::onRepair", [&](const auto& args) {
        damageCallbacks.push_back("repair:" + args[0].toString());
        return VMValue(1);
    });
    assert(objectiveWorld.setObjectiveState("ObjectiveA", 2));
    assert(objectiveWorld.setObjectiveState("ObjectiveA", 3));
    assert((objectiveCallbacks == std::vector<std::string>{"complete:ObjectiveA", "fail:ObjectiveA"}));
    script.setDemoStateProvider([] { return false; });
    const auto& natives = script.ts()->getNatives();
    assert(natives.at("isdemo")({}).toInt() == 0);
    script.setDemoStateProvider([] { return true; });
    assert(natives.at("isdemo")({}).toInt() == 1);

    script.setControlObjectProvider([] { return 17; });
    script.setCameraObjectProvider([] { return 23; });
    assert(natives.at("getcontrolobject")({}).toInt() == 17);
    assert(natives.at("getcontrolobjectid")({}).toInt() == 17);
    assert(natives.at("serverconnection::getcontrolobject")({}).toInt() == 17);
    assert(natives.at("getcameraobject")({}).toInt() == 23);
    assert(natives.at("gameconnection::getcameraobject")({}).toInt() == 23);
    script.setControlObjectProvider([] { return -1; });
    script.setCameraObjectProvider([] { return -1; });
    assert(natives.at("getcontrolobject")({}).toInt() == -1);
    assert(natives.at("getcameraobject")({}).toInt() == -1);
    script.setServerStateProvider([] { return false; });
    script.setClientStateProvider([] { return true; });
    assert(natives.at("isserver")({}).toInt() == 0);
    assert(natives.at("isclient")({}).toInt() == 1);
    script.setServerStateProvider([] { return true; });
    script.setClientStateProvider([] { return false; });
    assert(natives.at("isserver")({}).toInt() == 1);
    assert(natives.at("isclient")({}).toInt() == 0);
    script.setConnectionStateProvider([] {
        ScriptConnectionState state;
        state.serverAddress = "192.0.2.10:28000";
        state.serverPort = 28000;
        state.clientName = "LoopbackObserver";
        state.clientId = 7;
        state.missionName = "Dustbowl";
        state.missionType = "Capture the Flag";
        state.missionCrc = 0x78563412u;
        state.online = true;
        state.observer = true;
        return state;
    });
    assert(natives.at("getserveraddress")({}).toString() == "192.0.2.10:28000");
    assert(natives.at("getserverport")({}).toInt() == 28000);
    assert(natives.at("getclientname")({}).toString() == "LoopbackObserver");
    assert(natives.at("getclientid")({}).toInt() == 7);
    assert(natives.at("getmissionname")({}).toString() == "Dustbowl");
    assert(natives.at("getmissiontype")({}).toString() == "Capture the Flag");
    assert((uint32_t)natives.at("getmissioncrc")({}).toInt() == 0x78563412u);
    assert(natives.at("isonline")({}).toInt() == 1);
    assert(natives.at("isobserver")({}).toInt() == 1);

    script.setObjectStateProvider([](int id, ScriptObjectState& state) {
        if (id != 42) return false;
        state.datablockId = 77;
        state.className = "Player";
        state.shapeName = "shapes/test.dts";
        state.name = "TestPlayer";
        state.position = {1.25f, 2.5f, -3.75f};
        state.health = 80.0f;
        state.maxHealth = 100.0f;
        state.hasHealth = true;
        state.hasMaxHealth = true;
        state.teamId = 2;
        state.state = 1;
        return true;
    });
    assert(natives.at("isobject")({VMValue("42")}).toInt() == 1);
    assert(natives.at("getdatablock")({VMValue(42)}).toInt() == 77);
    assert(natives.at("getclassname")({VMValue(42)}).toString() == "Player");
    assert(natives.at("getshapefile")({VMValue(42)}).toString() == "shapes/test.dts");
    assert(natives.at("getname")({VMValue(42)}).toString() == "TestPlayer");
    assert(natives.at("getposition")({VMValue(42)}).toString() == "1.25 2.5 -3.75");
    assert(natives.at("getteam")({VMValue(42)}).toInt() == 2);
    assert(natives.at("getstate")({VMValue(42)}).toInt() == 1);
    assert(std::abs(natives.at("getdamagelevel")({VMValue(42)}).toFloat() - 0.2f) < 0.0001f);
    assert(natives.at("getrepairrate")({VMValue(42)}).toFloat() == 0.0f);
    assert(natives.at("isobject")({VMValue("999")}).toInt() == 0);
    assert(natives.at("getclassname")({VMValue(999)}).toString().empty());
    assert(natives.at("getposition")({VMValue(999)}).toString() == "0 0 0");

    int changedTeam = -1;
    script.setTeamMutationProvider([&](int objectId, int team) {
        changedTeam = objectId == 42 ? team : -1;
        return objectId == 42 && team >= 0;
    });
    assert(natives.at("setteam")({VMValue(42), VMValue(1)}).toInt() == 1);
    assert(changedTeam == 1);
    assert(natives.at("setteam")({VMValue(99), VMValue(1)}).toInt() == 0);
    assert(natives.at("setmountedimage")({VMValue(42), VMValue(0), VMValue(7)}).toInt() == 0);

    auto* targetOwner = new ScriptObject;
    targetOwner->className = "Player";
    targetOwner->name = "TargetOwner";
    script.objects[targetOwner->name] = targetOwner;
    assert(natives.at("createtarget")({VMValue("TargetOwner"), VMValue("Name"),
        VMValue("Skin"), VMValue("Voice"), VMValue(3), VMValue(2)}).toInt() > 0);
    const int targetId = targetOwner->fields["target"].toInt();
    assert(targetId > 0);
    assert(natives.at("gettargetname")({VMValue(targetId)}).toString() == "Name");
    assert(natives.at("gettargettype")({VMValue(targetId)}).toInt() == 3);
    assert(natives.at("settargetalwaysvismask")({VMValue(targetId), VMValue(4)}).toInt() == 1);
    assert(natives.at("gettargetalwaysvismask")({VMValue(targetId)}).toInt() == 4);
    assert(natives.at("settargetsensorgroup")({VMValue(targetId), VMValue(32)}).toInt() == 0);
    assert(natives.at("settarget")({VMValue("TargetOwner"), VMValue(9999)}).toInt() == 0);
    assert(natives.at("freetarget")({VMValue(targetId)}).toInt() == 1);
    assert(natives.at("gettarget")({VMValue("TargetOwner")}).toInt() == -1);
    assert(natives.at("freetarget")({VMValue(targetId)}).toInt() == 0);

    ScriptLoadoutState loadout;
    loadout.hasInventory = true;
    loadout.inventory["DiscPack"] = 3;
    loadout.maxInventory["DiscPack"] = 5;
    loadout.hasWeapons = true;
    loadout.weaponCount = 4;
    loadout.currentWeapon = 2;
    loadout.weaponAmmo[2] = 17;
    loadout.ammo = 17;
    loadout.hasBackpack = true;
    loadout.backpackIndex = 6;
    loadout.backpackActive = true;
    script.setLoadoutStateProvider([loadout] { return loadout; });
    assert(natives.at("getinventory")({VMValue("Player"), VMValue("DiscPack")}).toInt() == 3);
    assert(natives.at("maxinventory")({VMValue("Player"), VMValue("DiscPack")}).toInt() == 5);
    assert(natives.at("getweaponammo")({VMValue(2)}).toInt() == 17);
    assert(natives.at("getweaponammo")({VMValue(1)}).toInt() == -1);
    assert(natives.at("getammo")({}).toInt() == 17);
    assert(natives.at("getcurrentweapon")({}).toInt() == 2);
    assert(natives.at("getweaponcount")({}).toInt() == 4);
    assert(natives.at("getbackpack")({}).toInt() == 6);
    script.setLoadoutStateProvider([] { return ScriptLoadoutState{}; });
    assert(natives.at("getammo")({}).toInt() == -1);
    assert(natives.at("getcurrentweapon")({}).toInt() == -1);
    assert(natives.at("getweaponcount")({}).toInt() == 0);
    assert(natives.at("getbackpack")({}).toInt() == -1);

    int changedObject = -1;
    float changedHealth = -1.0f;
    float changedEnergy = -1.0f;
    float changedRepairRate = -1.0f;
    int changedSlot = -1;
    int changedAmmo = -1;
    script.setHealthMutationProvider([&](int objectId, float value) {
        changedObject = objectId; changedHealth = value; return objectId == 42;
    });
    script.setEnergyMutationProvider([&](int objectId, float value) {
        changedObject = objectId; changedEnergy = value; return objectId == 42;
    });
    script.setRepairRateMutationProvider([&](int objectId, float value) {
        changedObject = objectId; changedRepairRate = value; return objectId == 42;
    });
    script.setWeaponAmmoMutationProvider([&](int objectId, int slot, int ammo) {
        changedObject = objectId; changedSlot = slot; changedAmmo = ammo; return objectId == 42;
    });
    script.setCurrentWeaponMutationProvider([&](int objectId, int slot) {
        changedObject = objectId; changedSlot = slot; return objectId == 42;
    });
    assert(natives.at("sethealth")({VMValue(42), VMValue(125.0f)}).toInt() == 1);
    assert(changedObject == 42 && changedHealth == 100.0f);
    assert(natives.at("setenergylevel")({VMValue(42), VMValue(-5.0f)}).toInt() == 1);
    assert(changedEnergy == 0.0f);
    assert(natives.at("setrepairrate")({VMValue(42), VMValue(5.0f)}).toInt() == 1);
    assert(changedRepairRate == 5.0f);
    assert(natives.at("setweaponammo")({VMValue(42), VMValue(2), VMValue(19)}).toInt() == 1);
    assert(changedSlot == 2 && changedAmmo == 19);
    assert(natives.at("setcurrentweapon")({VMValue(42), VMValue(2)}).toInt() == 1);
    assert(natives.at("sethealth")({VMValue(99), VMValue(50.0f)}).toInt() == 0);
    assert(natives.at("damage")({VMValue(42), VMValue(20.0f)}).toInt() == 1);
    assert(changedHealth == 60.0f);
    assert(natives.at("repair")({VMValue(42), VMValue(10.0f)}).toInt() == 1);
    assert(changedHealth == 90.0f);
    assert((damageCallbacks == std::vector<std::string>{"damage:42", "repair:42"}));
    assert(natives.at("setrepairrate")({VMValue("42"), VMValue(-1.0f)}).toInt() == 0);
    assert(natives.at("sethealth")({VMValue(42), VMValue("nan")}).toInt() == 0);
    assert(natives.at("setinventory")({VMValue("42"), VMValue("DiscPack"), VMValue(3)}).toInt() == 0);
    assert(natives.at("setweaponammo")({VMValue(42), VMValue(2), VMValue(-2)}).toInt() == 0);

    int inventoryObject = -1;
    std::string inventoryItem;
    int inventoryDelta = 0;
    script.setInventoryMutationProvider([&](int objectId, const std::string& item, int delta) {
        inventoryObject = objectId;
        inventoryItem = item;
        inventoryDelta = delta;
        return objectId == 42 && item == "DiscPack";
    });
    assert(natives.at("setinventory")({VMValue(42), VMValue("DiscPack"), VMValue(3)}).toInt() == 1);
    assert(inventoryObject == 42 && inventoryItem == "DiscPack" && inventoryDelta == 3);
    assert(natives.at("incinventory")({VMValue(42), VMValue("DiscPack"), VMValue(2)}).toInt() == 1);
    assert(inventoryDelta == 2);
    assert(natives.at("decinventory")({VMValue(42), VMValue("DiscPack"), VMValue(1)}).toInt() == 1);
    assert(inventoryDelta == -1);
    script.setInventoryMutationProvider({});
    assert(natives.at("setinventory")({VMValue(42), VMValue("DiscPack"), VMValue(3)}).toInt() == 0);

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
    assert(!server.isRunning());
    return 0;
}
