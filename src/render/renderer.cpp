#include "render/renderer.h"
#include "render/shader.h"
#include "core/engine.h"
#include "core/console.h"
#include "fs/file_system.h"
#include "stb_image.h"
#include <GL/glew.h>
#include <SDL3/SDL.h>
#include "../stb_image_write.h"
#include <zlib.h>
// Forward-declare stbi_write_png_to_mem (defined in stb_image_write_impl.cpp)
extern "C" unsigned char* stbi_write_png_to_mem(const unsigned char* pixels, int stride_bytes, int x, int y, int n, int* out_len);
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "render/texture_frames.h"
#include "render/render_order.h"
#include <cctype>
#include <cstdlib>
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

    Console::instance().printf(LogLevel::Info, "Renderer: OpenGL %s, GLEW %s, Renderer: %s",
        glGetString(GL_VERSION), glewGetString(GLEW_VERSION), glGetString(GL_RENDERER));

    // Capture driver info for getVideoDriverInfo() (VENDOR\RENDERER\VERSION\EXTENSIONS).
    // GL_EXTENSIONS (string form) is NULL on core profiles — enumerate via glGetStringi.
    auto safeGL = [](GLenum name, const char* fallback) -> const char* {
        const char* s = (const char*)glGetString(name);
        return s ? s : fallback;
    };
    std::string extList;
    GLint nExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &nExt);
    if (nExt > 0) {
        extList.reserve(4096);
        for (GLint i = 0; i < nExt; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, i);
            if (!e) continue;
            if (!extList.empty()) extList += ' ';
            extList += e;
        }
    }
    gpuInfo = std::string(safeGL(GL_VENDOR, "Unknown")) + "\t" +
              safeGL(GL_RENDERER, "Unknown") + "\t" +
              safeGL(GL_VERSION, "Unknown") + "\t" +
              extList;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.3f, 0.5f, 0.8f, 1.0f);

    ShaderManager::init();
    defaultShader = ShaderManager::getDefaultShader();
    currentShader = defaultShader;

    initShadowMap(cfg.shadowMapSize);

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
    cameraTarget = target;
    MatrixF v;
    v.lookAt(pos, target, up);
    setView(v);

    auto& cfg = config();
    MatrixF p;
    const float aspect = std::max(1, cfg.width) / (float)std::max(1, cfg.height);
    p.perspective(Math::DEG2RAD(Math::horizontalFovToVertical(cfg.fov, aspect)),
                  aspect, cfg.nearPlane, cfg.farPlane);
    setProjection(p);
}

void Renderer::setDynamicLights(const std::vector<DynamicPointLight>& lights) {
    dynamicLights.clear();
    dynamicLights.reserve(MAX_DYNAMIC_LIGHTS);
    for (const auto& input : lights) {
        if ((int)dynamicLights.size() >= std::min(MAX_DYNAMIC_LIGHTS, std::max(0, cfg.maxLights))) break;
        DynamicPointLight light = input;
        if (!std::isfinite(light.x) || !std::isfinite(light.y) || !std::isfinite(light.z) ||
            !std::isfinite(light.r) || !std::isfinite(light.g) || !std::isfinite(light.b) ||
            !std::isfinite(light.radius) || !std::isfinite(light.falloff) || light.radius <= 0.0f)
            continue;
        light.r = std::clamp(light.r, 0.0f, 16.0f);
        light.g = std::clamp(light.g, 0.0f, 16.0f);
        light.b = std::clamp(light.b, 0.0f, 16.0f);
        light.radius = std::clamp(light.radius, 0.05f, 256.0f);
        light.falloff = std::clamp(light.falloff, 0.1f, 8.0f);
        dynamicLights.push_back(light);
    }
    auto apply = [&](Shader* shader) {
        if (!shader) return;
        shader->bind();
        shader->setUniform("uPointLightCount", (int32_t)dynamicLights.size());
        for (int i = 0; i < MAX_DYNAMIC_LIGHTS; ++i) {
            const auto* light = i < (int)dynamicLights.size() ? &dynamicLights[i] : nullptr;
            shader->setUniform(("uPointLightPos[" + std::to_string(i) + "]").c_str(),
                light ? Point3F{light->x, light->y, light->z} : Point3F{});
            shader->setUniform(("uPointLightColor[" + std::to_string(i) + "]").c_str(),
                light ? Point3F{light->r, light->g, light->b} : Point3F{});
            shader->setUniform(("uPointLightParams[" + std::to_string(i) + "]").c_str(),
                light ? Point3F{light->radius, light->falloff, 0.0f} : Point3F{});
        }
    };
    apply(ShaderManager::getDefaultShader());
    apply(ShaderManager::getTerrainShader());
}

