#include "game/collision.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

static inline float triArea2D(const Point3F& a, const Point3F& b, const Point3F& c) {
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

static inline bool pointInTri2D(const Point3F& p, const CollisionTri& tri) {
    float d0 = triArea2D(p, tri.v0, tri.v1);
    float d1 = triArea2D(p, tri.v1, tri.v2);
    float d2 = triArea2D(p, tri.v2, tri.v0);
    bool neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
    bool pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
    return !(neg && pos);
}

static inline bool rayTriIntersect(const Point3F& orig, const Point3F& dir,
    const Point3F& v0, const Point3F& v1, const Point3F& v2,
    float& t, float& u, float& v) {

    Point3F e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
    Point3F e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
    Point3F pvec = {
        dir.y * e2.z - dir.z * e2.y,
        dir.z * e2.x - dir.x * e2.z,
        dir.x * e2.y - dir.y * e2.x
    };
    float det = e1.x * pvec.x + e1.y * pvec.y + e1.z * pvec.z;
    if (det > -1e-8f && det < 1e-8f) return false;
    float invDet = 1.0f / det;
    Point3F tvec = {orig.x - v0.x, orig.y - v0.y, orig.z - v0.z};
    u = (tvec.x * pvec.x + tvec.y * pvec.y + tvec.z * pvec.z) * invDet;
    if (u < 0 || u > 1) return false;
    Point3F qvec = {
        tvec.y * e1.z - tvec.z * e1.y,
        tvec.z * e1.x - tvec.x * e1.z,
        tvec.x * e1.y - tvec.y * e1.x
    };
    v = (dir.x * qvec.x + dir.y * qvec.y + dir.z * qvec.z) * invDet;
    if (v < 0 || u + v > 1) return false;
    t = (e2.x * qvec.x + e2.y * qvec.y + e2.z * qvec.z) * invDet;
    return t >= 0;
}

void CollisionGrid::build(const std::vector<CollisionTri>& tris, float gridSize, int resolution) {
    resX = resolution;
    resZ = resolution;
    minX = -gridSize * 0.5f;
    minZ = -gridSize * 0.5f;
    cellW = gridSize / resolution;
    cellH = gridSize / resolution;
    cells.resize(resX * resZ);

    for (int ti = 0; ti < (int)tris.size(); ti++) {
        const auto& tri = tris[ti];
        float ax = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
        float az = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
        float bx = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
        float bz = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

        int ix0 = (int)((ax - minX) / cellW);
        int iz0 = (int)((az - minZ) / cellH);
        int ix1 = (int)((bx - minX) / cellW);
        int iz1 = (int)((bz - minZ) / cellH);
        if (ix0 < 0) ix0 = 0; if (ix0 >= resX) ix0 = resX - 1;
        if (iz0 < 0) iz0 = 0; if (iz0 >= resZ) iz0 = resZ - 1;
        if (ix1 < 0) ix1 = 0; if (ix1 >= resX) ix1 = resX - 1;
        if (iz1 < 0) iz1 = 0; if (iz1 >= resZ) iz1 = resZ - 1;

        for (int iz = iz0; iz <= iz1; iz++)
            for (int ix = ix0; ix <= ix1; ix++)
                cells[iz * resX + ix].push_back(ti);
    }
}

bool CollisionGrid::raycast(const std::vector<CollisionTri>& tris, const Point3F& origin,
    const Point3F& dir, float maxDist, float& outT, Point3F& outPos, Point3F& outNormal, int* outTriIdx) const
{
    if (resX == 0 || resZ == 0) return false;
    if (fabs(dir.x) < 1e-10f && fabs(dir.z) < 1e-10f) return false;

    float bestT = maxDist;
    bool hit = false;
    Point3F bestNorm;
    int bestIdx = -1;

    // Traverse grid cells along ray
    float t = 0;
    float stepX = (dir.x != 0) ? (cellW / fabs(dir.x)) : 1e10f;
    float stepZ = (dir.z != 0) ? (cellH / fabs(dir.z)) : 1e10f;
    int stepIx = (dir.x >= 0) ? 1 : -1;
    int stepIz = (dir.z >= 0) ? 1 : -1;

    int cx, cz;
    if (fabs(dir.x) > 1e-10f) {
        cx = (int)((origin.x - minX + (dir.x > 0 ? 0 : 0)) / cellW);
    } else cx = 0;
    if (fabs(dir.z) > 1e-10f) {
        cz = (int)((origin.z - minZ + (dir.z > 0 ? 0 : 0)) / cellH);
    } else cz = 0;
    if (cx < 0) cx = 0; if (cx >= resX) cx = resX - 1;
    if (cz < 0) cz = 0; if (cz >= resZ) cz = resZ - 1;

    float tMaxX = (dir.x > 0)
        ? ((cx + 1) * cellW + minX - origin.x) / dir.x
        : (cx * cellW + minX - origin.x) / dir.x;
    float tMaxZ = (dir.z > 0)
        ? ((cz + 1) * cellH + minZ - origin.z) / dir.z
        : (cz * cellH + minZ - origin.z) / dir.z;

    while (t < maxDist) {
        // Test triangles in current cell
        auto& cellTris = cells[cz * resX + cx];
        for (int ti : cellTris) {
            const auto& tri = tris[ti];
            float tt, u, vv;
            if (rayTriIntersect(origin, dir, tri.v0, tri.v1, tri.v2, tt, u, vv)) {
                if (tt >= 0 && tt < bestT) {
                    bestT = tt;
                    bestNorm = tri.normal;
                    bestIdx = ti;
                    hit = true;
                }
            }
        }

        if (hit) break;

        // Advance to next cell
        if (tMaxX < tMaxZ) {
            t = tMaxX;
            cx += stepIx;
            tMaxX += stepX;
        } else {
            t = tMaxZ;
            cz += stepIz;
            tMaxZ += stepZ;
        }
        if (cx < 0 || cx >= resX || cz < 0 || cz >= resZ) break;
    }

    if (hit) {
        outT = bestT;
        outPos = {origin.x + dir.x * bestT, origin.y + dir.y * bestT, origin.z + dir.z * bestT};
        outNormal = bestNorm;
        if (outTriIdx) *outTriIdx = bestIdx;
    }
    return hit;
}

bool CollisionGrid::sphereCollide(const std::vector<CollisionTri>& tris, const Point3F& center,
    float radius, Point3F& pushOut) const
{
    if (resX == 0 || resZ == 0) return false;

    int cx = (int)((center.x - minX) / cellW);
    int cz = (int)((center.z - minZ) / cellH);
    int range = (int)(radius / std::min(cellW, cellH)) + 1;

    bool collided = false;
    pushOut = {0, 0, 0};

    for (int dz = -range; dz <= range; dz++) {
        for (int dx = -range; dx <= range; dx++) {
            int gx = cx + dx, gz = cz + dz;
            if (gx < 0 || gx >= resX || gz < 0 || gz >= resZ) continue;
            for (int ti : cells[gz * resX + gx]) {
                const auto& tri = tris[ti];
                // Point-to-triangle distance
                Point3F e1 = {tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
                Point3F e2 = {tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
                Point3F toCenter = {center.x - tri.v0.x, center.y - tri.v0.y, center.z - tri.v0.z};

                // Project onto normal
                float nd = toCenter.x * tri.normal.x + toCenter.y * tri.normal.y + toCenter.z * tri.normal.z;
                if (nd > radius || nd < -radius) continue;

                // Check if point projects inside triangle
                Point3F proj = {center.x - tri.normal.x * nd, center.y - tri.normal.y * nd, center.z - tri.normal.z * nd};
                if (pointInTri2D(proj, tri)) {
                    float pen = radius - nd;
                    if (pen > 0) {
                        pushOut.x += tri.normal.x * pen;
                        pushOut.y += tri.normal.y * pen;
                        pushOut.z += tri.normal.z * pen;
                        collided = true;
                    }
                }
            }
        }
    }
    return collided;
}

void CollisionMesh::addMesh(const float* verts, int vertCount, const uint32_t* indices, int indexCount) {
    int startTri = (int)triangles.size();
    for (int i = 0; i + 2 < indexCount; i += 3) {
        CollisionTri tri;
        uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= (uint32_t)vertCount / 3 || i1 >= (uint32_t)vertCount / 3 || i2 >= (uint32_t)vertCount / 3) continue;
        tri.v0 = {verts[i0 * 3], verts[i0 * 3 + 1], verts[i0 * 3 + 2]};
        tri.v1 = {verts[i1 * 3], verts[i1 * 3 + 1], verts[i1 * 3 + 2]};
        tri.v2 = {verts[i2 * 3], verts[i2 * 3 + 1], verts[i2 * 3 + 2]};
        Point3F e1 = {tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
        Point3F e2 = {tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
        float nx = e1.y * e2.z - e1.z * e2.y;
        float ny = e1.z * e2.x - e1.x * e2.z;
        float nz = e1.x * e2.y - e1.y * e2.x;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
        tri.normal = {nx, ny, nz};
        triangles.push_back(tri);
    }
}

void CollisionMesh::build() {
    if (triangles.empty()) return;
    grid.build(triangles);
    loaded = true;
}

float CollisionMesh::getHeight(float x, float z) const {
    if (!loaded) return -1e10f;

    Point3F origin = {x, 10000.0f, z};
    Point3F dir = {0, -1, 0};
    float t;
    Point3F pos, norm;
    if (raycast(origin, dir, 20000.0f, t, pos, norm)) {
        return pos.y;
    }
    return -1e10f;
}

bool CollisionMesh::raycast(const Point3F& origin, const Point3F& dir, float maxDist, float& outT, Point3F& outPos, Point3F& outNormal) const {
    if (!loaded) return false;
    return grid.raycast(triangles, origin, dir, maxDist, outT, outPos, outNormal);
}

bool CollisionMesh::sphereCollide(const Point3F& center, float radius, Point3F& pushOut) const {
    if (!loaded) return false;
    return grid.sphereCollide(triangles, center, radius, pushOut);
}

bool CollisionMesh::lineOfSight(const Point3F& a, const Point3F& b) const {
    Point3F dir = {b.x - a.x, b.y - a.y, b.z - a.z};
    float dist = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dist < 0.001f) return true;
    dir.x /= dist; dir.y /= dist; dir.z /= dist;

    float t;
    Point3F pos, norm;
    // Start slightly ahead of origin to avoid self-intersection
    Point3F start = {a.x + dir.x * 0.1f, a.y + dir.y * 0.1f, a.z + dir.z * 0.1f};
    if (raycast(start, dir, dist, t, pos, norm))
        return false;
    return true;
}
