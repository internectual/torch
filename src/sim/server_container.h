#pragma once
// The server's static collision geometry (gServerContainer's terrain and
// interiors) and water, built from the server's own mission objects:
// TerrainBlock heightfields, InteriorInstance DIF hulls, WaterBlock boxes.
// Torque space (Z up).
#include "core/math.h"
#include "game/player_prediction.h"
#include <vector>

namespace ServerContainer {

// Triangles overlapping [min, max], normals toward free space.
void gatherTriangles(const Point3F& min, const Point3F& max, std::vector<PlayerPrediction::Triangle>& out);
// The water surface height at (x, y), NaN where there is no water.
float waterSurfaceAt(float x, float y);
// ShapeBase's waterFind over the WaterBlocks overlapping a world box: the
// last one found sets the coverage (of the box's height) and its liquid.
struct WaterInfo {
    float coverage = 0, density = 1, viscosity = 15, surface = 0;
    int liquidType = 0;
};
bool waterFind(const Point3F& min, const Point3F& max, WaterInfo& out);
// Rebuild from the current mission objects now.
void rebuild();

} // namespace ServerContainer
