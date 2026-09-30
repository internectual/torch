#include "sim/containers.h"
#include "sim/force_field.h"
#include "sim/player.h"
#include "sim/shape_base.h"
#include "sim/camera.h"
#include "sim/engine_classes.h"
#include "sim/net_object.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cstdio>
#include <limits>

namespace SimContainer {

namespace {

bool is(const std::string& cls, const char* base) { return EngineClasses::isA(cls, base); }

// World geometry lives in serverCollision()'s triangles, not as boxes.
bool isGeometryClass(const std::string& cls) {
    return is(cls, "TerrainBlock") || is(cls, "InteriorInstance") || is(cls, "ForceFieldBare") ||
           is(cls, "TSStatic") || is(cls, "WaterBlock") || is(cls, "Sky") || is(cls, "Sun") ||
           is(cls, "MissionArea");
}

ScriptObject* firstOfClass(const char* cls) {
    ScriptObject* best = nullptr;
    for (auto& [key, object] : ScriptEngine::instance().objects)
        if (object && is(object->className, cls) && (!best || object->id < best->id)) best = object;
    return best;
}

// The mission object a geometry hit is reported as (see containers.h).
ScriptObject* geometryOwner(uint32_t mask) {
    if (mask & TerrainObjectType)
        if (auto* o = firstOfClass("TerrainBlock")) return o;
    if (mask & InteriorObjectType)
        if (auto* o = firstOfClass("InteriorInstance")) return o;
    if (mask & ForceFieldObjectType)
        if (auto* o = firstOfClass("ForceFieldBare")) return o;
    if (auto* o = firstOfClass("TerrainBlock")) return o;
    return firstOfClass("InteriorInstance");
}

// Moller-Trumbore over the gathered triangles.
bool castGeometry(const Point3F& a, const Point3F& b, float& bestT, Point3F& normal, bool forceFields) {
    const auto& world = serverCollision();
    if (!world.triangles) return false;
    std::vector<PlayerPrediction::Triangle> tris;
    const Point3F lo{std::min(a.x, b.x) - 0.1f, std::min(a.y, b.y) - 0.1f, std::min(a.z, b.z) - 0.1f};
    const Point3F hi{std::max(a.x, b.x) + 0.1f, std::max(a.y, b.y) + 0.1f, std::max(a.z, b.z) + 0.1f};
    world.triangles(lo, hi, tris);
    // ForceFieldBare::castRay: a field blocks unless it is open.
    if (forceFields) ForceFields::gather(nullptr, lo, hi, tris);
    const Point3F d = sub(b, a);
    bool hit = false;
    for (const auto& t : tris) {
        const Point3F e1 = sub(t.b, t.a), e2 = sub(t.c, t.a);
        const Point3F p = cross(d, e2);
        const float det = dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const Point3F s = sub(a, t.a);
        const float u = dot(s, p) / det;
        if (u < 0 || u > 1) continue;
        const Point3F q = cross(s, e1);
        const float v = dot(d, q) / det;
        if (v < 0 || u + v > 1) continue;
        const float tt = dot(e2, q) / det;
        if (tt < 0 || tt > 1 || tt >= bestT) continue;
        bestT = tt;
        normal = t.n;
        hit = true;
    }
    return hit;
}

// The water surface crossing (either way) along a -> b.
bool castWater(const Point3F& a, const Point3F& b, float& bestT) {
    const auto& water = serverCollision().water;
    if (!water) return false;
    auto depth = [&](float t) {
        const Point3F p = add(a, mul(sub(b, a), t));
        const float level = water(p.x, p.y);
        return std::isnan(level) ? std::numeric_limits<float>::quiet_NaN() : level - p.z;
    };
    constexpr int Steps = 64;
    float prevT = 0.0f, prev = depth(0.0f);
    for (int i = 1; i <= Steps; ++i) {
        const float t = (float)i / Steps;
        const float cur = depth(t);
        if (!std::isnan(prev) && !std::isnan(cur) && (prev > 0.0f) != (cur > 0.0f)) {
            float lo = prevT, hi = t;
            for (int k = 0; k < 16; ++k) {
                const float mid = 0.5f * (lo + hi);
                const float m = depth(mid);
                if (!std::isnan(m) && (m > 0.0f) == (prev > 0.0f)) lo = mid; else hi = mid;
            }
            if (hi < bestT) { bestT = hi; return true; }
            return false;
        }
        prev = cur;
        prevT = t;
    }
    return false;
}

// Slab test; the entry face's normal.
bool castBox(const Point3F& a, const Point3F& b, const Point3F& lo, const Point3F& hi, float& tOut, Point3F& n) {
    const float o[3] = {a.x, a.y, a.z}, d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    const float mn[3] = {lo.x, lo.y, lo.z}, mx[3] = {hi.x, hi.y, hi.z};
    float tNear = 0.0f, tFar = 1.0f;
    int axis = -1;
    float sign = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        float t1 = (mn[i] - o[i]) / d[i], t2 = (mx[i] - o[i]) / d[i];
        float s = -1.0f;
        if (t1 > t2) { std::swap(t1, t2); s = 1.0f; }
        if (t1 > tNear) { tNear = t1; axis = i; sign = s; }
        tFar = std::min(tFar, t2);
        if (tNear > tFar) return false;
    }
    if (axis < 0) return false; // starts inside: the engine's box casts miss from inside
    tOut = tNear;
    n = {axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f};
    return true;
}

} // namespace

