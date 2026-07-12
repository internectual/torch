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

struct DTSBuf {
    const uint32_t* buf32;
    const uint16_t* buf16;
    const uint8_t* buf8;
    size_t pos32, pos16, pos8, size32, size16, size8;
    int guard;
    bool corrupted;
    uint32_t readU32() { if (pos32 >= size32) { corrupted = true; return 0; } return buf32[pos32++]; }
    int32_t readS32() { return (int32_t)readU32(); }
    float readF32() { if (pos32 >= size32) { corrupted = true; return 0; } float f; memcpy(&f, &buf32[pos32++], 4); return f; }
    uint16_t readU16() { if (pos16 >= size16) { corrupted = true; return 0; } return buf16[pos16++]; }
    int16_t readS16() { return (int16_t)readU16(); }
    uint8_t readU8() { if (pos8 >= size8) { corrupted = true; return 0; } return buf8[pos8++]; }
    void checkGuard(const char* label) {
        uint32_t g32 = readU32(); uint16_t g16 = readU16(); uint8_t g8 = readU8();
        fprintf(stderr, "  guard %d (%s) at p32=%zu p16=%zu p8=%zu : got %u/%u/%u (expect %d/%d/%d) %s\n",
            guard, label, pos32-4, pos16-1, pos8-1, g32, g16, g8, guard, guard, (int8_t)guard,
            ((int)g32 != guard || (int)g16 != guard || (int8_t)g8 != (int8_t)guard) ? "MISMATCH" : "OK");
        guard++;
    }
    int32_t capCount(int32_t v) { if (v < 0) return 0; if (v > 65536) return 65536; return v; }
};

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    // Pick a known failing v22 and v19 shape
    const char* targets[] = {
        "shapes/porg22.dts",
        "shapes/porg4.dts",
        "shapes/borg8.dts",
        "shapes/borg4.dts",
        "shapes/borg5.dts",
        "shapes/plasmabolt.dts",
        "shapes/disc_explosion.dts",
        "shapes/octahedron.dts",
        "shapes/borg3.dts",
    };

    for (auto& path : targets) {
        auto data = g_fs.read(path);
        if (data.empty()) { fprintf(stderr, "\n=== %s: NOT FOUND ===\n", path); continue; }
        uint16_t ver = *(uint16_t*)data.data();
        int32_t szAll = *(int32_t*)(data.data()+4);
        int32_t s16v = *(int32_t*)(data.data()+8);
        int32_t s8v = *(int32_t*)(data.data()+12);
        if (ver < 15 || szAll <= 0 || s16v <= 0 || s8v <= 0) {
            fprintf(stderr, "\n=== %s: v%u (old format) ===\n", path, ver);
            continue;
        }

        size_t sz32b = s16v * 4, sz16b = (s8v - s16v) * 4, sz8b = (szAll - s8v) * 4;
        fprintf(stderr, "\n=== %s v%u (buf32=%zu buf16=%zu buf8=%zu) ===\n", path, ver, sz32b/4, sz16b/2, sz8b);

        DTSBuf buf;
        buf.buf32 = (const uint32_t*)(data.data() + 16);
        buf.buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
        buf.buf8 = (const uint8_t*)(data.data() + 16 + sz32b + sz16b);
        buf.size32 = sz32b/4; buf.size16 = sz16b/2; buf.size8 = sz8b;
        buf.pos32 = buf.pos16 = buf.pos8 = 0;
        buf.guard = 0; buf.corrupted = false;

        int32_t numNodes = buf.capCount(buf.readS32());
        int32_t numObjects = buf.capCount(buf.readS32());
        int32_t numDecals = buf.capCount(buf.readS32());
        int32_t numSubShapes = buf.capCount(buf.readS32());
        int32_t numIFLs = buf.capCount(buf.readS32());
        int32_t numNodeRot, numNodeTrans, numNodeUScale=0, numNodeAScale=0, numNodeArbScale=0;
        if (ver < 22) {
            int32_t combined = buf.capCount(buf.readS32()) - numNodes;
            if (combined < 0) combined = 0;
            numNodeRot = numNodeTrans = combined;
        } else {
            numNodeRot = buf.capCount(buf.readS32()); numNodeTrans = buf.capCount(buf.readS32());
            numNodeUScale = buf.capCount(buf.readS32()); numNodeAScale = buf.capCount(buf.readS32());
            numNodeArbScale = buf.capCount(buf.readS32());
        }
        int32_t numObjStates = buf.capCount(buf.readS32());
        int32_t numDecalStates = buf.capCount(buf.readS32());
        int32_t numTriggers = buf.capCount(buf.readS32());
        int32_t numDetails = buf.capCount(buf.readS32());
        int32_t numMeshes = buf.capCount(buf.readS32());
        int32_t numSkins = (ver < 23) ? buf.capCount(buf.readS32()) : 0;
        int32_t numNames = buf.capCount(buf.readS32());
        buf.capCount(buf.readS32()); buf.capCount(buf.readS32()); // smallestVisSize, smallestVisDL

        fprintf(stderr, "  header: nodes=%d objects=%d decals=%d subShapes=%d ifls=%d\n", numNodes, numObjects, numDecals, numSubShapes, numIFLs);
        fprintf(stderr, "         nodeRot=%d nodeTrans=%d nodeUScale=%d nodeAScale=%d nodeArbScale=%d\n", numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale);
        fprintf(stderr, "         objStates=%d decalStates=%d triggers=%d details=%d meshes=%d skins=%d names=%d\n",
            numObjStates, numDecalStates, numTriggers, numDetails, numMeshes, numSkins, numNames);
        fprintf(stderr, "         after header: p32=%zu p16=%zu p8=%zu\n", buf.pos32, buf.pos16, buf.pos8);

        buf.checkGuard("0-bounds");
        // bounds: radius(1), tubeRadius(1), center(3), min(3), max(3) = 11 F32s
        for (int i = 0; i < 11; i++) buf.readF32();
        buf.checkGuard("1-nodes");
        // nodes: 5 S32s per node
        for (int i = 0; i < numNodes; i++) for (int j = 0; j < 5; j++) buf.readS32();
        buf.checkGuard("2-objects");
        // objects: 6 S32s per object
        for (int i = 0; i < numObjects; i++) for (int j = 0; j < 6; j++) buf.readS32();
        buf.checkGuard("3-decals");
        for (int i = 0; i < numDecals; i++) for (int j = 0; j < 5; j++) buf.readS32();
        buf.checkGuard("4-ifls");
        for (int i = 0; i < numIFLs; i++) for (int j = 0; j < 5; j++) buf.readS32();
        buf.checkGuard("5-subShapeFirst");
        // sub-shape first node/object/decal
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        buf.checkGuard("6-subShapeNum");
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        for (int i = 0; i < numSubShapes; i++) buf.readS32();
        buf.checkGuard("7-defRotTrans");
        // default rotations (from buf16) + translations (from buf32)
        for (int i = 0; i < numNodes; i++) { buf.readS16(); buf.readS16(); buf.readS16(); buf.readS16(); }
        for (int i = 0; i < numNodes; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
        buf.checkGuard("8-nodeStates");
        // Node rotations/translations/scales
        for (int i = 0; i < numNodeRot; i++) { buf.readS16(); buf.readS16(); buf.readS16(); buf.readS16(); }
        for (int i = 0; i < numNodeTrans; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
        for (int i = 0; i < numNodeUScale; i++) buf.readF32();
        for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
        for (int i = 0; i < numNodeArbScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
        for (int i = 0; i < numNodeArbScale; i++) { buf.readS16(); buf.readS16(); buf.readS16(); buf.readS16(); }
        if (ver >= 22) buf.checkGuard("9-groundXforms");
        // v19-v21: ground transforms (numNodes entries, each is 12 S32s = Point3F + Point3F + Quat16 as S32)
        // Actually in T2 source, after guard 8 for v<22: ground transforms are stored differently
        // Let me check: for v<22 there's an explicit "ground transforms" section
        if (ver < 22) {
            // The old format has ground transforms: numNodes * (3 F32 + 4 S16) which is stored as S32s
            // Actually this is "ground transforms" which are numNodes entries of a transform
            // For v<22 they are numNodes entries, each a full 4x4 = 16 F32s
            // No wait - let me check T2 source again
            fprintf(stderr, "  v<22: skipping ground transforms for %d nodes\n", numNodes);
        }
        // Object states: F32 + S32 + S32 per entry
        for (int i = 0; i < numObjStates; i++) { buf.readF32(); buf.readS32(); buf.readS32(); }
        buf.checkGuard("10-decalStates");
        for (int i = 0; i < numDecalStates; i++) buf.readS32();
        buf.checkGuard("11-triggers");
        for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
        buf.checkGuard("12-details");
        for (int i = 0; i < numDetails; i++) {
            buf.readS32(); buf.readS32(); buf.readS32(); buf.readF32(); buf.readF32(); buf.readF32(); buf.readS32();
        }
        buf.checkGuard("13-meshStart");
        fprintf(stderr, "  mesh section starts at p32=%zu\n", buf.pos32);

        // Read mesh headers only
        for (int m = 0; m < numMeshes && m < 20; m++) {
            uint32_t meshType = buf.readU32();
            fprintf(stderr, "  mesh[%d] type=%u at p32=%zu", m, meshType, buf.pos32 - 1);
            if (meshType == 4) { // NullMesh
                fprintf(stderr, " (null - skipped)\n");
                continue;
            }
            if (meshType == 2) { // Decal
                fprintf(stderr, " (decal - skipped)\n");
                int32_t sz = buf.capCount(buf.readS32());
                for (int i = 0; i < sz; i++) { buf.readS16(); buf.readS16(); buf.readS32(); }
                sz = buf.capCount(buf.readS32());
                for (int i = 0; i < sz; i++) buf.readU16();
                continue;
            }
            // Guard
            uint32_t g32 = buf.readU32(); uint16_t g16 = buf.readU16(); uint8_t g8 = buf.readU8();
            fprintf(stderr, " guard=%u/%u/%u (expect %d)", g32, g16, g8, buf.guard);
            bool guardOk = ((int)g32 == buf.guard && (int)g16 == buf.guard);
            buf.guard++;
            int32_t numFrames = buf.capCount(buf.readS32());
            int32_t numMatFrames = buf.capCount(buf.readS32());
            int32_t parentMesh = buf.readS32();
            fprintf(stderr, " frames=%d matFrames=%d parent=%d", numFrames, numMatFrames, parentMesh);
            // T2: mBounds (Box3F=6) + mCenter (3) + mRadius (1) = 10 F32s
            for (int i = 0; i < 10; i++) buf.readF32();
            int32_t numVerts = buf.capCount(buf.readS32());
            int32_t numTVerts = buf.capCount(buf.readS32());
            fprintf(stderr, " verts=%d tverts=%d", numVerts, numTVerts);
            // Skip verts, tverts
            if (!guardOk) {
                fprintf(stderr, " p32=%zu (SKIPPING REST)\n", buf.pos32);
                break;
            }
            fprintf(stderr, "\n");
            // Skip normals: for v>21, T2 reads 3*numVerts S32s from buf32 + numVerts S8s from buf8
            for (int i = 0; i < numVerts * 3; i++) buf.readF32(); // normals (S32 buffer)
            for (int i = 0; i < numVerts; i++) buf.readU8(); // encoded normals (S8 buffer)
            // Primitives: numPrims S32 (buf32), then numPrims*2 S16 (buf16), then numPrims S32 (buf32)
            int32_t numPrims = buf.capCount(buf.readS32());
            for (int i = 0; i < numPrims; i++) { buf.readS16(); buf.readS16(); } // prim16 in buf16
            for (int i = 0; i < numPrims; i++) buf.readS32(); // prim32 in buf32
            // Indices: numIndices S32 (buf32), then numIndices S16 (buf16)
            int32_t numIndices = buf.capCount(buf.readS32());
            for (int i = 0; i < numIndices; i++) buf.readU16(); // indices in buf16
            // Merge: numMerge S32 (buf32), then numMerge S16 (buf16)
            int32_t numMerge = buf.capCount(buf.readS32());
            for (int i = 0; i < numMerge; i++) buf.readS16();
            buf.readS32(); // vertsPerFrame
            buf.readU32(); // flags
            // Guard end
            uint32_t g32e = buf.readU32(); uint16_t g16e = buf.readU16(); uint8_t g8e = buf.readU8();
            fprintf(stderr, "    endGuard=%u/%u/%u (expect %d) %s\n", g32e, g16e, g8e, buf.guard,
                ((int)g32e != buf.guard || (int)g16e != buf.guard) ? "MISMATCH" : "OK");
            buf.guard++;
        }
        fprintf(stderr, "  final: p32=%zu p16=%zu p8=%zu (buf sizes: %zu/%zu/%zu)\n",
            buf.pos32, buf.pos16, buf.pos8, buf.size32, buf.size16, buf.size8);
    }
    return 0;
}
