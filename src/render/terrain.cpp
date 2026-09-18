#include "render/renderer.h"
#include "render/shader.h"
#include "render/dts_loader.h"
#include "render/dts_animation.h"
#include "render/dif_loader.h"
#include "render/material_parity.h"
#include "core/engine.h"
#include "stb_image.h"
#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <cctype>
#include <vector>
#include <algorithm>
#include <cmath>
#include <algorithm>

float TerrainBlock::sampleHeight(float wx, float wz) const {
    if (heights.empty() || size < 2 || squareSize <= 0.0f) return 0.0f;
    float fx = (wx - worldOffset.x) / squareSize;
    float fz = (worldOffset.z - wz) / squareSize;
    const int rawIx = (int)std::floor(fx);
    const int rawIz = (int)std::floor(fz);
    int ix = rawIx % size;
    int iz = rawIz % size;
    if (ix < 0) ix += size;
    if (iz < 0) iz += size;
    float tx = fx - rawIx;
    float tz = fz - rawIz;
    tx = Math::clamp(tx, 0.0f, 1.0f);
    tz = Math::clamp(tz, 0.0f, 1.0f);
    const int ix1 = (ix + 1) % size;
    const int iz1 = (iz + 1) % size;
    float h00 = heights[iz * size + ix];
    float h10 = heights[iz * size + ix1];
    float h01 = heights[iz1 * size + ix];
    float h11 = heights[iz1 * size + ix1];
    // Torque does not bilinearly smooth a square. Its split rule selects one
    // of the two planes represented by the terrain mesh, which is also what
    // the local mapper uses for terrain collision and ray hits.
    float height;
    if (((ix ^ iz) & 1) == 0) {
        height = tx >= tz
            ? h00 + (h10 - h00) * tx + (h11 - h10) * tz
            : h00 + (h01 - h00) * tz + (h11 - h01) * tx;
    } else {
        height = tx + tz <= 1.0f
            ? h00 + (h10 - h00) * tx + (h01 - h00) * tz
            : h11 + (h10 - h11) * (1.0f - tz) +
                  (h01 - h11) * (1.0f - tx);
    }
    return height * heightScale;
}

void TerrainBlock::reset() {
    const bool hasGLContext = SDL_GL_GetCurrentContext() != nullptr;
    if (hasGLContext)
        for (auto& mesh : meshes) mesh.destroy();
    meshes.clear();
    if (hasGLContext) {
        for (auto& texture : detailTextures) texture.destroy();
        for (auto& texture : normalTextures) texture.destroy();
        splatMap.destroy();
        splatMap2.destroy();
        gameGrid.destroy();
        lightmap.destroy();
    }
    detailTextures.clear();
    normalTextures.clear();
    textureNames.clear();
    heights.clear();
    emptySquares.clear();
    size = 256;
    heightScale = 1.0f;
    squareSize = 8.0f;
    worldOffset = {-1024, 0, 1024};
    std::fill(std::begin(detailTilings), std::end(detailTilings), 0.0f);
    lightDir = {0.5f, 0.8f, 0.6f};
    loaded = false;
}

void TerrainBlock::generateMesh() {
    if (heights.empty()) return;

    // Preserve the native terrain sample grid. Downsampling the 256x256 TER
    // into a 128x128 mesh changes silhouettes and makes map comparisons fail.
    int32_t gridRes = size + 1;
    float totalWorldSize = (float)size * squareSize;
    float step = totalWorldSize / (float)size;

    std::vector<Vertex> verts;
    std::vector<uint32_t> idxs;

    for (int32_t z = 0; z < gridRes; z++) {
        for (int32_t x = 0; x < gridRes; x++) {
            float wx = (float)x * step + worldOffset.x;
            float wz = worldOffset.z - (float)z * step;
            float h = sampleHeight(wx, wz);

            // Match MapGenius' smooth vertex normals: central differences of
            // the wrapped bilinear heightfield, rather than the selected
            // triangle plane (which creates diagonal banding).
            float eps = squareSize * 0.5f;
            float hxr = sampleHeight(wx + eps, wz);
            float hxl = sampleHeight(wx - eps, wz);
            float hzf = sampleHeight(wx, wz + eps);
            float hzb = sampleHeight(wx, wz - eps);
            Point3F n = { hxl - hxr, 2.0f * eps, hzb - hzf };
            float nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (nlen > 0) { n.x /= nlen; n.y /= nlen; n.z /= nlen; }

            const Point2F terrainUv{(float)x / size, (float)z / size};
            // The splat and lightmap textures are indexed by terrain square;
            // use the same 0..1/256 coordinates as MapGenius, including the
            // duplicated seam vertex at coordinate 1.
            verts.push_back({{wx, h, wz}, n, terrainUv, terrainUv, {1,1,1,1}});
            // Terrain coloration comes from the native material layers and
            // lightmap, not from an elevation-based replacement tint.
        }
    }

    for (int32_t z = 0; z < size; z++) {
        for (int32_t x = 0; x < size; x++) {
            // Torque-style alternating diagonals to avoid diagonal-cracks.
            // Quad corners (row z, col x): a=idx, b=idx+1, c=idx+gridRes, d=idx+gridRes+1
            int a = z * gridRes + x;
            int b = a + 1;
            int c = a + gridRes;
            int d = c + 1;
            if (!emptySquares.empty() && emptySquares[(size_t)z * size + x]) continue;
            if (((x ^ z) & 1) == 0) {
                // Split45: diagonal a->d, triangles (a,c,d) and (a,d,b)
                idxs.push_back(a); idxs.push_back(c); idxs.push_back(d);
                idxs.push_back(a); idxs.push_back(d); idxs.push_back(b);
            } else {
                // Split135: diagonal b->c, triangles (a,c,b) and (b,c,d)
                idxs.push_back(a); idxs.push_back(c); idxs.push_back(b);
                idxs.push_back(b); idxs.push_back(c); idxs.push_back(d);
            }
        }
    }

    MeshData mesh;
    mesh.vertices = std::move(verts);
    mesh.indices = std::move(idxs);
    mesh.upload();
    meshes.push_back(std::move(mesh));
}

void TerrainBlock::bakeLightmap() {
    // Generate a 512x512 terrain lightmap (2 px per terrain square) with smooth
    // bilinearly-sampled normals and ray-marched self-shadowing, matching the
    // approach used in t2-mapper / Torque's relight(). The result is NdotL*shadow
    // stored as the R channel, multiplied as lighting in the terrain shader.
    if (heights.empty()) return;
    // Lightmap resolution configurable via torch.cfg (default 512)
    int LM = 512;
    const char* lmRes = Console::instance().getStringVariable("terrainLightmapResolution");
    if (lmRes) {
        int parsed = atoi(lmRes);
        if (parsed > 0) LM = parsed;
    }
    std::vector<uint8_t> lm(LM * LM);
    auto hAt = [&](float col, float row) -> float {
        int cc = (int)std::floor(col);
        int rr = (int)std::floor(row);
        cc = Math::clamp((float)cc, 0.0f, (float)(size - 1));
        rr = Math::clamp((float)rr, 0.0f, (float)(size - 1));
        int c1 = std::min(cc + 1, size - 1);
        int r1 = std::min(rr + 1, size - 1);
        float fx = col - cc, fy = row - rr;
        float h00 = heights[rr * size + cc];
        float h10 = heights[rr * size + c1];
        float h01 = heights[r1 * size + cc];
        float h11 = heights[r1 * size + c1];
        float h0 = h00 + (h10 - h00) * fx;
        float h1 = h01 + (h11 - h01) * fx;
        return (h0 + (h1 - h0) * fy) * heightScale;
    };
    // Sun direction (world, Y-up, pointing FROM scene toward sun => direction light travels).
    Point3F L = lightDir;
    float len = std::sqrt(L.x*L.x + L.y*L.y + L.z*L.z);
    if (len > 0) { L.x/=len; L.y/=len; L.z/=len; } else { L = {0.5f,0.8f,0.6f}; }
    // Ray-march self-shadow
    auto rayShadow = [&](float sc, float sr, float sh) -> float {
        float dCol = L.x / squareSize;       // col ~ world +X (east)
        float dRow = L.z / squareSize;       // row ~ world +Z (south = increasing row after Y-up flip)
        float dHeight = L.y;                 // height ~ world +Y
        float hz = std::sqrt(dCol*dCol + dRow*dRow);
        if (hz < 0.0001f) return 1.0f;
        float scale = 0.5f / hz;
        dCol *= scale; dRow *= scale; dHeight *= scale;
        float col = sc, row = sr, h = sh + 0.1f;
        for (int i = 0; i < size * 3; i++) {
            col += dCol; row += dRow; h += dHeight;
            if (col < 0 || col >= size || row < 0 || row >= size) return 1.0f;
            if (h > 65535.0f) return 1.0f;
            if (h < hAt(col, row)) return 0.0f;
        }
        return 1.0f;
    };
    const float eps = 0.5f;
    for (int lr = 0; lr < LM; lr++) {
        for (int lc = 0; lc < LM; lc++) {
            float col = lc / 2.0f + 0.25f;
            float row = lr / 2.0f + 0.25f;
            float h = hAt(col, row);
            float hL = hAt(col - eps, row), hR = hAt(col + eps, row);
            float hU = hAt(col, row - eps), hD = hAt(col, row + eps);
            float dCol = (hR - hL) / (2 * eps);
            float dRow = (hD - hU) / (2 * eps);
            Point3F N{-dRow, squareSize, -dCol}; // world normal (row~X, col~Z)
            float nl = std::sqrt(N.x*N.x + N.y*N.y + N.z*N.z);
            if (nl > 0) { N.x/=nl; N.y/=nl; N.z/=nl; }
            float ndl = N.x*L.x + N.y*L.y + N.z*L.z;
            if (ndl < 0) ndl = 0;
            float shadow = 1.0f;
            if (ndl > 0) shadow = rayShadow(col, row, h);
            lm[lr * LM + lc] = (uint8_t)(ndl * shadow * 255);
        }
    }
    // Store as an RGBA texture (R channel holds intensity)
    std::vector<uint8_t> rgba(LM * LM * 4);
    for (int i = 0; i < LM * LM; i++) { rgba[i*4+0] = lm[i]; rgba[i*4+1] = lm[i]; rgba[i*4+2] = lm[i]; rgba[i*4+3] = 255; }
    lightmap.loadRaw(rgba.data(), LM, LM, 4);
    Console::instance().printf(LogLevel::Info, "Terrain: baked %dx%d self-shadowing lightmap", LM, LM);
}

