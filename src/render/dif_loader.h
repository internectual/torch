#pragma once
#include "render/renderer.h"
#include "render/dif_lighting.h"
#include <vector>
#include <string>
#include <cstdint>

struct DIFLoadResult {
    std::vector<MeshData> meshes;
    std::vector<Texture> textures;
    std::vector<uint32_t> materialFlags;
    std::vector<int16_t> materialLightmapIndex;
    // Per-material alarm lightmap (the normal one without an alarm state).
    std::vector<int16_t> materialAlarmLightmapIndex;
    std::vector<Texture> lightmaps;
    std::vector<std::string> materialNames;
    std::vector<DTSShape::DetailLevel> details;
    bool loaded = false;
    // Interior::mHasAlarmState of the highest detail (an alarm lightmap set).
    bool hasAlarmState = false;
    // Animated lights of the highest detail and their relight plans.
    DIFLightingData lighting;
    // Interior::mBaseAmbientColor / mAlarmAmbientColor (RGBA).
    uint8_t baseAmbient[4] = {0, 0, 0, 0}, alarmAmbient[4] = {0, 0, 0, 0};
    // Interior::mBoundingBox of the highest detail (interior space).
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
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
