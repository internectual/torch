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

    const char* files[] = {"shapes/xorg3.dts", "shapes/borg2.dts", "shapes/octahedron.dts", "shapes/heavy_male_dead.dts"};
    for (auto f : files) {
        auto data = g_fs.read(f);
        if (data.empty()) { fprintf(stderr, "EMPTY: %s\n", f); continue; }
        uint16_t ver = *(uint16_t*)data.data();
        int32_t szAll = *(int32_t*)(data.data()+4);
        int32_t s16 = *(int32_t*)(data.data()+8);
        int32_t s8 = *(int32_t*)(data.data()+12);
        fprintf(stderr, "%s: ver=%u szAll=%d s16=%d s8=%d size=%zu\n", f, ver, szAll, s16, s8, data.size());
        
        // Check validity
        size_t sz32b = (size_t)s16 * 4;
        size_t sz16b = (size_t)(s8 - s16) * 4;
        size_t sz8b = (size_t)(szAll - s8) * 4;
        fprintf(stderr, "  sz32b=%zu sz16b=%zu sz8b=%zu total=%zu\n", sz32b, sz16b, sz8b, 16+sz32b+sz16b+sz8b);
        
        DTSLoadResult r = loadDTS(data.data(), data.size(), f);
        fprintf(stderr, "  loaded=%d meshes=%zu nodes=%zu\n\n", r.loaded, r.meshes.size(), r.nodes.size());
    }
    return 0;
}
