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
    int32_t s16 = *(int32_t*)(data.data()+8);
    size_t sz32b = (size_t)s16 * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    fprintf(stderr, "S32 pos 979: %u (type=%u)\n", buf32[979], buf32[979] & 0x7);
    fprintf(stderr, "S32 pos 978-982: ");
    for (int i = 978; i < 983; i++) fprintf(stderr, "%u ", buf32[i]);
    fprintf(stderr, "\n");
    return 0;
}