Point3F position(ScriptObject* object) {
    auto* scene = object ? dynamic_cast<SceneObject*>(object->engine.get()) : nullptr;
    if (!scene) return {0, 0, 0};
    return {scene->transform[3], scene->transform[7], scene->transform[11]};
}

uint32_t typeMask(ScriptObject* object) {
    if (!object) return 0;
    const std::string& cls = object->className;
    uint32_t mask = 0;
    if (is(cls, "GameBase")) mask |= GameBaseObjectType;
    if (is(cls, "ShapeBase")) mask |= ShapeBaseObjectType;
    if (is(cls, "Camera")) mask |= 1u << 12;
    if (is(cls, "StaticShape")) mask |= 1u << 13;
    if (is(cls, "Player")) mask |= PlayerObjectType;
    if (is(cls, "Item")) mask |= 1u << 15;
    if (is(cls, "Vehicle")) mask |= VehicleObjectType;
    if (is(cls, "Projectile")) mask |= ProjectileObjectType;
    if (is(cls, "Turret")) mask |= TurretObjectType;
    if (is(cls, "TerrainBlock")) mask |= TerrainObjectType | StaticObjectType;
    if (is(cls, "InteriorInstance")) mask |= InteriorObjectType | StaticObjectType;
    if (is(cls, "WaterBlock")) mask |= WaterObjectType;
    if (is(cls, "Trigger")) mask |= 1u << 5;
    if (is(cls, "ForceFieldBare")) mask |= ForceFieldObjectType;
    if (is(cls, "TSStatic")) mask |= (1u << 24) | StaticObjectType;
    if (is(cls, "MissionMarker")) mask |= 1u << 6;
    // ShapeBase::onNewDataBlock: mTypeMask |= mDataBlock->dynamicTypeField.
    if (is(cls, "ShapeBase")) {
        const std::string block = ScriptEngine::instance().objectDataBlock(object);
        if (ScriptObject* data = block.empty() ? nullptr : ScriptEngine::instance().findObject(block.c_str()))
            mask |= (uint32_t)Fields::s32(data, "dynamicType", 0);
    }
    return mask;
}

bool worldBox(ScriptObject* object, Point3F& min, Point3F& max) {
    const Point3F p = position(object);
    min = max = p;
    auto* engine = object ? object->engine.get() : nullptr;
    if (!engine || dynamic_cast<Camera*>(engine)) return false;
    if (dynamic_cast<PlayerObject*>(engine)) {
        const std::string block = ScriptEngine::instance().objectDataBlock(object);
        const auto size = Fields::point(ScriptEngine::instance().findObject(block.c_str()), "boxSize", {1, 1, 2});
        min = {p.x - size[0] * 0.5f, p.y - size[1] * 0.5f, p.z};
        max = {p.x + size[0] * 0.5f, p.y + size[1] * 0.5f, p.z + size[2]};
        return true;
    }
    if (dynamic_cast<ShapeBase*>(engine)) {
        min = {p.x - 0.5f, p.y - 0.5f, p.z};
        max = {p.x + 0.5f, p.y + 0.5f, p.z + 1.0f};
        return true;
    }
    return false;
}

