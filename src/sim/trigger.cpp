#include "sim/trigger.h"
#include "sim/shape_base.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Object space -> world: the transform with the object's scale.
void toWorld(const SceneObject& o, const float p[3], float out[3]) {
    const float s[3] = {p[0] * o.scale[0], p[1] * o.scale[1], p[2] * o.scale[2]};
    const auto& m = o.transform;
    for (int i = 0; i < 3; ++i) out[i] = m[i * 4] * s[0] + m[i * 4 + 1] * s[1] + m[i * 4 + 2] * s[2] + m[i * 4 + 3];
}

// World -> object space (the transform is rigid, then the scale).
void toObject(const SceneObject& o, const float w[3], float out[3]) {
    const auto& m = o.transform;
    const float d[3] = {w[0] - m[3], w[1] - m[7], w[2] - m[11]};
    for (int i = 0; i < 3; ++i) {
        out[i] = m[i] * d[0] + m[4 + i] * d[1] + m[8 + i] * d[2];
        if (o.scale[i] != 0) out[i] /= o.scale[i];
    }
}

float det3(const float a[3], const float b[3], const float c[3]) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
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

// Trigger::testObject: the object's polys clipped by the polyhedron are not
// empty. APPROXIMATION: the object's world box against the parallelepiped
// (a box corner or centre inside it, or its centre inside the box).
bool TriggerObject::testObject(const std::string& object) const {
    if (!hasPolyhedron) return false;
    auto* shape = EngineObjects::get<ShapeBase>(object);
    if (!shape) return false;
    float lo[3], hi[3];
    shape->worldBox(lo, hi);
    const float d = det3(vecs[0], vecs[1], vecs[2]);
    if (std::fabs(d) < 1e-9f) return false;
    auto inside = [&](const float w[3]) {
        float o[3];
        toObject(*this, w, o);
        const float r[3] = {o[0] - origin[0], o[1] - origin[1], o[2] - origin[2]};
        // Cramer's rule for r = a*v0 + b*v1 + c*v2.
        const float a = det3(r, vecs[1], vecs[2]) / d, b = det3(vecs[0], r, vecs[2]) / d,
                    c = det3(vecs[0], vecs[1], r) / d;
        return a >= 0 && a <= 1 && b >= 0 && b <= 1 && c >= 0 && c <= 1;
    };
    for (int c = 0; c < 9; ++c) {
        float w[3];
        for (int i = 0; i < 3; ++i) w[i] = c == 8 ? (lo[i] + hi[i]) * 0.5f : ((c >> i) & 1) ? hi[i] : lo[i];
        if (inside(w)) return true;
    }
    float centre[3], p[3];
    for (int i = 0; i < 3; ++i) p[i] = origin[i] + (vecs[0][i] + vecs[1][i] + vecs[2][i]) * 0.5f;
    toWorld(*this, p, centre);
    return centre[0] >= lo[0] && centre[0] <= hi[0] && centre[1] >= lo[1] && centre[1] <= hi[1] &&
           centre[2] >= lo[2] && centre[2] <= hi[2];
}

void TriggerObject::potentialEnterObject(const std::string& object) {
    if (std::find(objects.begin(), objects.end(), object) != objects.end()) return;
    if (!testObject(object)) return;
    objects.push_back(object);
    callDataBlock("onEnterTrigger", {object});
}

void TriggerObject::processTick() {
    GameBase::processTick();
    if (objects.empty()) return;
    const int period = (int)dataFloat("tickPeriodMS", 100.0f);
    if (lastThink + period < currTick) {
        currTick = 0;
        lastThink = 0;
        for (int i = (int)objects.size() - 1; i >= 0; --i) {
            if (!EngineObjects::get<ShapeBase>(objects[i]) || !testObject(objects[i])) {
                const std::string gone = objects[i];
                objects.erase(objects.begin() + i);
                callDataBlock("onLeaveTrigger", {gone});
            }
        }
        if (!objects.empty()) callDataBlock("onTickTrigger");
    } else {
        currTick += 32;
    }
}

void triggersPotentialEnter(const std::string& object, const float lo[3], const float hi[3]) {
    std::vector<TriggerObject*> triggers;
    for (auto& [name, o] : ScriptEngine::instance().objects)
        if (auto* t = o ? dynamic_cast<TriggerObject*>(o->engine.get()) : nullptr) triggers.push_back(t);
    for (auto* t : triggers) {
        float tlo[3], thi[3];
        t->worldBox(tlo, thi);
        if (tlo[0] <= hi[0] && thi[0] >= lo[0] && tlo[1] <= hi[1] && thi[1] >= lo[1] && tlo[2] <= hi[2] && thi[2] >= lo[2])
            t->potentialEnterObject(object);
    }
}

void registerTriggerNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Trigger", [] { return std::make_shared<TriggerObject>(); });
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
