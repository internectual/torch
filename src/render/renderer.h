#pragma once
#include "core/math.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "render/dynamic_lighting.h"

// Native Torque material flags are shared by DTS, DIF, and renderer code.
// They must not live in the diagnostic GLB loader.
enum MaterialFlag : uint32_t {
    MatFlag_None = 0,
    MatFlag_Translucent = 1,
    MatFlag_Additive = 2,
    MatFlag_SelfIlluminating = 4,
    MatFlag_NeverEnvMap = 8,
    MatFlag_SWrap = 16,
    MatFlag_TWrap = 32,
};

struct SkinInfo {
    bool hasSkin = false;
    std::vector<Point3F> initialPositions;
    std::vector<Point3F> initialNormals;
    std::vector<MatrixF> initialTransforms;
    std::vector<int32_t> boneIndices;
    std::vector<float> boneWeights;
    std::vector<int32_t> nodeIndices;
    std::vector<int32_t> vertexIndices;
};

struct RenderConfig {
    int32_t width = 1024;
    int32_t height = 768;
    bool fullscreen = false;
    bool vsync = true;
    float fov = 90.0f; // horizontal degrees, matching Torque's camera FOV
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    int32_t maxLights = 8;
    float gamma = 1.0f;
    float detailScale = 1.0f; // texture tiling factor
    bool useNormalMap = false; // enable per-pixel normal mapping
    int32_t shadowMapSize = 2048; // default shadow map resolution
    float fogDensity = -1.0f; // exponential fog density override (-1 = not set)
    ColorF fogColor = {0.5f, 0.6f, 0.7f, 1.0f}; // default fog color
    bool fogColorOverride = false; // true once setFogColor has been used
};

struct Vertex {
    Point3F pos;
    Point3F normal;
    Point2F uv;
    Point2F uv2; // lightmap UV (for DIF interiors)
    ColorF color = {1,1,1,1};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<Point3F> frameVertices; // all mesh frames, frame-major
    int32_t numFrames = 1;
    std::vector<uint32_t> indices;
    std::vector<int32_t> tvertIndices; // per-vertex: original tvert index (for matFrame remapping)
    int32_t numTVertsPerFrame = 0;     // number of tverts per material frame
    uint32_t vao{}, vbo{}, ebo{};
    int32_t materialIndex = -1; // index into DTSShape::materialTextures
    int32_t materialIdx = -1;   // raw material index from GLB file
    int32_t nodeIndex = -1;     // DTS node index (-1 = no node)
    int32_t interiorZone = -1;  // DIF zone owning this mesh; -1 for non-interior meshes
    bool interiorOutsideVisible = false;
    bool uploaded = false;
    void upload();
    void updateGPU();
    void render();
    void destroy();
    void remapUVs(int32_t matFrame, const std::vector<Point2F>& tverts); // remap UVs based on material frame index
    void setFrame(int32_t frame);
};

struct Texture {
    uint32_t id{};
    int32_t width{}, height{};
    int32_t format{};
    bool loaded = false;
    bool hasAlpha = false;
    float alphaZeroRatio = 0.0f;      // fraction of pixels with alpha == 0
    float nearZeroAlphaRatio = 0.0f;  // fraction of pixels with 0 < alpha <= 10
    void load(const uint8_t* data, size_t size);
    bool loadBM8(const uint8_t* data, size_t size);
    void loadRaw(const uint8_t* pixels, int32_t w, int32_t h, int32_t channels);
    void bind(int32_t unit = 0);
    void destroy();

    // Decode .bm8 to raw RGBA pixels without creating a GL texture
    static bool decodeBM8(const uint8_t* data, size_t size,
                          std::vector<uint8_t>& outPixels,
                          int32_t& outW, int32_t& outH);
};

