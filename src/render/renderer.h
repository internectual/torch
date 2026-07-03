#pragma once
#include "core/math.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

struct SkinInfo {
    bool hasSkin = false;
    std::vector<Point3F> initialPositions;
    std::vector<Point3F> initialNormals;
    std::vector<MatrixF> initialTransforms;
    std::vector<int32_t> boneIndices;
    std::vector<float> boneWeights;
    std::vector<int32_t> nodeIndices;
};

struct RenderConfig {
    int32_t width = 1024;
    int32_t height = 768;
    bool fullscreen = false;
    bool vsync = true;
    float fov = 90.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    int32_t maxLights = 8;
    float gamma = 1.0f;
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
    std::vector<uint32_t> indices;
    uint32_t vao{}, vbo{}, ebo{};
    int32_t materialIndex = -1; // index into DTSShape::materialTextures
    int32_t materialIdx = -1;   // raw material index from GLB file
    int32_t nodeIndex = -1;     // DTS node index (-1 = no node)
    bool uploaded = false;
    void upload();
    void render();
    void destroy();
};

struct Texture {
    uint32_t id{};
    int32_t width{}, height{};
    int32_t format{};
    bool loaded = false;
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

struct Font {
    uint32_t texture{};
    int32_t charWidth{}, charHeight{};
    int32_t texWidth{}, texHeight{};
    float charUV[256][4]{};
    bool loaded = false;
    bool load(const uint8_t* data, size_t size);
    bool loadDefault();
    void render(const char* text, float x, float y, const ColorF& color, float scale = 1.0f);
    Point2F measure(const char* text, float scale = 1.0f);
};

struct DTSShape {
    // DTS shape data
    std::string name;
    std::vector<MeshData> meshes;
    struct DetailLevel {
        float size;
        int32_t meshIndex;
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
    };
    struct Animation {
        std::string name;
        float duration;
        bool looping = false;
        std::vector<Keyframe> keyframes;
    };
    std::vector<Animation> animations;
    std::vector<SkinInfo> skins; // parallel to meshes
    std::vector<MatrixF> defaultTransforms; // per-node bind pose transforms
    std::vector<std::string> materialNames; // original material names (for skin overrides)
    std::vector<Texture> materialTextures;
    std::vector<uint32_t> materialFlags; // parallel to materialTextures
    std::vector<float> materialMetallic; // parallel to materialTextures
    std::vector<float> materialRoughness; // parallel to materialTextures
    std::vector<Texture> lightmaps;
    std::vector<int8_t> materialLightmapIndex; // per-material: -1 no lightmap, >=0 index into lightmaps[]
    bool isInterior = false;
    bool loaded = false;
    // Hull collision data (from DIF files)
    std::vector<float> collisionVerts;
    std::vector<uint32_t> collisionIndices;
    bool load(const uint8_t* data, size_t size);
    bool loadGLB(const uint8_t* data, size_t size);
    void render(int32_t detailLevel = 0);
    void renderAnimation(const char* animName, float time);
    bool applySkin(const std::string& skinName);
};

struct TerrainBlock {
    int32_t size{256};
    float heightScale{1.0f};
    float squareSize{8.0f};
    Point3F worldOffset{-1024,-1024,0};
    std::vector<float> heights;
    std::vector<MeshData> meshes;
    std::vector<Texture> detailTextures;
    Texture splatMap;
    Texture lightmap;
    std::vector<std::string> textureNames;
    bool loaded = false;

    float sampleHeight(float wx, float wz) const;
    bool load(const uint8_t* data, size_t size);
    void generateMesh();
    void render(const Point3F& cameraPos, bool fogEnabled = false, const ColorF& fogColor = {0.5f, 0.6f, 0.7f, 1.0f}, float fogDensity = 0.005f, const Point3F* lightDir = nullptr);
};

struct Sky {
    uint32_t cubemap{};
    uint32_t vao{}, vbo{};
    Texture emap;
    bool loaded = false;
    void load(const std::vector<std::string>& faces);
    void render(const MatrixF& view, const MatrixF& proj);
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

    void drawMesh(MeshData& mesh, const MatrixF& transform);
    void drawLine(const Point3F& a, const Point3F& b, const ColorF& color);
    void drawLineStrip(const std::vector<Point3F>& points, const ColorF& color);
    void drawBox(const Box3F& box, const ColorF& color);
    void drawRectFill(const Point3F& a, const Point3F& b, const ColorF& color);
    void drawSprite(const Point3F& pos, float size, const ColorF& color, uint32_t texture = 0);
    void drawTexturedRect(const Point3F& a, const Point3F& b, uint32_t texture);

    Texture* loadTexture(const char* path);
    Shader* loadShader(const char* vertPath, const char* fragPath);
    void addShader(Shader* shader);
    void addTexture(Texture* tex);

    Font* getFont() { return defaultFont; }
    void renderText(const char* text, float x, float y, const ColorF& color, float scale = 1.0f);

    TerrainBlock* getTerrain() { return terrain; }
    Sky* getSky() { return sky; }

    RenderConfig& config() { return cfg; }
    void onResize(int32_t w, int32_t h);
    bool screenshot(const char* path);

    // Shadow mapping
    void initShadowMap(int32_t size = 2048);
    void beginShadowPass(const Point3F& lightDir, const Point3F& sceneCenter, float sceneRadius);
    void endShadowPass();
    const MatrixF& shadowMatrix() const { return shadowBiasVP; }
    const MatrixF& lightViewProj() const { return shadowVP; }
    bool shadowEnabled() const { return shadowSize > 0; }
    uint32_t shadowDepthTex = 0;

    Font* defaultFont{};
    TerrainBlock* terrain{};
    Sky* sky{};
    Shader* defaultShader{};
    Shader* currentShader{};
    MatrixF projection;
    MatrixF view;
    Point3F cameraPos;
    Point3F sunDir{0.5f, 0.8f, 0.6f};

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
};
