#include "render/renderer.h"
#include "render/shader.h"
#include "render/glb_loader.h"
#include "render/dts_loader.h"
#include "render/dif_loader.h"
#include "core/engine.h"
#include "stb_image.h"
#include <GL/glew.h>
#include <cstring>
#include <cctype>
#include <vector>
#include <algorithm>
#include <cmath>
#include <algorithm>

float TerrainBlock::sampleHeight(float wx, float wz) const {
    float fx = (wx - worldOffset.x) / squareSize;
    float fz = (wz - worldOffset.z) / squareSize;
    int ix = (int)std::floor(fx);
    int iz = (int)std::floor(fz);
    float tx = fx - ix;
    float tz = fz - iz;
    ix = Math::clamp(ix, 0, size - 2);
    iz = Math::clamp(iz, 0, size - 2);
    tx = Math::clamp(tx, 0.0f, 1.0f);
    tz = Math::clamp(tz, 0.0f, 1.0f);
    float h00 = heights[iz * size + ix];
    float h10 = heights[iz * size + ix + 1];
    float h01 = heights[(iz + 1) * size + ix];
    float h11 = heights[(iz + 1) * size + ix + 1];
    float h0 = h00 + (h10 - h00) * tx;
    float h1 = h01 + (h11 - h01) * tx;
    return (h0 + (h1 - h0) * tz) * heightScale;
}

void TerrainBlock::generateMesh() {
    if (heights.empty()) return;

    int32_t gridRes = 128;
    float totalWorldSize = (float)size * squareSize;
    float step = totalWorldSize / (float)gridRes;

    std::vector<Vertex> verts;
    std::vector<uint32_t> idxs;

    for (int32_t z = 0; z < gridRes; z++) {
        for (int32_t x = 0; x < gridRes; x++) {
            float wx = (float)x * step + worldOffset.x;
            float wz = (float)z * step + worldOffset.z;
            float h = sampleHeight(wx, wz);

            float eps = 0.5f;
            float hxr = sampleHeight(wx + eps, wz);
            float hxl = sampleHeight(wx - eps, wz);
            float hzf = sampleHeight(wx, wz + eps);
            float hzb = sampleHeight(wx, wz - eps);
            Point3F n = { hxl - hxr, 2.0f * eps, hzb - hzf };
            float nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (nlen > 0) { n.x /= nlen; n.y /= nlen; n.z /= nlen; }

            verts.push_back({{wx, h, wz}, n, {(float)x / gridRes, (float)z / gridRes}, {0,0}, {1,1,1,1}});
            float hn = (h + 10.0f) / 40.0f;
            hn = std::max(0.0f, std::min(1.0f, hn));
            ColorF vc;
            if (hn < 0.3f) {
                float t = hn / 0.3f;
                vc = {0.2f + t * 0.2f, 0.4f + t * 0.3f, 0.1f + t * 0.1f, 1.0f};
            } else if (hn < 0.6f) {
                float t = (hn - 0.3f) / 0.3f;
                vc = {0.4f + t * 0.2f, 0.7f - t * 0.3f, 0.2f - t * 0.1f, 1.0f};
            } else {
                float t = (hn - 0.6f) / 0.4f;
                vc = {0.6f + t * 0.3f, 0.4f + t * 0.4f, 0.1f + t * 0.5f, 1.0f};
            }
            verts.back().color = vc;
        }
    }

    for (int32_t z = 0; z < gridRes - 1; z++) {
        for (int32_t x = 0; x < gridRes - 1; x++) {
            int idx = z * gridRes + x;
            idxs.push_back(idx);
            idxs.push_back(idx + gridRes);
            idxs.push_back(idx + 1);
            idxs.push_back(idx + 1);
            idxs.push_back(idx + gridRes);
            idxs.push_back(idx + gridRes + 1);
        }
    }

    MeshData mesh;
    mesh.vertices = std::move(verts);
    mesh.indices = std::move(idxs);
    mesh.upload();
    meshes.push_back(std::move(mesh));
}

