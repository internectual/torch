#pragma once
#include "core/math.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

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
    ColorF color = {1,1,1,1};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t vao{}, vbo{}, ebo{};
    int32_t materialIndex = -1; // index into DTSShape::materialTextures
    int32_t materialIdx = -1;   // raw material index from GLB file
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
    bool load(const char* vertSrc, const char* fragSrc);
    bool loadFromFiles(const char* vertPath, const char* fragPath);
    void bind();
    void setUniform(const char* name, float v);
    void setUniform(const char* name, const Point3F& v);
    void setUniform(const char* name, const MatrixF& m);
    void setUniform(const char* name, int32_t v);
    void destroy();
};

struct Font {
    uint32_t texture{};
    int32_t charWidth{}, charHeight{};
    int32_t texWidth{}, texHeight{};
    float charUV[256][4]{};
    bool loaded = false;
    bool load(const uint8_t* data, size_t size);
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
    struct Animation {
        std::string name;
        float duration;
        // Keyframe data would go here
    };
    std::vector<Animation> animations;
    std::vector<Texture> materialTextures;
    bool loaded = false;
    bool load(const uint8_t* data, size_t size);
    bool loadGLB(const uint8_t* data, size_t size);
    void render(int32_t detailLevel = 0);
    void renderAnimation(const char* animName, float time);
};

struct TerrainBlock {
    int32_t size{256};
    float heightScale{100.0f};
    std::vector<float> heights;
    std::vector<ColorF> colors;
    std::vector<MeshData> meshes;
    bool loaded = false;

    bool load(const uint8_t* data, size_t size);
    void generateMesh();
    void render(const Point3F& cameraPos);
};

struct Sky {
    uint32_t cubemap{};
    uint32_t vao{}, vbo{};
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
    void setCamera(const Point3F& pos, const Point3F& target, const Point3F& up);

    void drawMesh(MeshData& mesh, const MatrixF& transform);
    void drawLine(const Point3F& a, const Point3F& b, const ColorF& color);
    void drawBox(const Box3F& box, const ColorF& color);

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

    Font* defaultFont{};
    TerrainBlock* terrain{};
    Sky* sky{};
    Shader* defaultShader{};
    Shader* currentShader{};
    MatrixF projection;
    MatrixF view;

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
};
