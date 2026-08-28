#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2.open((basePath + "/shapes.vl2").c_str());
    g_fs.addArchive(&g_vl2);

    const char* files[] = {
        "shapes/octahedron.dts", "shapes/xorg3.dts", "shapes/xorg21.dts",
        "shapes/borg3.dts", "shapes/chaingun_shot.dts", "shapes/plasmabolt.dts",
        "shapes/turret_muzzlepoint.dts", "shapes/bombers_eye.dts", "shapes/borg2.dts"
    };
    for (auto f : files) {
        auto data = g_fs.read(f);
        if (data.empty()) { fprintf(stderr, "EMPTY: %s\n", f); continue; }
        uint16_t ver = *(uint16_t*)data.data();
        fprintf(stderr, "\n=== %s (v%u, %zu bytes) ===\n", f, ver, data.size());
        DTSLoadResult r = loadDTS(data.data(), data.size(), f);
        fprintf(stderr, "  loaded=%d meshes=%zu nodes=%zu\n", r.loaded, r.meshes.size(), r.nodes.size());
    }
    return 0;
}