bool TerrainBlock::load(const uint8_t* data, size_t size) {
    Console::instance().printf(LogLevel::Debug, "Terrain load: %zu bytes", size);
    if (!data || size < 4) {
        Console::instance().printf(LogLevel::Error,
            "Terrain: native .ter data is missing or truncated");
        return false;
    }

    // Parse .ter heightmap format
    // Version 1 byte, then SIZE*SIZE u16 height values
    uint32_t pos = 0;
    uint8_t version = data[pos++];
    const uint32_t TERRAIN_SIZE = 256;

    this->size = TERRAIN_SIZE;
    heights.resize(TERRAIN_SIZE * TERRAIN_SIZE, 0.0f);

    float maxH = 0;
    for (uint32_t z = 0; z < TERRAIN_SIZE; z++) {
        for (uint32_t x = 0; x < TERRAIN_SIZE; x++) {
            if (pos + 2 <= size) {
                uint16_t raw = data[pos] | ((uint16_t)data[pos + 1] << 8);
                pos += 2;
                // Convert from T2 11.5 fixed-point heightfield to world units.
                // Tribes2.exe multiplies stored shorts by exactly 0.03125 (= 1/32),
                // NOT by 1/65535 normalization. Using the wrong scale produced
                // terrain with ~200x exaggeration and inverted elevations.
                float h = (float)raw / 32.0f;
                heights[z * TERRAIN_SIZE + x] = h;
                if (std::abs(h) > maxH) maxH = std::abs(h);
            }
        }
    }

    Console::instance().printf(LogLevel::Info, "Terrain: loaded .ter v%u, max height=%.1f", version, maxH);

    // The region immediately after the heightfield is per-square flag data
    // (not a lightmap). It encodes terrain square attributes (e.g. empty/hole
    // flags, small-range values). We consume and skip it rather than misreading
    // it as a baked lightmap (which would corrupt terrain lighting).
    if (pos + TERRAIN_SIZE * TERRAIN_SIZE <= size) {
        pos += TERRAIN_SIZE * TERRAIN_SIZE;
    }

    // Read texture names (8 entries). The alpha maps below are packed for
    // non-empty slots, matching t2-mapper's TerrainFile parser.
    textureNames.clear();
    int nonEmptyCount = 0;
    for (int i = 0; i < 8 && pos < size; i++) {
        uint8_t nameLen = data[pos++];
        std::string texName;
        if (nameLen > 0 && pos + nameLen <= size) {
            texName = std::string((const char*)data + pos, nameLen);
            pos += nameLen;
        }
        if (i < 6 && !texName.empty()) {
            textureNames.push_back(std::move(texName));
            nonEmptyCount++;
        }
    }

    // Read alpha maps (nonEmptyCount × 256 × 256 bytes)
    // Build two RGBA splat textures: layers 0-3 (RGBA) and layers 4-5 (RGBA).
    // Up to 6 detail layers are supported; real T2 terrains commonly have 5-6.
    const uint32_t S = TERRAIN_SIZE;
    std::vector<uint8_t> splatPixels0(S * S * 4, 0);
    std::vector<uint8_t> splatPixels1(S * S * 4, 0);
    int maxLayer = nonEmptyCount;
    if (maxLayer > 6) maxLayer = 6;
    const char* detailCapStr = Console::instance().getStringVariable("detailTextureCount", "0");
    int detailCap = detailCapStr ? atoi(detailCapStr) : 0;
    if (detailCap > 0 && detailCap < maxLayer) maxLayer = detailCap;
    for (int layer = 0; layer < maxLayer && pos + S * S <= size; layer++) {
        std::vector<uint8_t>& dst = (layer < 4) ? splatPixels0 : splatPixels1;
        int ch = layer % 4;
        for (uint32_t z = 0; z < S; z++) {
            for (uint32_t x = 0; x < S; x++) {
                uint8_t alpha = data[pos + z * S + x];
                dst[(z * S + x) * 4 + ch] = alpha;
            }
        }
        pos += S * S;
    }
    // Ensure at least one layer has full weight where all are 0
    {
        bool anyNonZero = false;
        for (size_t i = 0; i < S * S * 4; i++) if (splatPixels0[i] > 0) { anyNonZero = true; break; }
        if (!anyNonZero && maxLayer > 0) {
            for (uint32_t i = 0; i < S * S; i++) splatPixels0[i * 4] = 255;
        }
    }
    splatMap.loadRaw(splatPixels0.data(), S, S, 4);
    if (maxLayer > 4) {
        bool anyNonZero1 = false;
        for (size_t i = 0; i < S * S * 4; i++) if (splatPixels1[i] > 0) { anyNonZero1 = true; break; }
        if (!anyNonZero1) for (uint32_t i = 0; i < S * S; i++) splatPixels1[i * 4] = 255;
        splatMap2.loadRaw(splatPixels1.data(), S, S, 4);
    }

    // Load detail textures from filesystem
    auto& fs = Engine::instance().fs();
    static const char* exts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"};
    int loadLayers = nonEmptyCount;
    if (loadLayers > 6) loadLayers = 6;
    if (detailCap > 0 && detailCap < loadLayers) loadLayers = detailCap;
    if (SDL_GL_GetCurrentContext()) for (int i = 0; i < loadLayers; i++) {
        Texture tex;
        // Convert terrain.X.Y.Z → textures/terrain/X.Y.Z
        // Also try textures/terrain/ prefix for names like "LushWorld.RockLight"
        std::string search = textureNames[i];
        std::vector<std::string> searchPaths;
        if (search.compare(0, 8, "terrain.") == 0)
            searchPaths.push_back("textures/terrain/" + search.substr(8));
        else {
            searchPaths.push_back("textures/terrain/" + search);
            searchPaths.push_back("textures/" + search);
        }
        // Try each search path with each extension, in original case then lowercase
        for (const auto& sp : searchPaths) {
            bool found = false;
            for (auto* ext : exts) {
                auto d = fs.read((sp + ext).c_str());
                if (!d.empty()) {
                    if (std::strcmp(ext, ".bm8") == 0)
                        tex.loadBM8(d.data(), d.size());
                    else
                        tex.load(d.data(), d.size());
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::string lower = sp;
                for (auto& c : lower) c = std::tolower(c);
                for (auto* ext : exts) {
                    auto d = fs.read((lower + ext).c_str());
                    if (!d.empty()) {
                        if (std::strcmp(ext, ".bm8") == 0)
                            tex.loadBM8(d.data(), d.size());
                        else
                            tex.load(d.data(), d.size());
                        found = true;
                        break;
                    }
                }
            }
            if (tex.loaded) break;
        }
        detailTextures.push_back(std::move(tex));
        if (!detailTextures.back().loaded)
            Console::instance().printf(LogLevel::Warn,
                "Terrain: required detail texture '%s' could not be loaded",
                textureNames[i].c_str());
    }

    // Load optional normal maps for each layer (independent of detail textures)
    if (SDL_GL_GetCurrentContext()) for (int i = 0; i < loadLayers; i++) {
        Texture tex;
        std::string baseName = textureNames[i];
        // Convert terrain.X.Y.Z → textures/terrain/X.Y.Z
        // Also try textures/terrain/ prefix for names like "LushWorld.RockLight"
        std::vector<std::string> searchPaths;
        if (baseName.compare(0, 8, "terrain.") == 0)
            searchPaths.push_back("textures/terrain/" + baseName.substr(8));
        else {
            searchPaths.push_back("textures/terrain/" + baseName);
            searchPaths.push_back("textures/" + baseName);
        }
        std::string normalSuffix = "_normal";
        for (const auto& sp : searchPaths) {
            std::string normalSearch = sp + normalSuffix;
            bool found = false;
            for (auto* ext : exts) {
                auto d = fs.read((normalSearch + ext).c_str());
                if (!d.empty()) {
                    if (std::strcmp(ext, ".bm8") == 0)
                        tex.loadBM8(d.data(), d.size());
                    else
                        tex.load(d.data(), d.size());
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::string lower = normalSearch;
                for (auto& c : lower) c = std::tolower(c);
                for (auto* ext : exts) {
                    auto d = fs.read((lower + ext).c_str());
                    if (!d.empty()) {
                        if (std::strcmp(ext, ".bm8") == 0)
                            tex.loadBM8(d.data(), d.size());
                        else
                            tex.load(d.data(), d.size());
                        found = true;
                        break;
                    }
                }
            }
            if (tex.loaded) break;
        }
        normalTextures.push_back(std::move(tex));
    }
    // Pad normalTextures to match detail texture count (max 6)
    while (normalTextures.size() < 6) {
        Texture empty;
        normalTextures.push_back(std::move(empty));
    }

    generateMesh();
    bakeLightmap();
    loaded = true;
    return true;
}

void TerrainBlock::render(const Point3F& cameraPos, bool fogEnabled, const ColorF& fogColor, float fogDensity, const Point3F* lightDir,
                           const ColorF* sunColor, const ColorF* ambient, float fogStart, float fogEnd) {
    auto* shader = ShaderManager::getTerrainShader();
    if (!shader) return;
    shader->bind();

    const auto& authoredVolumes = Engine::instance().game().world().fogVolumes;
    for (int i = 0; i < 3; ++i) {
        ColorF packed{};
        if (i < (int)authoredVolumes.size() && authoredVolumes[i].visibleDistance > 0.0f) {
            const auto& volume = authoredVolumes[i];
            packed = {volume.visibleDistance > 0.0f ? 1.0f / volume.visibleDistance : 0.0f,
                      volume.minHeight, volume.maxHeight, 0.0f};
        }
        shader->setUniform((std::string("uFogVolume") + std::to_string(i)).c_str(), packed);
    }

    // Apply dynamic light direction if provided
    if (lightDir) {
        shader->setUniform("uLightDir", *lightDir);
    }
    if (sunColor) shader->setUniform("uSunColor", Point3F{sunColor->r, sunColor->g, sunColor->b});
    if (ambient) shader->setUniform("uAmbient", Point3F{ambient->r, ambient->g, ambient->b});

    if (splatMap.loaded) splatMap.bind(0);
    if (splatMap2.loaded) splatMap2.bind(7);
    if (detailTextures.size() >= 1 && detailTextures[0].loaded) detailTextures[0].bind(1);
    if (detailTextures.size() >= 2 && detailTextures[1].loaded) detailTextures[1].bind(2);
    if (detailTextures.size() >= 3 && detailTextures[2].loaded) detailTextures[2].bind(3);
    if (detailTextures.size() >= 4 && detailTextures[3].loaded) detailTextures[3].bind(4);
    if (detailTextures.size() >= 5 && detailTextures[4].loaded) detailTextures[4].bind(8);
    if (detailTextures.size() >= 6 && detailTextures[5].loaded) detailTextures[5].bind(9);
    shader->setUniform("uSplatMap", (int32_t)0);
    shader->setUniform("uSplatMap2", (int32_t)7);
    shader->setUniform("uUseSplatMap2", (int32_t)(splatMap2.loaded ? 1 : 0));
    shader->setUniform("uDetail0", (int32_t)1);
    shader->setUniform("uDetail1", (int32_t)2);
    shader->setUniform("uDetail2", (int32_t)3);
    shader->setUniform("uDetail3", (int32_t)4);
    shader->setUniform("uDetail4", (int32_t)8);
    shader->setUniform("uDetail5", (int32_t)9);
    // Bind normal map if enabled and available
    if (Engine::instance().renderer().config().useNormalMap && normalTextures.size() >= 1 && normalTextures[0].loaded) {
        normalTextures[0].bind(10);
        shader->setUniform("uNormal0", (int32_t)10);
        shader->setUniform("uUseNormalMap", (int32_t)1);
    } else {
        shader->setUniform("uUseNormalMap", (int32_t)0);
    }

    if (lightmap.loaded) {
        lightmap.bind(6);
        shader->setUniform("uLightmap", (int32_t)6);
        shader->setUniform("uUseLightmap", (int32_t)1);
    } else {
        shader->setUniform("uUseLightmap", (int32_t)0);
    }

    // Use vertex color when no detail textures (procedural terrain)
    bool hasDetails = splatMap.loaded && detailTextures.size() >= 1 && detailTextures[0].loaded;
    shader->setUniform("uUseVertexColor", (int32_t)(hasDetails ? 0 : 1));

    // Calculate detail tiling based on terrain world size
    // Detail textures should tile at a consistent ~8-unit world-space density
    float worldSize = (float)size * squareSize;
    float defaultTiling = (worldSize / 8.0f) * Engine::instance().renderer().config().detailScale;
    shader->setUniform("uDetailTiling", defaultTiling);
    shader->setUniform("uDetailTiling0", detailTilings[0] > 0 ? detailTilings[0] : defaultTiling);
    shader->setUniform("uDetailTiling1", detailTilings[1] > 0 ? detailTilings[1] : defaultTiling);
    shader->setUniform("uDetailTiling2", detailTilings[2] > 0 ? detailTilings[2] : defaultTiling);
    shader->setUniform("uDetailTiling3", detailTilings[3] > 0 ? detailTilings[3] : defaultTiling);
    shader->setUniform("uDetailTiling4", detailTilings[4] > 0 ? detailTilings[4] : defaultTiling);
    shader->setUniform("uDetailTiling5", detailTilings[5] > 0 ? detailTilings[5] : defaultTiling);

    // Z-flip in generateMesh reverses triangle winding order; disable culling
    glDisable(GL_CULL_FACE);


    auto& renderer = Engine::instance().renderer();
    MatrixF model;
    shader->setUniform("uProjection", renderer.projection);
    shader->setUniform("uView", renderer.view);
    shader->setUniform("uModel", model);
    shader->setUniform("uCamPos", cameraPos);
    // The terrain is rendered before the next shadow pass is configured. Keep
    // the sampler disabled when the current frame has no valid shadow map.
    shader->setUniform("uShadowStrength", renderer.shadowsActive ? 0.6f : 0.0f);
    shader->setUniform("uFogEnabled", (int32_t)(fogEnabled ? 1 : 0));
    if (fogEnabled) {
        shader->setUniform("uFogColor", Point3F{fogColor.r, fogColor.g, fogColor.b});
        shader->setUniform("uFogDensity", fogDensity);
        shader->setUniform("uFogStart", fogStart >= 0.0f ? fogStart : (fogDensity > 0.0f ? 1.0f / fogDensity : -1.0f));
        shader->setUniform("uFogEnd", fogEnd > 0.0f ? fogEnd : renderer.config().farPlane);
    }

    for (auto& mesh : meshes)
        mesh.render();
}

// Font
#include "render/font8x8.h"

bool Font::loadGFT(const uint8_t* data, size_t size) {
    // V12 GFT format: version(u32), fontHeight(u32), baseLine(u32), charCount(u32)
    if (size < 16) return false;
    uint32_t ver, fontHeight, baseLineVal, count;
    memcpy(&ver, data, 4); memcpy(&fontHeight, data+4, 4); memcpy(&baseLineVal, data+8, 4); memcpy(&count, data+12, 4);
    (void)ver;
    if (count > 256) return false;
    // Per-char data: 9 bytes each: bitmapIndex(u16), xOffset(u8), yOffset(u8), width(u8), height(u8), xOrigin(s8), yOrigin(s8), xAdvance(u8)
    // Note: xOffset/yOffset = atlas position, xOrigin/yOrigin = bearings (V12 naming)
    uint32_t charDataOff = 16;
    uint32_t charDataSize = count * 9;
    size_t pngStartOff = charDataOff + charDataSize + 4; // 4 bytes for bitmapCount
    if (pngStartOff >= size) return false;
    // Read per-char metrics
    int asciiOff = 32;
    uint32_t maxW = 0;
    for (uint32_t i = 0; i < count && i + asciiOff < 256; i++) {
        const uint8_t* cd = data + charDataOff + i * 9;
        uint32_t ci = i + asciiOff;
        glyphs[ci].width = cd[4];
        glyphs[ci].height = cd[5];
        glyphs[ci].xOff = (int8_t)cd[6];   // bearing X
        glyphs[ci].yOff = (int8_t)cd[7];   // bearing Y
        glyphs[ci].xAdvance = cd[8];
        if (glyphs[ci].width > maxW) maxW = glyphs[ci].width;
    }
    charWidth = maxW > 0 ? maxW : (int32_t)fontHeight;
    charHeight = (int32_t)fontHeight;
    baseLine = (int32_t)baseLineVal;
    if (charHeight <= 0) charHeight = 12;
    if (baseLine <= 0) baseLine = charHeight;
    fontSize = (int32_t)fontHeight;
    proportional = true;


    // Skip bitmapCount (4 bytes) - always 1. PNG data follows immediately.
    const uint8_t* pngData = data + pngStartOff;
    size_t pngAvail = size - pngStartOff;
    int tw, th, tc;
    // GFT atlases are single-channel grayscale where intensity == coverage.
    // Decode as 1 channel and expand to RGBA (white glyph, alpha = coverage)
    // so vColor * tex yields correctly anti-aliased glyphs with a transparent
    // background instead of opaque black boxes.
    unsigned char* pixels = stbi_load_from_memory(pngData, (int)pngAvail, &tw, &th, &tc, 1);
    if (!pixels) return false;
    texWidth = tw; texHeight = th;
    std::vector<uint8_t> rgba((size_t)tw * th * 4);
    for (size_t i = 0, n = (size_t)tw * th; i < n; i++) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = pixels[i];
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(pixels);
    // Build UV coordinates for each char from the per-char position data
    for (uint32_t i = 0; i < count && i + asciiOff < 256; i++) {
        const uint8_t* cd = data + charDataOff + i * 9;
        uint32_t ci = i + asciiOff;
        uint8_t xo = cd[2], yo = cd[3];
        uint8_t gw = cd[4], gh = cd[5];
        if (gw == 0 || gh == 0) continue;
        float u0 = (float)xo / texWidth, v0 = (float)yo / texHeight;
        float u1 = (float)(xo + gw) / texWidth, v1 = (float)(yo + gh) / texHeight;
        charUV[ci][0] = u0; charUV[ci][1] = v0;
        charUV[ci][2] = u1; charUV[ci][3] = v1;
    }
    loaded = true;
    return true;
}

bool Font::loadDefault(int size) {
    if (size <= 0) size = 8;
    static const int cols = 16, rows = 16;
    int cw = size, ch = size;
    int tw = cols * cw, th = rows * ch;
    std::vector<uint8_t> pixels(tw * th * 4, 0);

    for (int i = 32; i <= 126; i++) {
        int idx = i - 32;
        int cx = (i % cols) * cw;
        int cy = (i / cols) * ch;
        for (int py = 0; py < ch && idx < 95; py++) {
            int srcRow = (size <= 8) ? py : py * 8 / size;
            uint8_t row = font8x8_basic[idx][srcRow];
            for (int px = 0; px < cw; px++) {
                int pi = ((cy + py) * tw + (cx + px)) * 4;
                int sx = (size <= 8) ? px : px * 8 / size;
                if (row & (0x80 >> sx)) {
                    pixels[pi + 0] = 255;
                    pixels[pi + 1] = 255;
                    pixels[pi + 2] = 255;
                    pixels[pi + 3] = 255;
                }
            }
        }
    }

    texWidth = tw;
    texHeight = th;
    charWidth = cw;
    charHeight = ch;
    proportional = false;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    float cw_f = 1.0f / cols, ch_f = 1.0f / rows;
    for (int i = 0; i < 256; i++) {
        int x = i % cols, y = i / cols;
        charUV[i][0] = x * cw_f;
        charUV[i][1] = y * ch_f;
        charUV[i][2] = (x + 1) * cw_f;
        charUV[i][3] = (y + 1) * ch_f;
    }

    // Initialize glyph metrics for all chars (monospace)
    for (int i = 0; i < 256; i++) {
        glyphs[i].width = cw;
        glyphs[i].height = ch;
        glyphs[i].xAdvance = cw;
    }

    loaded = true;
    return true;
}

bool Font::load(const uint8_t* data, size_t size) {
    // Load a bitmap font texture
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) return false;

    texWidth = w;
    texHeight = h;
    charWidth = w / 16;
    charHeight = h / 16;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    stbi_image_free(pixels);

    // Build UV coordinates for each char
    float cw = 1.0f / 16.0f;
    float ch = 1.0f / 16.0f;
    for (int i = 0; i < 256; i++) {
        int cx = i % 16;
        int cy = i / 16;
        charUV[i][0] = cx * cw;
        charUV[i][1] = cy * ch;
        charUV[i][2] = (cx + 1) * cw;
        charUV[i][3] = (cy + 1) * ch;
    }

    loaded = true;
    return true;
}

void Font::render(const char* text, float x, float y, const ColorF& color, float scale, bool exactColor, int maxChars) {
    if (!loaded || !text) return;
    scale *= defaultScale;
    // Honor the requested color exactly. T2 profiles pair dark font colors
    // (e.g. ShellButtonProfile "8 19 6") with light skin backgrounds, so no
    // brightness clamping — the old clamp also turned black text into solid
    // boxes when combined with the atlas alpha bug.
    ColorF col = color;
    if (maxChars < 0) maxChars = 2147483647;

    // Flush any pending sprite batch before we bind our own shader/projection
    Engine::instance().renderer().flushSpriteBatch();

    auto* shader = ShaderManager::getSpriteShader();
    if (!shader || !shader->loaded) return;
    shader->bind();

    auto& eng = Engine::instance();
    GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
    GLboolean depthWriteWasOn = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasOn);
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Use the active 2D projection. GUI dialogs intentionally render in a
    // logical 640x480 canvas even when the window is larger; rebuilding this
    // matrix from the physical window size misaligns every glyph.
    shader->setUniform("uProjection", eng.renderer().projection);
    shader->setUniform("uView", eng.renderer().view);
    shader->setUniform("uUseTexture", int32_t(1));
    shader->setUniform("uTexture", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    float lh = charHeight * scale;
    bool prop = proportional;

    struct SpriteVert { float x, y, z; float u, v; float r, g, b, a; };
    std::vector<SpriteVert> verts;

    float penX = x;
    float penY = y;

    int ctr = 0;
    for (const char* p = text; *p; p++) {
        if (ctr >= maxChars) break;
        ctr++;
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            penX = x;
            penY += lh;
            continue;
        }

        float gW = prop ? (float)glyphs[c].width * scale : lh;
        float gH = prop ? (float)glyphs[c].height * scale : lh;
        float gXOff = prop ? (float)glyphs[c].xOff * scale : 0;
        float gYOff = prop ? (float)(baseLine - glyphs[c].yOff) * scale : 0;
        float adv = prop ? (float)glyphs[c].xAdvance * scale : lh;

        float l = penX + gXOff, r2 = l + gW;
        float t = penY + gYOff, b2 = t + gH;
        float ra = col.r, ga = col.g, ba = col.b, aa = col.a;

        float u0 = charUV[c][0], v0 = charUV[c][1];
        float u1 = charUV[c][2], v1 = charUV[c][3];

        // 6 vertices per glyph (2 triangles, no index buffer)
        verts.push_back({l, t, 0, u0, v0, ra, ga, ba, aa});
        verts.push_back({r2, t, 0, u1, v0, ra, ga, ba, aa});
        verts.push_back({l, b2, 0, u0, v1, ra, ga, ba, aa});
        verts.push_back({r2, t, 0, u1, v0, ra, ga, ba, aa});
        verts.push_back({r2, b2, 0, u1, v1, ra, ga, ba, aa});
        verts.push_back({l, b2, 0, u0, v1, ra, ga, ba, aa});
        penX += adv;
    }

    if (verts.empty()) {
        if (depthWasOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (cullWasOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        glDepthMask(depthWriteWasOn);
        glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                            (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
        return;
    }

    if (!fontVAO) {
        glGenVertexArrays(1, &fontVAO);
        glGenBuffers(1, &fontVBO);
        glBindVertexArray(fontVAO);
        glBindBuffer(GL_ARRAY_BUFFER, fontVBO);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)(5*sizeof(float)));
        glEnableVertexAttribArray(2);
    }

    glBindVertexArray(fontVAO);
    glBindBuffer(GL_ARRAY_BUFFER, fontVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(SpriteVert), verts.data(), GL_DYNAMIC_DRAW);

    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
    if (depthWasOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWasOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glDepthMask(depthWriteWasOn);
    glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                        (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
}

Point2F Font::measure(const char* text, float scale) {
    Point2F result;
    if (!text) return result;
    scale *= defaultScale;
    float lineWidth = 0.0f;
    int lines = 1;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            result.x = std::max(result.x, lineWidth);
            lineWidth = 0.0f;
            ++lines;
            continue;
        }
        const float advance = proportional
            ? (float)glyphs[(unsigned char)*p].xAdvance
            : (float)charWidth;
        lineWidth += advance * scale;
    }
    result.x = std::max(result.x, lineWidth);
    result.y = charHeight * scale * lines;
    return result;
}

// Sky
void Sky::reset() {
    if (SDL_GL_GetCurrentContext()) {
        if (cubemap) glDeleteTextures(1, &cubemap);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (cloudVAO) glDeleteVertexArrays(1, &cloudVAO);
        if (cloudVBO) glDeleteBuffers(1, &cloudVBO);
        emap.destroy();
        for (auto& layer : cloudLayers) layer.texture.destroy();
    }
    cubemap = vao = vbo = cloudVAO = cloudVBO = 0;
    emap = {};
    cloudLayers.clear();
    fogVolumes.clear();
    loaded = false;
}

void Sky::load(const std::vector<std::string>& faces) {
    glGenTextures(1, &cubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);

    // Tribes 2 .dml sky lists are ordered F, R, B, L, T, D (front/right/back/
    // left/top/down).  This assignment matches the authoritative t2-mapper
    // reference (Sky.tsx): it maps the DML faces into the OpenGL cube faces via
    //  +X=dml[1], -X=dml[3], +Y=dml[4], -Y=dml[5], +Z=dml[0], -Z=dml[2].
    static const GLenum kCubemapFaceForDML[6] = {
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z, // dml[0] front  -> +Z
        GL_TEXTURE_CUBE_MAP_POSITIVE_X, // dml[1] right  -> +X
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, // dml[2] back   -> -Z
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X, // dml[3] left   -> -X
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y, // dml[4] top    -> +Y
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, // dml[5] down   -> -Y
    };

    // A GL_TEXTURE_CUBE_MAP is only complete (sampleable) when all 6 faces share
    // the same resolution. T2's DML "down" face (e.g. desert/skies/dd2) is a tiny
    // 4x4 placeholder, which would otherwise make the whole cubemap incomplete and
    // sample as black. Decode every face, record the max size, then upscale any
    // smaller face (nearest-neighbor) before uploading.
    const int kMaxFaces = 6;
    std::vector<uint8_t> facePixels[kMaxFaces];
    int faceW[kMaxFaces] = {0}, faceH[kMaxFaces] = {0};
    bool faceLoaded[kMaxFaces] = {false};
    int maxSize = 0;

    for (int i = 0; i < kMaxFaces && i < (int)faces.size(); i++) {
        auto data = Engine::instance().fs().read(faces[i].c_str());
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            unsigned char* pixels = nullptr;
            std::vector<uint8_t> bm8pixels;
            bool isBM8 = faces[i].size() >= 4 &&
                (faces[i].compare(faces[i].size() - 4, 4, ".bm8") == 0 ||
                 faces[i].compare(faces[i].size() - 4, 4, ".BM8") == 0);
            if (isBM8) {
                int32_t bw, bh;
                if (Texture::decodeBM8(data.data(), data.size(), bm8pixels, bw, bh)) {
                    pixels = bm8pixels.data();
                    w = bw; h = bh; ch = 4;
                }
            } else {
                pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
            }
            if (pixels) {
                faceW[i] = w; faceH[i] = h;
                facePixels[i].assign(pixels, pixels + (size_t)w * h * 4);
                faceLoaded[i] = true;
                if (w > maxSize) maxSize = w;
                if (h > maxSize) maxSize = h;
                if (!isBM8) stbi_image_free(pixels);
            }
        }
    }

    for (int i = 0; i < kMaxFaces; i++) {
        int size = maxSize > 0 ? maxSize : 256;
        if (!faceLoaded[i]) {
            Console::instance().printf(LogLevel::Error,
                "Sky: required native sky face failed to load: %s",
                i < (int)faces.size() ? faces[i].c_str() : "(missing DML face)");
            loaded = false;
            return;
        }
        // Upscale small faces (e.g. the 4x4 down face) to cubemap-complete size.
        if (faceW[i] != size || faceH[i] != size) {
            std::vector<uint8_t> scaled(size * size * 4);
            for (int y = 0; y < size; y++) {
                int sy = std::min((int)((int64_t)y * faceH[i] / size), faceH[i] - 1);
                for (int x = 0; x < size; x++) {
                    int sx = std::min((int)((int64_t)x * faceW[i] / size), faceW[i] - 1);
                    size_t src = ((size_t)sy * faceW[i] + sx) * 4;
                    size_t dst = ((size_t)y * size + x) * 4;
                    scaled[dst + 0] = facePixels[i][src + 0];
                    scaled[dst + 1] = facePixels[i][src + 1];
                    scaled[dst + 2] = facePixels[i][src + 2];
                    scaled[dst + 3] = facePixels[i][src + 3];
                }
            }
            glTexImage2D(kCubemapFaceForDML[i], 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled.data());
        } else {
            glTexImage2D(kCubemapFaceForDML[i], 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, facePixels[i].data());
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    loaded = true;

    // Skybox fullscreen triangle: covers the whole screen in NDC regardless of
    // FOV/aspect (the old unit cube only covered ~90 degrees of view and left
    // the periphery showing the clear color).
    float skyVerts[] = {
        -1.0f, -1.0f, 0.0f,
         3.0f, -1.0f, 0.0f,
        -1.0f,  3.0f, 0.0f,
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    loaded = true;
}

void Sky::render(const MatrixF& view, const MatrixF& proj, float cameraHeight) {
    auto* shader = ShaderManager::getSkyShader();
    if (!shader) return;
    shader->bind();

    GLint depthFunc;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glDepthFunc(GL_LEQUAL);
    // Invert the full view-projection so the fullscreen skybox pass can recover
    // a per-pixel world-space ray that is independent of FOV and aspect ratio.
    MatrixF invVP = (proj * view).inverse();
    shader->setUniform("uInvViewProj", invVP);

    // The native renderer still draws the solid sky when a requested DML face
    // is unavailable; do not leave the frame clear-color exposed.
    const bool drawCubemap = useSkyTextures && loaded && cubemap;
    shader->setUniform("uUseGradient", (int32_t)(drawCubemap ? 0 : 1));
    shader->setUniform("uGradTop", Point3F{solidColor.r, solidColor.g, solidColor.b});
    shader->setUniform("uGradBot", Point3F{solidColor.r, solidColor.g, solidColor.b});
    shader->setUniform("uFogColor", Point3F{fogColor.r, fogColor.g, fogColor.b});
    float fogTop = 0.0f;
    float fogVisibility = 0.0f;
    float fogPercentage = 1.0f;
    int lastVolume = -1;
    int firstVolume = -1;
    for (size_t i = 0; i < fogVolumes.size(); ++i) {
        const auto& volume = fogVolumes[i];
        if (volume.visibleDistance > 0.0f) {
            if (firstVolume < 0) firstVolume = (int)i;
            lastVolume = (int)i;
            fogTop = std::max(fogTop, volume.maxHeight);
        }
    }
    if (lastVolume >= 0) {
        fogVisibility = fogVolumes[lastVolume].visibleDistance;
        fogPercentage = fogVolumes[firstVolume].percentage;
        // V12 attenuates the final volume's visibility through denser slabs
        // below it instead of selecting only the camera's current slab.
        for (int i = 0; i < lastVolume; ++i) {
            const auto& volume = fogVolumes[i];
            if (volume.visibleDistance <= 0.0f ||
                volume.visibleDistance >= fogVisibility) continue;
            const float depthInVolume = cameraHeight < volume.minHeight
                ? volume.maxHeight - volume.minHeight
                : volume.maxHeight - cameraHeight;
            if (depthInVolume > 0.0f)
                fogVisibility -= fogVisibility * depthInVolume / volume.visibleDistance;
        }
    }
    const float radius = 0.95f * visibleDistance / std::sqrt(3.0f);
    float h0 = 0.0f, h1 = 60.0f, a0 = 0.0f, a1 = 0.0f;
    const float depth = fogTop - cameraHeight;
    if (fogVisibility > 0.0f && depth > 0.0f) {
        const float cap = radius / std::sqrt(2.0f);
        if (fogVisibility <= depth) h0 = h1 = cap, a0 = a1 = 1.0f;
        else {
            const float side = std::sqrt(fogVisibility * fogVisibility - depth * depth);
            h0 = std::min(cap, radius * depth / side);
            h1 = std::min(cap, radius * depth / (fogVisibility * 0.2f));
            if (h1 < 60.0f) h1 = 60.0f;
            a0 = h0 / cap;
            // When both rings reach the sphere cap, V12 keeps the strip opaque
            // and derives the fan alpha from the saturation ray.
            if (h0 == cap && h1 == cap) {
                a0 = 1.0f;
                const float temp = ((radius * depth / side) - cap) * side / depth;
                a1 = temp <= radius ? temp / radius : 1.0f;
            }
        }
        h0 *= fogPercentage; h1 = std::max(60.0f, h1 * fogPercentage);
    }
    shader->setUniform("uFogBands", ColorF{h0, h1, a0, a1});
    shader->setUniform("uSkyRadius", radius);
    if (drawCubemap) {
        shader->setUniform("uSkybox", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    }

    // Ensure VAO exists (create on first render if needed)
    if (!vao) {
        // Fullscreen triangle: covers the whole screen in NDC with 3 vertices,
        // unlike the old unit cube which only covered a ~90 degree FOV and left
        // the periphery showing the clear color.
        float skyVerts[] = {
            -1.0f, -1.0f, 0.0f,
             3.0f, -1.0f, 0.0f,
            -1.0f,  3.0f, 0.0f,
        };
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
    }

    // A skybox must never be face-culled.
    GLboolean skyCullWasOn = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (skyCullWasOn) glEnable(GL_CULL_FACE);

    // Render cloud layers (scrolling textured quads at sky distance)
    if (!cloudLayers.empty()) {
        auto* cloudShader = ShaderManager::getCloudShader();
        if (cloudShader) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            cloudShader->bind();
            cloudShader->setUniform("uProjection", proj);
            cloudShader->setUniform("uView", view);

            float time = Engine::instance().game().gameTime();

            for (size_t ci = 0; ci < cloudLayers.size(); ci++) {
                auto& cloud = cloudLayers[ci];
                if (!cloud.texture.loaded) continue;

                // Create cloud VAO/VBO if needed
                if (!cloudVAO) {
                    float verts[] = {
                        // pos (x,y,z) + uv (u,v)
                        -1, 0, -1,  0, 0,
                         1, 0, -1,  1, 0,
                         1, 0,  1,  1, 1,
                        -1, 0, -1,  0, 0,
                         1, 0,  1,  1, 1,
                        -1, 0,  1,  0, 1,
                    };
                    glGenVertexArrays(1, &cloudVAO);
                    glGenBuffers(1, &cloudVBO);
                    glBindVertexArray(cloudVAO);
                    glBindBuffer(GL_ARRAY_BUFFER, cloudVBO);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
                    glEnableVertexAttribArray(1);
                }

                // Scroll UVs over time
                float scrollU = time * cloud.scrollSpeed * 0.001f;

                // Position the cloud dome above the camera
                MatrixF model;
                float height = 50.0f + cloud.height * 150.0f;
                model.setTranslation(Point3F(0, height, 0));

                MatrixF mvp = proj * view * model;
                cloudShader->setUniform("uMVP", mvp);
                cloudShader->setUniform("uOpacity", cloud.opacity);
                cloudShader->setUniform("uScrollU", scrollU);

                cloud.texture.bind(0);
                cloudShader->setUniform("uTexture", (int32_t)0);

                glBindVertexArray(cloudVAO);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                                (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
        }
    }

    glDepthFunc((GLenum)depthFunc);
}

#if 0 // GLB is a diagnostic/interchange format, never a production asset path.
bool DTSShape::loadGLB(const uint8_t* data, size_t size) {
    ::GLBMesh glb = ::loadGLB(data, size);
    if (glb.meshes.empty()) return false;
    meshes = std::move(glb.meshes);
    materialTextures = std::move(glb.textures);

    // ── Build material flags parallel to materialTextures ──
    // Each entry in materialTextures corresponds to a slot; map from GLB material index.
    std::vector<int> matToTex(glb.materials.size(), -1);
    materialFlags.clear();

    // For embedded textures, map material → existing texture index
    for (size_t i = 0; i < glb.materials.size(); i++) {
        if (i < materialTextures.size() && materialTextures[i].loaded) {
            matToTex[i] = (int)i;
        }
    }

    // Resolve textures from filesystem for materials with resource_path
    // but no embedded texture loaded (common for shapes)
    bool needsResolution = false;
    for (size_t i = 0; i < glb.materials.size(); i++) {
        if (!glb.materials[i].resourcePath.empty() && matToTex[i] < 0) {
            needsResolution = true;
            break;
        }
    }

    if (needsResolution) {
        auto& fs = Engine::instance().fs();
        static const char* exts[] = {".png", ".bm8", ".jpg", ".gif", ".bmp"};

        for (size_t i = 0; i < glb.materials.size(); i++) {
            auto& mat = glb.materials[i];
            if (mat.resourcePath.empty() || matToTex[i] >= 0) continue;

            std::string texPath = "textures/" + mat.resourcePath;
            for (auto& c : texPath) c = std::tolower(c);
            std::vector<uint8_t> texData;
            const char* matchedExt = nullptr;
            for (auto* ext : exts) {
                auto data = fs.read((texPath + ext).c_str());
                if (!data.empty()) {
                    texData = std::move(data);
                    matchedExt = ext;
                    break;
                }
            }

            if (!texData.empty()) {
                Texture tex;
                if (matchedExt && std::strcmp(matchedExt, ".bm8") == 0)
                    tex.loadBM8(texData.data(), texData.size());
                else
                    tex.load(texData.data(), texData.size());
                if (tex.loaded) {
                    matToTex[i] = (int)materialTextures.size();
                    materialTextures.push_back(std::move(tex));
                }
            }
        }
    }

    // Store material names (resource paths) for skin override support
    materialNames.clear();
    for (auto& mat : glb.materials)
        materialNames.push_back(mat.resourcePath);

    // Finalize material flags — one per slot in materialTextures
    materialFlags.resize(materialTextures.size(), 0);
    materialMetallic.resize(materialTextures.size(), 0.0f);
    materialRoughness.resize(materialTextures.size(), 0.5f);
    for (size_t i = 0; i < glb.materials.size(); i++) {
        int ti = matToTex[i];
        if (ti >= 0 && ti < (int)materialFlags.size()) {
            materialFlags[ti] = glb.materials[i].flags;
            materialMetallic[ti] = glb.materials[i].metallic;
            materialRoughness[ti] = glb.materials[i].roughness;
        }
    }

    // Store all lightmaps from GLB
    lightmaps = std::move(glb.lightmaps);

    // Build per-material lightmap index (maps material → lightmaps[] entry, -1 if none)
    materialLightmapIndex.resize(glb.materials.size(), -1);
    for (size_t i = 0; i < glb.materials.size(); i++) {
        int ei = glb.materials[i].emissiveTextureIndex;
        if (ei >= 0 && ei < (int)lightmaps.size() && lightmaps[ei].loaded)
            materialLightmapIndex[i] = ei;
    }

    // Update mesh materialIndex
    for (auto& mesh : meshes) {
        if (mesh.materialIdx >= 0 && mesh.materialIdx < (int)matToTex.size()) {
            int newIdx = matToTex[mesh.materialIdx];
            if (newIdx >= 0) mesh.materialIndex = newIdx;
        }
    }

    // Copy animations from GLB
    animations = std::move(glb.animations);

    // GLB meshes are authored Y-up; no Z-up->Y-up conversion needed.
    upConvert = false;

    loaded = true;
    return true;
}
#endif

// DTS/DIF shape loader — parses Torque DTS binary or DIF interior format
bool DTSShape::load(const uint8_t* data, size_t size) {
    if (!data || size < 12) return false;

    // DIF interiors: version 44 at offset 0 (or isInterior flag is set)
    uint32_t version = *(const uint32_t*)data;
    if (isInterior || version == 44) {
        DIFLoadResult difResult = loadDIF(data, size, name.c_str());
        if (difResult.loaded) {
            meshes = std::move(difResult.meshes);
            materialTextures = std::move(difResult.textures);
            materialFlags = std::move(difResult.materialFlags);
            materialLightmapIndex = std::move(difResult.materialLightmapIndex);
            lightmaps = std::move(difResult.lightmaps);
            materialNames = std::move(difResult.materialNames);
            collisionVerts = std::move(difResult.hullCollisionVerts);
            collisionIndices = std::move(difResult.hullCollisionIndices);
            details = difResult.details;
            interiorPlanes.clear();
            for (const auto& plane : difResult.interiorPlanes)
                interiorPlanes.push_back({plane.normal, plane.d});
            interiorBSP.clear();
            for (const auto& node : difResult.interiorBSP)
                interiorBSP.push_back({node.planeIndex, node.frontIndex, node.backIndex});
            interiorPortals.clear();
            for (const auto& portal : difResult.interiorPortals)
                interiorPortals.push_back({portal.planeIndex, portal.zoneFront, portal.zoneBack, portal.vertices});
            interiorZoneNeighbors = std::move(difResult.interiorZoneNeighbors);
            isInterior = true;
            loaded = true;
            return true;
        }
    }

    // Try native DTS loading
    try {
    nativeDTS = false;
    DTSLoadResult dtsResult = loadDTS(data, size, name.c_str());
    if (dtsResult.loaded) {
        meshes = std::move(dtsResult.meshes);
        skins = std::move(dtsResult.skins);
        defaultTransforms = std::move(dtsResult.defaultTransforms);
        defaultLocalTransforms = std::move(dtsResult.defaultLocalTransforms);
        materialTextures = std::move(dtsResult.textures);
        materialFlags = std::move(dtsResult.materialFlags);
        materialReflectionAmount = std::move(dtsResult.materialReflectionAmount);
        lightmaps = std::move(dtsResult.lightmaps);
        materialLightmapIndex = std::move(dtsResult.materialLightmapIndex);
        materialNames = std::move(dtsResult.materialNames);
        details = dtsResult.details;
        animations = dtsResult.animations;
        nodes = dtsResult.nodes;
        objectStartMesh = std::move(dtsResult.objectStartMesh);
        objectNumMeshes = std::move(dtsResult.objectNumMeshes);
        meshTVerts = std::move(dtsResult.meshTVerts);

        if (details.empty()) {
            DTSShape::DetailLevel dl;
            dl.size = 1000.0f;
            dl.meshIndex = 0;
            details.push_back(dl);
        }

        // Native DTS data was canonicalized to the renderer basis by loadDTS.
        // No additional render-time axis conversion is needed.
        upConvert = false;
        nativeDTS = true;

        loaded = true;
        return true;
    }

    return false;
    } catch (const std::exception& e) {
        Console::instance().printf(LogLevel::Warn, "DTS: exception loading '%s': %s", name.c_str(), e.what());
        return false;
    } catch (...) {
        Console::instance().printf(LogLevel::Warn, "DTS: unknown exception loading '%s' - skipping", name.c_str());
        return false;
    }
}

int DTSShape::interiorZoneForPoint(const Point3F& point) const {
    if (interiorBSP.empty() || interiorPlanes.empty()) return -1;
    uint16_t node = 0;
    for (size_t steps = 0; steps <= interiorBSP.size(); steps++) {
        if (node >= interiorBSP.size()) return -1;
        const auto& bsp = interiorBSP[node];
        if (bsp.planeIndex >= interiorPlanes.size()) return -1;
        const auto& plane = interiorPlanes[bsp.planeIndex & 0x7FFF];
        float distance = plane.normal.x * point.x + plane.normal.y * point.y +
                         plane.normal.z * point.z + plane.d;
        if (bsp.planeIndex & 0x8000) distance = -distance;
        uint16_t child = distance >= 0.0f ? bsp.frontIndex : bsp.backIndex;
        if ((child & 0x8000) == 0) { node = child; continue; }
        if (child & 0x4000) return -1;
        return child & ~0xC000;
    }
    return -1;
}

void DTSShape::interiorVisibleZones(int zone, const Point3F& camera,
                                    const MatrixF& clipTransform,
                                    std::vector<bool>& visible) const {
    visible.assign(interiorZoneNeighbors.size(), false);
    if (zone < 0) {
        std::fill(visible.begin(), visible.end(), true);
        return;
    }
    if (zone >= (int)interiorZoneNeighbors.size()) return;
    struct ScreenRect { float left, bottom, right, top; };
    struct ClipPlane { Point3F normal; float d; };
    const ScreenRect fullScreen{-1.0f, -1.0f, 1.0f, 1.0f};
    struct PendingZone { uint16_t zone; ScreenRect rect; std::vector<ClipPlane> planes; };
    std::vector<PendingZone> pending{{(uint16_t)zone, fullScreen, {}}};
    std::vector<ScreenRect> zoneRects(interiorZoneNeighbors.size(), {1.0f, 1.0f, -1.0f, -1.0f});
    zoneRects[zone] = fullScreen;
    visible[zone] = true;

    auto portalRect = [&](const InteriorPortal& portal, ScreenRect& rect) {
        rect = {1.0f, 1.0f, -1.0f, -1.0f};
        for (const auto& point : portal.vertices) {
            const float x = clipTransform.m[0][0] * point.x + clipTransform.m[0][1] * point.y +
                             clipTransform.m[0][2] * point.z + clipTransform.m[0][3];
            const float y = clipTransform.m[1][0] * point.x + clipTransform.m[1][1] * point.y +
                             clipTransform.m[1][2] * point.z + clipTransform.m[1][3];
            const float w = clipTransform.m[3][0] * point.x + clipTransform.m[3][1] * point.y +
                            clipTransform.m[3][2] * point.z + clipTransform.m[3][3];
            if (w <= 0.0f) {
                rect = fullScreen;
                return true;
            }
            const float ndcX = x / w, ndcY = y / w;
            rect.left = std::min(rect.left, ndcX);
            rect.bottom = std::min(rect.bottom, ndcY);
            rect.right = std::max(rect.right, ndcX);
            rect.top = std::max(rect.top, ndcY);
        }
        return rect.left <= 1.0f && rect.right >= -1.0f &&
               rect.bottom <= 1.0f && rect.top >= -1.0f;
    };

    for (size_t i = 0; i < pending.size(); i++) {
        uint16_t current = pending[i].zone;
        const ScreenRect parentRect = pending[i].rect;
        const auto parentPlanes = pending[i].planes;
        for (uint16_t neighbor : interiorZoneNeighbors[current]) {
            bool portalInView = false;
            ScreenRect childRect{};
            std::vector<ClipPlane> childPlanes;
            for (const auto& portal : interiorPortals) {
                const bool matches = (portal.zoneFront == current && portal.zoneBack == neighbor) ||
                                     (portal.zoneBack == current && portal.zoneFront == neighbor);
                if (!matches) continue;
                if ((portal.planeIndex & 0x7FFF) < interiorPlanes.size() &&
                    !interiorPortalAllowsTraversal(
                        portal.planeIndex, portal.zoneFront, portal.zoneBack,
                        current, camera, interiorPlanes[portal.planeIndex & 0x7FFF]))
                    continue;
                bool outsideParentPlane = false;
                for (const auto& plane : parentPlanes) {
                    bool anyInside = false;
                    for (const auto& point : portal.vertices) {
                        if (plane.normal.x * point.x + plane.normal.y * point.y +
                            plane.normal.z * point.z + plane.d >= 0.0f) {
                            anyInside = true;
                            break;
                        }
                    }
                    if (!anyInside) {
                        outsideParentPlane = true;
                        break;
                    }
                }
                if (outsideParentPlane) break;
                portalInView = portalRect(portal, childRect);
                childRect.left = std::max(childRect.left, parentRect.left);
                childRect.bottom = std::max(childRect.bottom, parentRect.bottom);
                childRect.right = std::min(childRect.right, parentRect.right);
                childRect.top = std::min(childRect.top, parentRect.top);
                portalInView = portalInView && childRect.left <= childRect.right &&
                               childRect.bottom <= childRect.top;
                if (portalInView && portal.vertices.size() >= 3) {
                    childPlanes = parentPlanes;
                    Point3F center{0, 0, 0};
                    for (const auto& point : portal.vertices) {
                        center.x += point.x; center.y += point.y; center.z += point.z;
                    }
                    const float count = (float)portal.vertices.size();
                    center.x /= count; center.y /= count; center.z /= count;
                    for (size_t edge = 0; edge < portal.vertices.size(); edge++) {
                        const Point3F& a = portal.vertices[edge];
                        const Point3F& b = portal.vertices[(edge + 1) % portal.vertices.size()];
                        const Point3F va{a.x - camera.x, a.y - camera.y, a.z - camera.z};
                        const Point3F vb{b.x - camera.x, b.y - camera.y, b.z - camera.z};
                        Point3F normal{
                            va.y * vb.z - va.z * vb.y,
                            va.z * vb.x - va.x * vb.z,
                            va.x * vb.y - va.y * vb.x};
                        float d = -(normal.x * camera.x + normal.y * camera.y + normal.z * camera.z);
                        if (normal.x * center.x + normal.y * center.y + normal.z * center.z + d < 0.0f) {
                            normal.x = -normal.x; normal.y = -normal.y; normal.z = -normal.z; d = -d;
                        }
                        childPlanes.push_back({normal, d});
                    }
                }
                break;
            }
            if (!portalInView) continue;
            if (neighbor < visible.size()) {
                const ScreenRect previous = zoneRects[neighbor];
                const ScreenRect merged{
                    std::min(previous.left, childRect.left),
                    std::min(previous.bottom, childRect.bottom),
                    std::max(previous.right, childRect.right),
                    std::max(previous.top, childRect.top)};
                const bool expanded = !visible[neighbor] ||
                    merged.left < previous.left || merged.bottom < previous.bottom ||
                    merged.right > previous.right || merged.top > previous.top;
                if (expanded) {
                    visible[neighbor] = true;
                    zoneRects[neighbor] = merged;
                    pending.push_back({neighbor, merged, std::move(childPlanes)});
                }
            }
        }
    }
}

bool DTSShape::applySkin(const std::string& skinName) {
    if (materialNames.empty() || materialTextures.empty() || skinName.empty()) return false;

    auto& fs = Engine::instance().fs();
    bool anyReplaced = false;
    static const char* exts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".dds"};

    for (size_t i = 0; i < materialNames.size(); i++) {
        std::string matName = materialNames[i];
        if (matName.empty()) continue;

        // Keep armor suffixes such as `.lmale`; strip only actual image
        // extensions from material resource paths.
        for (const char* ext : {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".dds"}) {
            if (matName.size() > std::strlen(ext) &&
                matName.compare(matName.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
                matName.resize(matName.size() - std::strlen(ext));
                break;
            }
        }

        // Remove leading "textures/" prefix if present (it's added by texture search)
        if (matName.find("textures/") == 0) matName = matName.substr(9);

        std::string materialFile = matName;
        const auto slash = materialFile.rfind('/');
        if (slash != std::string::npos) materialFile = materialFile.substr(slash + 1);
        const auto materialDot = materialFile.find('.');
        const std::string materialSuffix = materialDot == std::string::npos
            ? std::string() : materialFile.substr(materialDot);

        // Try multiple candidate paths for the skin variant
        std::vector<std::string> candidates = {
            matName + "/" + skinName,                        // "skins/base/light_red"
            "skins/" + skinName + "/" + matName,             // "skins/light_red/skins/base"
            skinName + "/" + matName,                        // "light_red/skins/base"
            "skins/" + skinName + "." + matName,             // "skins/beagle.lmale"
            skinName + "." + matName,                         // "beagle.lmale"
            "skins/" + skinName + materialSuffix,             // "skins/beagle.lmale"
            skinName + materialSuffix,                         // "beagle.lmale"
            "skins/" + skinName,                             // "skins/light_red"
            skinName,                                        // "light_red"
        };

        // Try each candidate with the "textures/" prefix and each extension
        bool found = false;
        for (auto& cand : candidates) {
            std::string basePath = "textures/" + cand;
            for (auto* ext : exts) {
                std::vector<uint8_t> data = fs.read((basePath + ext).c_str());
                if (!data.empty()) {
                    Texture tex;
                    if (std::strcmp(ext, ".bm8") == 0)
                        tex.loadBM8(data.data(), data.size());
                    else
                        tex.load(data.data(), data.size());
                    if (tex.loaded) {
                        // Find the texture slot for this material
                        // materialNames[i] corresponds to the i-th material in the DTS/GLB
                        // We need to find which texture slot it maps to
                        if (i < materialTextures.size()) {
                            materialTextures[i] = std::move(tex);
                            anyReplaced = true;
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (found) break;
        }
    }

    if (anyReplaced)
        Console::instance().printf(LogLevel::Debug, "applySkin('%s'): replaced %zu materials", skinName.c_str(), materialTextures.size());
    return anyReplaced;
}

void DTSShape::render(int32_t detailLevel, const NodeOverride* overrides, int numOverrides) {
    try {
    // renderAnimation mutates mesh frame/UV buffers; static draws must restore
    // the bind pose so an animated instance cannot contaminate the next draw.
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        meshes[mi].setFrame(0);
        if (mi < meshTVerts.size()) meshes[mi].remapUVs(0, meshTVerts[mi]);
    }
    auto* shader = ShaderManager::getDefaultShader();
    if (shader) shader->bind();
    auto& r = Engine::instance().renderer();
    if (!shader) return;
    shader->setUniform("uProjection", r.projection);
    shader->setUniform("uView", r.view);
    shader->setUniform("uCamPos", r.cameraPos);
    if (shader) shader->setUniform("uShadowStrength", r.shadowsActive ? 0.6f : 0.0f);
    shader->setUniform("uDebugInterior", (int32_t)(getenv("TORCH_DIF_RED") ? 1 : 0));
    shader->setUniform("uInterior", (int32_t)(isInterior ? 1 : 0));
    shader->setUniform("uInteriorOutsideVisible", (int32_t)0);
    shader->setUniform("uDebugLightmap", (int32_t)(getenv("TORCH_DIF_LMUV") ? 1 : 0));
    // Also check lightmap-only mode for debugging
    shader->setUniform("uDebugTex", (int32_t)(getenv("TORCH_DIF_TEXUV") ? 1 : 0));
    shader->setUniform("uDebugTexColor", (int32_t)(getenv("TORCH_DIF_TEXCOL") ? 1 : 0));
    shader->setUniform("uDebugTexOnly", (int32_t)(getenv("TORCH_DIF_TEXONLY") ? 1 : 0));
    shader->setUniform("uDebugLightmapContent", (int32_t)(getenv("TORCH_DIF_LMCONTENT") ? 1 : 0));

    // Build effective node transforms, applying any overrides
    std::vector<MatrixF> nodeWorld = defaultTransforms;
    if (overrides && numOverrides > 0) {
        std::vector<MatrixF> nodeLocal = defaultLocalTransforms;
        for (int i = 0; i < numOverrides; i++) {
            const int32_t nodeIndex = overrides[i].nodeIndex;
            if (nodeIndex < 0 || nodeIndex >= (int)nodeWorld.size()) continue;
            const int32_t parentIndex = nodes[nodeIndex].parentIndex;
            if (parentIndex >= 0 && parentIndex < (int)nodeWorld.size())
                nodeLocal[nodeIndex] = defaultTransforms[parentIndex].inverse() * overrides[i].transform;
            else
                nodeLocal[nodeIndex] = overrides[i].transform;
        }
        for (int32_t i = 0; i < (int32_t)nodeWorld.size(); i++) {
            const int32_t parentIndex = nodes[i].parentIndex;
            if (parentIndex >= 0 && parentIndex < (int32_t)nodeWorld.size())
                nodeWorld[i] = nodeWorld[parentIndex] * nodeLocal[i];
            else
                nodeWorld[i] = nodeLocal[i];
        }
    }
    const MatrixF baseModel = r.modelMatrix();

    if (isInterior) {
        // DIF interior surfaces have inconsistent per-surface winding and may be
        // viewed from outside (mapper). Render both faces so no surface fragment
        // disappears due to culling.
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    // Diagnostic: print world-space AABB of interiors for camera framing
    if (isInterior && getenv("TORCH_AABB")) {
        Point3F mn{1e30f,1e30f,1e30f}, mx{-1e30f,-1e30f,-1e30f};
        for (auto& m : meshes) for (auto& v : m.vertices) {
            Point3F wp = baseModel.transform(v.pos);
            mn.x=fminf(mn.x,wp.x); mn.y=fminf(mn.y,wp.y); mn.z=fminf(mn.z,wp.z);
            mx.x=fmaxf(mx.x,wp.x); mx.y=fmaxf(mx.y,wp.y); mx.z=fmaxf(mx.z,wp.z);
        }
        fprintf(stderr, "AABB '%s' min=(%.1f %.1f %.1f) max=(%.1f %.1f %.1f)\n",
                name.c_str(), mn.x,mn.y,mn.z, mx.x,mx.y,mx.z);
        // Per-mesh world-space AABB
        for (size_t mi=0; mi<meshes.size(); mi++) {
            auto& m = meshes[mi];
            Point3F mmn{1e30f,1e30f,1e30f}, mmx{-1e30f,-1e30f,-1e30f};
            for (auto& v : m.vertices) {
                Point3F wp = baseModel.transform(v.pos);
                mmn.x=fminf(mmn.x,wp.x); mmn.y=fminf(mmn.y,wp.y); mmn.z=fminf(mmn.z,wp.z);
                mmx.x=fmaxf(mmx.x,wp.x); mmx.y=fmaxf(mmx.y,wp.y); mmx.z=fmaxf(mmx.z,wp.z);
            }
            fprintf(stderr, "MESHW[%zu] tris=%zu aabb(min %.1f %.1f %.1f max %.1f %.1f %.1f)\n",
                    mi, m.indices.size()/3, mmn.x,mmn.y,mmn.z, mmx.x,mmx.y,mmx.z);
        }
    }

    // Reset GL state
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (getenv("TORCH_NODEPTHTEST")) glDepthFunc(GL_ALWAYS);
    if (getenv("TORCH_WIRE")) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    // T2 fadeSet: force alpha test + translucent blending for ALL materials.
    // For RGB textures (alpha=255), this changes nothing. For RGBA textures,
    // alpha=0 pixels are discarded and the rest blend correctly.
    // Two-pass: textures with alphaZeroRatio=0 render opaque (depth writes ON),
    // textures with alphaZeroRatio>0 render translucent (depth writes OFF, blending ON).
    auto renderMesh = [&](size_t mi, bool doBlend) {
        MeshData& mesh = meshes[mi];
        if (shader) shader->setUniform("uInteriorOutsideVisible",
            (int32_t)(isInterior && mesh.interiorOutsideVisible ? 1 : 0));
        if (mi < skins.size() && skins[mi].hasSkin) {
            updateSkinnedMesh(mesh, skins[mi], nodeWorld, defaultTransforms);
            r.setModel(baseModel);
        } else {
            MatrixF fm = baseModel;
            if (mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeWorld.size())
                fm = baseModel * nodeWorld[mesh.nodeIndex];
            r.setModel(fm);
        }
        uint32_t flags = 0;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialTextures.size()) {
            Texture* texOverride = cloakTextureOverride;
            auto& tex = texOverride ? *texOverride : materialTextures[mesh.materialIndex];
            if (tex.loaded) {
                tex.bind(0);
                if (shader) shader->setUniform("uTexture", (int32_t)0);
                if (shader) shader->setUniform("uUseTexture", (int32_t)1);
                if (getenv("TORCH_TEXDIAG") && materialLightmapIndex.size() == 102)
                    Console::instance().printf(LogLevel::Debug,
                        "TEXBIND mesh=%d matIdx=%d matIdx2=%d texId=%u %dx%d "
                        "matLMIndex_size=%zu matLMIndex[%d]=%d lmIdx=%d name='%s'",
                        (int)mi, (int)mesh.materialIndex, (int)mesh.materialIdx, tex.id,
                        tex.width, tex.height,
                        materialLightmapIndex.size(),
                        (int)mesh.materialIdx,
                        (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
                            ? materialLightmapIndex[mesh.materialIdx] : -999,
                        (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
                            ? materialLightmapIndex[mesh.materialIdx] : -1,
                        (mesh.materialIndex < (int)materialNames.size() ? materialNames[mesh.materialIndex].c_str() : "?"));
            } else {
                if (shader) shader->setUniform("uUseTexture", (int32_t)0);
            }
            if (mesh.materialIndex < (int)materialFlags.size())
                flags = materialFlags[mesh.materialIndex];
        } else {
            if (shader) shader->setUniform("uUseTexture", (int32_t)0);
        }

        // Alpha test only for materials with Translucent or Additive flags
        bool alphaTest = (flags & (MatFlag_Translucent | MatFlag_Additive)) != 0;
        if (getenv("TORCH_NO_ALPHATEST")) alphaTest = false; // diagnostic escape hatch
        if (shader) shader->setUniform("uAlphaTest", (int32_t)alphaTest);
        if (shader) shader->setUniform("uAlphaTestThreshold", materialAlphaTestThreshold(flags));

        int lmIdx = (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
            ? materialLightmapIndex[mesh.materialIdx] : -1;
        if (getenv("TORCH_NOLMMAP")) lmIdx = -1; // diagnostic: disable lightmaps
        if (lmIdx >= 0 && lmIdx < (int)lightmaps.size() && lightmaps[lmIdx].loaded) {
            lightmaps[lmIdx].bind(1);
            if (shader) shader->setUniform("uLightmap", (int32_t)1);
            if (shader) shader->setUniform("uUseLightmap", (int32_t)1);
        } else {
            if (shader) shader->setUniform("uUseLightmap", (int32_t)0);
        }

        // T2 fadeSet: all materials get translucent blending
        if (flags & MatFlag_Additive) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
        } else if (doBlend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        if (shader) shader->setUniform("uSelfIlluminated", (int32_t)((flags & MatFlag_SelfIlluminating) ? 1 : 0));

        float metallic = 0.0f, roughness = 0.5f;
        if (isInterior) roughness = 1.0f;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialMetallic.size()) {
            metallic = materialMetallic[mesh.materialIndex];
            roughness = materialRoughness[mesh.materialIndex];
        }
        if (shader) shader->setUniform("uMetallic", metallic);
        if (shader) shader->setUniform("uRoughness", roughness);
        float reflectionAmount = 0.0f;
        if (materialReflectionAmount.empty())
            reflectionAmount = 1.0f;
        else if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialReflectionAmount.size())
            reflectionAmount = materialReflectionAmount[mesh.materialIndex];
        if (shader) shader->setUniform("uReflectionAmount", reflectionAmount);

        bool useEnvMap = false;
        auto& ren = Engine::instance().renderer();
        // Native DTS materials use the sky sphere map independently of the
        // PBR metallic placeholder; the asset-side opt-out is NeverEnvMap.
        if (!isInterior && ren.sky && ren.sky->emap.loaded && reflectionAmount > 0.0f &&
            !(flags & MatFlag_NeverEnvMap))
            useEnvMap = true;
        if (shader) shader->setUniform("uUseEnvMap", (int32_t)(useEnvMap ? 1 : 0));

        mesh.render();
    };

    // Classify meshes by material flags (Translucent/Additive → translucent pass)
    auto needsTranslucent = [&](size_t mi) -> bool {
        if (mi >= meshes.size()) return false;
        int32_t matIdx = meshes[mi].materialIndex;
        if (matIdx >= 0 && matIdx < (int)materialFlags.size())
            return (materialFlags[matIdx] & (MatFlag_Translucent | MatFlag_Additive)) != 0;
        return false;
    };
    auto needsAdditive = [&](size_t mi) -> bool {
        if (mi >= meshes.size()) return false;
        const int32_t matIdx = meshes[mi].materialIndex;
        return matIdx >= 0 && matIdx < (int)materialFlags.size() &&
            (materialFlags[matIdx] & MatFlag_Additive) != 0;
    };

    // Determine which meshes to render for the selected detail level
    std::vector<size_t> renderList;
    if (detailLevel >= 0 && detailLevel < (int)details.size() && !details[detailLevel].meshIndices.empty()) {
        renderList.reserve(details[detailLevel].meshIndices.size());
        for (int32_t mi : details[detailLevel].meshIndices) {
            if (mi >= 0 && mi < (int)meshes.size())
                renderList.push_back((size_t)mi);
        }
    } else {
        renderList.reserve(meshes.size());
        for (size_t mi = 0; mi < meshes.size(); mi++)
            renderList.push_back(mi);
    }

    // Pass 1: Opaque meshes
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (isInterior) glDepthFunc(GL_LEQUAL);
    for (size_t mi : renderList) {
        if (isInterior && !activeInteriorZones.empty() &&
            meshes[mi].interiorZone >= 0 &&
            (meshes[mi].interiorZone >= (int)activeInteriorZones.size() ||
             !activeInteriorZones[meshes[mi].interiorZone]))
            continue;
        if (!needsTranslucent(mi))
            renderMesh(mi, false);
    }

    // Pass 2: alpha-blended meshes. Additive materials are deferred so glow
    // cannot be written underneath ordinary translucent surfaces.
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    for (size_t mi : renderList) {
        if (isInterior && !activeInteriorZones.empty() &&
            meshes[mi].interiorZone >= 0 &&
            (meshes[mi].interiorZone >= (int)activeInteriorZones.size() ||
             !activeInteriorZones[meshes[mi].interiorZone]))
            continue;
        if (needsTranslucent(mi) && !needsAdditive(mi))
            renderMesh(mi, true);
    }

    // Pass 3: additive glow after alpha transparency.
    for (size_t mi : renderList) {
        if (isInterior && !activeInteriorZones.empty() &&
            meshes[mi].interiorZone >= 0 &&
            (meshes[mi].interiorZone >= (int)activeInteriorZones.size() ||
             !activeInteriorZones[meshes[mi].interiorZone]))
            continue;
        if (needsAdditive(mi))
            renderMesh(mi, true);
    }

    // Restore GL state
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
    } catch (const std::exception& e) {
        fprintf(stderr, "DBG DTSShape::render EXCEPTION: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "DBG DTSShape::render EXCEPTION: unknown\n");
    }
}

int DTSShape::findNode(const std::string& name) const {
    for (int i = 0; i < (int)nodes.size(); i++)
        if (nodes[i].name == name) return i;
    std::string wanted = name;
    for (char& c : wanted) c = (char)std::tolower((unsigned char)c);
    for (int i = 0; i < (int)nodes.size(); i++) {
        std::string candidate = nodes[i].name;
        for (char& c : candidate) c = (char)std::tolower((unsigned char)c);
        if (candidate == wanted) return i;
    }
    return -1;
}

void DTSShape::renderAnimation(const char* animName, float time,
                               const NodeOverride* overrides, int numOverrides) {
    if (!loaded) return;

    // An object can be hidden in one sample and visible in the next. Reset all
    // mutable mesh state before sampling so skipped meshes cannot retain a
    // prior frame or material-frame UV set.
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        meshes[mi].setFrame(0);
        if (mi < meshTVerts.size()) meshes[mi].remapUVs(0, meshTVerts[mi]);
    }

    // Find the animation
    const Animation* anim = nullptr;
    std::string wanted = animName ? animName : "";
    for (char& c : wanted) c = (char)std::tolower((unsigned char)c);
    for (auto& a : animations) {
        std::string candidate = a.name;
        for (char& c : candidate) c = (char)std::tolower((unsigned char)c);
        if (candidate == wanted) { anim = &a; break; }
    }
    if (!anim) { render(0, overrides, numOverrides); return; }

    const Animation* objectAnim = anim;

    // Wrap time for looping animations
    float t = time;
    if (anim->duration > 0.0f) {
        if (anim->looping) {
            t = fmodf(t, anim->duration);
            if (t < 0.0f) t += anim->duration;
        } else {
            t = std::max(0.0f, std::min(t, anim->duration));
        }
    }

    int32_t numNodes = (int32_t)nodes.size();
    if (numNodes <= 0) { render(0); return; }

    // ── Step 1: Per-node interpolated rotation & translation (from keyframes) ──
    std::vector<QuatF> nodeRot(numNodes, {0, 0, 0, 1});
    std::vector<Point3F> nodeTrans(numNodes, {0, 0, 0});
    std::vector<Point3F> nodeScale(numNodes, {1, 1, 1});
    std::vector<bool> rotSet(numNodes, false), transSet(numNodes, false), scaleSet(numNodes, false);

    // Group keyframes by node — keyframes are sorted by (nodeIndex, time) from dts_loader.
    // For each animated node, find bracketing keyframes and interpolate.
    size_t ki = 0;
    while (ki < anim->keyframes.size()) {
        int32_t ni = anim->keyframes[ki].nodeIndex;
        if (ni < 0 || ni >= numNodes) { ki++; continue; }

        // Collect all keyframes for this node (contiguous since sorted by nodeIndex)
        size_t start = ki;
        while (ki < anim->keyframes.size() && anim->keyframes[ki].nodeIndex == ni) ki++;
        size_t end = ki;

        // Find bracketing pair for rotation
        if (!rotSet[ni]) {
            const Keyframe* kf0 = nullptr;
            const Keyframe* kf1 = nullptr;
            for (size_t j = start; j < end; j++) {
                if (!anim->keyframes[j].hasRotation) continue;
                if (anim->keyframes[j].time <= t) {
                    if (!kf0 || anim->keyframes[j].time >= kf0->time) kf0 = &anim->keyframes[j];
                }
                if (anim->keyframes[j].time >= t) {
                    if (!kf1 || anim->keyframes[j].time <= kf1->time) kf1 = &anim->keyframes[j];
                }
            }
            if (kf0 && kf1) {
                if (kf0 == kf1 || std::abs(kf1->time - kf0->time) < 0.0001f) {
                    nodeRot[ni] = kf0->rotation;
                } else {
                    float alpha = (t - kf0->time) / (kf1->time - kf0->time);
                    nodeRot[ni] = Math::quatSlerp(kf0->rotation, kf1->rotation, alpha);
                }
            } else if (kf0) {
                nodeRot[ni] = kf0->rotation;
            }
            if (kf0) rotSet[ni] = true;
        }

        // Find bracketing pair for translation
        if (!transSet[ni]) {
            const Keyframe* kf0 = nullptr;
            const Keyframe* kf1 = nullptr;
            for (size_t j = start; j < end; j++) {
                if (!anim->keyframes[j].hasTranslation) continue;
                if (anim->keyframes[j].time <= t) {
                    if (!kf0 || anim->keyframes[j].time >= kf0->time) kf0 = &anim->keyframes[j];
                }
                if (anim->keyframes[j].time >= t) {
                    if (!kf1 || anim->keyframes[j].time <= kf1->time) kf1 = &anim->keyframes[j];
                }
            }
            if (kf0 && kf1) {
                if (kf0 == kf1 || std::abs(kf1->time - kf0->time) < 0.0001f) {
                    nodeTrans[ni] = kf0->translation;
                } else {
                    float alpha = (t - kf0->time) / (kf1->time - kf0->time);
                    nodeTrans[ni].x = kf0->translation.x + alpha * (kf1->translation.x - kf0->translation.x);
                    nodeTrans[ni].y = kf0->translation.y + alpha * (kf1->translation.y - kf0->translation.y);
                    nodeTrans[ni].z = kf0->translation.z + alpha * (kf1->translation.z - kf0->translation.z);
                }
            } else if (kf0) {
                nodeTrans[ni] = kf0->translation;
            }
            if (kf0) transSet[ni] = true;
        }

        // Find bracketing pair for scale
        if (!scaleSet[ni]) {
            const Keyframe* kf0 = nullptr;
            const Keyframe* kf1 = nullptr;
            for (size_t j = start; j < end; j++) {
                if (!anim->keyframes[j].hasScale) continue;
                if (anim->keyframes[j].time <= t) {
                    if (!kf0 || anim->keyframes[j].time >= kf0->time) kf0 = &anim->keyframes[j];
                }
                if (anim->keyframes[j].time >= t) {
                    if (!kf1 || anim->keyframes[j].time <= kf1->time) kf1 = &anim->keyframes[j];
                }
            }
            if (kf0 && kf1) {
                if (kf0 == kf1 || std::abs(kf1->time - kf0->time) < 0.0001f) {
                    nodeScale[ni] = kf0->scale;
                } else {
                    float alpha = (t - kf0->time) / (kf1->time - kf0->time);
                    nodeScale[ni].x = kf0->scale.x + alpha * (kf1->scale.x - kf0->scale.x);
                    nodeScale[ni].y = kf0->scale.y + alpha * (kf1->scale.y - kf0->scale.y);
                    nodeScale[ni].z = kf0->scale.z + alpha * (kf1->scale.z - kf0->scale.z);
                }
            } else if (kf0) {
                nodeScale[ni] = kf0->scale;
            }
            if (kf0) scaleSet[ni] = true;
        }
    }

    // ── Step 2: Fill in defaults for unanimated components from bind pose ──
    for (int32_t i = 0; i < numNodes; i++) {
        if (!rotSet[i]) {
            nodeRot[i] = objectAnim->blend
                ? QuatF{0, 0, 0, 1}
                : QuatF::fromMatrix(defaultLocalTransforms[i]);
        }
        if (!transSet[i]) {
            nodeTrans[i] = objectAnim->blend
                ? Point3F{0, 0, 0}
                : Point3F{defaultLocalTransforms[i].m[0][3],
                          defaultLocalTransforms[i].m[1][3],
                          defaultLocalTransforms[i].m[2][3]};
        }
        if (!scaleSet[i]) {
            nodeScale[i] = {1, 1, 1};
        }
    }

    // ── Step 3: Build local matrices and compose world transforms ──
    // T2: setMatrix(rot, trans, &local) then world[i] = world[parent] * local
    std::vector<MatrixF> nodeLocal(numNodes);
    std::vector<MatrixF> nodeWorld(numNodes);
    for (int32_t i = 0; i < numNodes; i++) {
        MatrixF local;
        if (rotSet[i] || transSet[i] || scaleSet[i]) {
            // At least one component was animated — build from interpolated values
            local = nodeRot[i].toMatrix();
            MatrixF scaleMat;
            scaleMat.identity();
            scaleMat.setScale(nodeScale[i]);
            local = local * scaleMat;
            local.m[0][3] = nodeTrans[i].x;
            local.m[1][3] = nodeTrans[i].y;
            local.m[2][3] = nodeTrans[i].z;
            local.m[3][3] = 1.0f;
            if (objectAnim->blend)
                local = defaultLocalTransforms[i] * local;
        } else {
            // Fully unanimated — use bind-pose local transform directly
            local = defaultLocalTransforms[i];
        }

        nodeLocal[i] = local;

        // Compose with parent
        int32_t pi = nodes[i].parentIndex;
        if (pi >= 0 && pi < numNodes)
            nodeWorld[i] = nodeWorld[pi] * local;
        else
            nodeWorld[i] = local;
    }

    // Overrides are supplied as world transforms. Convert each one back into
    // the node's local frame before composition so descendants inherit the
    // changed parent transform instead of remaining in the old world frame.
    if (overrides && numOverrides > 0) {
        for (int i = 0; i < numOverrides; i++) {
            const int32_t nodeIndex = overrides[i].nodeIndex;
            if (nodeIndex < 0 || nodeIndex >= numNodes) continue;
            const int32_t parentIndex = nodes[nodeIndex].parentIndex;
            if (parentIndex >= 0 && parentIndex < numNodes)
                nodeLocal[nodeIndex] = nodeWorld[parentIndex].inverse() * overrides[i].transform;
            else
                nodeLocal[nodeIndex] = overrides[i].transform;
        }
        for (int32_t i = 0; i < numNodes; i++) {
            const int32_t parentIndex = nodes[i].parentIndex;
            if (parentIndex >= 0 && parentIndex < numNodes)
                nodeWorld[i] = nodeWorld[parentIndex] * nodeLocal[i];
            else
                nodeWorld[i] = nodeLocal[i];
        }
    }

    // ── Step 4: Handle object-level vis/frame/matFrame animation ──
    std::vector<bool> objectVisible(objectStartMesh.size() > 0 ? objectStartMesh.size() : defaultTransforms.size(), true);
    std::vector<int32_t> objectFrame(objectVisible.size(), 0);
    std::vector<int32_t> objectMatFrame(objectVisible.size(), 0);
    if (!objectAnim->objectKeyframes.empty()) {
        const float objectTime = std::min(t, objectAnim->duration);
        for (size_t okfIdx = 0; okfIdx < objectAnim->objectKeyframes.size(); ) {
            const auto& okf = objectAnim->objectKeyframes[okfIdx];
            int32_t objIdx = okf.objectIndex;
            if (objIdx < 0 || objIdx >= (int32_t)objectVisible.size()) { okfIdx++; continue; }
            float lastVis = 1.0f;
            int32_t lastFrame = 0;
            int32_t lastMatFrame = 0;
            while (okfIdx < objectAnim->objectKeyframes.size() &&
                   objectAnim->objectKeyframes[okfIdx].objectIndex == objIdx) {
                if (objectAnim->objectKeyframes[okfIdx].time <= objectTime) {
                    const auto sample = sampleDTSObject(objectAnim->objectKeyframes, objIdx, objectTime);
                    lastVis = sample.vis;
                    lastFrame = sample.frameIndex;
                    lastMatFrame = sample.matFrameIndex;
                }
                okfIdx++;
            }
            objectVisible[objIdx] = (lastVis > 0.5f);
            objectFrame[objIdx] = lastFrame;
            objectMatFrame[objIdx] = lastMatFrame;
        }
    }

    // ── Step 5: Render with animated transforms ──
    auto* shader = ShaderManager::getDefaultShader();
    if (shader) shader->bind();
    auto& r = Engine::instance().renderer();
    if (!shader) return;
    shader->setUniform("uProjection", r.projection);
    shader->setUniform("uView", r.view);
    shader->setUniform("uCamPos", r.cameraPos);
    shader->setUniform("uInterior", (int32_t)(isInterior ? 1 : 0));
    if (shader) shader->setUniform("uShadowStrength", r.shadowsActive ? 0.6f : 0.0f);

    const MatrixF baseModel = r.modelMatrix();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Classify meshes by material flags (same as render())
    auto needsTranslucent = [&](size_t mi) -> bool {
        if (mi >= meshes.size()) return false;
        int32_t matIdx = meshes[mi].materialIndex;
        if (matIdx >= 0 && matIdx < (int)materialFlags.size())
            return (materialFlags[matIdx] & (MatFlag_Translucent | MatFlag_Additive)) != 0;
        return false;
    };
    auto needsAdditive = [&](size_t mi) -> bool {
        if (mi >= meshes.size()) return false;
        const int32_t matIdx = meshes[mi].materialIndex;
        return matIdx >= 0 && matIdx < (int)materialFlags.size() &&
            (materialFlags[matIdx] & MatFlag_Additive) != 0;
    };

    // Pre-setup: determine mesh visibility, apply matFrame UVs, apply skinning
    // Two-pass render: opaque first (depth writes ON), then translucent (blending ON)
    auto renderAnimMesh = [&](size_t mi, bool doBlend) {
        MeshData& mesh = meshes[mi];
        int32_t frame = 0;
        for (size_t oi = 0; oi < objectStartMesh.size(); ++oi)
            if (mi >= (size_t)objectStartMesh[oi] &&
                mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                if (oi < objectFrame.size()) frame = objectFrame[oi];
                break;
            }
        mesh.setFrame(frame);
        if (shader) shader->setUniform("uInteriorOutsideVisible",
            (int32_t)(isInterior && mesh.interiorOutsideVisible ? 1 : 0));
        // Apply skinned mesh deformation if needed
        if (mi < skins.size() && skins[mi].hasSkin) {
            updateSkinnedMesh(mesh, skins[mi], nodeWorld, defaultTransforms);
            r.setModel(baseModel);
        } else {
            MatrixF fm = baseModel;
            if (mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeWorld.size())
                fm = baseModel * nodeWorld[mesh.nodeIndex];
            r.setModel(fm);
        }

        // Bind texture and set material properties
        uint32_t flags = 0;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialTextures.size()) {
            Texture* texOverride = cloakTextureOverride;
            auto& tex = texOverride ? *texOverride : materialTextures[mesh.materialIndex];
            if (tex.loaded) {
                tex.bind(0);
                if (shader) shader->setUniform("uTexture", (int32_t)0);
                if (shader) shader->setUniform("uUseTexture", (int32_t)1);
                if (getenv("TORCH_TEXDIAG") && materialLightmapIndex.size() == 102)
                    Console::instance().printf(LogLevel::Debug,
                        "TEXBIND mesh=%d matIdx=%d matIdx2=%d texId=%u %dx%d "
                        "matLMIndex_size=%zu matLMIndex[%d]=%d lmIdx=%d name='%s'",
                        (int)mi, (int)mesh.materialIndex, (int)mesh.materialIdx, tex.id,
                        tex.width, tex.height,
                        materialLightmapIndex.size(),
                        (int)mesh.materialIdx,
                        (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
                            ? materialLightmapIndex[mesh.materialIdx] : -999,
                        (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
                            ? materialLightmapIndex[mesh.materialIdx] : -1,
                        (mesh.materialIndex < (int)materialNames.size() ? materialNames[mesh.materialIndex].c_str() : "?"));
            } else {
                if (shader) shader->setUniform("uUseTexture", (int32_t)0);
            }
            if (mesh.materialIndex < (int)materialFlags.size())
                flags = materialFlags[mesh.materialIndex];
        } else {
            if (shader) shader->setUniform("uUseTexture", (int32_t)0);
        }

        // Alpha test only for materials with Translucent or Additive flags
        bool alphaTest = (flags & (MatFlag_Translucent | MatFlag_Additive)) != 0;
        if (shader) shader->setUniform("uAlphaTest", (int32_t)alphaTest);
        if (shader) shader->setUniform("uAlphaTestThreshold", materialAlphaTestThreshold(flags));

        // Lightmap
        int lmIdx = (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
            ? materialLightmapIndex[mesh.materialIdx] : -1;
        if (getenv("TORCH_NOLMMAP")) lmIdx = -1; // diagnostic: disable lightmaps
        if (lmIdx >= 0 && lmIdx < (int)lightmaps.size() && lightmaps[lmIdx].loaded) {
            lightmaps[lmIdx].bind(1);
            if (shader) shader->setUniform("uLightmap", (int32_t)1);
            if (shader) shader->setUniform("uUseLightmap", (int32_t)1);
        } else {
            if (shader) shader->setUniform("uUseLightmap", (int32_t)0);
        }

        // T2 fadeSet: all materials get translucent blending
        if (flags & MatFlag_Additive) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
        } else if (doBlend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        if (shader) shader->setUniform("uSelfIlluminated", (int32_t)((flags & MatFlag_SelfIlluminating) ? 1 : 0));

        float metallic = 0.0f, roughness = 0.5f;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialMetallic.size()) {
            metallic = materialMetallic[mesh.materialIndex];
            roughness = materialRoughness[mesh.materialIndex];
        }
        if (shader) shader->setUniform("uMetallic", metallic);
        if (shader) shader->setUniform("uRoughness", roughness);
        float reflectionAmount = 0.0f;
        if (materialReflectionAmount.empty())
            reflectionAmount = 1.0f;
        else if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialReflectionAmount.size())
            reflectionAmount = materialReflectionAmount[mesh.materialIndex];
        if (shader) shader->setUniform("uReflectionAmount", reflectionAmount);

        bool useEnvMap = false;
        auto& ren = Engine::instance().renderer();
        if (ren.sky && ren.sky->emap.loaded && reflectionAmount > 0.0f && !(flags & MatFlag_NeverEnvMap))
            useEnvMap = true;
        if (shader) shader->setUniform("uUseEnvMap", (int32_t)(useEnvMap ? 1 : 0));

        mesh.render();
    };

    // Determine which meshes to render for the selected detail level
    std::vector<size_t> renderList;
    {
        int32_t dl = 0; // default to highest detail
        if (dl >= 0 && dl < (int)details.size() && !details[dl].meshIndices.empty()) {
            renderList.reserve(details[dl].meshIndices.size());
            for (int32_t mi : details[dl].meshIndices) {
                if (mi >= 0 && mi < (int)meshes.size())
                    renderList.push_back((size_t)mi);
            }
        } else {
            renderList.reserve(meshes.size());
            for (size_t mi = 0; mi < meshes.size(); mi++)
                renderList.push_back(mi);
        }
    }

    // Pass 1: Opaque meshes
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    for (size_t mi : renderList) {
        // Check object visibility
        bool meshVisible = true;
        for (size_t oi = 0; oi < objectStartMesh.size(); oi++) {
            if (mi >= (size_t)objectStartMesh[oi] &&
                mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                if (oi < objectVisible.size())
                    meshVisible = objectVisible[oi];
                break;
            }
        }
        if (!meshVisible) continue;

        // Apply material frame animation
        {
            int32_t objForMesh = -1;
            for (size_t oi = 0; oi < objectStartMesh.size(); oi++) {
                if (mi >= (size_t)objectStartMesh[oi] &&
                    mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                    objForMesh = (int32_t)oi; break;
                }
            }
            int32_t mf = 0;
            if (objForMesh >= 0 && objForMesh < (int32_t)objectMatFrame.size())
                mf = objectMatFrame[objForMesh];
            if (mi < meshTVerts.size())
                meshes[mi].remapUVs(mf, meshTVerts[mi]);
        }

        if (!needsTranslucent(mi))
            renderAnimMesh(mi, false);
    }

    // Pass 2: Translucent meshes
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    for (size_t mi : renderList) {
        bool meshVisible = true;
        for (size_t oi = 0; oi < objectStartMesh.size(); oi++) {
            if (mi >= (size_t)objectStartMesh[oi] &&
                mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                if (oi < objectVisible.size())
                    meshVisible = objectVisible[oi];
                break;
            }
        }
        if (!meshVisible) continue;

        {
            int32_t objForMesh = -1;
            for (size_t oi = 0; oi < objectStartMesh.size(); oi++) {
                if (mi >= (size_t)objectStartMesh[oi] &&
                    mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                    objForMesh = (int32_t)oi; break;
                }
            }
            int32_t mf = 0;
            if (objForMesh >= 0 && objForMesh < (int32_t)objectMatFrame.size())
                mf = objectMatFrame[objForMesh];
            if (mi < meshTVerts.size())
                meshes[mi].remapUVs(mf, meshTVerts[mi]);
        }

        if (needsTranslucent(mi) && !needsAdditive(mi))
            renderAnimMesh(mi, true);
    }
    // Additive effects are the final transparent sub-pass.
    for (size_t mi : renderList) {
        bool meshVisible = true;
        for (size_t oi = 0; oi < objectStartMesh.size(); oi++) {
            if (mi >= (size_t)objectStartMesh[oi] &&
                mi < (size_t)(objectStartMesh[oi] + objectNumMeshes[oi])) {
                if (oi < objectVisible.size()) meshVisible = objectVisible[oi];
                break;
            }
        }
        if (!meshVisible || !needsAdditive(mi)) continue;
        renderAnimMesh(mi, true);
    }

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
}
