// Server TargetManager (game/targetManager.cc) over a loopback link: the
// target table from script natives, and the target events every packet
// carries decoding in the demo reader.
#include "sim/game_connection.h"
#include "sim/net_string_table.h"
#include "sim/player.h"
#include "sim/sim_state.h"
#include "sim/target_manager.h"
#include "game/demo.h"
#include "script/script_engine.h"
#include "script/torquescript.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

int main() {
    ScriptEngine script;
    assert(script.init());
    TorqueScript& ts = *script.ts();
    registerTargetManagerNatives(ts);
    auto global = [&](const char* name) { return ts.getGlobal(name).toString(); };

    ts.execute(
        "datablock PlayerData(TMArmor) { maxForwardSpeed = 14; runForce = 4200; mass = 90; "
        "  maxEnergy = 60; boxSize = \"1.2 1.2 2.3\"; };"
        "datablock SensorData(TMSensor) { detects = true; };"
        "datablock AudioDescription(TMAudio) { volume = 1.0; is3D = true; maxDistance = 50; };"
        "new GameConnection(TMClient);"
        "new Player(TMPlayer) { dataBlock = TMArmor; };"
        "TMPlayer.setTransform(\"5 6 400 0 0 1 0\");"
        "$armor = TMArmor.getId();"
        // Before the client has its datablocks nothing goes out.
        "$t0 = allocTarget('Alice', 'beagle', 'Male1', '_ClientConnection', 3, $armor, 1.25, 'beagle');"
        "$t1 = allocTarget('Bob', 'swolf', 'Male2', 'Heavy', 4, 0, 1.0);"
        "setTargetSensorData($t0, TMSensor);"
        "$name = getTaggedString(getTargetName($t0));"
        "$gameName = getTargetGameName($t0); $gameName1 = getTargetGameName($t1);"
        "$group = getTargetSensorGroup($t0); $db = getTargetDataBlock($t0);"
        "$pitch = getTargetVoicePitch($t0); $sensor = getTargetSensorData($t0);"
        "$friendly = isTargetFriendly($t0, 3) @ isTargetFriendly($t0, 4);"
        "$visible = isTargetVisible($t0, 3) @ isTargetVisible($t0, 4);"
        "$received = TMClient.getReceivedDataBlocks();"
        "$teamTarget = getTargetSensorGroup(7);"
        "$badGroup = setTargetSensorGroup(5, 2);");
    const int t0 = ts.getGlobal("$t0").toInt(), t1 = ts.getGlobal("$t1").toInt();
    assert(t0 == 32 && t1 == 33);
    assert(global("$name") == "Alice");
    assert(global("$gameName") == "Alice"); // '_' types are left out
    assert(global("$gameName1") == "Bob Heavy");
    assert(ts.getGlobal("$group").toInt() == 3);
    assert(ts.getGlobal("$db").toInt() == ts.getGlobal("$armor").toInt());
    assert(std::abs(ts.getGlobal("$pitch").toFloat() - 1.25f) < 1e-6f);
    assert(ts.getGlobal("$sensor").toInt() == script.objectId(script.findObject("TMSensor")));
    assert(global("$friendly") == "10" && global("$visible") == "10");
    assert(ts.getGlobal("$received").toInt() == 0);
    assert(ts.getGlobal("$teamTarget").toInt() == 7);

    auto* server = EngineObjects::get<GameConnection>("TMClient");
    auto* player = EngineObjects::get<PlayerObject>("TMPlayer");
    assert(server && player);
    GameConnection client;
    client.isServer = false;
    DemoParser parser;
    int packets = 0;
    std::vector<NetEventInfo> events;
    server->deliver = [&](const std::vector<uint8_t>& p) { client.receivePacket(p.data(), p.size()); };
    client.onServerPacket = [&](const std::vector<uint8_t>& p) {
        PacketData pd = parser.parsePacket(p.data(), p.size(), packets++);
        for (const auto& ev : pd.events) {
            if (ev.ghostMessage >= 0) client.clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
            events.push_back(ev);
        }
    };
    client.deliver = [&](const std::vector<uint8_t>& p) {
        parser.onSendPacketTrigger();
        server->receivePacket(p.data(), p.size());
    };
    double now = 100.0;
    auto pump = [&](int count) {
        for (int i = 0; i < count; ++i, now += 0.1) {
            server->checkPacketSend(now);
            client.checkPacketSend(now + 0.05);
        }
    };
    auto count = [&](int classIndex) {
        int n = 0;
        for (const auto& ev : events) n += ev.classId == T2Demo::NetEventClassFirst + classIndex;
        return n;
    };
    pump(3);
    assert(parser.getParseFault().empty());
    assert(count(ServerTargets::TargetInfoEvent) == 0 && count(ServerTargets::TargetFreeEvent) == 0);

    // GameConnection::dataBlocksDone: the flag, then every target.
    ts.execute("TMClient.setReceivedDataBlocks(true); $received = TMClient.getReceivedDataBlocks();"
               "sendTargetsToClient(TMClient);");
    assert(ts.getGlobal("$received").toInt() == 1);
    pump(5);
    assert(parser.getParseFault().empty());
    std::map<int, const NetEventInfo*> infos;
    int teamTargets = 0;
    for (const auto& ev : events)
        if (ev.hasTargetInfo) {
            infos[ev.targetId] = &ev;
            teamTargets += ev.targetId < 32;
        }
    assert(teamTargets == 32 && infos.size() == 34);
    {
        const NetEventInfo& alice = *infos.at(t0);
        assert(alice.targetName == "Alice" && alice.targetSkin == "beagle");
        assert(alice.targetVoice == "Male1" && alice.targetType == "_ClientConnection");
        assert(alice.targetSensorGroup == 3);
        // Datablock ids go out as id - DataBlockObjectIdFirst.
        assert(alice.targetDataBlockId == ts.getGlobal("$armor").toInt() - 3);
        assert(alice.hasTargetRenderFlags && alice.targetRenderFlags == 0);
        assert(std::abs(alice.targetVoicePitch - 1.25f) < 0.02f);
        const NetEventInfo& bob = *infos.at(t1);
        assert(bob.targetName == "Bob" && bob.targetSensorGroup == 4);
        assert(bob.targetDataBlockId == -2); // present, none
        assert(infos.at(7)->targetSensorGroup == 7 && infos.at(7)->targetName.empty());
    }

    // Changes go out as partial TargetInfoEvents; the client's sensor group
    // and its colours; a freed target.
    events.clear();
    ts.execute("setTargetSkin($t0, 'swolf'); setTargetSensorGroup($t0, 5); setTargetRenderMask($t0, 1 << $TargetInfo::HudRenderStart);"
               "setTargetDataBlock($t1, TMArmor.getId()); setTargetVoicePitch($t1, 0.75);"
               "setTargetName($t0, 'Alice');" // unchanged: nothing sent
               "TMClient.setSensorGroup(3); $clientGroup = TMClient.getSensorGroup();"
               "setSensorGroupColor(3, 1 << 5, \"0 255 0 255\"); $color = getSensorGroupColor(3, 5);"
               "setSensorGroupColor(4, 1 << 5, \"0 0 255 255\");" // TMClient is not in group 4
               "freeTarget($t1); $t1Name = getTargetName($t1);");
    assert(ts.getGlobal("$clientGroup").toInt() == 3);
    assert(global("$color") == "0 255 0 255");
    pump(5);
    assert(parser.getParseFault().empty());
    int skin = 0, group = 0, render = 0, datablock = 0, pitch = 0, other = 0;
    for (const auto& ev : events) {
        if (!ev.hasTargetInfo) continue;
        if (ev.targetId == t0 && ev.targetSkin == "swolf" && ev.targetName.empty()) ++skin;
        else if (ev.targetId == t0 && ev.targetSensorGroup == 5) ++group;
        else if (ev.targetId == t0 && ev.hasTargetRenderFlags && ev.targetRenderFlags == 1) ++render;
        else if (ev.targetId == t1 && ev.targetDataBlockId == ts.getGlobal("$armor").toInt() - 3) ++datablock;
        else if (ev.targetId == t1 && std::abs(ev.targetVoicePitch - 0.75f) < 0.02f) ++pitch;
        else ++other;
    }
    assert(skin == 1 && group == 1 && render == 1 && datablock == 1 && pitch == 1 && other == 0);
    assert(count(ServerTargets::SetSensorGroupEvent) == 1);
    assert(count(ServerTargets::SensorGroupColorEvent) == 2); // on joining group 3, then the change
    assert(count(ServerTargets::TargetFreeEvent) == 1);
    for (const auto& ev : events)
        if (ev.hasTargetFree) assert(ev.targetId == t1);
    const auto& colors = parser.getSensorGroupColors();
    assert(colors.at({3, 1u << 5}) == 0xff00ff00u); // r, g, b, a bytes
    assert(!colors.count({4, 1u << 5}));

    // GameBase::setTarget: the ghost carries the target, and the reader
    // applies the target's name, skin and sensor group to it.
    events.clear();
    ts.execute("TMPlayer.setTarget($t0); $objTarget = TMPlayer.getTarget(); $targetObject = getTargetObject($t0);");
    assert(ts.getGlobal("$objTarget").toInt() == t0);
    assert(ts.getGlobal("$targetObject").toInt() == script.objectId(script.findObject("TMPlayer")));
    assert(player->targetId == t0);
    server->activateGhosting();
    for (int i = 0; i < 20 && !server->isGhosting(); ++i) pump(1);
    assert(server->isGhosting());
    ts.execute("TMClient.setControlObject(TMPlayer);");
    for (int tick = 0; tick < 5; ++tick) {
        client.pushMove(ClientMoveIn{});
        client.checkPacketSend(now);
        SimState::advanceServer(now);
        server->checkPacketSend(now + 0.01);
        now += 0.032;
    }
    pump(10);
    assert(parser.getParseFault().empty());
    const int ghostIndex = server->ghostIndex(script.objectKey(script.findObject("TMPlayer")));
    assert(ghostIndex >= 0);
    const GhostEntry* ghost = parser.getGhostTracker().getGhost(ghostIndex);
    assert(ghost && ghost->className == "Player");
    assert(ghost->targetId == t0);
    // (The reader's NetEventInfo::targetSensorGroup defaults to 0, not -1,
    // so a partial TargetInfoEvent without a group, like the render-mask
    // one above, resets its target's group: the group is checked per event.)
    assert(ghost->playerName == "Alice" && ghost->skinName == "swolf");

    // Task targets, HUD task removal, and resets.
    events.clear();
    ts.execute("TMClient.setTargetId($t0); TMClient.setTargetPos(\"1 2 3\"); $tid = TMClient.getTargetId();"
               "TMClient.sendTargetTo(TMClient, true);"
               "removeClientTargetType(TMClient, \"AssignedTask\");"
               "resetClientTargets(TMClient, true);"
               "playTargetAudio($t0, addTaggedString(\"hurt\"), TMAudio, false);"
               "resetTargets(); $afterReset = getTargetName($t0);");
    assert(ts.getGlobal("$tid").toInt() == t0);
    pump(5);
    assert(parser.getParseFault().empty());
    assert(count(ServerTargets::TargetToEvent) == 1);
    assert(count(ServerTargets::RemoveClientTargetTypeEvent) == 1);
    // The controlling client hears it; the player is ghosted to it, so no
    // position goes along.
    assert(count(ServerTargets::SimTargetAudioEvent) == 1);
    for (const auto& ev : events)
        if (ev.classId == T2Demo::NetEventClassFirst + ServerTargets::SimTargetAudioEvent)
            assert(ev.targetId == t0 && !ev.hasAudioPosition);
    assert(count(ServerTargets::ResetClientTargetsEvent) == 2);
    assert(ts.getGlobal("$afterReset").toInt() == -1); // unallocated
    assert(ServerTargets::serverTarget(t0) && !ServerTargets::serverTarget(t0)->allocated);

    std::printf("target_manager_test: %d packets, OK\n", packets);
    return 0;
}