Point3F worldBoxCenter(ScriptObject* object) {
    Point3F lo, hi;
    worldBox(object, lo, hi);
    return mul(add(lo, hi), 0.5f);
}

bool pointInWater(const Point3F& point, float* surfaceHeight) {
    const auto& water = serverCollision().water;
    if (!water) return false;
    const float level = water(point.x, point.y);
    if (std::isnan(level) || point.z >= level) return false;
    if (surfaceHeight) *surfaceHeight = level;
    return true;
}

bool castRay(const Point3F& a, const Point3F& b, uint32_t mask, RayInfo& info, const std::vector<ScriptObject*>& exempt) {
    float best = 2.0f;
    bool hit = false;
    Point3F normal{0, 0, 1};
    ScriptObject* object = nullptr;
    uint32_t objectType = 0;
    if (mask & StaticCollisionMask) {
        float t = best;
        Point3F n;
        if (castGeometry(a, b, t, n, (mask & ForceFieldObjectType) != 0)) {
            best = t;
            normal = n;
            object = geometryOwner(mask);
            objectType = object ? typeMask(object) : TerrainObjectType | StaticObjectType;
            hit = true;
        }
    }
    if (mask & WaterObjectType) {
        float t = best;
        if (castWater(a, b, t)) {
            best = t;
            normal = {0, 0, 1};
            object = firstOfClass("WaterBlock");
            objectType = WaterObjectType;
            hit = true;
        }
    }
    for (auto& [key, candidate] : ScriptEngine::instance().objects) {
        if (!candidate || !candidate->engine || isGeometryClass(candidate->className)) continue;
        if (std::find(exempt.begin(), exempt.end(), candidate) != exempt.end()) continue;
        const uint32_t type = typeMask(candidate);
        if (!(type & mask)) continue;
        // ShapeBase::castRay tests the shape's LOS/collision meshes: a shape
        // without them (a marker) stops no ray. Players hit by their box.
        if (auto* shape = dynamic_cast<ShapeBase*>(candidate->engine.get());
            shape && !dynamic_cast<PlayerObject*>(shape) && !shapeHasCollision(shapeFileOf(*shape)))
            continue;
        Point3F lo, hi, n;
        if (!worldBox(candidate, lo, hi)) continue;
        float t = 0.0f;
        if (!castBox(a, b, lo, hi, t, n) || t >= best) continue;
        best = t;
        normal = n;
        object = candidate;
        objectType = type;
        hit = true;
    }
    if (!hit) return false;
    info.t = best;
    info.point = add(a, mul(sub(b, a), best));
    info.normal = normal;
    info.object = object;
    info.objectType = objectType;
    return true;
}

std::vector<ScriptObject*> findObjects(const Point3F& min, const Point3F& max, uint32_t mask) {
    std::vector<ScriptObject*> out;
    for (auto& [key, object] : ScriptEngine::instance().objects) {
        if (!object || !dynamic_cast<SceneObject*>(object->engine.get())) continue;
        if (!(typeMask(object) & mask)) continue;
        Point3F lo, hi;
        worldBox(object, lo, hi);
        if (hi.x < min.x || lo.x > max.x || hi.y < min.y || lo.y > max.y || hi.z < min.z || lo.z > max.z) continue;
        out.push_back(object);
    }
    std::sort(out.begin(), out.end(), [](ScriptObject* a, ScriptObject* b) { return a->id < b->id; });
    return out;
}

} // namespace SimContainer

namespace {

using namespace SimContainer;

Point3F parsePoint(const std::string& text) {
    Point3F p{0, 0, 0};
    std::sscanf(text.c_str(), "%f %f %f", &p.x, &p.y, &p.z);
    return p;
}

std::string format(std::initializer_list<float> values) {
    std::string out;
    char buffer[32];
    for (float v : values) {
        std::snprintf(buffer, sizeof(buffer), "%g", v);
        if (!out.empty()) out += ' ';
        out += buffer;
    }
    return out;
}

// Container::initRadiusSearch state (gServerContainer's search list).
struct RadiusSearch {
    Point3F reference{0, 0, 0};
    std::vector<std::string> list; // object keys, nearest box centre first
    int current = -1;
    ScriptObject* at(int i) const {
        if (i < 0 || i >= (int)list.size()) return nullptr;
        return ScriptEngine::instance().findObject(list[(size_t)i].c_str());
    }
};
RadiusSearch& radiusSearch() {
    static RadiusSearch search;
    return search;
}

struct FindQuery {
    std::vector<std::string> list;
    size_t index = 0;
};
FindQuery& findQuery() {
    static FindQuery query;
    return query;
}

} // namespace

