#include "render/renderer.h"
#include "render/shader.h"
#include "core/engine.h"
#include "core/console.h"
#include "fs/file_system.h"
#include "stb_image.h"
#include <GL/glew.h>
#include <SDL3/SDL.h>
#include "../stb_image_write.h"
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>

struct Renderer::Impl {
    SDL_Window* window{};
    SDL_GLContext glContext{};
    std::unordered_map<std::string, Texture*> textures;
    std::vector<Shader*> shaders;
    bool glewInit = false;

    // Current state
    MatrixF projection;
    MatrixF view;
    MatrixF model;
};

Renderer::Renderer() : impl(new Impl) {}
Renderer::~Renderer() { delete impl; }

bool Renderer::init(void* window) {
    impl->window = (SDL_Window*)window;
    impl->glContext = SDL_GL_GetCurrentContext();

    if (!impl->glContext) {
        Console::instance().printf(LogLevel::Error, "No OpenGL context available");
        return false;
    }

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        Console::instance().printf(LogLevel::Error, "GLEW init failed: %s", glewGetErrorString(err));
        return false;
    }

    impl->glewInit = true;
    initialized = true;

    Console::instance().printf(LogLevel::Info, "Renderer: OpenGL %s, GLEW %s",
        glGetString(GL_VERSION), glewGetString(GLEW_VERSION));

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.3f, 0.5f, 0.8f, 1.0f);

    ShaderManager::init();
    defaultShader = ShaderManager::getDefaultShader();
    currentShader = defaultShader;

    initShadowMap();

    return true;
}

