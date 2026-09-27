#include "render/projected_shadows.h"
#include "core/console.h"
#include <GL/glew.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr int AtlasSize = 1024;
constexpr int TileSize = 64;
constexpr int TilesPerRow = AtlasSize / TileSize;
constexpr int MaxCasters = TilesPerRow * TilesPerRow;
constexpr double IdleReleaseMs = 3000.0;

const char* silhouetteVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* silhouetteFrag = R"(
#version 330 core
out vec4 FragColor;
void main() { FragColor = vec4(0.0, 0.0, 0.0, 1.0); }
)";

const char* decalVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* decalFrag = R"(
#version 330 core
in vec3 vWorld;
out vec4 FragColor;
uniform sampler2D uAtlas;
// Tile origin (xy), tile scale (z) and one atlas texel (w).
uniform vec4 uTile;
uniform vec3 uCenter;
uniform vec3 uAxisX;
uniform vec3 uDir;
uniform vec3 uAxisZ;
uniform float uRadius;
uniform float uReach;
// 1 - the pixel-size/haze fade, applied with the depth falloff.
uniform float uFadeScale;
// The caster's own fade; below 1 it replaces the per-vertex darkening, as
// Shadow::render swaps the colour array for glColor4f.
uniform float uObjectAlpha;
uniform int uBlur;
uniform int uGeneric;

float tap(vec2 uv, vec2 offset) {
    float inset = 0.5 * uTile.w;
    vec2 atlasUv = uTile.xy + uv * uTile.z + offset * uTile.w;
    atlasUv = clamp(atlasUv, uTile.xy + inset, uTile.xy + uTile.z - inset);
    return texture(uAtlas, atlasUv).a;
}

void main() {
    vec3 d = vWorld - uCenter;
    vec3 l = vec3(dot(d, uAxisX), dot(d, uDir), dot(d, uAxisZ));
    if (l.y < 0.0 || l.y > uReach) discard;
    vec2 uv = l.xz / (2.0 * uRadius) + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;
    float a;
    if (uGeneric != 0) {
        vec2 c = uv * 2.0 - 1.0;
        float r2 = dot(c, c);
        a = r2 <= 0.99 ? )" "0.70588" R"( * (1.0 - r2) : 0.0;
    } else if (uBlur != 0) {
        a = (tap(uv, vec2(-1.0, -1.0)) + 2.0 * tap(uv, vec2(0.0, -1.0)) + tap(uv, vec2(1.0, -1.0))
           + 2.0 * tap(uv, vec2(-1.0, 0.0)) + 3.0 * tap(uv, vec2(0.0, 0.0)) + 2.0 * tap(uv, vec2(1.0, 0.0))
           + tap(uv, vec2(-1.0, 1.0)) + 2.0 * tap(uv, vec2(0.0, 1.0)) + tap(uv, vec2(1.0, 1.0))) / 15.0;
    } else {
        a = tap(uv, vec2(0.0));
    }
    a *= uObjectAlpha < 1.0 ? uObjectAlpha : uFadeScale * (1.0 - l.y / uReach);
    FragColor = vec4(0.0, 0.0, 0.0, a);
}
)";

void tileOrigin(int tile, int& x, int& y) {
    x = (tile % TilesPerRow) * TileSize;
    y = (tile / TilesPerRow) * TileSize;
}

bool sameBasis(const ShadowProjection::Basis& a, const ShadowProjection::Basis& b) {
    auto eq = [](const Point3F& p, const Point3F& q) { return p.x == q.x && p.y == q.y && p.z == q.z; };
    return eq(a.center, b.center) && eq(a.dir, b.dir) && eq(a.x, b.x) && eq(a.z, b.z);
}

} // namespace

