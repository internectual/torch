#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    const char* files[] = {
        "shapes/deploy_inventory.dts",
        "shapes/deploy_sensor_motion.dts",
        "shapes/deploy_sensor_pulse.dts",
        "shapes/nexusbase.dts",
        "shapes/nexuscap.dts",
    };
    for (auto path : files) {
        auto data = g_fs.read(path);
        if (data.empty()) { fprintf(stderr, "%s: not found\n", path); continue; }
        uint16_t ver = *(uint16_t*)data.data();
        fprintf(stderr, "\n=== %s (v%u, %zu bytes) ===\n", path, ver, data.size());
        DTSLoadResult r = loadDTS(data.data(), data.size(), path);
        fprintf(stderr, "  loaded=%d meshes=%zu nodes=%zu details=%zu\n", r.loaded, r.meshes.size(), r.nodes.size(), r.details.size());
        for (size_t i = 0; i < r.meshes.size(); i++)
            fprintf(stderr, "  mesh[%zu] verts=%zu tris=%zu nodeIdx=%d\n", i, r.meshes[i].vertices.size(), r.meshes[i].indices.size()/3, r.meshes[i].nodeIndex);
        for (size_t i = 0; i < r.nodes.size(); i++)
            fprintf(stderr, "  node[%zu] parent=%d name='%s'\n", i, r.nodes[i].parentIndex, r.nodes[i].name.c_str());
    }
    return 0;
}