struct Shader {
    uint32_t id{};
    bool loaded = false;
    std::unordered_map<std::string, int> uniformCache;
    int getUniformLoc(const char* name);
    bool load(const char* vertSrc, const char* fragSrc);
    bool loadFromFiles(const char* vertPath, const char* fragPath);
    void bind();
    void setUniform(const char* name, float v);
    void setUniform(const char* name, const Point3F& v);
    void setUniform(const char* name, const MatrixF& m);
    void setUniform(const char* name, int32_t v);
    void setUniform(const char* name, const ColorF& v);
    void destroy();
};

struct GlyphInfo {
    uint8_t width = 0, height = 0;
    int8_t xOff = 0, yOff = 0;
    uint8_t xAdvance = 0;
};

struct GFTChar {
    uint16_t bitmapIndex{};
    uint8_t xOrigin{}, yOrigin{}, width{}, height{};
    int8_t xOffset{}, yOffset{};
    uint8_t xAdvance{};
};

struct Font {
    uint32_t texture{};
    int32_t charWidth{}, charHeight{};   // max glyph size, font cell height
    int32_t baseLine = 0;                // distance from top of cell to baseline
    int32_t texWidth{}, texHeight{};
    float charUV[256][4]{};
    GlyphInfo glyphs[256]{};
    float defaultScale = 1.0f;
    bool loaded = false;
    bool proportional = false;
    std::string fontName;
    int32_t fontSize = 0;

    uint32_t fontVAO = 0, fontVBO = 0, fontEBO = 0;

    bool load(const uint8_t* data, size_t size);
    bool loadDefault(int size = 8);
    bool loadGFT(const uint8_t* data, size_t size);
    void render(const char* text, float x, float y, const ColorF& color, float scale = 1.0f, bool exactColor = false, int maxChars = -1);
    Point2F measure(const char* text, float scale = 1.0f);
};

struct DTSShape {
    // DTS shape data
    std::string name;
    std::vector<MeshData> meshes;
    struct DetailLevel {
        float size;
        int32_t meshIndex;
        std::vector<int32_t> meshIndices; // actual mesh indices for this detail level
    };
    std::vector<DetailLevel> details;
    struct Node {
        std::string name;
        int32_t parentIndex = -1;
    };
    std::vector<Node> nodes;

    struct Keyframe {
        float time;
        int32_t nodeIndex;
        Point3F translation;
        QuatF rotation;
        Point3F scale;
        bool hasTranslation = true;
        bool hasRotation = true;
        bool hasScale = true;
    };
    struct ObjectKeyframe {
        int32_t objectIndex;
        float time;
        float vis;           // 0.0 = invisible, 1.0 = visible
        int32_t frameIndex;  // mesh frame index
        int32_t matFrameIndex; // material frame index
    };
    struct Animation {
        std::string name;
        float duration;
        bool looping = false;
        bool blend = false;
        std::vector<Keyframe> keyframes;
        std::vector<ObjectKeyframe> objectKeyframes; // vis/frame animation
    };
    std::vector<Animation> animations;
    std::vector<SkinInfo> skins; // parallel to meshes
    std::vector<int32_t> objectStartMesh; // per-object: first mesh index
    std::vector<int32_t> objectNumMeshes; // per-object: number of meshes
    std::vector<MatrixF> defaultTransforms; // per-node bind pose world transforms
    std::vector<MatrixF> defaultLocalTransforms; // per-node bind pose local transforms
    std::vector<std::string> materialNames; // original material names (for skin overrides)
    std::vector<Texture> materialTextures;
    // Non-owning runtime override used by ShapeBase cloak state.
    Texture* cloakTextureOverride = nullptr;
    std::vector<uint32_t> materialFlags; // parallel to materialTextures
    std::vector<float> materialReflectionAmount; // parallel to materialTextures
    std::vector<float> materialMetallic; // parallel to materialTextures
    std::vector<float> materialRoughness; // parallel to materialTextures
    std::vector<std::vector<Point2F>> meshTVerts; // per-mesh: all tvert data (numTVerts * numMatFrames)
    std::vector<Texture> lightmaps;
    std::vector<int16_t> materialLightmapIndex; // per-material: -1 no lightmap, >=0 index into lightmaps[]
    struct InteriorPlane { Point3F normal; float d = 0.0f; };
    struct InteriorBSPNode { uint16_t planeIndex = 0, frontIndex = 0, backIndex = 0; };
    struct InteriorPortal {
        uint16_t planeIndex = 0;
        uint16_t zoneFront = 0, zoneBack = 0;
        std::vector<Point3F> vertices;
    };
    std::vector<InteriorPlane> interiorPlanes;
    std::vector<InteriorBSPNode> interiorBSP;
    std::vector<std::vector<uint16_t>> interiorZoneNeighbors;
    std::vector<InteriorPortal> interiorPortals;
    std::vector<bool> activeInteriorZones;
    bool isInterior = false;
    bool nativeDTS = false;
    bool loaded = false;
    /// Legacy orientation hook for non-native imported assets. Native DTS is
    /// canonicalized to Y-up by the loader and therefore returns identity.
    bool upConvert = true;
    MatrixF upOrientation() const {
        if (nativeDTS) return MatrixF{};
        return upConvert ? Math::czUpToYUp() : MatrixF{};
    }
    // Hull collision data (from DIF files)
    std::vector<float> collisionVerts;
    std::vector<uint32_t> collisionIndices;
    bool load(const uint8_t* data, size_t size);
    int interiorZoneForPoint(const Point3F& point) const;
    void interiorVisibleZones(int zone, const Point3F& camera, const MatrixF& clipTransform,
                              std::vector<bool>& visible) const;
    // Node overrides: per-instance transform modifications (e.g., turret barrel aiming)
    struct NodeOverride {
        int nodeIndex;
        MatrixF transform;
    };
    void render(int32_t detailLevel = 0, const NodeOverride* overrides = nullptr, int numOverrides = 0);
    void renderAnimation(const char* animName, float time,
                         const NodeOverride* overrides = nullptr,
                         int numOverrides = 0);
    bool applySkin(const std::string& skinName);

