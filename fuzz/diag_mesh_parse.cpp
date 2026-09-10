#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include "fs/file_system.h"
#include "fs/vl2_archive.h"
#include "core/engine.h"
#include "core/math.h"

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
            fprintf(stderr, "GUARD mismatch at %d (got %u/%u/%u) pos32=%zu pos16=%zu pos8=%zu\n",
                    guard, g32, g16, g8, pos32, pos16, pos8);
        }
        guard++;
    }
    void alignS16() { if (pos16 & 1) pos16++; }
    void align8() { pos8 = (pos8 + 3) & ~(size_t)3; }
};

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

    buf.checkGuard(); // 0
    buf.readF32(); buf.readF32(); buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F();
    buf.checkGuard(); // 1
    for (int i = 0; i < numNodes; i++) { capCount(buf.readS32()); buf.readS32(); capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard(); // 2
    for (int i = 0; i < numObjects; i++) { for (int j = 0; j < 6; j++) capCount(buf.readS32()); }
    buf.checkGuard(); // 3
    for (int i = 0; i < numDecals; i++) { for (int j = 0; j < 5; j++) capCount(buf.readS32()); }
    buf.checkGuard(); // 4
    for (int i = 0; i < numIFLs; i++) { for (int j = 0; j < 5; j++) capCount(buf.readS32()); }
    buf.checkGuard(); // 5
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    buf.checkGuard(); // 6
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    buf.checkGuard(); // 7

    // defRot (Quat16 from buf16)
    for (int i = 0; i < numNodes; i++) buf.readQuat16();
    buf.align8();
    // defTrans (Point3F from buf32)
    for (int i = 0; i < numNodes; i++) buf.readPoint3F();
    // node keyframes (all 0)
    buf.align8();
    buf.checkGuard(); // 8
    // scales (all 0)
    if (ver >= 22) { buf.align8(); buf.checkGuard(); } // 9
    // ground frames (none)
    // Object states
    for (int i = 0; i < numObjStates; i++) { buf.readF32(); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard(); // 10
    for (int i = 0; i < numDecalStates; i++) capCount(buf.readS32());
    buf.checkGuard(); // 11
    for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
    buf.checkGuard(); // 12
    // Details
    for (int i = 0; i < numDetails; i++) {
        capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32());
        buf.readF32(); buf.readF32(); buf.readF32(); capCount(buf.readS32());
    }
    buf.checkGuard(); // 13

    fprintf(stderr, "\nAfter guard 13: pos32=%zu pos16=%zu pos8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

    // Now read meshes with full body parsing
    fprintf(stderr, "\n=== Reading meshes ===\n");
    enum { DTSMesh_Standard = 0, DTSMesh_Skin = 1, DTSMesh_Decal = 2, DTSMesh_Sorted = 3, DTSMesh_Null = 4 };

    for (int m = 0; m < numMeshes; m++) {
        if (buf.pos32 >= buf.size32 && m > 100) { fprintf(stderr, "Buffer exhausted at mesh %d\n", m); break; }
        uint32_t meshTypeRaw = buf.readU32();
        uint32_t meshType = meshTypeRaw & 0x7;
        
        if (meshType == DTSMesh_Null) {
            fprintf(stderr, "Mesh[%d]: NULL (type=%u)\n", m, meshType);
            continue;
        }
        if (meshType == DTSMesh_Decal) {
            fprintf(stderr, "Mesh[%d]: DECAL (type=%u)\n", m, meshType);
            // Skip decal data...
            continue;
        }

        fprintf(stderr, "Mesh[%d]: type=%u raw=%u pos32=%zu ", m, meshType, meshTypeRaw, buf.pos32);
        buf.checkGuard(); // START
        
        int32_t numFrames = capCount(buf.readS32());
        int32_t numMatFrames = capCount(buf.readS32());
        int32_t parentMesh = buf.readS32();
        bool shareData = (parentMesh >= 0);
        
        // bounds: 3 Point3F + 1 F32 = 10 values
        buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F(); buf.readF32();
        
        int32_t numVerts = capCount(buf.readS32());
        
        fprintf(stderr, "frames=%d matFrames=%d parent=%d share=%d verts=%d pos32=%zu", 
                numFrames, numMatFrames, parentMesh, shareData, numVerts, buf.pos32);

        // Skip verts
        for (int f = 0; f < numFrames; f++)
            for (int v = 0; v < numVerts; v++) buf.readPoint3F();
        
        int32_t numTVerts = capCount(buf.readS32());
        for (int f = 0; f < numMatFrames; f++)
            for (int t = 0; t < numTVerts; t++) { buf.readF32(); buf.readF32(); }
        
        // Normals
        if (numVerts > 0) {
            for (int i = 0; i < numVerts; i++) buf.readPoint3F();
            // v > 21: encoded normals
            for (int i = 0; i < numVerts; i++) buf.readU8();
        }
        
        int32_t numPrimitives = capCount(buf.readS32());
        for (int i = 0; i < numPrimitives; i++) { buf.readS16(); buf.readS16(); }
        buf.alignS16();
        for (int i = 0; i < numPrimitives; i++) buf.readS32();
        
        int32_t numIndices = capCount(buf.readS32());
        for (int i = 0; i < numIndices; i++) buf.readU16();
        
        int32_t numMerge = capCount(buf.readS32());
        for (int i = 0; i < numMerge; i++) buf.readS16();
        
        buf.readS32(); // vertsPerFrame
        buf.readU32(); // flags
        buf.checkGuard(); // END

        if (meshType == DTSMesh_Skin) {
            int32_t sz = capCount(buf.readS32()); // initialVerts count
            fprintf(stderr, " SKEIN sz=%d", sz);
            if (!shareData) {
                for (int i = 0; i < sz; i++) buf.readPoint3F(); // initV
                for (int i = 0; i < sz; i++) buf.readPoint3F(); // initN
                for (int i = 0; i < sz; i++) buf.readU8(); // encoded normals
                sz = capCount(buf.readS32()); // initTransforms
                for (int i = 0; i < sz; i++) for (int j = 0; j < 16; j++) buf.readF32(); // matrix
                sz = capCount(buf.readS32()); // vertexIndex
                for (int i = 0; i < sz; i++) { buf.readS32(); buf.readS32(); buf.readF32(); } // vertIdx, boneIdx, weight
                sz = capCount(buf.readS32()); // nodeIndex
                for (int i = 0; i < sz; i++) buf.readS32(); // nodeIdx
                buf.checkGuard(); // skin end
                fprintf(stderr, " hasSkin=TRUE shareData=FALSE");
            } else {
                // Shared data - only counts
                sz = capCount(buf.readS32()); // initTransforms count
                sz = capCount(buf.readS32()); // vertexIndex count
                sz = capCount(buf.readS32()); // nodeIndex count
                buf.checkGuard(); // skin end
                fprintf(stderr, " hasSkin=FALSE shareData=TRUE");
            }
        }

        // Skip sorted/decal extensions
        if (meshType == 3) {
            int32_t sz = capCount(buf.readS32());
            for (int i = 0; i < sz * 8; i++) buf.readS32();
            for (int i = 0; i < 4; i++) { sz = capCount(buf.readS32()); for (int j = 0; j < sz; j++) buf.readS32(); }
            buf.readU8();
            buf.checkGuard();
        }

        fprintf(stderr, "\n");

        // Only show first 30 meshes in detail
        if (m >= 30) {
            fprintf(stderr, "(suppressing detail for remaining meshes)\n");
            // Just count mesh types
            int type0 = 0, type1 = 0, type2 = 0, type3 = 0, type4 = 0, typeOther = 0, skinCount = 0, skinShared = 0;
            for (int m2 = m + 1; m2 < numMeshes; m2++) {
                if (buf.pos32 >= buf.size32) break;
                uint32_t mt = buf.readU32();
                uint32_t t = mt & 0x7;
                if (t == 0) type0++;
                else if (t == 1) type1++;
                else if (t == 2) type2++;
                else if (t == 3) type3++;
                else if (t == 4) type4++;
                else typeOther++;
                // Can't easily skip mesh body, just count
            }
            fprintf(stderr, "Remaining: type0=%d type1=%d type2=%d type3=%d type4=%d other=%d\n",
                    type0, type1, type2, type3, type4, typeOther);
            break;
        }
    }

    return 0;
}
