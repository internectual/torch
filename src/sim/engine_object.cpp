#include "sim/engine_object.h"
#include "sim/game_base.h"
#include "sim/game_connection.h"
#include "sim/engine_classes.h"
#include "sim/net_object.h"
#include "script/script_engine.h"
#include "sim/sim_state.h"
#include "sim/target_manager.h"
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
        if (auto* net = dynamic_cast<NetObject*>(object->engine.get())) net->readFields();
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
    std::vector<int> ticking;
    for (auto& [name, object] : ScriptEngine::instance().objects)
        if (object && object->engine && object->engine->processesTicks()) ticking.push_back(object->id);
    std::sort(ticking.begin(), ticking.end());
    // Callbacks can delete, rename or create objects. Resolve the original id
    // for each visit, and keep its engine state alive through the callback.
    for (int id : ticking) {
        auto* object = ScriptEngine::instance().findObject(std::to_string(id).c_str());
        if (!object || !object->engine) continue;
        auto state = object->engine;
        visit(*state);
    }
}

} // namespace EngineObjects

namespace SimState {

namespace {
double gSimTime = 0.0;
}

double simTime() { return gSimTime; }

void advanceSimTime(double realElapsed) {
    if (realElapsed > 0) gSimTime += std::min(realElapsed, 1.024);
}

// ProcessList::advanceServerTime: every tick up to `now`.
void advanceServer(double now) {
    static double lastTick = -1.0;
    if (lastTick < 0.0 || now < lastTick) lastTick = now;
    while (now - lastTick >= TickSeconds) {
        ServerTargets::tickSensorState();
        EngineObjects::forEachTicking([](EngineObject& object) {
            // ProcessList::advanceObjects: an object its client controls
            // ticks once for each pending move.
            auto* base = dynamic_cast<GameBase*>(&object);
            auto* connection = base && !base->controllingClient.empty()
                ? EngineObjects::get<GameConnection>(base->controllingClient) : nullptr;
            if (connection && object.script &&
                connection->controlObject() == ScriptEngine::instance().objectKey(object.script)) {
                auto& engine = ScriptEngine::instance();
                const std::string objectId = std::to_string(object.script->id);
                const std::string connectionId = std::to_string(connection->script->id);
                auto connectionState = connection->script->engine;
                connection->getMoveList();
                // Move generation and processMove can delete either endpoint
                // or transfer control. Do not dispatch further moves afterward.
                auto stillControlled = [&] {
                    auto* actor = engine.findObject(objectId.c_str());
                    auto* client = engine.findObject(connectionId.c_str());
                    return actor && actor->engine.get() == base && client && client->engine.get() == connection &&
                           !base->controllingClient.empty() && connection->controlObject() == engine.objectKey(actor);
                };
                while (stillControlled() && !connection->moves.empty()) {
                    const ClientMoveIn move = connection->moves.front();
                    connection->moves.pop_front();
                    base->processMove(&move);
                }
                return;
            }
            object.processTick();
        });
        lastTick += TickSeconds;
        SimState::server().timeMs += 32;
    }
}

} // namespace SimState
