#include "render/dif_loader.h"
#include "core/console.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>

// ─── Read Helpers ────────────────────────────────────────────────

static uint8_t readU8(const uint8_t*& ptr, size_t& rem) {
    if (rem < 1) return 0;
    uint8_t v = *ptr;
    ptr += 1; rem -= 1;
    return v;
}

static uint16_t readU16(const uint8_t*& ptr, size_t& rem) {
    if (rem < 2) return 0;
    uint16_t v;
    memcpy(&v, ptr, 2);
    ptr += 2; rem -= 2;
    return v;
}

static uint32_t readU32(const uint8_t*& ptr, size_t& rem) {
    if (rem < 4) return 0;
    uint32_t v;
    memcpy(&v, ptr, 4);
    ptr += 4; rem -= 4;
    return v;
}

static float readF32(const uint8_t*& ptr, size_t& rem) {
    if (rem < 4) return 0;
    float v;
    memcpy(&v, ptr, 4);
    ptr += 4; rem -= 4;
    return v;
}

// ─── Helper: triangle fan → indexed triangles ──────────────────

struct TriFan {
    uint32_t windingStart;
    uint32_t windingCount;
};

static void fanToTriangles(const std::vector<uint32_t>& windings,
                           const TriFan& fan,
                           const uint8_t* fanMask,
                           std::vector<uint32_t>& outIndices,
                           uint32_t baseIndex)
{
    if (fan.windingCount < 3) return;

    uint32_t center = windings[fan.windingStart];
    for (uint32_t j = 2; j < fan.windingCount; j++) {
        // bit (j-2) of fanMask controls whether center switches to vertex (j-1)
        if (fanMask && (fanMask[(j - 2) >> 3] & (1 << ((j - 2) & 7)))) {
            center = windings[fan.windingStart + j - 1];
        }
        outIndices.push_back(baseIndex + center);
        outIndices.push_back(baseIndex + windings[fan.windingStart + j - 1]);
        outIndices.push_back(baseIndex + windings[fan.windingStart + j]);
    }
}

// ─── Texture resolution ────────────────────────────────────────

static Texture resolveDIFTexture(const std::string& matName) {
    Texture tex;
    if (matName.empty()) return tex;
    auto& fs = Engine::instance().fs();

    std::string lower = matName;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

    std::vector<std::string> candidates;
    candidates.push_back("textures/" + lower);
    candidates.push_back(lower);

    std::string base = lower;
    for (auto* se : {".lbioderm", ".ifl", ".iflod", ".dml", ".mis", ".png", ".bm8", ".jpg", ".dds"}) {
        size_t seLen = strlen(se);
        if (base.size() > seLen && base.rfind(se) == base.size() - seLen) {
            base = base.substr(0, base.size() - seLen);
            break;
        }
    }
    if (base != lower) {
        candidates.push_back("textures/" + base);
        candidates.push_back(base);
    }

    static const char* exts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".dds"};
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

// ─── Interior Surface ────────────────────────────────────────────

struct DIFSurface {
    uint32_t windingStart;
    uint8_t windingCount;
    uint16_t planeIndex;
    uint16_t textureIndex;
    uint32_t texGenIndex;
    uint8_t surfaceFlags;
    uint32_t fanMask;
    uint16_t encodedTexGen;
    float texGenOffsetX, texGenOffsetY;
    uint16_t lightCount;
    uint32_t lightStateInfoStart;
    uint8_t mapOffsetX, mapOffsetY;
    uint8_t mapSizeX, mapSizeY;
};

// ─── Interior parsing state ──────────────────────────────────────

struct DIFInterior {
    uint32_t detailLevel;
    float minPixels;
    float bbox[6];
    float sphere[4];
    bool hasAlarmState;

    // Planes
    std::vector<float> uniqueNormals;
    std::vector<uint16_t> planeNormalIndices;
    std::vector<float> planeD;

    // Points (Point3F only, no fog coord padding)
    std::vector<float> points;

    // Point visibility
    std::vector<uint8_t> pointVisibility;

    // TexGen planes
    std::vector<float> texGenEqs;

