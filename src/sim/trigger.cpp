#include "sim/trigger.h"
#include "sim/shape_base.h"
#include "game/trigger.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>

namespace {

// Object space -> world: the transform with the object's scale.
void toWorld(const SceneObject& o, const float p[3], float out[3]) {
    const float s[3] = {p[0] * o.scale[0], p[1] * o.scale[1], p[2] * o.scale[2]};
    const auto& m = o.transform;
    for (int i = 0; i < 3; ++i) out[i] = m[i * 4] * s[0] + m[i * 4 + 1] * s[1] + m[i * 4 + 2] * s[2] + m[i * 4 + 3];
}

bool overlapsAabb(const Point3F vertices[8], const float lo[3], const float hi[3]) {
    const Point3F center{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const Point3F half{(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    const Point3F edge[3] = {
        {vertices[1].x - vertices[0].x, vertices[1].y - vertices[0].y, vertices[1].z - vertices[0].z},
        {vertices[2].x - vertices[0].x, vertices[2].y - vertices[0].y, vertices[2].z - vertices[0].z},
        {vertices[3].x - vertices[0].x, vertices[3].y - vertices[0].y, vertices[3].z - vertices[0].z}};
    const Point3F boxAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::array<Point3F, 15> axes{};
    size_t axisCount = 0;
    for (const auto& axis : boxAxes) axes[axisCount++] = axis;
    for (int e = 0; e < 3; ++e) {
        const auto& a = edge[e];
        const auto& b = edge[(e + 1) % 3];
        axes[axisCount++] = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    for (const auto& e : edge)
        for (const auto& b : boxAxes)
            axes[axisCount++] = {e.y * b.z - e.z * b.y, e.z * b.x - e.x * b.z, e.x * b.y - e.y * b.x};
    for (const auto& axis : axes) {
        const float length2 = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
        if (length2 < 1e-12f) continue;
        float polyMin = vertices[0].x * axis.x + vertices[0].y * axis.y + vertices[0].z * axis.z;
        float polyMax = polyMin;
        for (int i = 1; i < 8; ++i) {
            const float projection = vertices[i].x * axis.x + vertices[i].y * axis.y + vertices[i].z * axis.z;
            polyMin = std::min(polyMin, projection);
            polyMax = std::max(polyMax, projection);
        }
        const float boxCenter = center.x * axis.x + center.y * axis.y + center.z * axis.z;
        const float boxRadius = half.x * std::fabs(axis.x) + half.y * std::fabs(axis.y) + half.z * std::fabs(axis.z);
        if (polyMax < boxCenter - boxRadius || boxCenter + boxRadius < polyMin) return false;
    }
    return true;
}

} // namespace

// TypeTriggerPolyhedron: "ox oy oz  ax ay az  bx by bz  cx cy cz", an
// origin and three edge vectors (object space).
void TriggerObject::readFields() {
    GameBase::readFields();
    const std::string text = Fields::string(script, "polyhedron");
    float v[12];
    hasPolyhedron = !text.empty() &&
                    std::sscanf(text.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4],
                                &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) == 12;
    if (!text.empty() && !hasPolyhedron) Console::instance().printf(LogLevel::Info, "Bad polyhedron!");
    if (!hasPolyhedron) return;
    for (int i = 0; i < 3; ++i) origin[i] = v[i];
    for (int e = 0; e < 3; ++e)
        for (int i = 0; i < 3; ++i) vecs[e][i] = v[3 + e * 3 + i];
}

void TriggerObject::worldBox(float lo[3], float hi[3]) const {
    for (int i = 0; i < 3; ++i) { lo[i] = 1e30f; hi[i] = -1e30f; }
    for (int c = 0; c < 8; ++c) {
        float p[3];
        for (int i = 0; i < 3; ++i)
            p[i] = origin[i] + ((c & 1) ? vecs[0][i] : 0) + ((c & 2) ? vecs[1][i] : 0) + ((c & 4) ? vecs[2][i] : 0);
        float w[3];
        toWorld(*this, p, w);
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], w[i]); hi[i] = std::max(hi[i], w[i]); }
    }
}