void Renderer::clearDynamicLights() { setDynamicLights({}); }

void Renderer::beginTransparentPass() {
    spriteBatchFlush();
    constexpr auto state = transparentPassState();
    if (state.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
    if (state.blending) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, state.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::endTransparentPass() {
    spriteBatchFlush();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDepthFunc(GL_LESS);
}

void Renderer::drawMesh(MeshData& mesh, const MatrixF& transform) {
    if (!mesh.uploaded) return;

    spriteBatchFlush();
    setModel(transform);
    mesh.render();
    stats.drawCalls++;
    stats.triangles += (int32_t)(mesh.indices.size() / 3);
}

void Renderer::drawLine(const Point3F& a, const Point3F& b, const ColorF& color) {
    spriteBatchFlush();
    auto* ls = ShaderManager::getLineShader();
    if (!ls) return;
    ls->bind();
    ls->setUniform("uProjection", projection);
    ls->setUniform("uView", view);
    ls->setUniform("uColor", color);

    if (!lineVAO) {
        glGenVertexArrays(1, &lineVAO);
        glGenBuffers(1, &lineVBO);
        glBindVertexArray(lineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
        glEnableVertexAttribArray(0);
    }

    float verts[] = {a.x, a.y, a.z, b.x, b.y, b.z};
    glBindVertexArray(lineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINES, 0, 2);

    stats.drawCalls++;
}

void Renderer::drawLineStrip(const std::vector<Point3F>& points, const ColorF& color) {
    if (points.size() < 2) return;
    spriteBatchFlush();
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

void Renderer::drawFilledQuad(float width, float depth) {
    spriteBatchFlush();
    static uint32_t vao = 0, vbo = 0;
    if (!vao) {
        float verts[] = {
            0, 0, 0,  0, 0,  width, 0, 0, 1, 0,  width, 0, depth, 1, 1,
            0, 0, 0,  0, 0,  width, 0, depth, 1, 1,  0, 0, depth, 0, 1
        };
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    stats.drawCalls++;
}

void Renderer::initSpriteVAO() {
    if (spriteVAO) return;
    glGenVertexArrays(1, &spriteVAO);
    glGenBuffers(1, &spriteVBO);
    glBindVertexArray(spriteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 54, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);
}

void Renderer::spriteBatchAdd(float* verts, uint32_t texId, bool additive) {
    const bool depthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool depthWrite = [&]() {
        GLboolean value = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &value);
        return value == GL_TRUE;
    }();
    const bool blend = glIsEnabled(GL_BLEND) == GL_TRUE;
    if ((texId != spriteBatchTex || additive != spriteBatchAdditive ||
         depthTest != spriteBatchDepthTest || depthWrite != spriteBatchDepthWrite ||
         blend != spriteBatchBlend) && spriteBatchCount > 0) {
        spriteBatchFlush();
    }
    spriteBatchTex = texId;
    spriteBatchAdditive = additive;
    spriteBatchDepthTest = depthTest;
    spriteBatchDepthWrite = depthWrite;
    spriteBatchBlend = blend;
    spriteBatchBuf.insert(spriteBatchBuf.end(), verts, verts + 54);
    spriteBatchCount++;
    if (spriteBatchCount >= SPRITE_BATCH_MAX) {
        spriteBatchFlush();
    }
}

void Renderer::spriteBatchFlush() {
    if (spriteBatchCount == 0) return;
    initSpriteVAO();

    auto* ss = ShaderManager::getSpriteShader();
    if (!ss) {
        spriteBatchBuf.clear();
        spriteBatchCount = 0;
        spriteBatchTex = UINT32_MAX;
        spriteBatchAdditive = false;
        return;
    }
    ss->bind();
    ss->setUniform("uProjection", projection);
    ss->setUniform("uView", view);
    if (spriteBatchTex == UINT32_MAX) {
        ss->setUniform("uUseTexture", false);
    } else {
        ss->setUniform("uUseTexture", int32_t(1));
        ss->setUniform("uTexture", int32_t(0));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, spriteBatchTex);
    }

    glBindVertexArray(spriteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVBO);
    glBufferData(GL_ARRAY_BUFFER, spriteBatchBuf.size() * sizeof(float), spriteBatchBuf.data(), GL_DYNAMIC_DRAW);

    // Quads are wound for front-facing with culling OFF. Preserve every state
    // changed here: effects and GUI use the same accumulator but different GL
    // depth/blend state.
    GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
    GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLboolean depthWriteWasOn = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasOn);
    GLint blendSrcRGB = GL_SRC_ALPHA, blendDstRGB = GL_ONE_MINUS_SRC_ALPHA;
    GLint blendSrcAlpha = GL_SRC_ALPHA, blendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glDisable(GL_CULL_FACE);
    if (spriteBatchDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (spriteBatchBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(spriteBatchDepthWrite ? GL_TRUE : GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, spriteBatchAdditive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(spriteBatchBuf.size() / 9));
    if (cullWasOn) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    if (depthWasOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(depthWriteWasOn);
    glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                        (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);

    stats.drawCalls++;
    stats.triangles += (int32_t)(spriteBatchBuf.size() / 27);
    spriteBatchBuf.clear();
    spriteBatchCount = 0;
    spriteBatchTex = UINT32_MAX;
    spriteBatchAdditive = false;
}

void Renderer::flushSpriteBatch() {
    spriteBatchFlush();
}

void Renderer::drawRectFill(const Point3F& a, const Point3F& b, const ColorF& color) {
    float verts[] = {
        a.x, a.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        b.x, a.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        a.x, b.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        b.x, a.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        b.x, b.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
        a.x, b.y, a.z,  0,0,  color.r, color.g, color.b, color.a,
    };
    spriteBatchAdd(verts, UINT32_MAX);
}
void Renderer::drawTexturedRect(const Point3F& a, const Point3F& b, uint32_t texId) {
    float verts[] = {
        a.x, a.y, a.z,  0,0,  1,1,1,1,
        b.x, a.y, a.z,  1,0,  1,1,1,1,
        a.x, b.y, a.z,  0,1,  1,1,1,1,
        b.x, a.y, a.z,  1,0,  1,1,1,1,
        b.x, b.y, a.z,  1,1,  1,1,1,1,
        a.x, b.y, a.z,  0,1,  1,1,1,1,
    };
    spriteBatchAdd(verts, texId);
}

void Renderer::drawTexturedRectUV(const Point3F& a, const Point3F& b, uint32_t texId, float u0, float v0, float u1, float v1, const ColorF* tint) {
    float cr = tint ? tint->r : 1, cg = tint ? tint->g : 1, cb = tint ? tint->b : 1, ca = tint ? tint->a : 1;
    float verts[] = {
        a.x,a.y,a.z, u0,v0, cr,cg,cb,ca,
        b.x,a.y,a.z, u1,v0, cr,cg,cb,ca,
        a.x,b.y,a.z, u0,v1, cr,cg,cb,ca,
        b.x,a.y,a.z, u1,v0, cr,cg,cb,ca,
        b.x,b.y,a.z, u1,v1, cr,cg,cb,ca,
        a.x,b.y,a.z, u0,v1, cr,cg,cb,ca,
    };
    spriteBatchAdd(verts, texId);
}

void Renderer::drawTexturedQuad(const Point3F& a, const Point3F& b, const Point3F& c,
                                const Point3F& d, uint32_t texture, const ColorF& tint,
                                float u0, float v0, float u1, float v1, bool additive) {
    float verts[] = {
        a.x,a.y,a.z,u0,v0,tint.r,tint.g,tint.b,tint.a,
        b.x,b.y,b.z,u1,v0,tint.r,tint.g,tint.b,tint.a,
        d.x,d.y,d.z,u0,v1,tint.r,tint.g,tint.b,tint.a,
        b.x,b.y,b.z,u1,v0,tint.r,tint.g,tint.b,tint.a,
        c.x,c.y,c.z,u1,v1,tint.r,tint.g,tint.b,tint.a,
        d.x,d.y,d.z,u0,v1,tint.r,tint.g,tint.b,tint.a,
    };
    spriteBatchAdd(const_cast<float*>(verts), texture, additive);
}

void Renderer::drawSprite(const Point3F& pos, float size, const ColorF& color,
                           uint32_t texture, bool additive) {
    initSpriteVAO();

    // Billboarding: extract right/up from view matrix
    const float* v = view.data();
    Point3F right = {v[0], v[4], v[8]};  // first column (transpose for row-major in memory)
    Point3F up = {v[1], v[5], v[9]};     // second column
    float s = size * 0.5f;

    float f[] = {
        pos.x + (-right.x + up.x) * s, pos.y + (-right.y + up.y) * s, pos.z + (-right.z + up.z) * s,  0,0, color.r,color.g,color.b,color.a,
        pos.x + (right.x + up.x) * s,  pos.y + (right.y + up.y) * s,  pos.z + (right.z + up.z) * s,   1,0, color.r,color.g,color.b,color.a,
        pos.x + (right.x - up.x) * s,  pos.y + (right.y - up.y) * s,  pos.z + (right.z - up.z) * s,   1,1, color.r,color.g,color.b,color.a,
        pos.x + (right.x - up.x) * s,  pos.y + (right.y - up.y) * s,  pos.z + (right.z - up.z) * s,   1,1, color.r,color.g,color.b,color.a,
        pos.x + (-right.x - up.x) * s, pos.y + (-right.y - up.y) * s, pos.z + (-right.z - up.z) * s,  0,1, color.r,color.g,color.b,color.a,
        pos.x + (-right.x + up.x) * s, pos.y + (-right.y + up.y) * s, pos.z + (-right.z + up.z) * s,  0,0, color.r,color.g,color.b,color.a,
    };
    GLboolean depthWrite = GL_TRUE;
    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    spriteBatchAdd(f, texture, additive);
    glDepthMask(depthWrite);
    if (!blendWasOn) glDisable(GL_BLEND);
}

void Renderer::drawOrientedSprite(const Point3F& pos, float size, const ColorF& color,
                                  const Point3F& direction, float angle,
                                  uint32_t texture, bool additive) {
    drawOrientedSpriteRect(pos, size, size, color, direction, angle, texture, 0, 0, 1, 1, additive);
}

void Renderer::drawOrientedSpriteRect(const Point3F& pos, float width, float height, const ColorF& color,
                                      const Point3F& direction, float angle, uint32_t texture,
                                      float u0, float v0, float u1, float v1, bool additive) {
    initSpriteVAO();
    Point3F normal = direction;
    float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length < 0.0001f) { drawSprite(pos, std::max(width, height), color, texture, additive); return; }
    normal.x /= length; normal.y /= length; normal.z /= length;
    Point3F toCamera{cameraPos.x - pos.x, cameraPos.y - pos.y, cameraPos.z - pos.z};
    float projection = toCamera.x * normal.x + toCamera.y * normal.y + toCamera.z * normal.z;
    Point3F up{toCamera.x - normal.x * projection, toCamera.y - normal.y * projection,
               toCamera.z - normal.z * projection};
    length = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (length < 0.0001f) { up = {0, 1, 0}; length = 1.0f; }
    up.x /= length; up.y /= length; up.z /= length;
    Point3F right{normal.y * up.z - normal.z * up.y,
                  normal.z * up.x - normal.x * up.z,
                  normal.x * up.y - normal.y * up.x};
    const float c = std::cos(angle), s = std::sin(angle);
    Point3F rr{right.x * c + up.x * s, right.y * c + up.y * s, right.z * c + up.z * s};
    Point3F uu{up.x * c - right.x * s, up.y * c - right.y * s, up.z * c - right.z * s};
    const float halfWidth = width * 0.5f, halfHeight = height * 0.5f;
    auto corner = [&](float x, float y) { return Point3F{
        pos.x + rr.x * x * halfWidth + uu.x * y * halfHeight,
        pos.y + rr.y * x * halfWidth + uu.y * y * halfHeight,
        pos.z + rr.z * x * halfWidth + uu.z * y * halfHeight}; };
    const Point3F a = corner(-1, 1), b = corner(1, 1), c0 = corner(1, -1), d = corner(-1, -1);
    float f[] = {a.x,a.y,a.z,u0,v0,color.r,color.g,color.b,color.a, b.x,b.y,b.z,u1,v0,color.r,color.g,color.b,color.a,
                 c0.x,c0.y,c0.z,u1,v1,color.r,color.g,color.b,color.a, c0.x,c0.y,c0.z,u1,v1,color.r,color.g,color.b,color.a,
                 d.x,d.y,d.z,u0,v1,color.r,color.g,color.b,color.a, a.x,a.y,a.z,u0,v0,color.r,color.g,color.b,color.a};
    GLboolean depthWrite = GL_TRUE;
    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    spriteBatchAdd(f, texture, additive);
    glDepthMask(depthWrite);
    if (!blendWasOn) glDisable(GL_BLEND);
}

void Renderer::drawShockwaveRing(const Point3F& center, float radius, float width,
                                 float height, int segments, uint32_t texture,
                                 const ColorF& color, float texWrap, bool additive,
                                 bool renderBottom, const Point3F& normal) {
    if (segments < 4) segments = 4;
    segments = std::min(segments, 256);
    const float inner = std::max(0.0f, radius - width * 0.5f);
    const float outer = radius + width * 0.5f;
    Point3F n = normal;
    const float nLen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nLen > 0.0001f) { n.x /= nLen; n.y /= nLen; n.z /= nLen; }
    const Point3F reference = std::fabs(n.y) < 0.9f ? Point3F{0, 1, 0} : Point3F{1, 0, 0};
    Point3F tangent{n.y * reference.z - n.z * reference.y,
                    n.z * reference.x - n.x * reference.z,
                    n.x * reference.y - n.y * reference.x};
    const float tangentLen = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
    if (tangentLen > 0.0001f) { tangent.x /= tangentLen; tangent.y /= tangentLen; tangent.z /= tangentLen; }
    Point3F bitangent{n.y * tangent.z - n.z * tangent.y,
                      n.z * tangent.x - n.x * tangent.z,
                      n.x * tangent.y - n.y * tangent.x};
    const auto ringPoint = [&](float angle, float distance, float normalOffset) {
        const float c = std::cos(angle), s = std::sin(angle);
        return Point3F{
            center.x + (tangent.x * c + bitangent.x * s) * distance + n.x * normalOffset,
            center.y + (tangent.y * c + bitangent.y * s) * distance + n.y * normalOffset,
            center.z + (tangent.z * c + bitangent.z * s) * distance + n.z * normalOffset};
    };
    const auto drawSurface = [&](float yOffset, float surfaceHeight) {
      for (int i = 0; i < segments; ++i) {
        const float a0 = (float)i / segments * 2.0f * Math::PI;
        const float a1 = (float)(i + 1) / segments * 2.0f * Math::PI;
        const float u0 = (float)i / segments * texWrap;
        const float u1 = (float)(i + 1) / segments * texWrap;
        const Point3F outerA = ringPoint(a0, outer, yOffset + surfaceHeight);
        const Point3F innerA = ringPoint(a0, inner, yOffset);
        const Point3F outerB = ringPoint(a1, outer, yOffset + surfaceHeight);
        const Point3F innerB = ringPoint(a1, inner, yOffset);
        float verts[] = {
            outerA.x, outerA.y, outerA.z, u0, 0.05f, color.r,color.g,color.b,color.a,
            innerA.x, innerA.y, innerA.z, u0, 0.95f, color.r,color.g,color.b,color.a,
            outerB.x, outerB.y, outerB.z, u1, 0.05f, color.r,color.g,color.b,color.a,
            innerA.x, innerA.y, innerA.z, u0, 0.95f, color.r,color.g,color.b,color.a,
            innerB.x, innerB.y, innerB.z, u1, 0.95f, color.r,color.g,color.b,color.a,
            outerB.x, outerB.y, outerB.z, u1, 0.05f, color.r,color.g,color.b,color.a,
        };
        spriteBatchAdd(verts, texture, additive);
      }
    };
    drawSurface(0.0f, height);
    if (renderBottom) drawSurface(height, -height);
}

Texture* Renderer::loadTexture(const char* path) {
    if (!path || !*path) return nullptr;
    auto it = impl->textures.find(path);
    if (it != impl->textures.end()) {
        // Do not retain failed/no-context loads as permanent asset handles.
        if (it->second && it->second->loaded) return it->second;
        if (!it->second) impl->textures.erase(it);
        else {
            delete it->second;
            impl->textures.erase(it);
        }
    }

    std::vector<uint8_t> data;
    std::string resolvedPath;
    if (!Engine::instance().fs().readTextureFile(path, data, &resolvedPath))
        return nullptr;
    if (data.empty()) {
        return nullptr;
    }

    auto* tex = new Texture;
    std::string extension = resolvedPath;
    for (char& c : extension) c = (char)std::tolower((unsigned char)c);
    if (extension.ends_with(".bm8")) tex->loadBM8(data.data(), data.size());
    else tex->load(data.data(), data.size());
    if (!tex->loaded) {
        delete tex;
        return nullptr;
    }
    impl->textures[path] = tex;
    stats.textures++;
    Console::instance().printf(LogLevel::Debug, "Texture loaded: %s (%dx%d)", path, tex->width, tex->height);
    return tex;
}

bool Renderer::loadTextureFrames(const char* path, std::vector<uint32_t>& frames,
                                 std::vector<float>& durations) {
    frames.clear();
    durations.clear();
    if (!path || !*path) return false;

    const std::string requested(path);
    std::vector<std::string> direct = {requested};
    if (requested.rfind("textures/", 0) != 0)
        direct.push_back("textures/" + requested);
    static constexpr const char* imageExtensions[] = {".png", ".bm8", ".jpg", ".gif", ".bmp", ".tga", ".dds"};
    // A companion IFL is the authored animation, so it takes precedence over
    // a same-named still image. If it has no usable frames, fall back below.
    std::vector<std::string> iflCandidates;
    const auto lower = [&]() {
        std::string value = requested;
        for (char& c : value) c = (char)std::tolower((unsigned char)c);
        return value;
    }();
    if (lower.ends_with(".ifl")) {
        iflCandidates = direct;
    } else {
        for (const auto& candidate : direct) iflCandidates.push_back(candidate + ".ifl");
        const size_t dot = lower.find_last_of('.');
        const size_t slash = lower.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            for (const auto& candidate : direct)
                iflCandidates.push_back(candidate.substr(0, candidate.find_last_of('.')) + ".ifl");
        }
    }
    auto readIfl = [&](const std::string& iflPath) {
        const std::string content = Engine::instance().fs().readText(iflPath.c_str());
        if (content.empty()) return false;
        const size_t slash = iflPath.find_last_of('/');
        const std::string directory = slash == std::string::npos
            ? std::string{} : iflPath.substr(0, slash + 1);
        for (const auto& source : parseTextureFrameSources(content)) {
            const std::string& frameName = source.name;
            const float duration = source.duration;
            std::vector<std::string> frameCandidates;
            if (frameName.find('/') == std::string::npos) frameCandidates.push_back(directory + frameName);
            frameCandidates.push_back(frameName);
            if (frameName.rfind("textures/", 0) != 0) frameCandidates.push_back("textures/" + frameName);
            for (const auto& framePath : frameCandidates) {
                Texture* texture = loadTexture(framePath.c_str());
                if (texture && texture->loaded) {
                    frames.push_back(texture->id);
                    durations.push_back(duration);
                    break;
                }
            }
        }
        return !frames.empty();
    };
    for (const auto& iflPath : iflCandidates) {
        if (readIfl(iflPath)) return true;
        frames.clear();
        durations.clear();
    }
    for (const auto& candidate : direct) {
        std::vector<std::string> candidates{candidate};
        const size_t dot = candidate.find_last_of('.');
        const size_t slash = candidate.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            for (const char* extension : imageExtensions)
                candidates.push_back(candidate + extension);
        for (const auto& image : candidates) {
            if (Texture* texture = loadTexture(image.c_str()); texture && texture->loaded) {
                frames.push_back(texture->id);
                return true;
            }
        }
    }

    return false;
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
    cfg.width = std::max(1, w);
    cfg.height = std::max(1, h);
    glViewport(0, 0, cfg.width, cfg.height);

    MatrixF p;
    const float aspect = cfg.width / (float)cfg.height;
    p.perspective(Math::DEG2RAD(Math::horizontalFovToVertical(cfg.fov, aspect)),
                  aspect, cfg.nearPlane, cfg.farPlane);
    setProjection(p);
}

// Mesh
void MeshData::upload() {
    if (uploaded) return;
    if (!SDL_GL_GetCurrentContext()) return;
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

void MeshData::updateGPU() {
    if (!uploaded) { upload(); return; }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(Vertex), vertices.data());
}

void MeshData::render() {
    if (vertices.empty() || indices.empty()) return;
    if (!uploaded) upload();
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, 0);
}

void MeshData::destroy() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    vao = vbo = ebo = 0;
    uploaded = false;
}