    // BSP
    struct BSPNode {
        uint16_t planeIndex;
        uint16_t frontIndex;
        uint16_t backIndex;
    };
    std::vector<BSPNode> bspNodes;

    struct BSPSolidLeaf {
        uint32_t surfaceIndex;
        uint16_t surfaceCount;
    };
    std::vector<BSPSolidLeaf> bspSolidLeaves;

    // Material list (from MaterialList::read)
    std::vector<std::string> matNames;

    // Windings
    std::vector<uint32_t> windings;
    std::vector<TriFan> windingIndices;

    // Zones
    struct Zone {
        uint16_t portalStart, portalCount;
        uint32_t surfaceStart;
        uint16_t surfaceCount;
        uint16_t flags;
    };
    std::vector<Zone> zones;
    std::vector<uint16_t> zoneSurfaces;
    std::vector<uint16_t> zonePortalList;

    // Portals
    struct Portal {
        uint16_t planeIndex;
        uint16_t triFanCount;
        uint32_t triFanStart;
        uint16_t zoneFront, zoneBack;
    };
    std::vector<Portal> portals;

    // Surfaces
    std::vector<DIFSurface> surfaces;

    // Lightmap indices (always read)
    std::vector<uint8_t> normalLMapIndices;
    std::vector<uint8_t> alarmLMapIndices;

    // Null surfaces
    struct NullSurface {
        uint32_t windingStart;
        uint16_t planeIndex;
        uint8_t surfaceFlags;
        uint8_t windingCount;
    };
    std::vector<NullSurface> nullSurfaces;

    // Lightmaps
    struct LMapEntry {
        std::vector<uint8_t> pngData;
        bool keep;
    };
    std::vector<LMapEntry> lightmapEntries;

    // Solid leaf surfaces
    std::vector<uint32_t> solidLeafSurfaces;
};

// ─── Read a single Interior (detail level) ──────────────────────

