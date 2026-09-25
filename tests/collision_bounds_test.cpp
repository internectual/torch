#include "game/collision.h"

#include <cassert>
#include <cmath>

int main() {
    const float vertices[] = {
        1500.0f, 0.0f, 1500.0f,
        1500.0f, 0.0f, 1510.0f,
        1510.0f, 0.0f, 1500.0f,
    };
    const uint32_t indices[] = {0, 1, 2};

    CollisionMesh mesh;
    mesh.addMesh(vertices, 9, indices, 3);
    mesh.build();

    float hitT = 0.0f;
    Point3F hit{}, normal{};
    assert(mesh.raycast({1502.0f, 10.0f, 1502.0f}, {0, -1, 0}, 20.0f,
                        hitT, hit, normal));
    assert(std::fabs(hit.y) < 1e-4f);
    assert(std::fabs(mesh.getFloorHeight(1502.0f, 10.0f, 1502.0f)) < 1e-4f);
    // Height queries use the same inclusive upper bounds as collision rays.
    assert(std::fabs(mesh.getFloorHeight(1510.0f, 10.0f, 1500.0f)) < 1e-4f);
    assert(std::fabs(mesh.getHeight(1500.0f, 1510.0f)) < 1e-4f);
    // A fast-moving actor can query after crossing below the floor. The
    // authored surface must still be returned so physics can recover.
    assert(std::fabs(mesh.getFloorHeight(1502.0f, -1.0f, 1502.0f)) < 1e-4f);

    // Incomplete replicated positions must not enter the grid with invalid
    // coordinates and produce an arbitrary floor or out-of-bounds cell.
    assert(mesh.getHeight(NAN, 1502.0f) < -1.0e9f);
    assert(mesh.getHeight(1502.0f, INFINITY) < -1.0e9f);
    assert(mesh.getFloorHeight(1502.0f, NAN, 1502.0f) < -1.0e9f);
    assert(mesh.getFloorHeight(INFINITY, 10.0f, 1502.0f) < -1.0e9f);

    // A floor query is height-aware in a multi-level interior. The generic
    // height query may find the upper surface, but a projectile on the lower
    // level must resolve against the lower floor.
    const float stackedVertices[] = {
        0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        0.0f, 10.0f, 0.0f,  0.0f, 10.0f, 1.0f,  1.0f, 10.0f, 0.0f,
    };
    const uint32_t stackedIndices[] = {0, 2, 1, 3, 4, 5};
    CollisionMesh stacked;
    stacked.addMesh(stackedVertices, 18, stackedIndices, 6);
    stacked.build();
    assert(std::fabs(stacked.getHeight(0.25f, 0.25f) - 10.0f) < 1e-4f);
    assert(std::fabs(stacked.getFloorHeight(0.25f, 2.0f, 0.25f)) < 1e-4f);
    assert(std::fabs(stacked.getFloorHeight(0.25f, 12.0f, 0.25f) - 10.0f) < 1e-4f);

    // A ceiling below an actor is not a floor fallback. Treating it as one
    // makes players crossing malformed/legacy interior geometry snap upward.
    const float ceilingVertices[] = {
        0.0f, 2.0f, 0.0f,  0.0f, 2.0f, 1.0f,  1.0f, 2.0f, 0.0f,
    };
    const uint32_t ceilingIndices[] = {0, 2, 1};
    CollisionMesh ceilingOnly;
    ceilingOnly.addMesh(ceilingVertices, 9, ceilingIndices, 3);
    ceilingOnly.build();
    assert(ceilingOnly.getFloorHeight(0.25f, 3.0f, 0.25f) < -1.0e9f);

    // Height queries outside the negative edge must not alias the first cell.
    assert(mesh.getHeight(1498.99f, 1502.0f) < -1.0e9f);
    // The same boundary rule applies to vertical collision rays used for
    // floor and line-of-sight queries.
    assert(!mesh.raycast({1498.99f, 10.0f, 1502.0f}, {0, -1, 0}, 20.0f,
                         hitT, hit, normal));

    // The inclusive upper edge of a collision grid still belongs to its
    // final cell for vertical floor queries.
    CollisionTri edgeFloor{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
    CollisionGrid edgeGrid;
    edgeGrid.resX = edgeGrid.resZ = 1;
    edgeGrid.minX = edgeGrid.minZ = 0.0f;
    edgeGrid.cellW = edgeGrid.cellH = 1.0f;
    edgeGrid.cells = {{0}};
    float edgeT = 0.0f;
    Point3F edgeHit{}, edgeNormal{};
    assert(edgeGrid.raycast({edgeFloor}, {1.0f, 1.0f, 0.0f}, {0, -1, 0}, 2.0f,
                            edgeT, edgeHit, edgeNormal));
    assert(std::fabs(edgeHit.y) < 1e-4f);

    // A vertical wall has a zero-area XZ projection. It must not look like a
    // floor to a vertical ray that happens to share its X coordinate.
    const float wallOnlyVertices[] = {
        0.0f, 0.0f, -2.0f,  0.0f, 2.0f, -2.0f,  0.0f, 1.0f, 2.0f,
    };
    CollisionMesh wallOnly;
    wallOnly.addMesh(wallOnlyVertices, 9, indices, 3);
    wallOnly.build();
    assert(!wallOnly.raycast({0.0f, 3.0f, 0.0f}, {0, -1, 0}, 10.0f,
                             hitT, hit, normal));

    Point3F push{};
    assert(mesh.sphereCollide({1502.0f, -0.25f, 1502.0f}, 0.5f, push));
    assert(std::fabs(push.y) > 0.0f);
    push = {};
    assert(mesh.sphereCollide({1502.0f, 0.0f, 1502.0f}, 0.5f, push));
    assert(std::fabs(std::fabs(push.y) - 0.5f) < 1e-4f);

    // A player-sized sphere may be just outside the broadphase bounds while
    // still touching geometry at the edge of the authored mesh.
    push = {};
    assert(mesh.sphereCollide({1498.75f, 0.0f, 1502.0f}, 1.3f, push));

    // Zero-area faces are not physical surfaces and must not create a player
    // push-out along their collapsed edge.
    const float degenerateVertices[] = {
        0.0f, 0.0f, 0.0f,  2.0f, 0.0f, 0.0f,  4.0f, 0.0f, 0.0f,
    };
    CollisionMesh degenerate;
    degenerate.addMesh(degenerateVertices, 9, indices, 3);
    degenerate.build();
    assert(degenerate.triangles.empty());
    assert(!degenerate.sphereCollide({1.0f, 0.0f, 0.0f}, 1.0f, push));

    // A short sight line must not be treated as clear just because it is
    // shorter than the origin self-intersection bias.
    const float wallVertices[] = {
        0.03f, -1.0f, -1.0f,
        0.03f, 1.0f, -1.0f,
        0.03f, 1.0f, 1.0f,
    };
    CollisionMesh shortLineMesh;
    shortLineMesh.addMesh(wallVertices, 9, indices, 3);
    shortLineMesh.build();
    assert(!shortLineMesh.lineOfSight({0.0f, 0.0f, 0.0f},
                                      {0.05f, 0.0f, 0.0f}));

    const float closeWallVertices[] = {
        0.01f, -1.0f, -1.0f,
        0.01f, 1.0f, -1.0f,
        0.01f, 1.0f, 1.0f,
    };
    CollisionMesh closeWallMesh;
    closeWallMesh.addMesh(closeWallVertices, 9, indices, 3);
    closeWallMesh.build();
    // The origin bias must not hide a wall only one centimeter away.
    assert(!closeWallMesh.lineOfSight({0.0f, 0.0f, 0.0f},
                                      {0.02f, 0.0f, 0.0f}));

    const float pointBlankWallVertices[] = {
        0.0005f, -1.0f, -1.0f,  0.0005f, 1.0f, -1.0f,
        0.0005f, 1.0f, 1.0f,
    };
    CollisionMesh pointBlankWall;
    pointBlankWall.addMesh(pointBlankWallVertices, 9, indices, 3);
    pointBlankWall.build();
    // Even sub-millimeter sight lines must respect intervening geometry.
    assert(!pointBlankWall.lineOfSight({0.0f, 0.0f, 0.0f},
                                       {0.0008f, 0.0f, 0.0f}));
    // Invalid replicated endpoints must fail closed instead of treating a
    // NaN distance as an unobstructed sight line.
    assert(!pointBlankWall.lineOfSight({NAN, 0.0f, 0.0f},
                                       {0.02f, 0.0f, 0.0f}));

    CollisionGrid safeGrid;
    safeGrid.build(mesh.triangles, 0.0f, 0);
    assert(safeGrid.resX == 1 && safeGrid.resZ == 1);

    // Crossing an X/Z cell corner must enter the diagonal cell. Otherwise a
    // ray can skip a surface that starts just beyond the corner.
    const CollisionTri diagonalSurface{
        {1.6f, 1.76f, 1.6f}, {1.9f, 1.76f, 1.6f}, {1.6f, 1.76f, 1.9f},
        {0.0f, 1.0f, 0.0f}};
    CollisionGrid diagonalGrid;
    diagonalGrid.build({diagonalSurface}, 0.0f, 2);
    float diagonalT = 0.0f;
    Point3F diagonalHit{}, diagonalNormal{};
    assert(diagonalGrid.raycast({diagonalSurface}, {0.5f, 2.0f, 0.5f},
                                {1.0f, -0.2f, 1.0f}, 4.0f,
                                diagonalT, diagonalHit, diagonalNormal));
    assert(std::fabs(diagonalHit.y - 1.76f) < 1e-4f);

    // A ray starting on an X cell boundary and travelling toward the lower
    // cell must test that lower cell before advancing again.
    const CollisionTri boundaryWall{
        {1.2f, -1.0f, 0.2f}, {1.2f, 1.0f, 0.2f}, {1.2f, 0.0f, 0.8f},
        {1.0f, 0.0f, 0.0f}};
    CollisionGrid boundaryGrid;
    boundaryGrid.build({boundaryWall}, 0.0f, 2);
    float boundaryT = 0.0f;
    Point3F boundaryHit{}, boundaryNormal{};
    assert(boundaryGrid.raycast({boundaryWall}, {1.5f, 0.0f, 0.4f},
                                {-1.0f, 0.0f, 0.0f}, 1.0f,
                                boundaryT, boundaryHit, boundaryNormal));
    assert(std::fabs(boundaryHit.x - 1.2f) < 1e-4f);
    return 0;
}
