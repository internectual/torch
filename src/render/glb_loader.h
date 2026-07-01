#pragma once
#include "render/renderer.h"
#include <string>
#include <vector>

enum MaterialFlag : uint32_t {
    MatFlag_None = 0,
    MatFlag_Translucent = 1,
    MatFlag_Additive = 2,
    MatFlag_SelfIlluminating = 4,
    MatFlag_NeverEnvMap = 8,
    MatFlag_SWrap = 16,
    MatFlag_TWrap = 32,
};

struct MaterialInfo {
    std::string resourcePath; // e.g. "skins/base.lbioderm" or "lush/BE_EWAL01B"
    int embeddedTextureIndex = -1; // index into GLB textures array, -1 if none
    int emissiveTextureIndex = -1; // index into GLB textures for emissive/lightmap, -1 if none
    uint32_t flags = 0;
    float metallic = 0.0f;
    float roughness = 0.5f;
    ColorF baseColorFactor{1, 1, 1, 1};
};

struct GLBMesh {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures; // loaded embedded textures (base colors)
    std::vector<Texture> lightmaps; // loaded embedded lightmap textures
    std::vector<MaterialInfo> materials; // per-GLB-material metadata
    std::string name;
    std::vector<DTSShape::Animation> animations;
};

GLBMesh loadGLB(const uint8_t* data, size_t size);
GLBMesh loadGLBFromFile(const std::string& path);