// Trigger::testObject: separating-axis overlap between the trigger's authored
// parallelepiped and the actor's world-box proxy. The retail engine clips the
// actor's collision poly list; this keeps the proxy test robust for slanted
// triggers whose volume crosses a box without containing sampled corners.
bool TriggerObject::testObject(const std::string& object) const {
    if (!hasPolyhedron) return false;
    auto* shape = EngineObjects::get<ShapeBase>(object);
    if (!shape) return false;
    float lo[3], hi[3];
    shape->worldBox(lo, hi);
    float worldBase[3];
    toWorld(*this, origin, worldBase);
    const Point3F base{worldBase[0], worldBase[1], worldBase[2]};
    Point3F edge[3];
    for (int e = 0; e < 3; ++e) {
        float local[3] = {origin[0] + vecs[e][0], origin[1] + vecs[e][1], origin[2] + vecs[e][2]};
        float worldEnd[3];
        toWorld(*this, local, worldEnd);
        edge[e] = {worldEnd[0] - base.x, worldEnd[1] - base.y, worldEnd[2] - base.z};
    }
    Point3F vertices[8];
    for (int c = 0; c < 8; ++c) {
        vertices[c] = base;
        for (int e = 0; e < 3; ++e)
            if (c & (1 << e)) {
                vertices[c].x += edge[e].x;
                vertices[c].y += edge[e].y;
                vertices[c].z += edge[e].z;
            }
    }
    return overlapsAabb(vertices, lo, hi);
}

void TriggerObject::potentialEnterObject(const std::string& object) {
    if (std::find(objects.begin(), objects.end(), object) != objects.end()) return;
    if (!testObject(object)) return;
    objects.push_back(object);
    ScriptEngine::instance().addDeleteNotify(script, ScriptEngine::instance().findObject(object.c_str()));
    callDataBlock("onEnterTrigger", {object});
}

void TriggerObject::onDeleteNotify(ScriptObject* object) {
    if (!object) return;
    const std::string id = std::to_string(object->id);
    auto it = std::find_if(objects.begin(), objects.end(), [&](const std::string& member) {
        return ScriptEngine::instance().findObject(member.c_str()) == object;
    });
    if (it == objects.end()) return;
    objects.erase(it);
    callDataBlock("onLeaveTrigger", {id});
}

void TriggerObject::processTick() {
    const std::string self = handle();
    GameBase::processTick();
    if (objects.empty()) return;
    const int period = (int)dataFloat("tickPeriodMS", 100.0f);
    if (lastThink + period < currTick) {
        currTick = 0;
        lastThink = 0;
        // A leave callback can delete another occupant, notifying us at once.
        const auto occupants = objects;
        for (auto member = occupants.rbegin(); member != occupants.rend(); ++member) {
            auto current = std::find(objects.begin(), objects.end(), *member);
            if (current == objects.end()) continue;
            if (!EngineObjects::get<ShapeBase>(*member) || !testObject(*member)) {
                const std::string gone = *member;
                objects.erase(current);
                ScriptEngine::instance().clearDeleteNotify(script, ScriptEngine::instance().findObject(gone.c_str()));
                callDataBlock("onLeaveTrigger", {gone});
                if (EngineObjects::get<TriggerObject>(self) != this) return;
            }
        }
        if (!objects.empty()) callDataBlock("onTickTrigger");
    } else {
        currTick += 32;
    }
}

void triggersPotentialEnter(const std::string& object, const float lo[3], const float hi[3]) {
    std::vector<int> triggers;
    for (auto& [name, o] : ScriptEngine::instance().objects)
        if (o && dynamic_cast<TriggerObject*>(o->engine.get())) triggers.push_back(o->id);
    for (int id : triggers) {
        // Enter callbacks can delete the subject, this trigger, or a later
        // candidate. Resolve each id anew and retain state during the callback.
        if (!EngineObjects::get<ShapeBase>(object)) return;
        auto* candidate = ScriptEngine::instance().findObject(std::to_string(id).c_str());
        if (!candidate) continue;
        auto state = candidate->engine;
        auto* t = dynamic_cast<TriggerObject*>(state.get());
        if (!t) continue;
        float tlo[3], thi[3];
        t->worldBox(tlo, thi);
        if (tlo[0] <= hi[0] && thi[0] >= lo[0] && tlo[1] <= hi[1] && thi[1] >= lo[1] && tlo[2] <= hi[2] && thi[2] >= lo[2])
            t->potentialEnterObject(object);
    }
}

void registerTriggerNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Trigger", [] { return std::make_shared<TriggerObject>(); });
    EngineObjects::registerClass("PhysicalZone", [] { return std::make_shared<PhysicalZoneObject>(); });
    ts.registerNative("PhysicalZone::activate", [](const std::vector<VMValue>& args) -> VMValue {
        if (auto* z = args.empty() ? nullptr : EngineObjects::get<PhysicalZoneObject>(args[0].toString())) z->activate();
        return VMValue("");
    });
    ts.registerNative("PhysicalZone::deactivate", [](const std::vector<VMValue>& args) -> VMValue {
        if (auto* z = args.empty() ? nullptr : EngineObjects::get<PhysicalZoneObject>(args[0].toString())) z->deactivate();
        return VMValue("");
    });
    using Args = std::vector<VMValue>;
    auto trigger = [](const Args& args) -> TriggerObject* {
        return args.empty() ? nullptr : EngineObjects::get<TriggerObject>(args[0].toString());
    };
    ts.registerNative("Trigger::getNumObjects", [trigger](const Args& args) -> VMValue {
        auto* t = trigger(args);
        return VMValue(t ? (int)t->objects.size() : 0);
    });
    ts.registerNative("Trigger::getObject", [trigger](const Args& args) -> VMValue {
        auto* t = trigger(args);
        const int index = args.size() > 1 ? args[1].toInt() : -1;
        if (!t || index < 0 || index >= (int)t->objects.size()) return VMValue(-1);
        ScriptObject* o = ScriptEngine::instance().findObject(t->objects[index].c_str());
        return VMValue(o ? ScriptEngine::instance().objectId(o) : -1);
    });
    // The TriggerData defaults: the trigger's group members hear onTrigger /
    // onTriggerTick (a datablock's script versions replace these).
    auto group = [](const std::string& handle, const std::function<void(const std::string&)>& each) {
        ScriptObject* t = ScriptEngine::instance().findObject(handle.c_str());
        if (!t) return;
        auto parent = t->internals.find("__parent");
        ScriptObject* g = parent == t->internals.end() ? nullptr : ScriptEngine::instance().findObject(parent->second.toString().c_str());
        if (!g) return;
        auto countIt = g->internals.find("__childCount");
        const int count = countIt == g->internals.end() ? 0 : countIt->second.toInt();
        for (int i = 0; i < count; ++i) {
            auto child = g->internals.find("__child" + std::to_string(i));
            if (child != g->internals.end()) each(child->second.toString());
        }
    };
    ts.registerNative("TriggerData::onEnterTrigger", [group](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const std::string id = args[1].toString();
        group(id, [&](const std::string& member) {
            if (auto* s = ScriptEngine::instance().ts()) s->callObjectMethod(member, "onTrigger", {VMValue(id), VMValue("1")});
        });
        return VMValue("");
    });
    ts.registerNative("TriggerData::onLeaveTrigger", [group, trigger](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const std::string id = args[1].toString();
        auto* t = EngineObjects::get<TriggerObject>(id);
        if (t && !t->objects.empty()) return VMValue("");
        group(id, [&](const std::string& member) {
            if (auto* s = ScriptEngine::instance().ts()) s->callObjectMethod(member, "onTrigger", {VMValue(id), VMValue("0")});
        });
        return VMValue("");
    });
    ts.registerNative("TriggerData::onTickTrigger", [group](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const std::string id = args[1].toString();
        group(id, [&](const std::string& member) {
            if (auto* s = ScriptEngine::instance().ts()) s->callObjectMethod(member, "onTriggerTick", {VMValue(id)});
        });
        return VMValue("");
    });
}

