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
    auto data = g_fs.read("shapes/bioderm_light.dts");
    uint16_t ver = *(uint16_t*)data.data();
    int32_t szAll = *(int32_t*)(data.data()+4);
    int32_t s16 = *(int32_t*)(data.data()+8);
    int32_t s8 = *(int32_t*)(data.data()+12);
    size_t sz32b = (size_t)s16 * 4, sz16b = (size_t)(s8 - s16) * 4, sz8b = (size_t)(szAll - s8) * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    int p32 = 19, p16 = 0, p8 = 0;
    // Skip guards and data sections to find mesh types
    // Just read the first few mesh types from the end of the S32 buffer
    fprintf(stderr, "sz32b/4=%zu sz16b/2=%zu sz8b=%zu\n", sz32b/4, sz16b/2, sz8b);
    fprintf(stderr, "Last 10 S32 values: ");
    for (int i = 10; i > 0; i--) fprintf(stderr, "%u ", buf32[(sz32b/4)-i]);
    fprintf(stderr, "\n");
    return 0;
}
