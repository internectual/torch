#pragma once
#include "render/renderer.h"
#include <string>
#include <vector>

struct MaterialInfo {
    std::string resourcePath; // e.g. "skins/base.lbioderm" or "lush/BE_EWAL01B"
    int embeddedTextureIndex = -1; // index into GLB textures array, -1 if none
};

struct GLBMesh {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures; // loaded embedded textures
    std::vector<MaterialInfo> materials; // per-GLB-material metadata
    std::string name;
};

GLBMesh loadGLB(const uint8_t* data, size_t size);
GLBMesh loadGLBFromFile(const std::string& path);