    // Find node index by name (-1 if not found)
    int findNode(const std::string& name) const;
};

inline bool interiorPortalAllowsTraversal(uint16_t planeIndex, uint16_t zoneFront,
                                          uint16_t zoneBack, int currentZone,
                                          const Point3F& camera,
                                          const DTSShape::InteriorPlane& plane) {
    if (zoneFront == zoneBack) return false;
    float side = plane.normal.x * camera.x + plane.normal.y * camera.y +
                 plane.normal.z * camera.z + plane.d;
    if (planeIndex & 0x8000) side = -side;
    if (side == 0.0f) return true;
    const float zoneSide = zoneFront == currentZone ? 1.0f : -1.0f;
    return (side > 0.0f ? 1.0f : -1.0f) == zoneSide;
}

struct TerrainBlock {
    int32_t size{256};
    float heightScale{1.0f};
    float squareSize{8.0f};
    Point3F worldOffset{-1024, 0, 1024};
    std::vector<float> heights;
    // Per-square terrain holes decoded from TerrainBlock.emptySquares.
    std::vector<uint8_t> emptySquares;
    std::vector<MeshData> meshes;
    std::vector<Texture> detailTextures;
    std::vector<Texture> normalTextures; // optional normal maps per layer
    float detailTilings[6] = {0, 0, 0, 0, 0, 0}; // 0 = use default
    Texture splatMap;   // RGBA: layers 0-3 alpha weights
    Texture splatMap2;  // RGBA: layers 4-5 alpha weights (R,G used)
    Texture gameGrid;   // MissionArea boundary overlay texture
    Texture lightmap;   // baked self-shadowing NdotL lightmap (computed from heightfield)
    std::vector<std::string> textureNames;
    bool loaded = false;

    float sampleHeight(float wx, float wz) const;
    void setEmptySquareRuns(const std::vector<uint32_t>& runs);
    bool isEmptySquare(float wx, float wz) const;
    bool load(const uint8_t* data, size_t size);
    void reset();
    void generateMesh();
    void bakeLightmap();
    void render(const Point3F& cameraPos, bool fogEnabled = false, const ColorF& fogColor = {0.5f, 0.6f, 0.7f, 1.0f}, float fogDensity = 0.005f, const Point3F* lightDir = nullptr,
                const ColorF* sunColor = nullptr, const ColorF* ambient = nullptr, float fogStart = -1.0f, float fogEnd = -1.0f);