bool ProjectedShadows::init() {
    if (initialized) return true;
    if (failed) return false;
    if (!silhouetteShader.load(silhouetteVert, silhouetteFrag) || !decalShader.load(decalVert, decalFrag)) {
        Console::instance().printf(LogLevel::Error, "Projected shadows: shader compile failed");
        failed = true;
        return false;
    }
    glGenTextures(1, &atlasTex);
    glBindTexture(GL_TEXTURE_2D, atlasTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AtlasSize, AtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glGenFramebuffers(1, &atlasFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, atlasFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlasTex, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
    if (!complete) {
        Console::instance().printf(LogLevel::Error, "Projected shadows: atlas framebuffer incomplete");
        destroy();
        failed = true;
        return false;
    }
    freeTiles.clear();
    for (int i = MaxCasters - 1; i >= 0; --i) freeTiles.push_back(i);
    initialized = true;
    return true;
}

void ProjectedShadows::renderSilhouette(Renderer& r, State& state,
                                        const std::vector<ShadowCaptureDraw>& draws) {
    const float radius = state.radius;
    const Point3F& c = state.basis.center;
    const Point3F& dir = state.basis.dir;
    MatrixF view, proj;
    view.lookAt({c.x - dir.x * 2.0f * radius, c.y - dir.y * 2.0f * radius, c.z - dir.z * 2.0f * radius},
                c, state.basis.z);
    proj.orthographic(-radius, radius, -radius, radius, 0.01f, 4.0f * radius);
    const MatrixF viewProj = proj * view;

    GLint previousFbo = 0, viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    int x, y;
    tileOrigin(state.tile, x, y);
    glBindFramebuffer(GL_FRAMEBUFFER, atlasFbo);
    glViewport(x, y, TileSize, TileSize);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, TileSize, TileSize);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    silhouetteShader.bind();
    for (const ShadowCaptureDraw& d : draws) {
        if (!d.mesh) continue;
        silhouetteShader.setUniform("uMVP", viewProj * d.model);
        d.mesh->render();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previousFbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    if (!scissor) glDisable(GL_SCISSOR_TEST);
    if (depthTest) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (blend) glEnable(GL_BLEND);
    (void)r;
}

bool ProjectedShadows::writeReceivers(State& state, const GatherReceivers& gather) {
    if (state.receiverRadius == state.radius && state.receiverReach == state.reach &&
        sameBasis(state.receiverBasis, state.basis))
        return state.vertexCount > 0;
    state.receiverBasis = state.basis;
    state.receiverRadius = state.radius;
    state.receiverReach = state.reach;
    // World box of the projection volume (+-R laterally, 0..reach along the light).
    Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    const auto& b = state.basis;
    for (int i = 0; i < 8; ++i) {
        const float lx = i & 1 ? state.radius : -state.radius;
        const float ly = i & 2 ? state.reach : 0.0f;
        const float lz = i & 4 ? state.radius : -state.radius;
        const Point3F p{b.center.x + b.x.x * lx + b.dir.x * ly + b.z.x * lz,
                        b.center.y + b.x.y * lx + b.dir.y * ly + b.z.y * lz,
                        b.center.z + b.x.z * lx + b.dir.z * ly + b.z.z * lz};
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    scratch.clear();
    gather(lo, hi, b.dir, scratch);
    state.vertexCount = scratch.size();
    if (scratch.empty()) return false;
    if (!state.vao) {
        glGenVertexArrays(1, &state.vao);
        glGenBuffers(1, &state.vbo);
        glBindVertexArray(state.vao);
        glBindBuffer(GL_ARRAY_BUFFER, state.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point3F), nullptr);
        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, state.vbo);
    if (scratch.size() > state.capacity) {
        state.capacity = std::max<size_t>(scratch.size(), state.capacity * 2);
        glBufferData(GL_ARRAY_BUFFER, state.capacity * sizeof(Point3F), nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, scratch.size() * sizeof(Point3F), scratch.data());
    return true;
}

void ProjectedShadows::submit(Renderer& r, const Caster& caster, double nowMs, float haze,
                              const GatherReceivers& gather) {
    if (!(caster.radius > 0.0f) || caster.alpha <= 0.0f || !init()) return;
    const Point3F& cam = r.cameraPos;
    const float dist = std::sqrt((cam.x - caster.center.x) * (cam.x - caster.center.x) +
                                 (cam.y - caster.center.y) * (cam.y - caster.center.y) +
                                 (cam.z - caster.center.z) * (cam.z - caster.center.z));
    // Pixel scale from the projection itself: height / 2 / tan(vfov / 2).
    const float pixelScale = (float)r.config().height * 0.5f * r.projection.m[1][1];
    const float px = dist > 1e-3f ? caster.radius / dist * pixelScale : INFINITY;
    const auto vis = ShadowProjection::visibility(px);
    if (!vis.visible) return;
    const float fade = std::max(vis.fade, haze);
    if (fade >= ShadowProjection::FadeCutoff) return;

    auto it = states.find(caster.key);
    if (it == states.end()) {
        if (freeTiles.empty()) return;
        State fresh;
        fresh.tile = freeTiles.back();
        freeTiles.pop_back();
        it = states.emplace(caster.key, fresh).first;
    }
    State& state = it->second;
    state.lastVisibleMs = nowMs;
    const auto spec = ShadowProjection::tileSpec(px);
    state.generic = spec.size == 0;
    state.blur = spec.blur;
    state.reach = ShadowProjection::ReachFactor * caster.radius *
                  (1.0f - ShadowProjection::distanceFade(dist).reachLoss);
    state.radius = state.generic ? caster.radius * ShadowProjection::GenericRadiusScale : caster.radius;
    state.basis = ShadowProjection::lightBasis(ShadowProjection::lightDir(dist), caster.center);
    state.fadeScale = 1.0f - fade;
    state.objectAlpha = std::min(1.0f, caster.alpha);
    if (!writeReceivers(state, gather)) { state.drawThisFrame = false; return; }
    state.drawThisFrame = true;
    if (!state.generic && caster.draws &&
        (state.lastRenderMs < -1e29 || nowMs - state.lastRenderMs >= spec.intervalMs)) {
        state.lastRenderMs = nowMs;
        renderSilhouette(r, state, *caster.draws);
    }
}

void ProjectedShadows::draw(Renderer& r) {
    if (!initialized) return;
    bool any = false;
    for (auto& [key, state] : states) any = any || state.drawThisFrame;
    if (!any) return;
    const GLboolean cull = glIsEnabled(GL_CULL_FACE), blend = glIsEnabled(GL_BLEND),
                    depthTest = glIsEnabled(GL_DEPTH_TEST), offset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLint depthFunc = GL_LESS, blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-2.0f, -2.0f);
    decalShader.bind();
    decalShader.setUniform("uViewProj", r.projection * r.view);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlasTex);
    decalShader.setUniform("uAtlas", (int32_t)0);
    for (auto& [key, state] : states) {
        if (!state.drawThisFrame || !state.vertexCount) continue;
        int x, y;
        tileOrigin(state.tile, x, y);
        decalShader.setUniform("uTile", ColorF{(float)x / AtlasSize, (float)y / AtlasSize,
                                               (float)TileSize / AtlasSize, 1.0f / AtlasSize});
        decalShader.setUniform("uCenter", state.basis.center);
        decalShader.setUniform("uAxisX", state.basis.x);
        decalShader.setUniform("uDir", state.basis.dir);
        decalShader.setUniform("uAxisZ", state.basis.z);
        decalShader.setUniform("uRadius", state.radius);
        decalShader.setUniform("uReach", state.reach);
        decalShader.setUniform("uFadeScale", state.fadeScale);
        decalShader.setUniform("uObjectAlpha", state.objectAlpha);
        decalShader.setUniform("uBlur", (int32_t)(state.blur ? 1 : 0));
        decalShader.setUniform("uGeneric", (int32_t)(state.generic ? 1 : 0));
        glBindVertexArray(state.vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)state.vertexCount);
        state.drawThisFrame = false;
    }
    glBindVertexArray(0);
    glPolygonOffset(0.0f, 0.0f);
    if (!offset) glDisable(GL_POLYGON_OFFSET_FILL);
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcA, blendDstA);
    if (!blend) glDisable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (!depthTest) glDisable(GL_DEPTH_TEST);
    glDepthMask(depthMask);
    glDepthFunc(depthFunc);
}

void ProjectedShadows::endFrame(double nowMs) {
    for (auto it = states.begin(); it != states.end();) {
        if (nowMs - it->second.lastVisibleMs > IdleReleaseMs) {
            if (it->second.vao) glDeleteVertexArrays(1, &it->second.vao);
            if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
            freeTiles.push_back(it->second.tile);
            it = states.erase(it);
        } else {
            ++it;
        }
    }
}

void ProjectedShadows::destroy() {
    for (auto& [key, state] : states) {
        if (state.vao) glDeleteVertexArrays(1, &state.vao);
        if (state.vbo) glDeleteBuffers(1, &state.vbo);
    }
    states.clear();
    if (atlasFbo) glDeleteFramebuffers(1, &atlasFbo);
    if (atlasTex) glDeleteTextures(1, &atlasTex);
    atlasFbo = atlasTex = 0;
    silhouetteShader.destroy();
    decalShader.destroy();
    initialized = false;
}
