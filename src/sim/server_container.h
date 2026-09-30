#pragma once
// The server's static collision geometry (gServerContainer's terrain and
// interiors) and water, built from the server's own mission objects:
// TerrainBlock heightfields, InteriorInstance DIF hulls, WaterBlock boxes.
// Torque space (Z up).
#include "core/math.h"
#include "game/player_prediction.h"
#include <string>
#include <vector>

struct ScriptObject;

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
// Rebuild now if the mission's terrain, interiors or water changed (the
// check gatherTriangles makes at most twice a second).
void refresh();

// gatherTriangles restricted to the terrain and / or the interiors; owners,
// when given, gets each triangle's TerrainBlock or InteriorInstance.
void gatherGeometry(const Point3F& min, const Point3F& max, bool terrain, bool interiors,
                    std::vector<PlayerPrediction::Triangle>& out,
                    std::vector<const ScriptObject*>* owners = nullptr);
// The first TerrainBlock's origin (its position) and square size; false
// when there is none.
bool terrainBlock(Point3F& origin, float& squareSize);
// TerrainBlock::getHeight / getNormal (terrain/terrData.cc) at a position
// relative to that block's origin: the block repeats, and an empty square
// has no height. The normal is left unnormalized unless asked.
bool terrainHeight(const Point2F& pos, float* height, Point3F* normal = nullptr, bool normalize = true);
// An InteriorInstance's world box: its interior's mBoundingBox, scaled,
// through its transform; false when the interior is not loaded.
bool interiorWorldBox(const ScriptObject* object, Point3F& min, Point3F& max);
// Interior::mHasAlarmState of a loaded interior file (setAlarmMode needs it).
bool interiorHasAlarmState(const std::string& interiorFile);

} // namespace ServerContainer
