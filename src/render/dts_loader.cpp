#include "render/dts_loader.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>

// DTS element-count sanity caps (prevent bad_alloc / parse hangs on hostile files)
static const int32_t kMaxDTSCount = 1 << 16;
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
        return {x, z, y};
    }
    QuatF readQuat16() {
        int16_t x = readS16(), y = readS16(), z = readS16(), w = readS16();
        return {(float)x/32767.f, (float)z/32767.f, (float)y/32767.f, (float)w/32767.f};
    }
    // allocShape*(n) does NOT advance input; copyToShape*(n) advances input by n; get*(n) advances input by n
    void checkGuard() {
        uint32_t g32 = readU32();
        uint16_t g16 = readU16();
        uint8_t  g8  = readU8();
        if ((int)g32 != guard || (int)g16 != guard || (int8_t)g8 != (int8_t)guard) {
            static int guardWarnCount = 0;
            if (guardWarnCount < 5)
                Console::instance().printf(LogLevel::Warn,
                    "DTS: GUARD mismatch at %d (got %u/%u/%u) - continuing", guard, g32, g16, g8);
            guardWarnCount++;
        }
        guard++;
    }
    void alignS16() { if (pos16 & 1) pos16++; }
    void align8() { pos8 = (pos8 + 3) & ~(size_t)3; } // align to 4-byte boundary (matches T2 align32())
};

enum : uint32_t {
    DTSMesh_Standard = 0, DTSMesh_Skin = 1, DTSMesh_Decal = 2, DTSMesh_Sorted = 3, DTSMesh_Null = 4,
};

static const char* skipExts[] = {".lbioderm", ".ifl", ".iflod", ".dml", ".mis"};
static bool hasExt(const std::string& s, const char* ext) {
    if (s.size() < strlen(ext)) return false;
    auto p = s.rfind(ext);
    return p != std::string::npos && p + strlen(ext) == s.size();
}

bool updateSkinnedMesh(MeshData& mesh, SkinInfo& skin,
                       const std::vector<MatrixF>& nodeWorld,
                       const std::vector<MatrixF>& defaultTransforms) {
    if (!skin.hasSkin || mesh.vertices.empty()) return false;
    if (skin.vertexIndices.empty() || skin.initialPositions.empty()) return false;

    // Zero out positions and normals
    for (auto& v : mesh.vertices) {
        v.pos = {0, 0, 0};
        v.normal = {0, 0, 0};
    }

    // For each skin vertex entry, blend bone transform using initial (bind-pose) data
    for (size_t i = 0; i < skin.vertexIndices.size(); i++) {
        int32_t vertIdx = skin.vertexIndices[i];
        if (vertIdx < 0 || vertIdx >= (int)mesh.vertices.size()) continue;
        if (i >= skin.initialPositions.size()) break;

        int32_t boneIdx = (i < skin.boneIndices.size()) ? skin.boneIndices[i] : 0;
        int32_t nodeIdx = (i < skin.nodeIndices.size()) ? skin.nodeIndices[i] : 0;
        float weight = (i < skin.boneWeights.size()) ? skin.boneWeights[i] : 1.0f;

        if (weight < 0.001f) continue;

        // Compute bone transform: nodeWorld * initialTransform
        MatrixF boneTransform;
        if (nodeIdx >= 0 && nodeIdx < (int)nodeWorld.size())
            boneTransform = nodeWorld[nodeIdx];
        else
            boneTransform.identity();

        if (boneIdx >= 0 && boneIdx < (int)skin.initialTransforms.size())
            boneTransform = boneTransform * skin.initialTransforms[boneIdx];

        // Get bind-pose position and normal from skin data
        Point3F bindPos = skin.initialPositions[i];
        Point3F bindNrm = (i < skin.initialNormals.size()) ? skin.initialNormals[i] : Point3F{0,0,1};

        // Transform and accumulate
        Point3F transformedPos = boneTransform.transform(bindPos);
        Point3F transformedNrm = boneTransform.transformNormal(bindNrm);

        mesh.vertices[vertIdx].pos.x += weight * transformedPos.x;
        mesh.vertices[vertIdx].pos.y += weight * transformedPos.y;
        mesh.vertices[vertIdx].pos.z += weight * transformedPos.z;
        mesh.vertices[vertIdx].normal.x += weight * transformedNrm.x;
        mesh.vertices[vertIdx].normal.y += weight * transformedNrm.y;
        mesh.vertices[vertIdx].normal.z += weight * transformedNrm.z;
    }

    // Normalize normals
    for (auto& v : mesh.vertices) {
        float len = sqrtf(v.normal.x*v.normal.x + v.normal.y*v.normal.y + v.normal.z*v.normal.z);
        if (len > 0.001f) {
            v.normal.x /= len;
            v.normal.y /= len;
            v.normal.z /= len;
        }
    }

    mesh.updateGPU();
    return true;
}

