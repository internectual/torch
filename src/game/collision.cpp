#include "game/collision.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

static inline float triArea2D(const Point3F& a, const Point3F& b, const Point3F& c) {
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

static inline bool pointInTri3D(const Point3F& p, const CollisionTri& tri) {
    const Point3F v0{tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    const Point3F v1{tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    const Point3F v2{p.x - tri.v0.x, p.y - tri.v0.y, p.z - tri.v0.z};
    const float d00 = v0.x * v0.x + v0.y * v0.y + v0.z * v0.z;
    const float d01 = v0.x * v1.x + v0.y * v1.y + v0.z * v1.z;
    const float d11 = v1.x * v1.x + v1.y * v1.y + v1.z * v1.z;
    const float d20 = v2.x * v0.x + v2.y * v0.y + v2.z * v0.z;
    const float d21 = v2.x * v1.x + v2.y * v1.y + v2.z * v1.z;
    const float denominator = d00 * d11 - d01 * d01;
    if (std::fabs(denominator) < 1e-10f) return false;
    const float baryV = (d11 * d20 - d01 * d21) / denominator;
    const float baryW = (d00 * d21 - d01 * d20) / denominator;
    return baryV >= -1e-4f && baryW >= -1e-4f && baryV + baryW <= 1.0001f;
}

static Point3F closestPointOnTriangle(const Point3F& p, const CollisionTri& tri) {
    const Point3F ab{tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    const Point3F ac{tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    const Point3F ap{p.x - tri.v0.x, p.y - tri.v0.y, p.z - tri.v0.z};
    const float d1 = ab.x * ap.x + ab.y * ap.y + ab.z * ap.z;
    const float d2 = ac.x * ap.x + ac.y * ap.y + ac.z * ap.z;
    if (d1 <= 0.0f && d2 <= 0.0f) return tri.v0;
    const Point3F bp{p.x - tri.v1.x, p.y - tri.v1.y, p.z - tri.v1.z};
    const float d3 = ab.x * bp.x + ab.y * bp.y + ab.z * bp.z;
    const float d4 = ac.x * bp.x + ac.y * bp.y + ac.z * bp.z;
    if (d3 >= 0.0f && d4 <= d3) return tri.v1;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return {tri.v0.x + v * ab.x, tri.v0.y + v * ab.y, tri.v0.z + v * ab.z};
    }
    const Point3F cp{p.x - tri.v2.x, p.y - tri.v2.y, p.z - tri.v2.z};
    const float d5 = ab.x * cp.x + ab.y * cp.y + ab.z * cp.z;
    const float d6 = ac.x * cp.x + ac.y * cp.y + ac.z * cp.z;
    if (d6 >= 0.0f && d5 <= d6) return tri.v2;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return {tri.v0.x + w * ac.x, tri.v0.y + w * ac.y, tri.v0.z + w * ac.z};
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {tri.v1.x + w * (tri.v2.x - tri.v1.x),
                tri.v1.y + w * (tri.v2.y - tri.v1.y),
                tri.v1.z + w * (tri.v2.z - tri.v1.z)};
    }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    return {tri.v0.x + ab.x * v + ac.x * w,
            tri.v0.y + ab.y * v + ac.y * w,
            tri.v0.z + ab.z * v + ac.z * w};
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
        if (ix0 < 0) { ix0 = 0; } if (ix0 >= resX) { ix0 = resX - 1; }
        if (iz0 < 0) { iz0 = 0; } if (iz0 >= resZ) { iz0 = resZ - 1; }
        if (ix1 < 0) { ix1 = 0; } if (ix1 >= resX) { ix1 = resX - 1; }
        if (iz1 < 0) { iz1 = 0; } if (iz1 >= resZ) { iz1 = resZ - 1; }

        for (int iz = iz0; iz <= iz1; iz++)
            for (int ix = ix0; ix <= ix1; ix++)
                cells[iz * resX + ix].push_back(ti);
    }
}

namespace {
// True if (x,z) lies inside the XZ-projection of triangle t.
bool pointInTriXZ(float x, float z, const CollisionTri& t) {
    auto cross = [](float ax, float az, float bx, float bz, float cx, float cz) {
        return (ax - cx) * (bz - cz) - (bx - cx) * (az - cz);
    };
    float d1 = cross(x, z, t.v0.x, t.v0.z, t.v1.x, t.v1.z);
    float d2 = cross(x, z, t.v1.x, t.v1.z, t.v2.x, t.v2.z);
    float d3 = cross(x, z, t.v2.x, t.v2.z, t.v0.x, t.v0.z);
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);
}
}

bool CollisionGrid::raycast(const std::vector<CollisionTri>& tris, const Point3F& origin,
    const Point3F& dir, float maxDist, float& outT, Point3F& outPos, Point3F& outNormal, int* outTriIdx) const
{
    if (resX == 0 || resZ == 0) return false;
    // Vertical (straight up/down) ray: only the column cell at (x,z) can intersect.
    if (fabs(dir.x) < 1e-10f && fabs(dir.z) < 1e-10f) {
        int ix = (int)((origin.x - minX) / cellW);
        int iz = (int)((origin.z - minZ) / cellH);
        if (ix < 0 || iz < 0 || ix >= resX || iz >= resZ) return false;
        float bestT = maxDist;
        bool hit = false;
        Point3F bestNorm{}; int bestIdx = -1;
        for (int ti : cells[iz * resX + ix]) {
            const auto& tri = tris[ti];
            float denom = tri.normal.y * dir.y;
            if (fabs(denom) < 1e-8f) continue;
            float tPlane = (tri.normal.x * (tri.v0.x - origin.x)
                          + tri.normal.y * (tri.v0.y - origin.y)
                          + tri.normal.z * (tri.v0.z - origin.z)) / denom;
            if (tPlane < 0 || tPlane > maxDist) continue;
            if (!pointInTriXZ(origin.x, origin.z, tri)) continue;
            if (tPlane < bestT) { bestT = tPlane; hit = true; bestNorm = tri.normal; bestIdx = ti; }
        }
        if (hit) {
            outT = bestT;
            outPos = { origin.x, origin.y + dir.y * bestT, origin.z };
            outNormal = bestNorm;
            if (outTriIdx) *outTriIdx = bestIdx;
            return true;
        }
        return false;
    }

    float gridMinX = minX, gridMaxX = minX + resX * cellW;
    float gridMinZ = minZ, gridMaxZ = minZ + resZ * cellH;
    float entry = 0.0f, exit = maxDist;
    auto clipAxis = [&](float originValue, float direction, float minValue, float maxValue) {
        if (std::fabs(direction) < 1e-10f)
            return originValue >= minValue && originValue <= maxValue;
        float nearT = (minValue - originValue) / direction;
        float farT = (maxValue - originValue) / direction;
        if (nearT > farT) std::swap(nearT, farT);
        entry = std::max(entry, nearT);
        exit = std::min(exit, farT);
        return entry <= exit;
    };
    if (!clipAxis(origin.x, dir.x, gridMinX, gridMaxX) ||
        !clipAxis(origin.z, dir.z, gridMinZ, gridMaxZ) || exit < 0.0f || entry > maxDist)
        return false;
    entry = std::max(0.0f, entry);
    float bestT = maxDist;
    bool hit = false;
    Point3F bestNorm;
    int bestIdx = -1;

    // Traverse grid cells along ray
    const Point3F entryPoint{origin.x + dir.x * entry, origin.y + dir.y * entry,
                             origin.z + dir.z * entry};
    float t = entry;
    float stepX = (dir.x != 0) ? (cellW / fabs(dir.x)) : 1e10f;
    float stepZ = (dir.z != 0) ? (cellH / fabs(dir.z)) : 1e10f;
    int stepIx = (dir.x >= 0) ? 1 : -1;
    int stepIz = (dir.z >= 0) ? 1 : -1;

    int cx, cz;
    cx = (int)((entryPoint.x - minX) / cellW);
    cz = (int)((entryPoint.z - minZ) / cellH);
    if (cx < 0) { cx = 0; } if (cx >= resX) { cx = resX - 1; }
    if (cz < 0) { cz = 0; } if (cz >= resZ) { cz = resZ - 1; }

    float tMaxX = 1e30f;
    if (std::fabs(dir.x) > 1e-10f) {
        tMaxX = (dir.x > 0)
            ? ((cx + 1) * cellW + minX - origin.x) / dir.x
            : (cx * cellW + minX - origin.x) / dir.x;
    }
    float tMaxZ = 1e30f;
    if (std::fabs(dir.z) > 1e-10f) {
        tMaxZ = (dir.z > 0)
            ? ((cz + 1) * cellH + minZ - origin.z) / dir.z
            : (cz * cellH + minZ - origin.z) / dir.z;
    }

    while (t <= exit && t <= maxDist) {
        const float nextCellT = std::min(tMaxX, tMaxZ);
        // Test triangles in current cell
        auto& cellTris = cells[cz * resX + cx];
        for (int ti : cellTris) {
            const auto& tri = tris[ti];
            float tt, u, vv;
            if (rayTriIntersect(origin, dir, tri.v0, tri.v1, tri.v2, tt, u, vv)) {
                // A triangle is indexed in every AABB cell it touches. Do
                // not accept an intersection that belongs to a later cell.
                if (tt >= t - 1e-5f && tt <= nextCellT + 1e-5f && tt < bestT) {
                    bestT = tt;
                    bestNorm = tri.normal;
                    bestIdx = ti;
                    hit = true;
                }
            }
        }

        if (hit && bestT <= nextCellT + 1e-5f) break;

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
    float bestPenetration = 0.0f;

    for (int dz = -range; dz <= range; dz++) {
        for (int dx = -range; dx <= range; dx++) {
            int gx = cx + dx, gz = cz + dz;
            if (gx < 0 || gx >= resX || gz < 0 || gz >= resZ) continue;
            for (int ti : cells[gz * resX + gx]) {
                const auto& tri = tris[ti];
                const Point3F closest = closestPointOnTriangle(center, tri);
                Point3F delta{center.x - closest.x, center.y - closest.y, center.z - closest.z};
                const float distanceSquared = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                if (distanceSquared > radius * radius) continue;
                float distance = std::sqrt(distanceSquared);
                if (distance < 1e-6f) {
                    delta = tri.normal;
                    distance = 1.0f;
                }
                const float pen = radius - distance;
                if (pen > bestPenetration) {
                    const float invDistance = 1.0f / distance;
                    pushOut = {delta.x * invDistance * pen,
                               delta.y * invDistance * pen,
                               delta.z * invDistance * pen};
                    bestPenetration = pen;
                    collided = true;
                }
            }
        }
    }
    return collided;
}

void CollisionMesh::addMesh(const float* verts, int vertCount, const uint32_t* indices, int indexCount) {
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

    const int ix = (int)((x - grid.minX) / grid.cellW);
    const int iz = (int)((z - grid.minZ) / grid.cellH);
    if (ix < 0 || iz < 0 || ix >= grid.resX || iz >= grid.resZ) return -1e10f;

    float bestHeight = -1e10f;
    for (int triIndex : grid.cells[iz * grid.resX + ix]) {
        const auto& tri = triangles[triIndex];
        // Downward-facing surfaces are ceilings or underside geometry.
        if (tri.normal.y <= 0.001f || !pointInTriXZ(x, z, tri)) continue;
        const float height = tri.v0.y -
            (tri.normal.x * (x - tri.v0.x) + tri.normal.z * (z - tri.v0.z)) /
            tri.normal.y;
        bestHeight = std::max(bestHeight, height);
    }
    return bestHeight;
}

float CollisionMesh::getFloorHeight(float x, float y, float z) const {
    if (!loaded) return -1e10f;
    float t = 0.0f;
    Point3F position{}, normal{};
    const float maxDist = 20000.0f;
    if (raycast({x, y + 0.001f, z}, {0, -1, 0}, maxDist, t, position, normal))
        return position.y;
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
