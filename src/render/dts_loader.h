#pragma once
#include "render/renderer.h"
#include <vector>
#include <string>
#include <cstdint>

struct DTSLoadResult {
    std::vector<MeshData> meshes;
    std::vector<SkinInfo> skins;            // parallel to meshes
    std::vector<Texture> textures;
    std::vector<uint32_t> materialFlags;
    std::vector<int8_t> materialLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    std::vector<DTSShape::Animation> animations;
    std::vector<DTSShape::Node> nodes;
    std::vector<MatrixF> defaultTransforms; // per-node default (bind pose) world transforms
    std::vector<MatrixF> defaultLocalTransforms; // per-node default (bind pose) local transforms
    bool loaded = false;
};

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name);
bool updateSkinnedMesh(MeshData& mesh, SkinInfo& skin,
                       const std::vector<MatrixF>& nodeWorld,
                       const std::vector<MatrixF>& initialTransforms);