bool TerrainBlock::load(const uint8_t* data, size_t size) {
    Console::instance().printf(LogLevel::Debug, "Terrain load: %zu bytes", size);
    if (!data || size < 4) {
        // Generate procedural terrain
        Console::instance().printf(LogLevel::Info, "Terrain: generating procedural terrain");
        heights.resize(size >= 4 ? reinterpret_cast<const uint32_t*>(data)[0] * reinterpret_cast<const uint32_t*>(data)[0] : 256 * 256, 0.0f);
        uint32_t s = (uint32_t)std::sqrt((float)heights.size());
        if (s > 0) this->size = s;
        for (int32_t z = 0; z < this->size; z++)
            for (int32_t x = 0; x < this->size; x++)
                heights[z * this->size + x] = (std::sin(x * 0.03f) * std::cos(z * 0.04f) * 20.0f
                    + std::sin(x * 0.07f + 1.3f) * std::cos(z * 0.08f + 0.7f) * 8.0f
                    + std::sin(x * 0.15f + 3.1f) * std::cos(z * 0.12f + 2.3f) * 3.0f);
        generateMesh();
        // Generate procedural splatmap for texture blending
        {
            int S = 128;
            std::vector<uint8_t> splatPixels(S * S * 4);
            for (int y = 0; y < S; y++) {
                for (int x = 0; x < S; x++) {
                    float fx = (float)x / S, fy = (float)y / S;
                    float n1 = sinf(fx * 12.0f + fy * 8.0f) * 0.5f + 0.5f;
                    float n2 = sinf(fx * 5.0f + fy * 15.0f + 1.3f) * 0.5f + 0.5f;
                    float n3 = sinf(fx * 20.0f - fy * 7.0f + 3.7f) * 0.5f + 0.5f;
                    float n4 = sinf(fx * 0.3f + fy * 0.7f) * 0.5f + 0.5f;
                    float total = n1 + n2 + n3 + n4;
                    if (total < 0.01f) total = 0.01f;
                    splatPixels[(y * S + x) * 4 + 0] = (uint8_t)(n1 / total * 255);
                    splatPixels[(y * S + x) * 4 + 1] = (uint8_t)(n2 / total * 255);
                    splatPixels[(y * S + x) * 4 + 2] = (uint8_t)(n3 / total * 255);
                    splatPixels[(y * S + x) * 4 + 3] = (uint8_t)(n4 / total * 255);
                }
            }
            splatMap.loadRaw(splatPixels.data(), S, S, 4);
            // Try loading some default terrain textures
            auto& fs = Engine::instance().fs();
            const char* texNames[] = {
                "textures/terrain/LushWorld.DirtMossy",
                "textures/terrain/LushWorld.Dirt",
                "textures/terrain/LushWorld.Grass",
                "textures/terrain/LushWorld.Rock",
            };
            for (int i = 0; i < 4; i++) {
                for (auto* ext : {".png", ".bm8", ".jpg"}) {
                    auto d = fs.read((std::string(texNames[i]) + ext).c_str());
                    if (!d.empty()) {
                        Texture t;
                        if (strcmp(ext, ".bm8") == 0) t.loadBM8(d.data(), d.size());
                        else t.load(d.data(), d.size());
                        if (t.loaded) { detailTextures.push_back(std::move(t)); break; }
                    }
                }
                if ((int)detailTextures.size() <= i) {
                    Texture white;
                    std::vector<uint8_t> whitePx(16, 200);
                    white.loadRaw(whitePx.data(), 2, 2, 4);
                    detailTextures.push_back(std::move(white));
                }
            }
        }
        loaded = true;
        return true;
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
                // Convert from T2 height units (usually 0-65535) to world units
                float h = (float)raw / 65535.0f * 200.0f - 100.0f;
                heights[z * TERRAIN_SIZE + x] = h;
                if (std::abs(h) > maxH) maxH = std::abs(h);
            }
        }
    }

    Console::instance().printf(LogLevel::Info, "Terrain: loaded .ter v%u, max height=%.1f", version, maxH);

    // Read lightmap (SIZE*SIZE bytes — single channel, white=lit, black=shadow)
    if (pos + TERRAIN_SIZE * TERRAIN_SIZE <= size) {
        std::vector<uint8_t> lmPixels(TERRAIN_SIZE * TERRAIN_SIZE * 4);
        for (uint32_t i = 0; i < TERRAIN_SIZE * TERRAIN_SIZE; i++) {
            uint8_t v = data[pos + i];
            lmPixels[i * 4 + 0] = v;
            lmPixels[i * 4 + 1] = v;
            lmPixels[i * 4 + 2] = v;
            lmPixels[i * 4 + 3] = 255;
        }
        lightmap.loadRaw(lmPixels.data(), TERRAIN_SIZE, TERRAIN_SIZE, 4);
        pos += TERRAIN_SIZE * TERRAIN_SIZE;
    }

    // Read texture names (8 entries)
    textureNames.clear();
    int nonEmptyCount = 0;
    for (int i = 0; i < 8 && pos < size; i++) {
        uint8_t nameLen = data[pos++];
        std::string texName;
        if (nameLen > 0 && pos + nameLen <= size) {
            texName = std::string((const char*)data + pos, nameLen);
            pos += nameLen;
        }
        textureNames.push_back(texName);
        if (!texName.empty() && i < 6) nonEmptyCount++;
    }

    // Read alpha maps (nonEmptyCount × 256 × 256 bytes)
    // Build RGBA splat texture from first 4 alpha channels
    const uint32_t S = TERRAIN_SIZE;
    std::vector<uint8_t> splatPixels(S * S * 4, 0);
    for (int layer = 0; layer < nonEmptyCount && layer < 4 && pos + S * S <= size; layer++) {
        for (uint32_t z = 0; z < S; z++) {
            for (uint32_t x = 0; x < S; x++) {
                uint8_t alpha = data[pos + z * S + x];
                splatPixels[(z * S + x) * 4 + layer] = alpha;
            }
        }
        pos += S * S;
    }
    // Ensure at least one layer has full weight where all are 0
    {
        bool anyNonZero = false;
        for (size_t i = 0; i < S * S * 4; i++) if (splatPixels[i] > 0) { anyNonZero = true; break; }
        if (!anyNonZero && nonEmptyCount > 0) {
            for (uint32_t i = 0; i < S * S; i++) splatPixels[i * 4] = 255;
        }
    }
    splatMap.loadRaw(splatPixels.data(), S, S, 4);

    // Load detail textures from filesystem
    auto& fs = Engine::instance().fs();
    static const char* exts[] = {".png", ".bm8", ".jpg", ".gif", ".bmp"};
    for (int i = 0; i < nonEmptyCount && i < 4; i++) {
        Texture tex;
        // Convert terrain.X.Y.Z → textures/terrain/X.Y.Z
        std::string search = textureNames[i];
        if (search.compare(0, 8, "terrain.") == 0)
            search = "textures/terrain/" + search.substr(8);
        else
            search = "textures/" + search;
        // Try original case first, then lowercase
        for (auto* ext : exts) {
            auto d = fs.read((search + ext).c_str());
            if (!d.empty()) {
                if (std::strcmp(ext, ".bm8") == 0)
                    tex.loadBM8(d.data(), d.size());
                else
                    tex.load(d.data(), d.size());
                break;
            }
        }
        if (!tex.loaded) {
            std::string lower = search;
            for (auto& c : lower) c = std::tolower(c);
            for (auto* ext : exts) {
                auto d = fs.read((lower + ext).c_str());
                if (!d.empty()) {
                    if (std::strcmp(ext, ".bm8") == 0)
                        tex.loadBM8(d.data(), d.size());
                    else
                        tex.load(d.data(), d.size());
                    break;
                }
            }
        }
        if (tex.loaded)
            Console::instance().printf(LogLevel::Debug, "  terrain tex loaded: %s", search.c_str());
        else
            Console::instance().printf(LogLevel::Debug, "  terrain tex not found: %s", search.c_str());
        detailTextures.push_back(std::move(tex));
    }
    // Pad with white textures if less than 4 layers
    while (detailTextures.size() < 4) {
        Texture white;
        std::vector<uint8_t> whitePx(4 * 4 * 4, 255);
        white.loadRaw(whitePx.data(), 4, 4, 4);
        detailTextures.push_back(std::move(white));
    }

    generateMesh();
    loaded = true;
    return true;
}