    Point3F lightDir{0.5f, 0.8f, 0.6f}; // normalized: points FROM scene toward sun; used for lightmap+bakeLightmap
};

struct Sky {
    uint32_t cubemap{};
    uint32_t vao{}, vbo{};
    Texture emap;
    bool loaded = false;
    bool useSkyTextures = true;
    ColorF solidColor{0, 0, 0, 1};
    ColorF fogColor{0.5f, 0.5f, 0.5f, 1};
    float visibleDistance = 250.0f;
    struct FogVolume { float visibleDistance, minHeight, maxHeight, percentage; };
    std::vector<FogVolume> fogVolumes;
    void reset();
    void load(const std::vector<std::string>& faces);
    void render(const MatrixF& view, const MatrixF& proj, float cameraHeight = 0.0f);

    // Cloud layers (from DML lines 7-9)
    struct CloudLayer {
        Texture texture;
        float scrollSpeed = 0.0f;   // horizontal scroll speed
        float opacity = 1.0f;
        float height = 0.5f;        // 0-1, position on sky dome
    };
    std::vector<CloudLayer> cloudLayers;
    uint32_t cloudVAO = 0, cloudVBO = 0;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(void* window);
    void shutdown();

    void beginFrame(const ColorF& clearColor = {0.3f, 0.5f, 0.8f, 1.0f});
    void endFrame();

    void setViewport(int32_t x, int32_t y, int32_t w, int32_t h);
    void setProjection(const MatrixF& proj);
    void setView(const MatrixF& view);
    void setModel(const MatrixF& model);
    const MatrixF& modelMatrix() const;
    const MatrixF& viewMatrix() const { return view; }
    const MatrixF& projectionMatrix() const { return projection; }
    void setCamera(const Point3F& pos, const Point3F& target, const Point3F& up);
    void setDynamicLights(const std::vector<DynamicPointLight>& lights);
    void clearDynamicLights();
    // Establish the state shared by transparent world effects. This is an
    // explicit pass boundary rather than relying on the previous draw call.
    void beginTransparentPass();
    void endTransparentPass();

    void drawMesh(MeshData& mesh, const MatrixF& transform);
    void drawLine(const Point3F& a, const Point3F& b, const ColorF& color);
    void drawLineStrip(const std::vector<Point3F>& points, const ColorF& color);
    void drawBox(const Box3F& box, const ColorF& color);
    void drawFilledQuad(float width, float depth);
    void drawRectFill(const Point3F& a, const Point3F& b, const ColorF& color);
    void drawSprite(const Point3F& pos, float size, const ColorF& color,
                    uint32_t texture = 0, bool additive = false);
    void drawOrientedSprite(const Point3F& pos, float size, const ColorF& color,
                            const Point3F& direction, float angle = 0.0f,
                            uint32_t texture = 0, bool additive = false);
    void drawOrientedSpriteRect(const Point3F& pos, float width, float height, const ColorF& color,
                                const Point3F& direction, float angle, uint32_t texture,
                                float u0, float v0, float u1, float v1, bool additive = false);
    void drawTexturedQuad(const Point3F& a, const Point3F& b, const Point3F& c,
                          const Point3F& d, uint32_t texture,
                          const ColorF& tint, float u0 = 0.0f, float v0 = 0.0f,
                          float u1 = 1.0f, float v1 = 1.0f, bool additive = true);
    void drawShockwaveRing(const Point3F& center, float radius, float width,
                           float height, int segments, uint32_t texture,
                           const ColorF& color, float texWrap = 1.0f,
                           bool additive = true, bool renderBottom = false,
                           const Point3F& normal = {0, 1, 0});
    void drawTexturedRect(const Point3F& a, const Point3F& b, uint32_t texture);
    void drawTexturedRectUV(const Point3F& a, const Point3F& b, uint32_t texture, float u0, float v0, float u1, float v1, const ColorF* tint = nullptr);
    void flushSpriteBatch();

