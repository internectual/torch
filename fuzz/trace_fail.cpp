#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
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

    const char* targets[] = {"shapes/huntersflag.dts", "shapes/stackable5l.dts", "shapes/deploy_sensor_motion.dts"};
    for (auto& path : targets) {
        auto data = g_fs.read(path);
        if (data.empty()) continue;
        uint16_t ver = *(uint16_t*)data.data();
        int32_t szAll = *(int32_t*)(data.data()+4);
        int32_t s16v = *(int32_t*)(data.data()+8);
        int32_t s8v = *(int32_t*)(data.data()+12);
        size_t sz32b = s16v * 4;
        const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
        size_t cnt32 = sz32b / 4;

        fprintf(stderr, "\n=== %s (v%u) ===\n", path, ver);
        // Just show first 25 header S32s
        for (int i = 0; i < 20 && i < (int)cnt32; i++)
            fprintf(stderr, "  S32[%d] = %d\n", i, (int)buf32[i]);
    }
    return 0;
}