// TypeTriggerPolyhedron as setDataTypeTriggerPolyhedron builds it: the
// eight corners, six planes (normals from the edge vectors), twelve edges.
void PhysicalZoneObject::readFields() {
    SceneObject::readFields();
    const std::string text = Fields::string(script, "polyhedron");
    float v[12];
    hasPolyhedron = !text.empty() &&
                    std::sscanf(text.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4],
                                &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) == 12;
    if (!text.empty() && !hasPolyhedron) Console::instance().printf(LogLevel::Info, "Bad polyhedron!");
    if (!hasPolyhedron) return;
    const float* o = v;
    const float* a = v + 3;
    const float* b = v + 6;
    const float* c = v + 9;
    auto set = [&](int i, int ka, int kb, int kc) {
        for (int k = 0; k < 3; ++k) points[i][k] = o[k] + (ka ? a[k] : 0) + (kb ? b[k] : 0) + (kc ? c[k] : 0);
    };
    set(0, 0, 0, 0);
    set(1, 1, 0, 0);
    set(2, 0, 1, 0);
    set(3, 0, 0, 1);
    set(4, 1, 1, 0);
    set(5, 1, 0, 1);
    set(6, 0, 1, 1);
    set(7, 1, 1, 1);
    auto plane = [&](int i, const float* p, const float* x, const float* y) {
        // PlaneF::set(p, mCross(x, y)): the normal normalized.
        float n[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) for (float& k : n) k /= len;
        for (int k = 0; k < 3; ++k) planes[i][k] = n[k];
        planes[i][3] = -(p[0] * n[0] + p[1] * n[1] + p[2] * n[2]);
    };
    plane(0, o, c, a);
    plane(1, o, a, b);
    plane(2, o, b, c);
    plane(3, points[7], b, a);
    plane(4, points[7], c, b);
    plane(5, points[7], a, c);
}

float PhysicalZoneObject::velocityMod() const {
    return physicalZoneModifier(Fields::f32(script, "velocityMod", 1.0f));
}

bool PhysicalZoneObject::overlapsBox(const float lo[3], const float hi[3]) const {
    if (!hasPolyhedron) return false;
    Point3F world[8];
    for (int i = 0; i < 8; ++i) {
        float p[3];
        toWorld(*this, points[i], p);
        world[i] = {p[0], p[1], p[2]};
    }
    return overlapsAabb(world, lo, hi);
}

uint32_t PhysicalZoneObject::packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) {
    static const uint32_t edges[12][4] = {
        // face[0], face[1], vertex[0], vertex[1]
        {0, 1, 0, 1}, {0, 4, 1, 5}, {0, 3, 5, 3}, {0, 2, 3, 0}, {3, 2, 3, 6}, {2, 5, 6, 2},
        {2, 1, 2, 0}, {4, 1, 1, 4}, {1, 5, 4, 2}, {4, 5, 4, 7}, {3, 4, 5, 7}, {3, 5, 7, 6}};
    if (w.writeFlag(mask & InitialUpdateMask)) {
        writeTransform(w);
        writeScale(w);
        const uint32_t count = hasPolyhedron ? 8 : 0;
        w.writeU32(count);
        for (uint32_t i = 0; i < count; ++i) w.writePoint({points[i][0], points[i][1], points[i][2]});
        w.writeU32(hasPolyhedron ? 6 : 0);
        for (uint32_t i = 0; i < (hasPolyhedron ? 6u : 0u); ++i)
            for (int k = 0; k < 4; ++k) w.writeF32(planes[i][k]);
        w.writeU32(hasPolyhedron ? 12 : 0);
        for (uint32_t i = 0; i < (hasPolyhedron ? 12u : 0u); ++i)
            for (int k = 0; k < 4; ++k) w.writeU32(edges[i][k]);
        w.writeF32(velocityMod());
        w.writeF32(physicalZoneModifier(Fields::f32(script, "gravityMod", 1.0f)));
        const auto force = Fields::point(script, "appliedForce", {0, 0, 0});
        const auto clampedForce = physicalZoneAppliedForce({force[0], force[1], force[2]});
        w.writePoint({clampedForce.x, clampedForce.y, clampedForce.z});
        w.writeFlag(active);
    } else {
        w.writeFlag(active);
    }
    return 0;
}

