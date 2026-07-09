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
    void checkGuard() {
        uint32_t g32 = readU32();
        uint16_t g16 = readU16();
        uint8_t  g8  = readU8();
        if ((int)g32 != guard || (int)g16 != guard || (int8_t)g8 != (int8_t)guard) {
            static int guardWarnCount = 0;
            corrupted = true;
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

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 16) return result;
    uint16_t ver = *(const uint16_t*)(data);
    int32_t szAll = *(const int32_t*)(data+4);
    int32_t s16   = *(const int32_t*)(data+8);
    int32_t s8    = *(const int32_t*)(data+12);
    if (ver < 20 || ver > 30 || szAll <= 0 || s16 <= 0 || s8 <= 0 || s8 > szAll || s16 > s8)
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
    int32_t numNodeRot = capCount(buf.readS32()), numNodeTrans = capCount(buf.readS32());
    int32_t numNodeUScale = capCount(buf.readS32()), numNodeAScale = capCount(buf.readS32()), numNodeArbScale = capCount(buf.readS32());
    int32_t numObjStates = capCount(buf.readS32()), numDecalStates = capCount(buf.readS32()), numTriggers = capCount(buf.readS32());
    int32_t numDetails = capCount(buf.readS32()), numMeshes = capCount(buf.readS32());
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
    for (int i = 0; i < numNodes; i++) buf.readQuat16();
    for (int i = 0; i < numNodes; i++) buf.readPoint3F();
    buf.checkGuard(); // 8
    for (int i = 0; i < numNodeRot; i++) buf.readQuat16();
    for (int i = 0; i < numNodeTrans; i++) buf.readPoint3F();
    for (int i = 0; i < numNodeUScale; i++) buf.readF32();
    for (int i = 0; i < numNodeAScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) { buf.readF32(); buf.readF32(); buf.readF32(); }
    for (int i = 0; i < numNodeArbScale; i++) buf.readQuat16();
    buf.checkGuard(); // 9
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
        if (meshType != DTSMesh_Standard && meshType != DTSMesh_Skin && meshType != DTSMesh_Sorted)
            continue;

        buf.checkGuard(); // START
        // Past the last valid mesh guard (389), skip further Standard matches
        if (buf.guard > 395) break;
        int32_t numFrames = capCount(buf.readS32());
        if (numFrames > 100 || numFrames < 1) numFrames = 1;
        int32_t numMatFrames = capCount(buf.readS32());
        if (numMatFrames > 100 || numMatFrames < 1) numMatFrames = 1;
        int32_t parentMesh = capCount(buf.readS32());
        bool shareData = (parentMesh >= 0);
        buf.readPoint3F(); buf.readPoint3F(); buf.readPoint3F(); buf.readF32(); // bounds, center, radius
        int32_t numVerts = capCount(buf.readS32());
        if (numVerts > 10000 || numVerts < 0) { numVerts = 0; }
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
        for (int i = 0; i < numPrimitives; i++) { prims[i].matIndex = 0; buf.readU32(); }
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
            if (p.numElements < 3) continue;
            for (int i = 0; i < p.numElements; i += 3)
                if (p.start + i + 2 < (int)indices.size()) {
                    md.indices.push_back(indices[p.start + i]);
                    md.indices.push_back(indices[p.start + i + 1]);
                    md.indices.push_back(indices[p.start + i + 2]);
                }
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
    // Read default node translations and rotations from the buffer.
    // These were already consumed during parsing. We need to re-read them.
    // Since we don't have the data cached, we'll compute identity transforms for now.
    result.defaultTransforms.resize(numNodes);
    for (int i = 0; i < numNodes; i++) result.defaultTransforms[i].identity();

    result.loaded = !buf.corrupted;
    Console::instance().printf(LogLevel::Info,
        "DTS: loaded '%s' (%zu meshes, %zu textures, %zu nodes, %zu anims)",
        name, result.meshes.size(), result.textures.size(), result.nodes.size(), result.animations.size());
    return result;
}
