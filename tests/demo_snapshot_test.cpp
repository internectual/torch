#include "game/demo.h"
#include "core/console.h"

#include <cstdio>
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

void Console::printf(LogLevel, const char*, ...) {}

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

    DemoParser parser;
    CHECK(parser.loadFile(argv[1]));
    const int blockCount = parser.getBlockCount();
    CHECK(blockCount > 8);
    const std::string initialMission = parser.currentMission();
    const uint32_t initialMissionCrc = parser.currentMissionCrc();
    const size_t initialPlayers = parser.getPlayerInfo().size();

    parser.handleHudRemoteCommand("setAmmoHudCount", {"setAmmoHudCount", "42"});
    CHECK(parser.getAmmoHud().count == 42);
    parser.handleHudRemoteCommand("setWeaponsHudAmmo", {"setWeaponsHudAmmo", "3", "7"});
    CHECK(parser.getWeaponsHud().slots.find(3) == parser.getWeaponsHud().slots.end());
    parser.handleHudRemoteCommand("setWeaponsHudItem", {"setWeaponsHudItem", "3", "9", "1"});
    parser.handleHudRemoteCommand("setWeaponsHudAmmo", {"setWeaponsHudAmmo", "3", "7"});
    CHECK(parser.getWeaponsHud().slots.at(3) == 7);
    parser.handleHudRemoteCommand("setWeaponsHudBitmap", {"setWeaponsHudBitmap", "3", "Blaster", "gui/blaster"});
    parser.handleHudRemoteCommand("setWeaponsHudItem", {"setWeaponsHudItem", "3", "0", "0"});
    CHECK(parser.getWeaponsHud().bitmaps.find(3) == parser.getWeaponsHud().bitmaps.end());
    parser.handleHudRemoteCommand("setInventoryHudAmount", {"setInventoryHudAmount", "4", "6"});
    CHECK(parser.getInventoryHud().slots.find(4) == parser.getInventoryHud().slots.end());
    parser.handleHudRemoteCommand("setInventoryHudItem", {"setInventoryHudItem", "4", "3", "1"});
    parser.handleHudRemoteCommand("setInventoryHudBitmap", {"setInventoryHudBitmap", "4", "Pack", "gui/pack"});
    parser.handleHudRemoteCommand("setInventoryHudItem", {"setInventoryHudItem", "4", "0", "0"});
    CHECK(parser.getInventoryHud().bitmaps.find(4) == parser.getInventoryHud().bitmaps.end());
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

    CHECK(parser.processBlocks(3) == 3);
    const DemoParserSnapshot snapshot = parser.captureSnapshot();
    const auto expected = readBlocks(parser, 4);
    CHECK(expected.size() == 4);
    CHECK(parser.getBlockCursor() == snapshot.blockCursor + 4);

    CHECK(parser.restoreSnapshot(snapshot));
    CHECK(parser.getBlockCursor() == snapshot.blockCursor);
    const auto restored = readBlocks(parser, 4);
    CHECK(sameBlocks(expected, restored));

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
    return 0;
}
