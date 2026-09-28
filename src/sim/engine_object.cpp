#include "sim/engine_object.h"
#include "sim/engine_classes.h"
#include "script/script_engine.h"
#include "sim/sim_state.h"
#include <algorithm>
#include <unordered_map>

namespace EngineObjects {

static std::unordered_map<std::string, Factory, EngineClasses::CaseHash, EngineClasses::CaseEqual>& factories() {
    static std::unordered_map<std::string, Factory, EngineClasses::CaseHash, EngineClasses::CaseEqual> table;
    return table;
}

void registerClass(const std::string& engineClass, Factory factory) {
    factories()[engineClass] = std::move(factory);
}

void attach(ScriptObject* object) {
    if (!object || object->engine) return;
    for (const auto& cls : EngineClasses::chain(object->className)) {
        auto it = factories().find(cls);
        if (it == factories().end()) continue;
        object->engine = it->second();
        object->engine->script = object;
        return;
    }
}

EngineObject* find(const std::string& handle) {
    if (!ScriptEngine::exists()) return nullptr;
    ScriptObject* object = ScriptEngine::instance().findObject(handle.c_str());
    return object ? object->engine.get() : nullptr;
}

void forEachTicking(const std::function<void(EngineObject&)>& visit) {
    if (!ScriptEngine::exists()) return;
    std::vector<ScriptObject*> ticking;
    for (auto& [name, object] : ScriptEngine::instance().objects)
        if (object && object->engine && object->engine->processesTicks()) ticking.push_back(object);
    std::sort(ticking.begin(), ticking.end(),
              [](const ScriptObject* a, const ScriptObject* b) { return a->id < b->id; });
    for (auto* object : ticking)
        if (object->engine) visit(*object->engine);
}

} // namespace EngineObjects

namespace SimState {

void advanceServer(double now) {
    static double lastTick = -1.0;
    if (lastTick < 0.0 || now < lastTick) lastTick = now;
    int ticks = 0;
    while (now - lastTick >= TickSeconds && ticks < 8) {
        EngineObjects::forEachTicking([](EngineObject& object) { object.processTick(); });
        lastTick += TickSeconds;
        ++ticks;
    }
    // A long stall does not replay a burst of ticks afterwards.
    if (now - lastTick >= TickSeconds) lastTick = now;
}

} // namespace SimState
