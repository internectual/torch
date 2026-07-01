#include "render/dts_loader.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// DTS section types
enum DTSection : uint32_t {
    DTSECT_NULL           = 0,
    DTSECT_HEADER         = 1,
    DTSECT_NODE           = 2,
    DTSECT_MESH           = 3,
    DTSECT_SEQUENCE       = 4,
    DTSECT_SKIN           = 5,
    DTSECT_MATERIALLIST   = 6,
    DTSECT_DETAIL         = 7,
    DTSECT_SUBSHAPE       = 8,
    DTSECT_NUMMAT         = 9,
    DTSECT_GROUND         = 10,
    DTSECT_ANIM           = 11,
    DTSECT_IFLMATERIAL    = 12,
    DTSECT_TRIGGER        = 13,
    DTSECT_BOUNDS         = 14,
    DTSECT_SHAPE          = 15,
};

struct DTSMeshSection {
    int32_t nodeIndex;
    int32_t materialIndex;
    std::vector<Point3F> positions;
    std::vector<Point3F> normals;
    std::vector<Point2F> uvs;
    std::vector<uint32_t> indices;
};

static uint32_t readU32(const uint8_t*& ptr, size_t& rem) {
    if (rem < 4) return 0;
    uint32_t v;
    memcpy(&v, ptr, 4);
    ptr += 4; rem -= 4;
    return v;
}

static int32_t readS32(const uint8_t*& ptr, size_t& rem) {
    return (int32_t)readU32(ptr, rem);
}

static float readF32(const uint8_t*& ptr, size_t& rem) {
    if (rem < 4) return 0;
    float v;
    memcpy(&v, ptr, 4);
    ptr += 4; rem -= 4;
    return v;
}