void MeshData::remapUVs(int32_t matFrame, const std::vector<Point2F>& tverts) {
    if (numTVertsPerFrame <= 0 || vertices.empty()) return;
    if (matFrame < 0) matFrame = 0;
    const int32_t frameCount = (int32_t)tverts.size() / numTVertsPerFrame;
    if (frameCount <= 0) return;
    matFrame = std::min(matFrame, frameCount - 1);
    int32_t offset = matFrame * numTVertsPerFrame;
    for (size_t i = 0; i < vertices.size(); i++) {
        int32_t baseIndex = tvertIndices.size() == vertices.size()
            ? tvertIndices[i] : (int32_t)i;
        int32_t ti = baseIndex + offset;
        if (ti >= 0 && ti < (int32_t)tverts.size())
            vertices[i].uv = tverts[ti];
    }
    if (uploaded) updateGPU();
}

void MeshData::setFrame(int32_t frame) {
    if (frameVertices.empty() || vertices.empty() || numFrames < 1) return;
    frame = std::clamp(frame, 0, numFrames - 1);
    const size_t count = vertices.size();
    const size_t offset = (size_t)frame * count;
    if (offset + count > frameVertices.size()) return;
    for (size_t i = 0; i < count; ++i) vertices[i].pos = frameVertices[offset + i];
    if (uploaded) updateGPU();
}