void Renderer::initShadowMap(int32_t size) {
    if (shadowFbo) {
        glDeleteFramebuffers(1, &shadowFbo);
        glDeleteTextures(1, &shadowDepthTex);
    }
    shadowSize = size;

    glGenTextures(1, &shadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Enable depth comparison for shadow sampling (PCF)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &shadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Console::instance().printf(LogLevel::Error, "Shadow FBO incomplete: %x", status);
        shadowSize = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Pre-compute bias matrix for NDC -> UV (0,1) mapping
    float bias[] = {
        0.5f, 0.0f, 0.0f, 0.5f,
        0.0f, 0.5f, 0.0f, 0.5f,
        0.0f, 0.0f, 0.5f, 0.5f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    memcpy((void*)shadowBiasMatrix.data(), bias, sizeof(bias));
}

void Renderer::beginShadowPass(const Point3F& lightDir, const Point3F& sceneCenter, float sceneRadius) {
    if (!shadowSize) return;

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
    glViewport(0, 0, shadowSize, shadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT); // Front-face culling for shadow volumes (reduces peter-panning)
    glDisable(GL_BLEND);

    // Orthographic projection from light direction
    Point3F up = {0, 1, 0};
    if (fabsf(lightDir.y) > 0.99f) up = {1, 0, 0};
    Point3F target = {sceneCenter.x + lightDir.x, sceneCenter.y + lightDir.y, sceneCenter.z + lightDir.z};

    float dist = sceneRadius * 1.5f;
    Point3F eye = {sceneCenter.x + lightDir.x * dist, sceneCenter.y + lightDir.y * dist, sceneCenter.z + lightDir.z * dist};
    MatrixF lightView;
    lightView.lookAt(eye, sceneCenter, up);

    MatrixF lightProj;
    lightProj.orthographic(-sceneRadius * 1.5f, sceneRadius * 1.5f,
                           -sceneRadius * 1.5f, sceneRadius * 1.5f,
                           0.1f, sceneRadius * 4.0f);

    shadowVP = lightProj * lightView;
    shadowBiasVP = shadowBiasMatrix * shadowVP;
}

void Renderer::endShadowPass() {
    if (!shadowSize) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, cfg.width, cfg.height);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
}

void Renderer::shutdown() {
    ShaderManager::destroy();
    for (auto& [k, v] : impl->textures) delete v;
    impl->textures.clear();
    initialized = false;
}

void Renderer::beginFrame(const ColorF& clearColor) {
    stats.drawCalls = 0;
    stats.triangles = 0;
    glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Renderer::endFrame() {
    glFlush();
}

void Renderer::setViewport(int32_t x, int32_t y, int32_t w, int32_t h) {
    glViewport(x, y, w, h);
}

void Renderer::setProjection(const MatrixF& proj) {
    impl->projection = proj;
    projection = proj;
    if (currentShader) {
        currentShader->bind();
        currentShader->setUniform("uProjection", proj);
    }
}

void Renderer::setView(const MatrixF& view) {
    impl->view = view;
    this->view = view;
    if (currentShader) {
        currentShader->bind();
        currentShader->setUniform("uView", view);
    }
}

void Renderer::setModel(const MatrixF& model) {
    impl->model = model;
    if (currentShader) {
        currentShader->bind();
        currentShader->setUniform("uModel", model);
    }
}

const MatrixF& Renderer::modelMatrix() const {
    return impl->model;
}

void Renderer::setCamera(const Point3F& pos, const Point3F& target, const Point3F& up) {
    cameraPos = pos;
    MatrixF v;
    v.lookAt(pos, target, up);
    setView(v);

    auto& cfg = config();
    MatrixF p;
    p.perspective(Math::DEG2RAD(cfg.fov), cfg.width / (float)cfg.height, cfg.nearPlane, cfg.farPlane);
    setProjection(p);
}

void Renderer::drawMesh(MeshData& mesh, const MatrixF& transform) {
    if (!mesh.uploaded) return;

    setModel(transform);
    mesh.render();
    stats.drawCalls++;
    stats.triangles += (int32_t)(mesh.indices.size() / 3);
}

void Renderer::drawLine(const Point3F& a, const Point3F& b, const ColorF& color) {
    auto* ls = ShaderManager::getLineShader();
    if (!ls) return;
    ls->bind();
    ls->setUniform("uProjection", projection);
    ls->setUniform("uView", view);
    ls->setUniform("uColor", Point3F{color.r, color.g, color.b});

    float verts[] = {a.x, a.y, a.z, b.x, b.y, b.z};
    uint32_t vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_LINES, 0, 2);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    stats.drawCalls++;
}

void Renderer::drawLineStrip(const std::vector<Point3F>& points, const ColorF& color) {
    if (points.size() < 2) return;
    auto* ls = ShaderManager::getLineShader();
    if (!ls) return;
    ls->bind();
    ls->setUniform("uProjection", projection);
    ls->setUniform("uView", view);
    ls->setUniform("uColor", Point3F{color.r, color.g, color.b});

    size_t vertCount = points.size();
    std::vector<float> verts;
    verts.reserve(vertCount * 3);
    for (auto& p : points) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z); }

    uint32_t vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)vertCount);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    stats.drawCalls++;
}

void Renderer::drawBox(const Box3F& box, const ColorF& color) {
    Point3F verts[8] = {
        {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z},
        {box.max.x, box.max.y, box.min.z}, {box.min.x, box.max.y, box.min.z},
        {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
        {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z}
    };
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    for (auto& e : edges)
        drawLine(verts[e[0]], verts[e[1]], color);
}

void Renderer::drawRectFill(const Point3F& a, const Point3F& b, const ColorF& color) {
    // Interleaved: pos.xy, uv (unused), color
    float verts[] = {
        a.x, a.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        b.x, a.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        a.x, b.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        b.x, b.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
    };
    uint32_t idxs[] = {0, 1, 2, 1, 3, 2};

    uint32_t vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // aPos (3 floats)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // aUV (2 floats)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    // aColor (4 floats)
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idxs), idxs, GL_STATIC_DRAW);

    auto* ss = ShaderManager::getSpriteShader();
    if (ss) {
        ss->bind();
        ss->setUniform("uProjection", projection);
        ss->setUniform("uView", view);
        ss->setUniform("uUseTexture", false);
    }

    glDisable(GL_CULL_FACE);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glEnable(GL_CULL_FACE);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);

    stats.drawCalls++;
}

