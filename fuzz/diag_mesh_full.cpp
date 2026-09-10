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
    Point3F readPoint3F() { float x = readF32(), y = readF32(), z = readF32(); return {x, y, z}; }
    QuatF readQuat16() {
        int16_t x = (int16_t)readU16(), y = (int16_t)readU16(), z = (int16_t)readU16(), w = (int16_t)readU16();
        return QuatF{(float)x/32767.f, (float)y/32767.f, (float)z/32767.f, (float)w/32767.f};
    }
    void checkGuard() {
        uint32_t g32 = readU32();
        uint16_t g16 = readU16();
        uint8_t  g8  = readU8();
        if ((int)g32 != guard || (int)g16 != guard || (int8_t)g8 != (int8_t)guard) {
            // Only print if there's an actual mismatch
            if (pos32 > 1) fprintf(stderr, "GUARD mismatch at %d (got %u/%u/%u)\n", guard, g32, g16, g8);
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

    // Read header
    int32_t numNodes = capCount(buf.readS32()), numObjects = capCount(buf.readS32()), numDecals = capCount(buf.readS32());
    int32_t numSubShapes = capCount(buf.readS32()), numIFLs = capCount(buf.readS32());
    int32_t numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale;
    if (ver < 22) { int32_t c = capCount(buf.readS32()) - numNodes; numNodeRot = numNodeTrans = (c<0?0:c); numNodeUScale = numNodeAScale = numNodeArbScale = 0; }
    else { numNodeRot = capCount(buf.readS32()); numNodeTrans = capCount(buf.readS32()); numNodeUScale = capCount(buf.readS32()); numNodeAScale = capCount(buf.readS32()); numNodeArbScale = capCount(buf.readS32()); }
    int32_t numGroundFrames = (ver > 23) ? capCount(buf.readS32()) : 0;
    int32_t numObjStates = capCount(buf.readS32()), numDecalStates = capCount(buf.readS32()), numTriggers = capCount(buf.readS32());
    int32_t numDetails = capCount(buf.readS32()), numMeshes = capCount(buf.readS32());
    if (numMeshes > 10000) numMeshes = 10000;
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
    for (int i = 0; i < numSubShapes; i++) { capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard(); // 6
    for (int i = 0; i < numSubShapes; i++) { capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard(); // 7

    for (int i = 0; i < numNodes; i++) buf.readQuat16();
    buf.align8();
    for (int i = 0; i < numNodes; i++) buf.readPoint3F();
    buf.align8();
    buf.checkGuard(); // 8

    for (int i = 0; i < numNodeUScale; i++) buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) buf.readQuat16();
    if (ver >= 22) { buf.align8(); buf.checkGuard(); } // 9

    for (int i = 0; i < numObjStates; i++) { buf.readF32(); capCount(buf.readS32()); capCount(buf.readS32()); }
    buf.checkGuard(); // 10
    for (int i = 0; i < numDecalStates; i++) capCount(buf.readS32());
    buf.checkGuard(); // 11
    for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
    buf.checkGuard(); // 12
    for (int i = 0; i < numDetails; i++) { capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32()); buf.readF32(); buf.readF32(); buf.readF32(); capCount(buf.readS32()); }
    buf.checkGuard(); // 13

    fprintf(stderr, "\n=== Reading meshes from pos32=%zu pos16=%zu pos8=%zu ===\n", buf.pos32, buf.pos16, buf.pos8);

    // Read raw 32-bit values to understand the mesh section
    fprintf(stderr, "\n=== Raw 32-bit dump at mesh start (pos32=%zu) ===\n", buf.pos32);
    for (size_t i = buf.pos32; i < std::min(buf.size32, buf.pos32 + 50); i++) {
        fprintf(stderr, "[%zu]=%d ", i, (int32_t)buf.buf32[i]);
        if ((i - buf.pos32 + 1) % 10 == 0) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    // Now try the ACTUAL loadDTS parsing logic
    enum { DTSMesh_Standard = 0, DTSMesh_Skin = 1, DTSMesh_Decal = 2, DTSMesh_Sorted = 3, DTSMesh_Null = 4 };

    // Read names
    std::vector<std::string> names;
    for (int i = 0; i < numNames; i++) {
        std::string name;
        while (buf.pos8 < buf.size8 && buf.buf8[buf.pos8] != 0) { name += (char)buf.readU8(); }
        if (buf.pos8 < buf.size8) buf.readU8(); // null terminator
        names.push_back(name);
    }

    // Read all nodes
    struct Node { int32_t ni, pi, fo, fc, ns; };
    std::vector<Node> nodes(numNodes);
    for (int i = 0; i < numNodes; i++) {
        nodes[i].ni = capCount(buf.readS32());
        nodes[i].pi = buf.readS32();
        nodes[i].fo = capCount(buf.readS32());
        nodes[i].fc = capCount(buf.readS32());
        nodes[i].ns = capCount(buf.readS32());
    }

    for (int m = 0; m < numMeshes; m++) {
        if (buf.pos32 >= buf.size32) { fprintf(stderr, "Mesh[%d]: buffer exhausted\n", m); break; }
        uint32_t meshTypeRaw = buf.readU32();
        uint32_t meshType = meshTypeRaw & 0x7;

        if (meshType == DTSMesh_Null) {
            fprintf(stderr, "Mesh[%d]: NULL\n", m);
            continue;
        }
        if (meshType == DTSMesh_Decal) {
            fprintf(stderr, "Mesh[%d]: DECAL\n", m);
            // Try to skip decal... but we don't have the exact format
            break;
        }

        fprintf(stderr, "\nMesh[%d]: type=%u parentVer=%u share=", m, meshType, ver);
        buf.checkGuard(); // START

        int32_t numFrames = capCount(buf.readS32());
        int32_t numMatFrames = capCount(buf.readS32());
        int32_t parentMesh = buf.readS32(); // -1 means no parent
        bool shareData = (parentMesh >= 0);
        fprintf(stderr, "\n  numFrames=%d numMatFrames=%d parentMesh=%d share=%d ", numFrames, numMatFrames, parentMesh, shareData);

        buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F(); buf.readF32(); // bounds
        int32_t numVerts = capCount(buf.readS32());
        fprintf(stderr, "verts=%d ", numVerts);

        if (!shareData) {
            for (int f = 0; f < numFrames; f++)
                for (int v = 0; v < numVerts; v++) buf.readPoint3F();
        }

        int32_t numTVerts = capCount(buf.readS32());
        fprintf(stderr, "tverts=%d ", numTVerts);
        if (!shareData) {
            for (int f = 0; f < numMatFrames; f++)
                for (int t = 0; t < numTVerts; t++) { buf.readF32(); buf.readF32(); }
        }

        if (!shareData && numVerts > 0) {
            for (int i = 0; i < numVerts; i++) buf.readPoint3F(); // normals
            if (ver > 21) {
                for (int i = 0; i < numVerts; i++) buf.readU8(); // encoded normals
            }
        }

        int32_t numPrimitives = capCount(buf.readS32());
        fprintf(stderr, "prims=%d ", numPrimitives);
        for (int i = 0; i < numPrimitives; i++) { buf.readS16(); buf.readS16(); }
        buf.alignS16();
        for (int i = 0; i < numPrimitives; i++) { buf.readS32(); } // matIndex
        int32_t numIndices = capCount(buf.readS32());
        fprintf(stderr, "indices=%d ", numIndices);
        for (int i = 0; i < numIndices; i++) buf.readU16();
        int32_t numMerge = capCount(buf.readS32());
        fprintf(stderr, "merge=%d ", numMerge);
        for (int i = 0; i < numMerge; i++) buf.readS16();
        buf.readS32(); // vertsPerFrame
        buf.readU32(); // flags
        buf.checkGuard(); // END

        fprintf(stderr, "pos32=%zu ", buf.pos32);

        // Skin data
        if (meshType == DTSMesh_Skin) {
            fprintf(stderr, "\n  SKIN MESH DETECTED! ");
            int32_t sz = capCount(buf.readS32());
            fprintf(stderr, "\n  skin: initVerts=%d shareData=%d ", sz, shareData);
            if (!shareData) {
                for (int i = 0; i < sz; i++) buf.readPoint3F(); // initV
                for (int i = 0; i < sz; i++) buf.readPoint3F(); // initN
                for (int i = 0; i < sz; i++) buf.readU8(); // encoded normals
                sz = capCount(buf.readS32());
                fprintf(stderr, "initTransform=%d ", sz);
                for (int i = 0; i < sz; i++) for (int j = 0; j < 16; j++) buf.readF32();
                sz = capCount(buf.readS32());
                fprintf(stderr, "vertIdx=%d ", sz);
                for (int i = 0; i < sz; i++) { buf.readS32(); buf.readS32(); buf.readF32(); }
                sz = capCount(buf.readS32());
                fprintf(stderr, "nodeIdx=%d ", sz);
                for (int i = 0; i < sz; i++) buf.readS32();
                buf.checkGuard(); // skin end
                fprintf(stderr, "pos32=%zu", buf.pos32);
            } else {
                sz = capCount(buf.readS32());
                sz = capCount(buf.readS32());
                sz = capCount(buf.readS32());
                buf.checkGuard();
                fprintf(stderr, " SHARED (no data read) pos32=%zu", buf.pos32);
            }
        }

        if (buf.corrupted) { fprintf(stderr, "\nBUFFER CORRUPTED at mesh %d, stopping\n", m); break; }
        if (m >= 5) { fprintf(stderr, "\n(stopping after 5 meshes for brevity)\n"); break; }
    }

    return 0;
}