void TerrainBlock::render(const Point3F& cameraPos, bool fogEnabled, const ColorF& fogColor, float fogDensity, const Point3F* lightDir) {
    auto* shader = ShaderManager::getTerrainShader();
    if (!shader) return;
    shader->bind();

    // Apply dynamic light direction if provided
    if (lightDir) {
        shader->setUniform("uLightDir", *lightDir);
    }

    if (splatMap.loaded) splatMap.bind(0);
    if (detailTextures.size() >= 1 && detailTextures[0].loaded) detailTextures[0].bind(1);
    if (detailTextures.size() >= 2 && detailTextures[1].loaded) detailTextures[1].bind(2);
    if (detailTextures.size() >= 3 && detailTextures[2].loaded) detailTextures[2].bind(3);
    if (detailTextures.size() >= 4 && detailTextures[3].loaded) detailTextures[3].bind(4);
    shader->setUniform("uSplatMap", (int32_t)0);
    shader->setUniform("uDetail0", (int32_t)1);
    shader->setUniform("uDetail1", (int32_t)2);
    shader->setUniform("uDetail2", (int32_t)3);
    shader->setUniform("uDetail3", (int32_t)4);

    if (lightmap.loaded) {
        lightmap.bind(5);
        shader->setUniform("uLightmap", (int32_t)5);
        shader->setUniform("uUseLightmap", (int32_t)1);
    } else {
        shader->setUniform("uUseLightmap", (int32_t)0);
    }

    // Use vertex color when no detail textures (procedural terrain)
    bool hasDetails = splatMap.loaded && detailTextures.size() >= 1 && detailTextures[0].loaded;
    shader->setUniform("uUseVertexColor", (int32_t)(hasDetails ? 0 : 1));

    auto& renderer = Engine::instance().renderer();
    MatrixF model;
    shader->setUniform("uProjection", renderer.projection);
    shader->setUniform("uView", renderer.view);
    shader->setUniform("uModel", model);
    shader->setUniform("uLightDir", Point3F{0.5f, 0.8f, 0.6f});
    shader->setUniform("uCamPos", cameraPos);
    shader->setUniform("uFogEnabled", (int32_t)(fogEnabled ? 1 : 0));
    if (fogEnabled) {
        shader->setUniform("uFogColor", Point3F{fogColor.r, fogColor.g, fogColor.b});
        shader->setUniform("uFogDensity", fogDensity);
    }

    for (auto& mesh : meshes)
        mesh.render();
}