static std::string readString(const uint8_t*& ptr, size_t& rem) {
    if (rem == 0) return "";
    uint32_t len = readU32(ptr, rem);
    if (len == 0 || len > rem) {
        if (len > rem) rem = 0;
        return "";
    }
    std::string s((const char*)ptr, len);
    ptr += len; rem -= len;
    // Strip trailing null if present
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// Convert triangle strip to indexed triangles
// Strip indices: bit31 = edge flag (strip restart), rest = vertex index
static void stripToTriangles(const std::vector<uint32_t>& strip, std::vector<uint32_t>& outIndices) {
    size_t i = 0;
    while (i < strip.size()) {
        // Skip restart markers
        if (strip[i] == 0xFFFFFFFF || (strip[i] & 0x80000000)) {
            i++;
            continue;
        }
        // Start of a strip: need at least 3 vertices
        if (i + 2 >= strip.size()) break;

        size_t start = i;
        size_t len = 0;
        while (i < strip.size() && strip[i] != 0xFFFFFFFF && !(strip[i] & 0x80000000)) {
            len++;
            i++;
        }

        if (len < 3) continue;

        // Convert strip to triangles
        for (size_t j = 0; j + 2 < len; j++) {
            uint32_t a = strip[start + j];
            uint32_t b = strip[start + j + 1];
            uint32_t c = strip[start + j + 2];
            if (a >= 0x80000000 || b >= 0x80000000 || c >= 0x80000000) continue;
            // Strip alternates winding: even triangles use (0,1,2), odd use (1,0,2)
            if (j & 1) {
                outIndices.push_back(b);
                outIndices.push_back(a);
                outIndices.push_back(c);
            } else {
                outIndices.push_back(a);
                outIndices.push_back(b);
                outIndices.push_back(c);
            }
        }
    }
}

// ─── Section Parsers ───────────────────────────────────────────────

static bool parseHeaderSection(const uint8_t*& ptr, size_t& rem) {
    uint32_t version = readU32(ptr, rem);
    Console::instance().printf(LogLevel::Debug, "DTS: version %u", version);
    // Read counts: nodeCount, meshCount, detailCount, seqCount, matCount
    // Skip over them since we'll discover sections as we iterate
    uint32_t nodeCount = readU32(ptr, rem);
    uint32_t meshCount = readU32(ptr, rem);
    uint32_t detailCount = readU32(ptr, rem);
    uint32_t seqCount = readU32(ptr, rem);
    uint32_t matCount = readU32(ptr, rem);
    Console::instance().printf(LogLevel::Debug, "DTS: %u nodes, %u meshes, %u details, %u seqs, %u mats",
        nodeCount, meshCount, detailCount, seqCount, matCount);
    return true;
}

static bool parseMeshSection(const uint8_t*& ptr, size_t& rem, std::vector<DTSMeshSection>& meshes) {
    DTSMeshSection mesh;
    mesh.nodeIndex = readS32(ptr, rem);
    mesh.materialIndex = readS32(ptr, rem);

    uint32_t vertexFormat = readU32(ptr, rem);
    uint32_t numVerts = readU32(ptr, rem);
    uint32_t numNormals = readU32(ptr, rem);
    uint32_t numUVs = readU32(ptr, rem);

    // Sanity check
    if (numVerts > 65536 || numNormals > 65536 || numUVs > 65536) {
        Console::instance().printf(LogLevel::Warn, "DTS: mesh with %u verts exceeds sanity", numVerts);
        return false;
    }

    Console::instance().printf(LogLevel::Debug, "DTS: mesh node=%d mat=%d fmt=0x%X verts=%u norms=%u uvs=%u",
        mesh.nodeIndex, mesh.materialIndex, vertexFormat, numVerts, numNormals, numUVs);

    mesh.positions.reserve(numVerts);
    mesh.normals.reserve(numNormals);
    mesh.uvs.reserve(numUVs);

    // Read positions (always present)
    for (uint32_t i = 0; i < numVerts; i++) {
        float x = readF32(ptr, rem);
        float y = readF32(ptr, rem);
        float z = readF32(ptr, rem);
        // DTS is Z-up, engine is Y-up: swap Y and Z
        mesh.positions.push_back({x, z, y});
    }

    // Read normals (may be fewer or zero)
    for (uint32_t i = 0; i < numNormals; i++) {
        float x = readF32(ptr, rem);
        float y = readF32(ptr, rem);
        float z = readF32(ptr, rem);
        mesh.normals.push_back({x, z, y});
    }

    // Read UVs (may be fewer or zero)
    for (uint32_t i = 0; i < numUVs; i++) {
        float u = readF32(ptr, rem);
        float v = readF32(ptr, rem);
        mesh.uvs.push_back({u, v});
    }

    // Read triangle strip data
    uint32_t stripCount = readU32(ptr, rem);
    Console::instance().printf(LogLevel::Debug, "DTS: mesh %zu strips", (size_t)stripCount);

    std::vector<uint32_t> allIndices;
    allIndices.reserve(stripCount * 64);

    for (uint32_t s = 0; s < stripCount; s++) {
        uint32_t count = readU32(ptr, rem);
        if (count == 0 || count > 65536) {
            Console::instance().printf(LogLevel::Warn, "DTS: strip %u has invalid count %u", s, count);
            break;
        }
        size_t startIdx = allIndices.size();
        for (uint32_t j = 0; j < count; j++) {
            allIndices.push_back(readU32(ptr, rem));
        }
    }

    if (!allIndices.empty()) {
        stripToTriangles(allIndices, mesh.indices);
    }

    meshes.push_back(std::move(mesh));
    return true;
}

// Store nodeIndex on MeshData during mesh-to-engine conversion
// This is done below after all sections are parsed

static bool parseMaterialListSection(const uint8_t*& ptr, size_t& rem, std::vector<std::string>& materials) {
    uint32_t count = readU32(ptr, rem);
    Console::instance().printf(LogLevel::Debug, "DTS: material list with %u entries", count);
    materials.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        std::string name = readString(ptr, rem);
        materials.push_back(name);
    }
    return true;
}

static bool parseDetailSection(const uint8_t*& ptr, size_t& rem, std::vector<DTSShape::DetailLevel>& details) {
    uint32_t count = readU32(ptr, rem);
    details.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        DTSShape::DetailLevel dl;
        dl.size = readF32(ptr, rem);
        dl.meshIndex = (int32_t)readU32(ptr, rem);
        details.push_back(dl);
    }
    Console::instance().printf(LogLevel::Debug, "DTS: %zu detail levels", details.size());
    return true;
}

static bool parseNodeSection(const uint8_t*& ptr, size_t& rem,
                             std::vector<DTSShape::Node>& nodes,
                             const std::vector<std::string>& materialNames,
                             size_t sectionSize) {
    uint32_t count = readU32(ptr, rem);
    if (count == 0) return true;
    nodes.reserve(count);
    size_t totalNodeBytes = sectionSize - sizeof(uint32_t);
    size_t bytesPerNode = totalNodeBytes / count;
    for (uint32_t i = 0; i < count; i++) {
        DTSShape::Node node;
        int32_t nameIndex = readS32(ptr, rem);
        node.parentIndex = readS32(ptr, rem);
        if (nameIndex >= 0 && nameIndex < (int)materialNames.size())
            node.name = materialNames[nameIndex];
        else
            node.name = "node" + std::to_string(i);
        nodes.push_back(std::move(node));
        size_t readSoFar = 8;
        if (bytesPerNode > readSoFar) {
            size_t skip = bytesPerNode - readSoFar;
            if (skip > rem) skip = rem;
            ptr += skip;
            rem -= skip;
        }
    }
    return true;
}

