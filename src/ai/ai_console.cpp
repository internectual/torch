// ai/aiConsole.cc: aiConnect, the AIConnection and AITask script methods,
// AISystemEnabled, the slicers.
#include "ai/ai_connection.h"
#include "ai/ai_step.h"
#include "ai/ai_task.h"
#include "sim/net_object.h"
#include "sim/player.h"
#include "sim/sim_state.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cstdio>
#include <strings.h>

namespace {

using Args = std::vector<VMValue>;

Point3F point(const VMValue& value) {
    Point3F p{0, 0, 0};
    std::sscanf(value.toString().c_str(), "%f %f %f", &p.x, &p.y, &p.z);
    return p;
}

std::string pointString(const Point3F& p) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%f %f %f", p.x, p.y, p.z);
    return buffer;
}

GameConnection* connectionArg(const VMValue& value) {
    ScriptObject* object = ScriptEngine::instance().findObject(value.toString().c_str());
    return object ? dynamic_cast<GameConnection*>(object->engine.get()) : nullptr;
}

// new <engine object>() + registerObject(name): a step or task, keyed by id
// (every bot's step of a kind registers under the kind's name).
std::string addEngineObject(const std::string& className, std::shared_ptr<EngineObject> engine) {
    auto& se = ScriptEngine::instance();
    auto* object = new ScriptObject;
    object->className = className;
    object->engine = std::move(engine);
    object->engine->script = object;
    se.addObject(object);
    se.objectAdded(object);
    return se.objectKey(object);
}

template <class Step, class... A>
void setStep(AIConnection& ai, const char* name, A&&... args) {
    auto step = std::make_shared<Step>(std::forward<A>(args)...);
    step->name = name;
    ai.setStep(addEngineObject(name, step));
}

} // namespace

