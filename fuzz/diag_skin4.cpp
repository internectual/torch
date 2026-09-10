#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2_shapes.open((basePath + "/shapes.vl2").c_str());
    g_vl2_base.open((basePath + "/base.vl2").c_str());
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    auto data = g_fs.read("shapes/bioderm_medium.dts");
    if (data.empty()) { fprintf(stderr, "Failed to read bioderm\n"); return 1; }

    uint16_t ver = *(const uint16_t*)data.data();
    fprintf(stderr, "DTS version: %u\n", ver);

    if (ver >= 19) {
        const uint8_t* d = data.data();
        int32_t sz32 = *(int32_t*)(d + 4);
        int32_t sz16 = *(int32_t*)(d + 8);
        int32_t sz8 = *(int32_t*)(d + 12);
        fprintf(stderr, "sz32=%d sz16=%d sz8=%d\n", sz32, sz16, sz8);

        const uint32_t* buf32 = (const uint32_t*)(d + 16);
        const uint16_t* buf16 = (const uint16_t*)(d + 16 + (size_t)sz32*4);
        const uint8_t* buf8 = (const uint8_t*)(d + 16 + (size_t)sz32*4 + (size_t)sz16*2);

        size_t p32 = 0, p16 = 0, p8 = 0;
        auto r32 = [&]() -> int32_t { return (int32_t)buf32[p32++]; };
        auto rU32 = [&]() -> uint32_t { return buf32[p32++]; };
        auto rf32 = [&]() -> float { float f; memcpy(&f, &buf32[p32++], 4); return f; };
        auto rs16 = [&]() -> int16_t { return (int16_t)buf16[p16++]; };
        auto ru8 = [&]() -> uint8_t { return buf8[p8++]; };

        // Skip version header (5 S32s)
        for (int i = 0; i < 5; i++) r32();

        int32_t numNodes = r32();
        int32_t numObjects = r32();
        int32_t numDecals = r32();
        int32_t numIFLs = r32();
        int32_t numSubShapes = r32();

        fprintf(stderr, "numNodes=%d numObjects=%d numDecals=%d numIFLs=%d numSubShapes=%d\n",
                numNodes, numObjects, numDecals, numIFLs, numSubShapes);

        // Skip nodes (5 S32 each)
        p32 += numNodes * 5;
        // Skip objects (6 S32 each)
        p32 += numObjects * 6;
        // Skip decals (5 S32 each)
        p32 += numDecals * 5;
        // Skip IFLs (2 S32 each)
        p32 += numIFLs * 2;
        // Skip subshapes (6 S32 each)
        p32 += numSubShapes * 6;

        // Skip meshIndexList (v15 only, not present in v17+)
        // MeshIndexList: numMeshesUsed S32 + that many S32s
        // Actually for v17+, this is not read

        // Skip keyframe lists (v15-v16 only)
        // (not present in v17+)

        // Skip default node states (defRot, defTrans)
        // defRot: numNodes Quat16s from buf16
        p16 += numNodes * 4;
        // align8
        p8 = (p8 + 3) & ~(size_t)3;
        // defTrans: numNodes Point3Fs from buf32
        p32 += numNodes * 3;

        // nodeRotations: numNodeRot Quat16s
        int32_t numNodeRot = r32();
        p16 += numNodeRot * 4;
        // nodeTranslations: numNodeTrans Point3Fs
        int32_t numNodeTrans = r32();
        p32 += numNodeTrans * 3;
        // align8
        p8 = (p8 + 3) & ~(size_t)3;

        // nodeUScales
        int32_t numNodeUScale = r32();
        p32 += numNodeUScale;
        // nodeAScales (3 F32 each)
        int32_t numNodeAScale = r32();
        p32 += numNodeAScale * 3;
        // nodeAScaleRots (Quat16 each)
        int32_t numNodeArbScale = r32();
        p16 += numNodeArbScale * 4;
        if (ver >= 22) { p8 = (p8 + 3) & ~(size_t)3; }

        fprintf(stderr, "pos32 before sequences: %zu, pos16: %zu, pos8: %zu\n", p32, p16, p8);

        // Ground frame count
        int32_t numGroundFrames = r32();
        fprintf(stderr, "numGroundFrames=%d\n", numGroundFrames);

        // Read sequences
        int32_t numSequences = r32();
        fprintf(stderr, "numSequences=%d\n", numSequences);
        size_t p32seq = p32, p16seq = p16, p8seq = p8;
        for (int s = 0; s < numSequences; s++) {
            r32(); // nameIndex
            if (ver > 21) r32(); // flags
            if (ver < 17) { r32(); r32(); } else { r32(); } // numKeyframes
            rf32(); // duration
            if (ver < 22) { ru8(); ru8(); ru8(); } // blend, cyclic, makePath
            r32(); // priority
            r32(); // firstGroundFrame
            r32(); // numGroundFrames
            if (ver > 21) { for (int k = 0; k < 5; k++) r32(); }
            else if (ver >= 17) { for (int k = 0; k < 3; k++) r32(); }
            r32(); // firstTrigger
            r32(); // numTriggers
            rf32(); // toolBegin
            // rotationMatters
            int32_t sz = r32(); p32 += sz; // TSIntegerSet: count + sz ints
            if (ver >= 22) { sz = r32(); p32 += sz; } // translationMatters
            if (ver >= 22) { sz = r32(); p32 += sz; } // scaleMatters
            if (ver < 17) { sz = r32(); p32 += sz; } // nodeTransformStatic
            sz = r32(); p32 += sz; // decalMatters
            sz = r32(); p32 += sz; // iflMatters
            sz = r32(); p32 += sz; // visMatters
            sz = r32(); p32 += sz; // frameMatters
            sz = r32(); p32 += sz; // matFrameMatters
            if (ver < 17) { sz = r32(); p32 += sz; } // nodeTransformStatic
        }
        p32 = p32seq;
        p16 = p16seq;
        p8 = p8seq;

        // Read ground translations and rotations (v > 23)
        // For v < 24, ground frame data is embedded in sequences
        // For now, skip this

        // Skip object states
        int32_t numObjStates = r32();
        fprintf(stderr, "numObjStates=%d (pos32=%zu)\n", numObjStates, p32);
        p32 += numObjStates * 3; // vis, frameIndex, matFrameIndex

        // Skip decalstates, triggers
        int32_t numDecalStates = r32();
        p32 += numDecalStates;
        int32_t numTriggers = r32();
        p32 += numTriggers * 2; // state, pos

        // Details
        int32_t numDetails = r32();
        fprintf(stderr, "numDetails=%d (pos32=%zu)\n", numDetails, p32);
        for (int i = 0; i < numDetails; i++) {
            r32(); // nameIdx
            r32(); // subShape
            r32(); // objDetail
            rf32(); // size
            rf32(); rf32(); // avgError, maxError
            r32(); // polyCount
        }
        fprintf(stderr, "pos32 after details: %zu\n", p32);

        // Sequences (already accounted for above, but they're in the 8-bit buffer for post)
        // Actually no, for v17+, sequences are in the 32-bit buffer. Let me re-check.

        // Actually, I realized the sequence reading above was wrong - let me look at the actual code

        // Now read meshes
        int32_t numMeshes = r32();
        fprintf(stderr, "numMeshes=%d (pos32=%zu)\n", numMeshes, p32);

        // For each mesh, read the mesh type
        for (int m = 0; m < std::min(numMeshes, 30); m++) {
            uint32_t mt = rU32();
            uint32_t meshType = mt & 0x7;

            if (meshType == 4) {
                fprintf(stderr, "  mesh[%d]: Null (type=%u)\n", m, meshType);
                continue;
            }
            if (meshType == 2) {
                fprintf(stderr, "  mesh[%d]: Decal (type=%u)\n", m, meshType);
                // skip decal data... complex, just note it
                continue;
            }
            if (meshType == 3) {
                fprintf(stderr, "  mesh[%d]: Sorted (type=%u)\n", m, meshType);
                // skip for now
                continue;
            }

            fprintf(stderr, "  mesh[%d]: type=%u raw=%u ", m, meshType, mt);

            // START guard: read 3 values (32, 16, 8)
            uint32_t g32 = r32(); p32++;
            // We'd need to read p16, p8 too... but let's just note the type

            int32_t numFrames = r32();
            int32_t numMatFrames = r32();
            int32_t parentMesh = r32();
            bool shareData = (parentMesh >= 0);

            // bounds: 3 Point3Fs
            for (int i = 0; i < 3; i++) { rf32(); rf32(); rf32(); }

            int32_t numVerts = r32();
            fprintf(stderr, "frames=%d matFrames=%d parent=%d shareData=%d verts=%d", 
                    numFrames, numMatFrames, parentMesh, shareData, numVerts);

            if (meshType == 1) {
                fprintf(stderr, " ** SKIN ** ");
                if (!shareData) {
                    // Read initialVerts
                    p32 += numVerts * 3; // already counted as meshVerts
                    // Read initialNorms
                    p32 += numVerts * 3;
                    p8 += numVerts; // encoded normals
                    p8 = (p8 + 3) & ~(size_t)3;

                    int32_t sz = r32();
                    fprintf(stderr, " initXform=%d ", sz);
                    p32 += sz * 16;
                    sz = r32(); // vertexIndex
                    p16 += sz * 4; // boneIndex
                    p32 += sz * 4; // Hmm, this is wrong, need to be more careful
                }
            }

            // Skip the rest for now
            fprintf(stderr, "\n");

            // Skip to approximate position after this mesh
            // This is complex, so let's just run the actual loader
            break;
        }
    }

    // Use the actual loader
    DTSLoadResult r = loadDTS(data.data(), data.size(), "bioderm_medium.dts");
    fprintf(stderr, "\n=== Loader results ===\n");
    fprintf(stderr, "Loaded: %d meshes=%zu skins=%zu nodes=%zu\n", r.loaded, r.meshes.size(), r.skins.size(), r.nodes.size());

    int skinCount = 0;
    for (size_t mi = 0; mi < std::min(r.meshes.size(), r.skins.size()); mi++) {
        auto& mesh = r.meshes[mi];
        auto& skin = r.skins[mi];
        if (skin.hasSkin) skinCount++;
        if (mi < 20)
            fprintf(stderr, "Mesh[%zu]: verts=%zu nodeIdx=%d hasSkin=%d initPos=%zu initXform=%zu\n",
                    mi, mesh.vertices.size(), mesh.nodeIndex,
                    skin.hasSkin, skin.initialPositions.size(), skin.initialTransforms.size());
    }
    fprintf(stderr, "Total skinned meshes: %d\n", skinCount);

    return 0;
}
