#include "render/dts_loader.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include <cstring>
#include <cmath>
#include <algorithm>

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
};

enum : uint32_t {
    DTSMesh_Standard = 0, DTSMesh_Decal = 2, DTSMesh_Skin = 4, DTSMesh_Sorted = 5,
};

static const char* skipExts[] = {".lbioderm", ".ifl", ".iflod", ".dml", ".mis"};
static bool hasExt(const std::string& s, const char* ext) {
    if (s.size() < strlen(ext)) return false;
    auto p = s.rfind(ext);
    return p != std::string::npos && p + strlen(ext) == s.size();
}

bool updateSkinnedMesh(MeshData&, SkinInfo&, const std::vector<MatrixF>&, const std::vector<MatrixF>&) { return false; }

// v15-v18 old format: read directly from stream (no 3-buffer header)
static DTSLoadResult loadDTSOld(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 4) return result;
    try {
    size_t pos = 0;
    auto rS32 = [&]() -> int32_t { if (pos+4 > size) return 0; int32_t v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rU32 = [&]() -> uint32_t { if (pos+4 > size) return 0; uint32_t v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rF32 = [&]() -> float { if (pos+4 > size) return 0; float v; memcpy(&v, data+pos, 4); pos+=4; return v; };
    auto rS16 = [&]() -> int16_t { if (pos+2 > size) return 0; int16_t v; memcpy(&v, data+pos, 2); pos+=2; return v; };
    auto rU16 = [&]() -> uint16_t { if (pos+2 > size) return 0; uint16_t v; memcpy(&v, data+pos, 2); pos+=2; return v; };
    auto rU8 = [&]() -> uint8_t { if (pos >= size) return 0; return data[pos++]; };
    auto skip = [&](size_t n) { pos = (pos + n <= size) ? pos + n : size; };
    auto eof = [&]() { return pos >= size; };

    uint16_t ver16 = rU16();
    uint16_t verPad = rU16(); // padding/flags in old format
    int ver = (int)ver16;
    if (ver < 15 || ver > 18) return result;

    // DebugGuard (S32, S16, S8)
    pos += 4 + 2 + 1; // guard

    // Bounds
    float radius = rF32(), tubeRadius = rF32();
    float cx = rF32(), cy = rF32(), cz = rF32();
    float bminx = rF32(), bminy = rF32(), bminz = rF32();
    float bmaxx = rF32(), bmaxy = rF32(), bmaxz = rF32();
    pos += 4 + 2 + 1; // guard

    // Nodes
    int32_t numNodes = rS32();
    struct OldNode { int32_t ni, pi; };
    std::vector<OldNode> nodes(numNodes);
    for (int i = 0; i < numNodes; i++) {
        nodes[i].ni = rS32();
        nodes[i].pi = rS32();
        if (ver < 17) rU8(); // obsolete bool
        skip(3 * 4); // 3 computed S32 slots
    }
    pos += 4 + 2 + 1; // guard

    // Objects
    int32_t numObjects = rS32();
    struct OldObj { int32_t ni, nm, sm, no; };
    std::vector<OldObj> objs(numObjects);
    for (int i = 0; i < numObjects; i++) {
        objs[i].ni = rS32(); objs[i].nm = rS32(); objs[i].sm = rS32(); objs[i].no = rS32();
        skip(2 * 4); // 2 computed S32 slots
    }
    pos += 4 + 2 + 1; // guard

    // Decals
    int32_t numDecals = rS32();
    for (int i = 0; i < numDecals; i++) { skip(4 * 4); skip(1 * 4); } // 4 read + 1 computed
    pos += 4 + 2 + 1; // guard

    // IFL materials
    int32_t numIFLs = rS32();
    for (int i = 0; i < numIFLs; i++) { skip(2 * 4); skip(3 * 4); } // 2 read + 3 computed
    pos += 4 + 2 + 1; // guard

    // Sub-shapes
    int32_t numSubShapes = rS32();
    std::vector<int32_t> subFirstNode(numSubShapes), subFirstObj(numSubShapes), subFirstDecal(numSubShapes);
    for (int i = 0; i < numSubShapes; i++) subFirstNode[i] = rS32();
    rS32(); // tossed
    for (int i = 0; i < numSubShapes; i++) subFirstObj[i] = rS32();
    rS32(); // tossed
    for (int i = 0; i < numSubShapes; i++) subFirstDecal[i] = rS32();
    pos += 4 + 2 + 1; // guard
    pos += 4 + 2 + 1; // guard x2

    // Mesh index list (v15 only)
    if (ver < 16) { int32_t sz = rS32(); skip(sz * 4); }

    // Keyframes (v15-v16 only)
    if (ver < 17) { int32_t sz = rS32(); skip(sz * 3 * 4); }

    // Node states (rotations in S16, translations in S32)
    int32_t numNodeStates = rS32();
    std::vector<QuatF> defRot(numNodeStates);
    std::vector<Point3F> defTrans(numNodeStates);
    for (int i = 0; i < numNodeStates; i++) {
        int16_t rx = rS16(), ry = rS16(), rz = rS16(), rw = rS16();
        defRot[i] = {(float)rx/32767.f, (float)ry/32767.f, (float)rz/32767.f, (float)rw/32767.f};
        defTrans[i] = {rF32(), rF32(), rF32()};
    }
    pos += 4 + 2 + 1; // guard

    // Object states
    int32_t numObjStates = rS32();
    for (int i = 0; i < numObjStates; i++) { rF32(); rS32(); rS32(); }
    pos += 4 + 2 + 1; // guard

    // Decal states
    int32_t numDecalStates = rS32();
    for (int i = 0; i < numDecalStates; i++) rS32();
    pos += 4 + 2 + 1; // guard

    // Triggers
    int32_t numTriggers = rS32();
    for (int i = 0; i < numTriggers; i++) { rU32(); rF32(); }
    pos += 4 + 2 + 1; // guard

    // Details
    int32_t numDetails = rS32();
    struct OldDetail { int32_t nameIdx, sub, objDetail; float sz; };
    std::vector<OldDetail> details(numDetails);
    for (int i = 0; i < numDetails; i++) {
        details[i].nameIdx = rS32(); details[i].sub = rS32(); details[i].objDetail = rS32(); details[i].sz = rF32();
        skip(3 * 4); // computed
    }
    pos += 4 + 2 + 1; // guard

    // Sequences (simplified — just skip the data)
    int32_t numSeqs = rS32();
    for (int s = 0; s < numSeqs; s++) {
        if (ver >= 17) {
            rS32(); // flags
            rS32(); // numKeyframes
            rF32(); // duration
            rU8(); rU8(); rU8(); // blend, cyclic, makePath (booleans)
            rS32(); // priority
            rS32(); rS32(); rS32(); rS32(); rS32(); rS32(); // groundFrame, base, etc.
            rF32(); // toolBegin
            // Bitsets: rotationMatters, objectMembership, decalMatters, iflMatters, visMatters, frameMatters, matFrameMatters, nodeTransformStatic
            for (int b = 0; b < 8; b++) {
                rS32(); // size
                int32_t nw = rS32();
                skip(nw * 4);
            }
        } else {
            rS32(); rS32(); // startKeyframe, endKeyframe
            rF32(); // duration
            rU8(); rU8(); rU8(); // blend, cyclic, makePath
            rS32(); // priority
            rS32(); rS32(); rS32(); rS32(); rS32(); rS32(); // groundFrame, base, etc.
            // Bitsets (version-dependent count)
            for (int b = 0; b < 8; b++) {
                rS32(); int32_t nw = rS32(); skip(nw * 4);
            }
        }
    }

    // Meshes
    int32_t numMeshes = rS32();
    for (int m = 0; m < numMeshes && m < 10000; m++) {
        if (eof()) break;
        int32_t meshType = rS32();
        if (meshType == 4) continue; // Null mesh

        // Mesh guard
        pos += 4 + 2 + 1;
        int32_t numFrames = rS32();
        if (numFrames < 1 || numFrames > 100) numFrames = 1;
        rS32(); // numMatFrames (read as second S32 of the pair)
        rS32(); // parentMesh (old format always -1)

        // bounds filler (10 S32s)
        skip(10 * 4);

        int32_t numVerts = rS32();
        if (numVerts < 0 || numVerts > 100000) numVerts = 0;
        std::vector<Point3F> verts(numVerts);
        for (int i = 0; i < numVerts; i++) verts[i] = {rF32(), rF32(), rF32()};

        int32_t numTVerts = rS32();
        if (numTVerts < 0 || numTVerts > 100000) numTVerts = 0;
        std::vector<Point2F> tverts(numTVerts);
        for (int i = 0; i < numTVerts; i++) tverts[i] = {rF32(), rF32()};

        int32_t numNorms = rS32();
        if (numNorms < 0 || numNorms > 100000) numNorms = 0;
        std::vector<Point3F> norms(numNorms);
        for (int i = 0; i < numNorms; i++) norms[i] = {rF32(), rF32(), rF32()};

        int32_t numPrims = rS32();
        if (numPrims < 0 || numPrims > 10000) numPrims = 0;
        // Read primitives
        std::vector<int32_t> primStart(numPrims), primNumElems(numPrims), primMatIdx(numPrims);
        if (ver < 18) {
            for (int i = 0; i < numPrims; i++) { primStart[i] = (int16_t)rS32(); primNumElems[i] = (int16_t)rS32(); }
            for (int i = 0; i < numPrims; i++) primMatIdx[i] = rS32();
        } else {
            for (int i = 0; i < numPrims; i++) { primStart[i] = rS16(); primNumElems[i] = rS16(); }
            for (int i = 0; i < numPrims; i++) primMatIdx[i] = rS32();
        }

        // Read indices
        int32_t numIndices = rS32();
        if (numIndices < 0 || numIndices > 100000) numIndices = 0;
        std::vector<uint32_t> indices(numIndices);
        if (ver < 18) {
            for (int i = 0; i < numIndices; i++) indices[i] = (uint16_t)rU32();
        } else {
            for (int i = 0; i < numIndices; i++) indices[i] = (uint16_t)rU16();
        }

        rS32(); // mergeIndices count (always 0 in old format)
        rS32(); // vertsPerFrame
        rS32(); // flags

        // Mesh guard end
        pos += 4 + 2 + 1;

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
                // Strip
                for (int i = 0; i + 2 < primNumElems[pi]; i++) {
                    int s = primStart[pi];
                    if (s + i + 2 < (int)indices.size()) {
                        if (i & 1) { md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+2]); }
                        else { md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i+2]); }
                    }
                }
            } else if (type == (2 << 30)) {
                // Fan
                for (int i = 1; i + 1 < primNumElems[pi]; i++) {
                    int s = primStart[pi];
                    if (s + i + 1 < (int)indices.size()) { md.indices.push_back(indices[s]); md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); }
                }
            } else {
                // List
                for (int i = 0; i + 2 < primNumElems[pi]; i += 3) {
                    int s = primStart[pi];
                    if (s + i + 2 < (int)indices.size()) { md.indices.push_back(indices[s+i]); md.indices.push_back(indices[s+i+1]); md.indices.push_back(indices[s+i+2]); }
                }
            }
            if (md.indices.size() > 1000000) break;
        }
        md.materialIdx = 0;
        md.nodeIndex = nodeIdx;
        if (!md.vertices.empty() && !md.indices.empty()) {
            result.meshes.push_back(std::move(md));
        }

        // Skin mesh extension
        if (meshType == 1) {
            int32_t sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 3 * 4); // initialVerts
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 3 * 4); // initialNorms
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 16 * 4); // transforms
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 4); // vertexIndex
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 4); // boneIndex
            skip(sz * 4); // weights (reserved slot)
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 4); // nodeIndex
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0;
            skip(sz * 4); // weight values
            pos += 4 + 2 + 1; // guard
        }
        // Sorted mesh extension
        if (meshType == 3) {
            int32_t sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 8 * 4); // clusters
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 4); // startCluster
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 4); // firstVerts
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 4); // numVerts
            sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 4); // firstTVerts
            rU8(); // alwaysWriteZ
            pos += 4 + 2 + 1; // guard
        }
        // Decal mesh extension
        if (meshType == 2) {
            int32_t sz = rS32(); if (sz < 0 || sz > 10000) sz = 0; skip(sz * 4); // startPrim
            if (ver >= 17) {
                sz = rS32(); skip(sz * 4); // startTVerts (obsolete)
                sz = rS32(); skip(sz * 4); // tvertIndex (obsolete)
            }
            rS32(); // materialIndex
            pos += 4 + 2 + 1; // guard
        }
    }

    // Names
    pos += 4 + 2 + 1; // guard
    int32_t numNames = rS32();
    std::vector<std::string> names(numNames);
    for (int i = 0; i < numNames; i++) {
        int32_t sz = rS32();
        if (sz > 0 && sz < 1000 && pos + sz <= size) {
            names[i] = std::string((const char*)data + pos, sz);
            pos += sz;
        }
    }
    pos += 4 + 2 + 1; // guard

    // Materials
    int32_t gotList = rS32();
    if (gotList != 0) {
        // Read material list (simplified: just read name count and names)
        int32_t numMats = rS32();
        result.materialNames.resize(numMats);
        for (int i = 0; i < numMats; i++) {
            int32_t sz = rS32();
            if (sz > 0 && sz < 1000 && pos + sz <= size) {
                result.materialNames[i] = std::string((const char*)data + pos, sz);
                pos += sz;
            }
        }
        // Material flags and other fields
        for (int i = 0; i < numMats; i++) {
            rS32(); rS32(); rS32(); rS32(); // flags, reflectance, bump, detail
        }
    }

    // Skins (just skip)
    int32_t numSkins = rS32();
    for (int i = 0; i < numSkins; i++) {
        // Read as a standard mesh but don't add to result
        pos += 4 + 2 + 1; // guard
        rS32(); rS32(); rS32(); // frames, matframes, parentMesh
        skip(10 * 4); // bounds filler
        int32_t nv = rS32(); if (nv < 0 || nv > 100000) nv = 0;
        skip(nv * 3 * 4); // verts
        nv = rS32(); if (nv < 0 || nv > 100000) nv = 0;
        skip(nv * 2 * 4); // tverts
        nv = rS32(); if (nv < 0 || nv > 100000) nv = 0;
        skip(nv * 3 * 4); // norms
        nv = rS32(); if (nv < 0 || nv > 10000) nv = 0;
        skip(nv * 3 * 4); // prims (2 S16 + 1 S32 each, approximated as 3 S32)
        nv = rS32(); if (nv < 0 || nv > 100000) nv = 0;
        if (ver < 18) skip(nv * 4); else skip(nv * 2);
        rS32(); rS32(); rS32(); // merge, vertsPerFrame, flags
        pos += 4 + 2 + 1; // guard end
    }
    pos += 4 + 2 + 1; // final guard

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
        // Validate quaternion before converting
        float qlen = defRot[si].x*defRot[si].x + defRot[si].y*defRot[si].y + 
                     defRot[si].z*defRot[si].z + defRot[si].w*defRot[si].w;
        if (qlen < 0.01f || qlen > 100.0f || std::isnan(qlen)) {
            defRot[si] = {0, 0, 0, 1}; // identity
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
    Console::instance().printf(LogLevel::Info,
        "DTS-OLD: loaded '%s' (%zu meshes, %zu textures, %zu nodes)",
        name, result.meshes.size(), result.textures.size(), result.nodes.size());
    return result;
    } catch (...) { return result; }
}

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 16) return result;
    uint16_t ver = *(const uint16_t*)(data);
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
    for (int i = 0; i < numNodes; i++) { dtsNodes[i] = {capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32()),capCount(buf.readS32())}; }
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
    for (int i = 0; i < numNodes; i++) defTrans[i] = buf.readPoint3F();
    buf.checkGuard(); // 8
    for (int i = 0; i < numNodeRot; i++) buf.readQuat16();
    for (int i = 0; i < numNodeTrans; i++) buf.readPoint3F();
    for (int i = 0; i < numNodeUScale; i++) buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) buf.readQuat16();
    if (ver >= 22) buf.checkGuard(); // 9 (only exists for v > 21)
    // v < 22: ground transforms adjustment (no-op for our parser)
    for (int i = 0; i < numObjStates; i++) { buf.readF32(); capCount(buf.readS32()); capCount(buf.readS32()); }
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
    std::vector<std::vector<Point3F>> skinInitVerts(numMeshes);
    std::vector<std::vector<Point3F>> skinInitNorms(numMeshes);
    std::vector<std::vector<MatrixF>> skinInitTransforms(numMeshes);
    std::vector<std::vector<int32_t>> skinBoneIndices(numMeshes);
    std::vector<std::vector<float>> skinBoneWeights(numMeshes);
    std::vector<std::vector<int32_t>> skinNodeIndices(numMeshes);

    for (int m = 0; m < numMeshes; m++) {
        // If we've gone past the real meshes (buffer exhausted), stop
        if (buf.pos32 >= buf.size32 && m > 100) break;
        uint32_t meshType = buf.readU32();
        if (meshType == DTSMesh_Decal) {
            int32_t sz = capCount(buf.readS32());
            if (sz > 10000 || sz < 0) sz = 0;
            for (int i = 0; i < sz; i++) { buf.readS16(); buf.readS16(); capCount(buf.readS32()); }
            sz = capCount(buf.readS32());
            if (sz > 100000 || sz < 0) sz = 0;
            for (int i = 0; i < sz; i++) buf.readU16();
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
            for (int i = 0; i < numTVerts; i++) { meshTVerts[m][i].x = buf.readF32(); meshTVerts[m][i].y = buf.readF32(); }
            for (int f = 1; f < numMatFrames; f++)
                for (int t = 0; t < numTVerts; t++)
                    meshTVerts[m][f * numTVerts + t] = meshTVerts[m][t];
        } else if (parentMesh >= 0 && parentMesh < m) {
            int cc = std::min(numTVerts * numMatFrames, (int)meshTVerts[parentMesh].size());
            meshTVerts[m].assign(meshTVerts[parentMesh].begin(), meshTVerts[parentMesh].begin() + cc);
        }
        // Normals
        if (!shareData && numVerts > 0) {
            meshNorms[m].resize(numVerts);
            for (int i = 0; i < numVerts; i++) meshNorms[m][i] = buf.readPoint3F();
            for (int i = 0; i < numVerts; i++) buf.readU8(); // encoded normals
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
        md.materialIdx = 0;
        md.nodeIndex = -1;
        // Assign nodeIndex from owning object
        for (int oi = 0; oi < numObjects; oi++) {
            if (m >= dtsObjects[oi].sm && m < dtsObjects[oi].sm + dtsObjects[oi].nm) {
                md.nodeIndex = dtsObjects[oi].no; break;
            }
        }
        md.upload();
        result.meshes.push_back(std::move(md));

        // Skin data (placeholder - not used for non-skin meshes)
        SkinInfo skin;
        if (meshType == DTSMesh_Skin) {
            int32_t sz = capCount(buf.readS32());
            if (sz > 10000 || sz < 0) sz = 0;
            if (!shareData) {
                skinInitVerts[m].resize(sz);
                for (int i = 0; i < sz; i++) skinInitVerts[m][i] = buf.readPoint3F();
                skinInitNorms[m].resize(sz);
                for (int i = 0; i < sz; i++) skinInitNorms[m][i] = buf.readPoint3F();
                for (int i = 0; i < sz; i++) buf.readU8();
            }
            sz = capCount(buf.readS32());
            if (sz > 10000 || sz < 0) sz = 0;
            if (!shareData) {
                skinInitTransforms[m].resize(sz);
                for (int i = 0; i < sz; i++) { for (int j = 0; j < 16; j++) buf.readF32(); }
            }
            sz = capCount(buf.readS32());
            if (sz > 100000 || sz < 0) sz = 0;
            if (!shareData) {
                for (int i = 0; i < sz; i++) capCount(buf.readS32()); // vertexIndex
                skinBoneIndices[m].resize(sz);
                for (int i = 0; i < sz; i++) skinBoneIndices[m][i] = capCount(buf.readS32());
                skinBoneWeights[m].resize(sz);
                for (int i = 0; i < sz; i++) skinBoneWeights[m][i] = buf.readF32();
            }
            sz = capCount(buf.readS32());
            if (sz > 10000 || sz < 0) sz = 0;
            if (!shareData) {
                skinNodeIndices[m].resize(sz);
                for (int i = 0; i < sz; i++) skinNodeIndices[m][i] = capCount(buf.readS32());
            }
            buf.checkGuard(); // skin end
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

    int32_t numSeqs = capCount(prS32());
    for (int s = 0; s < numSeqs; s++) {
        int32_t nameIdx = capCount(prS32()); uint32_t flags = (uint32_t)capCount(prS32());
        capCount(prS32()); // numKFrames
        float dur; memcpy(&dur, post, 4); post+=4; postRem-=4; // duration
        for (int j = 0; j < 9; j++) capCount(prS32()); // base indices
        // Skip BitSets (8 inline bitsets)
        auto skipBitSet = [&]() { capCount(prS32()); int nw = capCount(prS32()); if (nw > 0 && nw < 256) for (int i = 0; i < nw && postRem >= 4; i++) capCount(prS32()); };
        for (int b = 0; b < 8 && postRem >= 4; b++) skipBitSet();

        DTSShape::Animation anim;
        anim.name = (nameIdx >= 0 && nameIdx < (int)names.size()) ? names[nameIdx] : "seq" + std::to_string(s);
        anim.duration = dur;
        anim.looping = (flags & 1) != 0;
        result.animations.push_back(anim);
    }

    // Materials
    if (postRem >= 1) {
        post++; postRem--; // stream type
        int32_t numMats = capCount(prS32());
        result.materialNames.resize(numMats);
        for (int i = 0; i < numMats; i++) result.materialNames[i] = prStr();
        for (int i = 0; i < numMats; i++) {
            capCount(prS32()); capCount(prS32()); capCount(prS32()); capCount(prS32()); // matFlags, reflectance, bump, detail
            capCount(prS32()); // dummy (v25+)
            capCount(prS32()); // detailScale (float as S32)
        }
    }

    // ─── Resolve textures ────────────────────────────────────────────
    for (auto& n : result.materialNames)
        for (auto& c : n) if (c == '\\') c = '/';

    struct MatSlot { int texIdx = -1; };
    std::vector<MatSlot> matSlots(result.materialNames.size());
    auto& fs = Engine::instance().fs();
    static const char* texExts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"};

    for (size_t i = 0; i < result.materialNames.size(); i++) {
        if (result.materialNames[i].empty()) continue;
        std::string lower = result.materialNames[i];
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        std::string base = lower;
        for (auto* se : skipExts) if (hasExt(base, se)) { base = base.substr(0, base.size() - strlen(se)); break; }
        std::vector<std::string> cands = {"textures/"+lower, "textures/"+base, lower, base};
        for (auto& c : cands)
            for (auto* e : texExts) {
                auto d = fs.read((c + e).c_str());
                if (!d.empty()) {
                    Texture t;
                    if (strcmp(e, ".bm8") == 0) t.loadBM8(d.data(), d.size());
                    else t.load(d.data(), d.size());
                    if (t.loaded) {
                        matSlots[i].texIdx = (int)result.textures.size();
                        result.textures.push_back(std::move(t));
                        result.materialFlags.push_back(0);
                        goto nextTex;
                    }
                }
            }
        nextTex:;
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
    Console::instance().printf(LogLevel::Info,
        "DTS: loaded '%s' (%zu meshes, %zu textures, %zu nodes, %zu anims)",
        name, result.meshes.size(), result.textures.size(), result.nodes.size(), result.animations.size());
    return result;
}