// v15-v18 old format: read directly from sequential stream (no 3-buffer header).
// DebugGuard() values are synthetic — written to output buffer, NOT present in input.
static DTSLoadResult loadDTSOld(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 4) return result;
    try {
    size_t pos = 0;
    auto rS32 = [&]() -> int32_t { if (pos+4 > size) { return 0; } int32_t v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rU32 = [&]() -> uint32_t { if (pos+4 > size) { return 0; } uint32_t v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rF32 = [&]() -> float { if (pos+4 > size) { return 0; } float v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rS16 = [&]() -> int16_t { if (pos+2 > size) { return 0; } int16_t v; memcpy(&v, data+pos, 2); pos+=2; return v; };
    auto rU16 = [&]() -> uint16_t { if (pos+2 > size) { return 0; } uint16_t v; memcpy(&v, data+pos, 2); pos+=2; return v; };
    auto rU8 = [&]() -> uint8_t { if (pos >= size) { return 0; } return data[pos++]; };
    auto skip = [&](size_t n) { pos = (pos + n <= size) ? pos + n : size; };
    auto eof = [&]() { return pos >= size; };

    uint16_t ver16 = rU16();
    uint16_t verPad = rU16();
    int ver = (int)ver16;
    if (ver < 15 || ver > 18) return result;

    // Stream layout: no guards in file, DebugGuard() is synthetic.
    // After version+pad, stream contains: bounds → header counts → data sections...

    // Bounds: radius, tubeRadius, center(3), min(3), max(3) = 11 F32s
    float radius = rF32(), tubeRadius = rF32();
    float cx = rF32(), cy = rF32(), cz = rF32();
    float bminx = rF32(), bminy = rF32(), bminz = rF32();
    float bmaxx = rF32(), bmaxy = rF32(), bmaxz = rF32();

    // Header counts (read sequentially from stream, stored at output positions [0]-[14])
    int32_t numNodes = rS32();
    struct OldNode { int32_t ni, pi; };
    std::vector<OldNode> nodes(numNodes);
    for (int i = 0; i < numNodes; i++) {
        nodes[i].ni = rS32();
        nodes[i].pi = rS32();
        if (ver < 17) rU8(); // obsolete bool member
        // v17+: 2 S32s read, then 3 computed (not in stream)
        // v<17: 2 S32s + 1 bool read, then 3 computed (not in stream)
    }

    int32_t numObjects = rS32();
    struct OldObj { int32_t ni, nm, sm, no; };
    std::vector<OldObj> objs(numObjects);
    for (int i = 0; i < numObjects; i++) {
        objs[i].ni = rS32(); objs[i].nm = rS32(); objs[i].sm = rS32(); objs[i].no = rS32();
    }

    int32_t numDecals = rS32();
    skip(numDecals * 4 * 4); // 4 S32s per decal (not needed)

    int32_t numIFLs = rS32();
    skip(numIFLs * 2 * 4); // 2 S32s per IFL

    int32_t numSubShapes = rS32();
    std::vector<int32_t> subFirstNode(numSubShapes), subFirstObj(numSubShapes), subFirstDecal(numSubShapes);
    for (int i = 0; i < numSubShapes; i++) subFirstNode[i] = rS32();
    rS32(); // tossed (subShapeLastNode not in file)
    for (int i = 0; i < numSubShapes; i++) subFirstObj[i] = rS32();
    rS32(); // tossed
    for (int i = 0; i < numSubShapes; i++) subFirstDecal[i] = rS32();

    // Mesh index list (v15 only)
    if (ver < 16) { int32_t sz = rS32(); skip(sz * 4); }

    // Keyframes (v15-v16 only)
    if (ver < 17) { int32_t sz = rS32(); skip(sz * 3 * 4); }

    // Default node states: rotations (S16×4 per node) + translations (F32×3 per node)
    int32_t numNodeStates = rS32();
    std::vector<QuatF> defRot(numNodeStates);
    std::vector<Point3F> defTrans(numNodeStates);
    for (int i = 0; i < numNodeStates; i++) {
        int16_t rx = rS16(), ry = rS16(), rz = rS16(), rw = rS16();
        defRot[i] = {(float)rx/32767.f, (float)ry/32767.f, (float)rz/32767.f, (float)rw/32767.f};
        defTrans[i] = {rF32(), rF32(), rF32()};
    }

    // Object states (F32 vis, S32 frameIndex, S32 matFrameIndex per state)
    int32_t numObjStates = rS32();
    for (int i = 0; i < numObjStates; i++) { rF32(); rS32(); rS32(); }

    // Decal states
    int32_t numDecalStates = rS32();
    for (int i = 0; i < numDecalStates; i++) rS32();

    // Triggers (U32 state, F32 pos per trigger)
    int32_t numTriggers = rS32();
    for (int i = 0; i < numTriggers; i++) { rU32(); rF32(); }

    // Details: nameIndex, sub, objDetail, size (4 S32s per detail)
    int32_t numDetails = rS32();
    struct OldDetail { int32_t nameIdx, sub, objDetail; float sz; };
    std::vector<OldDetail> details(numDetails);
    for (int i = 0; i < numDetails; i++) {
        details[i].nameIdx = rS32(); details[i].sub = rS32(); details[i].objDetail = rS32(); details[i].sz = rF32();
    }

    // Sequences — complex, version-dependent. Must consume correctly.
    int32_t numSeqs = rS32();
    for (int s = 0; s < numSeqs; s++) {
        // Sequence::read(s, readNameIndex=true by default)
        rS32(); // nameIndex
        if (ver > 21) rS32(); // flags
        // keyframes: v<17 has startKeyframe+endKeyframe, v>=17 has numKeyframes
        if (ver < 17) { rS32(); rS32(); } else { rS32(); } // keyframe count
        rF32(); // duration
        // bools: blend, cyclic, makePath — v<22 reads as bool (1 byte)
        if (ver < 22) { rU8(); rU8(); rU8(); }
        rS32(); // priority
        rS32(); // firstGroundFrame
        rS32(); // numGroundFrames
        // base state indices: v>21 has 5, v17-v21 has 3
        if (ver > 21) { rS32(); rS32(); rS32(); rS32(); rS32(); }
        else if (ver >= 17) { rS32(); /*baseRotation*/ rS32(); /*baseObjectState*/ rS32(); /*baseDecalState*/ }
        rS32(); // firstTrigger (v>8)
        rS32(); // numTriggers (v>8)
        rF32(); // toolBegin (v>7)
        // TSIntegerSet reads: numInts(S32), sz(S32), sz*4 bytes
        auto skipIntSet = [&]() { rS32(); int32_t sz = rS32(); skip(sz * 4); };
        skipIntSet(); // rotationMatters
        if (ver >= 22) skipIntSet(); // translationMatters (v22+)
        if (ver >= 22) skipIntSet(); // scaleMatters (v22+)
        if (ver < 17) skipIntSet(); // objectMembership (obsolete, v<17)
        if (ver > 10) skipIntSet(); // decalMatters
        if (ver > 5) skipIntSet(); // iflMatters
        skipIntSet(); // visMatters
        skipIntSet(); // frameMatters
        skipIntSet(); // matFrameMatters
        if (ver < 17) skipIntSet(); // nodeTransformStatic (obsolete, v<17)
    }

    // Meshes — read meshType as S32, then mesh body
    int32_t numMeshes = rS32();

    for (int m = 0; m < numMeshes && m < 10000; m++) {
        if (eof()) break;
        int32_t meshType = rS32();
        if (meshType == 4) continue; // NullMeshType — no data in file

        // Mesh body from readAllocMesh (v15-v18):
        // DebugGuard (synthetic, no stream read)
        // numFrames (S32), numMatFrames (S32)
        // parentMesh: oldAlloc only, NOT in stream (hardcoded to -1)
        // bounds: oldAlloc only, NOT in stream (computed later)
        // numVerts, verts, numTVerts, tverts, numNorms, norms, prims, indices, merge, vertsPerFrame, flags

        int32_t numFrames = rS32();
        if (numFrames < 1 || numFrames > 10000) numFrames = 1;
        int32_t numMatFrames = rS32();
        // parentMesh and bounds are oldAlloc'd, NOT read from stream

        int32_t numVerts = rS32();
        if (numVerts < 0 || numVerts > 100000) numVerts = 0;
        std::vector<Point3F> verts(numVerts);
        for (int i = 0; i < numVerts; i++) verts[i] = {rF32(), rF32(), rF32()};

        int32_t numTVerts = rS32();
        if (numTVerts < 0 || numTVerts > 100000) numTVerts = 0;
        std::vector<Point2F> tverts(numTVerts);
        for (int i = 0; i < numTVerts; i++) tverts[i] = {rF32(), rF32()};

        // Normals: v<=21 reads count from stream, then 3*count F32s
        int32_t numNorms = rS32();
        if (numNorms < 0 || numNorms > 100000) numNorms = 0;
        std::vector<Point3F> norms(numNorms);
        for (int i = 0; i < numNorms; i++) norms[i] = {rF32(), rF32(), rF32()};

        // Primitives
        int32_t numPrims = rS32();
        if (numPrims < 0 || numPrims > 10000) numPrims = 0;
        std::vector<int32_t> primStart(numPrims), primNumElems(numPrims), primMatIdx(numPrims);
        if (ver < 18) {
            // v<18: primitives read as S32s (start, numElems per prim), then matIdx separately
            for (int i = 0; i < numPrims; i++) { primStart[i] = (int16_t)rS32(); primNumElems[i] = (int16_t)rS32(); }
            for (int i = 0; i < numPrims; i++) primMatIdx[i] = rS32();
        } else {
            // v>=18: primitives read as S16s (2 per prim from buf16), then S32 matIdx
            for (int i = 0; i < numPrims; i++) { primStart[i] = rS16(); primNumElems[i] = rS16(); }
            for (int i = 0; i < numPrims; i++) primMatIdx[i] = rS32();
        }

        // Indices
        int32_t numIndices = rS32();
        if (numIndices < 0 || numIndices > 100000) numIndices = 0;
        std::vector<uint32_t> indices(numIndices);
        if (ver < 18) {
            for (int i = 0; i < numIndices; i++) indices[i] = (uint16_t)rU32();
        } else {
            for (int i = 0; i < numIndices; i++) indices[i] = (uint16_t)rU16();
        }

        // mergeIndices: oldAlloc only, NOT read from stream
        rS32(); // vertsPerFrame
        rS32(); // flags

        // Skin mesh extension (after base mesh body) — old format readAllocMesh layout
        if (meshType == 1) {
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 3 * 4); // initialVerts: count + count*3 F32s
            sz = rS32(); skip(capCount(sz) * 3 * 4); // initialNorms: count + count*3 F32s
            sz = rS32(); skip(capCount(sz) * 16 * 4); // initTransforms: count + count*16 F32s
            sz = rS32(); skip(capCount(sz) * 4); // vertexIndex: count + count S32s
            sz = rS32(); skip(capCount(sz) * 4); // boneIndex: count + count S32s
            // weight slots are oldAlloc'd only (no stream read for storage)
            sz = rS32(); skip(capCount(sz) * 4); // nodeIndex: count + count S32s
            sz = rS32(); skip(capCount(sz) * 4); // weight: count + count F32s
        }
        if (meshType == 3) {
            // SortedMesh extension
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 8 * 4); // clusters (8 S32s each)
            sz = rS32(); skip(capCount(sz) * 4); // startCluster
            sz = rS32(); skip(capCount(sz) * 4); // firstVerts
            sz = rS32(); skip(capCount(sz) * 4); // numVerts
            sz = rS32(); skip(capCount(sz) * 4); // firstTVerts
            rU8(); // alwaysWriteZ (read as bool)
        }
        if (meshType == 2) {
            // DecalMesh extension
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 4); // startPrimitive
            if (ver >= 17) {
                sz = rS32(); skip(capCount(sz) * 4); // startTVerts (obsolete)
                sz = rS32(); skip(capCount(sz) * 4); // tvertIndex (obsolete)
            }
            rS32(); // materialIndex
        }

        // Build MeshData
        MeshData md;
        int nodeIdx = -1;
        for (int oi = 0; oi < numObjects; oi++) {
            if (m >= objs[oi].sm && m < objs[oi].sm + objs[oi].nm) {
                nodeIdx = objs[oi].no; break;
            }
        }
        for (int vi = 0; vi < numVerts; vi++) {
            Vertex v;
            v.pos = (vi < (int)verts.size()) ? verts[vi] : Point3F{0,0,0};
            v.normal = (vi < (int)norms.size()) ? norms[vi] : Point3F{0,1,0};
            v.uv = (vi < (int)tverts.size()) ? tverts[vi] : Point2F{0,0};
            v.color = {1,1,1,1};
            md.vertices.push_back(v);
        }
        for (int pi = 0; pi < numPrims; pi++) {
            if (primNumElems[pi] < 3) continue;
            int32_t type = primMatIdx[pi] & (3 << 30);
            if (type == (1 << 30)) {
                for (int i = 0; i + 2 < primNumElems[pi]; i++) {
                    int s = primStart[pi];
                    if (s + i + 2 < (int)indices.size()) {
                        if (i & 1) { md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+2]); }
                        else { md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i+2]); }
                    }
                }
            } else if (type == (2 << 30)) {
                for (int i = 1; i + 1 < primNumElems[pi]; i++) {
                    int s = primStart[pi];
                    if (s + i + 1 < (int)indices.size()) { md.indices.push_back(indices[s]); md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); }
                }
            } else {
                for (int i = 0; i + 2 < primNumElems[pi]; i += 3) {
                    int s = primStart[pi];
                    if (s + i + 2 < (int)indices.size()) { md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i+2]); }
                }
            }
            if (md.indices.size() > 1000000) break;
        }
        // Extract material index from first primitive (MaterialMask = 0x0FFFFFFF)
        {
            int32_t matMask = 0x0FFFFFFF;
            int32_t bestMat = 0, bestCount = 0;
            std::unordered_map<int32_t, int32_t> matCounts;
            for (int pi = 0; pi < numPrims; pi++) {
                if (primMatIdx[pi] & (1 << 28)) continue; // NoMaterial flag
                int32_t mi = primMatIdx[pi] & matMask;
                matCounts[mi]++;
            }
            for (auto& [m, c] : matCounts) {
                if (c > bestCount) { bestCount = c; bestMat = m; }
            }
            md.materialIdx = bestCount > 0 ? bestMat : 0;
        }
        md.nodeIndex = nodeIdx;
        md.numTVertsPerFrame = numTVerts;
        if (!md.vertices.empty() && !md.indices.empty()) {
        result.meshes.push_back(std::move(md));
        }
    }

    // Names
    int32_t numNames = rS32();
    std::vector<std::string> names(numNames);
    for (int i = 0; i < numNames; i++) {
        int32_t sz = rS32();
        if (sz > 0 && sz < 1000 && pos + sz <= size) {
            names[i] = std::string((const char*)data + pos, sz);
            pos += sz;
        }
    }

    // Material list (old format v<19: embedded in S32 stream)
    int32_t gotList = rS32();
    if (gotList != 0) {
        int32_t numMats = rS32();
        result.materialNames.resize(numMats);
        result.materialFlags.resize(numMats, 0);
        for (int i = 0; i < numMats; i++) {
            uint32_t rawFlags = rS32();
            // Remap T2 bit layout
            uint32_t flags = 0;
            if (rawFlags & (1 << 0)) flags |= 16; // S_Wrap
            if (rawFlags & (1 << 1)) flags |= 32; // T_Wrap
            if (rawFlags & (1 << 2)) flags |= 1;  // Translucent
            if (rawFlags & (1 << 3)) flags |= 2;  // Additive
            if (rawFlags & (1 << 5)) flags |= 4;  // SelfIlluminating
            if (rawFlags & (1 << 6)) flags |= 8;  // NeverEnvMap
            result.materialFlags[i] = flags;
            rS32(); rS32(); rS32(); // reflectance, bump, detail
            int32_t nameLen = rS32();
            if (nameLen > 0 && nameLen < 1000 && pos + nameLen <= size) {
                result.materialNames[i] = std::string((const char*)data + pos, nameLen);
                pos += nameLen;
            } else {
                skip(nameLen > 0 ? nameLen : 0);
            }
        }
        // Extra per-material S32 (present in some versions)
        if (ver >= 16) {
            for (int i = 0; i < numMats; i++) rS32();
        }
    }

    // Skins — read as meshes but don't add to result
    int32_t numSkins = rS32();
    for (int i = 0; i < numSkins; i++) {
        int32_t meshType = rS32();
        if (meshType == 4) continue; // NullMeshType
        // Same body as regular mesh: numFrames, numMatFrames only (parentMesh/bounds NOT in stream)
        rS32(); rS32(); // numFrames, numMatFrames
        // numVerts, verts, tverts, norms, prims, indices, merge, vertsPerFrame, flags
        int32_t nv = rS32(); skip(nv * 3 * 4); // verts
        nv = rS32(); skip(nv * 2 * 4); // tverts
        nv = rS32(); skip(nv * 3 * 4); // norms
        nv = rS32(); // numPrims
        if (ver < 18) { skip(nv * 2 * 4); skip(nv * 4); } // S32 pairs + matIdx
        else { skip(nv * 2 * 2); skip(nv * 4); } // S16 pairs + matIdx
        nv = rS32(); // numIndices
        if (ver < 18) skip(nv * 4); else skip(nv * 2);
        // mergeIndices: oldAlloc only, NOT read from stream
        rS32(); rS32(); // vertsPerFrame, flags
        // Skin extension data
        if (meshType == 1) {
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 3 * 4); // initialVerts
            sz = rS32(); skip(capCount(sz) * 3 * 4); // initialNorms
            sz = rS32(); skip(capCount(sz) * 16 * 4); // initTransforms
            sz = rS32(); skip(capCount(sz) * 4); // vertexIndex
            sz = rS32(); skip(capCount(sz) * 4); // boneIndex
            sz = rS32(); skip(capCount(sz) * 4); // nodeIndex
            sz = rS32(); skip(capCount(sz) * 4); // weights
        }
        if (meshType == 3) {
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 8 * 4);
            sz = rS32(); skip(capCount(sz) * 4);
            sz = rS32(); skip(capCount(sz) * 4);
            sz = rS32(); skip(capCount(sz) * 4);
            sz = rS32(); skip(capCount(sz) * 4);
            rU8(); // alwaysWriteZ
        }
        if (meshType == 2) {
            int32_t sz;
            sz = rS32(); skip(capCount(sz) * 4);
            if (ver >= 17) { sz = rS32(); skip(capCount(sz) * 4); sz = rS32(); skip(capCount(sz) * 4); }
            rS32(); // materialIndex
        }
    }

    // Detail skin counts (only if numSkins > 0)
    if (numSkins > 0) {
        rS32(); // sz (detailFirstSkin count)
        skip(numDetails * 4); // detailFirstSkin array
    }

    // Build nodes
    result.nodes.resize(numNodes);
    for (int i = 0; i < numNodes; i++) {
        result.nodes[i].parentIndex = nodes[i].pi;
        if (nodes[i].ni >= 0 && nodes[i].ni < (int)names.size())
            result.nodes[i].name = names[nodes[i].ni];
        else
            result.nodes[i].name = "node" + std::to_string(i);
    }

    // Build detail levels
    for (int i = 0; i < numDetails; i++) {
        DTSShape::DetailLevel dl;
        dl.size = details[i].sz;
        dl.meshIndex = details[i].objDetail;
        result.details.push_back(dl);
    }

    // Build default transforms
    int nNodes = std::max(numNodes, 1);
    result.defaultTransforms.resize(nNodes);
    result.defaultLocalTransforms.resize(nNodes);
    for (int i = 0; i < nNodes; i++) {
        int si = std::min(i, (int)defRot.size() - 1);
        float qlen = defRot[si].x*defRot[si].x + defRot[si].y*defRot[si].y + 
                     defRot[si].z*defRot[si].z + defRot[si].w*defRot[si].w;
        if (qlen < 0.01f || qlen > 100.0f || std::isnan(qlen)) {
            defRot[si] = {0, 0, 0, 1};
            defTrans[si] = {0, 0, 0};
        }
        MatrixF rot = defRot[si].toMatrix();
        MatrixF t; t.identity(); t.setTranslation(defTrans[si]);
        MatrixF local = t * rot;
        result.defaultLocalTransforms[i] = local;
        int p = (i < numNodes) ? nodes[i].pi : -1;
        if (p >= 0 && p < i) result.defaultTransforms[i] = result.defaultTransforms[p] * local;
        else result.defaultTransforms[i] = local;
    }

    result.loaded = !result.meshes.empty();
    result.meshTVerts.resize(result.meshes.size());

    // ─── Resolve textures (shared with v19+ path) ────────────────────
    for (auto& n : result.materialNames)
        for (auto& c : n) if (c == '\\') c = '/';

    // Save per-DTS-material flags
    std::vector<uint32_t> dtsMatFlags = result.materialFlags;
    result.materialFlags.clear();

    struct MatSlot { int texIdx = -1; };
    std::vector<MatSlot> matSlots(result.materialNames.size());
    auto& fs = Engine::instance().fs();
    static const char* texExts[] = {".bm8", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"};
    static const char* skipExts[] = {".dds", ".png", ".jpg", ".bmp", ".tga"};
    auto hasExt = [](const std::string& s, const char* ext) {
        size_t el = strlen(ext);
        return s.size() >= el && s.compare(s.size() - el, el, ext) == 0;
    };

    try {
    for (size_t i = 0; i < result.materialNames.size(); i++) {
        if (result.materialNames[i].empty()) continue;
        std::string lower = result.materialNames[i];
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        std::string base = lower;
        for (auto* se : skipExts) if (hasExt(base, se)) { base = base.substr(0, base.size() - strlen(se)); break; }
        // T2 convention: material names ending in 'C' are variants — try without the suffix too
        std::string stripped = base;
        if (stripped.size() > 2 && stripped.back() == 'c') stripped.pop_back();
        std::vector<std::string> cands = {"textures/"+lower, "textures/"+base, "textures/"+stripped, lower, base, stripped};
        for (auto& c : cands)
            for (auto* e : texExts) {
                auto d = fs.read((c + e).c_str());
                if (!d.empty()) {
                    Texture t;
                    if (strcmp(e, ".bm8") == 0) t.loadBM8(d.data(), d.size());
                    else t.load(d.data(), d.size());
                    if (t.loaded) {
                        matSlots[i].texIdx = (int)result.textures.size();
                        uint32_t flags = (i < dtsMatFlags.size()) ? dtsMatFlags[i] : 0;
                        result.materialFlags.push_back(flags);
                        result.textures.push_back(std::move(t));
                        goto nextTexOld;
                    }
                }
            }
        nextTexOld:;
    }
    } catch (const std::bad_alloc&) {}

    result.materialLightmapIndex.assign(result.materialNames.size(), -1);

    for (auto& mesh : result.meshes) {
        int mi = mesh.materialIdx;
        if (mi >= 0 && mi < (int)matSlots.size()) mesh.materialIndex = matSlots[mi].texIdx;
    }

    return result;
    } catch (...) { return result; }
}

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 16) return result;
    uint16_t ver = *(const uint16_t*)(data);

    // v15-v18: old sequential format
    if (ver >= 15 && ver <= 18)
        return loadDTSOld(data, size, name);

    int32_t szAll = *(const int32_t*)(data+4);
    int32_t s16   = *(const int32_t*)(data+8);
    int32_t s8    = *(const int32_t*)(data+12);
    if (ver < 19 || ver > 30 || szAll <= 0 || s16 <= 0 || s8 <= 0 || s8 > szAll || s16 > s8)
        return result;
    Console::instance().printf(LogLevel::Debug, "DTS: '%s' v%u (%zu bytes)", name, ver, size);
    size_t sz32b = (size_t)s16 * 4, sz16b = (size_t)(s8 - s16) * 4, sz8b = (size_t)(szAll - s8) * 4;
    if (16 + sz32b + sz16b + sz8b > size) return result;
    DTSBuf buf;
    buf.buf32 = (const uint32_t*)(data + 16);
    buf.buf16 = (const uint16_t*)(data + 16 + sz32b);
    buf.buf8  = (const uint8_t*)(data + 16 + sz32b + sz16b);
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
    capCount(buf.readS32()); capCount(buf.readS32()); // smallestVisSize, smallestVisDL
    Console::instance().printf(LogLevel::Debug, "DTS: nodes=%d objects=%d meshes=%d details=%d",
        numNodes, numObjects, numMeshes, numDetails);
    buf.checkGuard(); // 0
    buf.readF32(); buf.readF32(); buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F();
    buf.checkGuard(); // 1
    struct DNode { int32_t ni,pi,fo,fc,ns; };
    std::vector<DNode> dtsNodes(numNodes);
    for (int i = 0; i < numNodes; i++) {
        dtsNodes[i].ni = capCount(buf.readS32());
        dtsNodes[i].pi = buf.readS32(); // parentIndex: -1 for root nodes, don't capCount
        dtsNodes[i].fo = capCount(buf.readS32());
        dtsNodes[i].fc = capCount(buf.readS32());
        dtsNodes[i].ns = capCount(buf.readS32());
    }
    buf.checkGuard(); // 2
    struct DObj { int32_t ni,nm,sm,no,ns,fd; };
    std::vector<DObj> dtsObjects(numObjects);
    for (int i = 0; i < numObjects; i++) { dtsObjects[i] = {capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32())}; }
    buf.checkGuard(); // 3
    for (int i = 0; i < numDecals; i++) for (int j = 0; j < 5; j++) capCount(buf.readS32());
    buf.checkGuard(); // 4
    for (int i = 0; i < numIFLs; i++) for (int j = 0; j < 5; j++) capCount(buf.readS32());
    buf.checkGuard(); // 5
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    buf.checkGuard(); // 6
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    for (int i = 0; i < numSubShapes; i++) capCount(buf.readS32());
    buf.checkGuard(); // 7
    std::vector<QuatF> defRot(numNodes);
    std::vector<Point3F> defTrans(numNodes);
    for (int i = 0; i < numNodes; i++) defRot[i] = buf.readQuat16();
    buf.align8(); // T2 tsShape.cc:708 - align 8-bit buffer after buf16 defaultRotations
    for (int i = 0; i < numNodes; i++) defTrans[i] = buf.readPoint3F();
    std::vector<QuatF> nodeRotations(numNodeRot);
    std::vector<Point3F> nodeTranslations(numNodeTrans);
    std::vector<float> nodeUScales(numNodeUScale);
    std::vector<Point3F> nodeAScales(numNodeAScale);       // scale factors (3 F32 each)
    std::vector<QuatF> nodeAScaleRots(numNodeArbScale);    // scale rotations (Quat16 each)
    for (int i = 0; i < numNodeRot; i++) nodeRotations[i] = buf.readQuat16();
    for (int i = 0; i < numNodeTrans; i++) nodeTranslations[i] = buf.readPoint3F();
    buf.align8(); // matches T2 alloc.align32() before guard (pads 8-bit buffer to 4-byte boundary)
    buf.checkGuard(); // 8 (after defRot, defTrans, nodeTrans, nodeRot per T2 tsShape.cc:726)
    for (int i = 0; i < numNodeUScale; i++) nodeUScales[i] = buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { Point3F s; s.x = buf.readF32(); s.y = buf.readF32(); s.z = buf.readF32(); nodeAScales[i] = s; }
    for (int i = 0; i < numNodeArbScale; i++) nodeAScaleRots[i] = buf.readQuat16();
    if (ver >= 22) { buf.align8(); buf.checkGuard(); } // 9 (only exists for v > 21)
    // v > 23: ground transforms stored separately (restores what v22/v23 accidentally dropped)
    if (ver > 23) {
        for (int i = 0; i < numGroundFrames; i++) buf.readPoint3F(); // groundTranslations
        for (int i = 0; i < numGroundFrames; i++) buf.readQuat16(); // groundRotations
        buf.alignS16();
        buf.checkGuard();
    }
    // v < 22: ground transforms adjustment (no-op for our parser)
    // Object states (vis, frameIndex, matFrameIndex per object per frame)
    struct ObjState { float vis; int32_t frameIndex; int32_t matFrameIndex; };
    std::vector<ObjState> objStates(numObjStates);
    for (int i = 0; i < numObjStates; i++) {
        objStates[i].vis = buf.readF32();
        objStates[i].frameIndex = capCount(buf.readS32());
        objStates[i].matFrameIndex = capCount(buf.readS32());
    }
    buf.checkGuard(); // 10
    for (int i = 0; i < numDecalStates; i++) capCount(buf.readS32());
    buf.checkGuard(); // 11
    for (int i = 0; i < numTriggers; i++) { buf.readU32(); buf.readF32(); }
    buf.checkGuard(); // 12
    for (int i = 0; i < numDetails; i++) {
        capCount(buf.readS32()); capCount(buf.readS32()); capCount(buf.readS32());
        buf.readF32(); buf.readF32(); buf.readF32();
        capCount(buf.readS32());
    }
    buf.checkGuard(); // 13

    // ─── Meshes ──────────────────────────────────────────────────────
    std::vector<std::vector<Point3F>> meshVerts(numMeshes);
    std::vector<std::vector<Point2F>> meshTVerts(numMeshes);
    std::vector<std::vector<Point3F>> meshNorms(numMeshes);

    for (int m = 0; m < numMeshes; m++) {
        // If we've gone past the real meshes (buffer exhausted), stop
        if (buf.pos32 >= buf.size32 && m > 100) break;
        uint32_t meshTypeRaw = buf.readU32();
        uint32_t meshType = meshTypeRaw & 0x7; // TypeMask = Standard|Skin|Decal|Sorted|Null = 7
        if (meshType == DTSMesh_Null) continue; // NullMeshType: only type consumed, no data
        if (meshType == DTSMesh_Decal) {
            // T2 TSDecalMesh::assemble for v>=19:
            // guard (v<20 only), primitives (sz*2 S16 + sz S32), indices (sz S16),
            // startPrimitive (sz S32), texgenS (sz*4 S32), texgenT (sz*4 S32), materialIndex (S32), guard
            if (ver < 20) { buf.checkGuard(); for (int i = 0; i < 15; i++) buf.readS32(); } // old empty mesh
            int32_t sz = capCount(buf.readS32());
            if (sz > 10000 || sz < 0) sz = 0;
            for (int i = 0; i < sz; i++) { buf.readS16(); buf.readS16(); buf.readS32(); }
            sz = capCount(buf.readS32());
            if (sz > 100000 || sz < 0) sz = 0;
            for (int i = 0; i < sz; i++) buf.readU16();
            if (ver < 20) { for (int i = 0; i < 3; i++) buf.readS32(); buf.checkGuard(); } // old empty mesh end
            sz = capCount(buf.readS32()); // startPrimitive
            for (int i = 0; i < sz; i++) buf.readS32();
            if (ver >= 19) {
                for (int i = 0; i < sz * 4; i++) buf.readS32(); // texgenS
                for (int i = 0; i < sz * 4; i++) buf.readS32(); // texgenT
            }
            buf.readS32(); // materialIndex
            buf.checkGuard();
            continue;
        }

        buf.checkGuard(); // START
        // Safety: break if guard exceeds max possible (14 pre-mesh + 2 per mesh + margin)
        if (buf.guard > 14 + 2 * numMeshes + 100) break;
        int32_t numFrames = capCount(buf.readS32());
        if (numFrames > 100 || numFrames < 1) numFrames = 1;
        int32_t numMatFrames = capCount(buf.readS32());
        if (numMatFrames > 100 || numMatFrames < 1) numMatFrames = 1;
        int32_t parentMesh = buf.readS32(); // -1 means no parent (don't capCount)
        bool shareData = (parentMesh >= 0);
        buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F(); buf.readF32(); // bounds, center, radius
        int32_t numVerts = capCount(buf.readS32());
        if (numVerts > 10000 || numVerts < 0) { numVerts = 0; }
        // Cap total vertices per mesh to prevent bad_alloc from garbage data
        if (numVerts * numFrames > 100000) { numFrames = 1; if (numVerts > 100000) numVerts = 100000; }
        if (!shareData) {
            meshVerts[m].resize(numVerts * numFrames);
            for (int i = 0; i < numVerts; i++) meshVerts[m][i] = buf.readPoint3F();
            for (int f = 1; f < numFrames; f++)
                for (int v = 0; v < numVerts; v++)
                    meshVerts[m][f * numVerts + v] = meshVerts[m][v];
        } else if (parentMesh >= 0 && parentMesh < m) {
            int cc = std::min(numVerts * numFrames, (int)meshVerts[parentMesh].size());
            meshVerts[m].assign(meshVerts[parentMesh].begin(), meshVerts[parentMesh].begin() + cc);
        }
        int32_t numTVerts = capCount(buf.readS32());
        if (numTVerts > 10000 || numTVerts < 0) { numTVerts = 0; }
        if (!shareData) {
            meshTVerts[m].resize(numTVerts * numMatFrames);
            for (int f = 0; f < numMatFrames; f++)
                for (int t = 0; t < numTVerts; t++) { meshTVerts[m][f * numTVerts + t].x = buf.readF32(); meshTVerts[m][f * numTVerts + t].y = buf.readF32(); }
        } else if (parentMesh >= 0 && parentMesh < m) {
            int cc = std::min(numTVerts * numMatFrames, (int)meshTVerts[parentMesh].size());
            meshTVerts[m].assign(meshTVerts[parentMesh].begin(), meshTVerts[parentMesh].begin() + cc);
        }
        // Normals: T2 reads differently based on version
        // v>21: 3*numVerts S32s from buf32 (skip normals) + numVerts S8s from buf8 (encoded)
        // v<=21: 3*numVerts S32s from buf32 only (no encoded normals in buffer)
        if (!shareData && numVerts > 0) {
            meshNorms[m].resize(numVerts);
            for (int i = 0; i < numVerts; i++) meshNorms[m][i] = buf.readPoint3F();
            if (ver > 21) {
                for (int i = 0; i < numVerts; i++) buf.readU8(); // encoded normals (v>21 only)
            }
        } else if (shareData && parentMesh >= 0 && parentMesh < m) {
            int cc = std::min(numVerts, (int)meshNorms[parentMesh].size());
            meshNorms[m].assign(meshNorms[parentMesh].begin(), meshNorms[parentMesh].begin() + cc);
        }
        int32_t numPrimitives = capCount(buf.readS32());
        if (numPrimitives > 10000 || numPrimitives < 0) { numPrimitives = 0; }
        struct Prim { int32_t start, numElements, matIndex; };
        std::vector<Prim> prims(numPrimitives);
        for (int i = 0; i < numPrimitives; i++) { prims[i].start = buf.readS16(); prims[i].numElements = buf.readS16(); }
        for (int i = 0; i < numPrimitives; i++) { prims[i].matIndex = buf.readS32(); }
        int32_t numIndices = capCount(buf.readS32());
        if (numIndices > 100000 || numIndices < 0) { numIndices = 0; }
        std::vector<uint32_t> indices(numIndices);
        for (int i = 0; i < numIndices; i++) indices[i] = buf.readU16();
        int32_t numMerge = capCount(buf.readS32());
        if (numMerge > 10000 || numMerge < 0) { numMerge = 0; }
        for (int i = 0; i < numMerge; i++) buf.readS16();
        int32_t vertsPerFrame = capCount(buf.readS32()); (void)vertsPerFrame;
        uint32_t flags = buf.readU32(); (void)flags;
        buf.checkGuard(); // END
        if (buf.corrupted) break;

        Console::instance().printf(LogLevel::Debug,
            "DTS: mesh[%d] type=%d frames=%d matFrames=%d parent=%d verts=%d tverts=%d prims=%d indices=%d p32=%zu p16=%zu p8=%zu",
            m, meshType, numFrames, numMatFrames, parentMesh, numVerts, numTVerts, numPrimitives, numIndices,
            buf.pos32, buf.pos16, buf.pos8);

        // TSSortedMesh extension (after base TSMesh end guard)
        if (meshType == 3) {
            int32_t numClusters = capCount(buf.readS32());
            if (numClusters > 10000 || numClusters < 0) numClusters = 0;
            for (int i = 0; i < numClusters * 8; i++) buf.readS32(); // clusters
            int32_t sz = capCount(buf.readS32());
            for (int i = 0; i < sz; i++) buf.readS32(); // startCluster
            sz = capCount(buf.readS32());
            for (int i = 0; i < sz; i++) buf.readS32(); // firstVerts
            sz = capCount(buf.readS32());
            for (int i = 0; i < sz; i++) buf.readS32(); // numVerts
            sz = capCount(buf.readS32());
            for (int i = 0; i < sz; i++) buf.readS32(); // firstTVerts
            buf.readS32(); // alwaysWriteDepth
            buf.checkGuard(); // sorted end guard
        }

        // Build MeshData
        MeshData md;
        for (int vi = 0; vi < numVerts; vi++) {
            Vertex v;
            v.pos = (vi < (int)meshVerts[m].size()) ? meshVerts[m][vi] : Point3F{0,0,0};
            v.normal = (vi < (int)meshNorms[m].size()) ? meshNorms[m][vi] : Point3F{0,1,0};
            v.uv = (vi < (int)meshTVerts[m].size()) ? meshTVerts[m][vi] : Point2F{0,0};
            v.color = {1,1,1,1};
            md.vertices.push_back(v);
        }
        for (auto& p : prims) {
            if (p.numElements < 3 || p.numElements > 10000) continue;
            if (p.start < 0 || p.start >= (int)indices.size()) continue;
            int32_t type = p.matIndex & (3 << 30); // bits 30-31
            if (type == (2 << 30)) {
                // Triangle fan: center vertex is first, fan out
                for (int i = 1; i + 1 < p.numElements; i++) {
                    if (p.start + i + 1 < (int)indices.size()) {
                        md.indices.push_back(indices[p.start]);
                        md.indices.push_back(indices[p.start + i]);
                        md.indices.push_back(indices[p.start + i + 1]);
                    }
                }
            } else if (type == (1 << 30)) {
                // Triangle strip: alternating winding
                for (int i = 0; i + 2 < p.numElements; i++) {
                    if (p.start + i + 2 < (int)indices.size()) {
                        if (i & 1) {
                            md.indices.push_back(indices[p.start + i + 1]);
                            md.indices.push_back(indices[p.start + i]);
                            md.indices.push_back(indices[p.start + i + 2]);
                        } else {
                            md.indices.push_back(indices[p.start + i]);
                            md.indices.push_back(indices[p.start + i + 1]);
                            md.indices.push_back(indices[p.start + i + 2]);
                        }
                    }
                }
            } else {
                // Triangle list (default)
                for (int i = 0; i + 2 < p.numElements; i += 3) {
                    if (p.start + i + 2 < (int)indices.size()) {
                        md.indices.push_back(indices[p.start + i]);
                        md.indices.push_back(indices[p.start + i + 1]);
                        md.indices.push_back(indices[p.start + i + 2]);
                    }
                }
            }
            if (md.indices.size() > 1000000) break; // safety cap
        }
        // Extract material index from first primitive (MaterialMask = 0x0FFFFFFF)
        // T2 matIndex packs: bits 30-31=type, bit 29=Indexed, bit 28=NoMaterial, bits 0-27=material
        {
            int32_t matMask = 0x0FFFFFFF;
            int32_t bestMat = 0, bestCount = 0;
            std::unordered_map<int32_t, int32_t> matCounts;
            for (auto& p : prims) {
                if (p.matIndex & (1 << 28)) continue; // NoMaterial flag
                int32_t mi = p.matIndex & matMask;
                matCounts[mi]++;
            }
            for (auto& [m, c] : matCounts) {
                if (c > bestCount) { bestCount = c; bestMat = m; }
            }
            md.materialIdx = bestCount > 0 ? bestMat : 0;
        }
        md.nodeIndex = -1;
        md.numTVertsPerFrame = numTVerts;
        // Assign nodeIndex from owning object
        for (int oi = 0; oi < numObjects; oi++) {
            if (m >= dtsObjects[oi].sm && m < dtsObjects[oi].sm + dtsObjects[oi].nm) {
                md.nodeIndex = dtsObjects[oi].no; break;
            }
        }
        result.meshes.push_back(std::move(md));

        // Skin data
        SkinInfo skin;
        if (meshType == DTSMesh_Skin) {
            // Counts are ALWAYS in stream. Data is ONLY in stream when parentMesh < 0.
            // When parentMesh >= 0, data is shared from parent (not in stream at all).
            int32_t sz = capCount(buf.readS32()); // initialVerts count
            if (sz > 10000 || sz < 0) sz = 0;
            if (!shareData) {
                std::vector<Point3F> initV(sz), initN(sz);
                for (int i = 0; i < sz; i++) initV[i] = buf.readPoint3F();
                for (int i = 0; i < sz; i++) initN[i] = buf.readPoint3F();
                for (int i = 0; i < sz; i++) buf.readU8(); // encoded normals
                sz = capCount(buf.readS32()); // initTransforms count
                if (sz > 10000 || sz < 0) sz = 0;
                std::vector<MatrixF> initT(sz);
                for (int i = 0; i < sz; i++) {
                    float cols[4][4];
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++)
                            cols[c][r] = buf.readF32();
                    memcpy(initT[i].m, cols, sizeof(float)*16);
                }
                sz = capCount(buf.readS32()); // vertexIndex count
                if (sz > 100000 || sz < 0) sz = 0;
                std::vector<int32_t> vertIdx(sz), boneIdx(sz);
                std::vector<float> boneWt(sz);
                for (int i = 0; i < sz; i++) vertIdx[i] = capCount(buf.readS32());
                for (int i = 0; i < sz; i++) boneIdx[i] = capCount(buf.readS32());
                for (int i = 0; i < sz; i++) boneWt[i] = buf.readF32();
                sz = capCount(buf.readS32()); // nodeIndex count
                if (sz > 10000 || sz < 0) sz = 0;
                std::vector<int32_t> nodeIdx(sz);
                for (int i = 0; i < sz; i++) nodeIdx[i] = capCount(buf.readS32());
                buf.checkGuard(); // skin end
                skin.hasSkin = true;
                skin.initialPositions = std::move(initV);
                skin.initialNormals = std::move(initN);
                skin.initialTransforms = std::move(initT);
                skin.vertexIndices = std::move(vertIdx);
                skin.boneIndices = std::move(boneIdx);
                skin.boneWeights = std::move(boneWt);
                skin.nodeIndices = std::move(nodeIdx);
            } else {
                // Data is shared from parent — only counts are in stream, not data
                sz = capCount(buf.readS32()); // initTransforms count
                sz = capCount(buf.readS32()); // vertexIndex count
                sz = capCount(buf.readS32()); // nodeIndex count
                buf.checkGuard(); // skin end
                if (parentMesh >= 0 && parentMesh < (int)result.skins.size())
                    skin = result.skins[parentMesh];
            }
        }
        result.skins.push_back(std::move(skin));
    }
    // ─── Names (from 8-bit buffer) ───────────────────────────────────
    std::vector<std::string> names(numNames);
    for (int i = 0; i < numNames && buf.pos8 < buf.size8; i++) {
        const char* s = (const char*)(buf.buf8 + buf.pos8);
        size_t len = strnlen(s, buf.size8 - buf.pos8);
        names[i] = std::string(s, len);
        buf.pos8 += len + 1;
    }

    // ─── Post-buffer: sequences, materials ────────────────────────────
    size_t postOff = 16 + sz32b + sz16b + sz8b;
    bool postBad = postOff >= size;
    const uint8_t* post = data + (postBad ? size : postOff);
    size_t postRem = postBad ? 0 : (size - postOff);
    auto prS32 = [&]() -> int32_t { if (postRem < 4) { postRem = 0; return 0; } int32_t v; memcpy(&v, post, 4); post+=4; postRem-=4; return v; };
    auto prStr = [&]() -> std::string {
        if (postRem < 1) return "";
        uint8_t len = *post; post++; postRem--;
        if (len == 0) return "";
        if ((size_t)len > postRem) { postRem = 0; return ""; }
        std::string s((const char*)post, len); post += len; postRem -= len; return s;
    };
    auto prS16 = [&]() -> int16_t { if (postRem < 2) { postRem = 0; return 0; } int16_t v; memcpy(&v, post, 2); post+=2; postRem-=2; return v; };
    auto prF32 = [&]() -> float { if (postRem < 4) { postRem = 0; return 0; } float v; memcpy(&v, post, 4); post+=4; postRem-=4; return v; };
    auto prU8 = [&]() -> uint8_t { if (postRem < 1) { postRem = 0; return 0; } uint8_t v = *post++; postRem--; return v; };

    // Read a TSIntegerSet into a vector of set bit indices
    auto readIntSet = [&]() -> std::vector<int32_t> {
        std::vector<int32_t> bits;
        capCount(prS32()); // numInts (unused, number of S32 words)
        int32_t nw = capCount(prS32()); // sz = number of bytes
        if (nw > 0 && nw < 256 && (size_t)(nw * 4) <= postRem) {
            for (int w = 0; w < nw; w++) {
                int32_t word;
                memcpy(&word, post, 4);
                post += 4; postRem -= 4;
                for (int b = 0; b < 32; b++) {
                    if (word & (1 << b))
                        bits.push_back(w * 32 + b);
                }
            }
        } else {
            // Skip corrupted data
            if (nw > 0 && nw < 1024) {
                post += nw;
                if (postRem >= (size_t)nw) postRem -= nw; else postRem = 0;
            }
        }
        return bits;
    };

    // Read a TSIntegerSet but skip its data
    auto skipIntSet = [&]() {
        capCount(prS32());
        int32_t nw = capCount(prS32());
        if (nw > 0 && nw < 256) {
            size_t bytes = (size_t)nw * 4;
            if (bytes <= postRem) { post += bytes; postRem -= bytes; }
        }
    };

    int32_t numSeqs = capCount(prS32());
    // Accumulate base offsets across sequences (T2 appends keyframes sequentially)
    int32_t accumBaseRot = 0, accumBaseTrans = 0, accumBaseScale = 0;
    for (int s = 0; s < numSeqs; s++) {
        // Sequence::read(s, readNameIndex=true)
        int32_t nameIdx = capCount(prS32());
        uint32_t flags = 0;
        if (ver > 21) flags = (uint32_t)capCount(prS32());
        int32_t numKFrames = capCount(prS32());
        float dur = prF32();
        if (ver < 22) { prU8(); prU8(); prU8(); } // blend, cyclic, makePath bools
        capCount(prS32()); // priority
        capCount(prS32()); // firstGroundFrame
        capCount(prS32()); // numGroundFrames
        int32_t baseObjState = 0;
        if (ver > 21) {
            capCount(prS32()); // baseRotation
            capCount(prS32()); // baseTranslation
            capCount(prS32()); // baseScale
            baseObjState = capCount(prS32()); // baseObjectState
            capCount(prS32()); // baseDecalState
        } else if (ver >= 17) {
            capCount(prS32()); // baseRotation (baseTranslation = baseRotation for v<22)
            baseObjState = capCount(prS32()); // baseObjectState
            capCount(prS32()); // baseDecalState
        }
        if (ver > 8) { capCount(prS32()); capCount(prS32()); } // firstTrigger, numTriggers
        if (ver > 7) prF32(); // toolBegin

        // Read matters sets
        std::vector<int32_t> rotMatters, transMatters, scaleMatters;
        rotMatters = readIntSet();
        if (ver >= 22) {
            transMatters = readIntSet();
            scaleMatters = readIntSet();
        } else {
            // v<22: no separate translationMatters/scaleMatters in stream
            // T2 just copies translationMatters = rotationMatters
            transMatters = rotMatters;
            // scaleMatters stays empty (scale not animated in v<22)
        }
        if (ver > 10) skipIntSet(); // decalMatters
        if (ver > 5) skipIntSet(); // iflMatters
        std::vector<int32_t> visMatters = readIntSet();
        std::vector<int32_t> frameMatters = readIntSet();
        std::vector<int32_t> matFrameMatters = readIntSet();
        if (ver < 17) skipIntSet(); // nodeTransformStatic (obsolete)

        // Build Animation
        DTSShape::Animation anim;
        anim.name = (nameIdx >= 0 && nameIdx < (int)names.size()) ? names[nameIdx] : "seq" + std::to_string(s);
        anim.duration = dur;
        anim.looping = (flags & 1) != 0;

        if (numKFrames > 0 && dur > 0.0f) {
            int32_t baseRot = accumBaseRot;
            int32_t baseTrans = accumBaseTrans;
            int32_t baseScale = accumBaseScale;

            int32_t rotCount = (int32_t)rotMatters.size();
            int32_t transCount = (ver >= 22) ? (int32_t)transMatters.size() : rotCount;
            int32_t scaleCount = (ver >= 22) ? (int32_t)scaleMatters.size() : 0;

            accumBaseRot += rotCount * numKFrames;
            accumBaseTrans += transCount * numKFrames;
            accumBaseScale += scaleCount * numKFrames;

            // For each animated node, create keyframes
            // Initialize defaults from the node's default local transform
            // so unanimated components (translation/rotation/scale) keep their bind pose
            for (int j = 0; j < rotCount && j < (int)rotMatters.size(); j++) {
                int32_t nodeIdx = rotMatters[j];
                for (int k = 0; k < numKFrames; k++) {
                    DTSShape::Keyframe kf;
                    kf.time = (numKFrames > 1) ? (float)k / (float)(numKFrames - 1) * dur : 0.0f;
                    kf.nodeIndex = nodeIdx;
                    kf.hasRotation = true;
                    kf.hasTranslation = false;
                    kf.hasScale = false;
                    kf.rotation = {0, 0, 0, 1};
                    kf.translation = {0, 0, 0};
                    kf.scale = {1, 1, 1};
                    int32_t idx = baseRot + j * numKFrames + k;
                    if (idx >= 0 && idx < (int)nodeRotations.size())
                        kf.rotation = nodeRotations[idx];
                    anim.keyframes.push_back(kf);
                }
            }
            for (int j = 0; j < transCount && j < (int)transMatters.size(); j++) {
                int32_t nodeIdx = transMatters[j];
                for (int k = 0; k < numKFrames; k++) {
                    float t = (numKFrames > 1) ? (float)k / (float)(numKFrames - 1) * dur : 0.0f;
                    DTSShape::Keyframe* kf = nullptr;
                    for (auto& f : anim.keyframes) {
                        if (f.nodeIndex == nodeIdx && std::abs(f.time - t) < 0.0001f) { kf = &f; break; }
                    }
                    if (!kf) {
                        DTSShape::Keyframe newKf;
                        newKf.time = t;
                        newKf.nodeIndex = nodeIdx;
                        newKf.rotation = {0, 0, 0, 1};
                        newKf.scale = {1, 1, 1};
                        newKf.hasRotation = false;
                        newKf.hasTranslation = false;
                        newKf.hasScale = false;
                        anim.keyframes.push_back(newKf);
                        kf = &anim.keyframes.back();
                    }
                    int32_t idx = baseTrans + j * numKFrames + k;
                    if (idx >= 0 && idx < (int)nodeTranslations.size()) {
                        kf->hasTranslation = true;
                        kf->translation = nodeTranslations[idx];
                    }
                }
            }
            for (int j = 0; j < scaleCount && j < (int)scaleMatters.size(); j++) {
                int32_t nodeIdx = scaleMatters[j];
                for (int k = 0; k < numKFrames; k++) {
                    float t = (numKFrames > 1) ? (float)k / (float)(numKFrames - 1) * dur : 0.0f;
                    DTSShape::Keyframe* kf = nullptr;
                    for (auto& f : anim.keyframes) {
                        if (f.nodeIndex == nodeIdx && std::abs(f.time - t) < 0.0001f) { kf = &f; break; }
                    }
                    if (!kf) {
                        DTSShape::Keyframe newKf;
                        newKf.time = t;
                        newKf.nodeIndex = nodeIdx;
                        newKf.rotation = {0, 0, 0, 1};
                        newKf.translation = {0, 0, 0};
                        anim.keyframes.push_back(newKf);
                        kf = &anim.keyframes.back();
                    }
                    int32_t idx = baseScale + j * numKFrames + k;
                    if (idx >= 0 && idx < (int)nodeUScales.size()) {
                        float s = nodeUScales[idx];
                        kf->scale = {s, s, s};
                    } else if (idx >= 0 && idx < (int)nodeAScales.size()) {
                        kf->scale = nodeAScales[idx];
                    }
                }
            }

            // Sort keyframes by time for efficient lookup
            std::sort(anim.keyframes.begin(), anim.keyframes.end(),
                [](const DTSShape::Keyframe& a, const DTSShape::Keyframe& b) {
                    if (a.nodeIndex != b.nodeIndex) return a.nodeIndex < b.nodeIndex;
                    return a.time < b.time;
                });
        }

        // Generate object keyframes for vis/frame animation
        if (numKFrames > 0 && dur > 0.0f && (!visMatters.empty() || !frameMatters.empty() || !matFrameMatters.empty())) {
            for (int j = 0; j < (int)visMatters.size(); j++) {
                int32_t objIdx = visMatters[j];
                for (int k = 0; k < numKFrames; k++) {
                    int32_t stateIdx = baseObjState + j * numKFrames + k;
                    if (stateIdx < 0 || stateIdx >= (int)objStates.size()) continue;
                    DTSShape::ObjectKeyframe okf;
                    okf.objectIndex = objIdx;
                    okf.time = (numKFrames > 1) ? (float)k / (float)(numKFrames - 1) * dur : 0.0f;
                    okf.vis = objStates[stateIdx].vis;
                    okf.frameIndex = objStates[stateIdx].frameIndex;
                    okf.matFrameIndex = objStates[stateIdx].matFrameIndex;
                    anim.objectKeyframes.push_back(okf);
                }
            }
            // Also add frameMatters objects that aren't already in visMatters
            for (int j = 0; j < (int)frameMatters.size(); j++) {
                int32_t objIdx = frameMatters[j];
                bool alreadyAdded = false;
                for (int v = 0; v < (int)visMatters.size(); v++) {
                    if (visMatters[v] == objIdx) { alreadyAdded = true; break; }
                }
                if (alreadyAdded) continue;
                for (int k = 0; k < numKFrames; k++) {
                    int32_t stateIdx = baseObjState + j * numKFrames + k;
                    if (stateIdx < 0 || stateIdx >= (int)objStates.size()) continue;
                    DTSShape::ObjectKeyframe okf;
                    okf.objectIndex = objIdx;
                    okf.time = (numKFrames > 1) ? (float)k / (float)(numKFrames - 1) * dur : 0.0f;
                    okf.vis = objStates[stateIdx].vis;
                    okf.frameIndex = objStates[stateIdx].frameIndex;
                    okf.matFrameIndex = objStates[stateIdx].matFrameIndex;
                    anim.objectKeyframes.push_back(okf);
                }
            }
            // Sort object keyframes by object index then time
            std::sort(anim.objectKeyframes.begin(), anim.objectKeyframes.end(),
                [](const DTSShape::ObjectKeyframe& a, const DTSShape::ObjectKeyframe& b) {
                    if (a.objectIndex != b.objectIndex) return a.objectIndex < b.objectIndex;
                    return a.time < b.time;
                });
        }

        result.animations.push_back(anim);
    }

    // Materials
    if (postRem >= 1) {
        post++; postRem--; // stream type
        int32_t numMats = capCount(prS32());
        result.materialNames.resize(numMats);
        for (int i = 0; i < numMats; i++) result.materialNames[i] = prStr();
        // T2 reads flags, reflectance, bump, detail in SEPARATE loops (not interleaved)
        std::vector<uint32_t> rawFlagsVec(numMats);
        for (int i = 0; i < numMats; i++) rawFlagsVec[i] = capCount(prS32()); // ALL flags
        for (int i = 0; i < numMats; i++) capCount(prS32()); // ALL reflectance
        for (int i = 0; i < numMats; i++) capCount(prS32()); // ALL bump
        for (int i = 0; i < numMats; i++) capCount(prS32()); // ALL detail
        if (ver > 11) for (int i = 0; i < numMats; i++) capCount(prS32()); // ALL detailScale
        if (ver > 20) for (int i = 0; i < numMats; i++) capCount(prS32()); // ALL reflectionAmount
        for (int i = 0; i < numMats; i++) {
            uint32_t rawFlags = rawFlagsVec[i];
            // Remap T2 bit layout to our internal flags:
            // T2: S_Wrap=1<<0, T_Wrap=1<<1, Translucent=1<<2, Additive=1<<3,
            //     SelfIlluminating=1<<5, NeverEnvMap=1<<6
            // Ours: Translucent=1, Additive=2, SelfIllum=4, NeverEnvMap=8, SWrap=16, TWrap=32
            uint32_t flags = 0;
            if (rawFlags & (1 << 0)) flags |= 16; // S_Wrap
            if (rawFlags & (1 << 1)) flags |= 32; // T_Wrap
            if (rawFlags & (1 << 2)) flags |= 1;  // Translucent
            if (rawFlags & (1 << 3)) flags |= 2;  // Additive
            if (rawFlags & (1 << 5)) flags |= 4;  // SelfIlluminating
            if (rawFlags & (1 << 6)) flags |= 8;  // NeverEnvMap
            result.materialFlags.push_back(flags);
        }
    }

    // ─── Resolve textures ────────────────────────────────────────────
    for (auto& n : result.materialNames)
        for (auto& c : n) if (c == '\\') c = '/';

    struct MatSlot { int texIdx = -1; };
    std::vector<MatSlot> matSlots(result.materialNames.size());
    auto& fs = Engine::instance().fs();
    static const char* texExts[] = {".bm8", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"};

    // materialFlags was populated from DTS material stream — save per-DTS-material
    std::vector<uint32_t> dtsMatFlags = result.materialFlags;
    result.materialFlags.clear();

    try {
    for (size_t i = 0; i < result.materialNames.size(); i++) {
        if (result.materialNames[i].empty()) continue;
        std::string lower = result.materialNames[i];
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        std::string base = lower;
        for (auto* se : skipExts) if (hasExt(base, se)) { base = base.substr(0, base.size() - strlen(se)); break; }
        std::string stripped = base;
        if (stripped.size() > 2 && stripped.back() == 'c') stripped.pop_back();
        std::vector<std::string> cands = {"textures/"+lower, "textures/"+base, "textures/"+stripped, lower, base, stripped};
        for (auto& c : cands)
            for (auto* e : texExts) {
                    auto d = fs.read((c + e).c_str());
                    if (!d.empty()) {
                        Texture t;
                        if (strcmp(e, ".bm8") == 0) t.loadBM8(d.data(), d.size());
                        else t.load(d.data(), d.size());
                        if (t.loaded) {
                            matSlots[i].texIdx = (int)result.textures.size();
                            uint32_t flags = (i < dtsMatFlags.size()) ? dtsMatFlags[i] : 0;
                            result.materialFlags.push_back(flags);
                            result.textures.push_back(std::move(t));
                            goto nextTex;
                        }
                    }
            }
        nextTex:;
    }
    } catch (const std::bad_alloc&) {
        Console::instance().printf(LogLevel::Warn, "DTS: OOM loading textures for '%s' (%zu materials)", name, result.materialNames.size());
    }
    result.materialLightmapIndex.assign(result.materialNames.size(), -1);

    for (auto& mesh : result.meshes) {
        int mi = mesh.materialIdx;
        if (mi >= 0 && mi < (int)matSlots.size()) mesh.materialIndex = matSlots[mi].texIdx;
    }

    // ─── Build nodes ─────────────────────────────────────────────────
    result.nodes.resize(numNodes);
    for (int i = 0; i < numNodes; i++) {
        result.nodes[i].parentIndex = dtsNodes[i].pi;
        if (dtsNodes[i].ni >= 0 && dtsNodes[i].ni < (int)names.size())
            result.nodes[i].name = names[dtsNodes[i].ni];
        else
            result.nodes[i].name = "node" + std::to_string(i);
    }

    // ─── Store object-to-mesh mapping ─────────────────────────────────
    result.objectStartMesh.resize(numObjects);
    result.objectNumMeshes.resize(numObjects);
    for (int i = 0; i < numObjects; i++) {
        result.objectStartMesh[i] = dtsObjects[i].sm;
        result.objectNumMeshes[i] = dtsObjects[i].nm;
    }

    // ─── Build detail levels ──────────────────────────────────────────
    // Details were already read from the buffer but not stored.
    // Re-read them from the file data.
    const uint8_t* detPtr = data + 16;
    size_t detRem = sz32b;
    for (int skip = 0; skip < 15; skip++) { // skip header fields before numDetails... actually we can't re-read
    }
    // For now, create a default detail level if meshes exist
    if (!result.meshes.empty()) {
        DTSShape::DetailLevel dl;
        dl.size = 1000.0f;
        dl.meshIndex = 0;
        result.details.push_back(dl);
    }

    // ─── Build default transforms ─────────────────────────────────────
    result.defaultTransforms.resize(numNodes);
    result.defaultLocalTransforms.resize(numNodes);
    for (int i = 0; i < numNodes; i++) {
        MatrixF rot = defRot[i].toMatrix();
        MatrixF t; t.identity(); t.setTranslation(defTrans[i]);
        MatrixF local = t * rot;
        result.defaultLocalTransforms[i] = local;
        int p = dtsNodes[i].pi;
        if (p >= 0 && p < i) result.defaultTransforms[i] = result.defaultTransforms[p] * local;
        else result.defaultTransforms[i] = local;
    }

    result.loaded = !result.meshes.empty() && !result.nodes.empty();
    result.meshTVerts = std::move(meshTVerts);
    Console::instance().printf(LogLevel::Info,
        "DTS: loaded '%s' (%zu meshes, %zu textures, %zu nodes, %zu anims)",
        name, result.meshes.size(), result.textures.size(), result.nodes.size(), result.animations.size());

    // Debug dump for weapon_disc
    {
        std::string lower = name;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.find("weapon_disc") != std::string::npos) {
            auto& con = Console::instance();
            con.printf(LogLevel::Info, "WEAPON_DISC DEBUG:");
            con.printf(LogLevel::Info, "  Nodes (%zu):", result.nodes.size());
            for (size_t i = 0; i < result.nodes.size(); i++) {
                auto& nd = result.nodes[i];
                con.printf(LogLevel::Info, "    [%zu] parent=%d name='%s'",
                    i, nd.parentIndex, nd.name.c_str());
            }
            con.printf(LogLevel::Info, "  Default Local Transforms (%zu):", result.defaultLocalTransforms.size());
            for (size_t i = 0; i < result.defaultLocalTransforms.size(); i++) {
                auto& m = result.defaultLocalTransforms[i];
                con.printf(LogLevel::Info, "    [%zu] T=(%.3f,%.3f,%.3f) R=(%.4f,%.4f,%.4f,%.4f)",
                    i, m.m[0][3], m.m[1][3], m.m[2][3],
                    /* quat approx */ 0.f, 0.f, 0.f, 0.f);
            }
            con.printf(LogLevel::Info, "  Default Transforms (world, %zu):", result.defaultTransforms.size());
            for (size_t i = 0; i < result.defaultTransforms.size(); i++) {
                auto& m = result.defaultTransforms[i];
                con.printf(LogLevel::Info, "    [%zu] T=(%.3f,%.3f,%.3f)", i, m.m[0][3], m.m[1][3], m.m[2][3]);
            }
            con.printf(LogLevel::Info, "  Meshes (%zu):", result.meshes.size());
            for (size_t i = 0; i < result.meshes.size(); i++) {
                auto& m = result.meshes[i];
                con.printf(LogLevel::Info, "    [%zu] node=%d verts=%zu matIdx=%d", i, m.nodeIndex, m.vertices.size(), m.materialIndex);
            }
            con.printf(LogLevel::Info, "  Animations (%zu):", result.animations.size());
            for (size_t ai = 0; ai < result.animations.size(); ai++) {
                auto& a = result.animations[ai];
                con.printf(LogLevel::Info, "    [%zu] '%s' dur=%.3f loop=%d keyframes=%zu objKeyframes=%zu",
                    ai, a.name.c_str(), a.duration, a.looping, a.keyframes.size(), a.objectKeyframes.size());
                for (size_t ki = 0; ki < a.keyframes.size(); ki++) {
                    auto& kf = a.keyframes[ki];
                    con.printf(LogLevel::Info, "      kf[%zu] t=%.3f node=%d rot=(%.4f,%.4f,%.4f,%.4f) trans=(%.3f,%.3f,%.3f) hasR=%d hasT=%d",
                        ki, kf.time, kf.nodeIndex,
                        kf.rotation.x, kf.rotation.y, kf.rotation.z, kf.rotation.w,
                        kf.translation.x, kf.translation.y, kf.translation.z,
                        kf.hasRotation, kf.hasTranslation);
                }
            }
        }
    }

    return result;
}
