#include <cstdint>
#include <cstdio>
#include <cstring>
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

    auto data = g_fs.read("shapes/porg22.dts");
    if (data.empty()) return 1;
    uint16_t ver = *(uint16_t*)data.data();
    int32_t s16v = *(int32_t*)(data.data()+8);
    int32_t s8v = *(int32_t*)(data.data()+12);
    size_t sz32b = s16v * 4;
    size_t sz16b = (s8v - s16v) * 4;
    size_t sz8b = (s8v - s16v) * 4; // wrong: should be (szAll - s8v)*4
    int32_t szAll = *(int32_t*)(data.data()+4);
    sz8b = (szAll - s8v) * 4;
    
    fprintf(stderr, "v%u buf32=%zu buf16=%zu buf8=%zu\n", ver, sz32b/4, sz16b/2, sz8b);
    
    const uint32_t* p32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* p16 = (const uint16_t*)(data.data() + 16 + sz32b);
    const uint8_t* p8 = (const uint8_t*)(data.data() + 16 + sz32b + sz16b);
    size_t cnt32 = sz32b/4;
    size_t cnt16 = sz16b/2;
    size_t cnt8 = sz8b;
    
    // Mesh section starts at p32=100 (from trace_buf)
    // mesh[0] type=3 at p32=100
    fprintf(stderr, "\n=== Mesh section dump (starting at S32[100]) ===\n");
    for (size_t i = 100; i < 160 && i < cnt32; i++) {
        fprintf(stderr, "  S32[%zu] = %d (0x%08x)", i, (int)p32[i], p32[i]);
        float f; memcpy(&f, &p32[i], 4);
        if (f > -10000 && f < 10000 && f != 0.0f && f == f) fprintf(stderr, "  [float=%g]", f);
        fprintf(stderr, "\n");
    }
    
    // Also dump S16 around the mesh
    fprintf(stderr, "\n=== S16 dump around mesh data ===\n");
    // After buf32 mesh data, S16 buffer has primitives and indices
    // For this mesh: verts=126, tverts=0
    // After bounds(10) + numVerts(1) + verts(126*3=378) + numTVerts(1) + tverts(0) + norms(126*3=378) + encodedNorms(126) + numPrims(1) + ...
    // The S16 buffer should have primitives and indices
    for (size_t i = 0; i < 40 && i < cnt16; i++) {
        fprintf(stderr, "  S16[%zu] = %d\n", i, (int)p16[i]);
    }
    
    return 0;
}
