#pragma once
#include "core/math.h"
#include <vector>
#include <cstdint>

struct CollisionTri {
    Point3F v0, v1, v2;
    Point3F normal;
};

struct CollisionGrid {
    int resX = 0, resZ = 0;
    float minX = 0, minZ = 0, cellW = 1, cellH = 1;
    std::vector<std::vector<int>> cells;

    void build(const std::vector<CollisionTri>& tris, float gridSize = 1000.0f, int resolution = 64);
    bool raycast(const std::vector<CollisionTri>& tris, const Point3F& origin, const Point3F& dir, float maxDist, float& outT, Point3F& outPos, Point3F& outNormal, int* outTriIdx = nullptr) const;
    bool sphereCollide(const std::vector<CollisionTri>& tris, const Point3F& center, float radius, Point3F& pushOut) const;
};

struct CollisionMesh {
    std::vector<CollisionTri> triangles;
    CollisionGrid grid;
    bool loaded = false;

    void addMesh(const float* verts, int vertCount, const uint32_t* indices, int indexCount);
    void build();
    float getHeight(float x, float z) const;
    // Select the highest walkable surface below the actor, rather than a
    // ceiling above it.  The unbounded overload is retained for terrain-style
    // callers that do not have an actor height.
    float getFloorHeight(float x, float y, float z) const;
    bool raycast(const Point3F& origin, const Point3F& dir, float maxDist, float& outT, Point3F& outPos, Point3F& outNormal) const;
    bool sphereCollide(const Point3F& center, float radius, Point3F& pushOut) const;
    bool lineOfSight(const Point3F& a, const Point3F& b) const;
};