// Texture
void Texture::load(const uint8_t* data, size_t size) {
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) {
        // Try BM8 format (Tribes 2 proprietary)
        if (loadBM8(data, size)) return;
        Console::instance().printf(LogLevel::Error,
            "Texture decode failed; original asset required");
        return;
    }
    // T2 PNG skins key transparency by COLOR (magenta #FF00FF / pure blue
    // #0000FF), not via the alpha channel. When the corner pixel is a key
    // color, zero every matching pixel so shell art composites correctly.
    if (w > 0 && h > 0) {
        auto isKey = [](const unsigned char* p) {
            return (p[0] == 255 && p[1] == 0 && p[2] == 255) ||
                   (p[0] == 0 && p[1] == 0 && p[2] == 255);
        };
        if (isKey(pixels)) {
            size_t n = (size_t)w * h;
            for (size_t i = 0; i < n; i++)
                if (isKey(pixels + i * 4)) pixels[i * 4 + 3] = 0;
        }
    }
    loadRaw(pixels, w, h, 4);
    if (loaded) {
        int64_t total = 0, zeroAlpha = 0, nearZeroAlpha = 0;
        for (size_t i = 0; i < (size_t)(w * h * 4); i += 4) {
            total++;
            uint8_t a = pixels[i + 3];
            if (a == 0) zeroAlpha++;
            else if (a <= 10) nearZeroAlpha++;
        }
        alphaZeroRatio = total > 0 ? (float)((double)zeroAlpha / (double)total) : 0.0f;
        nearZeroAlphaRatio = total > 0 ? (float)((double)nearZeroAlpha / (double)total) : 0.0f;
        hasAlpha = alphaZeroRatio > 0.6f;
    }
    stbi_image_free(pixels);
}

