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
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    // Simulate the guard checks up to the mesh section
    int guard = 0;
    int p32 = 0;
    int p16 = 0;
    int p8 = 0;
    // Guard 0
    p32++; p16++; p8++;
    // Bounds: 11 S32
    p32 += 11;
    // Guard 1
    p32++; p16++; p8++;
    // Nodes: numNodes*5 = 31*5 = 155 S32
    p32 += 155;
    // Guard 2
    p32++; p16++; p8++;
    // Objects: numObjects*6 = 19*6 = 114 S32
    p32 += 114;
    // Guard 3
    p32++; p16++; p8++;
    // Decals: numDecals*5 = 24*5 = 120 S32
    p32 += 120;
    // Guard 4
    p32++; p16++; p8++;
    // IFLs: numIFLs*5 = 0
    // Guard 5
    p32++; p16++; p8++;
    // SubShape1: numSubShapes*3 = 1*3 = 3 S32
    p32 += 3;
    // Guard 6
    p32++; p16++; p8++;
    // SubShape2: numSubShapes*3 = 1*3 = 3 S32
    p32 += 3;
    // Guard 7
    p32++; p16++; p8++;
    // subShapeFirstTranslucentObject: numSubShapes = 1 S32 (skip for v >= 22)
    // Rotations: numNodes quaternions = 31*4 = 124 S16
    p16 += 124;
    // Translations: numNodes*3 = 31*3 = 93 S32
    p32 += 93;
    // Guard 8
    p32++; p16++; p8++;
    // Scales: 0 (v23 has numNode* scales)
    // Guard 9
    p32++; p16++; p8++;
    // Object states: numObjStates*3 = 30*3 = 90 S32
    p32 += 90;
    // Guard 10
    p32++; p16++; p8++;
    // Decal states: numDecalStates = 288 S32
    p32 += 288;
    // Guard 11
    p32++; p16++; p8++;
    // Triggers: numTriggers*2 = 0
    // Guard 12
    p32++; p16++; p8++;
    // Details: numDetails*7 = 10*7 = 70 S32
    p32 += 70;
    // Guard 13
    p32++; p16++; p8++;
    fprintf(stderr, "Expected mesh S32 pos: %d\n", p32);
    fprintf(stderr, "Actual mesh S32 pos from loadDTS: 979\n");
    fprintf(stderr, "Difference: %d\n", p32 - 979);
    return 0;
}
