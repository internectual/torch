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

    // We know the pre-mesh section is correct. Dump S32s starting at mesh section.
    const char* targets[] = {"shapes/borg34.dts", "shapes/huntersflag.dts", "shapes/stackable5l.dts"};
    for (auto& path : targets) {
        auto data = g_fs.read(path);
        if (data.empty()) continue;
        uint16_t ver = *(uint16_t*)data.data();
        int32_t s16v = *(int32_t*)(data.data()+8);
        int32_t s8v = *(int32_t*)(data.data()+12);
        size_t sz32b = s16v * 4;
        const uint32_t* p32 = (const uint32_t*)(data.data() + 16);
        const int16_t* p16 = (const int16_t*)(data.data() + 16 + sz32b);
        const uint8_t* p8 = (const uint8_t*)(data.data() + 16 + sz32b + (s8v-s16v)*4);
        size_t cnt32 = sz32b / 4;
        
        // Read header to find where mesh section starts (same as trace_buf)
        size_t pos = 0;
        auto rS32 = [&]() -> int32_t { return (int32_t)p32[pos++]; };
        int32_t numNodes = rS32(), numObjects = rS32(), numDecals = rS32();
        int32_t numSubShapes = rS32(), numIFLs = rS32();
        int32_t numNodeRot, numNodeTrans;
        if (ver < 22) { int32_t c = rS32() - numNodes; numNodeRot = numNodeTrans = c < 0 ? 0 : c; }
        else { numNodeRot = rS32(); numNodeTrans = rS32(); rS32(); rS32(); rS32(); }
        rS32(); rS32(); rS32(); // objStates, decalStates, triggers
        int32_t numDetails = rS32(), numMeshes = rS32();
        int32_t numSkins = (ver < 23) ? rS32() : 0;
        int32_t numNames = rS32();
        rS32(); rS32(); // smallest
        
        fprintf(stderr, "\n=== %s v%u (mesh section from p32=%zu) ===\n", path, ver, pos);
        
        // Now manually advance through guards to mesh section
        // We verified trace_buf shows all guards 0-13 pass. Mesh starts after guard 13.
        // Just dump S32s from the mesh start position
        size_t meshStart = pos; // we know this from trace_buf: borg34=97, huntersflag=180, stackable5l=142
        
        // Actually let's just find the mesh start by looking for the guard sequence 13/13/13 in S32/S16/S8
        // Or better: let's just dump from the known positions
        // borg34: mesh section starts at S32[97]
        // huntersflag: mesh section starts at S32[180]
        // stackable5l: mesh section starts at S32[142]
        
        for (size_t i = meshStart; i < meshStart + 120 && i < cnt32; i++) {
            fprintf(stderr, "  S32[%zu] = %d (0x%08x)", i, (int)p32[i], p32[i]);
            // Also show as float
            float f; memcpy(&f, &p32[i], 4);
            if (f > -10000 && f < 10000 && f != 0.0f) fprintf(stderr, "  [float=%g]", f);
            fprintf(stderr, "\n");
        }
    }
    return 0;
}
