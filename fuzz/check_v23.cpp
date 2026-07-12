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
    // Check a working v23 shape
    auto data = g_fs.read("shapes/bioderm_light.dts");
    uint16_t ver = *(uint16_t*)data.data();
    int32_t szAll = *(int32_t*)(data.data()+4);
    int32_t s16 = *(int32_t*)(data.data()+8);
    int32_t s8 = *(int32_t*)(data.data()+12);
    size_t sz32b = (size_t)s16 * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    printf("v23 bioderm: sz32=%zu sz16=%zu\n", sz32b/4, (size_t)(s8-s16)*4/2);
    int p32=0;
    #define R32() buf32[p32++]
    printf("  counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32());
    printf("  counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32());
    printf("  counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32());
    printf("  guard0: %d (pos=%d)\n", R32(), p32);
    printf("  bounds: skip 11 (pos=%d)\n", p32+11); p32+=11;
    printf("  guard1: %d (pos=%d)\n", R32(), p32);
    printf("  numNodes=%d, skip %d (pos=%d)\n", buf32[p32], buf32[p32]*5, p32+buf32[p32]*5);
    DTSLoadResult r = loadDTS(data.data(), data.size(), "bioderm_light.dts");
    printf("  loaded=%d meshes=%zu\n", r.loaded, r.meshes.size());
    return 0;
}
