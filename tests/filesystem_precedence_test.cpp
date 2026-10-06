#include "fs/file_system.h"
#include "fs/vl2_archive.h"
#include "fs/zip_archive.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <zip.h>

static void put16(std::ofstream& f, uint16_t value) { f.write((char*)&value, 2); }
static void put32(std::ofstream& f, uint32_t value) { f.write((char*)&value, 4); }

static void writeStoredVl2(const std::filesystem::path& path, const char* textureData = "archive") {
    const std::pair<const char*, const char*> entries[] = {
        {"Textures/GUI/Button.PNG", textureData},
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

static std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

static void addZipEntry(zip_t* archive, const char* name, const void* bytes, size_t size) {
    zip_source_t* source = zip_source_buffer(archive, bytes, size, 0);
    assert(source);
    assert(zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8) >= 0);
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
    writeStoredVl2(root / "Base.VL2");

    FileSystem fs;
    fs.setOriginalOnly(true);
    fs.addPath(root.c_str());
    auto* archive = new Vl2Archive;
    assert(archive->open((root / "Base.VL2").c_str()));
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
    std::ofstream(root / "empty.cs");
    assert(fs.fileExists("EMPTY.CS"));
    assert(fs.readFile("empty.cs", data));
    assert(data.empty());
    fs.shutdown();

    // A direct base root must work without its installation parent, and the
    // active classic loose/archive layers must win over base.
    const auto layered = root / "layered";
    std::filesystem::create_directories(layered / "base/textures/gui");
    std::filesystem::create_directories(layered / "classic/textures/gui");
    std::ofstream(layered / "base/textures/gui/layer.png") << "base-loose";
    std::ofstream(layered / "classic/textures/gui/layer.png") << "classic-loose";
    writeStoredVl2(layered / "base/Base.VL2", "base-archive");
    writeStoredVl2(layered / "classic/Classic.VL2", "classic-archive");

    FileSystem directBase;
    assert(directBase.init({(layered / "base").string()}));
    assert(directBase.readFile("textures/gui/layer.png", data));
    assert(std::string(data.begin(), data.end()) == "base-loose");
    directBase.shutdown();

    FileSystem classic;
    classic.init({(layered / "classic").string(), (layered / "base").string()});
    auto* baseArchive = new Vl2Archive;
    auto* classicArchive = new Vl2Archive;
    assert(baseArchive->open((layered / "base/Base.VL2").c_str()));
    assert(classicArchive->open((layered / "classic/Classic.VL2").c_str()));
    classic.addArchive(baseArchive);
    classic.addArchive(classicArchive);
    assert(classic.readFile("textures/gui/layer.png", data));
    assert(std::string(data.begin(), data.end()) == "classic-loose");
    std::filesystem::remove(layered / "classic/textures/gui/layer.png");
    assert(classic.readFile("textures/gui/button.png", data));
    assert(std::string(data.begin(), data.end()) == "classic-archive");
    classic.shutdown();

    // A ZIP datadir root exposes its qualified directory and mounts nested
    // VL2 files without extracting either the outer ZIP or the inner archive.
    int zipError = 0;
    const auto nestedVl2Path = root / "NestedFromZip.VL2";
    zip_t* innerZip = zip_open(nestedVl2Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    assert(innerZip);
    static const char innerShape[] = "shape";
    static const char innerMission[] = "mission";
    addZipEntry(innerZip, "Shapes/Native.DTS", innerShape, sizeof(innerShape) - 1);
    addZipEntry(innerZip, "Missions/Native.MIS", innerMission, sizeof(innerMission) - 1);
    assert(zip_close(innerZip) == 0);
    const auto nestedVl2 = readBytes(nestedVl2Path);
    const auto outerZipPath = root / "t2-linux.zip";
    zip_t* outerZip = zip_open(outerZipPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    assert(outerZip);
    static const char looseData[] = "zip-root-loose";
    addZipEntry(outerZip, "t2-linux/console_start.cs", looseData, sizeof(looseData) - 1);
    addZipEntry(outerZip, "t2-linux/base/Nested.VL2", nestedVl2.data(), nestedVl2.size());
    assert(zip_close(outerZip) == 0);

    FileSystem zipped;
    assert(zipped.init({}));
    auto* zipRoot = new ZipArchive;
    assert(zipRoot->open(outerZipPath.c_str(), "t2-linux/"));
    auto* nestedArchive = new ZipArchive;
    assert(nestedArchive->openMember(*zipRoot, "base/nested.vl2"));
    zipped.addArchive(zipRoot);
    zipped.addArchive(nestedArchive);
    assert(zipped.readTextFile("console_start.cs", mission));
    assert(mission == looseData);
    assert(zipped.readTextFile("SHAPES/NATIVE.DTS", mission));
    assert(mission == "shape");
    assert(zipped.fileExists("missions/native.mis"));
    zipped.shutdown();

    // Invalid paths must not become mounts or escape the logical namespace.
    FileSystem invalid;
    invalid.addPath((layered / "does-not-exist").c_str());
    assert(!invalid.fileExists("textures/gui/layer.png"));
    std::filesystem::remove_all(root);
    return 0;
}