bool Texture::decodeBM8(const uint8_t* data, size_t size,
                         std::vector<uint8_t>& outPixels,
                         int32_t& outW, int32_t& outH) {
    // T2 BM8 format (from bitmapBm8.cc):
    //   U32 byteSize        - total pixel data size (all mip levels)
    //   U32 width
    //   U32 height
    //   U32 bytesPerPixel   - 1 for palettized
    //   U32 numMipLevels
    //   U32 mipLevelOffsets[numMipLevels]
    //   GPalette: U32 version + U32 type + ColorI[256] (1024 bytes RGBA)
    //   U8 pixelData[byteSize]
    if (size < 20) return false;
    uint32_t byteSize, w, h, bytesPerPixel, numMipLevels;
    memcpy(&byteSize, data + 0, 4);
    memcpy(&w, data + 4, 4);
    memcpy(&h, data + 8, 4);
    memcpy(&bytesPerPixel, data + 12, 4);
    memcpy(&numMipLevels, data + 16, 4);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    if (bytesPerPixel != 1) return false; // only palettized supported

    uint32_t mipOffsetsStart = 20;
    uint32_t mipDataStart = mipOffsetsStart + numMipLevels * 4;
    if (mipDataStart + 1032 > size) return false; // need palette (version+type+colors)

    // Skip GPalette version (U32) and type (U32)
    uint32_t paletteStart = mipDataStart + 8;
    if (paletteStart + 1024 > size) return false;

    uint32_t pixelOffset = paletteStart + 1024;
    if (pixelOffset + byteSize > size) return false;

    uint8_t palette[1024];
    memcpy(palette, data + paletteStart, 1024);

    // Read palette type: RGB(0) or RGBA(1)
    // T2 uploads RGB palettes as GL_RGB (no alpha) — all pixels are opaque
    uint32_t palType = 0;
    memcpy(&palType, data + mipDataStart + 4, 4);
    bool isRGB = (palType == 0);

    // For RGB palettes, alpha channel is unused — force all to 255 (opaque)
    if (isRGB) {
        for (int i = 0; i < 256; i++)
            palette[i * 4 + 3] = 255;
    }

    uint32_t pixelCount = w * h;
    if (pixelCount > byteSize) pixelCount = byteSize;

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
    if (loaded) {
        int64_t total = 0, zeroAlpha = 0, nearZeroAlpha = 0;
        for (size_t i = 0; i < rgba.size(); i += 4) {
            total++;
            uint8_t a = rgba[i + 3];
            if (a == 0) zeroAlpha++;
            else if (a <= 10) nearZeroAlpha++;
        }
        alphaZeroRatio = total > 0 ? (float)((double)zeroAlpha / (double)total) : 0.0f;
        nearZeroAlphaRatio = total > 0 ? (float)((double)nearZeroAlpha / (double)total) : 0.0f;
        hasAlpha = alphaZeroRatio > 0.6f;
    }
    return loaded;
}