// Font
#include "render/font8x8.h"

bool Font::loadDefault() {
    static const int srcSize = 8;
    static const int scale = 3; // scale up 3x for readability
    static const int cw = srcSize * scale, ch = srcSize * scale;
    static const int cols = 16, rows = 16;
    int tw = cols * cw, th = rows * ch;
    std::vector<uint8_t> pixels(tw * th * 4, 0);

    // Render ASCII 32-126 into the font texture (scaled up)
    for (int i = 32; i <= 126; i++) {
        int idx = i - 32;
        int cx = (i % cols) * cw;
        int cy = (i / cols) * ch;
        for (int py = 0; py < srcSize && idx < 95; py++) {
            uint8_t row = font8x8_basic[idx][py];
            for (int px = 0; px < srcSize; px++) {
                if (row & (0x80 >> px)) {
                    // Fill scaled block
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int pi = ((cy + py * scale + sy) * tw + (cx + px * scale + sx)) * 4;
                            pixels[pi + 0] = 255;
                            pixels[pi + 1] = 255;
                            pixels[pi + 2] = 255;
                            pixels[pi + 3] = 255;
                        }
                }
            }
        }
    }

    texWidth = tw;
    texHeight = th;
    charWidth = cw;
    charHeight = ch;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    float cw_f = 1.0f / cols, ch_f = 1.0f / rows;
    for (int i = 0; i < 256; i++) {
        int x = i % cols, y = i / cols;
        charUV[i][0] = x * cw_f;
        charUV[i][1] = y * ch_f;
        charUV[i][2] = (x + 1) * cw_f;
        charUV[i][3] = (y + 1) * ch_f;
    }

    // Flip the pixel buffer vertically so row 0 (atlas top) maps to v=1 (texture top)
    std::vector<uint8_t> flipped(tw * th * 4);
    for (int row = 0; row < th; row++)
        memcpy(&flipped[row * tw * 4], &pixels[(th - 1 - row) * tw * 4], tw * 4);
    pixels = std::move(flipped);

    // Flip UVs to match the flipped buffer: row y → v = (rows-1-y)/rows
    for (int i = 0; i < 256; i++) {
        int y = i / cols;
        charUV[i][1] = (rows - 1 - y) * ch_f;
        charUV[i][3] = (rows - y) * ch_f;
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

void Font::render(const char* text, float x, float y, const ColorF& color, float scale) {
    if (!loaded || !text) return;

    auto* shader = ShaderManager::getTextShader();
    if (!shader) return;
    shader->bind();

    auto& eng = Engine::instance();
    auto w = (float)eng.platform().width();
    auto h = (float)eng.platform().height();

    MatrixF ortho;
    ortho.identity();
    ortho.m[0][0] = 2.0f / w;
    ortho.m[1][1] = -2.0f / h;  // flip Y: screen y=0 (top) → NDC y=1 (top)
    ortho.m[3][0] = -1.0f;
    ortho.m[3][1] = 1.0f;

    shader->setUniform("uProjection", ortho);
    shader->setUniform("uColor", color);
    shader->setUniform("uTexture", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    float cw = charWidth * scale;
    float ch = charHeight * scale;

    std::vector<float> verts;
    std::vector<float> uvs;
    std::vector<uint32_t> idxs;

    float penX = x;
    float penY = y;

    for (const char* p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            penX = x;
            penY += ch;
            continue;
        }

        float l = penX;
        float r = penX + cw;
        float t = penY;
        float b = penY + ch;

        verts.insert(verts.end(), {l, t, r, t, l, b, r, b});
        uvs.insert(uvs.end(), {
            charUV[c][0], charUV[c][1],
            charUV[c][2], charUV[c][1],
            charUV[c][0], charUV[c][3],
            charUV[c][2], charUV[c][3]
        });

        uint32_t base = (uint32_t)(verts.size() / 2 - 4);
        idxs.insert(idxs.end(), {base, base+1, base+2, base+1, base+3, base+2});
        penX += cw;
    }

    if (verts.empty()) return;

    uint32_t vao, vbo, uvbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &uvbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, uvbo);
    glBufferData(GL_ARRAY_BUFFER, uvs.size() * sizeof(float), uvs.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idxs.size() * sizeof(uint32_t), idxs.data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, (GLsizei)idxs.size(), GL_UNSIGNED_INT, 0);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &uvbo);
    glDeleteBuffers(1, &ebo);
}

