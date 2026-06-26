#include "render/renderer.h"
#include "render/shader.h"
#include "render/glb_loader.h"
#include "core/engine.h"
#include "stb_image.h"
#include <GL/glew.h>
#include <cstring>
#include <cctype>
#include <vector>
#include <cmath>

void TerrainBlock::generateMesh() {
    if (heights.empty()) return;

    auto& r = Engine::instance().renderer();

    int32_t gridRes = 64; // Render grid resolution
    float step = (float)size / (float)gridRes;

    std::vector<Vertex> verts;
    std::vector<uint32_t> idxs;

    for (int32_t z = 0; z < gridRes; z++) {
        for (int32_t x = 0; x < gridRes; x++) {
            float wx = (float)x * step - (float)size * 0.5f;
            float wz = (float)z * step - (float)size * 0.5f;

            int hx = (int)((float)x / (float)gridRes * size);
            int hz = (int)((float)z / (float)gridRes * size);
            hx = Math::clamp(hx, 0, (int)heights.size() / (int)size - 1);
            hz = Math::clamp(hz, 0, (int)heights.size() / (int)size - 1);
            float h = heights[hz * size + hx] * heightScale;

            // Simple normal from neighbors
            float hl = hz * size + Math::max(hx - 1, 0);
            float hr = hz * size + Math::min(hx + 1, size - 1);
            float hd = Math::max(hz - 1, 0) * size + hx;
            float hu = Math::min(hz + 1, size - 1) * size + hx;
            Point3F n = {
                heights[hl] - heights[hr],
                2.0f / step,
                heights[hd] - heights[hu]
            };
            float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (nl > 0) { n.x /= nl; n.y /= nl; n.z /= nl; }

            ColorF c = {0.5f + h / heightScale * 0.5f, 0.3f + h / heightScale * 0.3f, 0.1f, 1.0f};

            verts.push_back({{wx, h, wz}, n, {(float)x / gridRes, (float)z / gridRes}, c});
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
                heights[z * this->size + x] = std::sin(x * 0.05f) * std::cos(z * 0.05f) * 10.0f;
        generateMesh();
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

    // Skip lightmap (SIZE*SIZE bytes)
    pos += TERRAIN_SIZE * TERRAIN_SIZE;

    // Read texture names (8 entries)
    for (int i = 0; i < 8 && pos < size; i++) {
        uint8_t nameLen = data[pos++];
        if (nameLen > 0 && pos + nameLen <= size) {
            std::string texName((const char*)data + pos, nameLen);
            Console::instance().printf(LogLevel::Debug, "  terrain tex[%d]: %s", i, texName.c_str());
            pos += nameLen;
        }
    }

    generateMesh();
    loaded = true;
    return true;
}

void TerrainBlock::render(const Point3F& cameraPos) {
    auto* shader = ShaderManager::getTerrainShader();
    if (!shader) return;
    shader->bind();

    auto& renderer = Engine::instance().renderer();
    MatrixF model;
    shader->setUniform("uProjection", renderer.projection);
    shader->setUniform("uView", renderer.view);
    shader->setUniform("uModel", model);
    shader->setUniform("uLightDir", Point3F{0.5f, 0.8f, 0.6f});

    for (auto& mesh : meshes)
        mesh.render();
}

// Font
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
    ortho.m[1][1] = 2.0f / h;
    ortho.m[3][0] = -1.0f;
    ortho.m[3][1] = -1.0f;

    shader->setUniform("uProjection", ortho);
    shader->setUniform("uColor", Point3F{color.r, color.g, color.b});
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
            int w, h, ch;
            unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
            if (pixels) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                stbi_image_free(pixels);
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
    if (!loaded) return;

    auto* shader = ShaderManager::getSkyShader();
    if (!shader) return;
    shader->bind();

    glDepthFunc(GL_LEQUAL);
    shader->setUniform("uProjection", proj);
    shader->setUniform("uView", view);
    shader->setUniform("uSkybox", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glDepthFunc(GL_LESS);
}

bool DTSShape::loadGLB(const uint8_t* data, size_t size) {
    ::GLBMesh glb = ::loadGLB(data, size);
    if (glb.meshes.empty()) return false;
    meshes = std::move(glb.meshes);
    materialTextures = std::move(glb.textures);

    // Resolve textures from filesystem for materials with resource_path
    // but no embedded texture loaded (common for shapes)
    std::vector<int> matToTex(glb.materials.size(), -1);
    bool needsResolution = false;
    for (size_t i = 0; i < glb.materials.size(); i++) {
        if (!glb.materials[i].resourcePath.empty() &&
            (i >= materialTextures.size() || !materialTextures[i].loaded)) {
            needsResolution = true;
            break;
        }
    }

    if (needsResolution) {
        auto& fs = Engine::instance().fs();
        static const char* exts[] = {".png", ".bm8", ".jpg", ".gif", ".bmp"};

        for (size_t i = 0; i < glb.materials.size(); i++) {
            auto& mat = glb.materials[i];
            if (mat.resourcePath.empty()) continue;

            std::string texPath = "textures/" + mat.resourcePath;
            // Normalize to lowercase for filesystem lookup (VL2 archives are lowercase)
            for (auto& c : texPath) c = std::tolower(c);
            std::vector<uint8_t> texData;
            for (auto* ext : exts) {
                auto data = fs.read((texPath + ext).c_str());
                if (!data.empty()) {
                    texData = std::move(data);
                    break;
                }
            }

            if (!texData.empty()) {
                Texture tex;
                tex.load(texData.data(), texData.size());
                if (tex.loaded) {
                    matToTex[i] = (int)materialTextures.size();
                    materialTextures.push_back(std::move(tex));
                    Console::instance().printf(LogLevel::Debug, "  loaded texture: %s", texPath.c_str());
                }
            } else {
                Console::instance().printf(LogLevel::Debug, "  texture not found: %s", texPath.c_str());
            }
        }

        // Update mesh materialIndex to point into our new materialTextures
        for (auto& mesh : meshes) {
            if (mesh.materialIdx >= 0 && mesh.materialIdx < (int)matToTex.size()) {
                int newIdx = matToTex[mesh.materialIdx];
                if (newIdx >= 0) mesh.materialIndex = newIdx;
            }
        }
    }

    loaded = true;
    return true;
}

// DTS shape stub
bool DTSShape::load(const uint8_t* data, size_t size) {
    // DTS loading - full implementation would parse the shape format
    // For now create a placeholder
    Console::instance().printf(LogLevel::Debug, "DTS load: %s (%zu bytes)", name.c_str(), size);
    return true;
}

void DTSShape::render(int32_t detailLevel) {
    auto* shader = ShaderManager::getDefaultShader();
    for (auto& mesh : meshes) {
        if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)materialTextures.size()) {
            auto& tex = materialTextures[mesh.materialIndex];
            if (tex.loaded) {
                tex.bind(0);
                if (shader) shader->setUniform("uUseTexture", (int32_t)1);
            } else {
                if (shader) shader->setUniform("uUseTexture", (int32_t)0);
            }
        } else {
            if (shader) shader->setUniform("uUseTexture", (int32_t)0);
        }
        mesh.render();
    }
}
