#pragma once
// Engine-style projected shadows (Tribes2.exe shadow.cc, as ported by
// t2-mapper ShadowPool) for players and vehicles. Per caster and frame it
//   - culls and fades by the caster's projected radius (Shadow::prepare),
//   - tilts the constant light toward vertical with camera distance,
//   - re-renders the caster's opaque silhouette into a 64-px atlas tile at
//     the engine's 25/100 ms cadence (its software rasteriser; the 3x3 blur
//     runs in the decal shader),
//   - gathers terrain and interior triangles inside the projection box
//     (+-R laterally, 10R along the light) that face the light, and
//   - draws them with the silhouette projected on, darkening the
//     framebuffer by silhouette x (1 - depth / reach) x fade under
//     glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA), no depth write,
//     polygon offset.
// Casters below ~10 projected pixels get the engine's generic blob.
#include "render/renderer.h"
#include "render/shadow_projection.h"
#include <functional>
#include <unordered_map>
#include <vector>

class ProjectedShadows {
public:
    // Appends triangles (three points each) inside [lo, hi] whose normal
    // faces `lightDir` (normal . lightDir < ShadowProjection::ReceiverFacing).
    using GatherReceivers = std::function<void(const Point3F& lo, const Point3F& hi,
                                               const Point3F& lightDir, std::vector<Point3F>& out)>;
    struct Caster {
        int key = -1;
        const std::vector<ShadowCaptureDraw>* draws = nullptr;
        Point3F center{};   // world-space bounds centre
        float radius = 0;   // half the bounds diagonal
        float alpha = 1;    // object fade (mFadeVal)
    };

    // Right after the caster drew (its posed buffers are still current).
    // `haze` is the scene haze at the caster, 0..1.
    void submit(Renderer& r, const Caster& caster, double nowMs, float haze,
                const GatherReceivers& gather);
    // Draw this frame's shadows over the opaque receivers.
    void draw(Renderer& r);
    // Release casters idle for 3 s.
    void endFrame(double nowMs);
    void destroy();

private:
    struct State {
        int tile = -1;
        double lastRenderMs = -1e30;
        double lastVisibleMs = 0;
        bool drawThisFrame = false;
        ShadowProjection::Basis basis{};
        float radius = 1, reach = 1, fadeScale = 1, objectAlpha = 1;
        bool blur = true, generic = false;
        // Receiver cache: the light frame it was gathered in.
        ShadowProjection::Basis receiverBasis{};
        float receiverRadius = -1, receiverReach = -1;
        uint32_t vao = 0, vbo = 0;
        size_t vertexCount = 0, capacity = 0;
    };
    bool init();
    void renderSilhouette(Renderer& r, State& state, const std::vector<ShadowCaptureDraw>& draws);
    bool writeReceivers(State& state, const GatherReceivers& gather);

    bool initialized = false, failed = false;
    uint32_t atlasTex = 0, atlasFbo = 0;
    Shader silhouetteShader, decalShader;
    std::unordered_map<int, State> states;
    std::vector<int> freeTiles;
    std::vector<Point3F> scratch;
};