Point2F Font::measure(const char* text, float scale) {
    Point2F result;
    result.x = (float)strlen(text) * charWidth * scale;
    result.y = charHeight * scale;
    return result;
}

// Sky
void Sky::load(const std::vector<std::string>& faces) {
    glGenTextures(1, &cubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);

    for (int i = 0; i < 6 && i < (int)faces.size(); i++) {
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
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                if (!isBM8) stbi_image_free(pixels);
            }
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Skybox cube
    float skyVerts[] = {
        -1,-1,-1, 1,-1,-1, 1, 1,-1, -1, 1,-1,
        -1,-1, 1, 1,-1, 1, 1, 1, 1, -1, 1, 1,
        -1,-1,-1, -1, 1,-1, -1, 1, 1, -1,-1, 1,
        1,-1,-1, 1, 1,-1, 1, 1, 1, 1,-1, 1,
        -1,-1,-1, -1,-1, 1, 1,-1, 1, 1,-1,-1,
        -1, 1,-1, -1, 1, 1, 1, 1, 1, 1, 1,-1
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

void Sky::render(const MatrixF& view, const MatrixF& proj) {
    auto* shader = ShaderManager::getSkyShader();
    if (!shader) return;
    shader->bind();

    glDepthFunc(GL_LEQUAL);
    shader->setUniform("uProjection", proj);
    shader->setUniform("uView", view);

    if (loaded && emap.loaded) {
        // Cubemap sky
        shader->setUniform("uUseGradient", (int32_t)0);
        shader->setUniform("uSkybox", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    } else {
        // Gradient sky fallback (no cubemap available)
        shader->setUniform("uUseGradient", (int32_t)1);
        shader->setUniform("uGradTop", Point3F{0.2f, 0.4f, 0.7f});
        shader->setUniform("uGradBot", Point3F{0.75f, 0.8f, 0.85f});
    }

    // Ensure VAO exists (create on first render if needed)
    if (!vao) {
        float skyVerts[] = {
            -1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
            -1,-1,1, 1,-1,1, 1,1,1, -1,1,1,
            -1,-1,-1, -1,1,-1, -1,1,1, -1,-1,1,
            1,-1,-1, 1,1,-1, 1,1,1, 1,-1,1,
            -1,-1,-1, -1,-1,1, 1,-1,1, 1,-1,-1,
            -1,1,-1, -1,1,1, 1,1,1, 1,1,-1,
        };
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
    }

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glDepthFunc(GL_LESS);
}

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

    loaded = true;
    return true;
}

// DTS/DIF shape loader — parses Torque DTS binary or DIF interior format
bool DTSShape::load(const uint8_t* data, size_t size) {
    if (!data || size < 12) return false;

    // Check if this is actually a GLB file
    uint32_t magic = *(const uint32_t*)data;
    if (magic == 0x46546C67) {
        Console::instance().printf(LogLevel::Debug, "  detected GLB format, using GLB loader");
        return loadGLB(data, size);
    }

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
            isInterior = true;
            loaded = true;
            return true;
        }
    }

    // Try native DTS loading
    DTSLoadResult dtsResult = loadDTS(data, size, name.c_str());
    if (dtsResult.loaded) {
        meshes = std::move(dtsResult.meshes);
        materialTextures = std::move(dtsResult.textures);
        materialFlags = std::move(dtsResult.materialFlags);
        lightmaps = std::move(dtsResult.lightmaps);
        materialLightmapIndex = std::move(dtsResult.materialLightmapIndex);
        materialNames = std::move(dtsResult.materialNames);
        details = dtsResult.details;
        animations = dtsResult.animations;
        nodes = dtsResult.nodes;

        if (details.empty()) {
            DTSShape::DetailLevel dl;
            dl.size = 1000.0f;
            dl.meshIndex = 0;
            details.push_back(dl);
        }

        loaded = true;
        return true;
    }

    // Fallback to GLB
    return loadGLB(data, size);
}

