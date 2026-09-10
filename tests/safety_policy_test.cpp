#include "fs/path_policy.h"
#include "net/master_query.h"
#include "net/protocol.h"
#include "net/network.h"
#include "game/mission_parser.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

static bool check(bool value, const char* expression) {
    if (!value) std::cerr << "failed: " << expression << "\n";
    return value;
}

#define CHECK(expr) if (!check((expr), #expr)) return 1

int main() {
    CHECK(TorchPath::isSafeLogicalPath("missions/RiverDance.mis"));
    CHECK(TorchPath::isSafeLogicalPath("textures/gui/button.png"));
    CHECK(!TorchPath::isSafeLogicalPath("/etc/passwd"));
    CHECK(!TorchPath::isSafeLogicalPath("../outside.cs"));
    CHECK(!TorchPath::isSafeLogicalPath("missions/../outside.mis"));
    CHECK(!TorchPath::isSafeLogicalPath("missions\\outside.mis"));
    CHECK(!TorchPath::isSafeLogicalPath("./mission.mis"));
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "torch-safety-test";
    const std::filesystem::path tempOutside = std::filesystem::temp_directory_path() / "torch-safety-outside";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::remove_all(tempOutside);
    std::filesystem::create_directories(tempRoot);
    std::filesystem::create_directories(tempOutside);
    std::ofstream(tempOutside / "secret.cs") << "secret";
    std::filesystem::create_symlink(tempOutside / "secret.cs", tempRoot / "link.cs");
    CHECK(TorchPath::staysWithinRoot(tempRoot.c_str(), (tempRoot / "link.cs").c_str()) == false);
    CHECK(TorchPath::staysWithinRoot(tempRoot.c_str(), (tempRoot / "inside.cs").c_str()));
    std::filesystem::remove_all(tempRoot);
    std::filesystem::remove_all(tempOutside);

    std::string host;
    uint16_t port = 0;
    CHECK(TorchMaster::parseAddressLine("  127.0.0.1:28000\r\n", host, port));
    CHECK(host == "127.0.0.1" && port == 28000);
    CHECK(!TorchMaster::parseAddressLine("127.0.0.1", host, port));
    CHECK(!TorchMaster::parseAddressLine("127.0.0.1:0", host, port));
    CHECK(!TorchMaster::parseAddressLine("127.0.0.1:70000", host, port));
    CHECK(!TorchMaster::parseAddressLine("../evil:28000", host, port));
    CHECK(!TorchMaster::parseAddressLine("host/name:28000", host, port));

    T2Protocol::ChatMessage chat{};
    const uint8_t oversizedSender[] = {T2Protocol::GDT_ChatMessage, 32, 0, 0, 0};
    CHECK(!T2Protocol::decodeChat(oversizedSender, sizeof(oversizedSender), chat));
    const uint8_t oversizedText[] = {T2Protocol::GDT_ChatMessage, 0, 0xff, 0x01};
    CHECK(!T2Protocol::decodeChat(oversizedText, sizeof(oversizedText), chat));
    CHECK(isObserverSetupCommand({"setPlayerTeam", "0"}));
    CHECK(isObserverSetupCommand({"ScopeCommanderMap", "1"}));
    CHECK(isObserverSetupCommand({"WatchOnly", "ImaWatcher"}));
    CHECK(!isObserverSetupCommand({"setPlayerTeam", "1"}));
    CHECK(!isObserverSetupCommand({"WatchOnly", "ImaWatcher", "extra"}));

    WireHeader wire{0x11223344u, 0x55667788u, 0x99aabbccu, 0x0d, 0xeeff};
    uint8_t wireBytes[sizeof(WireHeader)]{};
    encodeWireHeader(wireBytes, wire);
    CHECK(sizeof(WireHeader) == 15);
    CHECK(wireBytes[0] == 0x44 && wireBytes[1] == 0x33 && wireBytes[2] == 0x22 && wireBytes[3] == 0x11);
    CHECK(wireBytes[12] == 0x0d && wireBytes[13] == 0xff && wireBytes[14] == 0xee);
    const WireHeader decoded = decodeWireHeader(wireBytes);
    CHECK(decoded.sequence == wire.sequence && decoded.ack == wire.ack &&
          decoded.ackMask == wire.ackMask && decoded.type == wire.type &&
          decoded.checksum == wire.checksum);


    const auto mission = parseMisFile(
        "new SimGroup(MissionGroup) { new StaticShape(TestShape) { position = \"1 2 3\"; }; };");
    CHECK(mission.size() == 2);
    CHECK(mission[1].className == "StaticShape");
    CHECK(parseMisFile(std::string(MisParseBudget::MaxContentBytes + 1, 'x')).empty());
    std::string oversizedMission = "new StaticShape(Test) { field = \"";
    oversizedMission.append(MisParseBudget::MaxValueBytes + 1, 'x');
    oversizedMission += "\"; };";
    CHECK(parseMisFile(oversizedMission).empty());
    return 0;
}