static bool readInterior(const uint8_t*& ptr, size_t& rem, DIFInterior& out) {
    uint32_t fileVersion = readU32(ptr, rem);
    if (fileVersion != 0) {
        Console::instance().printf(LogLevel::Warn, "DIF: unexpected Interior version %u (expected 0)", fileVersion);
        return false;
    }

    out.detailLevel = readU32(ptr, rem);
    out.minPixels = readF32(ptr, rem);

    for (int i = 0; i < 6; i++) out.bbox[i] = readF32(ptr, rem);
    for (int i = 0; i < 4; i++) out.sphere[i] = readF32(ptr, rem);

    out.hasAlarmState = readU8(ptr, rem) != 0;

    uint32_t numLightStateEntries = readU32(ptr, rem);
    (void)numLightStateEntries;

    // ── Planes (indexed encoding) ──
    uint32_t uniqueNormalCount = readU32(ptr, rem);
    out.uniqueNormals.reserve(uniqueNormalCount * 3);
    for (uint32_t i = 0; i < uniqueNormalCount; i++) {
        out.uniqueNormals.push_back(readF32(ptr, rem));
        out.uniqueNormals.push_back(readF32(ptr, rem));
        out.uniqueNormals.push_back(readF32(ptr, rem));
    }

    uint32_t planeCount = readU32(ptr, rem);
    out.planeNormalIndices.reserve(planeCount);
    out.planeD.reserve(planeCount);
    for (uint32_t i = 0; i < planeCount; i++) {
        out.planeNormalIndices.push_back(readU16(ptr, rem));
        out.planeD.push_back(readF32(ptr, rem));
    }

    // ── Points (Point3F only, no fog coord) ──
    uint32_t pointCount = readU32(ptr, rem);
    out.points.reserve(pointCount * 3);
    for (uint32_t i = 0; i < pointCount; i++) {
        float x = readF32(ptr, rem);
        float y = readF32(ptr, rem);
        float z = readF32(ptr, rem);
        out.points.push_back(x);
        out.points.push_back(y);
        out.points.push_back(z);
    }

    // ── Point visibility ──
    uint32_t pointVisCount = readU32(ptr, rem);
    out.pointVisibility.resize(pointVisCount);
    for (uint32_t i = 0; i < pointVisCount; i++)
        out.pointVisibility[i] = readU8(ptr, rem);

    // ── TexGen planes ──
    uint32_t texGenCount = readU32(ptr, rem);
    out.texGenEqs.reserve(texGenCount * 8);
    for (uint32_t i = 0; i < texGenCount; i++) {
        for (int j = 0; j < 8; j++) out.texGenEqs.push_back(readF32(ptr, rem));
    }

    // ── BSP nodes ──
    uint32_t nodeCount = readU32(ptr, rem);
    out.bspNodes.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; i++) {
        out.bspNodes[i].planeIndex = readU16(ptr, rem);
        out.bspNodes[i].frontIndex = readU16(ptr, rem);
        out.bspNodes[i].backIndex = readU16(ptr, rem);
    }

    // ── BSP solid leaves ──
    uint32_t leafCount = readU32(ptr, rem);
    out.bspSolidLeaves.resize(leafCount);
    for (uint32_t i = 0; i < leafCount; i++) {
        out.bspSolidLeaves[i].surfaceIndex = readU32(ptr, rem);
        out.bspSolidLeaves[i].surfaceCount = readU16(ptr, rem);
    }

    // ── Material list (MaterialList::read format) ──
    // U8 BINARY_FILE_VERSION, U32 count, then (U8 length, string) per entry
    uint8_t mlVersion = readU8(ptr, rem);
    if (mlVersion != 1 && mlVersion != 2) {
        Console::instance().printf(LogLevel::Warn, "DIF: unexpected MaterialList version %u", mlVersion);
    }
    (void)mlVersion;
    uint32_t matCount = readU32(ptr, rem);
    out.matNames.reserve(matCount);
    for (uint32_t i = 0; i < matCount; i++) {
        uint8_t strLen = readU8(ptr, rem);
        if (strLen > rem) {
            out.matNames.push_back("");
            break;
        }
        out.matNames.push_back(std::string((const char*)ptr, strLen));
        ptr += strLen; rem -= strLen;
    }

    // ── Windings ──
    uint32_t windingCount = readU32(ptr, rem);
    out.windings.reserve(windingCount);
    for (uint32_t i = 0; i < windingCount; i++) {
        out.windings.push_back(readU32(ptr, rem));
    }

    // ── Winding indices (TriFans) ──
    uint32_t fanCount = readU32(ptr, rem);
    out.windingIndices.resize(fanCount);
    for (uint32_t i = 0; i < fanCount; i++) {
        out.windingIndices[i].windingStart = readU32(ptr, rem);
        out.windingIndices[i].windingCount = readU32(ptr, rem);
    }

    // ── Zones ──
    uint32_t zoneCount = readU32(ptr, rem);
    out.zones.resize(zoneCount);
    for (uint32_t i = 0; i < zoneCount; i++) {
        out.zones[i].portalStart = readU16(ptr, rem);
        out.zones[i].portalCount = readU16(ptr, rem);
        out.zones[i].surfaceStart = readU32(ptr, rem);
        out.zones[i].surfaceCount = readU16(ptr, rem);
        out.zones[i].flags = readU16(ptr, rem);
    }

    // ── Zone surfaces ──
    uint32_t zoneSurfaceCount = readU32(ptr, rem);
    out.zoneSurfaces.resize(zoneSurfaceCount);
    for (uint32_t i = 0; i < zoneSurfaceCount; i++) {
        out.zoneSurfaces[i] = readU16(ptr, rem);
    }

    // ── Zone portal list ──
    uint32_t zonePortalCount = readU32(ptr, rem);
    out.zonePortalList.resize(zonePortalCount);
    for (uint32_t i = 0; i < zonePortalCount; i++) {
        out.zonePortalList[i] = readU16(ptr, rem);
    }

    // ── Portals ──
    uint32_t portalCount = readU32(ptr, rem);
    out.portals.resize(portalCount);
    for (uint32_t i = 0; i < portalCount; i++) {
        out.portals[i].planeIndex = readU16(ptr, rem);
        out.portals[i].triFanCount = readU16(ptr, rem);
        out.portals[i].triFanStart = readU32(ptr, rem);
        out.portals[i].zoneFront = readU16(ptr, rem);
        out.portals[i].zoneBack = readU16(ptr, rem);
    }

    // ── Surfaces ──
    uint32_t surfaceCount = readU32(ptr, rem);
    out.surfaces.resize(surfaceCount);
    for (uint32_t i = 0; i < surfaceCount; i++) {
        auto& s = out.surfaces[i];
        s.windingStart = readU32(ptr, rem);
        s.windingCount = readU8(ptr, rem);
        s.planeIndex = readU16(ptr, rem);
        s.textureIndex = readU16(ptr, rem);
        s.texGenIndex = readU32(ptr, rem);
        s.surfaceFlags = readU8(ptr, rem);
        // No padding here - fanMask follows immediately
        s.fanMask = readU32(ptr, rem);
        // Lightmap texgen: U16 encoded + F32 offsetX + F32 offsetY
        s.encodedTexGen = readU16(ptr, rem);
        s.texGenOffsetX = readF32(ptr, rem);
        s.texGenOffsetY = readF32(ptr, rem);
        s.lightCount = readU16(ptr, rem);
        s.lightStateInfoStart = readU32(ptr, rem);
        s.mapOffsetX = readU8(ptr, rem);
        s.mapOffsetY = readU8(ptr, rem);
        s.mapSizeX = readU8(ptr, rem);
        s.mapSizeY = readU8(ptr, rem);
    }

    // ── Normal lightmap indices (always read) ──
    uint32_t normalLMapCount = readU32(ptr, rem);
    out.normalLMapIndices.resize(normalLMapCount);
    for (uint32_t i = 0; i < normalLMapCount; i++) {
        out.normalLMapIndices[i] = readU8(ptr, rem);
    }

    // ── Alarm lightmap indices (always read) ──
    uint32_t alarmLMapCount = readU32(ptr, rem);
    out.alarmLMapIndices.resize(alarmLMapCount);
    for (uint32_t i = 0; i < alarmLMapCount; i++) {
        out.alarmLMapIndices[i] = readU8(ptr, rem);
    }

    // ── Null surfaces ──
    uint32_t nullSurfaceCount = readU32(ptr, rem);
    out.nullSurfaces.resize(nullSurfaceCount);
    for (uint32_t i = 0; i < nullSurfaceCount; i++) {
        auto& ns = out.nullSurfaces[i];
        ns.windingStart = readU32(ptr, rem);
        ns.planeIndex = readU16(ptr, rem);
        ns.surfaceFlags = readU8(ptr, rem);
        ns.windingCount = readU8(ptr, rem);
    }

    // ── Lightmaps ──
    // PNG data is stored raw (no size prefix), self-delimiting via IEND chunk
    // PNG chunk lengths are big-endian
    uint32_t lightmapCount = readU32(ptr, rem);
    out.lightmapEntries.resize(lightmapCount);
    static const uint8_t s_pngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (uint32_t i = 0; i < lightmapCount; i++) {
        if (rem >= 8 && memcmp(ptr, s_pngMagic, 8) == 0) {
            const uint8_t* scan = ptr + 8;
            size_t scanRem = rem - 8;
            bool foundEnd = false;
            int safety = 100000;
            while (scanRem >= 12 && --safety > 0) {
                uint32_t chunkLen = ((uint32_t)scan[0] << 24) | ((uint32_t)scan[1] << 16) |
                                    ((uint32_t)scan[2] << 8) | (uint32_t)scan[3];
                if (memcmp(scan + 4, "IEND", 4) == 0) {
                    uint32_t pngSize = (uint32_t)(scan + 12 - ptr);
                    out.lightmapEntries[i].pngData.assign(ptr, ptr + pngSize);
                    ptr += pngSize; rem -= pngSize;
                    foundEnd = true;
                    break;
                }
                uint32_t skip = chunkLen + 12;
                if (skip > scanRem) skip = (uint32_t)scanRem;
                if (skip < 12) skip = 12; // minimum forward progress
                scan += skip;
                scanRem -= skip;
            }
            if (!foundEnd) {
                out.lightmapEntries[i].keep = false;
                continue;
            }
        }
        out.lightmapEntries[i].keep = (readU8(ptr, rem) != 0);
    }

    // ── Solid leaf surfaces ──
    uint32_t slsCount = readU32(ptr, rem);
    out.solidLeafSurfaces.resize(slsCount);
    for (uint32_t i = 0; i < slsCount; i++) {
        out.solidLeafSurfaces[i] = readU32(ptr, rem);
    }

    // ── Animated lights ──
    uint32_t animLightCount = readU32(ptr, rem);
    for (uint32_t i = 0; i < animLightCount; i++) {
        readU32(ptr, rem); readU32(ptr, rem); readU16(ptr, rem); readU16(ptr, rem); readU32(ptr, rem);
    }

    // ── Light states ──
    uint32_t lightStateCount = readU32(ptr, rem);
    for (uint32_t i = 0; i < lightStateCount; i++) {
        readU8(ptr, rem); readU8(ptr, rem); readU8(ptr, rem); // RGB
        readU32(ptr, rem); // activeTime
        readU32(ptr, rem); // dataIndex
        readU16(ptr, rem); // dataCount
    }

    // ── State data ──
    uint32_t stateDataCount = readU32(ptr, rem);
    for (uint32_t i = 0; i < stateDataCount; i++) {
        readU32(ptr, rem); // surfaceIndex
        readU32(ptr, rem); // mapIndex
        readU16(ptr, rem); // lightStateIndex
    }

    // ── State data buffer ──
    uint32_t stateDataBufSize = readU32(ptr, rem);
    readU32(ptr, rem); // flags (always 0)
    if (stateDataBufSize > rem) stateDataBufSize = (uint32_t)rem;
    ptr += stateDataBufSize; rem -= stateDataBufSize;

    // ── Name buffer ──
    uint32_t nameBufSize = readU32(ptr, rem);
    if (nameBufSize > rem) nameBufSize = (uint32_t)rem;
    ptr += nameBufSize; rem -= nameBufSize;

    // ── Sub-objects ──
    // InteriorSubObject::readISO reads U32 key, then tag-dependent data
    uint32_t subObjectCount = readU32(ptr, rem);
    for (uint32_t i = 0; i < subObjectCount; i++) {
        readU32(ptr, rem); // key
        // MirrorSubObject: U32 dl, U32 zone, F32 alpha, U32 sc, U32 ss, 3xF32
        readU32(ptr, rem); readU32(ptr, rem); readF32(ptr, rem);
        readU32(ptr, rem); readU32(ptr, rem);
        readF32(ptr, rem); readF32(ptr, rem); readF32(ptr, rem);
    }

    // ── Convex hulls ──
    uint32_t hullCount = readU32(ptr, rem);
    {
        size_t hullBytes = (size_t)hullCount * 52;
        if (hullBytes > rem) hullBytes = rem;
        ptr += hullBytes; rem -= hullBytes;
    }
    Console::instance().printf(LogLevel::Debug, "  hull: count=%u skipped=%zu", hullCount, (size_t)hullCount * 52);

    auto skipVecU8 = [&]() { uint32_t n = readU32(ptr, rem); size_t nb = n; if (nb > rem) nb = rem; ptr += nb; rem -= nb; };
    auto skipVecU16 = [&]() { uint32_t n = readU32(ptr, rem); size_t nb = (size_t)n * 2; if (nb > rem) nb = rem; ptr += nb; rem -= nb; };
    auto skipVecU32 = [&]() { uint32_t n = readU32(ptr, rem); size_t nb = (size_t)n * 4; if (nb > rem) nb = rem; ptr += nb; rem -= nb; };

    skipVecU8();  // hullEmitStrings
    skipVecU32(); // hullIndices
    skipVecU16(); // hullPlaneIndices
    skipVecU32(); // hullEmitStringIndices
    skipVecU32(); // hullSurfaceIndices
    skipVecU16(); // polyListPlanes
    skipVecU32(); // polyListPoints
    skipVecU8();  // polyListStrings

    // ── Coord bins (16x16 = 256 entries, each U32+U32) ──
    if (rem >= 256 * 8) { ptr += 256 * 8; rem -= 256 * 8; }
    skipVecU16(); // coord bin indices
    readU32(ptr, rem); // coord bin mode

    // ── Ambient colors (ColorF = 4xF32 each, ambient + alarm) ──
    if (rem >= 32) { ptr += 32; rem -= 32; }

    // ── 4x U32 padding ──
    if (rem >= 16) { ptr += 16; rem -= 16; }

    return true;
}

