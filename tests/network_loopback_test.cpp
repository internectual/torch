#include "net/network.h"
#include "net/protocol.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "game/game.h"
#include "core/engine.h"

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
    ScriptEngine providerEngine;
    assert(providerEngine.init());
    auto* testFileSystem = new FileSystem;
    assert(testFileSystem->init({"/home/methodown/t2-linux"}));
    Engine::instance().filesys = testFileSystem;
    Engine::instance().scr = &providerEngine;
    TorqueScript* utilityScript = providerEngine.ts();
    assert(std::abs(utilityScript->callFunction("mAtan", {VMValue(1.0), VMValue(0.0)}).toFloat() -
                    1.5707963f) < 1e-5f);
    assert(utilityScript->callFunction("mRound", {VMValue(-1.5)}).toInt() == -2);
    assert(utilityScript->callFunction("mClampI", {VMValue(9), VMValue(0), VMValue(3)}).toInt() == 3);
    assert(utilityScript->callFunction("mClampF", {VMValue(-1.0), VMValue(3.0), VMValue(0.0)}).toFloat() == 0.0f);
    assert(utilityScript->callFunction("mFloatLength", {}).toFloat() == 0.0f);
    assert(utilityScript->callFunction("mFloatLength", {VMValue(12.3456), VMValue(2)}).toFloat() == 12.35f);
    assert(utilityScript->callFunction("setWindVelocity", {VMValue("1 2 3")}).toInt() == 1);
    const std::string windBeforeInvalid = utilityScript->callFunction("getWindVelocity", {}).toString();
    assert(utilityScript->callFunction("setWindVelocity", {}).toInt() == 0);
    assert(utilityScript->callFunction("setWindVelocity", {VMValue("not a vector")}).toInt() == 0);
    assert(utilityScript->callFunction("setWindVelocity", {VMValue("1 2")}).toInt() == 0);
    assert(utilityScript->callFunction("getWindVelocity", {}).toString() == windBeforeInvalid);
    assert(utilityScript->callFunction("setWindVelocity", {VMValue("0 0 0")}).toInt() == 1);
    assert(utilityScript->callFunction("VectorCross", {VMValue("1 0 0"), VMValue("0 1 0")}).toString() == "0 0 1");
    utilityScript->callFunction("setRandomSeed", {VMValue(7)});
    const int firstRandom = utilityScript->callFunction("getRandom", {VMValue(-3), VMValue(3)}).toInt();
    assert(firstRandom >= -3 && firstRandom <= 3);
    assert(utilityScript->callFunction("getRandom", {VMValue(INT32_MIN), VMValue(INT32_MAX)}).toInt() >= INT32_MIN);
    const int realTime = utilityScript->callFunction("getRealTime", {}).toInt();
    assert(realTime != 0);
    assert(utilityScript->callFunction("getSimTime", {}).toInt() >= 0);
    int velocityId = 0;
    Point3F velocity{};
    providerEngine.setVelocityMutationProvider([&](int id, const Point3F& value) {
        velocityId = id;
        velocity = value;
        return true;
    });
    assert(providerEngine.mutateVelocity(7, {1.0f, 2.0f, 3.0f}));
    assert(velocityId == 7 && velocity.z == 3.0f);
    int operation = 0;
    std::string threadValue;
    providerEngine.setThreadMutationProvider([&](int id, int slot, int op, const std::string& value) {
        assert(id == 7 && slot == 2);
        operation = op;
        threadValue = value;
        return true;
    });
    assert(providerEngine.mutateThread(7, 2, 1, "run"));
    assert(operation == 1 && threadValue == "run");
    providerEngine.setVelocityMutationProvider(nullptr);
    providerEngine.setThreadMutationProvider(nullptr);
    assert(!providerEngine.mutateVelocity(7, {}));
    assert(!providerEngine.mutateThread(7, 0, 1, "run"));

    World objectiveWorld;
    World::WorldObject objective;
    objective.className = "AIObjective";
    objective.objectName = "ObjectiveA";
    objective.missionObjective = true;
    objectiveWorld.addObject(objective);
    assert(objectiveWorld.setObjectiveActive("ObjectiveA", false));
    assert(!objectiveWorld.objects().front().objectiveActive);
    assert(objectiveWorld.setObjectiveState("ObjectiveA", 0));
    assert(!objectiveWorld.objects().front().objectiveActive &&
           objectiveWorld.objects().front().objectiveState == 0);
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

    World::WorldObject transformable;
    transformable.className = "StaticShape";
    transformable.objectName = "Transformable";
    transformable.pos = {1.25f, -2.5f, 3.75f};
    transformable.rot = {0.0f, 0.7071f, 0.0f};
    transformable.rotAngleDeg = 90.0f;
    objectiveWorld.addObject(transformable);
    std::string transform;
    assert(objectiveWorld.getMissionObjectTransform("Transformable", transform));
    assert(transform == "1.25 -2.5 3.75 0 0.7071 0 90");
    assert(objectiveWorld.setMissionObjectTransform(
        "Transformable", "4 5 6 1 0 0 45"));
    assert(!objectiveWorld.setMissionObjectTransform(
        "Transformable", "4 5 6 1 0 0 45 trailing"));
    assert(!objectiveWorld.setMissionObjectTransform(
        "Transformable", "4 5 6 1 0 0"));
    assert(objectiveWorld.getMissionObjectTransform("Transformable", transform));
    assert(transform == "4 5 6 1 0 0 45");
    Point3F position;
    assert(objectiveWorld.setMissionObjectPosition("Transformable", {7, 8, 9}));
    assert(objectiveWorld.getMissionObjectPosition("Transformable", position));
    assert(position.x == 7.0f && position.y == 8.0f && position.z == 9.0f);
    Point3F axis;
    float angle = 0.0f;
    assert(objectiveWorld.setMissionObjectRotation("Transformable", {0, 1, 0}, 135.0f));
    assert(objectiveWorld.getMissionObjectRotation("Transformable", axis, angle));
    assert(axis.y == 1.0f && angle == 135.0f);
    Point3F scale;
    assert(objectiveWorld.setMissionObjectScale("Transformable", {2, 3, 4}));
    assert(objectiveWorld.getMissionObjectScale("Transformable", scale));
    assert(scale.x == 2.0f && scale.y == 3.0f && scale.z == 4.0f);
    assert(!objectiveWorld.setMissionObjectScale("Transformable", {0, 1, 1}));

    World::WorldObject camera;
    camera.className = "Camera";
    camera.objectName = "MissionCamera";
    objectiveWorld.addObject(camera);
    providerEngine.setMissionObjects({
        ScriptMissionObject{0, "Camera", "MissionCamera", "", {}}
    });
    assert(objectiveWorld.getMissionObjectTransform("MissionCamera", transform));
    // Mission objects carry dynamic SimObject ids; a numeric handle resolves.
    const std::string cameraId = std::to_string(providerEngine.missionObjects().front().id);
    assert(providerEngine.missionObjects().front().id >= 1027);
    assert(objectiveWorld.setMissionObjectPosition(cameraId, {1, 2, 3}));
    assert(objectiveWorld.setMissionObjectPosition("MissionCamera", {1, 2, 3}));
    assert(objectiveWorld.getMissionObjectPosition("MissionCamera", position));
    assert(position.x == 1.0f && position.y == 2.0f && position.z == 3.0f);
    World::WorldObject vehicle;
    vehicle.className = "Vehicle";
    vehicle.objectName = "ImageVehicle";
    vehicle.mountedImages[2] = "RepairImage";
    objectiveWorld.addObject(vehicle);
    std::string image;
    assert(objectiveWorld.getMissionObjectImage("ImageVehicle", 2, image));
    assert(image == "RepairImage");
    assert(!objectiveWorld.getMissionObjectImage("Transformable", 2, image));

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
    // Use an original mission asset for deterministic replacement coverage.
    const size_t startupGhosts = server.ghostCount();
    server.changeMap("test");
    assert(server.ghostCount() != startupGhosts);
    const size_t loadedGhosts = server.ghostCount();
    assert(server.loadMission("missions/test.mis"));
    assert(server.ghostCount() == loadedGhosts);
    assert(!server.loadMission("missions/does-not-exist.mis"));
    assert(server.ghostCount() == loadedGhosts);
    // Keep the packet small enough that this test reaches its fixture in the
    // first native ghost batch, independent of UDP scheduling.
    for (uint32_t index = 1; index <= 26; ++index)
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
    std::vector<uint8_t> oversizedPacket(V12::MaxPacketDataSize + 1, 0);
    assert(!client.ingestObserverPacket(oversizedPacket.data(), oversizedPacket.size()));
    for (int i = 0; i < 20 && client.observerSnapshot().missionCrc == 0; ++i) {
        client.update();
        server.update();
        std::this_thread::yield();
    }
    assert(client.observerSnapshot().missionCrc != 0);

    Connection snapshotClient;
    bool targetAssigned = false;
    uint16_t assignedTarget = 0;
    snapshotClient.setTargetControlCallback([&](bool hasTarget, uint16_t targetId,
                                        bool hasPosition, const V12Vec3& position,
                                        bool assign) {
        targetAssigned = hasTarget && assign;
        assignedTarget = targetId;
        (void)hasPosition;
        (void)position;
    });
    std::vector<uint16_t> restoredTargets;
    bool ghostsRestored = false;
    snapshotClient.setGhostCallback([&](const V12::GhostUpdate&, const V12::PlayerGhostState*) {
        ghostsRestored = true;
    });
    snapshotClient.setTargetCallback([&](const V12::ServerEvent::TargetInfo* target, uint16_t id) {
        assert(ghostsRestored);
        if (target && target->hasName) restoredTargets.push_back(id);
    });
    Connection::ObserverSnapshot seeded;
    seeded.missionCrc = 0x12345678;
    seeded.hasCameraFov = true;
    seeded.cameraFov = 92;
    seeded.controlGhost = 12;
    seeded.controlAssigned = true;
    seeded.ghostClasses.emplace_back(12, 25);
    seeded.ghostClasses.emplace_back(44, 20);
    seeded.players.emplace_back(12, V12::PlayerGhostState{});
    seeded.players.back().second.hasDatablock = true;
    seeded.players.back().second.datablockId = 77;
    seeded.targets.push_back(V12::ServerEvent::TargetInfo{12, true, false, false,
        false, false, "ObserverTarget"});
    V12::DecodedDataBlock audioData;
    audioData.audioFilename = "audio/environment_loop";
    audioData.audioEnvironmentRef = 17;
    seeded.datablocks.emplace(77, audioData);
    assert(snapshotClient.seedObserverSnapshot(seeded));
    assert(targetAssigned && assignedTarget == 12);
    const auto restored = snapshotClient.observerSnapshot();
    assert(restored.missionCrc == 0x12345678 && restored.hasCameraFov && restored.cameraFov == 92);
    assert(restored.controlAssigned && restored.controlGhost == 12);
    assert(restored.ghostClasses.size() == 2 && restored.ghostClasses[1] == std::pair<uint16_t, uint16_t>(44, 20));
    assert(restored.datablocks.at(77).audioEnvironmentRef == 17);
    assert(restoredTargets == std::vector<uint16_t>{12});

    // Stock scripts must be able to distinguish live sessions from replay
    // playback; the native is backed by an injectable provider for tests.
    ScriptEngine script;
    assert(script.init());
    const auto& stringNatives = script.ts()->getNatives();
    assert(stringNatives.at("strlwr")({VMValue("MiXeD")}).toString() == "mixed");
    assert(stringNatives.at("strupr")({VMValue("MiXeD")}).toString() == "MIXED");
    assert(stringNatives.at("trim")({VMValue(" \t value\n")}).toString() == "value");
    assert(stringNatives.at("strreplace")({VMValue("a-b-b"), VMValue("b"), VMValue("x")}).toString() == "a-x-x");
    assert(stringNatives.at("getwordcount")({VMValue(" one\t two  three ")}).toInt() == 3);
    assert(stringNatives.at("getwords")({VMValue("one two three"), VMValue(1), VMValue(2)}).toString() == "two three");
    assert(stringNatives.at("getfieldcount")({VMValue("one\ttwo\t")}).toInt() == 3);
    assert(stringNatives.at("getfields")({VMValue("one\ttwo\tthree"), VMValue(1)}).toString() == "two\tthree");
    assert(stringNatives.at("format")({VMValue("%1/%2"), VMValue("base"), VMValue("file.cs")}).toString() == "base/file.cs");
    assert(stringNatives.at("filename")({VMValue("scripts/server.cs")}).toString() == "server.cs");
    assert(stringNatives.at("filepath")({VMValue("scripts/server.cs")}).toString() == "scripts/");
    assert(stringNatives.at("filebase")({VMValue("scripts/server.cs")}).toString() == "server");
    assert(stringNatives.at("fileext")({VMValue("scripts/server.cs")}).toString() == ".cs");
    assert(stringNatives.at("getsubstr")({VMValue("abcdef"), VMValue(2), VMValue(-1)}).toString() == "cdef");
    script.ts()->execute(
        "function Lifecycle::onAdd(%this) { $lifecycleAdd = $lifecycleAdd + 1; }"
        "function Lifecycle::onFieldModified(%this,%field,%old,%new) { $lifecycleField = $lifecycleField + 1; }"
         "function Lifecycle::onRemove(%this) { $lifecycleRemove = $lifecycleRemove + 1; }"
         "function Listener::onDeleteNotify(%this,%deleted) { $lifecycleNotify = $lifecycleNotify + 1; }"
         "function Lifecycle::getName(%this) { return 'class:' @ %this; }"
        "function Lifecycle::onTimer(%this,%value) { $timerTrace = $timerTrace @ %this @ ':' @ %value; }"
        "function globalTimer(%value) { $timerTrace = $timerTrace @ 'global:' @ %value; }"
        "function clientCmdPing(%a,%b) { $commandTrace = %a @ ':' @ %b; }"
        "function serverCmdPing(%value) { $serverTrace = %value; }"
        "function onMissionEnd() { $missionTrace = $missionTrace @ 'end>'; }"
        "function onMissionStart(%mission) { $missionTrace = $missionTrace @ 'start:' @ %mission; }"
        "function messageCallback(%type,%unused,%value) { $messageTrace = %type @ ':' @ %value; }");
    assert(script.ts()->dispatchClientCommand({"Ping", "left", "right"}));
    assert(script.ts()->dispatchServerCommand({"Ping", "server"}));
    assert(script.ts()->getGlobal("$commandTrace").toString() == "left:right");
    assert(script.ts()->getGlobal("$serverTrace").toString() == "server");
    assert(!script.ts()->dispatchMissionCallback("onMissionEnded", {}));
    assert(script.ts()->dispatchMissionCallback("onMissionEnd", {}));
    assert(script.ts()->dispatchMissionCallback("onMissionStart", {VMValue("missions/test.mis")}));
    assert(script.ts()->getGlobal("$missionTrace").toString() == "end>start:missions/test.mis");
    script.ts()->registerNative("clientCmdNativePing", [](const auto& args) {
        return VMValue(args.empty() ? "" : args[0].toString());
    });
    assert(script.ts()->dispatchClientCommand({"NATIVEPING", "native"}));
    assert(script.ts()->callFunction("clientCmdNativePing", {VMValue("native")}).toString() == "native");
    const VMValue echoResult = script.ts()->getNatives().at("echo")({VMValue("one"), VMValue("two")});
    const VMValue warnResult = script.ts()->getNatives().at("warn")({VMValue("warning")});
    const VMValue errorResult = script.ts()->getNatives().at("error")({VMValue("failure")});
    assert(echoResult.type == VMValue::None && warnResult.type == VMValue::None &&
           errorResult.type == VMValue::None);
    const auto& log = Console::instance().getLog();
    assert(log.size() >= 3 && log[log.size() - 3] == "[INFO] one two" &&
           log[log.size() - 2] == "[WARN] warning" && log.back() == "[ERROR] failure");
    script.ts()->registerMessageCallback("MsgTest", "messageCallback");
    script.ts()->dispatchMessageCallback("MsgTest", {VMValue("MsgTest"), VMValue(""), VMValue("payload")});
    assert(script.ts()->getGlobal("$messageTrace").toString() == "MsgTest:payload");
    script.ts()->execute("new SimGroup(LifecycleGroup) { new Lifecycle(LifecycleChild); };"
                         "new Listener(LifecycleListener);");
    assert(script.ts()->getGlobal("$lifecycleAdd").toInt() == 1);
    auto* lifecycleChild = script.findObject("LifecycleChild");
    auto* lifecycleListener = script.findObject("LifecycleListener");
    assert(lifecycleChild && lifecycleListener);
    script.ts()->execute("new SimGroup(CaseObject);");
    assert(script.deleteScriptObject("caseobject"));
    assert(!script.findObject("CaseObject"));
    assert(script.ts()->execute("LifecycleChild.getName();").toString() == "class:LifecycleChild");
    assert(script.addDeleteNotify(lifecycleListener, lifecycleChild));
    assert(script.setObjectField(lifecycleChild, "state", VMValue("ready")));
    assert(script.ts()->getGlobal("$lifecycleField").toInt() == 1);
    script.ts()->execute("LifecycleChild.StAtE = 1; LifecycleChild.state++; "
                         "$SemanticValue = LifecycleChild.STATE; "
                         "$SemanticArray[0] = 7; "
                         "function SemanticReturn(%value) { return %value; }");
    assert(script.ts()->getGlobal("$semanticvalue").toInt() == 2);
    assert(script.ts()->execute("$SemanticArray[0];").toInt() == 7);
    assert(script.ts()->execute("SemanticReturn(9);").toInt() == 9);
    assert(script.ts()->execute("LifecycleChild.missingField;").toString().empty());
    assert(VMValue("FALSE").toBool() == false);
    script.ts()->execute(
        "function ArgumentSemantics(%first,%second,%third) { "
        "$argumentTrace = %first @ ':' @ %second @ ':' @ %third; "
        "$argumentCount = %argc; $argumentZero = %argv[0]; $argumentTwo = %argv[2]; "
        "}"
        "$Indexed[2] = 4; $indexedAlias = $indexed2; "
        "ArgumentSemantics('one');");
    assert(script.ts()->getGlobal("$argumentTrace").toString() == "one::");
    assert(script.ts()->getGlobal("$argumentCount").toInt() == 1);
    assert(script.ts()->getGlobal("$argumentZero").toString() == "one");
    assert(script.ts()->getGlobal("$argumentTwo").toString().empty());
    assert(script.ts()->getGlobal("$indexedAlias").toInt() == 4);
    script.ts()->execute(
        "function CallSemantics(%first,%second) { "
        "$callTrace = %argc @ ':' @ %argv[0] @ ':' @ %argv[1] @ ':' @ %second; "
        "return %argc + 0; } "
        "function NestedCall(%value) { if (%value <= 0) return %argc; "
        "return NestedCall(%value - 1) + 1; } "
        "$callResult = CallSemantics('x'); $nestedResult = NestedCall(3);");
    assert(script.ts()->getGlobal("$callTrace").toString() == "1:x::");
    assert(script.ts()->getGlobal("$callResult").toInt() == 1);
    assert(script.ts()->getGlobal("$nestedResult").toInt() == 4);
    script.ts()->execute(
        "function PackageProbe() { return 'base'; }"
        "package PatchProbe { function PackageProbe() { return Parent::PackageProbe() @ '-patch'; } }"
        "activatePackage('PatchProbe');"
        "$packageActive = PackageProbe();");
    assert(script.ts()->getGlobal("$packageActive").toString() == "base-patch");
    assert(script.ts()->isActivePackage("PatchProbe"));
    assert(Console::instance().getIntVariable("$TotalNumberOfPackages", 0) == 1);
    assert(std::string(Console::instance().getStringVariable("$Package[0]", "")) == "patchprobe");
    assert(script.ts()->execute("deactivatePackage('PatchProbe'); PackageProbe();").toString() == "base");
    assert(!script.ts()->isActivePackage("PatchProbe"));
    assert(Console::instance().getIntVariable("$TotalNumberOfPackages", 0) == 0);
    assert(std::string(Console::instance().getStringVariable("$Package[0]", "x")).empty());
    script.ts()->execute(
        "function StackProbe() { return 'base'; }"
        "package StackLow { function StackProbe() { return Parent::StackProbe() @ '-low'; } }"
        "package StackHigh { function StackProbe() { return Parent::StackProbe() @ '-high'; } }"
        "activatePackage('stacklow'); activatePackage('STACKHIGH');"
        "$stackResult = StackProbe();");
    assert(script.ts()->getGlobal("$stackResult").toString() == "base-low-high");
    assert(script.ts()->execute("deactivatePackage('stackhigh'); StackProbe();").toString() == "base-low");
    assert(script.ts()->execute("deactivatePackage('STACKLOW'); StackProbe();").toString() == "base");
    assert(script.ts()->execute("activatePackage('missingPackage');").toInt() == 0);
    script.ts()->execute(
        "function NamespaceProbe::value() { return 'base'; }"
        "package NamespacePatch { function NamespaceProbe::value() { return 'patch'; } }"
        "activatePackage('namespacepatch'); $namespaceResult = NamespaceProbe::value();");
    assert(script.ts()->getGlobal("$namespaceResult").toString() == "patch");
    assert(script.ts()->execute("deactivatePackage('NamespacePatch'); NamespaceProbe::value();").toString() == "base");
    script.ts()->execute("package StaleParent { function StaleProbe() { return Parent::Missing(); } } activatePackage('StaleParent'); StaleProbe();");
    assert(script.ts()->execute("function AfterParent() { return 'normal'; } AfterParent();").toString() == "normal");
    script.ts()->clearPackages();
    assert(Console::instance().getIntVariable("$TotalNumberOfPackages", 0) == 0);
    assert(!script.ts()->isActivePackage("StaleParent"));
    script.ts()->execute("$syntaxBefore = 1; function Broken( { $syntaxAfter = 1; }");
    assert(script.ts()->getGlobal("$syntaxBefore").toInt() == 1);
    assert(script.ts()->getGlobal("$syntaxAfter").toInt() == 0);
    assert(script.ts()->execute("$postSyntax = 7;").type != VMValue::None);
    assert(script.ts()->getGlobal("$postSyntax").toInt() == 7);
    script.ts()->execute(
        "$short = 0; 0 && ($short = 1); 1 || ($short = 2); "
        "$assignLeft = 10; $assignRight = 3; $assignLeft = $assignRight; "
        "$compound = 5; $compound += 3; $compound *= 2; $compound %= 5; "
        "$compound |= 8; $compound &= 11; $compound ^= 2; $compound <<= 1; $compound >>= 2; "
        "$doCount = 0; do { $doCount++; } while ($doCount < 3); "
        "$numericStringEqual = ('02' == 2); $stringEqual = ('Foo' $= 'fOO'); "
        "switch (2) { case 1: $switchResult = 1; break; case 2: $switchResult = 2; break; default: $switchResult = 3; } "
        "function EmptyReturn() { return; } EmptyReturn();");
    assert(script.ts()->getGlobal("$short").toInt() == 0);
    assert(script.ts()->getGlobal("$assignLeft").toInt() == 3);
    assert(script.ts()->getGlobal("$compound").toInt() == 5);
    assert(script.ts()->getGlobal("$doCount").toInt() == 3);
    assert(script.ts()->getGlobal("$numericStringEqual").toInt() == 1);
    assert(script.ts()->getGlobal("$stringEqual").toInt() == 1);
    assert(script.ts()->getGlobal("$switchResult").toInt() == 2);
    assert(script.ts()->callFunction("EmptyReturn", {}).type == VMValue::None);
    script.ts()->execute(
        "$loopTrace = ''; $i = 0; while ($i < 1) { $i++; $loopTrace = 'body'; break; $loopTrace = 'bad'; } "
        "while (0) { $loopTrace = 'bad2'; } "
        "for ($j = 0; $j < 3; $j++) { if ($j == 1) continue; $loopTrace = $loopTrace @ $j; } "
        "$conditionProbe = 0; $conditionBodies = 0; while (++$conditionProbe < 3) { $conditionBodies++; } "
        "$forProbe = 0; for ($k = 0; ++$forProbe < 3; $k++) { } "
        "function ScopeProbe(%value) { %local = %value; return %local; } "
        "function Factorial(%value) { if (%value <= 1) return 1; return %value * Factorial(%value - 1); } "
        "$scopeResult = ScopeProbe(7); $factorial = Factorial(5); $outsideLocal = %local;");
    assert(script.ts()->getGlobal("$loopTrace").toString() == "body02");
    assert(script.ts()->getGlobal("$conditionProbe").toInt() == 3);
    assert(script.ts()->getGlobal("$conditionBodies").toInt() == 2);
    assert(script.ts()->getGlobal("$forProbe").toInt() == 3);
    assert(script.ts()->getGlobal("$scopeResult").toInt() == 7);
    assert(script.ts()->getGlobal("$factorial").toInt() == 120);
    assert(script.ts()->getGlobal("$outsideLocal").toInt() == 0);
    script.ts()->registerNative("Interop::Capture", [](const auto& args) {
        return VMValue(args.size() == 2 ? args[0].toString() + "|" + args[1].toString() : "");
    });
    assert(script.ts()->execute("interop::capture('left', 'right');").toString() == "left|right");
    assert(script.ts()->execute("LifecycleChild.deleteNotify(LifecycleListener);").toInt() == 1);
    assert(script.ts()->execute("LifecycleChild.clearNotify(LifecycleListener);").toInt() == 1);
    assert(script.ts()->execute("LifecycleChild.deleteNotify(LifecycleListener);").toInt() == 1);
    script.ts()->execute("new SimGroup(NotifyTarget); new Listener(NotifyListener); "
                         "NotifyTarget.deleteNotify(NotifyListener);");
    assert(script.deleteScriptObject("NotifyListener"));
    script.ts()->execute("new Listener(NotifyListener); $lifecycleNotify = 0;");
    assert(script.deleteScriptObject("NotifyTarget"));
    assert(script.ts()->getGlobal("$lifecycleNotify").toInt() == 0);
    const int lifecycleEvent = script.ts()->scheduleEvent(0.0, 100.0, "LifecycleChild", "noop", {});
    assert(script.ts()->isEventPending(lifecycleEvent));
    assert(script.deleteScriptObject("LifecycleGroup"));
    assert(script.ts()->getGlobal("$lifecycleRemove").toInt() == 1);
    assert(script.ts()->getGlobal("$lifecycleNotify").toInt() == 1);
    assert(!script.ts()->isEventPending(lifecycleEvent));
    assert(!script.findObject("LifecycleChild") && !script.findObject("LifecycleGroup"));

    script.ts()->execute("new Lifecycle(LifecycleTimer);");
    const int methodEvent = script.ts()->scheduleEvent(10.0, 1.0, "LifecycleTimer", "onTimer", {VMValue("method")});
    const int globalEvent = script.ts()->scheduleEvent(10.0, 1.0, "0", "globalTimer", {VMValue("global")});
    const int invalidEvent = script.ts()->scheduleEvent(10.0, 1.0, "MissingObject", "globalTimer", {VMValue("invalid")});
    assert(script.ts()->getNatives().count("cancelevent") == 1);
    assert(script.ts()->processScheduledEvents(10.5) == 0);
    assert(script.ts()->getNatives().at("cancelevent")({VMValue(invalidEvent)}).toInt() == 1);
    assert(script.ts()->processScheduledEvents(11.0) == 2);
    assert(script.ts()->getGlobal("$timerTrace").toString() == "LifecycleTimer:methodglobal:global");
    assert(!script.ts()->isEventPending(methodEvent) && !script.ts()->isEventPending(globalEvent));

    script.ts()->execute("new Lifecycle(ArrayFields) { Values[0] = 4; };"
                         "ArrayFields.values[0] += 3; "
                         "ArrayFields.VALUES[1] += 2; "
                         "ArrayFields.values[0]++; "
                         "ArrayFields.values[7] += 5; "
                         "$arrayFieldRead = ArrayFields.VALUES[0]; "
                         "$arrayFieldMissing = ArrayFields.values[9]; "
                         "$arrayFieldUndefinedCompound = ArrayFields.VALUES[7];");
    auto* arrayFields = script.findObject("ArrayFields");
    assert(arrayFields);
    assert(arrayFields->fields.at("Values[0]").toInt() == 8);
    assert(arrayFields->fields.at("VALUES[1]").toInt() == 2);
    assert(script.ts()->getGlobal("$arrayFieldRead").toInt() == 8);
    assert(script.ts()->getGlobal("$arrayFieldMissing").toString().empty());
    assert(script.ts()->getGlobal("$arrayFieldUndefinedCompound").toInt() == 5);

    script.setMissionObjects({
        ScriptMissionObject{0, "MissionTimer", "MissionTimerObject", "", {}}
    });
    script.ts()->execute("function MissionTimer::onTick(%this,%value) { $missionTimer = %this @ ':' @ %value; }");
    const std::string timerId = std::to_string(script.missionObjects().front().id);
    const int missionEvent = script.ts()->scheduleEvent(20.0, 0.0, timerId, "onTick", {VMValue("tick")});
    assert(script.ts()->processScheduledEvents(20.0) == 1);
    assert(script.ts()->getGlobal("$missionTimer").toString() == timerId + ":tick");
    assert(!script.ts()->isEventPending(missionEvent));
    const int globalMissionEvent = script.ts()->scheduleEvent(
        30.0, 0.0, "0", "globalTimer", {VMValue("survives")});
    script.cancelMissionEvents();
    assert(script.ts()->isEventPending(globalMissionEvent));
    assert(script.ts()->processScheduledEvents(30.0) == 1);
    assert(script.ts()->getGlobal("$timerTrace").toString().find("global:survives") != std::string::npos);
    script.clearMissionObjects();
    script.setMissionObjects({
        ScriptMissionObject{0, "SimGroup", "CycleA", "CycleB", {}},
        ScriptMissionObject{0, "SimGroup", "CycleB", "CycleA", {}}
    });
    const auto cycleOrder = script.missionDeletionOrder("CycleA");
    assert((cycleOrder == std::vector<std::string>{"CycleB", "CycleA"}));
    script.clearMissionObjects();
    script.ts()->execute("function MissionThing::onRemove(%this) { $missionRemovalTrace = $missionRemovalTrace @ %this @ ';'; }");
    script.setMissionObjects({
        ScriptMissionObject{0, "MissionThing", "MissionRoot", "", {}},
        ScriptMissionObject{0, "MissionThing", "MissionChild", "MissionRoot", {}}
    });
    script.dispatchMissionObjectRemovalCallbacks();
    assert(script.ts()->getGlobal("$missionRemovalTrace").toString() == "MissionChild;MissionRoot;");
    script.clearMissionObjects();

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
    script.setDemoModeProvider([] { return false; });
    const auto& natives = script.ts()->getNatives();
    assert(natives.at("strcmp")({VMValue("a"), VMValue("z")}).toInt() == -1);
    assert(natives.at("strcmp")({VMValue("z"), VMValue("a")}).toInt() == 1);
    assert(natives.at("strcmp")({VMValue("same"), VMValue("same")}).toInt() == 0);
    assert(natives.at("isdemo")({}).toInt() == 0);
    assert(natives.at("isdemoplaying")({}).toInt() == 0);
    script.setDemoStateProvider([] { return true; });
    assert(script.isDemoPlaying());
    assert(natives.at("isdemoplaying")({}).toInt() == 1);
    assert(natives.at("isdemo")({}).toInt() == 0);
    script.setDemoModeProvider([] { return true; });
    assert(natives.at("isdemo")({}).toInt() == 1);
    script.setDemoStateProvider([] { return false; });
    assert(!script.isDemoPlaying());
    assert(natives.at("isdemoplaying")({}).toInt() == 0);
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
        state.skinName = "blue";
        state.profileName = "TestProfile";
        state.type = 0x1234;
        state.name = "TestPlayer";
         state.position = {1.25f, 2.5f, -3.75f};
         state.rotation = {0.0f, 0.7071f, 0.0f};
         state.rotationW = 0.7071f;
         state.hasRotation = true;
         state.sensorGroup = 4;
         state.headPitch = 0.25f;
         state.headYaw = -0.5f;
         state.hasHeadAngles = true;
         state.barrelPitch = 0.75f;
         state.barrelYaw = -0.25f;
         state.hasTurretAim = true;
         state.shieldLevel = 0.6f;
         state.hasShield = true;
         state.health = 80.0f;
         state.maxHealth = 100.0f;
         state.damageState = 2;
         state.hasDamageState = true;
         state.jetting = true;
         state.frozen = false;
         state.braking = true;
         state.hasVehicleState = true;
         state.cloaked = true;
         state.hasCloak = true;
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
    assert(natives.at("getshapename")({VMValue(42)}).toString() == "shapes/test.dts");
    assert(natives.at("getskinname")({VMValue(42)}).toString() == "blue");
    assert(natives.at("getprofilename")({VMValue(42)}).toString() == "TestProfile");
    assert(natives.at("gettype")({VMValue(42)}).toInt() == 0x1234);
    assert(natives.at("getname")({VMValue(42)}).toString() == "TestPlayer");
    assert(natives.at("getposition")({VMValue(42)}).toString() == "1.25 2.5 -3.75");
    assert(natives.at("getrotation")({VMValue(42)}).toString() == "0 0.7071 0 0.7071");
    assert(natives.at("getsensorgroup")({VMValue(42)}).toInt() == 4);
    assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("rotation")}).toString() == "0 0.7071 0 0.7071");
    assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("headRotation")}).toString() == "0.25 -0.5");
    assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("barrelRotation")}).toString() == "0.75 -0.25");
     assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("shieldLevel")}).toFloat() == 0.6f);
     assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("damageState")}).toInt() == 2);
     assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("jetting")}).toInt() == 1);
     assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("braking")}).toInt() == 1);
     assert(natives.at("getfieldvalue")({VMValue("42"), VMValue("cloaked")}).toInt() == 1);
    assert(natives.at("getteam")({VMValue(42)}).toInt() == 2);
    assert(natives.at("getstate")({VMValue(42)}).toInt() == 1);
    assert(std::abs(natives.at("getdamagelevel")({VMValue(42)}).toFloat() - 0.2f) < 0.0001f);
    assert(natives.at("getrepairrate")({VMValue(42)}).toFloat() == 0.0f);
    assert(natives.at("isobject")({VMValue("999")}).toInt() == 0);
    assert(natives.at("getclassname")({VMValue(999)}).toString().empty());
    assert(natives.at("getshapename")({VMValue(999)}).toString().empty());
    assert(natives.at("getskinname")({VMValue(999)}).toString().empty());
    assert(natives.at("getprofilename")({VMValue(999)}).toString().empty());
    assert(natives.at("ismemberofclass")({VMValue(42), VMValue("Player")}).toInt() == 1);
    assert(natives.at("ismemberofclass")({VMValue(42), VMValue("GameBase")}).toInt() == 1);
    assert(natives.at("ismemberofclass")({VMValue(999), VMValue("Player")}).toInt() == 0);
    assert(natives.at("istypeof")({VMValue(42), VMValue("Player")}).toInt() == 1);
    assert(natives.at("istypeof")({VMValue(42), VMValue(0x1234)}).toInt() == 1);
    assert(natives.at("getposition")({VMValue(999)}).toString() == "0 0 0");

    auto* testData = new ScriptObject;
    testData->className = "PlayerData";
    testData->name = "TestPlayerData";
    testData->fields["mass"] = VMValue(42.5f);
    testData->fields["shapeFile"] = VMValue("shapes/test.dts");
    script.objects[testData->name] = testData;
    auto* testObject = new ScriptObject;
    testObject->className = "Player";
    testObject->name = "TestScriptObject";
    testObject->fields["datablock"] = VMValue("TestPlayerData");
    testObject->fields["dynamicValue"] = VMValue("initial");
    testObject->internals["parent"] = VMValue("MissionGroup");
    script.objects[testObject->name] = testObject;
    auto* missionGroup = new ScriptObject;
    missionGroup->className = "SimGroup";
    missionGroup->name = "MissionGroup";
    script.objects[missionGroup->name] = missionGroup;
    assert(natives.at("getfield")({VMValue("TestScriptObject"), VMValue("dynamicValue")}).toString() == "initial");
    assert(natives.at("getfield")({VMValue("TestScriptObject"), VMValue("DYNAMICVALUE")}).toString() == "initial");
    assert(natives.at("getdatafield")({VMValue("TestScriptObject"), VMValue("mass")}).toFloat() == 42.5f);
    assert(natives.at("setfield")({VMValue("TestScriptObject"), VMValue("dynamicValue"), VMValue("changed")}).toInt() == 1);
    assert(testObject->fields["dynamicValue"].toString() == "changed");
    assert(natives.at("getgroup")({VMValue("TestScriptObject")}).toString() == "MissionGroup");
    assert(natives.at("getcount")({VMValue("MissionGroup")}).toInt() == 1);
    assert(natives.at("getname")({VMValue("TestScriptObject")}).toString() == "TestScriptObject");
    assert(natives.at("getfield")({VMValue("a\tb"), VMValue(1)}).toString() == "b");
    assert(natives.at("setfield")({VMValue("a\tb"), VMValue(0), VMValue("x")}).toString() == "x\tb");
    assert(natives.at("getfield")({VMValue("999"), VMValue("field")}).toString().empty());
    assert(natives.at("getdatafield")({VMValue("42"), VMValue("mass")}).toString().empty());

    script.setMissionObjects({
        ScriptMissionObject{0, "SimGroup", "MissionGroup", "", {}},
        ScriptMissionObject{0, "SimGroup", "TeamGroup", "MissionGroup", {}},
        ScriptMissionObject{0, "StaticShape", "FirstShape", "TeamGroup", {{"shapeFile", VMValue("first.dts")}}},
        ScriptMissionObject{0, "StaticShape", "SecondShape", "TeamGroup", {}}
    });
    // Mission objects take consecutive dynamic SimObject ids.
    const int missionGroupId = script.missionObjects()[0].id;
    const int teamGroupId = script.missionObjects()[1].id;
    const int firstShapeId = script.missionObjects()[2].id;
    const int secondShapeId = script.missionObjects()[3].id;
    assert(missionGroupId >= 1027 && teamGroupId == missionGroupId + 1);
    assert(natives.at("isobject")({VMValue(missionGroupId)}).toInt() == 1);
    assert(natives.at("getname")({VMValue(firstShapeId)}).toString() == "FirstShape");
    assert(natives.at("getid")({VMValue("TeamGroup")}).toInt() == teamGroupId);
    assert(natives.at("getgroup")({VMValue(firstShapeId)}).toInt() == teamGroupId);
    assert(natives.at("getcount")({VMValue(teamGroupId)}).toInt() == 2);
    assert(natives.at("getobject")({VMValue(teamGroupId), VMValue(0)}).toInt() == firstShapeId);
    assert(natives.at("nextobject")({VMValue(firstShapeId)}).toInt() == secondShapeId);
    assert(natives.at("prevobject")({VMValue(secondShapeId)}).toInt() == firstShapeId);
    assert(natives.at("getfieldvalue")({VMValue(firstShapeId), VMValue("SHAPEFILE")}).toString() == "first.dts");
    assert(natives.at("delete")({VMValue(teamGroupId)}).toInt() == 1);
    assert(!natives.at("isobject")({VMValue(teamGroupId)}).toInt());
    assert(!natives.at("isobject")({VMValue(firstShapeId)}).toInt());
    assert(natives.at("isobject")({VMValue(missionGroupId)}).toInt() == 1);
    script.clearMissionObjects();

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
    script.setVelocityMutationProvider([&](int id, const Point3F& value) {
        velocityId = id;
        velocity = value;
        return id == 42;
    });
    assert(natives.at("setfieldvalue")({VMValue("42"), VMValue("velocity"), VMValue("3 4 5")}).toInt() == 1);
    assert(velocityId == 42 && velocity.x == 3.0f && velocity.y == 4.0f && velocity.z == 5.0f);
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
    const uint16_t firstPort = server.port();
    server.stop();
    assert(!server.isRunning());

    // Reusing a server instance must release both sockets and mission-owned
    // state, and a second start must bind fresh ephemeral ports.
    assert(server.start(0));
    assert(server.isRunning() && server.port() != 0 && server.port() != firstPort);
    assert(server.ghostCount() > 0);
    server.stop();
    assert(!server.isRunning() && server.port() == 0 && server.queryPort() == 0);
    assert(server.start(0));
    assert(server.isRunning() && server.port() != 0);
    server.stop();
    assert(!server.isRunning());
    Engine::instance().scr = nullptr;
    Engine::instance().filesys = nullptr;
    delete testFileSystem;
    return 0;
}
