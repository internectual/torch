#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

static const int kMaxDTSCount = 1 << 16;
static inline int32_t capCount(int32_t v) {
    if (v < 0) return 0;
    if (v > kMaxDTSCount) return kMaxDTSCount;
    return v;
}

struct DTSBuf {
    const uint32_t* buf32 = nullptr;
    const uint16_t* buf16 = nullptr;
    const uint8_t*  buf8  = nullptr;
    size_t pos32 = 0, pos16 = 0, pos8 = 0;
    size_t size32 = 0, size16 = 0, size8 = 0;
    int guard = 0;
    bool corrupted = false;

    uint32_t readU32() {
        if (pos32 >= size32) { corrupted = true; return 0; }
        return buf32[pos32++];
    }
    int32_t  readS32() { return (int32_t)readU32(); }
    float    readF32() {
        if (pos32 >= size32) { corrupted = true; return 0; }
        float f; memcpy(&f, &buf32[pos32++], 4); return f;
    }
    uint16_t readU16() {
        if (pos16 >= size16) { corrupted = true; return 0; }
        return buf16[pos16++];
    }
    int16_t  readS16() { return (int16_t)readU16(); }
    uint8_t  readU8()  {
        if (pos8 >= size8) { corrupted = true; return 0; }
        return buf8[pos8++];
    }
    Point3F readPoint3F() {
        float x = readF32(), y = readF32(), z = readF32();
        return {x, y, z};
    }
    QuatF readQuat16() {
        int16_t x = readS16(), y = readS16(), z = readS16(), w = readS16();
        return {(float)x/32767.f, (float)y/32767.f, (float)z/32767.f, (float)w/32767.f};
    }
    void checkGuard() {
        uint32_t g32 = readU32();
        uint16_t g16 = readU16();
        uint8_t  g8  = readU8();
        if ((int)g32 != guard || (int)g16 != guard || (int8_t)g8 != (int8_t)guard) {
            fprintf(stderr, "GUARD mismatch at %d (got %u/%u/%u) - continuing\n", guard, g32, g16, g8);
        }
        guard++;
    }
    void alignS16() { if (pos16 & 1) pos16++; }
    void align8() { pos8 = (pos8 + 3) & ~(size_t)3; }
    void skip8(size_t n) { pos8 += n; }
    void skip16(size_t n) { pos16 += n; }
    void skip32(size_t n) { pos32 += n; }
};

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2.open((basePath + "/shapes.vl2").c_str());
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read("shapes/bioderm_medium.dts");
    if (data.empty()) { fprintf(stderr, "Failed to read\n"); return 1; }

    fprintf(stderr, "DTS size: %zu bytes\n", data.size());

    DTSBuf buf;
    const uint8_t* d = data.data();
    uint16_t ver = *(const uint16_t*)d;
    int32_t szAll = *(const int32_t*)(d+4);
    int32_t s16 = *(const int32_t*)(d+8);
    int32_t s8 = *(const int32_t*)(d+12);
    fprintf(stderr, "ver=%u szAll=%d s16=%d s8=%d\n", ver, szAll, s16, s8);

    size_t sz32b = (size_t)s16 * 4;
    size_t sz16b = (size_t)(s8 - s16) * 4;
    size_t sz8b = (size_t)(szAll - s8) * 4;

    buf.buf32 = (const uint32_t*)(d + 16);
    buf.buf16 = (const uint16_t*)(d + 16 + sz32b);
    buf.buf8  = (const uint8_t*)(d + 16 + sz32b + sz16b);
    buf.size32 = sz32b / 4; buf.size16 = sz16b / 2; buf.size8 = sz8b;

    int32_t numNodes = capCount(buf.readS32());
    int32_t numObjects = capCount(buf.readS32());
    int32_t numDecals = capCount(buf.readS32());
    int32_t numSubShapes = capCount(buf.readS32());
    int32_t numIFLs = capCount(buf.readS32());

    int32_t numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale;
    if (ver < 22) {
        int32_t combined = capCount(buf.readS32()) - numNodes;
        if (combined < 0) combined = 0;
        numNodeRot = numNodeTrans = combined;
        numNodeUScale = numNodeAScale = numNodeArbScale = 0;
    } else {
        numNodeRot = capCount(buf.readS32());
        numNodeTrans = capCount(buf.readS32());
        numNodeUScale = capCount(buf.readS32());
        numNodeAScale = capCount(buf.readS32());
        numNodeArbScale = capCount(buf.readS32());
    }

    int32_t numGroundFrames = 0;
    if (ver > 23) numGroundFrames = capCount(buf.readS32());
    int32_t numObjStates = capCount(buf.readS32());
    int32_t numDecalStates = capCount(buf.readS32());
    int32_t numTriggers = capCount(buf.readS32());
    int32_t numDetails = capCount(buf.readS32());
    int32_t numMeshes = capCount(buf.readS32());
    int32_t numSkins = (ver < 23) ? capCount(buf.readS32()) : 0;
    int32_t numNames = capCount(buf.readS32());
    capCount(buf.readS32()); capCount(buf.readS32()); // smallestVisSize, smallestVisDL

    fprintf(stderr, "numNodes=%d numObjects=%d numDecals=%d numSubShapes=%d numIFLs=%d\n",
            numNodes, numObjects, numDecals, numSubShapes, numIFLs);
    fprintf(stderr, "numNodeRot=%d numNodeTrans=%d numNodeUScale=%d numNodeAScale=%d numNodeArbScale=%d\n",
            numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale);
    fprintf(stderr, "numGroundFrames=%d numObjStates=%d numDecalStates=%d numTriggers=%d\n",
            numGroundFrames, numObjStates, numDecalStates, numTriggers);
    fprintf(stderr, "numDetails=%d numMeshes=%d numSkins=%d numNames=%d\n",
            numDetails, numMeshes, numSkins, numNames);

    // Guard 0
    buf.checkGuard();
    // bounds (3 Point3F + 1 F32)
    buf.readF32(); buf.readF32(); buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F();
    buf.checkGuard();

    // Nodes
    for (int i = 0; i < numNodes; i++) {
        capCount(buf.readS32()); buf.readS32(); capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32());
    }
    buf.checkGuard();

    // Objects
    for (int i = 0; i < numObjects; i++) { for (int j = 0; j < 6; j++) capCount(buf.readS32()); }
    buf.checkGuard();

    // Decals
    for (int i = 0; i < numDecals; i++) { for (int j = 0; j < 5; j++) capCount(buf.readS32()); }
    buf.checkGuard();

    // IFLs
    for (int i = 0; i < numIFLs; i++) { for (int j = 0; j < 5; j++) capCount(buf.readS32()); }
    buf.checkGuard();

    // SubShapes
    for (int i = 0; i < numSubShapes * 6; i++) capCount(buf.readS32());
    buf.checkGuard();

    // Default node states
    for (int i = 0; i < numNodes; i++) { buf.readQuat16(); } // defRot
    buf.align8();
    for (int i = 0; i < numNodes; i++) { buf.readPoint3F(); } // defTrans
    // Node keyframes
    for (int i = 0; i < numNodeRot; i++) { buf.readQuat16(); }
    for (int i = 0; i < numNodeTrans; i++) { buf.readPoint3F(); }
    buf.align8();
    buf.checkGuard(); // 8

    for (int i = 0; i < numNodeUScale; i++) buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) buf.readQuat16();
    if (ver >= 22) { buf.align8(); buf.checkGuard(); } // 9

    if (ver > 23) {
        for (int i = 0; i < numGroundFrames; i++) { buf.readPoint3F(); }
        for (int i = 0; i < numGroundFrames; i++) { buf.readQuat16(); }
        buf.alignS16();
        buf.checkGuard();
    }

    // Object states
    for (int i = 0; i < numObjStates; i++) { buf.readF32(); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard();

    for (int i = 0; i < numDecalStates; i++) capCount(buf.readS32());
    buf.checkGuard();

    for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
    buf.checkGuard();

    // Details
    for (int i = 0; i < numDetails; i++) {
        capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32()); buf.readF32(); buf.readF32(); buf.readF32(); capCount(buf.readS32());
    }
    buf.checkGuard(); // 13

    fprintf(stderr, "pos32 before meshes: %zu, pos16: %zu, pos8: %zu, guard: %d\n",
            buf.pos32, buf.pos16, buf.pos8, buf.guard);

    // Now read mesh types
    fprintf(stderr, "\n=== First 15 mesh types ===\n");
    for (int m = 0; m < std::min(numMeshes, 15); m++) {
        uint32_t mt = buf.readU32();
        uint32_t meshType = mt & 0x7;
        fprintf(stderr, "Mesh[%d]: raw=%u type=%u (32-bit pos was %zu)\n", m, mt, meshType, buf.pos32 - 1);

        if (meshType == 4) {
            fprintf(stderr, "  -> NULL mesh\n");
            continue;
        }
        if (meshType == 2) {
            fprintf(stderr, "  -> DECAL mesh\n");
            continue;
        }

        // START guard
        fprintf(stderr, "  START guard expected=%d\n", buf.guard);
        uint32_t g32 = buf.readU32();
        uint16_t g16 = buf.readU16();
        uint8_t  g8  = buf.readU8();
        fprintf(stderr, "  guard got=%u/%u/%u", g32, g16, g8);

        int32_t numFrames = capCount(buf.readS32());
        int32_t numMatFrames = capCount(buf.readS32());
        int32_t parentMesh = buf.readS32();
        fprintf(stderr, "  numFrames=%d numMatFrames=%d parentMesh=%d", numFrames, numMatFrames, parentMesh);

        // bounds: 3 Point3F + 1 F32
        Point3F b1 = buf.readPoint3F();
        Point3F b2 = buf.readPoint3F();
        Point3F b3 = buf.readPoint3F();
        float r = buf.readF32();
        fprintf(stderr, " bounds=(%.2f,%.2f,%.2f) rad=%.2f", b3.x, b3.y, b3.z, r);
        fprintf(stderr, "\n");

        // Now we need to skip the rest of this mesh data
        // Read numVerts
        int32_t numVerts = capCount(buf.readS32());
        int32_t numTVerts = capCount(buf.readS32());
        int32_t numNorms = capCount(buf.readS32());
        int32_t numPrims = capCount(buf.readS32());

        fprintf(stderr, "  numVerts=%d numTVerts=%d numNorms=%d numPrims=%d\n", numVerts, numTVerts, numNorms, numPrims);

        // Skip verts (numVerts * numFrames Point3Fs from buf32)
        buf.skip32(numVerts * 3); // This is wrong - need to account for numFrames

        // Actually for non-shared data:
        // verts: numVerts * numFrames Point3F from buf32
        // tverts: numTVerts * numMatFrames Point2F
        // but Point2F is 2 F32s
        // normals: numNorms Point3F from buf32
        // primitives: numPrims S16 pairs + S32 matIndex
        // indices: numIndices S16
        // merge: numMerge S16
        // vertsPerFrame: S32
        // flags: U32
        buf.skip32(numVerts * 3); // skip actual verts (just frame 0, since numFrames may be 1)

        // skip remaining frames
        // Actually for non-shared, verts are numVerts * numFrames
        // We already read numVerts (frame 0) above
        // Skip remaining frames
        for (int f = 1; f < numFrames; f++) buf.skip32(numVerts * 3);

        // TVerts: numTVerts * numMatFrames * 2
        buf.skip32(numTVerts * 2 * numMatFrames);

        // Normals: numVerts (not numVerts*numFrames)
        buf.skip32(numNorms * 3);

        // Primitives
        for (int i = 0; i < numPrims; i++) { buf.readU16(); buf.readU16(); } // start, numElements
        buf.alignS16();
        for (int i = 0; i < numPrims; i++) capCount(buf.readS32()); // matIndex

        // Indices
        int32_t numIndices = capCount(buf.readS32());
        buf.skip16(numIndices); // S16 each

        // Merge
        int32_t numMerge = capCount(buf.readS32());
        buf.skip16(numMerge);

        // vertsPerFrame, flags
        capCount(buf.readS32()); // vertsPerFrame
        buf.readU32(); // flags

        buf.checkGuard(); // END

        // For skin meshes: skin data
        if (meshType == 1) {
            fprintf(stderr, "  *** SKIN MESH DETECTED ***\n");
            int32_t sz = capCount(buf.readS32()); // initialVerts
            fprintf(stderr, "    initVerts=%d\n", sz);
            buf.skip32(sz * 3); // verts
            buf.skip32(sz * 3); // norms
            buf.skip8(sz); // encoded norms
            buf.align8();
            sz = capCount(buf.readS32()); // initTransforms
            fprintf(stderr, "    initTransforms=%d\n", sz);
            buf.skip32(sz * 16);
            sz = capCount(buf.readS32()); // vertexIndex
            fprintf(stderr, "    vertIdx=%d\n", sz);
            buf.skip32(sz);
            sz = capCount(buf.readS32()); // boneIndex
            fprintf(stderr, "    boneIdx=%d\n", sz);
            buf.skip32(sz);
            buf.skip32(sz); // weights (F32)
            sz = capCount(buf.readS32()); // nodeIndex
            fprintf(stderr, "    nodeIdx=%d\n", sz);
            buf.skip32(sz);
            buf.checkGuard(); // skin end
        }

        if (meshType == 3) {
            // Sorted
            int32_t sz = capCount(buf.readS32());
            buf.skip32(sz * 8);
            for (int i = 0; i < 4; i++) {
                sz = capCount(buf.readS32());
                buf.skip32(sz);
            }
            buf.readU8(); // alwaysWriteZ
            buf.checkGuard();
        }
    }

    fprintf(stderr, "\npos32 after 15 meshes: %zu, pos16: %zu, pos8: %zu, guard: %d\n",
            buf.pos32, buf.pos16, buf.pos8, buf.guard);

    return 0;
}
