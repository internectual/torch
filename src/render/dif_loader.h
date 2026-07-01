#pragma once
#include "render/renderer.h"
#include <vector>
#include <string>
#include <cstdint>

struct DIFLoadResult {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures;
    std::vector<uint32_t> materialFlags;
    std::vector<int8_t> materialLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    bool loaded = false;
    // Collision triangles extracted from hull surfaces
    std::vector<float> hullCollisionVerts;
    std::vector<uint32_t> hullCollisionIndices;
};

DIFLoadResult loadDIF(const uint8_t* data, size_t size, const char* name, bool skipGpu = false);