void Renderer::drawTexturedRect(const Point3F& a, const Point3F& b, uint32_t texId) {
    float verts[] = {
        a.x, a.y, a.z,  0,0,  1,1,1,1,
        b.x, a.y, a.z,  1,0,  1,1,1,1,
        a.x, b.y, a.z,  0,1,  1,1,1,1,
        b.x, b.y, a.z,  1,1,  1,1,1,1,
    };
    uint32_t idxs[] = {0,1,2,1,3,2};
    uint32_t vao, vbo, ebo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(5*sizeof(float))); glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idxs), idxs, GL_STATIC_DRAW);
    auto* ss = ShaderManager::getSpriteShader();
    if (ss) {
        ss->bind();
        ss->setUniform("uProjection", projection);
        ss->setUniform("uView", view);
        ss->setUniform("uUseTexture", int32_t(1));
        ss->setUniform("uTexture", int32_t(0));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    glDisable(GL_CULL_FACE);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glEnable(GL_CULL_FACE);
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ebo);
    stats.drawCalls++;
}

void Renderer::drawTexturedRectUV(const Point3F& a, const Point3F& b, uint32_t texId, float u0, float v0, float u1, float v1) {
    float verts[] = {a.x,a.y,a.z, u0,v0, 1,1,1,1, b.x,a.y,a.z, u1,v0, 1,1,1,1, a.x,b.y,a.z, u0,v1, 1,1,1,1, b.x,b.y,a.z, u1,v1, 1,1,1,1};
    uint32_t idxs[] = {0,1,2,1,3,2};
    uint32_t vao, vbo, ebo;
    glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(5*sizeof(float))); glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(idxs),idxs,GL_STATIC_DRAW);
    auto* ss = ShaderManager::getSpriteShader();
    if (ss) { ss->bind(); ss->setUniform("uProjection",projection); ss->setUniform("uView",view);
        ss->setUniform("uUseTexture",int32_t(1)); ss->setUniform("uTexture",int32_t(0)); }
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texId);
    glDisable(GL_CULL_FACE); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0); glEnable(GL_CULL_FACE);
    glDeleteVertexArrays(1,&vao); glDeleteBuffers(1,&vbo); glDeleteBuffers(1,&ebo);
    stats.drawCalls++;
}

void Renderer::drawSprite(const Point3F& pos, float size, const ColorF& color, uint32_t texture) {
    auto* shader = ShaderManager::getSpriteShader();
    if (!shader) return;
    shader->bind();

    // Billboarding: extract right/up from view matrix
    const float* v = view.data();
    Point3F right = {v[0], v[4], v[8]};  // first column (transpose for row-major in memory)
    Point3F up = {v[1], v[5], v[9]};     // second column
    float s = size * 0.5f;

    struct SpriteVert { float x, y, z; float u, v; float r, g, b, a; };
    SpriteVert verts[4] = {
        {pos.x + (-right.x + up.x) * s, pos.y + (-right.y + up.y) * s, pos.z + (-right.z + up.z) * s, 0, 0, color.r, color.g, color.b, color.a},
        {pos.x + (right.x + up.x) * s,  pos.y + (right.y + up.y) * s,  pos.z + (right.z + up.z) * s,  1, 0, color.r, color.g, color.b, color.a},
        {pos.x + (right.x - up.x) * s,  pos.y + (right.y - up.y) * s,  pos.z + (right.z - up.z) * s,  1, 1, color.r, color.g, color.b, color.a},
        {pos.x + (-right.x - up.x) * s, pos.y + (-right.y - up.y) * s, pos.z + (-right.z - up.z) * s, 0, 1, color.r, color.g, color.b, color.a},
    };
    uint16_t idxs[6] = {0, 1, 2, 0, 2, 3};

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idxs), idxs, GL_STREAM_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)(5*sizeof(float)));
    glEnableVertexAttribArray(2);

    shader->setUniform("uProjection", projection);
    shader->setUniform("uUseTexture", (int32_t)(texture ? 1 : 0));
    if (texture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        shader->setUniform("uTexture", (int32_t)0);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    glDepthMask(GL_TRUE);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);

    stats.drawCalls++;
    stats.triangles += 2;
}