    Texture* loadTexture(const char* path);
    // Resolve a direct texture or an existing IFL into renderable frames.
    // Durations are in seconds and are empty when the asset has no timing.
    bool loadTextureFrames(const char* path, std::vector<uint32_t>& frames,
                           std::vector<float>& durations);
    Shader* loadShader(const char* vertPath, const char* fragPath);
    void addShader(Shader* shader);
    void addTexture(Texture* tex);

    Font* getFont() { return defaultFont; }
    Font* getFont(const char* name, int size);
    void addFont(Font* font);
    void setFontScale(float scale);
    void renderText(const char* text, float x, float y, const ColorF& color, float scale = 1.0f);

    TerrainBlock* getTerrain() { return terrain; }
    Sky* getSky() { return sky; }

    RenderConfig& config() { return cfg; }
    void onResize(int32_t w, int32_t h);
    bool screenshot(const char* path);
    bool screenshot(const char* path, const char* metaData);
    // Shadow mapping
    void initShadowMap(int32_t size = 2048);
    void beginShadowPass(const Point3F& lightDir, const Point3F& sceneCenter, float sceneRadius);
    void endShadowPass();
    const MatrixF& shadowMatrix() const { return shadowBiasVP; }
    const MatrixF& lightViewProj() const { return shadowVP; }
    bool shadowEnabled() const { return shadowSize > 0; }
    /// Tab-separated "VENDOR\RENDERER\VERSION\EXTENSIONS" for getVideoDriverInfo()
    /// (populated on the first successful video init).
    const std::string& gpuDriverInfo() const { return gpuInfo; }
    /// True while a valid shadow map is bound for the main pass (set by game.cpp
    /// before the world render, cleared outside gameplay). DTSShape::render uses
    /// this to decide whether shapes should sample the shadow map.
    bool shadowsActive = false;
    uint32_t shadowDepthTex = 0;

    Font* defaultFont{};
    TerrainBlock* terrain{};
    Sky* sky{};
    Shader* defaultShader{};
    Shader* currentShader{};
    MatrixF projection;
    MatrixF view;
    Point3F cameraPos;
    Point3F cameraTarget;
    Point3F sunDir{0.5f, 0.8f, 0.6f};
    std::string gpuInfo;
    std::vector<DynamicPointLight> dynamicLights;

    // Stats
    struct Stats {
        int32_t drawCalls = 0;
        int32_t triangles = 0;
        int32_t textures = 0;
    } stats;

private:
    struct Impl;
    Impl* impl;
    RenderConfig cfg;
    bool initialized = false;

    // Shadow mapping internals
    int32_t shadowSize = 0;
    uint32_t shadowFbo = 0;
    MatrixF shadowVP;           // light's view-projection
    MatrixF shadowBiasVP;       // bias * lightVP (world -> shadow UV)
    MatrixF shadowBiasMatrix;   // bias for NDC -> UV

    // Font cache
    std::unordered_map<std::string, Font*> fontCache;

    void initSpriteVAO();
    void spriteBatchAdd(float* verts, uint32_t texId, bool additive = false);
    void spriteBatchFlush();

    // Sprite batch accumulator
    std::vector<float> spriteBatchBuf;
    uint32_t spriteBatchTex = UINT32_MAX;
    bool spriteBatchAdditive = false;
    bool spriteBatchDepthTest = true;
    bool spriteBatchDepthWrite = true;
    bool spriteBatchBlend = true;
    int spriteBatchCount = 0;
    static constexpr int SPRITE_BATCH_MAX = 512; // max rects per batch
    static constexpr int MAX_DYNAMIC_LIGHTS = 8;

    // Persistent 2D sprite/line rendering (created once to avoid per-frame glGen/glDelete)
    uint32_t spriteVAO = 0, spriteVBO = 0, spriteEBO = 0;
    uint32_t lineVAO = 0, lineVBO = 0;
};
