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
    struct IflMaterial { std::string name; int32_t materialSlot = -1; };
    std::vector<IflMaterial> iflMaterials; // TSShape::IflMaterial
    struct UtilityDetail { std::string name; std::vector<int32_t> meshIndices; };
    std::vector<UtilityDetail> utilityDetails; // negative-size details (Collision-N, LOS-N)
    struct ObjectDefault { float vis = 1.0f; int32_t frame = 0, matFrame = 0; };
    std::vector<ObjectDefault> objectDefaults; // TSShape::objectStates[object]
    std::vector<float> materialReflectionAmount;
    std::vector<int16_t> materialLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    std::vector<DTSShape::Animation> animations;
    std::vector<DTSShape::Node> nodes;
    std::vector<MatrixF> defaultTransforms; // per-node default (bind pose) world transforms
    std::vector<MatrixF> defaultLocalTransforms; // per-node default (bind pose) local transforms
    std::vector<int32_t> objectStartMesh; // per-object: first mesh index
    std::vector<int32_t> objectNumMeshes; // per-object: number of meshes
    std::vector<std::vector<Point2F>> meshTVerts; // per-mesh: all tvert data (numTVerts * numMatFrames)
    bool loaded = false;
};

DTSLoadResult loadDTS(const uint8_t* data, size_t size, const char* name);
// TSShape::importSequences: appends a DSQ's sequences, mapping its nodes to
// `nodes` by name. A non-empty alias renames the last imported sequence, as
// TSShapeConstructor does. Returns the number appended, or -1 on failure.
int importDSQ(const uint8_t* data, size_t size, const std::vector<DTSShape::Node>& nodes,
              const std::string& alias, std::vector<DTSShape::Animation>& out);
bool updateSkinnedMesh(MeshData& mesh, SkinInfo& skin,
                       const std::vector<MatrixF>& nodeWorld,
                       const std::vector<MatrixF>& initialTransforms);