static bool parseSequenceSection(const uint8_t*& ptr, size_t& rem, std::vector<DTSShape::Animation>& anims) {
    uint32_t nameLen = readU32(ptr, rem);
    std::string name((const char*)ptr, nameLen);
    ptr += nameLen; rem -= nameLen;
    float duration = readF32(ptr, rem);

    DTSShape::Animation anim;
    anim.name = name;
    anim.duration = duration;

    uint32_t flags = readU32(ptr, rem);
    anim.looping = (flags & 1) != 0;

    uint32_t numKeyframes = readU32(ptr, rem);
    anim.keyframes.reserve(numKeyframes);

    for (uint32_t i = 0; i < numKeyframes; i++) {
        DTSShape::Keyframe kf;
        kf.time = readF32(ptr, rem);
        kf.nodeIndex = readS32(ptr, rem);
        float tx = readF32(ptr, rem);
        float ty = readF32(ptr, rem);
        float tz = readF32(ptr, rem);
        kf.translation = {tx, tz, ty};
        float qx = readF32(ptr, rem);
        float qy = readF32(ptr, rem);
        float qz = readF32(ptr, rem);
        float qw = readF32(ptr, rem);
        kf.rotation = {qx, qz, qy, qw};
        float sx = readF32(ptr, rem);
        float sy = readF32(ptr, rem);
        float sz = readF32(ptr, rem);
        kf.scale = {sx, sz, sy};
        anim.keyframes.push_back(kf);
    }

    anims.push_back(std::move(anim));
    return true;
}

// ─── Texture Resolution ───────────────────────────────────────────

// Non-image extensions commonly used in DTS material names
static const char* skipExts[] = {".lbioderm", ".ifl", ".iflod", ".dml", ".mis"};

static bool hasExt(const std::string& s, const char* ext) {
    if (s.size() < strlen(ext)) return false;
    auto pos = s.rfind(ext);
    return pos != std::string::npos && pos + strlen(ext) == s.size();
}

static Texture resolveTexture(const std::string& matName) {
    Texture tex;
    if (matName.empty()) return tex;

    auto& fs = Engine::instance().fs();

    // Build candidate paths to try
    std::vector<std::string> candidates;

    std::string lower = matName;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

    std::string base = lower;
    // Strip non-image extension
    for (auto* se : skipExts) {
        if (hasExt(base, se)) {
            base = base.substr(0, base.size() - strlen(se));
            break;
        }
    }

    candidates.push_back("textures/" + lower);
    candidates.push_back("textures/" + base);
    // Also try the material name itself (some are just filenames without folder)
    candidates.push_back(lower);
    candidates.push_back(base);

    static const char* exts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"};

    for (auto& cand : candidates) {
        for (auto* ext : exts) {
            auto data = fs.read((cand + ext).c_str());
            if (!data.empty()) {
                if (strcmp(ext, ".bm8") == 0)
                    tex.loadBM8(data.data(), data.size());
                else
                    tex.load(data.data(), data.size());
                if (tex.loaded) return tex;
            }
        }
    }

    return tex;
}

