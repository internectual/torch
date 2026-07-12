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
    int32_t szAll = *(int32_t*)(data.data()+4);
    int32_t s16v = *(int32_t*)(data.data()+8);
    int32_t s8v = *(int32_t*)(data.data()+12);
    size_t sz32b = s16v * 4;
    size_t sz16b = (s8v - s16v) * 4;
    size_t sz8b = (szAll - s8v) * 4;
    
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    const uint8_t* buf8 = (const uint8_t*)(data.data() + 16 + sz32b + sz16b);
    size_t cnt32 = sz32b/4, cnt16 = sz16b/2, cnt8 = sz8b;
    
    fprintf(stderr, "v%u szAll=%d s16=%d s8=%d cnt32=%zu cnt16=%zu cnt8=%zu\n", ver, szAll, s16v, s8v, cnt32, cnt16, cnt8);
    
    // Manually trace exactly like the parser does
    size_t p32=0, p16=0, p8=0;
    auto rS32 = [&]() -> int32_t { return (int32_t)buf32[p32++]; };
    auto rU32 = [&]() -> uint32_t { return buf32[p32++]; };
    auto rF32 = [&]() -> float { float f; memcpy(&f, &buf32[p32++], 4); return f; };
    auto rS16 = [&]() -> int16_t { return (int16_t)buf16[p16++]; };
    auto rU16 = [&]() -> uint16_t { return buf16[p16++]; };
    auto rU8 = [&]() -> uint8_t { return buf8[p8++]; };
    
    // Header (15 fields for v19)
    int32_t numNodes=rS32(), numObjects=rS32(), numDecals=rS32();
    int32_t numSubShapes=rS32(), numIFLs=rS32();
    int32_t combined=rS32() - numNodes; if (combined<0) combined=0;
    int32_t numNodeRot=combined, numNodeTrans=combined;
    int32_t numObjStates=rS32(), numDecalStates=rS32(), numTriggers=rS32();
    int32_t numDetails=rS32(), numMeshes=rS32();
    int32_t numSkins=rS32();
    int32_t numNames=rS32();
    rS32(); rS32(); // smallest
    fprintf(stderr, "nodes=%d objects=%d meshes=%d details=%d skins=%d names=%d nodeRot=%d\n", numNodes, numObjects, numMeshes, numDetails, numSkins, numNames, numNodeRot);
    fprintf(stderr, "after header: p32=%zu\n", p32);
    
    // Guard 0
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 0 at p32=%zu (before read): S32=%d S16=%d S8=%d\n", p32-1, g, g16, g8); }
    // Bounds: radius, tubeRadius, center(3), min(3), max(3) = 11 F32s
    for (int i=0; i<11; i++) rF32();
    // Guard 1
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 1 at p32=%zu: S32=%d S16=%d S8=%d\n", p32-1, g, g16, g8); }
    // Nodes: 5 S32s each
    for (int i=0; i<numNodes; i++) for (int j=0; j<5; j++) rS32();
    // Guard 2
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 2 at p32=%zu: S32=%d S16=%d S8=%d\n", p32-1, g, g16, g8); }
    // Objects: 6 S32s each
    for (int i=0; i<numObjects; i++) for (int j=0; j<6; j++) rS32();
    // Guard 3
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 3 at p32=%zu\n", p32-1); }
    // Decals: 5 S32s each
    for (int i=0; i<numDecals; i++) for (int j=0; j<5; j++) rS32();
    // Guard 4
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 4 at p32=%zu\n", p32-1); }
    // IFLs: 5 S32s each
    for (int i=0; i<numIFLs; i++) for (int j=0; j<5; j++) rS32();
    // Guard 5
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 5 at p32=%zu\n", p32-1); }
    // SubShape first: 3 arrays of numSubShapes S32s
    for (int i=0; i<numSubShapes; i++) rS32();
    for (int i=0; i<numSubShapes; i++) rS32();
    for (int i=0; i<numSubShapes; i++) rS32();
    // Guard 6
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 6 at p32=%zu\n", p32-1); }
    // SubShape num: 3 arrays of numSubShapes S32s
    for (int i=0; i<numSubShapes; i++) rS32();
    for (int i=0; i<numSubShapes; i++) rS32();
    for (int i=0; i<numSubShapes; i++) rS32();
    // Guard 7
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 7 at p32=%zu\n", p32-1); }
    // Default rotations (4 S16s from buf16 per node)
    for (int i=0; i<numNodes; i++) { rS16(); rS16(); rS16(); rS16(); }
    // Default translations (3 F32s from buf32 per node)
    for (int i=0; i<numNodes; i++) { rF32(); rF32(); rF32(); }
    // Guard 8
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 8 at p32=%zu (p16=%zu p8=%zu)\n", p32-1, p16-1, p8-1); }
    // Node rotations/translations
    for (int i=0; i<numNodeRot; i++) { rS16(); rS16(); rS16(); rS16(); }
    for (int i=0; i<numNodeTrans; i++) { rF32(); rF32(); rF32(); }
    fprintf(stderr, "after nodeRot/Trans: p32=%zu p16=%zu p8=%zu\n", p32, p16, p8);
    // No guard 9 for v<22
    // Object states: F32 + S32 + S32 per entry
    for (int i=0; i<numObjStates; i++) { rF32(); rS32(); rS32(); }
    // Guard 10 (v19 guard counter = 9 since no guard 9)
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 9/decal at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    // Decal states
    for (int i=0; i<numDecalStates; i++) rS32();
    // Guard 11/triggers
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 10/trigger at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    // Triggers
    for (int i=0; i<numTriggers; i++) { rU32(); rF32(); }
    // Guard 12/details
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 11/detail at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    // Details: 7 S32s each
    for (int i=0; i<numDetails; i++) { rS32(); rS32(); rS32(); rF32(); rF32(); rF32(); rS32(); }
    // Guard 13/meshStart
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "guard 12/mesh at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    
    fprintf(stderr, "\n=== MESH SECTION: p32=%zu p16=%zu p8=%zu ===\n", p32, p16, p8);
    
    // Now trace mesh[0] exactly as parser does
    uint32_t meshType = rU32();
    fprintf(stderr, "mesh[0] type=%u at p32=%zu\n", meshType, p32-1);
    
    // Guard START
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "mesh START at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    
    int32_t numFrames=rS32(), numMatFrames=rS32(), parentMesh=rS32();
    fprintf(stderr, "frames=%d matFrames=%d parent=%d at p32=%zu\n", numFrames, numMatFrames, parentMesh, p32);
    
    // Bounds: 10 F32s
    for (int i=0; i<10; i++) rF32();
    fprintf(stderr, "after bounds: p32=%zu\n", p32);
    
    int32_t numVerts = rS32();
    fprintf(stderr, "numVerts=%d at p32=%zu\n", numVerts, p32-1);
    
    // Verts
    for (int i=0; i<numVerts*3; i++) rF32();
    fprintf(stderr, "after verts: p32=%zu\n", p32);
    
    int32_t numTVerts = rS32();
    fprintf(stderr, "numTVerts=%d at p32=%zu\n", numTVerts, p32-1);
    
    // TVerts
    for (int i=0; i<numTVerts*2; i++) rF32();
    
    // Normals: for v<=21, read 3*numVerts F32s from buf32 only
    // For v>21, read 3*numVerts F32s from buf32 (skip) + numVerts U8s from buf8
    if (ver > 21) {
        // v>22: skip normals (3*numVerts from buf32) + read encoded (numVerts from buf8)
        for (int i=0; i<numVerts*3; i++) rF32();
        for (int i=0; i<numVerts; i++) rU8();
    } else {
        // v<=21: read normals (3*numVerts from buf32), NO U8s
        for (int i=0; i<numVerts*3; i++) rF32();
    }
    fprintf(stderr, "after normals: p32=%zu p16=%zu p8=%zu\n", p32, p16, p8);
    
    // Primitives: numPrims S32 (buf32), numPrims*2 S16 (buf16), numPrims S32 (buf32)
    int32_t numPrims = rS32();
    fprintf(stderr, "numPrims=%d at p32=%zu\n", numPrims, p32-1);
    for (int i=0; i<numPrims*2; i++) rS16();
    for (int i=0; i<numPrims; i++) rS32();
    
    // Indices: numIndices S32 (buf32), numIndices S16 (buf16)
    int32_t numIndices = rS32();
    fprintf(stderr, "numIndices=%d at p32=%zu\n", numIndices, p32-1);
    for (int i=0; i<numIndices; i++) rU16();
    
    // Merge: numMerge S32 (buf32), numMerge S16 (buf16)
    int32_t numMerge = rS32();
    fprintf(stderr, "numMerge=%d at p32=%zu\n", numMerge, p32-1);
    for (int i=0; i<numMerge; i++) rS16();
    
    rS32(); // vertsPerFrame
    rU32(); // flags
    fprintf(stderr, "before END guard: p32=%zu p16=%zu p8=%zu\n", p32, p16, p8);
    
    // END guard
    { int32_t g=rS32(); uint16_t g16=rU16(); uint8_t g8=rU8(); fprintf(stderr, "mesh END at p32=%zu: %d/%d/%d\n", p32-1, g, g16, g8); }
    
    return 0;
}
