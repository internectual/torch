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
    // Read header counts
    int p = 0;
    int numNodes = buf32[p++], numObjects = buf32[p++], numDecals = buf32[p++];
    int numSubShapes = buf32[p++], numIFLs = buf32[p++];
    int numNodeRot = buf32[p++], numNodeTrans = buf32[p++];
    int numNodeUScale = buf32[p++], numNodeAScale = buf32[p++], numNodeArbScale = buf32[p++];
    int numObjStates = buf32[p++], numDecalStates = buf32[p++], numTriggers = buf32[p++];
    int numDetails = buf32[p++], numMeshes = buf32[p++];
    int numSkins = buf32[p++];
    int numNames = buf32[p++];
    int smallestVisSize = buf32[p++], smallestVisDL = buf32[p++];
    fprintf(stderr, "header: nodes=%d objects=%d decals=%d subshapes=%d ifls=%d\n", numNodes, numObjects, numDecals, numSubShapes, numIFLs);
    fprintf(stderr, "nodeRot=%d nodeTrans=%d nodeUScale=%d nodeAScale=%d nodeArbScale=%d\n", numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale);
    fprintf(stderr, "objStates=%d decalStates=%d triggers=%d details=%d meshes=%d skins=%d names=%d\n", numObjStates, numDecalStates, numTriggers, numDetails, numMeshes, numSkins, numNames);
    fprintf(stderr, "S32 pos after header: %d (buf32 size: %zu)\n", p, sz32b/4);
    return 0;
}
