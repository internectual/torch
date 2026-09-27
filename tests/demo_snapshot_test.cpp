#include "game/demo.h"
#include "core/console.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Demo parsing logs errors through Console. Keep this test independent from the
// engine and script runtime, which are not needed to exercise parser state.
Console::Console() : impl(nullptr) {}
Console::~Console() {}

Console& Console::instance() {
    static Console console;
    return console;
}

// Set TORCH_TEST_VERBOSE to see parser diagnostics.
void Console::printf(LogLevel, const char* format, ...) {
    static const bool verbose = std::getenv("TORCH_TEST_VERBOSE") != nullptr;
    if (!verbose) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

static bool check(bool value, const char* expression) {
    if (!value) std::cerr << "failed: " << expression << "\n";
    return value;
}

#define CHECK(expr) if (!check((expr), #expr)) return 1

static std::vector<DemoBlock> readBlocks(DemoParser& parser, int count) {
    std::vector<DemoBlock> blocks;
    for (int i = 0; i < count; ++i) {
        std::unique_ptr<DemoBlock> block(parser.nextBlock());
        if (!block) break;
        blocks.push_back(*block);
    }
    return blocks;
}

static bool sameBlocks(const std::vector<DemoBlock>& left,
                       const std::vector<DemoBlock>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].index != right[i].index || left[i].type != right[i].type ||
            left[i].size != right[i].size || left[i].data != right[i].data)
            return false;
    }
    return true;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);

    // A recorded neutral view must reset a previous camera direction. The
    // playback path must reject malformed floats without rejecting zero.
    CHECK(demoMoveOrientationValid(0.0f, 0.0f));
    CHECK(!demoMoveOrientationValid(NAN, 0.0f));
    CHECK(!demoMoveOrientationValid(0.0f, INFINITY));

    // Blocks P M P P M M P: each Move block is one 32 ms tick.
    const std::vector<int> ticks{0, 0, 1, 1, 1, 2, 3, 3};
    CHECK(T2Demo::playbackTargetBlock(0.0f, ticks) == 0);
    CHECK(T2Demo::playbackTargetBlock(0.031f, ticks) == 0);
    CHECK(T2Demo::playbackTargetBlock(0.032f, ticks) == 2);
    CHECK(T2Demo::playbackTargetBlock(0.064f, ticks) == 5);
    CHECK(T2Demo::playbackTargetBlock(0.096f, ticks) == 6);
    CHECK(T2Demo::playbackTargetBlock(10.0f, ticks) == 7);
    CHECK(T2Demo::playbackTargetBlock(NAN, ticks) == 0);
    CHECK(T2Demo::playbackTargetBlock(1.0f, {}) == 0);
    CHECK(T2Demo::playbackBlockTime(3, ticks) == 0.032f);
    CHECK(T2Demo::playbackBlockTime(99, ticks) == 0.096f);
    CHECK(T2Demo::playbackBlockTime(-1, ticks) == 0.0f);
    CHECK(T2Demo::playbackBlockTime(5, {}) == 0.0f);
    CHECK(T2Demo::playbackProgress(-1.0f, 10.0f) == 0.0f);
    CHECK(T2Demo::playbackProgress(15.0f, 10.0f) == 1.0f);
    CHECK(T2Demo::playbackProgress(1.0f, 0.0f) == 0.0f);
    CHECK(T2Demo::playbackProgress(NAN, 10.0f) == 0.0f);
    CHECK(T2Demo::playbackProgress(1.0f, INFINITY) == 0.0f);
    CHECK(T2Demo::formatPlaybackClock(65.9f) == "01:05");
    CHECK(T2Demo::formatPlaybackClock(3661.0f) == "1:01:01");

    errno = 0;
    std::FILE* recording = std::fopen(argv[1], "rb");
    if (!recording && (errno == ENOENT || errno == ENOTDIR)) {
        std::cout << "SKIP: optional recording file not found: " << argv[1] << "\n";
        return 0;
    }
    if (!recording) {
        std::cerr << "failed to open recording: " << argv[1] << "\n";
        return 1;
    }
    std::fclose(recording);

    DemoParser parser;
    CHECK(parser.loadFile(argv[1]));
    const int blockCount = parser.getBlockCount();
    CHECK(blockCount > 8);
    // Parse the complete supported recording before exercising seek/snapshot
    // state. This covers packet, ghost, mission, and effect event paths.
    CHECK(parser.seekToBlock(blockCount));
    CHECK(parser.getBlockCursor() == blockCount);
    // A supported recording parses without faulting, so no packet is dropped.
    if (!parser.getParseFault().empty())
        std::cerr << "parse fault: " << parser.getParseFault() << " ("
                  << parser.getPacketsDroppedAfterFault() << " packets dropped of "
                  << blockCount << " blocks)\n";
    CHECK(parser.getParseFault().empty());
    CHECK(parser.getPacketsDroppedAfterFault() == 0);
    // Target info resolves through each ghost's GameBase target id: every
    // player gets a name, and only CTF flag items (never carriers) are flags.
    int flagGhosts = 0;
    for (int index : parser.getGhostTracker().getAllIndices()) {
        const GhostEntry* g = parser.getGhostTracker().getGhost(index);
        if (g->className == "Player") {
            CHECK(!g->playerName.empty());
            CHECK(!g->isFlag);
        }
        if (g->isFlag) ++flagGhosts;
    }
    CHECK(flagGhosts <= 2);
    // Demo sounds resolve through the streamed AudioProfile datablocks:
    // every profile names a file and every image state sound is a profile.
    const auto& dataBlocks = parser.getInitialBlock().dataBlocks;
    int audioProfiles = 0;
    for (const auto& [id, block] : dataBlocks) {
        if (block.className == "AudioProfile") {
            ++audioProfiles;
            CHECK(!block.decoded.audioFilename.empty());
        }
        for (const auto& state : block.decoded.imageStates) {
            if (!state.valid || state.sound < 0) continue;
            auto profile = dataBlocks.find((uint32_t)state.sound);
            CHECK(profile != dataBlocks.end() && profile->second.className == "AudioProfile");
        }
    }
    CHECK(audioProfiles > 0);
    // Vehicle jets: flying vehicles carry forward/backward/down/trail
    // emitters, hover vehicles three; emitters are ParticleEmitterData.
    int flyingVehicles = 0;
    for (const auto& [id, block] : dataBlocks) {
        const auto& d = block.decoded;
        if (!d.isFlyingVehicleData && !d.isHoverVehicleData) continue;
        CHECK(d.vehicleJetEmitters.size() == (d.isFlyingVehicleData ? 4u : 3u));
        CHECK(d.shapeMass > 0.0f && std::isfinite(d.shapeMass));
        for (uint32_t ref : d.vehicleJetEmitters) {
            if (!ref) continue;
            auto emitter = dataBlocks.find(ref);
            CHECK(emitter != dataBlocks.end() && emitter->second.decoded.hasEmitter);
        }
        if (d.isFlyingVehicleData) {
            ++flyingVehicles;
            CHECK(std::isfinite(d.vehicleMinTrailSpeed) && d.vehicleMinTrailSpeed >= 0.0f);
        }
    }
    if (std::getenv("TORCH_TEST_VERBOSE"))
        std::cerr << "flying vehicle datablocks: " << flyingVehicles << "\n";
    // SniperProjectileData: twelve beam textures and a positive fade.
    for (const auto& [id, block] : dataBlocks) {
        const auto& beam = block.decoded.sniperBeam;
        if (!beam.valid) continue;
        CHECK(beam.textures.size() == 12);
        CHECK(beam.fadeTime > 0.0f && beam.startWidth > 0.0f);
        CHECK(!beam.textures[11].empty());
    }
    // ShapeBaseData::emap: the stock armours enable it.
    bool anyEmap = false;
    for (const auto& [id, block] : dataBlocks)
        if (block.decoded.isPlayerData && block.decoded.shapeEmap) anyEmap = true;
    CHECK(anyEmap);
    // PlayerData::Sounds: jetSound (index 0) is an AudioProfile.
    for (const auto& [id, block] : dataBlocks) {
        if (!block.decoded.isPlayerData) continue;
        CHECK(block.decoded.playerSounds.size() == 32);
        auto jet = dataBlocks.find(block.decoded.playerSounds[0]);
        CHECK(jet != dataBlocks.end() && jet->second.className == "AudioProfile");
    }
    const std::vector<int>& recordedTicks = parser.getMoveTicksBefore();
    CHECK((int)recordedTicks.size() == blockCount + 1);
    CHECK(recordedTicks.back() == parser.getMoveBlockCount());
    CHECK(std::is_sorted(recordedTicks.begin(), recordedTicks.end()));
    CHECK(T2Demo::playbackTargetBlock(1.0e9f, recordedTicks) == blockCount);
    CHECK(parser.seekToBlock(0));
    const std::string initialMission = parser.currentMission();
    CHECK(!parser.getInitialBlock().missionName.empty());
    for (unsigned char c : parser.getInitialBlock().missionName)
        CHECK(c >= 0x20 && c <= 0x7e);
    CHECK(parser.getInitialBlock().missionName.find(".mis") == std::string::npos);
    const uint32_t initialMissionCrc = parser.currentMissionCrc();
    const size_t initialPlayers = parser.getPlayerInfo().size();

    parser.handleHudRemoteCommand("setAmmoHudCount", {"setAmmoHudCount", "42"});
    CHECK(parser.getAmmoHud().count == 42);
    // recordings.cs saveDemoSettings: the HUD starts from the recorded state.
    CHECK(!parser.getWeaponsHud().bitmaps.empty());
    CHECK(!parser.getWeaponsHud().slots.empty());
    parser.handleHudRemoteCommand("setWeaponsHudAmmo", {"setWeaponsHudAmmo", "30", "7"});
    CHECK(parser.getWeaponsHud().slots.find(30) == parser.getWeaponsHud().slots.end());
    parser.handleHudRemoteCommand("setWeaponsHudItem", {"setWeaponsHudItem", "30", "9", "1"});
    parser.handleHudRemoteCommand("setWeaponsHudAmmo", {"setWeaponsHudAmmo", "30", "7"});
    CHECK(parser.getWeaponsHud().slots.at(30) == 7);
    parser.handleHudRemoteCommand("setWeaponsHudBitmap", {"setWeaponsHudBitmap", "30", "Blaster", "gui/blaster"});
    parser.handleHudRemoteCommand("setWeaponsHudItem", {"setWeaponsHudItem", "30", "0", "0"});
    // Removing an item keeps its bitmap (sent once, on connect).
    CHECK(parser.getWeaponsHud().slots.find(30) == parser.getWeaponsHud().slots.end());
    CHECK(parser.getWeaponsHud().bitmaps.at(30) == "gui/blaster");
    parser.handleHudRemoteCommand("setInventoryHudAmount", {"setInventoryHudAmount", "4", "6"});
    CHECK(parser.getInventoryHud().slots.find(4) == parser.getInventoryHud().slots.end());
    parser.handleHudRemoteCommand("setInventoryHudItem", {"setInventoryHudItem", "4", "3", "1"});
    parser.handleHudRemoteCommand("setInventoryHudBitmap", {"setInventoryHudBitmap", "4", "Pack", "gui/pack"});
    parser.handleHudRemoteCommand("setInventoryHudItem", {"setInventoryHudItem", "4", "0", "0"});
    CHECK(parser.getInventoryHud().slots.find(4) == parser.getInventoryHud().slots.end());
    CHECK(parser.getInventoryHud().bitmaps.at(4) == "gui/pack");
    parser.handleHudRemoteCommand("setBackpackHudItem", {"setBackpackHudItem", "2", "1"});
    parser.handleHudRemoteCommand("updatePackText", {"updatePackText", "5"});
    parser.handleHudRemoteCommand("setInventoryHudClearAll", {"setInventoryHudClearAll"});
    CHECK(!parser.getBackpackHud().active);
    parser.handleHudRemoteCommand("setVWeaponsHudActive", {"setVWeaponsHudActive", "2", "Bomber"});
    CHECK(parser.getVehicleHud().activeWeapon == 2);
    CHECK(parser.getVehicleHud().vehicleType == "Bomber");
    parser.handleHudRemoteCommand("showVehicleGauges", {"showVehicleGauges", "Shrike", "0"});
    CHECK(parser.getVehicleHud().dashboardVisible);
    CHECK(parser.getVehicleHud().vehicleType == "Shrike");
    parser.handleHudRemoteCommand("hideVehicleGauges", {"hideVehicleGauges"});
    CHECK(!parser.getVehicleHud().dashboardVisible);
    CHECK(parser.getVehicleHud().vehicleType == "Shrike");

    CHECK(parser.processBlocks(3) == 3);
    const size_t capturedExplosions = DemoParser::s_pendingExplosions.size() + 1;
    DemoParser::s_pendingExplosions.push_back({{}, {}, 0.0f, 1});
    DemoParser::s_pendingTerrainFile = "captured.ter";
    DemoParser::s_sunData.azimuth = 0.5f;
    DemoParser::s_sunData.valid = true;
    const DemoParserSnapshot snapshot = parser.captureSnapshot();
    CHECK(snapshot.pendingExplosions.size() == capturedExplosions);
    const auto expected = readBlocks(parser, 4);
    CHECK(expected.size() == 4);
    CHECK(parser.getBlockCursor() == snapshot.blockCursor + 4);

    CHECK(parser.restoreSnapshot(snapshot));
    CHECK(parser.getBlockCursor() == snapshot.blockCursor);
    const auto restored = readBlocks(parser, 4);
    CHECK(sameBlocks(expected, restored));

    DemoParser::s_pendingTerrainFile = "stale.ter";
    DemoParser::s_sunData.direction = {1.0f, 2.0f, 3.0f};
    DemoParser::s_sunData.azimuth = 0.5f;
    DemoParser::s_sunData.elevation = 0.25f;
    DemoParser::s_sunData.r = 10;
    DemoParser::s_sunData.g = 20;
    DemoParser::s_sunData.b = 30;
    DemoParser::s_sunData.valid = true;
    CHECK(parser.restoreSnapshot(snapshot));
    CHECK(parser.consumeExplosions().size() == capturedExplosions);
    CHECK(DemoParser::s_pendingTerrainFile == "captured.ter");
    CHECK(DemoParser::s_sunData.valid == snapshot.sunValid);
    CHECK(DemoParser::s_sunData.azimuth == snapshot.sunAzimuth);

    CHECK(parser.seekToBlock(3));
    CHECK(parser.getBlockCursor() == 3);
    const auto seeked = readBlocks(parser, 4);
    CHECK(sameBlocks(expected, seeked));

    CHECK(parser.seekToBlock(blockCount));
    CHECK(parser.getBlockCursor() == blockCount);
    CHECK(parser.nextBlock() == nullptr);
    CHECK(parser.seekToBlock(0));
    CHECK(parser.getBlockCursor() == 0);
    CHECK(parser.currentMission() == initialMission);
    CHECK(parser.currentMissionCrc() == initialMissionCrc);
    CHECK(parser.getPlayerInfo().size() == initialPlayers);

    GhostTracker customGhosts;
    customGhosts.createGhost(1, 76, "Class76");
    customGhosts.createGhost(9, 76, "Class76");
    customGhosts.createGhost(3, 76, "Class76");
    const GhostEntry* customGhost = customGhosts.getGhost(1);
    CHECK(customGhost != nullptr);
    CHECK(customGhost->classId == 76);
    CHECK(customGhost->className == "Class76");
    const auto customIndices = customGhosts.getAllIndices();
    CHECK((customIndices == std::vector<int>{1, 3, 9}));

    return 0;
}
