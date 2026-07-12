#include <cstdint>
#include <cstdio>
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
    g_vl2.open("/home/methodown/t2-linux/base/shapes.vl2");
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
