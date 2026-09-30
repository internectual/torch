#include "sim/game_connection.h"
#include "sim/net_interface.h"
#include "core/timer.h"
#include "net/v12_protocol.h"
#include "net/v12_bitstream.h"
#include <fcntl.h>
#include <functional>
#include "sim/net_string_table.h"
#include "sim/camera.h"
#include "sim/player.h"
#include "sim/projectiles.h"
#include "sim/vehicle.h"
#include "sim/path_manager.h"
#include "sim/force_field.h"
#include "sim/projectile_aim.h"
#include <map>
#include <set>
#include "sim/sim_state.h"
#include "game/demo.h"
#include "net/network.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "game/game.h"
#include "core/engine.h"

#include <cassert>
#include <limits>
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
         "function Lifecycle::getName(%this) { return \"class:\" @ %this; }"
        "function Lifecycle::onTimer(%this,%value) { $timerTrace = $timerTrace @ %this @ \":\" @ %value; }"
        "function globalTimer(%value) { $timerTrace = $timerTrace @ \"global:\" @ %value; }"
        "function clientCmdPing(%a,%b) { $commandTrace = %a @ \":\" @ %b; }"
        "function serverCmdPing(%value) { $serverTrace = %value; }"
        "function onMissionEnd() { $missionTrace = $missionTrace @ \"end>\"; }"
        "function onMissionStart(%mission) { $missionTrace = $missionTrace @ \"start:\" @ %mission; }"
        "function messageCallback(%type,%unused,%value) { $messageTrace = %type @ \":\" @ %value; }");
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
        "$argumentTrace = %first @ \":\" @ %second @ \":\" @ %third; "
        "$argumentCount = %argc; $argumentZero = %argv[0]; $argumentTwo = %argv[2]; "
        "}"
        "$Indexed[2] = 4; $indexedAlias = $indexed2; "
        "ArgumentSemantics(\"one\");");
    assert(script.ts()->getGlobal("$argumentTrace").toString() == "one::");
    assert(script.ts()->getGlobal("$argumentCount").toInt() == 1);
    assert(script.ts()->getGlobal("$argumentZero").toString() == "one");
    assert(script.ts()->getGlobal("$argumentTwo").toString().empty());
    assert(script.ts()->getGlobal("$indexedAlias").toInt() == 4);
    script.ts()->execute(
        "function CallSemantics(%first,%second) { "
        "$callTrace = %argc @ \":\" @ %argv[0] @ \":\" @ %argv[1] @ \":\" @ %second; "
        "return %argc + 0; } "
        "function NestedCall(%value) { if (%value <= 0) return %argc; "
        "return NestedCall(%value - 1) + 1; } "
        "$callResult = CallSemantics(\"x\"); $nestedResult = NestedCall(3);");
    assert(script.ts()->getGlobal("$callTrace").toString() == "1:x::");
    assert(script.ts()->getGlobal("$callResult").toInt() == 1);
    assert(script.ts()->getGlobal("$nestedResult").toInt() == 4);
    script.ts()->execute(
        "function PackageProbe() { return \"base\"; }"
        "package PatchProbe { function PackageProbe() { return Parent::PackageProbe() @ \"-patch\"; } }"
        "activatePackage(\"PatchProbe\");"
        "$packageActive = PackageProbe();");
    assert(script.ts()->getGlobal("$packageActive").toString() == "base-patch");
    assert(script.ts()->isActivePackage("PatchProbe"));
    assert(script.ts()->execute("deactivatePackage(\"PatchProbe\"); PackageProbe();").toString() == "base");
    assert(!script.ts()->isActivePackage("PatchProbe"));
    script.ts()->execute(
        "function StackProbe() { return \"base\"; }"
        "package StackLow { function StackProbe() { return Parent::StackProbe() @ \"-low\"; } }"
        "package StackHigh { function StackProbe() { return Parent::StackProbe() @ \"-high\"; } }"
        "activatePackage(\"stacklow\"); activatePackage(\"STACKHIGH\");"
        "$stackResult = StackProbe();");
    assert(script.ts()->getGlobal("$stackResult").toString() == "base-low-high");
    assert(script.ts()->execute("deactivatePackage(\"stackhigh\"); StackProbe();").toString() == "base-low");
    assert(script.ts()->execute("deactivatePackage(\"STACKLOW\"); StackProbe();").toString() == "base");
    assert(script.ts()->execute("activatePackage(\"missingPackage\");").toInt() == 0);
    script.ts()->execute(
        "function NamespaceProbe::value() { return \"base\"; }"
        "package NamespacePatch { function NamespaceProbe::value() { return \"patch\"; } }"
        "activatePackage(\"namespacepatch\"); $namespaceResult = NamespaceProbe::value();");
    assert(script.ts()->getGlobal("$namespaceResult").toString() == "patch");
    assert(script.ts()->execute("deactivatePackage(\"NamespacePatch\"); NamespaceProbe::value();").toString() == "base");
    script.ts()->execute("package StaleParent { function StaleProbe() { return Parent::Missing(); } } activatePackage(\"StaleParent\"); StaleProbe();");
    assert(script.ts()->execute("function AfterParent() { return \"normal\"; } AfterParent();").toString() == "normal");
    script.ts()->clearPackages();
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
        "$numericStringEqual = (\"02\" == 2); $stringEqual = (\"Foo\" $= \"fOO\"); "
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
        "$loopTrace = \"\"; $i = 0; while ($i < 1) { $i++; $loopTrace = \"body\"; break; $loopTrace = \"bad\"; } "
        "while (0) { $loopTrace = \"bad2\"; } "
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
    assert(script.ts()->execute("interop::capture(\"left\", \"right\");").toString() == "left|right");
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
    const int methodEvent = script.ts()->scheduleEvent(10.0, 1.0, "LifecycleTimer", "onTimer", {VMValue("method")}, true);
    const int globalEvent = script.ts()->scheduleEvent(10.0, 1.0, "0", "globalTimer", {VMValue("global")});
    const int invalidEvent = script.ts()->scheduleEvent(10.0, 1.0, "MissingObject", "globalTimer", {VMValue("invalid")});
    assert(script.ts()->getNatives().count("cancelevent") == 1);
    assert(script.ts()->processScheduledEvents(10.5) == 0);
    assert(script.ts()->getNatives().at("cancelevent")({VMValue(invalidEvent)}).toInt() == 1);
    assert(script.ts()->processScheduledEvents(11.0) == 2);
    // SimConsoleEvent: %this is the object's id.
    assert(script.ts()->getGlobal("$timerTrace").toString() ==
           std::to_string(script.findObject("LifecycleTimer")->id) + ":methodglobal:global");
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
    script.ts()->execute("function MissionTimer::onTick(%this,%value) { $missionTimer = %this @ \":\" @ %value; }");
    const std::string timerId = std::to_string(script.missionObjects().front().id);
    const int missionEvent = script.ts()->scheduleEvent(20.0, 0.0, timerId, "onTick", {VMValue("tick")}, true);
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
    script.ts()->execute("function MissionThing::onRemove(%this) { $missionRemovalTrace = $missionRemovalTrace @ %this @ \";\"; }");
    script.setMissionObjects({
        ScriptMissionObject{0, "MissionThing", "MissionRoot", "", {}},
        ScriptMissionObject{0, "MissionThing", "MissionChild", "MissionRoot", {}}
    });
    script.dispatchMissionObjectRemovalCallbacks();
    assert(script.ts()->getGlobal("$missionRemovalTrace").toString() == "MissionChild;MissionRoot;");
    script.clearMissionObjects();

    std::vector<std::string> damageCallbacks;
    script.ts()->registerNative("Player::onDamage", [&](const auto& args) {
        damageCallbacks.push_back("damage:" + args[0].toString());
        return VMValue(1);
    });
    script.ts()->registerNative("Player::onRepair", [&](const auto& args) {
        damageCallbacks.push_back("repair:" + args[0].toString());
        return VMValue(1);
    });
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
    script.addObject(testData);
    auto* testObject = new ScriptObject;
    testObject->className = "Player";
    testObject->name = "TestScriptObject";
    testObject->fields["datablock"] = VMValue("TestPlayerData");
    testObject->fields["dynamicValue"] = VMValue("initial");
    testObject->internals["parent"] = VMValue("MissionGroup");
    script.addObject(testObject);
    auto* missionGroup = new ScriptObject;
    missionGroup->className = "SimGroup";
    missionGroup->name = "MissionGroup";
    script.addObject(missionGroup);
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

    Engine::instance().scr = nullptr;
    Engine::instance().filesys = nullptr;
    delete testFileSystem;
    {
        // Server GameConnection packets decode with the demo reader: the
        // remote command's tags arrive with their NetStringEvents first and
        // expand like RemoteCommandEvent::process.
        GameConnection connection;
        std::vector<std::vector<uint8_t>> packets;
        connection.deliver = [&](const std::vector<uint8_t>& packet) { packets.push_back(packet); };
        const std::string command = NetStrings::literal("ServerMessage");
        const std::string type = NetStrings::literal("MsgProbe");
        const std::string format = NetStrings::literal("%1 joined the %2.");
        connection.sendRemoteCommand({command, type, format, "Bob", "game", "12", "-40000"});
        connection.setMissionCRC(0x1234abcd);
        connection.checkPacketSend(1.0);
        assert(packets.size() == 1);
        DemoParser parser;
        PacketData pd = parser.parsePacket(packets[0].data(), packets[0].size(), -1);
        const NetEventInfo* remote = nullptr;
        bool crc = false;
        int strings = 0;
        for (const auto& event : pd.events) {
            if (event.classId == T2Demo::NetEventClassFirst + 9) remote = &event;
            if (event.classId == T2Demo::NetEventClassFirst + 13) crc = true;
            if (event.classId == T2Demo::NetEventClassFirst + 7) ++strings;
        }
        assert(strings == 3 && crc && remote);
        assert(remote->arguments.size() == 7);
        assert(remote->arguments[0] == "ServerMessage" && remote->arguments[1] == "MsgProbe");
        assert(remote->arguments[2] == "Bob joined the game.");
        assert(remote->arguments[5] == "12" && remote->arguments[6] == "-40000");
        // Nothing new to send: the next packet carries no events again.
        connection.checkPacketSend(2.0);
        assert(packets.size() == 2);
        PacketData again = parser.parsePacket(packets[1].data(), packets[1].size(), -1);
        assert(again.events.empty());
    }
    {
        // activateGhosting sends the ScopeAlways scene as GhostAlwaysObject
        // events the demo reader turns into ghosts with their scene fields.
        script.ts()->execute(
            "new TerrainBlock(GhostTerrain) { terrainFile = \"Minotaur.ter\"; detailTexture = \"details/baddet1\"; "
            "  squareSize = \"8\"; emptySquares = \"150887 151143\"; position = \"-1024 -1024 0\"; };"
            "new Sky(GhostSky) { materialList = \"sky_badlands_cloudy.dml\"; fogColor = \"0.5 0.4 0.3 1\"; "
            "  visibleDistance = \"1200\"; fogDistance = \"600\"; fogVolume1 = \"300 0 200\"; "
            "  fogVolumeColor1 = \"0.1 0.2 0.3 1\"; cloudText1 = \"skies/cloud1\"; windVelocity = \"1 2 0\"; };"
            "new Sun(GhostSun) { direction = \"0.57735 0.57735 -0.57735\"; color = \"0.6 0.6 0.6 1\"; ambient = \"0.2 0.2 0.2 1\"; };"
            "new InteriorInstance(GhostInterior) { interiorFile = \"xbunk1.dif\"; position = \"10 20 30\"; "
            "  rotation = \"0 0 1 90\"; scale = \"1 1 1\"; showTerrainInside = \"0\"; };"
            "new TSStatic(GhostStatic) { shapeName = \"xorg20.dts\"; position = \"1 2 3\"; };"
            "new MissionArea(GhostArea) { area = \"-536 -648 880 592\"; flightCeiling = \"300\"; flightCeilingRange = \"20\"; };"
            "new WaterBlock(GhostWater) { position = \"0 0 50\"; scale = \"128 128 20\"; liquidType = \"Lava\"; "
            "  surfaceTexture = \"liquidTiles/lava\"; density = \"2\"; };");
        GameConnection connection;
        std::vector<std::vector<uint8_t>> packets;
        connection.deliver = [&](const std::vector<uint8_t>& packet) { packets.push_back(packet); };
        connection.activateGhosting();
        // The events fill packets to the connection's packetSize (200
        // bytes by default), so the scene spans several packets.
        for (int i = 0; i < 12; ++i) connection.checkPacketSend(10.0 + i);
        assert(packets.size() == 12);
        DemoParser parser;
        for (const auto& packet : packets) parser.parsePacket(packet.data(), packet.size(), -1);
        for (const auto& packet : packets) assert(packet.size() <= V12::MaxPacketDataSize);
        assert(parser.getParseFault().empty());
        const GhostTracker& tracker = parser.getGhostTracker();
        std::map<std::string, const GhostEntry*> byClass;
        for (int index = 1024 - 7; index < 1024; ++index) {
            const GhostEntry* ghost = tracker.getGhost(index);
            assert(ghost);
            byClass[ghost->className] = ghost;
        }
        assert(byClass.size() == 7);
        auto prop = [&](const char* cls, const char* name) {
            const auto* ghost = byClass.at(cls);
            for (const auto& [key, value] : ghost->sceneProps) if (key == name) return value;
            return std::string();
        };
        assert(prop("TerrainBlock", "terrainFile") == "Minotaur.ter");
        assert(prop("TerrainBlock", "emptySquares") == "150887 151143");
        assert(prop("Sky", "materialList") == "sky_badlands_cloudy.dml");
        assert(prop("Sky", "cloudText1") == "skies/cloud1");
        assert(prop("InteriorInstance", "interiorFile") == "xbunk1.dif");
        assert(prop("TSStatic", "shapeName") == "xorg20.dts");
        assert(prop("MissionArea", "area") == "-536 -648 880 592");
        assert(prop("WaterBlock", "liquidType") == "4");
        assert(std::abs(byClass.at("InteriorInstance")->position.x - 10.0f) < 1e-4f);
        // The client answers GhostAlwaysDone with ReadyForNormalGhosts; the
        // ghost section then opens (empty: every object went as an event).
        V12::ProtocolState client;
        V12::DnetHeader header;
        for (const auto& packet : packets) {
            V12BitStream stream(packet.data(), packet.size());
            assert(V12::readDnetHeader(stream, header));
            client.processReceived(header);
        }
        V12::ClientPacketOptions reply;
        reply.events.push_back(V12::makeGhostingMessageEvent(1, GameConnection::ReadyForNormalGhosts, 0));
        const auto replyPacket = client.buildClientPacket(reply);
        connection.receivePacket(replyPacket.data(), replyPacket.size());
        assert(connection.isGhosting());
    }
    {
        // transmitDataBlocks over a local link: more datablocks than the
        // DataBlockQueueCount window; each SimDataBlockEvent decodes in the
        // demo reader, and the client role's acks bring dataBlocksDone.
        std::string source =
            "datablock AudioDescription(LinkDescription) { volume = 1.0; is3D = true; };"
            "datablock AudioProfile(LinkSound) { filename = \"fx/misc/thunder.wav\"; description = LinkDescription; };"
            "function GameConnection::dataBlocksDone(%this, %sequence) { $LinkDone = %sequence; }"
            "new GameConnection(LinkClient);";
        for (int i = 0; i < 20; ++i)
            source += "datablock DebrisData(LinkDebris" + std::to_string(i) + ") { numBounces = " +
                      std::to_string(i) + "; shapeName = \"debris_generic.dts\"; };";
        script.ts()->execute(source);
        auto* server = EngineObjects::get<GameConnection>("LinkClient");
        assert(server);
        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            parser.parsePacket(p.data(), p.size(), packets++);
        };
        // The recording's SendPacket block: the reader follows the client's
        // own packet sequence.
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->transmitDataBlocks(7);
        for (int tick = 0; tick < 200 && script.ts()->getGlobal("$LinkDone").toString().empty(); ++tick) {
            server->checkPacketSend(100.0 + tick);
            client.checkPacketSend(100.0 + tick + 0.5);
        }
        assert(parser.getParseFault().empty());
        assert(script.ts()->getGlobal("$LinkDone").toString() == "7");
        ScriptObject* group = ScriptEngine::instance().findObject("DataBlockGroup");
        assert(group);
        const size_t total = (size_t)group->internals["__childCount"].toInt();
        const auto& blocks = parser.getInitialBlock().dataBlocks;
        assert(total >= 22 && blocks.size() == total);
        // Ids go out as id - DataBlockObjectIdFirst.
        ScriptObject* debris = ScriptEngine::instance().findObject("LinkDebris19");
        auto decoded = blocks.find((uint32_t)ScriptEngine::instance().objectId(debris) - 3);
        assert(decoded != blocks.end() && decoded->second.className == "DebrisData");
        assert(decoded->second.decoded.debris.numBounces == 19);
        assert(decoded->second.decoded.debris.shape == "debris_generic.dts");
        ScriptObject* sound = ScriptEngine::instance().findObject("LinkSound");
        auto profile = blocks.find((uint32_t)ScriptEngine::instance().objectId(sound) - 3);
        assert(profile != blocks.end() && profile->second.decoded.audioFilename == "fx/misc/thunder.wav");
        // The client's modified key is current: nothing more to send.
        script.ts()->execute("$LinkDone = \"\";");
        server->transmitDataBlocks(8);
        assert(script.ts()->getGlobal("$LinkDone").toString() == "8");
    }
    {
        // scan.l FLOAT is {INTEGER}.{INTEGER}: "<id>.field" is an object id
        // and a field access.
        script.ts()->execute("new ScriptObject(IdFieldProbe) { text = \"LOBBY\"; };");
        const int id = ScriptEngine::instance().objectId(ScriptEngine::instance().findObject("IdFieldProbe"));
        script.ts()->execute("$idFieldTest = (" + std::to_string(id) + ".text $= \"LOBBY\"); $floatTest = 3.25 + 1.5e1;");
        assert(script.ts()->getGlobal("$idFieldTest").toInt() == 1);
        assert(std::abs(script.ts()->getGlobal("$floatTest").toFloat() - 18.25f) < 1e-4f);
    }
    {
        // An exec'd file's top-level locals live in its own frame; eval runs
        // in the caller's frame.
        script.ts()->execute("for (%i = 0; %i < 3; %i++) eval(\"$frameProbe\" @ %i @ \" = %i + 10;\");", "frame_probe.cs");
        assert(script.ts()->getGlobal("$frameProbe2").toInt() == 12);
    }
    {
        // A client's Camera: scoped to it and made its control object, it is
        // ghosted, its packet data carries position and mode, and the
        // client's moves fly it (ProcessList: one tick per move).
        script.ts()->execute("new GameConnection(CamLinkClient);"
                             "new Camera(CamLinkCamera) { position = \"10 20 30\"; };");
        auto* server = EngineObjects::get<GameConnection>("CamLinkClient");
        assert(server);
        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        GameState lastCamera{};
        bool sawCamera = false;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
            if (pd.gameState.hasCameraTransform) { lastCamera = pd.gameState; sawCamera = true; }
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 300.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("CamLinkCamera.scopeToClient(CamLinkClient); CamLinkClient.setControlObject(CamLinkCamera);");
        assert(script.ts()->getGlobal("$dummy").toString().empty());
        ClientMoveIn forward;
        forward.y = 32; // full forward
        for (int tick = 0; tick < 10; ++tick) {
            client.pushMove(forward);
            client.checkPacketSend(now);
            SimState::advanceServer(now);
            server->checkPacketSend(now + 0.01);
            now += 0.032;
        }
        // The unchanged control state goes whole again after
        // ControlStateSkipAmount packets of position only.
        for (int i = 0; i < GameConnection::ControlStateSkipAmount + 4; ++i, now += 0.2) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.1);
        }
        assert(parser.getParseFault().empty());
        const int ghostIndex = server->ghostIndex(ScriptEngine::instance().objectKey(
            ScriptEngine::instance().findObject("CamLinkCamera")));
        assert(ghostIndex >= 0);
        const GhostEntry* ghost = parser.getGhostTracker().getGhost(ghostIndex);
        assert(ghost && ghost->className == "Camera");
        assert(sawCamera && lastCamera.cameraMode == Camera::FlyMode);
        // Ten forward moves at 40 m/s, 32 ms each, from y = 20.
        auto* camera = EngineObjects::get<Camera>("CamLinkCamera");
        assert(std::abs(camera->transform[7] - (20.0f + 10 * 40.0f * 0.032f)) < 1e-3f);
        assert(std::abs(lastCamera.cameraPosition.x - 10.0f) < 1e-3f);
        assert(std::abs(lastCamera.cameraPosition.y - camera->transform[7]) < 1e-3f);
        assert(std::abs(lastCamera.cameraPosition.z - 30.0f) < 1e-3f);
    }
    {
        // A StaticShape running a script thread ghosts in the shipped layout
        // (sounds, then threads as sequence, state, direction, end): the
        // ghost after it in the same packet still reads.
        script.ts()->execute("datablock StaticShapeData(ThreadStation) { maxDamage = 1.0; };"
                             "new GameConnection(ThreadLinkClient);"
                             "new StaticShape(ThreadLinkShape) { dataBlock = ThreadStation; position = \"1 2 3\"; };"
                             "new Camera(ThreadLinkCamera) { position = \"4 5 6\"; };"
                             "new BeaconObject(ThreadLinkBeacon) { dataBlock = ThreadStation; position = \"7 8 9\"; };"
                             "ThreadLinkBeacon.setBeaconType(friend); $beaconType = ThreadLinkBeacon.getBeaconType();");
        assert(script.ts()->getGlobal("$beaconType").toString() == "friend");
        auto* server = EngineObjects::get<GameConnection>("ThreadLinkClient");
        auto* shape = EngineObjects::get<ShapeBase>("ThreadLinkShape");
        assert(server && shape);
        shape->threads[0].sequence = 3;
        shape->threads[0].state = ShapeBase::ScriptThread::Play;
        shape->threads[0].forward = false;
        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 500.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("ThreadLinkShape.scopeToClient(ThreadLinkClient); ThreadLinkBeacon.scopeToClient(ThreadLinkClient);"
                             "ThreadLinkCamera.scopeToClient(ThreadLinkClient);");
        for (int i = 0; i < 6; ++i, now += 0.2) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.1);
        }
        assert(parser.getParseFault().empty());
        auto& engine = ScriptEngine::instance();
        const int shapeGhost = server->ghostIndex(engine.objectKey(engine.findObject("ThreadLinkShape")));
        const int cameraGhost = server->ghostIndex(engine.objectKey(engine.findObject("ThreadLinkCamera")));
        assert(shapeGhost >= 0 && cameraGhost >= 0);
        const GhostEntry* shapeEntry = parser.getGhostTracker().getGhost(shapeGhost);
        const GhostEntry* cameraEntry = parser.getGhostTracker().getGhost(cameraGhost);
        assert(shapeEntry && shapeEntry->className == "StaticShape");
        assert(cameraEntry && cameraEntry->className == "Camera");
        const int beaconGhost = server->ghostIndex(engine.objectKey(engine.findObject("ThreadLinkBeacon")));
        assert(beaconGhost >= 0 && parser.getGhostTracker().getGhost(beaconGhost)->className == "BeaconObject");
        assert(shapeEntry->threads[0].valid && shapeEntry->threads[0].sequence == 3 &&
               shapeEntry->threads[0].state == ShapeBase::ScriptThread::Play && !shapeEntry->threads[0].forward);
    }
    {
        // Path::finishPath: a Path's Markers in seqNum order; the total time
        // leaves out the last marker's msToNext.
        const size_t before = PathManager::paths().size();
        script.ts()->execute("new SimGroup(MissionGroup) { new Path(TestPath) {"
                             "  new Marker() { seqNum = 2; position = \"3 3 3\"; msToNext = 500; };"
                             "  new Marker() { seqNum = 1; position = \"1 1 1\"; msToNext = 250; }; }; };"
                             "pathOnMissionLoadDone();");
        const auto& paths = PathManager::paths();
        assert(paths.size() == before + 1);
        const auto& path = paths.back();
        assert(path.positions.size() == 2 && path.positions[0].x == 1.0f && path.positions[1].x == 3.0f);
        assert(path.msToNext[0] == 250 && path.msToNext[1] == 500 && path.totalTime == 250);
    }
    {
        // math/mathTypes.cc: AngAxisF matrices as the engine builds them.
        script.ts()->execute("$mulV = MatrixMulVector(\"0 0 0 0 0 1 1.5707963\", \"1 0 0\");"
                             "$mulP = MatrixMulPoint(\"1 2 3 0 0 1 0\", \"1 1 1\");"
                             "$create = MatrixCreate(\"1 2 3\", \"0 0 1 180\");");
        float x = 9, y = 9, z = 9;
        std::sscanf(script.ts()->getGlobal("$mulV").toString().c_str(), "%f %f %f", &x, &y, &z);
        assert(std::abs(x) < 1e-5f && std::abs(y + 1.0f) < 1e-5f && std::abs(z) < 1e-5f);
        assert(script.ts()->getGlobal("$mulP").toString() == "2 3 4");
        assert(script.ts()->getGlobal("$create").toString() == "1 2 3 0 0 1 3.14159");
    }
    {
        // A controlled Player: its packet data (Player::writePacketData)
        // decodes as the control player with the server's position, and
        // moves run Player::processTick.
        script.ts()->execute("datablock PlayerData(LinkArmor) { maxForwardSpeed = 14; runForce = 4200; mass = 90; "
                             "  maxEnergy = 60; boxSize = \"1.2 1.2 2.3\"; };"
                             "new GameConnection(PlayerLinkClient);"
                             "new Player(PlayerLinkPlayer) { dataBlock = LinkArmor; };"
                             "PlayerLinkPlayer.setTransform(\"5 6 400 0 0 1 0\");");
        auto* server = EngineObjects::get<GameConnection>("PlayerLinkClient");
        auto* player = EngineObjects::get<PlayerObject>("PlayerLinkPlayer");
        assert(server && player);
        assert(std::abs(player->transform[11] - 400.0f) < 1e-4f);
        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        GameState last{};
        bool sawPlayer = false;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
            if (pd.gameState.controlPlayer.hasPosition) { last = pd.gameState; sawPlayer = true; }
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 500.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("PlayerLinkClient.setControlObject(PlayerLinkPlayer);");
        for (int tick = 0; tick < 20; ++tick) {
            client.pushMove(ClientMoveIn{});
            client.checkPacketSend(now);
            SimState::advanceServer(now);
            server->checkPacketSend(now + 0.01);
            now += 0.032;
        }
        // The whole control state again after ControlStateSkipAmount packets.
        for (int i = 0; i < GameConnection::ControlStateSkipAmount + 4; ++i, now += 0.2) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.1);
        }
        assert(parser.getParseFault().empty());
        const int ghostIndex = server->ghostIndex(ScriptEngine::instance().objectKey(
            ScriptEngine::instance().findObject("PlayerLinkPlayer")));
        assert(ghostIndex >= 0);
        const GhostEntry* ghost = parser.getGhostTracker().getGhost(ghostIndex);
        assert(ghost && ghost->className == "Player");
        // Falling under gravity: the client's control data is the server's.
        assert(player->transform[11] < 400.0f);
        assert(sawPlayer);
        assert(std::abs(last.controlPlayer.position.x - 5.0f) < 1e-4f);
        assert(std::abs(last.controlPlayer.position.z - player->transform[11]) < 1e-3f);
    }
    {
        // Mounted images (shapeImage.cc): a ShapeBaseImageData state machine
        // on a controlled Player. The fire trigger of a move drives image
        // slot 0 (Player::updateMove); entering the fire state bumps the
        // networked fireCount and runs the state's script on the image
        // datablock; the client decodes the image in the ghost's ImageMask.
        script.ts()->execute(
            "datablock PlayerData(ImageArmor) { maxForwardSpeed = 14; runForce = 4200; mass = 90; "
            "  maxEnergy = 60; boxSize = \"1.2 1.2 2.3\"; };"
            // The source interpreter's datablock body takes no array fields
            // (parseDatablock); the states are set as fields afterwards.
            "datablock ShapeBaseImageData(LinkTestImage) { offset = \"0 0.5 0\"; };"
            "LinkTestImage.stateName[0] = \"Activate\"; LinkTestImage.stateTimeoutValue[0] = 0.2;"
            "LinkTestImage.stateTransitionOnTimeout[0] = \"Ready\"; LinkTestImage.stateAllowImageChange[0] = false;"
            "LinkTestImage.stateName[1] = \"Ready\"; LinkTestImage.stateTransitionOnTriggerDown[1] = \"Fire\";"
            "LinkTestImage.stateName[2] = \"Fire\"; LinkTestImage.stateFire[2] = true; LinkTestImage.stateScript[2] = \"onFire\";"
            "LinkTestImage.stateTimeoutValue[2] = 0.1; LinkTestImage.stateTransitionOnTimeout[2] = \"Reload\";"
            "LinkTestImage.stateName[3] = \"Reload\"; LinkTestImage.stateTimeoutValue[3] = 0.1;"
            "LinkTestImage.stateTransitionOnTimeout[3] = \"Ready\";"
            "datablock ShapeBaseImageData(LinkOtherImage) { }; LinkOtherImage.stateName[0] = \"Idle\";"
            "function LinkTestImage::onMount(%data, %obj, %slot) { $imgMounted = %slot; $imgMountObj = %obj; }"
            "function LinkTestImage::onUnmount(%data, %obj, %slot) { $imgUnmounted++; }"
            "function LinkTestImage::onFire(%data, %obj, %slot) { $imgFired++; $imgFireSlot = %slot; $imgFireObj = %obj; }"
            "new GameConnection(ImageLinkClient);"
            "new Player(ImageLinkPlayer) { dataBlock = ImageArmor; };"
            "ImageLinkPlayer.setTransform(\"5 6 400 0 0 1 0\");"
            "$imgFired = 0; $imgUnmounted = 0;"
            "ImageLinkPlayer.mountImage(LinkTestImage, 0);"
            "$imgState0 = ImageLinkPlayer.getImageState(0);"
            "$imgMountedId = ImageLinkPlayer.getMountedImage(0);"
            "$imgSlot = ImageLinkPlayer.getMountSlot(LinkTestImage);"
            "$imgIsMounted = ImageLinkPlayer.isImageMounted(LinkTestImage);"
            "$imgLoaded = ImageLinkPlayer.getImageLoaded(0);"
            // Activate disallows image changes: the other image waits.
            "ImageLinkPlayer.mountImage(LinkOtherImage, 0);"
            "$imgPending = ImageLinkPlayer.getPendingImage(0);"
            "ImageLinkPlayer.mountImage(LinkTestImage, 0);"
            "$imgPendingAfter = ImageLinkPlayer.getPendingImage(0);"
            "$imgMuzzlePoint = ImageLinkPlayer.getMuzzlePoint(0);"
            "$imgMuzzleVector = ImageLinkPlayer.getMuzzleVector(0);");
        auto* server = EngineObjects::get<GameConnection>("ImageLinkClient");
        auto* player = EngineObjects::get<PlayerObject>("ImageLinkPlayer");
        assert(server && player);
        ScriptObject* playerObject = ScriptEngine::instance().findObject("ImageLinkPlayer");
        const int playerId = ScriptEngine::instance().objectId(playerObject);
        const int imageId = ScriptEngine::instance().objectId(ScriptEngine::instance().findObject("LinkTestImage"));
        const int otherId = ScriptEngine::instance().objectId(ScriptEngine::instance().findObject("LinkOtherImage"));
        assert(imageId >= 3 && imageId <= 2050);
        assert(script.ts()->getGlobal("$imgMounted").toInt() == 0);
        assert(script.ts()->getGlobal("$imgMountObj").toInt() == playerId);
        assert(script.ts()->getGlobal("$imgState0").toString() == "Activate");
        assert(script.ts()->getGlobal("$imgMountedId").toInt() == imageId);
        assert(script.ts()->getGlobal("$imgSlot").toInt() == 0);
        assert(script.ts()->getGlobal("$imgIsMounted").toInt() == 1);
        assert(script.ts()->getGlobal("$imgLoaded").toInt() == 1); // loaded defaults to true
        assert(script.ts()->getGlobal("$imgPending").toInt() == otherId);
        assert(script.ts()->getGlobal("$imgPendingAfter").toInt() == 0);
        assert(player->getMountedImage(0) && player->getMountedImage(0)->id == imageId);
        // Muzzle: the image offset from the eye-height mount at yaw 0.
        float mx = 0, my = 0, mz = 0;
        std::sscanf(script.ts()->getGlobal("$imgMuzzlePoint").toString().c_str(), "%f %f %f", &mx, &my, &mz);
        assert(std::abs(mx - 5.0f) < 1e-3f && std::abs(my - 6.5f) < 1e-3f && mz > 401.0f && mz < 403.0f);
        assert(script.ts()->getGlobal("$imgMuzzleVector").toString() == "0 1 0");

        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 700.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("ImageLinkClient.setControlObject(ImageLinkPlayer);");
        auto tick = [&](const ClientMoveIn& move) {
            client.pushMove(move);
            client.checkPacketSend(now);
            SimState::advanceServer(now);
            server->checkPacketSend(now + 0.01);
            now += 0.032;
        };
        // Activate times out (0.2 s) into Ready; the ghost goes out with
        // the image mounted (initial update).
        for (int i = 0; i < 15; ++i) tick(ClientMoveIn{});
        assert(std::string(player->getImageState(0)) == "Ready");
        assert(player->images[0].fireCount == 0);
        assert(script.ts()->getGlobal("$imgFired").toInt() == 0);
        const int ghostIndex = server->ghostIndex(ScriptEngine::instance().objectKey(playerObject));
        assert(ghostIndex >= 0);
        const GhostEntry* ghost = parser.getGhostTracker().getGhost(ghostIndex);
        assert(ghost && ghost->mountedImages[0].datablockId == imageId - 3);
        assert(ghost->mountedImages[0].fireCount == 0);
        // One tick of the fire trigger: Ready -> Fire.
        ClientMoveIn fire;
        fire.trigger[0] = true;
        tick(fire);
        assert(script.ts()->getGlobal("$imgFired").toInt() == 1);
        assert(script.ts()->getGlobal("$imgFireSlot").toInt() == 0);
        assert(script.ts()->getGlobal("$imgFireObj").toInt() == playerId);
        assert(player->images[0].fireCount == 1);
        assert(player->isImageFiring(0));
        // Released: Fire -> Reload -> Ready, no second shot.
        for (int i = 0; i < 15; ++i) tick(ClientMoveIn{});
        for (int i = 0; i < 20; ++i, now += 0.1) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.05);
        }
        assert(std::string(player->getImageState(0)) == "Ready");
        assert(script.ts()->getGlobal("$imgFired").toInt() == 1);
        assert(parser.getParseFault().empty());
        ghost = parser.getGhostTracker().getGhost(ghostIndex);
        assert(ghost && ghost->className == "Player");
        assert(ghost->mountedImages[0].datablockId == imageId - 3);
        assert(ghost->mountedImages[0].fireCount == 1);
        assert(ghost->mountedImages[0].loaded && !ghost->mountedImages[0].triggerDown);
        // Unmounting runs onUnmount and empties the slot on the client too.
        script.ts()->execute("ImageLinkPlayer.unmountImage(0); $imgAfterUnmount = ImageLinkPlayer.getMountedImage(0);");
        assert(script.ts()->getGlobal("$imgUnmounted").toInt() == 1);
        assert(script.ts()->getGlobal("$imgAfterUnmount").toInt() == 0);
        for (int i = 0; i < 10; ++i, now += 0.1) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.05);
        }
        assert(parser.getParseFault().empty());
        ghost = parser.getGhostTracker().getGhost(ghostIndex);
        assert(ghost && ghost->mountedImages[0].datablockId == -1);
    }
    {
        // Projectiles (projectile.cc, linearProjectile.cc and the retail
        // proj*.cc): created from script with retail-like datablocks, they
        // tick on the server against a ground plane and a Player, run the
        // datablock's onCollision / onExplode (RadiusExplosion's container
        // search), and ghost in the layout the client's readers decode,
        // explosions included.
        auto savedTriangles = serverCollision().triangles;
        auto savedWater = serverCollision().water;
        // No water here (an earlier block left a lava WaterBlock at 0 0 50).
        serverCollision().water = [](float, float) { return std::numeric_limits<float>::quiet_NaN(); };
        serverCollision().triangles = [](const Point3F&, const Point3F&, std::vector<PlayerPrediction::Triangle>& out) {
            out.push_back({{-1000, -1000, 0}, {1000, -1000, 0}, {1000, 1000, 0}, {0, 0, 1}});
            out.push_back({{-1000, -1000, 0}, {1000, 1000, 0}, {-1000, 1000, 0}, {0, 0, 1}});
        };
        // The same flat ground as terrain for the container's rays.
        auto savedGeometry = serverCollision().geometry;
        serverCollision().geometry = [](const Point3F& lo, const Point3F& hi, bool terrain, bool,
                                        std::vector<PlayerPrediction::Triangle>& out,
                                        std::vector<const ScriptObject*>* owners) {
            if (!terrain) return;
            serverCollision().triangles(lo, hi, out);
            if (owners) owners->resize(out.size(), nullptr);
        };
        script.ts()->execute(
            "datablock PlayerData(ProjArmor) { maxForwardSpeed = 14; runForce = 4200; mass = 90; maxEnergy = 60; "
            "  boxSize = \"1.2 1.2 2.3\"; maxDamage = 1.0; };"
            "datablock LinearProjectileData(ProjDisc) { directDamage = 0.5; hasDamageRadius = true; indirectDamage = 0.5; "
            "  damageRadius = 7.5; kickBackStrength = 1750; dryVelocity = 90; wetVelocity = 50; velInheritFactor = 0.5; "
            "  fizzleTimeMS = 5000; lifetimeMS = 5000; explodeOnDeath = true; };"
            "datablock TracerProjectileData(ProjTracer) { directDamage = 0.1; dryVelocity = 425; wetVelocity = 100; "
            "  fizzleTimeMS = 3000; lifetimeMS = 3000; tracerLength = 15; tracerWidth = 0.1; };"
            "datablock GrenadeProjectileData(ProjGrenade) { hasDamageRadius = true; indirectDamage = 0.4; damageRadius = 15; "
            "  grenadeElasticity = 0.3; grenadeFriction = 0.2; armingDelayMS = 250; muzzleVelocity = 40; gravityMod = 1.9; };"
            "datablock EnergyProjectileData(ProjBolt) { directDamage = 0.15; grenadeElasticity = 0.998; grenadeFriction = 0; "
            "  armingDelayMS = 500; muzzleVelocity = 90; drag = 0.05; gravityMod = 0; };"
            "datablock BombProjectileData(ProjBomb) { hasDamageRadius = true; indirectDamage = 1.1; damageRadius = 30; "
            "  grenadeElasticity = 0.25; grenadeFriction = 0.4; armingDelayMS = 2000; muzzleVelocity = 0.1; drag = 0.3; };"
            "datablock FlareProjectileData(ProjFlare) { grenadeElasticity = 0.35; grenadeFriction = 0.2; armingDelayMS = 6000; "
            "  muzzleVelocity = 15; drag = 0.1; gravityMod = 0.15; };"
            "datablock SeekerProjectileData(ProjMissile) { hasDamageRadius = true; indirectDamage = 0.8; damageRadius = 8; "
            "  lifetimeMS = 6000; muzzleVelocity = 10; maxVelocity = 80; turningSpeed = 110; acceleration = 200; "
            "  proximityRadius = 3; flechetteDelayMs = 550; };"
            "datablock SniperProjectileData(ProjSniper) { directDamage = 0.4; maxRifleRange = 1000; fadeTime = 1.0; };"
            "datablock TargetProjectileData(ProjLaser) { maxRifleRange = 1000; };"
            "datablock ShockLanceProjectileData(ProjLance) { zapDuration = 1.0; boltLength = 16.0; };"
            "datablock ELFProjectileData(ProjElf) { beamRange = 75; };"
            "datablock RepairProjectileData(ProjRepair) { beamRange = 10; };"
            "function ProjectileData::onCollision(%data, %proj, %col, %mod, %pos, %normal) {"
            "  $projCollision = $projCollision @ %data.getName() SPC %col SPC %mod @ \";\"; }"
            "function ProjectileData::onExplode(%data, %proj, %pos, %mod) {"
            "  $projExplode = $projExplode @ %data.getName() @ \";\";"
            "  InitContainerRadiusSearch(%pos, %data.damageRadius, $TypeMasks::PlayerObjectType);"
            "  while ((%t = containerSearchNext()) != 0)"
            "    $projRadius = $projRadius @ %t SPC containerSearchCurrRadDamageDist() SPC"
            "      calcExplosionCoverage(%pos, %t, $TypeMasks::TerrainObjectType) @ \";\"; }"
            "function ProjElf::zapTarget(%data, %proj, %target, %targeter) { $projZap = %target SPC %targeter; }"
            "function ProjElf::unzapTarget(%data, %proj, %target, %targeter) { $projUnzap = %target; }"
            // A burst of twelve projectiles: OptionsDlg's T1/LAN preset
            // ghosts them all before they expire.
            "$pref::Net::PacketRateToClient = 32; $pref::Net::PacketSize = 450; $pref::Net::PacketRateToServer = 32;"
            "new GameConnection(ProjClient);"
            "new Camera(ProjCamera) { position = \"0 0 20\"; };"
            "new Player(ProjTarget) { dataBlock = ProjArmor; };"
            "ProjTarget.setTransform(\"0 40 0 0 0 1 0\");"
            "new Player(ProjShooter) { dataBlock = ProjArmor; };"
            "ProjShooter.setTransform(\"0 -5 0 0 0 1 0\");"
            "new Player(ProjZapper) { dataBlock = ProjArmor; };"
            "ProjZapper.setTransform(\"0 -5 5 0 0 1 0\");");
        auto* server = EngineObjects::get<GameConnection>("ProjClient");
        assert(server);
        GameConnection client;
        client.isServer = false;
        script.ts()->execute("$pref::Net::PacketRateToClient = \"\"; $pref::Net::PacketSize = \"\"; "
                             "$pref::Net::PacketRateToServer = \"\";");
        DemoParser parser;
        parser.consumeExplosions();
        int packets = 0;
        std::set<std::string> classes;
        std::vector<DemoParser::PendingExplosion> explosions;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
            for (auto& e : parser.consumeExplosions()) explosions.push_back(e);
            for (int i = 0; i < 1024; ++i)
                if (const GhostEntry* g = parser.getGhostTracker().getGhost(i))
                    if (g->className.find("Projectile") != std::string::npos) classes.insert(g->className);
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 700.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("ProjCamera.scopeToClient(ProjClient); ProjClient.setControlObject(ProjCamera);");
        auto step = [&](int ticks) {
            for (int tick = 0; tick < ticks; ++tick) {
                SimState::advanceServer(now);
                server->checkPacketSend(now + 0.01);
                client.checkPacketSend(now + 0.02);
                now += 0.032;
            }
        };
        step(1);
        // (Torch's parser does not take `new (%class)()` yet, so each is spelled out.)
        auto fire = [&](const char* var, const char* cls, const char* data, const char* pos, const char* dir,
                        const char* shooter = "ProjShooter") {
            script.ts()->execute(std::string("$") + var + " = new " + cls + "() { dataBlock = " + data +
                                 "; initialPosition = \"" + pos + "\"; initialDirection = \"" + dir +
                                 "\"; sourceObject = " + shooter + "; sourceSlot = 0; vehicleObject = 0; };"
                                 "$" + var + ".scopeToClient(ProjClient);");
        };
        fire("projDisc", "LinearProjectile", "ProjDisc", "0 0 1", "0 1 0");
        fire("projGren", "GrenadeProjectile", "ProjGrenade", "0 0 5", "1 0 0");
        fire("projTracer", "TracerProjectile", "ProjTracer", "5 0 1", "0 0 -1");
        fire("projBolt", "EnergyProjectile", "ProjBolt", "-5 0 1", "0 -1 0");
        fire("projBomb", "BombProjectile", "ProjBomb", "10 10 8", "0 0 -1");
        fire("projFlare", "FlareProjectile", "ProjFlare", "-10 -10 3", "0 0 1");
        fire("projMissile", "SeekerProjectile", "ProjMissile", "0 0 3", "0 1 0");
        fire("projSniper", "SniperProjectile", "ProjSniper", "0 0 1", "0 1 0");
        fire("projLaser", "TargetProjectile", "ProjLaser", "0 0 1", "0 1 0");
        fire("projLance", "ShockLanceProjectile", "ProjLance", "0 38 1", "0 1 0");
        // The ELF's source is in the air when it fires (no image is mounted, so
        // the muzzle is the shape's origin).
        fire("projElf", "ELFProjectile", "ProjElf", "0 0 1", "0 1 0", "ProjZapper");
        script.ts()->execute(
            "$projMissile.setObjectTarget(ProjTarget);"
            "$projMissileTarget = $projMissile.getTargetObject();"
            "$projSniper.setEnergyPercentage(0.5);"
            "$projRepair = new RepairProjectile() { dataBlock = ProjRepair; initialPosition = \"0 0 1\"; "
            "  initialDirection = \"0 1 0\"; sourceObject = ProjShooter; sourceSlot = 0; targetObject = ProjTarget; };"
            "$projRepair.scopeToClient(ProjClient);");
        // The sniper hit the target in onAdd.
        const int targetId = ScriptEngine::instance().objectId(ScriptEngine::instance().findObject("ProjTarget"));
        assert(script.ts()->getGlobal("$projCollision").toString().find("ProjSniper " + std::to_string(targetId)) !=
               std::string::npos);
        assert(script.ts()->getGlobal("$projMissileTarget").toInt() == targetId);
        const std::string discKey = script.ts()->getGlobal("$projDisc").toString();
        auto* disc = EngineObjects::get<LinearProjectileObject>(discKey);
        assert(disc && !disc->segments[0].cutShort && disc->numSegments == 1);
        step(60);
        assert(parser.getParseFault().empty());
        // The disc struck the Player (a dynamic hit: the explosion update).
        const std::string collisions = script.ts()->getGlobal("$projCollision").toString();
        assert(collisions.find("ProjDisc " + std::to_string(targetId) + " 1") != std::string::npos);
        const std::string exploded = script.ts()->getGlobal("$projExplode").toString();
        assert(exploded.find("ProjDisc;") != std::string::npos);
        assert(exploded.find("ProjGrenade;") != std::string::npos);
        assert(exploded.find("ProjTracer;") != std::string::npos);
        assert(exploded.find("ProjBomb;") != std::string::npos);
        assert(exploded.find("ProjBolt") == std::string::npos); // unarmed bolt: mirrored off the ground
        // RadiusExplosion's search found the target, in the open.
        const std::string radius = script.ts()->getGlobal("$projRadius").toString();
        assert(radius.find(std::to_string(targetId) + " ") != std::string::npos);
        assert(radius.find(" 1;") != std::string::npos);
        // The ELF latched onto the Player in front of it.
        assert(script.ts()->getGlobal("$projZap").toString().find(std::to_string(targetId)) == 0);
        bool discExplosion = false, groundExplosion = false;
        for (const auto& e : explosions) {
            if (std::fabs(e.position.y - 39.4f) < 0.2f && std::fabs(e.position.z - 1.0f) < 0.1f) discExplosion = true;
            if (std::fabs(e.position.z) < 0.1f && e.normal.z > 0.99f) groundExplosion = true;
        }
        assert(discExplosion && groundExplosion);
        for (const char* cls : {"LinearProjectile", "TracerProjectile", "GrenadeProjectile", "EnergyProjectile",
                                "BombProjectile", "FlareProjectile", "SeekerProjectile", "SniperProjectile",
                                "TargetProjectile", "ShockLanceProjectile", "ELFProjectile", "RepairProjectile"})
            assert(classes.count(cls));
        // A spent grenade is deleted DeleteWaitTicks after it explodes; a
        // linear projectile stays hidden until its forecast end (+13 ticks).
        step(40);
        disc = EngineObjects::get<LinearProjectileObject>(discKey);
        assert(disc && disc->hidden);
        assert(!ScriptEngine::instance().findObject(script.ts()->getGlobal("$projGren").toString().c_str()));
        assert(parser.getParseFault().empty());
        // (the delete() native reaches for the client World, absent here)
        for (const char* var : {"$projElf", "$projLaser", "$projRepair", "$projFlare"})
            ScriptEngine::instance().deleteScriptObject(script.ts()->getGlobal(var).toString());
        assert(script.ts()->getGlobal("$projUnzap").toInt() == targetId);
        serverCollision().triangles = savedTriangles;
        serverCollision().geometry = savedGeometry;
        serverCollision().water = savedWater;
    }
    {
        // Vehicles (vehicle.cc, hoverVehicle.cc, flyingVehicle.cc): the
        // Rigid body over a ground plane. A HoverVehicle settles on its
        // stabilizer springs, a FlyingVehicle holds its height on the
        // hovering jet, and the hover's ghost decodes in the client's reader.
        auto savedTriangles = serverCollision().triangles;
        auto savedWater = serverCollision().water;
        serverCollision().water = [](float, float) { return std::numeric_limits<float>::quiet_NaN(); };
        serverCollision().triangles = [](const Point3F&, const Point3F&, std::vector<PlayerPrediction::Triangle>& out) {
            out.push_back({{-1000, -1000, 0}, {1000, -1000, 0}, {1000, 1000, 0}, {0, 0, 1}});
            out.push_back({{-1000, -1000, 0}, {1000, 1000, 0}, {-1000, 1000, 0}, {0, 0, 1}});
        };
        // The same flat ground as terrain for the container's rays.
        auto savedGeometry = serverCollision().geometry;
        serverCollision().geometry = [](const Point3F& lo, const Point3F& hi, bool terrain, bool,
                                        std::vector<PlayerPrediction::Triangle>& out,
                                        std::vector<const ScriptObject*>* owners) {
            if (!terrain) return;
            serverCollision().triangles(lo, hi, out);
            if (owners) owners->resize(out.size(), nullptr);
        };
        script.ts()->execute(
            "datablock HoverVehicleData(TestHover) { mass = 400; dragForce = 25 / 45.0; mainThrustForce = 30; "
            "  strafeThrustForce = 20; stabLenMin = 2.25; stabLenMax = 3.75; stabSpringConstant = 30; "
            "  stabDampingConstant = 16; floatingGravMag = 3.5; maxEnergy = 150; };"
            "datablock FlyingVehicleData(TestFlyer) { mass = 150; maxAutoSpeed = 15; autoAngularForce = 400; "
            "  autoLinearForce = 300; hoverHeight = 5; jetForce = 3000; maxEnergy = 150; };"
            "new GameConnection(VehLinkClient);"
            "new HoverVehicle(TestHoverV) { dataBlock = TestHover; position = \"0 0 6\"; };"
            "new FlyingVehicle(TestFlyerV) { dataBlock = TestFlyer; position = \"30 0 12\"; };"
            "$hoverClass = TestHoverV.getDataBlock().className;");
        // GameBaseData::onAdd: className defaults to the C++ class name.
        assert(script.ts()->getGlobal("$hoverClass").toString() == "HoverVehicleData");
        auto* hover = EngineObjects::get<HoverVehicleObject>("TestHoverV");
        auto* flyer = EngineObjects::get<FlyingVehicleObject>("TestFlyerV");
        assert(hover && flyer);
        for (int i = 0; i < 250; ++i) {
            hover->processMove(nullptr);
            flyer->processMove(nullptr);
        }
        // The hover's box floats above the ground, at rest.
        assert(hover->transform[11] > 0.05f && hover->transform[11] < 4.0f);
        assert(std::abs(hover->getVelocity().z) < 0.5f);
        assert(std::abs(flyer->transform[11] - 12.0f) < 1.0f);

        auto* server = EngineObjects::get<GameConnection>("VehLinkClient");
        assert(server);
        GameConnection client;
        client.isServer = false;
        DemoParser parser;
        int packets = 0;
        server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
        client.onServerPacket = [&](const std::vector<uint8_t>& p) {
            PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
            for (const auto& ev : pd.events)
                if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
        };
        client.deliver = [&](const std::vector<uint8_t>& p) {
            parser.onSendPacketTrigger();
            server->receivePacket(p.data(), p.size());
        };
        server->activateGhosting();
        double now = 900.0;
        for (int i = 0; i < 20 && !server->isGhosting(); ++i, now += 1.0) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.5);
        }
        assert(server->isGhosting());
        script.ts()->execute("TestHoverV.scopeToClient(VehLinkClient); TestFlyerV.scopeToClient(VehLinkClient);");
        for (int i = 0; i < 20; ++i, now += 0.2) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.1);
        }
        assert(parser.getParseFault().empty());
        const int hoverGhost = server->ghostIndex(ScriptEngine::instance().objectKey(hover->script));
        const int flyerGhost = server->ghostIndex(ScriptEngine::instance().objectKey(flyer->script));
        assert(hoverGhost >= 0 && flyerGhost >= 0);
        const GhostEntry* ghost = parser.getGhostTracker().getGhost(hoverGhost);
        assert(ghost && ghost->className == "HoverVehicle");
        assert(std::abs(ghost->position.z - hover->transform[11]) < 0.05f);
        assert(parser.getGhostTracker().getGhost(flyerGhost)->className == "FlyingVehicle");
        {
            // Vehicle::advanceToCollision: a player in the hover's way is
            // pushed along (Player::displaceObject), not run into.
            script.ts()->execute("datablock PlayerData(PushArmor) { mass = 90; maxEnergy = 60; boxSize = \"1.2 1.2 2.3\"; };"
                                 "new Player(PushPlayer) { dataBlock = PushArmor; };");
            auto* pushed = EngineObjects::get<PlayerObject>("PushPlayer");
            assert(pushed);
            const float startX = hover->transform[3] + 5.0f;
            pushed->setTransform({1, 0, 0, startX, 0, 1, 0, hover->transform[7], 0, 0, 1, 0, 0, 0, 0, 1});
            for (int i = 0; i < 10; ++i) pushed->processMove(nullptr);
            const float groundX = pushed->transform[3];
            hover->applyImpulse({hover->transform[3], hover->transform[7], hover->transform[11]}, {400.0f * 15.0f, 0, 0});
            float maxX = groundX;
            for (int i = 0; i < 40; ++i) {
                hover->processMove(nullptr);
                pushed->processMove(nullptr);
                maxX = std::max(maxX, pushed->transform[3]);
            }
            assert(maxX > groundX + 1.0f);
            ScriptEngine::instance().deleteScriptObject("PushPlayer");
        }
        for (const char* name : {"TestHoverV", "TestFlyerV"}) ScriptEngine::instance().deleteScriptObject(name);
        serverCollision().triangles = savedTriangles;
        serverCollision().geometry = savedGeometry;
        serverCollision().water = savedWater;
    }
    {
        // ProjectileData::calculateAim: a linear shot at a still target goes
        // straight at it; a grenade's arc lands on it (the quartic solver);
        // a sniper shot past maxRifleRange cannot be aimed.
        script.ts()->execute(
            "datablock LinearProjectileData(AimDisc) { dryVelocity = 90; velInheritFactor = 0.5; lifetimeMS = 5000; };"
            "datablock GrenadeProjectileData(AimGren) { muzzleVelocity = 47; velInheritFactor = 0.5; gravityMod = 1.0; "
            "  lifetimeMS = 5000; };"
            "datablock SniperProjectileData(AimSnipe) { maxRifleRange = 1000; };");
        Point3F vMin, vMax;
        float tMin, tMax;
        auto* disc = ScriptEngine::instance().findObject("AimDisc");
        assert(ProjectileAim::calculateAim(disc, {0, 90, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, &vMin, &tMin, &vMax, &tMax));
        assert(std::abs(vMin.y - 1.0f) < 1e-4f && std::abs(tMin - 1.0f) < 1e-3f);
        auto* gren = ScriptEngine::instance().findObject("AimGren");
        assert(ProjectileAim::calculateAim(gren, {0, 60, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, &vMin, &tMin, &vMax, &tMax));
        // Fired along vMin at 47 m/s under 9.81 m/s^2, it lands at (0, 60, 0).
        const float x = vMin.y * 47 * tMin, z = vMin.z * 47 * tMin - 0.5f * 9.81f * tMin * tMin;
        assert(std::abs(x - 60.0f) < 0.05f && std::abs(z) < 0.05f && vMin.z > 0);
        auto* snipe = ScriptEngine::instance().findObject("AimSnipe");
        assert(!ProjectileAim::calculateAim(snipe, {0, 1500, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, &vMin, &tMin, &vMax, &tMax));
        assert(ProjectileAim::calculateAim(snipe, {0, 500, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, &vMin, &tMin, &vMax, &tMax));
    }
    {
        // ForceFieldBare: closed, its box blocks every mover (and rays);
        // open() fades it over fadeMS and then it blocks only vehicles.
        script.ts()->execute(
            "datablock ForceFieldBareData(TestField) { fadeMS = 64; teamPermiable = true; };"
            "new ForceFieldBare(TestFieldF) { dataBlock = TestField; position = \"0 0 0\"; scale = \"4 0.25 4\"; };"
            "new Player(TestFieldP) { dataBlock = ProjArmor; };");
        auto* field = EngineObjects::get<ForceFieldBareObject>("TestFieldF");
        auto* walker = EngineObjects::get<PlayerObject>("TestFieldP");
        assert(field && walker && field->state == ForceFieldBareObject::Closed);
        std::vector<PlayerPrediction::Triangle> tris;
        ForceFields::gather(nullptr, {-1, -1, -1}, {1, 1, 1}, tris);
        assert(tris.size() == 12);
        // Same sensor group (no targets: 0) and teamPermiable: the player passes.
        tris.clear();
        ForceFields::gather(walker, {-1, -1, -1}, {1, 1, 1}, tris);
        assert(tris.empty());
        script.ts()->execute("TestFieldF.open();");
        assert(field->state == ForceFieldBareObject::Opening);
        for (int i = 0; i < 3; ++i) field->processMove(nullptr);
        assert(field->isOpen());
        tris.clear();
        ForceFields::gather(nullptr, {-1, -1, -1}, {1, 1, 1}, tris);
        assert(tris.empty());
        for (const char* name : {"TestFieldF", "TestFieldP"}) ScriptEngine::instance().deleteScriptObject(name);
    }
    {
        // netDispatch.cc over real loopback UDP: setNetPort opens the
        // server's port, connect() runs the challenge / connect request, the
        // server's GameConnection gets onConnect(args...) and the client's
        // ServerConnection the server's packets.
        TorqueScript* ts = script.ts();
        uint16_t port = 0;
        {
            const int probe = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            assert(bind(probe, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0);
            socklen_t length = sizeof local;
            assert(getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0);
            port = ntohs(local.sin_port);
            close(probe);
        }
        ts->execute(
            "function GameConnection::onConnect(%client, %name, %raceGender, %skin, %voice, %voicePitch) {"
            "  $netConnectArgs = %name @ \"|\" @ %raceGender @ \"|\" @ %skin @ \"|\" @ %voice @ \"|\" @ %voicePitch;"
            "  $netClient = %client; $netClientAddress = %client.getAddress();"
            "  commandToClient(%client, 'NetHello', \"world\", 42); }"
            "function GameConnection::onDrop(%client, %reason) { $netDrop = %client @ \":\" @ %reason; }"
            "function ServerConnectionAccepted() { $netAccepted = $netAccepted + 1; }"
            "function onChallengeRequestRejected(%msg) { $netChallengeReject = %msg; }"
            "function onConnectRequestRejected(%msg) { $netConnectReject = %msg; }"
            "function onConnectionToServerLost(%msg) { $netLost = %msg; }");
        ts->setGlobal("$Host::MaxPlayers", VMValue(16));
        ts->setGlobal("$HostGamePlayerCount", VMValue(0));
        ts->setGlobal("$Host::Password", VMValue(""));
        ts->setGlobal("$ServerName", VMValue("Torch Loopback"));
        ts->callFunction("setNetPort", {VMValue((int)port)});
        ts->callFunction("allowConnections", {VMValue(1)});

        GameConnection* clientConnection = nullptr;
        std::vector<std::vector<uint8_t>> clientPackets;
        auto savedStarted = gLocalClientStarted;
        gLocalClientStarted = [&](GameConnection& connection) {
            clientConnection = &connection;
            connection.onServerPacket = [&](const std::vector<uint8_t>& packet) { clientPackets.push_back(packet); };
        };
        auto pump = [&](const std::function<bool()>& done) {
            bool finished = false;
            for (int i = 0; i < 1500 && !(finished = done()); ++i) {
                const double now = Timer::now();
                netInterfaceProcess(now);
                serverNetProcess(now);
                clientNetProcess(now);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return finished;
        };
        const std::string address = "127.0.0.1:" + std::to_string(port);
        ts->callFunction("connect", {VMValue(address), VMValue(""), VMValue("Tester"), VMValue("Human Male"),
                                     VMValue("beagle"), VMValue("Male1")});
        DemoParser parser;
        bool hello = false;
        assert(pump([&] {
            for (const auto& packet : clientPackets) {
                PacketData pd = parser.parsePacket(packet.data(), packet.size(), -1);
                for (const auto& event : pd.events)
                    if (event.classId == T2Demo::NetEventClassFirst + 9 && event.arguments.size() == 3 &&
                        event.arguments[0] == "NetHello" && event.arguments[1] == "world" &&
                        event.arguments[2] == "42")
                        hello = true;
            }
            clientPackets.clear();
            return hello;
        }));
        assert(ts->getGlobal("$netConnectArgs").toString() == "Tester|Human Male|beagle|Male1|");
        assert(ts->getGlobal("$netClientAddress").toString().rfind("IP:127.0.0.1:", 0) == 0);
        assert(ts->getGlobal("$netAccepted").toInt() == 1);
        assert(clientConnection && !clientConnection->isServer);
        auto* serverSide = EngineObjects::get<GameConnection>(ts->getGlobal("$netClient").toString());
        assert(serverSide && serverSide->isServer && serverSide->address.rfind("IP:127.0.0.1:", 0) == 0);
        auto* named = EngineObjects::get<GameConnection>("ServerConnection");
        assert(named == clientConnection && named->address == "IP:127.0.0.1:" + std::to_string(port));
        ScriptObject* clientGroup = ScriptEngine::instance().findObject("ClientGroup");
        assert(clientGroup);

        // handleGamePingRequest: the server answers from $ServerName.
        {
            const int query = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            server.sin_port = htons(port);
            const auto request = V12::buildGameQuery(V12::OobGamePingRequest, 0, 0x1234);
            sendto(query, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&server), sizeof server);
            fcntl(query, F_SETFL, O_NONBLOCK);
            uint8_t reply[1500];
            ssize_t got = -1;
            pump([&] { got = recv(query, reply, sizeof reply, 0); return got > 0; });
            assert(got > 6 && reply[0] == 16);
            V12BitStream stream(reply + 1, (size_t)got - 1);
            stream.readU8();
            assert(stream.readU32() == 0x1234);
            assert(stream.readHuffmanString() == "VER5");
            assert(stream.readU32() == V12::ProtocolVersion);
            stream.readU32();
            stream.readU32();
            assert(stream.readHuffmanString() == "Torch Loopback");
            close(query);
        }

        // Disconnect() deletes the ServerConnection: GameConnection::onRemove
        // sends the Disconnect, and the server drops the client.
        const std::string serverKey = ts->getGlobal("$netClient").toString();
        ts->execute("ServerConnection.delete();");
        assert(!ScriptEngine::instance().findObject("ServerConnection"));
        assert(pump([&] { return !ts->getGlobal("$netDrop").toString().empty(); }));
        assert(ts->getGlobal("$netDrop").toString() == serverKey + ":");
        assert(!EngineObjects::get<GameConnection>(serverKey));

        // A server with a password turns a wrong one away at the challenge.
        ts->setGlobal("$Host::Password", VMValue("secret"));
        ts->callFunction("connect", {VMValue("IP:" + address), VMValue("wrong"), VMValue("Tester")});
        assert(pump([&] { return !ts->getGlobal("$netChallengeReject").toString().empty(); }));
        assert(ts->getGlobal("$netChallengeReject").toString() == "PASSWORD");
        assert(ts->getGlobal("$netAccepted").toInt() == 1);

        // A full server rejects the connect request.
        ts->setGlobal("$Host::Password", VMValue(""));
        ts->setGlobal("$Host::MaxPlayers", VMValue(0));
        ts->callFunction("connect", {VMValue(address), VMValue(""), VMValue("Tester")});
        assert(pump([&] { return !ts->getGlobal("$netConnectReject").toString().empty(); }));
        assert(ts->getGlobal("$netConnectReject").toString() == "CR_SERVERFULL");
        assert(ts->getGlobal("$netAccepted").toInt() == 1);

        ts->callFunction("allowConnections", {VMValue(0)});
        gLocalClientStarted = savedStarted;
    }
    return 0;
}
