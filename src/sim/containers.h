#pragma once
// The server container (sim/sceneObject.cc Container): ray casts, box
// queries and the radius search the server scripts drive, over Torch's
// engine objects and the server's world geometry (serverCollision()).
//
// World geometry is the triangle soup serverCollision().triangles gathers
// (terrain, interiors, force fields); a hit on it reports the mission's
// TerrainBlock (or InteriorInstance / ForceFieldBare when the mask asks only
// for those) as the object, since the soup carries no owner. Water is the
// serverCollision().water surface. Objects collide by an axis-aligned box:
// a Player by its PlayerData boxSize (centred in x/y, feet at the origin, as
// Player::onNewDataBlock builds mObjBox), any other ShapeBase by its DTS
// bounds through its transform (a 1 m box on its origin when the shape
// cannot be read), and everything else as a point (no collision).
#include "core/math.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct ScriptObject;
class TorqueScript;

namespace SimContainer {

// game/objectTypes.h.
enum TypeMasks : uint32_t {
    StaticObjectType = 1u << 0,
    TerrainObjectType = 1u << 2,
    InteriorObjectType = 1u << 3,
    WaterObjectType = 1u << 4,
    TriggerObjectType = 1u << 5,
    MarkerObjectType = 1u << 6,
    ForceFieldObjectType = 1u << 8,
    GameBaseObjectType = 1u << 10,
    ShapeBaseObjectType = 1u << 11,
    CameraObjectType = 1u << 12,
    StaticShapeObjectType = 1u << 13,
    PlayerObjectType = 1u << 14,
    ItemObjectType = 1u << 15,
    VehicleObjectType = 1u << 16,
    VehicleBlockerObjectType = 1u << 17,
    ProjectileObjectType = 1u << 18,
    CorpseObjectType = 1u << 20,
    TurretObjectType = 1u << 21,
    StaticTSObjectType = 1u << 24,
    DamagableItemObjectType = 1u << 27,
    SensorObjectType = 1u << 28,
    StationObjectType = 1u << 29,
    GeneratorObjectType = 1u << 30,
};
// Projectile::csmStaticCollisionMask / csmDynamicCollisionMask (retail
// 0x10d / 0x78214000).
constexpr uint32_t StaticCollisionMask = StaticObjectType | TerrainObjectType | InteriorObjectType | ForceFieldObjectType;
constexpr uint32_t DynamicCollisionMask = PlayerObjectType | VehicleObjectType | StationObjectType |
                                          GeneratorObjectType | SensorObjectType | DamagableItemObjectType |
                                          TurretObjectType;
constexpr uint32_t DamageableMask = DynamicCollisionMask;

inline Point3F add(const Point3F& a, const Point3F& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Point3F sub(const Point3F& a, const Point3F& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Point3F mul(const Point3F& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Point3F cross(const Point3F& a, const Point3F& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float len(const Point3F& a) { return std::sqrt(dot(a, a)); }
// Point3F::normalize: a zero vector stays zero.
inline Point3F normalize(const Point3F& a) {
    const float l = len(a);
    return l > 0.0f ? mul(a, 1.0f / l) : a;
}
// Point3F::normalizeSafe (1e-4 threshold), falling back to `fallback`.
inline Point3F normalizeSafe(const Point3F& a, const Point3F& fallback = {0, 0, 1}) {
    const float l = len(a);
    return l > 1e-4f ? mul(a, 1.0f / l) : fallback;
}

// SimObject::getType: the class's type bits and the datablock's dynamicType.
uint32_t typeMask(ScriptObject* object);
// The object's world box; false when it has no extent (a point).
bool worldBox(ScriptObject* object, Point3F& min, Point3F& max);
Point3F worldBoxCenter(ScriptObject* object);
Point3F position(ScriptObject* object);

struct RayInfo {
    float t = 1.0f;
    Point3F point{0, 0, 0};
    Point3F normal{0, 0, 1};
    ScriptObject* object = nullptr; // null for geometry with no mission owner
    uint32_t objectType = 0;        // the hit object's type mask
};
// Container::castRay: the first hit from a to b of anything in `mask`,
// ignoring `exempt` (disableCollision).
bool castRay(const Point3F& a, const Point3F& b, uint32_t mask, RayInfo& info,
             const std::vector<ScriptObject*>& exempt = {});
// Container::findObjects: objects of `mask` whose world box overlaps.
std::vector<ScriptObject*> findObjects(const Point3F& min, const Point3F& max, uint32_t mask);
// Container::buildPolyList into an EarlyOutPolyList: whether any polygon of
// the mask's objects (terrain, interiors and force fields as triangles,
// shapes as their collision boxes) falls inside the box [lo, hi] in the
// frame `frame` (rotation + position, row-major like transforms).
bool polysInBox(const std::array<float, 16>& frame, const Point3F& lo, const Point3F& hi, uint32_t mask,
                const std::vector<ScriptObject*>& exempt = {});
// WaterBlock::isPointSubmergedSimple over the server's water surface.
bool pointInWater(const Point3F& point, float* surfaceHeight = nullptr);

} // namespace SimContainer

// InitContainerRadiusSearch, ContainerSearchNext, ContainerSearchCurrDist,
// ContainerSearchCurrRadDamageDist, ContainerRayCast, calcExplosionCoverage,
// containerFindFirst / containerFindNext, SceneObject::getWorldBox(Center).
void registerContainerNatives(TorqueScript& ts);