// ─── Convert Interior to meshes ──────────────────────────────────

static bool interiorToMeshes(DIFInterior& interior,
                              std::vector<MeshData>& outMeshes,
                              std::vector<Texture>& outTextures,
                              std::vector<uint32_t>& outMatFlags,
                              std::vector<int8_t>& outMatLMIndex,
                              std::vector<Texture>& outLightmaps,
                              std::vector<std::string>& outMatNames,
                              bool skipGpu = false)
{
    Console::instance().printf(LogLevel::Debug, "  i2m: start (mats=%zu)", interior.matNames.size());
    if (interior.surfaces.empty() || interior.points.empty()) {
        Console::instance().printf(LogLevel::Warn, "DIF: no surfaces or points in interior");
        return false;
    }

    // Load textures from material names (skip GPU ops if skipGpu)
    struct MatSlot { int texIdx = -1; };
    std::vector<MatSlot> matSlots(interior.matNames.size());

    if (!skipGpu) {
        for (size_t i = 0; i < interior.matNames.size(); i++) {
            Texture tex = resolveDIFTexture(interior.matNames[i]);
            if (tex.loaded) {
                matSlots[i].texIdx = (int)outTextures.size();
                outTextures.push_back(std::move(tex));
                outMatFlags.push_back(0);
                outMatNames.push_back(interior.matNames[i]);
            }
        }
    }
    Console::instance().printf(LogLevel::Debug, "  i2m: textures done (%zu mats)", interior.matNames.size());

    // Load lightmaps (skip GPU ops if skipGpu)
    if (!skipGpu) {
        for (auto& lme : interior.lightmapEntries) {
            if (lme.pngData.empty()) {
                Texture empty;
                outLightmaps.push_back(std::move(empty));
                continue;
            }
            Texture lmap;
            lmap.load(lme.pngData.data(), lme.pngData.size());
            outLightmaps.push_back(std::move(lmap));
        }
    }

    // Build lightmap index per material
    outMatLMIndex.assign(interior.matNames.size(), -1);
    for (auto& surf : interior.surfaces) {
        int matIdx = surf.textureIndex;
        if (matIdx >= 0 && matIdx < (int)outMatLMIndex.size()) {
            int lmIdx = surf.textureIndex < (int)interior.normalLMapIndices.size()
                ? interior.normalLMapIndices[surf.textureIndex] : -1;
            if (lmIdx >= 0 && lmIdx < (int)outLightmaps.size()) {
                if (outMatLMIndex[matIdx] < 0)
                    outMatLMIndex[matIdx] = (int8_t)lmIdx;
            }
        }
    }

    // Group surfaces by material index
    std::unordered_map<int, std::vector<int>> surfGroups;
    for (size_t si = 0; si < interior.surfaces.size(); si++) {
        auto& surf = interior.surfaces[si];
        if (surf.windingCount < 3) continue;
        int matIdx = surf.textureIndex;
        int texIdx = (matIdx >= 0 && matIdx < (int)matSlots.size()) ? matSlots[matIdx].texIdx : -1;
        surfGroups[texIdx].push_back((int)si);
    }

    if (surfGroups.empty()) {
        Console::instance().printf(LogLevel::Warn, "DIF: no surface groups");
        return false;
    }

    // Get plane normal
    auto getPlaneNormal = [&](uint16_t planeIdx) -> Point3F {
        uint16_t ni = planeIdx & 0x7FFF;
        bool flipped = (planeIdx & 0x8000) != 0;
        if (ni < interior.planeNormalIndices.size()) {
            uint16_t normIdx = interior.planeNormalIndices[ni];
            if (normIdx < interior.uniqueNormals.size() / 3) {
                Point3F n;
                n.x = interior.uniqueNormals[normIdx * 3];
                n.y = interior.uniqueNormals[normIdx * 3 + 1];
                n.z = interior.uniqueNormals[normIdx * 3 + 2];
                if (flipped) { n.x = -n.x; n.y = -n.y; n.z = -n.z; }
                return n;
            }
        }
        return {0, 1, 0};
    };

    // Generate UVs from TexGen planes
    auto genUV = [&](uint32_t texGenIdx, const Point3F& pos) -> Point2F {
        if (texGenIdx < interior.texGenEqs.size() / 8) {
            float* eq = &interior.texGenEqs[texGenIdx * 8];
            float u = eq[0]*pos.x + eq[1]*pos.y + eq[2]*pos.z + eq[3];
            float v = eq[4]*pos.x + eq[5]*pos.y + eq[6]*pos.z + eq[7];
            return {u, v};
        }
        return {0, 0};
    };

    // Build meshes
    for (auto& [texGroup, surfIdxs] : surfGroups) {
        MeshData mesh;
        mesh.materialIndex = texGroup;
        mesh.materialIdx = texGroup;

        struct VertKey {
            float x, y, z;
            float nx, ny, nz;
            float u, v;
            bool operator==(const VertKey& o) const {
                return fabsf(x-o.x)<1e-6f && fabsf(y-o.y)<1e-6f && fabsf(z-o.z)<1e-6f
                    && fabsf(nx-o.nx)<1e-4f && fabsf(ny-o.ny)<1e-4f && fabsf(nz-o.nz)<1e-4f
                    && fabsf(u-o.u)<1e-5f && fabsf(v-o.v)<1e-5f;
            }
        };
        struct VertHash {
            size_t operator()(const VertKey& k) const {
                size_t h1 = std::hash<float>()(k.x) ^ std::hash<float>()(k.y) ^ std::hash<float>()(k.z);
                size_t h2 = std::hash<float>()(k.u) ^ std::hash<float>()(k.v);
                return h1 ^ (h2 << 1);
            }
        };
        std::unordered_map<VertKey, uint32_t, VertHash> vertMap;
        std::vector<uint32_t> triIndices;

        for (int si : surfIdxs) {
            auto& surf = interior.surfaces[si];
            Point3F normal = getPlaneNormal(surf.planeIndex);

            uint8_t fanMaskBytes[32] = {};
            for (int b = 0; b < 4 && b < 32; b++) {
                fanMaskBytes[b] = (uint8_t)((surf.fanMask >> (b * 8)) & 0xFF);
            }

            std::vector<uint32_t> windVerts;
            for (uint32_t j = 0; j < surf.windingCount && (surf.windingStart + j) < interior.windings.size(); j++) {
                uint32_t ptIdx = interior.windings[surf.windingStart + j];
                if (ptIdx * 3 + 2 < interior.points.size()) {
                    float px = interior.points[ptIdx * 3];
                    float py = interior.points[ptIdx * 3 + 1];
                    float pz = interior.points[ptIdx * 3 + 2];
                    Point3F pos = {px, py, pz};
                    Point2F uv = genUV(surf.texGenIndex, pos);

                    VertKey vk = {pos.x, pos.y, pos.z, normal.x, normal.y, normal.z, uv.x, uv.y};
                    auto it = vertMap.find(vk);
                    uint32_t idx;
                    if (it != vertMap.end()) {
                        idx = it->second;
                    } else {
                        idx = (uint32_t)mesh.vertices.size();
                        vertMap[vk] = idx;
                        Vertex vert;
                        vert.pos = pos;
                        vert.normal = normal;
                        vert.uv = uv;
                        vert.color = {1, 1, 1, 1};
                        mesh.vertices.push_back(vert);
                    }
                    windVerts.push_back(idx);
                }
            }

            if (windVerts.size() < 3) continue;

            fanToTriangles(windVerts, {0, (uint32_t)windVerts.size()}, fanMaskBytes, triIndices, 0);
        }

        if (triIndices.empty()) continue;

        mesh.indices = std::move(triIndices);
        // upload() called later when renderer is ready (DTSShape::render)
        outMeshes.push_back(std::move(mesh));
    }

    return !outMeshes.empty();
}