void PhysicalZoneObject::activate() {
    if (!active) setMaskBits(ActiveMask);
    active = true;
}

void PhysicalZoneObject::deactivate() {
    if (active) setMaskBits(ActiveMask);
    active = false;
}

void PhysicalZoneObject::faces(std::vector<PlayerPrediction::Triangle>& out) const {
    if (!hasPolyhedron) return;
    float world[8][3];
    for (int i = 0; i < 8; ++i) toWorld(*this, points[i], world[i]);
    Point3F centre{0, 0, 0};
    for (auto& p : world) centre = {centre.x + p[0] / 8, centre.y + p[1] / 8, centre.z + p[2] / 8};
    // The six faces as quads of the setDataTypeTriggerPolyhedron corners.
    static const int quads[6][4] = {{0, 1, 4, 2}, {3, 5, 7, 6}, {0, 1, 5, 3}, {2, 4, 7, 6}, {0, 2, 6, 3}, {1, 4, 7, 5}};
    auto pt = [&](int i) { return Point3F{world[i][0], world[i][1], world[i][2]}; };
    for (const auto& q : quads) {
        for (const auto& tri : {std::array<int, 3>{q[0], q[1], q[2]}, std::array<int, 3>{q[0], q[2], q[3]}}) {
            PlayerPrediction::Triangle t{pt(tri[0]), pt(tri[1]), pt(tri[2]), {}};
            Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(t.b, t.a), PlayerPrediction::sub(t.c, t.a));
            const float len = PlayerPrediction::length(n);
            if (len < 1e-12f) continue;
            n = PlayerPrediction::mul(n, 1.0f / len);
            // Outward: away from the centre.
            if (PlayerPrediction::dot(n, PlayerPrediction::sub(t.a, centre)) < 0) {
                std::swap(t.b, t.c);
                n = PlayerPrediction::mul(n, -1.0f);
            }
            t.n = n;
            out.push_back(t);
        }
    }
}

namespace PhysicalZones {
Effects effects(const float lo[3], const float hi[3]) {
    Effects result;
    for (auto& [key, object] : ScriptEngine::instance().objects) {
        auto* zone = object ? dynamic_cast<PhysicalZoneObject*>(object->engine.get()) : nullptr;
        if (!zone || !zone->active || !zone->overlapsBox(lo, hi)) continue;
        result.gravityMod *= physicalZoneModifier(Fields::f32(zone->script, "gravityMod", 1.0f));
        const auto force = Fields::point(zone->script, "appliedForce", {0, 0, 0});
        const auto clamped = physicalZoneAppliedForce({force[0], force[1], force[2]});
        result.appliedForce = {result.appliedForce.x + clamped.x, result.appliedForce.y + clamped.y,
                               result.appliedForce.z + clamped.z};
    }
    return result;
}

void gather(const Point3F& min, const Point3F& max, std::vector<PlayerPrediction::Zone>& out) {
    for (auto& [key, object] : ScriptEngine::instance().objects) {
        auto* zone = object ? dynamic_cast<PhysicalZoneObject*>(object->engine.get()) : nullptr;
        if (!zone || !zone->active || !zone->hasPolyhedron) continue;
        PlayerPrediction::Zone z;
        zone->faces(z.triangles);
        Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& t : z.triangles)
            for (const Point3F* p : {&t.a, &t.b, &t.c}) {
                lo = {std::min(lo.x, p->x), std::min(lo.y, p->y), std::min(lo.z, p->z)};
                hi = {std::max(hi.x, p->x), std::max(hi.y, p->y), std::max(hi.z, p->z)};
            }
        if (hi.x < min.x || lo.x > max.x || hi.y < min.y || lo.y > max.y || hi.z < min.z || lo.z > max.z) continue;
        z.velocityMod = zone->velocityMod();
        out.push_back(std::move(z));
    }
}
} // namespace PhysicalZones
