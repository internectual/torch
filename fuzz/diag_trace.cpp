#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

// Copy of DTSBuf from dts_loader.cpp with debug printing
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
            fprintf(stderr, "GUARD mismatch at %d (got %u/%u/%u) pos32=%zu pos16=%zu pos8=%zu - continuing\n",
                    guard, g32, g16, g8, pos32, pos16, pos8);
        }
        guard++;
    }
    void alignS16() { if (pos16 & 1) pos16++; }
    void align8() { pos8 = (pos8 + 3) & ~(size_t)3; }
    void skip32(size_t n) { pos32 += n; }
};

static const int kMaxDTSCount = 1 << 16;
static inline int32_t capCount(int32_t v) {
    if (v < 0) return 0;
    if (v > kMaxDTSCount) return kMaxDTSCount;
    return v;
}

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2.open((basePath + "/shapes.vl2").c_str());
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read("shapes/bioderm_medium.dts");
    if (data.empty()) { fprintf(stderr, "Failed to read\n"); return 1; }

    DTSBuf buf;
    const uint8_t* d = data.data();
    uint16_t ver = *(const uint16_t*)(d);
    int32_t szAll = *(const int32_t*)(d+4);
    int32_t s16 = *(const int32_t*)(d+8);
    int32_t s8 = *(const int32_t*)(d+12);
    size_t sz32b = (size_t)s16 * 4, sz16b = (size_t)(s8 - s16) * 4, sz8b = (size_t)(szAll - s8) * 4;

    buf.buf32 = (const uint32_t*)(d + 16);
    buf.buf16 = (const uint16_t*)(d + 16 + sz32b);
    buf.buf8  = (const uint8_t*)(d + 16 + sz32b + sz16b);
    buf.size32 = sz32b / 4; buf.size16 = sz16b / 2; buf.size8 = sz8b;

    fprintf(stderr, "DTS v%u, 32bit size=%zu 16bit size=%zu 8bit size=%zu\n", ver, buf.size32, buf.size16, buf.size8);

    int32_t numNodes = capCount(buf.readS32()), numObjects = capCount(buf.readS32()), numDecals = capCount(buf.readS32());
    int32_t numSubShapes = capCount(buf.readS32()), numIFLs = capCount(buf.readS32());
    int32_t numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale;
    if (ver < 22) {
        int32_t combined = capCount(buf.readS32()) - numNodes;
        if (combined < 0) combined = 0;
        numNodeRot = numNodeTrans = combined;
        numNodeUScale = numNodeAScale = numNodeArbScale = 0;
    } else {
        numNodeRot = capCount(buf.readS32()); numNodeTrans = capCount(buf.readS32());
        numNodeUScale = capCount(buf.readS32()); numNodeAScale = capCount(buf.readS32()); numNodeArbScale = capCount(buf.readS32());
    }
    int32_t numGroundFrames = 0;
    if (ver > 23) numGroundFrames = capCount(buf.readS32());
    int32_t numObjStates = capCount(buf.readS32()), numDecalStates = capCount(buf.readS32()), numTriggers = capCount(buf.readS32());
    int32_t numDetails = capCount(buf.readS32()), numMeshes = capCount(buf.readS32());
    if (numMeshes > 10000) numMeshes = 10000;
    int32_t numSkins = (ver < 23) ? capCount(buf.readS32()) : 0; (void)numSkins;
    int32_t numNames = capCount(buf.readS32());
    capCount(buf.readS32()); capCount(buf.readS32());

    fprintf(stderr, "numNodes=%d numObjects=%d numDecals=%d numSubShapes=%d numIFLs=%d\n",
            numNodes, numObjects, numDecals, numSubShapes, numIFLs);
    fprintf(stderr, "numNodeRot=%d numNodeTrans=%d numNodeUScale=%d numNodeAScale=%d numNodeArbScale=%d\n",
            numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale);
    fprintf(stderr, "numGroundFrames=%d numObjStates=%d numDecalStates=%d numTriggers=%d\n",
            numGroundFrames, numObjStates, numDecalStates, numTriggers);
    fprintf(stderr, "numDetails=%d numMeshes=%d numSkins=%d numNames=%d\n",
            numDetails, numMeshes, numSkins, numNames);

    buf.checkGuard(); // 0
    fprintf(stderr, "After guard 0: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.readF32(); buf.readF32(); buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F();
    fprintf(stderr, "After bounds: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.checkGuard(); // 1
    fprintf(stderr, "After guard 1: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    for (int i = 0; i < numNodes; i++) {
        capCount(buf.readS32()); buf.readS32(); capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32());
    }
    fprintf(stderr, "After nodes: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.checkGuard(); // 2

    for (int i = 0; i < numObjects; i++) {
        for (int j = 0; j < 6; j++) capCount(buf.readS32());
    }
    fprintf(stderr, "After objects: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.checkGuard(); // 3

    for (int i = 0; i < numDecals; i++) {
        for (int j = 0; j < 5; j++) capCount(buf.readS32());
    }
    fprintf(stderr, "After decals (33*5=165): pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.checkGuard(); // 4

    for (int i = 0; i < numIFLs; i++) {
        for (int j = 0; j < 5; j++) capCount(buf.readS32());
    }
    fprintf(stderr, "After IFLs (0*5=0): pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);
    buf.checkGuard(); // 5

    fprintf(stderr, "Before subshapes: pos32=%zu, reading %d subshapes\n", buf.pos32, numSubShapes);

    // First subshape set
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  firstNode[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  firstObject[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  firstDecal[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    buf.checkGuard(); // 6
    fprintf(stderr, "After guard 6: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Second subshape set (if exists)
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  second1[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  second2[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    for (int i = 0; i < numSubShapes; i++) fprintf(stderr, "  second3[%d]=%d pos32=%zu\n", i, capCount(buf.readS32()), buf.pos32);
    buf.checkGuard(); // 7
    fprintf(stderr, "After guard 7: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Read default rotations (Quat16 from buf16)
    for (int i = 0; i < numNodes; i++) buf.readQuat16();
    buf.align8();
    fprintf(stderr, "After defRot+align8: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Read default translations (Point3F from buf32)
    for (int i = 0; i < numNodes; i++) buf.readPoint3F();
    fprintf(stderr, "After defTrans: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Node keyframes
    for (int i = 0; i < numNodeRot; i++) buf.readQuat16();
    for (int i = 0; i < numNodeTrans; i++) buf.readPoint3F();
    buf.align8();
    buf.checkGuard(); // 8
    fprintf(stderr, "After guard 8: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Scales
    for (int i = 0; i < numNodeUScale; i++) buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) buf.readQuat16();
    if (ver >= 22) { buf.align8(); buf.checkGuard(); } // 9
    fprintf(stderr, "After guard 9: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Ground frames (v > 23 only)
    if (ver > 23) {
        for (int i = 0; i < numGroundFrames; i++) buf.readPoint3F();
        for (int i = 0; i < numGroundFrames; i++) buf.readQuat16();
        buf.alignS16();
        buf.checkGuard();
    }

    // Object states
    for (int i = 0; i < numObjStates; i++) {
        buf.readF32(); capCount(buf.readS32()); capCount(buf.readS32());
    }
    buf.checkGuard(); // 10
    fprintf(stderr, "After guard 10: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    for (int i = 0; i < numDecalStates; i++) capCount(buf.readS32());
    buf.checkGuard(); // 11
    fprintf(stderr, "After guard 11: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
    buf.checkGuard(); // 12
    fprintf(stderr, "After guard 12: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Details
    for (int i = 0; i < numDetails; i++) {
        capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32());
        buf.readF32(); buf.readF32(); buf.readF32(); capCount(buf.readS32());
    }
    buf.checkGuard(); // 13
    fprintf(stderr, "After guard 13: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Meshes
    fprintf(stderr, "\n=== Reading %d meshes ===\n", numMeshes);
    for (int m = 0; m < numMeshes; m++) {
        if (buf.pos32 >= buf.size32 && m > 5) {
            fprintf(stderr, "  Mesh[%d]: buffer exhausted, stopping\n", m);
            break;
        }
        uint32_t meshTypeRaw = buf.readU32();
        uint32_t meshType = meshTypeRaw & 0x7;
        fprintf(stderr, "Mesh[%d]: raw=%u type=%u pos32=%zu\n", m, meshTypeRaw, meshType, buf.pos32);

        if (meshType >= 5) {
            fprintf(stderr, "  *** UNKNOWN mesh type %u! ***\n", meshType);
            // Try to recover by skipping some data
            buf.skip32(100);
            continue;
        }
    }

    return 0;
}

// Hack: add skip32 to DTSBuf
