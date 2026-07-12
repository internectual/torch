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
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main(int argc, char** argv) {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    const char* targets[] = {
        "shapes/borg1.dts", "shapes/borg7.dts", "shapes/huntersflag.dts",
        "shapes/stackable5l.dts", "shapes/deploy_sensor_motion.dts",
        "shapes/borg3.dts", "shapes/octahedron.dts", "shapes/plasmabolt.dts",
        "shapes/disc_explosion.dts"
    };
    for (auto& path : targets) {
        auto data = g_fs.read(path);
        if (data.empty()) { fprintf(stderr, "CANT READ: %s\n", path); continue; }
        uint16_t ver = *(uint16_t*)data.data();
        fprintf(stderr, "\n=== %s (v%u, %zu bytes) ===\n", path, ver, data.size());
        DTSLoadResult r = loadDTS(data.data(), data.size(), path);
        fprintf(stderr, "loaded=%d meshes=%zu nodes=%zu anims=%zu\n",
            r.loaded, r.meshes.size(), r.nodes.size(), r.animations.size());
        if (!r.meshes.empty()) {
            size_t totalVerts = 0, totalIdx = 0;
            for (auto& m : r.meshes) { totalVerts += m.vertices.size(); totalIdx += m.indices.size(); }
            fprintf(stderr, "totalVerts=%zu totalIdx=%zu\n", totalVerts, totalIdx);
        }
    }
    return 0;
}