// ─── Main DTS Loader ──────────────────────────────────────────────

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name) {
    DTSLoadResult result;
    if (!data || size < 12) return result;

    // DTS header: [size:u32][version:u32]
    // GLB magic is 0x46546C67 - if we see that, it's not DTS
    uint32_t dtsSize = *(const uint32_t*)(data);
    if (dtsSize == 0x46546C67) return result; // GLB magic, not DTS

    // Version at offset 4
    uint32_t version = *(const uint32_t*)(data + 4);

    // Check for reasonable DTS version range (T2 uses v24-26)
    if (version < 20 || version > 40) {
        return result;
    }

    Console::instance().printf(LogLevel::Debug, "DTS: loading '%s' v%u (%zu bytes)", name, version, size);

    const uint8_t* ptr = data + 8; // Skip size + version
    size_t rem = size - 8;

    std::vector<DTSMeshSection> meshes;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    bool hasHeader = false;

    // Parse sections
    while (rem >= 8) {
        uint32_t sectionType = readU32(ptr, rem);
        uint32_t sectionSize = readU32(ptr, rem);

        if (sectionType == DTSECT_NULL && sectionSize == 0) break;

        if (sectionSize > rem) {
            Console::instance().printf(LogLevel::Warn, "DTS: section type %u size %u exceeds remaining %zu",
                sectionType, sectionSize, rem);
            break;
        }

        const uint8_t* sectPtr = ptr;
        size_t sectRem = sectionSize;
        ptr += sectionSize;
        rem -= sectionSize;

        switch (sectionType) {
            case DTSECT_HEADER:
                hasHeader = parseHeaderSection(sectPtr, sectRem);
                break;
            case DTSECT_MESH:
                parseMeshSection(sectPtr, sectRem, meshes);
                break;
            case DTSECT_MATERIALLIST:
                parseMaterialListSection(sectPtr, sectRem, materialNames);
                break;
            case DTSECT_DETAIL:
                parseDetailSection(sectPtr, sectRem, details);
                result.details = details;
                break;
            case DTSECT_SEQUENCE:
            case DTSECT_ANIM:
                parseSequenceSection(sectPtr, sectRem, result.animations);
                break;
            case DTSECT_NODE:
                parseNodeSection(sectPtr, sectRem, result.nodes, materialNames, sectionSize);
                break;
            case DTSECT_SKIN:
            case DTSECT_SUBSHAPE:
            case DTSECT_NUMMAT:
            case DTSECT_GROUND:
            case DTSECT_IFLMATERIAL:
            case DTSECT_TRIGGER:
            case DTSECT_BOUNDS:
            case DTSECT_SHAPE:
                // Skip these sections for now
                break;
            case DTSECT_NULL:
            default:
                break;
        }
    }

    if (meshes.empty()) {
        Console::instance().printf(LogLevel::Debug, "DTS: no meshes found in '%s'", name);
        return result;
    }

    // ─── Convert DTS meshes to engine MeshData ──────────────────────

    // Resolve textures from material names
    struct MatSlot { int texIdx = -1; uint32_t flags = 0; };
    std::vector<MatSlot> matSlots(materialNames.size());

    for (size_t i = 0; i < materialNames.size(); i++) {
        Texture tex = resolveTexture(materialNames[i]);
        if (tex.loaded) {
            matSlots[i].texIdx = (int)result.textures.size();
            result.textures.push_back(std::move(tex));
            result.materialFlags.push_back(0);
        }
    }

    result.materialLightmapIndex.assign(materialNames.size(), -1);

    // Build meshes
    result.meshes.reserve(meshes.size());
    for (auto& sm : meshes) {
        if (sm.indices.empty() || sm.positions.empty()) {
            Console::instance().printf(LogLevel::Debug, "DTS: skipping empty mesh (node=%d)", sm.nodeIndex);
            continue;
        }

        MeshData meshData;
        meshData.materialIdx = sm.materialIndex;
        meshData.nodeIndex = sm.nodeIndex;

        // Map material index
        if (sm.materialIndex >= 0 && sm.materialIndex < (int)matSlots.size()) {
            meshData.materialIndex = matSlots[sm.materialIndex].texIdx;
        } else {
            meshData.materialIndex = -1;
        }

        // Build vertices
        size_t vertCount = sm.positions.size();
        meshData.vertices.reserve(vertCount);
        for (size_t vi = 0; vi < vertCount; vi++) {
            Vertex v;
            v.pos = (vi < sm.positions.size()) ? sm.positions[vi] : Point3F{0,0,0};
            if (vi < sm.normals.size()) {
                v.normal = sm.normals[vi];
            } else {
                v.normal = {0, 1, 0};
            }
            if (vi < sm.uvs.size()) {
                v.uv = sm.uvs[vi];
            } else {
                v.uv = {0, 0};
            }
            v.color = {1, 1, 1, 1};
            meshData.vertices.push_back(v);
        }

        meshData.indices = std::move(sm.indices);

        meshData.upload();
        result.meshes.push_back(std::move(meshData));
    }

    if (result.meshes.empty()) {
        Console::instance().printf(LogLevel::Warn, "DTS: no valid meshes in '%s'", name);
        return result;
    }

    // Build detail levels if none were in the file
    if (result.details.empty()) {
        DTSShape::DetailLevel dl;
        dl.size = 1000.0f;
        dl.meshIndex = 0;
        result.details.push_back(dl);
    }

    result.materialNames = materialNames;
    Console::instance().printf(LogLevel::Info, "DTS: loaded '%s' (%zu meshes, %zu textures, %zu mats)", name, result.meshes.size(), result.textures.size(), materialNames.size());
    result.loaded = true;
    return result;
}
