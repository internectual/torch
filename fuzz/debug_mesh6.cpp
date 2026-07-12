#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
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
    // Test a simpler shape first
    auto data = g_fs.read("shapes/octahedron.dts");
    fprintf(stderr, "=== octahedron.dts (v%u, %zu bytes) ===\n", *(uint16_t*)data.data(), data.size());
    DTSLoadResult r = loadDTS(data.data(), data.size(), "octahedron.dts");
    fprintf(stderr, "loaded=%d meshes=%zu nodes=%zu corrupted=%d\n", r.loaded, r.meshes.size(), r.nodes.size(), 0);
    
    data = g_fs.read("shapes/bioderm_light.dts");
    fprintf(stderr, "\n=== bioderm_light.dts (v%u, %zu bytes) ===\n", *(uint16_t*)data.data(), data.size());
    r = loadDTS(data.data(), data.size(), "bioderm_light.dts");
    fprintf(stderr, "loaded=%d meshes=%zu nodes=%zu\n", r.loaded, r.meshes.size(), r.nodes.size());
    return 0;
}