Texture* Renderer::loadTexture(const char* path) {
    auto it = impl->textures.find(path);
    if (it != impl->textures.end()) return it->second;

    auto data = Engine::instance().fs().read(path);
    if (data.empty()) {
        return nullptr;
    }

    auto* tex = new Texture;
    tex->load(data.data(), data.size());
    impl->textures[path] = tex;
    stats.textures++;
    Console::instance().printf(LogLevel::Debug, "Texture loaded: %s (%dx%d)", path, tex->width, tex->height);
    return tex;
}

Shader* Renderer::loadShader(const char* vertPath, const char* fragPath) {
    auto* s = new Shader;
    if (!s->loadFromFiles(vertPath, fragPath)) {
        delete s;
        return nullptr;
    }
    impl->shaders.push_back(s);
    return s;
}

void Renderer::addShader(Shader* shader) {
    impl->shaders.push_back(shader);
}

void Renderer::addTexture(Texture* tex) {
    static int counter = 0;
    char name[64];
    snprintf(name, sizeof(name), "__tex_%d", counter++);
    impl->textures[name] = tex;
}

void Renderer::renderText(const char* text, float x, float y, const ColorF& color, float scale) {
    if (defaultFont) defaultFont->render(text, x, y, color, scale);
}

void Renderer::onResize(int32_t w, int32_t h) {
    cfg.width = w;
    cfg.height = h;
    glViewport(0, 0, w, h);

    MatrixF p;
    p.perspective(Math::DEG2RAD(cfg.fov), w / (float)h, cfg.nearPlane, cfg.farPlane);
    setProjection(p);
}

// Mesh
void MeshData::upload() {
    if (uploaded) return;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv2));
    glEnableVertexAttribArray(4);

    uploaded = true;
}

void MeshData::render() {
    if (!uploaded) upload();
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, 0);
}

void MeshData::destroy() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    uploaded = false;
}

// Texture
void Texture::load(const uint8_t* data, size_t size) {
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) {
        // Try BM8 format (Tribes 2 proprietary)
        if (loadBM8(data, size)) return;
        // Generate magenta/black checkerboard as fallback
        const int cw = 32, ch = 32;
        std::vector<uint8_t> cb(cw * ch * 4);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                bool bright = ((x / 8) + (y / 8)) % 2 == 0;
                cb[(y * cw + x) * 4 + 0] = bright ? 255 : 0;
                cb[(y * cw + x) * 4 + 1] = 0;
                cb[(y * cw + x) * 4 + 2] = bright ? 255 : 0;
                cb[(y * cw + x) * 4 + 3] = 255;
            }
        loadRaw(cb.data(), cw, ch, 4);
        return;
    }
    loadRaw(pixels, w, h, 4);
    stbi_image_free(pixels);
}

bool Texture::decodeBM8(const uint8_t* data, size_t size,
                         std::vector<uint8_t>& outPixels,
                         int32_t& outW, int32_t& outH) {
    if (size < 32) return false;
    uint32_t hdr[8];
    memcpy(hdr, data, 32);
    uint32_t w = hdr[1], h = hdr[2], flags = hdr[4];
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    if (flags != 1 && flags < 3) return false;

    uint32_t paletteSize = 1024;
    if (flags != 1) paletteSize += (flags - 1) * 4;
    uint32_t pixelOffset = 32 + paletteSize;
    uint32_t pixelCount = w * h;
    if (pixelOffset + pixelCount > size) return false;

    uint8_t palette[1024];
    memcpy(palette, data + 32, 1024);

    outPixels.resize(pixelCount * 4);
    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t idx = data[pixelOffset + i];
        outPixels[i * 4 + 0] = palette[idx * 4 + 0];
        outPixels[i * 4 + 1] = palette[idx * 4 + 1];
        outPixels[i * 4 + 2] = palette[idx * 4 + 2];
        outPixels[i * 4 + 3] = palette[idx * 4 + 3];
    }
    outW = (int32_t)w;
    outH = (int32_t)h;
    return true;
}

