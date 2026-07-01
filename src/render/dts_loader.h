#pragma once
#include "render/renderer.h"
#include <vector>
#include <string>
#include <cstdint>

struct DTSLoadResult {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures;
    std::vector<uint32_t> materialFlags;
    std::vector<int8_t> materialLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames; // original material name/path per-texture
    std::vector<DTSShape::DetailLevel> details;
    std::vector<DTSShape::Animation> animations;
    std::vector<DTSShape::Node> nodes;
    bool loaded = false;
};

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name);
