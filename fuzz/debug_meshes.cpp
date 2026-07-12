#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"
#include "core/console.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    const char* targets[] = {
        "shapes/borg1.dts", "shapes/huntersflag.dts", "shapes/deploy_sensor_motion.dts"
    };
    for (auto& path : targets) {
        auto data = g_fs.read(path);
        if (data.empty()) continue;
        uint16_t ver = *(uint16_t*)data.data();
        int32_t szAll = *(int32_t*)(data.data()+4);
        int32_t s16 = *(int32_t*)(data.data()+8);
        int32_t s8 = *(int32_t*)(data.data()+12);
        fprintf(stderr, "\n=== %s (v%u, %zu bytes) header: szAll=%d s16=%d s8=%d ===\n", path, ver, data.size(), szAll, s16, s8);
        // Print first 20 S32s of the 32-bit section
        const uint32_t* p = (const uint32_t*)(data.data() + 16);
        for (int i = 0; i < 20; i++) fprintf(stderr, "  S32[%d] = %d (0x%08x)\n", i, (int)p[i], p[i]);
    }
    return 0;
}