void Texture::loadRaw(const uint8_t* pixels, int32_t w, int32_t h, int32_t channels) {
    if (!SDL_GL_GetCurrentContext()) {
        width = w;
        height = h;
        loaded = false;
        return;
    }
    if (!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    GLenum fmt = (channels == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
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
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, vertSrc);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

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
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(id);
        id = 0;
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
    glUniformMatrix4fv(getUniformLoc(name), 1, GL_TRUE, m.data());
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
    return screenshot(path, nullptr);
}

bool Renderer::screenshot(const char* path, const char* metaData) {
    int w = cfg.width, h = cfg.height;
    if (impl->window) SDL_GetWindowSizeInPixels(impl->window, &w, &h);
    w = std::max(1, w);
    h = std::max(1, h);
    std::vector<uint8_t> pixels(w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);

    if (!metaData || strlen(metaData) == 0) {
        return stbi_write_png(path, w, h, 3, pixels.data(), w * 3) != 0;
    }

    // Write PNG to memory, inject tEXt chunk before IEND, write to file
    int len = 0;
    stbi_uc* pngData = stbi_write_png_to_mem(pixels.data(), w * 3, w, h, 3, &len);
    unsigned char* pngBytes = (unsigned char*)pngData;
    if (!pngData) return false;

    // Build tEXt chunk: keyword\0value
    std::string chunkData = "TorchMapper";
    chunkData.push_back('\0');
    chunkData += metaData;

    uint32_t dataLen = (uint32_t)chunkData.size();
    std::vector<uint8_t> out;
    // Walk PNG chunks to find IEND
    const unsigned char* iend = nullptr;
    size_t pos = 8; // skip 8-byte PNG signature
    while (pos + 8 <= (size_t)len) {
        uint32_t clen = (pngBytes[pos] << 24) | (pngBytes[pos+1] << 16) | (pngBytes[pos+2] << 8) | pngBytes[pos+3];
        if (memcmp(pngBytes + pos + 4, "IEND", 4) == 0) {
            iend = pngBytes + pos;
            break;
        }
        pos += 12 + clen; // 4(len) + 4(type) + data + 4(crc)
    }
    if (!iend) {
        // Fallback: just write normally
        bool ok = stbi_write_png(path, w, h, 3, pixels.data(), w * 3) != 0;
        stbi_image_free(pngData);
        return ok;
    }

    // Copy data before IEND
    size_t beforeLen = iend - pngBytes;
    out.assign(pngBytes, pngBytes + beforeLen);

    // Insert tEXt chunk
    uint8_t lenBytes[4] = {
        (dataLen >> 24) & 0xFF, (dataLen >> 16) & 0xFF,
        (dataLen >> 8) & 0xFF, dataLen & 0xFF
    };
    out.insert(out.end(), lenBytes, lenBytes + 4);
    const char tEXt[] = "tEXt";
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(tEXt), reinterpret_cast<const uint8_t*>(tEXt) + 4);
    out.insert(out.end(), chunkData.begin(), chunkData.end());
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const uint8_t*>(tEXt), 4);
    crc = crc32(crc, reinterpret_cast<const uint8_t*>(chunkData.data()), dataLen);
    uint8_t crcBytes[4] = {
        (crc >> 24) & 0xFF, (crc >> 16) & 0xFF,
        (crc >> 8) & 0xFF, crc & 0xFF
    };
    out.insert(out.end(), crcBytes, crcBytes + 4);

    // Copy IEND and rest
    out.insert(out.end(), (uint8_t*)iend, (uint8_t*)(pngBytes + len));

    FILE* f = fopen(path, "wb");
    if (!f) { stbi_image_free(pngData); return false; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    stbi_image_free(pngData);
    return true;
}