bool DTSShape::applySkin(const std::string& skinName) {
    if (materialNames.empty() || materialTextures.empty() || skinName.empty()) return false;

    auto& fs = Engine::instance().fs();
    bool anyReplaced = false;
    static const char* exts[] = {".png", ".bm8", ".jpg", ".jpeg", ".gif", ".bmp", ".dds"};

    for (size_t i = 0; i < materialNames.size(); i++) {
        std::string matName = materialNames[i];
        if (matName.empty()) continue;

        // Strip extension for searching
        auto dot = matName.rfind('.');
        if (dot != std::string::npos) matName = matName.substr(0, dot);

        // Remove leading "textures/" prefix if present (it's added by texture search)
        if (matName.find("textures/") == 0) matName = matName.substr(9);

        // Try multiple candidate paths for the skin variant
        std::vector<std::string> candidates = {
            matName + "/" + skinName,                        // "skins/base/light_red"
            "skins/" + skinName + "/" + matName,             // "skins/light_red/skins/base"
            skinName + "/" + matName,                        // "light_red/skins/base"
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

void DTSShape::render(int32_t detailLevel) {
    auto* shader = ShaderManager::getDefaultShader();

    if (isInterior) {
        glCullFace(GL_FRONT);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    for (auto& mesh : meshes) {
        uint32_t flags = 0;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialTextures.size()) {
            auto& tex = materialTextures[mesh.materialIndex];
            if (tex.loaded) {
                tex.bind(0);
                if (shader) shader->setUniform("uUseTexture", (int32_t)1);
            } else {
                if (shader) shader->setUniform("uUseTexture", (int32_t)0);
            }
            if (mesh.materialIndex < (int)materialFlags.size())
                flags = materialFlags[mesh.materialIndex];
        } else {
            if (shader) shader->setUniform("uUseTexture", (int32_t)0);
        }

        // Bind lightmap if material uses one
        int lmIdx = (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
            ? materialLightmapIndex[mesh.materialIdx] : -1;
        if (lmIdx >= 0 && lmIdx < (int)lightmaps.size() && lightmaps[lmIdx].loaded) {
            lightmaps[lmIdx].bind(1);
            if (shader) shader->setUniform("uLightmap", (int32_t)1);
            if (shader) shader->setUniform("uUseLightmap", (int32_t)1);
        } else {
            if (shader) shader->setUniform("uUseLightmap", (int32_t)0);
        }

        // Set blend mode based on material flags
        if (flags & MatFlag_Additive) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
        } else if (flags & MatFlag_Translucent) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        if (shader) shader->setUniform("uSelfIlluminated", (int32_t)((flags & MatFlag_SelfIlluminating) ? 1 : 0));

        // Set PBR metallic/roughness per-material
        float metallic = 0.0f, roughness = 0.5f;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialMetallic.size()) {
            metallic = materialMetallic[mesh.materialIndex];
            roughness = materialRoughness[mesh.materialIndex];
        }
        if (shader) shader->setUniform("uMetallic", metallic);
        if (shader) shader->setUniform("uRoughness", roughness);

        // Enable env map for materials that don't have NeverEnvMap
        bool useEnvMap = false;
        auto& ren = Engine::instance().renderer();
        if (ren.sky && ren.sky->emap.loaded && !(flags & MatFlag_NeverEnvMap)) {
            useEnvMap = true;
        }
        if (shader) shader->setUniform("uUseEnvMap", (int32_t)(useEnvMap ? 1 : 0));

        mesh.render();
    }

    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
}

void DTSShape::renderAnimation(const char* animName, float time) {
    if (!loaded) return;

    auto& r = Engine::instance().renderer();
    auto* shader = ShaderManager::getDefaultShader();
    if (shader) shader->bind();

    // Find animation
    int animIndex = -1;
    for (size_t i = 0; i < animations.size(); i++) {
        if (animName && animations[i].name == animName) {
            animIndex = (int)i;
            break;
        }
    }

    size_t nodeCount = std::max(nodes.size(), (size_t)1);
    std::vector<MatrixF> nodeLocal(nodeCount);
    for (auto& m : nodeLocal) m.identity();

    if (animIndex >= 0 && !animations[animIndex].keyframes.empty()) {
        auto& anim = animations[animIndex];

        float t = time;
        if (anim.looping && anim.duration > 0)
            t = fmodf(time, anim.duration);
        else if (t > anim.duration)
            t = anim.duration;

        // For each node, find bracketing keyframes and interpolate
        for (size_t ni = 0; ni < nodeCount; ni++) {
            int kfA = -1, kfB = -1;
            for (size_t ki = 0; ki < anim.keyframes.size(); ki++) {
                auto& kf = anim.keyframes[ki];
                if (kf.nodeIndex != (int)ni) continue;
                if (kf.time <= t) {
                    if (kfA < 0 || kf.time > anim.keyframes[kfA].time)
                        kfA = (int)ki;
                }
                if (kf.time >= t) {
                    if (kfB < 0 || kf.time < anim.keyframes[kfB].time)
                        kfB = (int)ki;
                }
            }

            if (kfA < 0 && kfB < 0) continue;

            auto* ka = (kfA >= 0) ? &anim.keyframes[kfA] : nullptr;
            auto* kb = (kfB >= 0) ? &anim.keyframes[kfB] : nullptr;

            if (ka && kb && ka->time == kb->time) kb = nullptr;

            float lerpT = 0;
            if (ka && kb && kb->time != ka->time)
                lerpT = (t - ka->time) / (kb->time - ka->time);

            Point3F trans;
            if (ka && !kb) trans = ka->translation;
            else if (!ka && kb) trans = kb->translation;
            else trans = {
                Math::lerp(ka->translation.x, kb->translation.x, lerpT),
                Math::lerp(ka->translation.y, kb->translation.y, lerpT),
                Math::lerp(ka->translation.z, kb->translation.z, lerpT)
            };

            QuatF rot;
            if (ka && !kb) rot = ka->rotation;
            else if (!ka && kb) rot = kb->rotation;
            else {
                rot = Math::quatSlerp(ka->rotation, kb->rotation, lerpT);
            }

            Point3F scale;
            if (ka && !kb) scale = ka->scale;
            else if (!ka && kb) scale = kb->scale;
            else scale = {
                Math::lerp(ka->scale.x, kb->scale.x, lerpT),
                Math::lerp(ka->scale.y, kb->scale.y, lerpT),
                Math::lerp(ka->scale.z, kb->scale.z, lerpT)
            };

            MatrixF tMat, sMat;
            tMat.setTranslation(trans);
            sMat.setScale(scale);
            nodeLocal[ni] = tMat * rot.toMatrix() * sMat;
        }
    }

    // Build world transforms through hierarchy
    std::vector<MatrixF> nodeWorld(nodeCount);
    for (size_t ni = 0; ni < nodeCount; ni++) {
        int parent = (ni < nodes.size()) ? nodes[ni].parentIndex : -1;
        if (parent >= 0 && parent < (int)ni)
            nodeWorld[ni] = nodeWorld[parent] * nodeLocal[ni];
        else
            nodeWorld[ni] = nodeLocal[ni];
    }

    // Render meshes
    const MatrixF& baseModel = r.modelMatrix();

    if (isInterior) {
        glCullFace(GL_FRONT);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    for (auto& mesh : meshes) {
        // Compute final model matrix
        MatrixF finalModel = baseModel;
        if (mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeCount)
            finalModel = baseModel * nodeWorld[mesh.nodeIndex];

        r.setModel(finalModel);

        uint32_t flags = 0;
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialTextures.size()) {
            auto& tex = materialTextures[mesh.materialIndex];
            if (tex.loaded) {
                tex.bind(0);
                if (shader) shader->setUniform("uUseTexture", (int32_t)1);
            } else {
                if (shader) shader->setUniform("uUseTexture", (int32_t)0);
            }
            if (mesh.materialIndex < (int)materialFlags.size())
                flags = materialFlags[mesh.materialIndex];
        } else {
            if (shader) shader->setUniform("uUseTexture", (int32_t)0);
        }

        int lmIdx = (mesh.materialIdx >= 0 && mesh.materialIdx < (int)materialLightmapIndex.size())
            ? materialLightmapIndex[mesh.materialIdx] : -1;
        if (lmIdx >= 0 && lmIdx < (int)lightmaps.size() && lightmaps[lmIdx].loaded) {
            lightmaps[lmIdx].bind(1);
            if (shader) shader->setUniform("uLightmap", (int32_t)1);
            if (shader) shader->setUniform("uUseLightmap", (int32_t)1);
        } else {
            if (shader) shader->setUniform("uUseLightmap", (int32_t)0);
        }

        if (flags & MatFlag_Additive) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
        } else if (flags & MatFlag_Translucent) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        if (shader) shader->setUniform("uSelfIlluminated", (int32_t)((flags & MatFlag_SelfIlluminating) ? 1 : 0));

        {
            float m = 0.0f, r = 0.5f;
            if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialMetallic.size()) {
                m = materialMetallic[mesh.materialIndex];
                r = materialRoughness[mesh.materialIndex];
            }
            if (shader) shader->setUniform("uMetallic", m);
            if (shader) shader->setUniform("uRoughness", r);
        }

        bool useEnvMap = false;
        if (r.sky && r.sky->emap.loaded && !(flags & MatFlag_NeverEnvMap)) {
            useEnvMap = true;
        }
        if (shader) shader->setUniform("uUseEnvMap", (int32_t)(useEnvMap ? 1 : 0));

        mesh.render();
        r.stats.drawCalls++;
        r.stats.triangles += (int32_t)(mesh.indices.size() / 3);
    }

    // Restore base model
    r.setModel(baseModel);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
}
