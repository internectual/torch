#include "fs/file_system.h"
#include "game/mission_discovery.h"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "torch-mission-resolution";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "base/missions/Official");
    std::ofstream(root / "base/missions/Official/Desert.MISPK") << "mission";

    FileSystem fs;
    fs.init({root.string(), (root / "base").string()});
    fs.setOriginalOnly(true);

    std::string path;
    std::string content;
    assert(resolveMissionFile(fs, "official/desert", path, content));
    assert(path == "missions/official/desert.misPK");
    assert(content == "mission");

    FileSystem baseOnly;
    baseOnly.init({(root / "base").string()});
    assert(resolveMissionFile(baseOnly, "Desert", path, content));
    assert(path == "missions/Official/Desert.MISPK");

    fs.shutdown();
    baseOnly.shutdown();
    std::filesystem::remove_all(root);
    return 0;
}