bool Texture::loadBM8(const uint8_t* data, size_t size) {
    std::vector<uint8_t> rgba;
    int32_t w, h;
    if (!decodeBM8(data, size, rgba, w, h)) return false;
    loadRaw(rgba.data(), w, h, 4);
    return loaded;
}

void Texture::loadRaw(const uint8_t* pixels, int32_t w, int32_t h, int32_t channels) {
    if (!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    GLenum fmt = (channels == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    width = w; height = h;
    loaded = true;
}

void Texture::bind(int32_t unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id);
}

void Texture::destroy() {
    if (id) glDeleteTextures(1, &id);
    loaded = false;
}

// Shader
bool Shader::load(const char* vertSrc, const char* fragSrc) {
    auto compile = [](GLenum type, const char* src) -> uint32_t {
        uint32_t shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            Console::instance().printf(LogLevel::Error, "Shader compile error: %s", log);
            return 0;
        }
        return shader;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, vertSrc);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) return false;

    id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);

    GLint success;
    glGetProgramiv(id, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(id, sizeof(log), nullptr, log);
        Console::instance().printf(LogLevel::Error, "Shader link error: %s", log);
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    loaded = true;
    return true;
}

bool Shader::loadFromFiles(const char* vertPath, const char* fragPath) {
    auto readFile = [](const char* path) -> std::string {
        return Engine::instance().fs().readText(path);
    };
    auto vert = readFile(vertPath);
    auto frag = readFile(fragPath);
    if (vert.empty() || frag.empty()) return false;
    return load(vert.c_str(), frag.c_str());
}

void Shader::bind() { if (id) glUseProgram(id); }

GLint Shader::getUniformLoc(const char* name) {
    auto it = uniformCache.find(name);
    if (it != uniformCache.end()) return it->second;
    GLint loc = glGetUniformLocation(id, name);
    uniformCache[name] = loc;
    return loc;
}

void Shader::setUniform(const char* name, float v) {
    glUniform1f(getUniformLoc(name), v);
}

void Shader::setUniform(const char* name, const Point3F& v) {
    glUniform3f(getUniformLoc(name), v.x, v.y, v.z);
}

void Shader::setUniform(const char* name, const MatrixF& m) {
    glUniformMatrix4fv(getUniformLoc(name), 1, GL_FALSE, m.data());
}

void Shader::setUniform(const char* name, int32_t v) {
    glUniform1i(getUniformLoc(name), v);
}

void Shader::setUniform(const char* name, const ColorF& v) {
    glUniform4f(getUniformLoc(name), v.r, v.g, v.b, v.a);
}

void Shader::destroy() { if (id) glDeleteProgram(id); loaded = false; }

Font* Renderer::getFont(const char* name, int size) {
    std::string key = std::string(name) + "_" + std::to_string(size);
    auto it = fontCache.find(key);
    if (it != fontCache.end()) return it->second;
    return defaultFont;
}

void Renderer::addFont(Font* font) {
    if (!font || !font->loaded) return;
    std::string key = font->fontName + "_" + std::to_string(font->fontSize);
    fontCache[key] = font;
}

void Renderer::setFontScale(float scale) {
    for (auto& [key, ft] : fontCache) {
        if (ft) ft->defaultScale = scale;
    }
    if (defaultFont) defaultFont->defaultScale = scale;
}

bool Renderer::screenshot(const char* path) {
    int w = cfg.width, h = cfg.height;
    std::vector<uint8_t> pixels(w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path, w, h, 3, pixels.data(), w * 3) != 0;
}
