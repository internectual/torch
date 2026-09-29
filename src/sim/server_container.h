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
// Rebuild from the current mission objects now.
void rebuild();

} // namespace ServerContainer