void registerContainerNatives(TorqueScript& ts) {
    using Args = std::vector<VMValue>;
    auto arg = [](const Args& args, size_t i) { return i < args.size() ? args[i].toString() : std::string(); };

    // InitContainerRadiusSearch("x y z", radius, mask): objects whose world
    // box is within radius of the point, sorted by box-centre distance.
    ts.registerNative("InitContainerRadiusSearch", [arg](const Args& args) -> VMValue {
        RadiusSearch& search = radiusSearch();
        search.list.clear();
        search.current = -1;
        const Point3F p = parsePoint(arg(args, 0));
        const float r = args.size() > 1 ? args[1].toFloat() : 0.0f;
        const uint32_t mask = (uint32_t)(args.size() > 2 ? args[2].toInt() : 0);
        search.reference = p;
        std::vector<std::pair<float, ScriptObject*>> found;
        for (ScriptObject* object : findObjects(sub(p, {r, r, r}), add(p, {r, r, r}), mask)) {
            Point3F lo, hi;
            worldBox(object, lo, hi);
            const float pt[3] = {p.x, p.y, p.z}, mn[3] = {lo.x, lo.y, lo.z}, mx[3] = {hi.x, hi.y, hi.z};
            float sum = 0;
            for (int j = 0; j < 3; ++j) {
                if (pt[j] < mn[j]) sum += (pt[j] - mn[j]) * (pt[j] - mn[j]);
                else if (pt[j] > mx[j]) sum += (pt[j] - mx[j]) * (pt[j] - mx[j]);
            }
            if (sum < r * r) found.push_back({len(sub(worldBoxCenter(object), p)), object});
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [d, object] : found) search.list.push_back(ScriptEngine::instance().objectKey(object));
        return VMValue("");
    });
    ts.registerNative("ContainerSearchNext", [](const Args&) -> VMValue {
        RadiusSearch& search = radiusSearch();
        if (search.current >= (int)search.list.size()) return VMValue(0);
        ++search.current;
        while (search.current < (int)search.list.size() && !search.at(search.current)) ++search.current;
        ScriptObject* object = search.at(search.current);
        return VMValue(object ? ScriptEngine::instance().objectId(object) : 0);
    });
    ts.registerNative("ContainerSearchCurrDist", [](const Args&) -> VMValue {
        RadiusSearch& search = radiusSearch();
        ScriptObject* object = search.at(search.current);
        return VMValue(object ? len(sub(worldBoxCenter(object), search.reference)) : 0.0f);
    });
    ts.registerNative("ContainerSearchCurrRadDamageDist", [](const Args&) -> VMValue {
        RadiusSearch& search = radiusSearch();
        ScriptObject* object = search.at(search.current);
        if (!object) return VMValue(0.0f);
        Point3F lo, hi;
        worldBox(object, lo, hi);
        float dist = len(sub(mul(add(lo, hi), 0.5f), search.reference));
        dist -= std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        return VMValue(dist < 0 ? 0.0f : dist);
    });
    // ContainerRayCast("x y z", "x y z", mask[, exempt]): "id x y z nx ny nz" or 0.
    ts.registerNative("ContainerRayCast", [arg](const Args& args) -> VMValue {
        const Point3F a = parsePoint(arg(args, 0)), b = parsePoint(arg(args, 1));
        const uint32_t mask = (uint32_t)(args.size() > 2 ? args[2].toInt() : 0);
        std::vector<ScriptObject*> exempt;
        if (args.size() > 3)
            if (ScriptObject* e = ScriptEngine::instance().findObject(arg(args, 3).c_str())) exempt.push_back(e);
        RayInfo info;
        if (!castRay(a, b, mask, info, exempt)) return VMValue(0);
        const int id = info.object ? ScriptEngine::instance().objectId(info.object) : 0;
        if (!id) return VMValue(0);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%d %g %g %g %g %g %g", id, info.point.x, info.point.y, info.point.z,
                      info.normal.x, info.normal.y, info.normal.z);
        return VMValue(buffer);
    });
    // explosion.cc cCalcCoverage: 1 when the object's box centre is in line
    // of sight (directly, or over a 1 m step up), else 0.
    ts.registerNative("calcExplosionCoverage", [arg](const Args& args) -> VMValue {
        const Point3F pos = parsePoint(arg(args, 0));
        ScriptObject* object = ScriptEngine::instance().findObject(arg(args, 1).c_str());
        const uint32_t mask = (uint32_t)(args.size() > 2 ? args[2].toInt() : 0);
        if (!object || !dynamic_cast<SceneObject*>(object->engine.get())) {
            Console::instance().printf(LogLevel::Warn, "calcExplosionCoverage: couldn't find object: %s", arg(args, 1).c_str());
            return VMValue(1.0f);
        }
        const Point3F center = worldBoxCenter(object);
        const std::vector<ScriptObject*> exempt{object};
        RayInfo info;
        if (!castRay(pos, center, mask, info, exempt)) return VMValue(1.0f);
        const Point3F up = add(pos, {0, 0, 1});
        if (!castRay(pos, up, mask, info, exempt) && !castRay(up, center, mask, info, exempt)) return VMValue(1.0f);
        return VMValue(0.0f);
    });
    // gameFunctions.cc containerFindFirst(type, point, x, y, z) (the point
    // is read from the second argument; V12 scans the first by mistake).
    ts.registerNative("containerFindFirst", [arg](const Args& args) -> VMValue {
        FindQuery& query = findQuery();
        query.list.clear();
        query.index = 0;
        const uint32_t mask = (uint32_t)(args.empty() ? 0 : args[0].toInt());
        const Point3F origin = parsePoint(arg(args, 1));
        const Point3F size{std::fabs(args.size() > 2 ? args[2].toFloat() : 0.0f),
                           std::fabs(args.size() > 3 ? args[3].toFloat() : 0.0f),
                           std::fabs(args.size() > 4 ? args[4].toFloat() : 0.0f)};
        for (ScriptObject* object : findObjects(sub(origin, size), add(origin, size), mask))
            query.list.push_back(std::to_string(ScriptEngine::instance().objectId(object)));
        if (query.list.empty()) return VMValue("");
        return VMValue(query.list[query.index++]);
    });
    ts.registerNative("containerFindNext", [](const Args&) -> VMValue {
        FindQuery& query = findQuery();
        if (query.index < query.list.size()) return VMValue(query.list[query.index++]);
        return VMValue("");
    });
    // SceneObject::getWorldBox / getWorldBoxCenter for engine scene objects
    // (see containers.h for the boxes); other objects keep the field-backed
    // answer of the unscoped natives.
    ts.registerNative("SceneObject::getWorldBox", [arg](const Args& args) -> VMValue {
        ScriptObject* object = ScriptEngine::instance().findObject(arg(args, 0).c_str());
        if (!object) return VMValue("");
        if (!dynamic_cast<SceneObject*>(object->engine.get())) {
            auto it = object->fields.find("worldBox");
            return it != object->fields.end() ? it->second : VMValue("0 0 0 0 0 0");
        }
        Point3F lo, hi;
        worldBox(object, lo, hi);
        return VMValue(format({lo.x, lo.y, lo.z, hi.x, hi.y, hi.z}));
    });
    ts.registerNative("SceneObject::getWorldBoxCenter", [arg](const Args& args) -> VMValue {
        ScriptObject* object = ScriptEngine::instance().findObject(arg(args, 0).c_str());
        if (!object) return VMValue("");
        if (!dynamic_cast<SceneObject*>(object->engine.get())) {
            auto it = object->fields.find("worldBoxCenter");
            if (it != object->fields.end()) return it->second;
            return object->fields["position"];
        }
        const Point3F c = worldBoxCenter(object);
        return VMValue(format({c.x, c.y, c.z}));
    });
}