// ─── Main DIF Loader ──────────────────────────────────────────────

DIFLoadResult loadDIF(const uint8_t* data, size_t size, const char* name, bool skipGpu) {
    DIFLoadResult result;
    if (!data || size < 12) return result;

    const uint8_t* ptr = data;
    size_t rem = size;

    uint32_t fileVersion = readU32(ptr, rem);
    if (fileVersion != 44) {
        Console::instance().printf(LogLevel::Warn, "DIF: unsupported version %u (expected 44) for '%s'", fileVersion, name);
        return result;
    }

    Console::instance().printf(LogLevel::Debug, "DIF: loading '%s' v%u (%zu bytes)", name, fileVersion, size);

    bool hasPreview = readU8(ptr, rem) != 0;
    if (hasPreview) {
        uint32_t pngSize = readU32(ptr, rem);
        if (pngSize > 0 && pngSize <= rem) {
            ptr += pngSize; rem -= pngSize;
        }
    }

    uint32_t numDetailLevels = readU32(ptr, rem);
    Console::instance().printf(LogLevel::Debug, "DIF: %u detail levels, starting parse", numDetailLevels);

    size_t prevRem = rem;
    std::vector<DIFInterior> interiors(numDetailLevels);
    for (uint32_t i = 0; i < numDetailLevels; i++) {
        Console::instance().printf(LogLevel::Debug, "DIF: parsing detail level %u (%zu bytes remaining)", i, rem);
        if (rem < 60) {
            Console::instance().printf(LogLevel::Warn, "DIF: not enough data for detail level %u", i);
            break;
        }
        if (!readInterior(ptr, rem, interiors[i])) {
            Console::instance().printf(LogLevel::Warn, "DIF: failed to parse detail level %u", i);
            break;
        }
        // Sanity: if we consumed suspiciously few bytes (e.g., all zeros), skip
        size_t consumed = prevRem - rem;
        Console::instance().printf(LogLevel::Debug, "DIF: detail level %u done (%zu surfaces, consumed %zu)", i, interiors[i].surfaces.size(), consumed);
        if (interiors[i].surfaces.empty() && consumed < 100) {
            Console::instance().printf(LogLevel::Warn, "DIF: detail level %u suspiciously small, stopping", i);
            break;
        }
        prevRem = rem;
    }
    Console::instance().printf(LogLevel::Debug, "DIF: all detail levels parsed (surfaces=%zu, points=%zu, windings=%zu, materials=%zu)",
        interiors[0].surfaces.size(), interiors[0].points.size() / 3,
        interiors[0].windings.size(), interiors[0].matNames.size());

    // Skip sub-objects and remaining top-level data (triggers, paths, etc.)
    // We only need the highest-detail level for rendering.

    Console::instance().printf(LogLevel::Debug, "DIF: converting interior meshes for '%s'", name);

    if (interiors.empty()) return result;

    Console::instance().printf(LogLevel::Debug, "DIF: highest detail: %zu surfaces, %zu points, %zu windings",
        interiors[0].surfaces.size(), interiors[0].points.size() / 3, interiors[0].windings.size());

    if (!interiorToMeshes(interiors[0],
                          result.meshes,
                          result.textures,
                          result.materialFlags,
                          result.materialLightmapIndex,
                          result.lightmaps,
                          result.materialNames,
                          skipGpu))
    {
        Console::instance().printf(LogLevel::Warn, "DIF: no meshes generated from '%s'", name);
        return result;
    }

    DTSShape::DetailLevel dl;
    dl.size = interiors[0].minPixels > 0 ? interiors[0].minPixels : 1000.0f;
    dl.meshIndex = 0;
    result.details.push_back(dl);

    Console::instance().printf(LogLevel::Info, "DIF: loaded '%s' (%zu meshes, %zu textures, %zu lightmaps)",
        name, result.meshes.size(), result.textures.size(), result.lightmaps.size());
    result.loaded = true;
    return result;
}
