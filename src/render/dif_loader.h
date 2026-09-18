#pragma once
#include "render/renderer.h"
#include <vector>
#include <string>
#include <cstdint>

struct DIFLoadResult {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures;
    std::vector<uint32_t> materialFlags;
    std::vector<int16_t> materialLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    bool loaded = false;
    // Collision triangles extracted from hull surfaces
    std::vector<float> hullCollisionVerts;
    std::vector<uint32_t> hullCollisionIndices;
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
};

DIFLoadResult loadDIF(const uint8_t* data, size_t size, const char* name, bool skipGpu = false);
