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
    auto data = g_fs.read("shapes/xorg21.dts");
    uint16_t ver = *(uint16_t*)data.data();
    int32_t szAll = *(int32_t*)(data.data()+4);
    int32_t s16 = *(int32_t*)(data.data()+8);
    int32_t s8 = *(int32_t*)(data.data()+12);
    size_t sz32b = (size_t)s16 * 4;
    size_t sz16b = (size_t)(s8 - s16) * 4;
    size_t sz8b = (size_t)(szAll - s8) * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    printf("ver=%u sz32=%zu sz16=%zu sz8=%zu\n", ver, sz32b/4, sz16b/2, sz8b);
    int p32=0, p16=0;
    #define R32() buf32[p32++]
    #define R16() buf16[p16++]
    printf("counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32()); // 5
    printf("counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32()); // 10
    printf("counts: %d %d %d %d %d\n", R32(), R32(), R32(), R32(), R32()); // 15
    printf("guard0: %d (S32 pos=%d)\n", R32(), p32);
    printf("radius/tube/center/bounds: skip 11\n"); p32+=11;
    printf("guard1: %d (S32 pos=%d)\n", R32(), p32);
    int numN = buf32[p32]; // peek
    printf("numNodes=%d, nodes will take %d S32 (pos %d->%d)\n", numN, numN*5, p32, p32+numN*5);
    p32 += numN*5;
    printf("guard2: %d (S32 pos=%d)\n", R32(), p32);
    int numO = buf32[p32];
    printf("numObjects=%d (pos %d->%d)\n", numO, p32, p32+numO*6);
    p32 += numO*6;
    printf("guard3: %d (S32 pos=%d)\n", R32(), p32);
    // peek at what comes next
    printf("next S32 values: %d %d %d %d %d\n", buf32[p32], buf32[p32+1], buf32[p32+2], buf32[p32+3], buf32[p32+4]);
    DTSLoadResult r = loadDTS(data.data(), data.size(), "xorg21.dts");
    printf("loaded=%d meshes=%zu nodes=%zu\n", r.loaded, r.meshes.size(), r.nodes.size());
    return 0;
}
