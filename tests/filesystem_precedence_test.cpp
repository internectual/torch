#include "fs/file_system.h"
#include "fs/vl2_archive.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

static void put16(std::ofstream& f, uint16_t value) { f.write((char*)&value, 2); }
static void put32(std::ofstream& f, uint32_t value) { f.write((char*)&value, 4); }

static void writeStoredVl2(const std::filesystem::path& path) {
    const std::pair<const char*, const char*> entries[] = {
        {"Textures/GUI/Button.PNG", "archive"},
        {"Shapes/Native.DTS", "shape"},
        {"Missions/Native.MIS", "mission"},
    };
    std::ofstream f(path, std::ios::binary);
    for (const auto& [name, data] : entries) {
        f.write("PK\3\4", 4);
        put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        const uint32_t dataSize = (uint32_t)std::char_traits<char>::length(data);
        put32(f, 0); put32(f, dataSize); put32(f, dataSize);
        put16(f, (uint16_t)std::string(name).size()); put16(f, 0);
        f.write(name, std::string(name).size());
        f.write(data, dataSize);
    }
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / "torch-filesystem-precedence";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "textures/gui");
    std::ofstream(root / "textures/gui/button.png") << "loose";
    std::ofstream(root / "textures/gui/virtual.BM8") << "virtual-bm8";
    std::filesystem::create_directories(root / "@vl2/Directory.vl2/Textures/GUI");
    std::ofstream(root / "@vl2/Directory.vl2/Textures/GUI/Bundle.PNG") << "directory";
    std::ofstream(root / "@vl2/Directory.vl2/Textures/GUI/Converted.GLB") << "converted";
    std::filesystem::create_directories(root / "@vl2/Nested/Maps.vl2/Missions");
    std::ofstream(root / "@vl2/Nested/Maps.vl2/Missions/Nested.MIS") << "nested";
    writeStoredVl2(root / "base.vl2");

    FileSystem fs;
    fs.setOriginalOnly(true);
    fs.addPath(root.c_str());
    auto* archive = new Vl2Archive;
    assert(archive->open((root / "base.vl2").c_str()));
    fs.addArchive(archive);

    std::vector<uint8_t> data;
    assert(fs.readFile("TEXTURES/GUI/BUTTON.PNG", data));
    assert(std::string(data.begin(), data.end()) == "loose");
    assert(fs.fileExists("textures/gui/BUTTON.png"));
    assert(fs.readFile("shapes/native.dts", data));
    assert(std::string(data.begin(), data.end()) == "shape");
    std::string mission;
    assert(fs.readTextFile("MISSIONS/NATIVE.MIS", mission));
    assert(mission == "mission");
    assert(fs.readFile("textures/gui/bundle.png", data));
    assert(std::string(data.begin(), data.end()) == "directory");
    assert(fs.readFile("textures/gui/virtual", data));
    assert(std::string(data.begin(), data.end()) == "virtual-bm8");
    std::string resolved;
    assert(fs.readTextureFile("TEXTURES/GUI/VIRTUAL", data, &resolved));
    assert(resolved == "TEXTURES/GUI/VIRTUAL.bm8");
    assert(fs.fileExists("textures/gui/virtual"));
    assert(fs.fileExists("TEXTURES/GUI/BUNDLE.PNG"));
    assert(!fs.readFile("textures/gui/converted.glb", data));
    assert(fs.readTextFile("missions/nested.mis", mission));
    assert(mission == "nested");

    std::vector<std::string> files;
    fs.listFiles("textures/gui/*.png", files);
    assert(files.size() == 2 && files[0] == "Textures/GUI/Bundle.PNG" &&
           files[1] == "textures/gui/button.png");
    files.clear();
    fs.listFiles("@vl2/*", files);
    assert(files.empty());

    assert(!fs.readFile("generated/button.png", data));
    assert(!fs.fileExists("cache/native.dts"));
    assert(!fs.fileExists("textures/gui/button.glb"));
    assert(!fs.fileExists("textures/gui/missing"));
    fs.shutdown();
    std::filesystem::remove_all(root);
    return 0;
}