void registerAINatives(TorqueScript& ts) {
    EngineObjects::registerClass("AIConnection", [] { return std::make_shared<AIConnection>(); });
    EngineObjects::registerClass("AITask", [] { return std::make_shared<AITask>(); });

    // AIObjectiveQ::sortByWeight (ai/aiObjective.cc): the queue's objectives
    // by weightLevel1, heaviest first. dQsort does not keep equal weights in
    // order; the stable sort keeps them in add order.
    ts.registerNative("AIObjectiveQ::sortByWeight", [](const Args& args) -> VMValue {
        auto& engine = ScriptEngine::instance();
        ScriptObject* queue = args.empty() ? nullptr : engine.findObject(args[0].toString().c_str());
        if (!queue) return VMValue("");
        const int count = queue->internals["__childCount"].toInt();
        std::vector<std::pair<int32_t, VMValue>> members;
        for (int i = 0; i < count; ++i) {
            const VMValue key = queue->internals["__child" + std::to_string(i)];
            ScriptObject* objective = engine.findObject(key.toString().c_str());
            members.emplace_back(objective ? Fields::s32(objective, "weightLevel1", 0) : 0, key);
        }
        std::stable_sort(members.begin(), members.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int i = 0; i < count; ++i) queue->internals["__child" + std::to_string(i)] = members[i].second;
        return VMValue("");
    });

    ts.registerNative("aiConnect", [](const Args& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& se = ScriptEngine::instance();
        se.ensureEngineGroups();
        ScriptObject* object = se.createEngineObject("AIConnection", "");
        if (!object) return VMValue(0);
        if (ScriptObject* group = se.findObject("ClientGroup")) se.addToSet(group, object);
        // NetConnection::getAddress of a connection with no net address.
        if (auto* ai = dynamic_cast<AIConnection*>(object->engine.get())) ai->address = "IP:0.0.0.0:0";
        std::string team = "-2", skill = "0.5", offense = "1", voice, voicePitch = "0";
        if (args.size() >= 2) team = args[1].toString();
        if (args.size() >= 3) skill = args[2].toString();
        if (args.size() >= 4)
            offense = strcasecmp(args[3].toString().c_str(), "true") == 0 || args[3].toInt() > 0 ? "1" : "0";
        if (args.size() >= 5) voice = args[4].toString();
        if (args.size() >= 6) voicePitch = args[5].toString();
        auto* ts = se.ts();
        if (ts)
            ts->callObjectMethod(se.objectKey(object), "onAIConnect",
                                 {VMValue(args[0].toString()), VMValue(team), VMValue(skill), VMValue(offense),
                                  VMValue(voice), VMValue(voicePitch)});
        return VMValue(se.objectId(object));
    });
    ts.registerNative("AIGetPathDistance", [](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        return VMValue(NavigationGraph::fastDistance(point(args[1]), point(args[0])));
    });
    ts.registerNative("AISlicerInit", [](const Args&) -> VMValue {
        gCalcWeightSlicer().init(30, 1);
        gScriptEngageSlicer().init(15, 2);
        return VMValue(1);
    });
    ts.registerNative("AISlicerReset", [](const Args&) -> VMValue {
        gCalcWeightSlicer().reset();
        gScriptEngageSlicer().reset();
        return VMValue(1);
    });

    // AIConnection methods (args[0] is the connection).
    auto method = [&ts](const char* name, std::function<VMValue(AIConnection&, const Args&)> body) {
        ts.registerNative(std::string("AIConnection::") + name, [body](const Args& args) -> VMValue {
            ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
            auto* ai = object ? dynamic_cast<AIConnection*>(object->engine.get()) : nullptr;
            return ai ? body(*ai, args) : VMValue("");
        });
    };
    method("drop", [](AIConnection& ai, const Args&) {
        auto* ts = ScriptEngine::instance().ts();
        const std::string key = ScriptEngine::instance().objectKey(ai.script);
        if (ts) ts->callObjectMethod(key, "onAIDrop", {});
        Console::instance().printf(LogLevel::Info, "AI Client %d is disconnected.", ai.id());
        if (ai.script) ai.script->internals["__disconnectReason"] = VMValue("Removed");
        if (ts) ts->callObjectMethod(key, "delete", {});
        return VMValue("");
    });
    method("setSkillLevel", [](AIConnection& ai, const Args& a) { ai.setSkillLevel(a.size() > 1 ? a[1].toFloat() : 0); return VMValue(""); });
    method("getSkillLevel", [](AIConnection& ai, const Args&) { return VMValue(ai.getSkillLevel()); });
    method("setEngageTarget", [](AIConnection& ai, const Args& a) {
        ai.setEngageTarget(a.size() > 1 ? connectionArg(a[1]) : nullptr);
        return VMValue("");
    });
    method("getEngageTarget", [](AIConnection& ai, const Args&) { return VMValue(ai.getEngageTarget()); });
    method("setVictim", [](AIConnection& ai, const Args& a) {
        GameConnection* victim = a.size() > 1 ? connectionArg(a[1]) : nullptr;
        auto* corpse = a.size() > 2 ? EngineObjects::get<PlayerObject>(a[2].toString()) : nullptr;
        if (victim && corpse) ai.setVictim(victim, corpse);
        return VMValue("");
    });
    method("getVictimCorpse", [](AIConnection& ai, const Args&) { return VMValue(ai.getVictimCorpse()); });
    method("getVictimTime", [](AIConnection& ai, const Args&) { return VMValue(ai.getVictimTime()); });
    method("hasLOSToClient", [](AIConnection& ai, const Args& a) {
        int losTime;
        Point3F losLocation;
        return VMValue(ai.hasLOSToClient(a.size() > 1 ? a[1].toInt() : 0, losTime, losLocation) ? 1 : 0);
    });
    method("getClientLOSTime", [](AIConnection& ai, const Args& a) {
        int losTime;
        Point3F losLocation;
        ai.hasLOSToClient(a.size() > 1 ? a[1].toInt() : 0, losTime, losLocation);
        return VMValue(losTime);
    });
    method("getDetectLocation", [](AIConnection& ai, const Args& a) {
        int losTime;
        Point3F losLocation{0, 0, 0};
        ai.hasLOSToClient(a.size() > 1 ? a[1].toInt() : 0, losTime, losLocation);
        return VMValue(pointString(losLocation));
    });
    method("clientDetected", [](AIConnection& ai, const Args& a) { ai.clientDetected(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    method("setDetectPeriod", [](AIConnection& ai, const Args& a) { ai.setDetectPeriod(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    method("getDetectPeriod", [](AIConnection& ai, const Args&) { return VMValue(ai.getDetectPeriod()); });
    method("setBlinded", [](AIConnection& ai, const Args& a) { ai.setBlinded(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    method("setDangerLocation", [](AIConnection& ai, const Args& a) {
        if (a.size() > 1) ai.setEvadeLocation(point(a[1]), a.size() == 3 ? a[2].toInt() : 0);
        return VMValue("");
    });
    method("setTargetObject", [](AIConnection& ai, const Args& a) {
        auto* object = a.size() > 1 ? EngineObjects::get<ShapeBase>(a[1].toString()) : nullptr;
        if (!object) {
            ai.setTargetObject(nullptr);
            return VMValue("");
        }
        const float range = a.size() >= 3 ? a[2].toFloat() : 30;
        int objectMode = AIConnection::DestroyObject;
        static const char* modes[AIConnection::NumObjectModes] = {"Destroy", "Repair", "Laze", "Mortar", "Missile",
                                                                  "MissileNoLock", "AttackMode1", "AttackMode2",
                                                                  "AttackMode3", "AttackMode4"};
        if (a.size() == 4)
            for (int i = 0; i < AIConnection::NumObjectModes; ++i)
                if (strcasecmp(a[3].toString().c_str(), modes[i]) == 0) {
                    objectMode = i;
                    break;
                }
        ai.setTargetObject(object, range, objectMode);
        return VMValue("");
    });
    method("getTargetObject", [](AIConnection& ai, const Args&) { return VMValue(ai.getTargetObject()); });
    method("targetInSight", [](AIConnection& ai, const Args&) { return VMValue(ai.targetInSight() ? 1 : 0); });
    method("targetInRange", [](AIConnection& ai, const Args&) { return VMValue(ai.targetInRange() ? 1 : 0); });
    method("pressFire", [](AIConnection& ai, const Args& a) { ai.scriptSustainFire(a.size() == 2 ? a[1].toInt() : 1); return VMValue(""); });
    method("pressJump", [](AIConnection& ai, const Args&) { ai.pressJump(); return VMValue(""); });
    method("pressJet", [](AIConnection& ai, const Args&) { ai.pressJet(); return VMValue(""); });
    method("pressGrenade", [](AIConnection& ai, const Args&) { ai.pressGrenade(); return VMValue(""); });
    method("pressMine", [](AIConnection& ai, const Args&) { ai.pressMine(); return VMValue(""); });
    method("setWeaponInfo", [](AIConnection& ai, const Args& a) {
        if (a.size() < 4) return VMValue("");
        ai.setWeaponInfo(a[1].toString(), a[2].toInt(), a[3].toInt(), a.size() >= 5 ? a[4].toInt() : 1,
                         a.size() >= 6 ? a[5].toFloat() : 0.0f, a.size() >= 7 ? a[6].toFloat() : 1.0f);
        return VMValue("");
    });
    method("aimAt", [](AIConnection& ai, const Args& a) {
        if (a.size() < 2) return VMValue(0);
        const int duration = a.size() == 3 ? std::max(0, a[2].toInt()) : 1500;
        return VMValue(ai.setScriptAimLocation(point(a[1]), duration) ? 1 : 0);
    });
    method("getAimLocation", [](AIConnection& ai, const Args&) { return VMValue(pointString(ai.getAimLocation())); });
    method("isMountingVehicle", [](AIConnection& ai, const Args&) {
        return VMValue(ai.getMoveMode() == AIConnection::ModeMountVehicle ? 1 : 0);
    });
    method("setTurretMounted", [](AIConnection& ai, const Args& a) { ai.setTurretMounted(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    method("setPilotDestination", [](AIConnection& ai, const Args& a) {
        if (a.size() > 1) ai.setPilotDestination(point(a[1]), a.size() >= 3 ? a[2].toFloat() : 1.0f);
        return VMValue("");
    });
    method("setPilotAim", [](AIConnection& ai, const Args& a) { if (a.size() > 1) ai.setPilotAimLocation(point(a[1])); return VMValue(""); });
    method("setPilotPitchRange", [](AIConnection& ai, const Args& a) {
        if (a.size() >= 4) ai.setPilotPitchRange(a[1].toFloat(), a[2].toFloat(), a[3].toFloat());
        return VMValue("");
    });
    method("getPathDistance", [](AIConnection& ai, const Args& a) {
        if (a.size() < 2) return VMValue(-1);
        return VMValue(ai.getPathDistance(point(a[1]), a.size() == 3 ? point(a[2]) : Point3F{-1, -1, -1}));
    });
    method("pathDistRemaining", [](AIConnection& ai, const Args& a) {
        return VMValue(ai.getPathDistRemaining(a.size() > 1 ? a[1].toFloat() : 0));
    });
    method("getLOSLocation", [](AIConnection& ai, const Args& a) {
        if (a.size() < 2) return VMValue("");
        return VMValue(pointString(ai.getLOSLocation(point(a[1]), a.size() >= 3 ? a[2].toFloat() : -1,
                                                     a.size() >= 4 ? a[3].toFloat() : 1e6f,
                                                     a.size() >= 5 ? point(a[4]) : Point3F{-1, -1, -1})));
    });
    method("getHideLocation", [](AIConnection& ai, const Args& a) {
        if (a.size() < 5) return VMValue("");
        return VMValue(pointString(ai.getHideLocation(point(a[1]), a[2].toFloat(), point(a[3]), a[4].toFloat())));
    });
    method("clearStep", [](AIConnection& ai, const Args&) { ai.clearStep(); return VMValue(""); });
    method("getStepStatus", [](AIConnection& ai, const Args&) { return VMValue(ai.getStepStatus()); });
    method("getStepName", [](AIConnection& ai, const Args&) { return VMValue(ai.getStepName()); });
    method("stop", [](AIConnection& ai, const Args&) { ai.setMoveMode(AIConnection::ModeStop); return VMValue(""); });
    // (the shipped setMoveSpeed takes dAtoi of its argument)
    method("stepMove", [](AIConnection& ai, const Args& a) {
        ai.clearStep();
        if (a.size() < 2) return VMValue("");
        ai.setMoveDestination(point(a[1]));
        ai.setMoveTolerance(a.size() >= 3 ? a[2].toFloat() : 0.25f);
        if (ai.getMoveMode() != AIConnection::ModeStuck)
            ai.setMoveMode(a.size() >= 4 ? a[3].toInt() : AIConnection::ModeExpress);
        return VMValue("");
    });
    method("stepEscort", [](AIConnection& ai, const Args& a) {
        ai.clearStep();
        if (GameConnection* target = a.size() > 1 ? connectionArg(a[1]) : nullptr)
            setStep<AIStepEscort>(ai, "AIStepEscort", ScriptEngine::instance().objectKey(target->script));
        return VMValue("");
    });
    method("stepEngage", [](AIConnection& ai, const Args& a) {
        GameConnection* target = a.size() > 1 ? connectionArg(a[1]) : nullptr;
        if (!target) {
            ai.clearStep();
            return VMValue("");
        }
        // already engaging this target
        if (strcasecmp(ai.getStepName(), "AIStepEngage") == 0 &&
            ai.getEngageTarget() == ScriptEngine::instance().objectId(target->script))
            return VMValue("");
        setStep<AIStepEngage>(ai, "AIStepEngage", ScriptEngine::instance().objectKey(target->script));
        return VMValue("");
    });
    method("stepRangeObject", [](AIConnection& ai, const Args& a) {
        if (a.size() < 5) return VMValue("");
        ScriptObject* target = ScriptEngine::instance().findObject(a[1].toString().c_str());
        if (!target) return VMValue("");
        const Point3F from = a.size() == 6 ? point(a[5]) : Point3F{0, 0, 0};
        setStep<AIStepRangeObject>(ai, "AIStepRangeObject", ScriptEngine::instance().objectKey(target), a[2].toString(),
                                   a[3].toFloat(), a[4].toFloat(), a.size() == 6 ? &from : nullptr);
        return VMValue("");
    });
    method("stepIdle", [](AIConnection& ai, const Args& a) {
        ai.clearStep();
        const Point3F idleLocation = a.size() > 1 ? point(a[1]) : Point3F{0, 0, 0};
        setStep<AIStepIdlePatrol>(ai, "AIStepIdlePatrol", &idleLocation);
        return VMValue("");
    });
    method("stepJet", [](AIConnection& ai, const Args& a) {
        if (a.size() < 2) return VMValue("");
        ai.clearStep();
        setStep<AIStepJet>(ai, "AIStepJet", point(a[1]));
        return VMValue("");
    });
    method("setPath", [](AIConnection& ai, const Args& a) {
        if (a.size() == 2) {
            const Point3F dest = point(a[1]);
            ai.setPathDest(&dest);
        } else {
            ai.setPathDest();
        }
        return VMValue("");
    });
    method("clearTasks", [](AIConnection& ai, const Args&) { ai.clearTasks(); return VMValue(""); });
    // addTask(taskName): an AITask whose namespace is the task name.
    method("addTask", [](AIConnection& ai, const Args& a) {
        if (a.size() < 2) return VMValue("");
        auto task = std::make_shared<AITask>();
        task->name = a[1].toString();
        const std::string key = addEngineObject("AITask", task);
        if (task->script) task->script->internals["__namespace"] = VMValue(task->name);
        ai.addTask(key);
        return VMValue(ScriptEngine::instance().objectId(task->script));
    });
    method("removeTask", [](AIConnection& ai, const Args& a) { ai.removeTask(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    method("listTasks", [](AIConnection& ai, const Args&) { ai.listTasks(); return VMValue(""); });
    method("getTaskId", [](AIConnection& ai, const Args&) {
        AITask* task = ai.currentTask();
        return VMValue(task && task->script ? ScriptEngine::instance().objectId(task->script) : -1);
    });
    method("getTaskName", [](AIConnection& ai, const Args&) {
        AITask* task = ai.currentTask();
        return VMValue(task ? task->name : std::string());
    });
    method("getTaskTime", [](AIConnection& ai, const Args&) { return VMValue(ai.getTaskTime()); });
    method("missionCycleCleanup", [](AIConnection& ai, const Args&) { ai.missionCycleCleanup(); return VMValue(""); });

    // AITask methods.
    auto task = [&ts](const char* name, std::function<VMValue(AITask&, const Args&)> body) {
        ts.registerNative(std::string("AITask::") + name, [body](const Args& args) -> VMValue {
            auto* t = args.empty() ? nullptr : EngineObjects::get<AITask>(args[0].toString());
            return t ? body(*t, args) : VMValue("");
        });
    };
    // (the shipped setWeightFreq triples the frequency: calcWeight was 10%
    // of some profiles)
    task("setWeightFreq", [](AITask& t, const Args& a) { t.setWeightFreq((a.size() > 1 ? a[1].toInt() : 0) * 3); return VMValue(""); });
    task("setWeight", [](AITask& t, const Args& a) { t.setWeight(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    task("getWeight", [](AITask& t, const Args&) { return VMValue(std::to_string(t.getWeight())); });
    task("reWeight", [](AITask& t, const Args&) { t.reWeight(); return VMValue(""); });
    task("setMonitorFreq", [](AITask& t, const Args& a) { t.setMonitorFreq(a.size() > 1 ? a[1].toInt() : 0); return VMValue(""); });
    task("reMonitor", [](AITask& t, const Args&) { t.reMonitor(); return VMValue(""); });
}
