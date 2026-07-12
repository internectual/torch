#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

static size_t pos = 0;
static size_t dataSize = 0;
static const uint8_t* fileData = nullptr;

static int32_t rS32() { if (pos+4>dataSize) return 0; int32_t v; memcpy(&v,fileData+pos,4); pos+=4; return v; }
static float rF32() { if (pos+4>dataSize) return 0; float v; memcpy(&v,fileData+pos,4); pos+=4; return v; }
static int16_t rS16() { if (pos+2>dataSize) return 0; int16_t v; memcpy(&v,fileData+pos,2); pos+=2; return v; }
static uint8_t rU8() { if (pos>=dataSize) return 0; return fileData[pos++]; }
static void skip(size_t n) { pos = (pos+n<=dataSize) ? pos+n : dataSize; }

static void skipIntSet() {
    rS32(); int32_t sz = rS32(); skip(sz * 4);
}

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    auto data = g_fs.read("shapes/octahedron.dts");
    if (data.empty()) return 1;
    fileData = data.data();
    dataSize = data.size();
    pos = 0;
    
    uint16_t ver = *(uint16_t*)fileData;
    uint16_t pad = *(uint16_t*)(fileData+2);
    pos = 4;
    fprintf(stderr, "file=%zu bytes version=%u pad=%u\n", dataSize, ver, pad);
    
    // Bounds (11 F32s)
    float radius=rF32(), tubeRadius=rF32();
    float cx=rF32(), cy=rF32(), cz=rF32();
    float bminx=rF32(), bminy=rF32(), bminz=rF32();
    float bmaxx=rF32(), bmaxy=rF32(), bmaxz=rF32();
    fprintf(stderr, "pos=%zu after bounds: center=(%f,%f,%f) min=(%f,%f,%f) max=(%f,%f,%f)\n", pos, cx,cy,cz, bminx,bminy,bminz, bmaxx,bmaxy,bmaxz);
    
    // Header
    int32_t numNodes = rS32();
    fprintf(stderr, "pos=%zu numNodes=%d\n", pos, numNodes);
    
    // Nodes: for v>=17, each node is 2 S32s read from stream
    struct OldNode { int32_t ni, pi; };
    std::vector<OldNode> nodes(numNodes);
    for (int i = 0; i < numNodes; i++) {
        nodes[i].ni = rS32();
        nodes[i].pi = rS32();
        if (ver < 17) rU8(); // obsolete bool
    }
    fprintf(stderr, "pos=%zu after nodes\n", pos);
    for (int i = 0; i < numNodes; i++)
        fprintf(stderr, "  node[%d] nameIdx=%d parent=%d\n", i, nodes[i].ni, nodes[i].pi);
    
    int32_t numObjects = rS32();
    fprintf(stderr, "pos=%zu numObjects=%d\n", pos, numObjects);
    struct OldObj { int32_t ni, nm, sm, no; };
    std::vector<OldObj> objs(numObjects);
    for (int i = 0; i < numObjects; i++) {
        objs[i].ni = rS32(); objs[i].nm = rS32(); objs[i].sm = rS32(); objs[i].no = rS32();
        fprintf(stderr, "  obj[%d] nameIdx=%d numMeshes=%d startMesh=%d nodeIdx=%d\n", i, objs[i].ni, objs[i].nm, objs[i].sm, objs[i].no);
    }
    
    int32_t numDecals = rS32();
    fprintf(stderr, "pos=%zu numDecals=%d\n", pos, numDecals);
    skip(numDecals * 4 * 4);
    
    int32_t numIFLs = rS32();
    fprintf(stderr, "pos=%zu numIFLs=%d\n", pos, numIFLs);
    skip(numIFLs * 2 * 4);
    
    int32_t numSubShapes = rS32();
    fprintf(stderr, "pos=%zu numSubShapes=%d\n", pos, numSubShapes);
    for (int i = 0; i < numSubShapes; i++) rS32();
    rS32(); // tossed
    for (int i = 0; i < numSubShapes; i++) rS32();
    rS32(); // tossed
    for (int i = 0; i < numSubShapes; i++) rS32();
    
    if (ver < 16) { int32_t sz = rS32(); skip(sz * 4); }
    if (ver < 17) { int32_t sz = rS32(); skip(sz * 3 * 4); }
    
    // Default node states
    int32_t numNodeStates = rS32();
    fprintf(stderr, "pos=%zu numNodeStates=%d\n", pos, numNodeStates);
    for (int i = 0; i < numNodeStates; i++) {
        rS16(); rS16(); rS16(); rS16(); // quat
        rF32(); rF32(); rF32(); // translation
    }
    
    // Object states
    int32_t numObjStates = rS32();
    fprintf(stderr, "pos=%zu numObjStates=%d\n", pos, numObjStates);
    for (int i = 0; i < numObjStates; i++) { rF32(); rS32(); rS32(); }
    
    // Decal states
    int32_t numDecalStates = rS32();
    fprintf(stderr, "pos=%zu numDecalStates=%d\n", pos, numDecalStates);
    for (int i = 0; i < numDecalStates; i++) rS32();
    
    // Triggers
    int32_t numTriggers = rS32();
    fprintf(stderr, "pos=%zu numTriggers=%d\n", pos, numTriggers);
    for (int i = 0; i < numTriggers; i++) { rS32(); rF32(); }
    
    // Details
    int32_t numDetails = rS32();
    fprintf(stderr, "pos=%zu numDetails=%d\n", pos, numDetails);
    for (int i = 0; i < numDetails; i++) {
        int32_t n = rS32(), s = rS32(), o = rS32(); float sz = rF32();
        fprintf(stderr, "  detail[%d] nameIdx=%d sub=%d objDetail=%d size=%f\n", i, n, s, o, sz);
    }
    
    // Sequences
    int32_t numSeqs = rS32();
    fprintf(stderr, "pos=%zu numSeqs=%d\n", pos, numSeqs);
    for (int s = 0; s < numSeqs; s++) {
        fprintf(stderr, "  seq[%d] start at pos=%zu\n", s, pos);
        if (ver > 21) rS32(); // flags
        if (ver < 17) { rS32(); rS32(); } else { rS32(); } // keyframes
        rF32(); // duration
        if (ver < 22) { rU8(); rU8(); rU8(); } // bools
        rS32(); // priority
        rS32(); // firstGroundFrame
        rS32(); // numGroundFrames
        if (ver > 21) { rS32(); rS32(); rS32(); rS32(); rS32(); }
        else if (ver >= 17) { rS32(); rS32(); rS32(); }
        rS32(); // firstTrigger
        rS32(); // numTriggers
        rF32(); // toolBegin
        skipIntSet(); // rotationMatters
        if (ver >= 22) skipIntSet(); // translationMatters
        if (ver >= 22) skipIntSet(); // scaleMatters
        if (ver < 17) skipIntSet(); // objectMembership
        if (ver > 10) skipIntSet(); // decalMatters
        if (ver > 5) skipIntSet(); // iflMatters
        skipIntSet(); // visMatters
        skipIntSet(); // frameMatters
        skipIntSet(); // matFrameMatters
        if (ver < 17) skipIntSet(); // nodeTransformStatic
        fprintf(stderr, "  seq[%d] end at pos=%zu\n", s, pos);
    }
    
    // Meshes
    int32_t numMeshes = rS32();
    fprintf(stderr, "pos=%zu numMeshes=%d\n", pos, numMeshes);
    
    for (int m = 0; m < numMeshes; m++) {
        int32_t meshType = rS32();
        fprintf(stderr, "  mesh[%d] type=%d at pos=%zu\n", m, meshType, pos-4);
        if (meshType == 4) continue;
        
        // Mesh body from readAllocMesh (v15-v18):
        // DebugGuard (synthetic), numFrames, numMatFrames, parentMesh(-1 hardcoded), bounds(10)
        // Then verts, tverts, norms, prims, indices, merge, vertsPerFrame, flags
        // Then DebugGuard (synthetic)
        
        int32_t numFrames = rS32();
        int32_t numMatFrames = rS32();
        fprintf(stderr, "    frames=%d matFrames=%d at pos=%zu\n", numFrames, numMatFrames, pos);
        
        // parentMesh and bounds are oldAlloc'd, NOT in stream
        int32_t numVerts = rS32();
        fprintf(stderr, "    numVerts=%d at pos=%zu\n", numVerts, pos-4);
        skip(numVerts * 3 * 4); // verts
        
        int32_t numTVerts = rS32();
        fprintf(stderr, "    numTVerts=%d at pos=%zu\n", numTVerts, pos-4);
        skip(numTVerts * 2 * 4); // tverts
        
        int32_t numNorms = rS32();
        fprintf(stderr, "    numNorms=%d at pos=%zu\n", numNorms, pos-4);
        skip(numNorms * 3 * 4); // norms
        
        int32_t numPrims = rS32();
        fprintf(stderr, "    numPrims=%d at pos=%zu\n", numPrims, pos-4);
        if (ver < 18) {
            for (int i = 0; i < numPrims; i++) { rS32(); rS32(); } // S32 pairs
            for (int i = 0; i < numPrims; i++) rS32(); // matIdx
        } else {
            for (int i = 0; i < numPrims; i++) { rS16(); rS16(); } // S16 pairs
            for (int i = 0; i < numPrims; i++) rS32(); // matIdx
        }
        fprintf(stderr, "    after prims: pos=%zu\n", pos);
        
        int32_t numIndices = rS32();
        fprintf(stderr, "    numIndices=%d at pos=%zu\n", numIndices, pos-4);
        if (ver < 18) skip(numIndices * 4);
        else skip(numIndices * 2);
        
        rS32(); // merge
        rS32(); // vertsPerFrame
        rS32(); // flags
        fprintf(stderr, "    end of mesh body: pos=%zu (file size=%zu)\n", pos, dataSize);
        
        // Mesh extension
        if (meshType == 1) { // Skin
            int32_t sz;
            sz = rS32(); skip(sz * 3 * 4);
            sz = rS32(); skip(sz * 3 * 4);
            sz = rS32(); skip(sz * 16 * 4);
            sz = rS32(); skip(sz); // vertexIndex
            sz = rS32(); skip(sz); // boneIndex
            sz = rS32(); skip(sz * 4); // weights
            sz = rS32(); skip(sz); // nodeIndex
            fprintf(stderr, "    skin extension end: pos=%zu\n", pos);
        }
        if (meshType == 3) { // Sorted
            int32_t sz;
            sz = rS32(); skip(sz * 8 * 4);
            sz = rS32(); skip(sz * 4);
            sz = rS32(); skip(sz * 4);
            sz = rS32(); skip(sz * 4);
            sz = rS32(); skip(sz * 4);
            rU8(); // alwaysWriteZ
            fprintf(stderr, "    sorted extension end: pos=%zu\n", pos);
        }
        if (meshType == 2) { // Decal
            int32_t sz;
            sz = rS32(); skip(sz * 4);
            if (ver >= 17) { sz = rS32(); skip(sz * 4); sz = rS32(); skip(sz * 4); }
            rS32(); // materialIndex
            fprintf(stderr, "    decal extension end: pos=%zu\n", pos);
        }
    }
    
    fprintf(stderr, "\n=== After meshes: pos=%zu file=%zu remaining=%zu ===\n", pos, dataSize, dataSize-pos);
    
    // Names
    int32_t numNames = rS32();
    fprintf(stderr, "numNames=%d at pos=%zu\n", numNames, pos-4);
    for (int i = 0; i < numNames; i++) {
        int32_t sz = rS32();
        if (sz > 0 && sz < 1000 && pos + sz <= dataSize) {
            fprintf(stderr, "  name[%d]: '%.*s'\n", i, sz, fileData+pos);
            pos += sz;
        } else {
            fprintf(stderr, "  name[%d]: invalid sz=%d\n", i, sz);
            break;
        }
    }
    
    // Materials
    int32_t gotList = rS32();
    fprintf(stderr, "gotList=%d at pos=%zu\n", gotList, pos-4);
    
    fprintf(stderr, "\nFinal pos=%zu file=%zu\n", pos, dataSize);
    
    return 0;
}
