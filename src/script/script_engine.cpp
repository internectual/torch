#include "script/script_engine.h"
#include "render/commander_map.h"
#include "sim/game_connection.h"
#include "sim/engine_classes.h"
#include "sim/sim_state.h"
#include "sim/sim_natives.h"
#include "sim/engine_object.h"
#include "sim/path_manager.h"
#include "sim/projectile_aim.h"
#include "sim/net_string_table.h"
#include "sim/net_object.h"
#include "sim/game_base.h"
#include "game/material_property_map.h"
#include <limits>
#include "script/conversion_parity.h"
#include "script/torquescript.h"
#include "core/console.h"
#include "core/console_args.h"
#include "core/config.h"
#include "core/engine.h"
#include "core/string_table.h"
#include "game/damage_parity.h"
#include "game/mission_parser.h"
#include "game/mission_discovery.h"
#include "game/demo.h"
#include "game/wind.h"
#include "render/environment_commands.h"
#include "net/master_query.h"
#include <fstream>
#include <sstream>
#include <stack>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <climits>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <filesystem>
#include <fnmatch.h>
#include <chrono>

static bool parseObjectId(const std::string& handle, int& id);

namespace {
std::string lowerName(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    return lower;
}
} // namespace


extern char** environ;

namespace {

static bool sameFieldName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

static bool sameObjectValue(const VMValue& a, const VMValue& b) {
    return a.type == b.type && a.toString() == b.toString();
}

static VMValue* findObjectField(ScriptObject* object, const std::string& name) {
    if (!object) return nullptr;
    auto it = object->fields.find(name); // FieldMap matches names case-insensitively
    return it == object->fields.end() ? nullptr : &it->second;
}

static ScriptObject* namedScriptObject(const std::string& name) {
    return name.empty() ? nullptr : ScriptEngine::instance().findObject(name.c_str());
}

// SimObject::getName: anonymous objects have none (an anonymous GUI control
// keeps an internal "_unnamed" registry name).
static std::string scriptObjectName(const ScriptObject* object) {
    if (!object || object->name.rfind("_unnamed", 0) == 0) return {};
    return object->name;
}

static bool providerObjectId(const std::string& value, int& id) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) return false;
    id = (int)parsed;
    return true;
}

static VMValue providerField(const ScriptObjectState& state, const std::string& field) {
    if (sameFieldName(field, "datablock")) return VMValue(state.datablockId);
    if (sameFieldName(field, "class") || sameFieldName(field, "classname")) return VMValue(state.className);
    if (sameFieldName(field, "name")) return VMValue(state.name);
    if (sameFieldName(field, "shapeFile")) return VMValue(state.shapeName);
    if (sameFieldName(field, "skin")) return VMValue(state.skinName);
    if (sameFieldName(field, "profile")) return VMValue(state.profileName);
    if (sameFieldName(field, "type")) return VMValue(state.type);
    if (sameFieldName(field, "position")) {
        char value[96];
        snprintf(value, sizeof(value), "%g %g %g", state.position.x, state.position.y, state.position.z);
        return VMValue(value);
    }
    if (sameFieldName(field, "velocity")) {
        char value[96];
        snprintf(value, sizeof(value), "%g %g %g", state.velocity.x, state.velocity.y, state.velocity.z);
        return VMValue(value);
    }
    if (sameFieldName(field, "rotation")) {
        char value[128];
        snprintf(value, sizeof(value), "%g %g %g %g", state.rotation.x,
                 state.rotation.y, state.rotation.z, state.rotationW);
        return VMValue(value);
    }
    if (sameFieldName(field, "health") && state.hasHealth) return VMValue(state.health);
    if (sameFieldName(field, "maxHealth") && state.hasMaxHealth) return VMValue(state.maxHealth);
    if (sameFieldName(field, "energy") && state.hasEnergy) return VMValue(state.energy);
    if (sameFieldName(field, "damageState") && state.hasDamageState) return VMValue(state.damageState);
    if (sameFieldName(field, "repairRate")) return VMValue(state.repairRate);
    if (sameFieldName(field, "sensorGroup")) return VMValue(state.sensorGroup);
    if (sameFieldName(field, "jetting") && state.hasVehicleState) return VMValue(state.jetting ? 1 : 0);
    if (sameFieldName(field, "frozen") && state.hasVehicleState) return VMValue(state.frozen ? 1 : 0);
    if (sameFieldName(field, "braking") && state.hasVehicleState) return VMValue(state.braking ? 1 : 0);
    if (sameFieldName(field, "cloaked") && state.hasCloak) return VMValue(state.cloaked ? 1 : 0);
    if (sameFieldName(field, "headRotation") && state.hasHeadAngles) {
        char value[64];
        snprintf(value, sizeof(value), "%g %g", state.headPitch, state.headYaw);
        return VMValue(value);
    }
    if (sameFieldName(field, "barrelRotation") && state.hasTurretAim) {
        char value[64];
        snprintf(value, sizeof(value), "%g %g", state.barrelPitch, state.barrelYaw);
        return VMValue(value);
    }
    if (sameFieldName(field, "shieldLevel") && state.hasShield)
        return VMValue(state.shieldLevel);
    if (sameFieldName(field, "team")) return VMValue(state.teamId);
    if (sameFieldName(field, "state") || sameFieldName(field, "damageState")) return VMValue(state.state);
    return VMValue("");
}

static std::vector<std::string> splitFields(const std::string& value) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('\t', start);
        result.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

static std::vector<std::string> splitScriptWords(const std::string& value) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && std::isspace((unsigned char)value[pos])) ++pos;
        if (pos == value.size()) break;
        const size_t start = pos;
        while (pos < value.size() && !std::isspace((unsigned char)value[pos])) ++pos;
        result.push_back(value.substr(start, pos - start));
    }
    return result;
}

static std::string replaceAll(std::string value, const std::string& from,
                              const std::string& to) {
    if (from.empty()) return value;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

struct ScriptTarget {
    std::string objectName;
    VMValue nameTag;
    VMValue skinTag;
    VMValue voiceTag;
    VMValue typeTag;
    int sensorGroup = 0;
    VMValue datablock;
    double voicePitch = 1.0;
    uint32_t renderMask = 0;
    uint32_t alwaysVisMask = 0;
    VMValue sensorData;
};

std::unordered_map<int, ScriptTarget> s_scriptTargets;
int s_nextScriptTarget = 1;

static void clearScriptMissionState() {
    s_scriptTargets.clear();
    s_nextScriptTarget = 1;
    if (auto* taskList = ScriptEngine::instance().findObject("TaskList")) {
        taskList->fields["currentTaskClient"] = VMValue("");
        taskList->fields["currentAIObjective"] = VMValue("");
        taskList->fields["currentTaskIsTeam"] = VMValue("0");
        taskList->fields["currentTaskDescription"] = VMValue("");
    }
}

static bool parseFogColor(const std::vector<VMValue>& args, float& r, float& g,
                          float& b, float& a) {
    if (args.size() >= 4) {
        r = args[0].toFloat(); g = args[1].toFloat();
        b = args[2].toFloat(); a = args[3].toFloat();
        return true;
    }
    if (args.size() != 1) return false;
    return sscanf(args[0].toString().c_str(), "%f %f %f %f", &r, &g, &b, &a) == 4;
}

static VMValue setScriptFogDistance(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    auto& fog = Engine::instance().game().world().fog;
    fog.transitioning = false;
    const bool applied = applyFogDistance(fog.distance, fog.density, args[0].toFloat());
    if (applied) fog.enabled = true;
    return VMValue(applied ? 1 : 0);
}

static VMValue setScriptFogDensity(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    auto& fog = Engine::instance().game().world().fog;
    fog.transitioning = false;
    const bool applied = applyFogDensity(fog.distance, fog.density, args[0].toFloat());
    if (applied) fog.enabled = true;
    return VMValue(applied ? 1 : 0);
}

static VMValue setScriptFogColor(const std::vector<VMValue>& args) {
    float r, g, b, a;
    if (!parseFogColor(args, r, g, b, a)) return VMValue(0);
    auto& fog = Engine::instance().game().world().fog;
    fog.transitioning = false;
    const bool applied = applyFogColor(fog.color, r, g, b, a);
    if (applied) fog.enabled = true;
    return VMValue(applied ? 1 : 0);
}

static VMValue setScriptSkyColor(const std::vector<VMValue>& args) {
    float r, g, b, a = 1.0f;
    if (args.size() == 1) {
        if (sscanf(args[0].toString().c_str(), "%f %f %f %f", &r, &g, &b, &a) < 3) return VMValue(0);
    } else if (args.size() == 3 || args.size() == 4) {
        r = args[0].toFloat(); g = args[1].toFloat(); b = args[2].toFloat();
        if (args.size() == 4) a = args[3].toFloat();
    } else return VMValue(0);
    return VMValue(Engine::instance().game().world().setSkyColor({r, g, b, a}) ? 1 : 0);
}

static VMValue setScriptSkyMaterialList(const std::vector<VMValue>& args) {
    if (args.size() != 1) return VMValue(0);
    return VMValue(Engine::instance().game().world().setSkyMaterialList(args[0].toString()) ? 1 : 0);
}

static bool parseVector(const VMValue& value, Point3F& result) {
    return sscanf(value.toString().c_str(), "%f %f %f", &result.x, &result.y, &result.z) == 3;
}

static VMValue setScriptSunDirection(const std::vector<VMValue>& args) {
    Point3F direction;
    if (args.size() == 1 ? !parseVector(args[0], direction) : args.size() != 3) return VMValue(0);
    if (args.size() == 3) direction = {args[0].toFloat(), args[1].toFloat(), args[2].toFloat()};
    return VMValue(Engine::instance().game().world().setSunDirection(direction) ? 1 : 0);
}

static VMValue setScriptSunColor(const std::vector<VMValue>& args) {
    float r, g, b, a = 1.0f;
    if (args.size() == 1) {
        if (sscanf(args[0].toString().c_str(), "%f %f %f %f", &r, &g, &b, &a) < 3) return VMValue(0);
    } else if (args.size() == 3 || args.size() == 4) {
        r = args[0].toFloat(); g = args[1].toFloat(); b = args[2].toFloat();
        if (args.size() == 4) a = args[3].toFloat();
    } else return VMValue(0);
    return VMValue(Engine::instance().game().world().setSunColor({r, g, b, a}) ? 1 : 0);
}

static VMValue setScriptSunAmbient(const std::vector<VMValue>& args) {
    float r, g, b, a = 1.0f;
    if (args.size() == 1) {
        if (sscanf(args[0].toString().c_str(), "%f %f %f %f", &r, &g, &b, &a) < 3) return VMValue(0);
    } else if (args.size() == 3 || args.size() == 4) {
        r = args[0].toFloat(); g = args[1].toFloat(); b = args[2].toFloat();
        if (args.size() == 4) a = args[3].toFloat();
    } else return VMValue(0);
    return VMValue(Engine::instance().game().world().setSunAmbient({r, g, b, a}) ? 1 : 0);
}

static VMValue setScriptFogTransition(const std::vector<VMValue>& args) {
    if (args.size() < 2) return VMValue(0);
    ColorF color{};
    const ColorF* colorPtr = nullptr;
    if (args.size() >= 3) {
        float r, g, b, a = 1.0f;
        if (args.size() == 3 && sscanf(args[2].toString().c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)
            color = {r, g, b, a};
        else if (args.size() == 5) {
            r = args[2].toFloat(); g = args[3].toFloat(); b = args[4].toFloat();
            color = {r, g, b, a};
        } else return VMValue(0);
        colorPtr = &color;
    }
    return VMValue(Engine::instance().game().world().setFogTransition(
        args[0].toFloat() / 1000.0f, args[1].toFloat(), colorPtr) ? 1 : 0);
}

static VMValue setScriptWaterLevel(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    const std::string target = args.size() > 1 ? args[0].toString() : "";
    const float level = args.back().toFloat();
    return VMValue(Engine::instance().game().world().setWaterLevel(target, level) ? 1 : 0);
}

static VMValue setScriptWaterType(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    const std::string target = args.size() > 1 ? args[0].toString() : "";
    int type = 0;
    if (!parseWaterType(args.back().toString(), type)) return VMValue(0);
    return VMValue(Engine::instance().game().world().setWaterType(target, type) ? 1 : 0);
}

static VMValue setScriptWaterOpacity(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    const std::string target = args.size() > 1 ? args[0].toString() : "";
    return VMValue(Engine::instance().game().world().setWaterOpacity(target, args.back().toFloat()) ? 1 : 0);
}

static VMValue setScriptWaterColor(const std::vector<VMValue>& args) {
    if (args.empty()) return VMValue(0);
    const bool targeted = args.size() == 2;
    const std::string target = targeted ? args[0].toString() : "";
    const std::string value = targeted ? args[1].toString() : args[0].toString();
    float r = 0, g = 0, b = 0, a = 1;
    if (targeted || args.size() == 1) {
        if (sscanf(value.c_str(), "%f %f %f %f", &r, &g, &b, &a) < 3) return VMValue(0);
    } else if (args.size() == 3 || args.size() == 4) {
        r = args[0].toFloat(); g = args[1].toFloat(); b = args[2].toFloat();
        if (args.size() == 4) a = args[3].toFloat();
    } else return VMValue(0);
    ColorF color{r, g, b, a};
    if (!applyWaterColor(color, r, g, b) || !applyWaterOpacity(color.a, a)) return VMValue(0);
    return VMValue(Engine::instance().game().world().setWaterColor(target, color) ? 1 : 0);
}

static VMValue setScriptPrecipitation(const std::vector<VMValue>& args) {
    if (args.size() != 2) return VMValue(0);
    return VMValue(Engine::instance().game().world().setPrecipitation(args[0].toInt(), args[1].toFloat()) ? 1 : 0);
}

static VMValue setScriptPrecipitationEnabled(const std::vector<VMValue>& args) {
    if (args.size() != 1) return VMValue(0);
    return VMValue(Engine::instance().game().world().setPrecipitationEnabled(args[0].toBool()) ? 1 : 0);
}

static VMValue setScriptPrecipitationType(const std::vector<VMValue>& args) {
    if (args.size() != 1) return VMValue(0);
    return VMValue(Engine::instance().game().world().setPrecipitationType(args[0].toInt()) ? 1 : 0);
}

static VMValue setScriptPrecipitationWind(const std::vector<VMValue>& args) {
    Point3F velocity;
    if (args.size() == 1 ? !parseVector(args[0], velocity) : args.size() != 3) return VMValue(0);
    if (args.size() == 3) velocity = {args[0].toFloat(), args[1].toFloat(), args[2].toFloat()};
    return VMValue(Engine::instance().game().world().setPrecipitationWind(velocity) ? 1 : 0);
}

static VMValue setScriptPrecipitationBox(const std::vector<VMValue>& args) {
    if (args.size() != 2) return VMValue(0);
    return VMValue(Engine::instance().game().world().setPrecipitationBox(args[0].toFloat(), args[1].toFloat()) ? 1 : 0);
}

static VMValue setScriptLightning(const std::vector<VMValue>& args) {
    if (args.size() != 1) return VMValue(0);
    return VMValue(Engine::instance().game().world().setLightningEnabled(args[0].toBool()) ? 1 : 0);
}

static VMValue strikeScriptLightning(const std::vector<VMValue>& args) {
    if (!args.empty()) return VMValue(0);
    return VMValue(Engine::instance().game().world().strikeLightning() ? 1 : 0);
}

// Commands assigned by scripts after a .gui has been parsed must update both
// the live control and its click closure.  The stock message-box helpers do
// exactly this when they install their transient button callbacks.
static bool setGuiCommand(const std::string& name, const std::string& command) {
    auto* ctl = Engine::instance().guiRenderer().findControl(name);
    if (!ctl) return false;
    ctl->command = command;
    if (auto* obj = ScriptEngine::instance().findObject(name.c_str()))
        obj->fields["command"] = VMValue(command);
    ctl->onClick = nullptr;
    if (!command.empty()) {
        ctl->onClick = [command]() {
            Console::instance().execute(command.c_str());
        };
    }
    return true;
}
}

// === VMValue ===
int32_t VMValue::toInt() const {
    switch (type) {
        case Int: return i;
        case Float: return (int32_t)f;
        case String: return atoi(str.c_str());
        default: return 0;
    }
}

float VMValue::toFloat() const {
    switch (type) {
        case Int: return (float)i;
        case Float: return (float)f;
        case String: return (float)atof(str.c_str());
        default: return 0.0f;
    }
}

double VMValue::toDouble() const {
    switch (type) {
        case Int: return (double)i;
        case Float: return f;
        case String: return atof(str.c_str());
        default: return 0.0;
    }
}

std::string VMValue::toString() const {
    switch (type) {
        case Int: return std::to_string(i);
        case Float: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", f);
            return buf;
        }
        case String: return str;
        default: return "";
    }
}

bool VMValue::toBool() const {
    switch (type) {
        case Int: return i != 0;
        case Float: return f != 0.0;
        case String: {
            std::string value = str;
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return !value.empty() && value != "0" && value != "false";
        }
        default: return false;
    }
}

// === ScriptObject ===
ScriptObject* findScriptObject(const char* name) {
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(StringTable::instance().insert(name));
    if (it != objs.end()) return it->second;
    for (const auto& [objectName, object] : objs)
        if (sameFieldName(objectName, name ? name : "")) return object;
    return nullptr;
}

// === VirtualMachine ===
struct VMContext {
    DSOFile* dso{};
    uint32_t ip{};
    std::vector<VMValue> exprStack;
    std::unordered_map<std::string, VMValue> locals;
    std::string curVarName;
    ScriptObject* curObject{};
    std::string curFieldName;
    VMValue result;

    // Array index for SETCURVAR_ARRAY
    std::string curArrayKey;

    // String builder
    std::string strBuilder;
    int strBuilderLen{};
};

struct ArgFrame {
    std::vector<VMValue> args;
};

struct VirtualMachine::Impl {
    ScriptEngine* engine;
    std::vector<DSOFile*> loaded;
    std::stack<VMContext> callStack;
    std::unordered_map<std::string, VMValue> globals;
    std::unordered_map<std::string, NativeFunc> natives;
    std::vector<ArgFrame> argFrames;

    // Current variable/object/field context
    std::string curVar;
    ScriptObject* curObj{};
    std::string curField;

    Impl(ScriptEngine* e) : engine(e) {}
};

VirtualMachine::VirtualMachine(ScriptEngine* engine) : impl(new Impl(engine)) {}
VirtualMachine::~VirtualMachine() {
    for (auto dso : impl->loaded) delete dso;
    delete impl;
}

void VirtualMachine::registerNativeFunction(const char* name, NativeFunc fn) {
    impl->natives[name] = std::move(fn);
}

bool VirtualMachine::unloadScript(const char* name) {
    if (!name) return false;
    bool removed = false;
    for (auto it = impl->loaded.begin(); it != impl->loaded.end();) {
        if (*it && (*it)->filename == name) {
            delete *it;
            it = impl->loaded.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
}

VMValue VirtualMachine::getVariable(const char* name) {
    auto it = impl->globals.find(name);
    if (it != impl->globals.end()) return it->second;
    auto* item = Console::instance().find(name);
    if (item && item->type == Console::ConsoleItem::Variable)
        return VMValue(item->value.c_str());
    return VMValue(0);
}

void VirtualMachine::setVariable(const char* name, const VMValue& val) {
    impl->globals[name] = val;
    Console::instance().setVariable(name, val.toString().c_str());
}

ScriptObject* VirtualMachine::getObject(const char* name) {
    return ScriptEngine::instance().findObject(name);
}

void VirtualMachine::addObject(ScriptObject* obj) {
    if (!obj) return;
    ScriptEngine::instance().addObject(obj);
}

bool VirtualMachine::loadScript(const uint8_t* data, size_t size, const char* name) {
    auto* dso = new DSOFile;
    DSOReader reader;
    if (!reader.read(data, size, *dso)) {
        Console::instance().printf(LogLevel::Warn, "VM: failed to load DSO: %s", name ? name : "unknown");
        delete dso;
        return false;
    }
    if (dso->version != 33 && dso->version != 174) {
        Console::instance().printf(LogLevel::Warn,
                                   "VM: unsupported DSO version %u: %s",
                                   dso->version, name ? name : "unknown");
        delete dso;
        return false;
    }

    // Walk opcodes to find function declarations
    // Each code slot is either a byte opcode or an extended opcode (0xFF + u32)
    // We need to parse opcodes and their arguments to find OP_FUNC_DECL

    const uint8_t* code = dso->code.data();
    size_t codeLen = dso->code.size();
    uint32_t ip = 0;
    std::vector<uint32_t> opcodes;
    std::vector<uint32_t> ips;

    // Decode opcodes from raw code
    const uint8_t* cp = code;
    size_t cl = codeLen;
    while (cl > 0 && ip < dso->codeSize) {
        uint32_t op = *cp++;
        cl--;
        if (op == 0xFF && cl >= 4) {
            op = *(const uint32_t*)cp;
            cp += 4; cl -= 4;
        }
        opcodes.push_back(op);
        ips.push_back(ip);
        ip++;
    }

    // Now walk opcodes looking for OP_FUNC_DECL
    size_t i = 0;
    while (i < opcodes.size()) {
        uint32_t op = opcodes[i];
        uint32_t curIp = ips[i];

        switch (static_cast<DSOOpcode>(op & 0xFF)) {
            case DSOOpcode::OP_FUNC_DECL: {
                DSOFunction fn;
                // Args: nameIdx, nsIdx, packageIdx, hasBody, endAddr, argc,
                // [argNameIdxs...]. Tribes 2/TURD has no separate varargs slot.
                if (i + 6 < opcodes.size()) {
                    uint32_t nameIdx = opcodes[i + 1];
                    uint32_t nsIdx = opcodes[i + 2];
                    uint32_t pkgIdx = opcodes[i + 3];
                    uint32_t hasBody = opcodes[i + 4];
                    uint32_t endAddr = opcodes[i + 5];
                    uint32_t argc = opcodes[i + 6];

                    fn.name = reader.globalString(*dso, nameIdx);
                    // TURD uses zero as the no-namespace/no-package sentinel;
                    // offset zero in the packed string table is also a valid
                    // function name, so only these two fields need sentinel
                    // handling.
                    fn.ns = nsIdx == 0 ? "" : reader.globalString(*dso, nsIdx);
                    fn.package = pkgIdx == 0 ? "" : reader.globalString(*dso, pkgIdx);
                    fn.startIp = curIp;
                    fn.endIp = endAddr;
                    fn.argc = argc;
                    fn.hasVarArgs = false;

                    // Read arg names
                    uint32_t argStart = i + 7;
                    for (uint32_t a = 0; a < argc && argStart + a < opcodes.size(); a++) {
                        fn.argNames.push_back(reader.globalString(*dso, opcodes[argStart + a]));
                    }

                    dso->functions.push_back(fn);
                    // A namespaced function is only Namespace::name: a bare
                    // call must not reach IRCClient::connect.
                    if (fn.ns.empty()) dso->funcMap[fn.name] = &dso->functions.back();
                    else dso->funcMap[fn.ns + "::" + fn.name] = &dso->functions.back();

                    Console::instance().printf(LogLevel::Debug, "VM: func %s (ip=%u, end=%u, argc=%u%s)%s%s%s",
                        fn.name.c_str(), fn.startIp, fn.endIp, fn.argc, fn.hasVarArgs ? "+" : "",
                        fn.ns.empty() ? "" : (" ns:" + fn.ns).c_str(),
                        fn.package.empty() ? "" : (" pkg:" + fn.package).c_str(),
                        hasBody ? "" : " [ext]");
                    // Function bodies can contain byte value 0, which is also
                    // OP_FUNC_DECL. Skip directly to the compiler's exclusive
                    // end address instead of scanning body operands as new
                    // declarations.
                    const size_t nextFunction = hasBody && endAddr > curIp
                        ? (size_t)endAddr : i + 7 + argc;
                    i = nextFunction > i ? nextFunction : i + 1;
                } else {
                    i += 8;
                }
                break;
            }
            default:
                i++;
                break;
        }
    }

    impl->loaded.push_back(dso);
    dso->filename = name ? name : "";
    Console::instance().printf(LogLevel::Info, "VM: loaded '%s' v%u (%zu funcs)", name ? name : "unknown", dso->version, dso->functions.size());
    return true;
}

bool VirtualMachine::loadScriptFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    return loadScript(data.data(), data.size(), path);
}

const std::vector<DSOFile*>& VirtualMachine::loadedScripts() const {
    return impl->loaded;
}

VMValue VirtualMachine::callFunction(const char* name, const std::vector<VMValue>& args) {
    // Check natives first
    auto nit = impl->natives.find(name);
    if (nit != impl->natives.end()) {
        return nit->second(args);
    }

    // Find in loaded DSOs
    for (auto* dso : impl->loaded) {
        auto fit = dso->funcMap.find(name);
        if (fit != dso->funcMap.end()) {
            if (!fit->second->package.empty() &&
                (!ScriptEngine::instance().ts() ||
                 !ScriptEngine::instance().ts()->isActivePackage(fit->second->package)))
                continue;
            return execute(dso, fit->second->startIp, args);
        }
    }

    return {};
}

bool VirtualMachine::callScriptFunction(const char* name, const std::vector<VMValue>& args,
                                        VMValue& result) {
    for (auto* dso : impl->loaded) {
        auto fit = dso->funcMap.find(name);
        if (fit == dso->funcMap.end()) continue;
        if (!fit->second->package.empty() &&
            (!ScriptEngine::instance().ts() ||
             !ScriptEngine::instance().ts()->isActivePackage(fit->second->package)))
            continue;
        result = execute(dso, fit->second->startIp, args);
        return true;
    }
    return false;
}

VMValue VirtualMachine::callMethod(const char* objName, const char* method, const std::vector<VMValue>& args) {
    // DSO methods receive the target as %this before explicit arguments.
    std::string fullName = std::string(objName) + "::" + method;
    std::vector<VMValue> methodArgs;
    methodArgs.reserve(args.size() + 1);
    methodArgs.emplace_back(objName);
    methodArgs.insert(methodArgs.end(), args.begin(), args.end());
    for (auto* dso : impl->loaded) {
        auto fit = dso->funcMap.find(fullName);
        if (fit != dso->funcMap.end() &&
            (fit->second->package.empty() ||
             (ScriptEngine::instance().ts() &&
              ScriptEngine::instance().ts()->isActivePackage(fit->second->package))))
            return execute(dso, fit->second->startIp, methodArgs, true);
    }
    return {};
}

VMValue VirtualMachine::execute(DSOFile* dso, uint32_t startIp,
                                const std::vector<VMValue>& args, bool methodCall) {
    VMContext ctx;
    ctx.dso = dso;
    ctx.ip = startIp;
    uint32_t bodyStart = startIp;

    // Map passed arguments to local variable names by matching to function declaration
    for (auto& fn : dso->functions) {
        if (fn.startIp == startIp) {
            // For namespaced functions, %this is the method's target object
            // (passed as args[0] from the method dispatch, before script-level args)
            bool isMethod = methodCall && !fn.ns.empty();
            uint32_t argOfs = 0;
            if (isMethod && args.size() > 0) {
                ctx.locals["%this"] = args[0];
                argOfs = 1;
            }
            // TorqueScript exposes the complete call frame through these
            // variables.  Keep DSO functions consistent with the source
            // interpreter, including empty values for omitted parameters.
            ctx.locals["%argc"] = VMValue((int32_t)args.size());
            for (size_t ai = 0; ai < args.size(); ++ai)
                ctx.locals["%argv[" + std::to_string(ai) + "]"] = args[ai];
            for (uint32_t ai = 0; ai < fn.argc && ai + argOfs < (uint32_t)args.size(); ai++) {
                std::string localName = ai < fn.argNames.size() ? fn.argNames[ai] : "";
                if (localName.empty() || (localName[0] != '%' && localName[0] != '$'))
                    localName.insert(localName.begin(), '%');
                ctx.locals[localName] = args[ai + argOfs];
            }
            for (uint32_t ai = 0; ai < fn.argc; ++ai) {
                std::string localName = ai < fn.argNames.size() ? fn.argNames[ai] : "";
                if (localName.empty() || (localName[0] != '%' && localName[0] != '$'))
                    localName.insert(localName.begin(), '%');
                if (!ctx.locals.count(localName)) ctx.locals[localName] = VMValue("");
            }
            bodyStart = fn.startIp + 7 + fn.argc;
            const uint32_t varArgStart = fn.argc + argOfs;
            if (fn.hasVarArgs && varArgStart < (uint32_t)args.size()) {
                std::string varargStr;
                for (uint32_t ai = varArgStart; ai < (uint32_t)args.size(); ai++) {
                    if (ai > varArgStart) varargStr += "\t";
                    varargStr += args[ai].toString();
                }
                ctx.locals["%__rest__"] = VMValue(varargStr);
            }
            break;
        }
    }

    impl->callStack.push(ctx);
    VMContext* frame = &impl->callStack.top();

    // Decode opcode stream
    const uint8_t* code = dso->code.data();
    size_t codeLen = dso->code.size();

    // First, decode all opcodes with their slot positions
    std::vector<uint32_t> opcodes;
    std::vector<uint32_t> slotToRaw; // slot index -> raw byte position in code
    const uint8_t* cp = code;
    size_t cl = codeLen;
    uint32_t slotIdx = 0;
    while (cl > 0 && slotIdx < dso->codeSize) {
        slotToRaw.push_back(cp - code);
        uint32_t op = *cp++;
        cl--;
        if (op == 0xFF && cl >= 4) {
            op = *(const uint32_t*)cp;
            cp += 4; cl -= 4;
        }
        opcodes.push_back(op);
        slotIdx++;
    }

    auto identifierIndex = [&](size_t slot, uint32_t raw) -> uint32_t {
        const auto it = dso->identTable.find((uint32_t)slot);
        return it == dso->identTable.end() ? raw : it->second;
    };

    // Convert vector to stack interface
    struct ExprStack {
        std::vector<VMValue> v;
        VMValue def;
        void push(const VMValue& x) { v.push_back(x); }
        VMValue pop() { if (v.empty()) return {}; VMValue x = v.back(); v.pop_back(); return x; }
        VMValue& top() { if (v.empty()) { def = {}; return def; } return v.back(); }
        bool empty() const { return v.empty(); }
        size_t size() const { return v.size(); }
    };
    ExprStack stack;
    // Copy existing stack items
    for (auto& item : frame->exprStack) stack.push(item);
    frame->exprStack.clear();

    size_t execIp = bodyStart;
    uint32_t safety = 0;
    const uint32_t MAX_OPS = 100000;

    while (execIp < opcodes.size() && safety++ < MAX_OPS) {
        uint32_t op = opcodes[execIp];

        switch (op) {
            // === Control flow ===
            case (uint32_t)DSOOpcode::OP_RETURN: {
                VMValue ret;
                if (!stack.empty()) {
                    ret = stack.top();
                }
                impl->callStack.pop();
                return ret;
            }

            case (uint32_t)DSOOpcode::OP_JMP: {
                if (execIp + 1 < opcodes.size())
                    execIp = opcodes[execIp + 1];
                else execIp++;
                continue;
            }

            case (uint32_t)DSOOpcode::OP_JMPIF: {
                if (execIp + 1 < opcodes.size()) {
                    bool cond = false;
                    if (!stack.empty()) { cond = stack.top().toBool(); stack.pop(); }
                    if (cond) execIp = opcodes[execIp + 1];
                    else execIp++;
                } else execIp++;
                continue;
            }

            case (uint32_t)DSOOpcode::OP_JMPIFNOT: {
                if (execIp + 1 < opcodes.size()) {
                    bool cond = false;
                    if (!stack.empty()) { cond = stack.top().toBool(); stack.pop(); }
                    if (!cond) execIp = opcodes[execIp + 1];
                    else execIp++;
                } else execIp++;
                continue;
            }

            case (uint32_t)DSOOpcode::OP_JMPIFF: {
                if (execIp + 1 < opcodes.size()) {
                    double val = 0;
                    if (!stack.empty()) { val = stack.top().toDouble(); stack.pop(); }
                    if (val != 0.0) execIp = opcodes[execIp + 1];
                    else execIp++;
                } else execIp++;
                continue;
            }

            case (uint32_t)DSOOpcode::OP_JMPIFFNOT: {
                if (execIp + 1 < opcodes.size()) {
                    double val = 0;
                    if (!stack.empty()) { val = stack.top().toDouble(); stack.pop(); }
                    if (val == 0.0) execIp = opcodes[execIp + 1];
                    else execIp++;
                } else execIp++;
                continue;
            }

            // === Stack operations ===
            case (uint32_t)DSOOpcode::OP_PUSH: {
                // Pop top of expr stack into current arg frame
                if (!impl->argFrames.empty() && !stack.empty()) {
                    impl->argFrames.back().args.push_back(stack.pop());
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_PUSH_FRAME: {
                // Start a new argument frame
                impl->argFrames.push_back(ArgFrame{});
                break;
            }

            // === Immediate load ===
            case (uint32_t)DSOOpcode::OP_LOADIMMED_UINT: {
                if (execIp + 1 < opcodes.size())
                    stack.push(VMValue((int32_t)opcodes[execIp + 1]));
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADIMMED_FLT: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = opcodes[execIp + 1];
                    // Check function float table first, then global
                    double val = 0;
                    if (idx < dso->functionFloats.size()) val = dso->functionFloats[idx];
                    else if (idx < dso->globalFloats.size()) val = dso->globalFloats[idx];
                    stack.push(VMValue((float)val));
                }
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADIMMED_STR: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = opcodes[execIp + 1];
                    const char* s = "";
                    if (idx < dso->functionStrings.size()) s = &dso->functionStrings[idx];
                    else if (idx < dso->globalStrings.size()) s = &dso->globalStrings[idx];
                    stack.push(VMValue(s));
                }
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADIMMED_IDENT:
            case (uint32_t)DSOOpcode::OP_TAG_TO_STR: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    const char* s = "";
                    if (idx < dso->globalStrings.size()) s = &dso->globalStrings[idx];
                    stack.push(VMValue(s));
                }
                execIp++;
                break;
            }

            // === Variable operations ===
            case (uint32_t)DSOOpcode::OP_SETCURVAR:
            case (uint32_t)DSOOpcode::OP_SETCURVAR_CREATE: {
                // Clear any stale array key from previous access
                frame->curArrayKey.clear();
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    const char* name = "";
                    if (idx < dso->globalStrings.size()) name = &dso->globalStrings[idx];

                    // Handle $ prefix for global, % for local
                    if (name[0] == '$') {
                        frame->curVarName = name + 1;
                    } else if (name[0] == '%') {
                        // Local variable
                        frame->curVarName = std::string("%") + (name + 1);
                    } else {
                        frame->curVarName = name;
                    }
                }
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADVAR_UINT:
            case (uint32_t)DSOOpcode::OP_LOADVAR_FLT:
            case (uint32_t)DSOOpcode::OP_LOADVAR_STR: {
                if (!frame->curVarName.empty()) {
                    std::string varKey = frame->curVarName;
                    // Append array key if set
                    if (!frame->curArrayKey.empty())
                        varKey += "[" + frame->curArrayKey + "]";

                    if (frame->curVarName[0] == '%') {
                        auto it = frame->locals.find(varKey);
                        if (it != frame->locals.end())
                            stack.push(it->second);
                        else
                            stack.push(VMValue(0));
                    } else {
                        auto it = impl->globals.find(varKey);
                        stack.push(it != impl->globals.end() ? it->second : VMValue(0));
                    }
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SAVEVAR_UINT:
            case (uint32_t)DSOOpcode::OP_SAVEVAR_FLT:
            case (uint32_t)DSOOpcode::OP_SAVEVAR_STR: {
                if (!frame->curVarName.empty() && !stack.empty()) {
                    VMValue val = stack.top(); stack.pop();
                    std::string varKey = frame->curVarName;
                    if (!frame->curArrayKey.empty())
                        varKey += "[" + frame->curArrayKey + "]";

                    if (frame->curVarName[0] == '%') {
                        frame->locals[varKey] = val;
                    } else {
                        impl->globals[varKey] = val;
                        Console::instance().setVariable(varKey.c_str(), val.toString().c_str());
                    }
                }
                break;
            }

            // === Arithmetic ===
            case (uint32_t)DSOOpcode::OP_ADD: {
                if (stack.size() >= 2) {
                    VMValue b = stack.top(); stack.pop();
                    VMValue a = stack.top(); stack.pop();
                    // OP_ADD is a float add; OP_ADVANCE_STR concatenates.
                    stack.push(VMValue(a.toDouble() + b.toDouble()));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SUB: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(b - a));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_MUL: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(a * b));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_DIV: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    if (a != 0) stack.push(VMValue(b / a));
                    else stack.push(VMValue(0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_MOD: {
                if (stack.size() >= 2) {
                    int32_t b = stack.top().toInt(); stack.pop();
                    int32_t a = stack.top().toInt(); stack.pop();
                    if (a != 0) stack.push(VMValue(b % a));
                    else stack.push(VMValue(0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_NEG: {
                if (!stack.empty()) {
                    stack.top() = VMValue(-stack.top().toDouble());
                }
                break;
            }

            // === Comparison ===
            case (uint32_t)DSOOpcode::OP_CMPEQ: {
                if (stack.size() >= 2) {
                    VMValue b = stack.top(); stack.pop();
                    VMValue a = stack.top(); stack.pop();
                    bool eq = (a.toDouble() == b.toDouble()) ||
                              (a.type == VMValue::String && b.type == VMValue::String && a.str == b.str);
                    stack.push(VMValue(eq ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPNE: {
                if (stack.size() >= 2) {
                    VMValue b = stack.top(); stack.pop();
                    VMValue a = stack.top(); stack.pop();
                    bool ne = (a.toDouble() != b.toDouble());
                    if (a.type == VMValue::String && b.type == VMValue::String) ne = (a.str != b.str);
                    stack.push(VMValue(ne ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPGR: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(b > a ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPGE: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(b >= a ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPLT: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(b < a ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPLE: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(b <= a ? 1 : 0));
                }
                break;
            }

            // === Logical ===
            case (uint32_t)DSOOpcode::OP_NOT: {
                if (!stack.empty()) {
                    stack.top() = VMValue(!stack.top().toBool() ? 1 : 0);
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_NOTF: {
                if (!stack.empty()) {
                    stack.top() = VMValue(!stack.top().toBool() ? 1.0 : 0.0);
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_AND: {
                if (stack.size() >= 2) {
                    bool b = stack.top().toBool(); stack.pop();
                    bool a = stack.top().toBool(); stack.pop();
                    stack.push(VMValue((a && b) ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_OR: {
                if (stack.size() >= 2) {
                    bool b = stack.top().toBool(); stack.pop();
                    bool a = stack.top().toBool(); stack.pop();
                    stack.push(VMValue((a || b) ? 1 : 0));
                }
                break;
            }

            // === Call ===
            case (uint32_t)DSOOpcode::OP_CALLFUNC:
            case (uint32_t)DSOOpcode::OP_CALLFUNC_RESOLVE: {
                if (execIp + 3 < opcodes.size()) {
                    uint32_t nameIdx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    uint32_t nsIdx = identifierIndex(execIp + 2, opcodes[execIp + 2]);
                    (void)opcodes[execIp + 3];

                    const char* name = "";
                    if (nameIdx < dso->globalStrings.size()) name = &dso->globalStrings[nameIdx];

                    const char* ns = "";
                    if (nsIdx < dso->globalStrings.size()) ns = &dso->globalStrings[nsIdx];

                    // Collect arguments from current arg frame
                    std::vector<VMValue> callArgs;
                    if (!impl->argFrames.empty()) {
                        callArgs = std::move(impl->argFrames.back().args);
                        impl->argFrames.pop_back();
                    }
                    // Build full function name: ns::name
                    std::string fullName;
                    if (ns && ns[0]) fullName = std::string(ns) + "::" + name;
                    else fullName = name;

                    // Check native first (try namespaced, then bare name)
                    // Try native (namespaced first, then bare name fallback)
                    auto findNative = [&](const std::string& fn) -> bool {
                        auto low = fn;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        auto nit = impl->natives.find(low);
                        if (nit != impl->natives.end()) { stack.push(nit->second(callArgs)); return true; }
                        return false;
                    };
                    auto findDSO = [&](const std::string& fn, bool methodCall) -> bool {
                        for (auto* ds : impl->loaded) {
                            auto fit = ds->funcMap.find(fn);
                            if (fit != ds->funcMap.end()) {
                                if (!fit->second->package.empty() &&
                                    (!ScriptEngine::instance().ts() ||
                                     !ScriptEngine::instance().ts()->isActivePackage(fit->second->package)))
                                    continue;
                                stack.push(execute(ds, fit->second->startIp, callArgs, methodCall));
                                return true;
                            }
                        }
                        return false;
                    };
                    // A script definition replaces the engine function of
                    // the same name (Namespace::addFunction), so script
                    // functions resolve before natives.
                    const bool methodCall = op == (uint32_t)DSOOpcode::OP_CALLFUNC;
                    if (!findDSO(fullName, methodCall) && !findNative(fullName)) {
                        // bare name fallback (e.g. "someField" from "SomeCtrl::someField")
                        if (!findDSO(name, methodCall) && !findNative(name)) {
                            Console::instance().printf(LogLevel::Debug, "VM: calling unknown func %s", fullName.c_str());
                            stack.push(VMValue(0));
                        }
                    }

                    execIp += 3;
                }
                break;
            }

            // === Type conversion ===
            case (uint32_t)DSOOpcode::OP_STR_TO_UINT: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toInt()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_STR_TO_FLT: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toFloat()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_STR_TO_NONE: {
                // String to void - drop
                if (!stack.empty()) stack.pop();
                break;
            }
            case (uint32_t)DSOOpcode::OP_FLT_TO_UINT: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toInt()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_FLT_TO_STR: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toString()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_FLT_TO_NONE: {
                if (!stack.empty()) stack.pop();
                break;
            }
            case (uint32_t)DSOOpcode::OP_UINT_TO_FLT: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toFloat()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_UINT_TO_STR: {
                if (!stack.empty()) { stack.top() = VMValue(stack.top().toString()); }
                break;
            }
            case (uint32_t)DSOOpcode::OP_UINT_TO_NONE: {
                if (!stack.empty()) stack.pop();
                break;
            }

            // === Bitwise ===
            case (uint32_t)DSOOpcode::OP_BITAND: {
                if (stack.size() >= 2) {
                    int32_t b = stack.top().toInt(); stack.pop();
                    int32_t a = stack.top().toInt(); stack.pop();
                    stack.push(VMValue(a & b));
                }
                break;
            }
            case (uint32_t)DSOOpcode::OP_BITOR: {
                if (stack.size() >= 2) {
                    int32_t b = stack.top().toInt(); stack.pop();
                    int32_t a = stack.top().toInt(); stack.pop();
                    stack.push(VMValue(a | b));
                }
                break;
            }
            case (uint32_t)DSOOpcode::OP_XOR: {
                if (stack.size() >= 2) {
                    int32_t b = stack.top().toInt(); stack.pop();
                    int32_t a = stack.top().toInt(); stack.pop();
                    stack.push(VMValue(a ^ b));
                }
                break;
            }

            // === String ===
            case (uint32_t)DSOOpcode::OP_COMPARE_STR: {
                if (stack.size() >= 2) {
                    std::string b = stack.top().toString(); stack.pop();
                    std::string a = stack.top().toString(); stack.pop();
                    stack.push(VMValue(a == b ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_ADVANCE_STR: {
                // Append the current operand to the Torque string builder.
                // Nested concatenations use ADVANCE/REWIND pairs, so this must
                // preserve any text already accumulated in the frame.
                if (!stack.empty()) {
                    frame->strBuilder += stack.top().toString();
                    stack.pop();
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_REWIND_STR:
            case (uint32_t)DSOOpcode::OP_TERMINATE_REWIND_STR: {
                if (!stack.empty()) {
                    frame->strBuilder += stack.top().toString();
                    stack.pop();
                }
                stack.push(VMValue(frame->strBuilder));
                frame->strBuilder.clear();
                break;
            }

            // === Object operations ===
            case (uint32_t)DSOOpcode::OP_CREATE_OBJECT: {
                // Create a new script object
                if (execIp + 3 < opcodes.size()) {
                    uint32_t parentIdx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    (void)opcodes[execIp + 3];

                    const char* parent = "";
                    if (parentIdx < dso->globalStrings.size()) parent = &dso->globalStrings[parentIdx];

                    auto* obj = new ScriptObject;
                    if (!impl->argFrames.empty()) {
                        auto objectArgs = std::move(impl->argFrames.back().args);
                        impl->argFrames.pop_back();
                        if (!objectArgs.empty()) obj->className = objectArgs[0].toString();
                        if (objectArgs.size() > 1) obj->name = objectArgs[1].toString();
                    }
                    if (obj->className.empty()) obj->className = parent;
                    frame->curObject = obj;
                    execIp += 3;
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_ADD_OBJECT: {
                if (frame->curObject) {
                    ScriptEngine::instance().addObject(frame->curObject);
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_END_OBJECT: {
                frame->curObject = nullptr;
                break;
            }

            case (uint32_t)DSOOpcode::OP_SETCUROBJECT:
            case (uint32_t)DSOOpcode::OP_SETCUROBJECT_NEW: {
                // Pop object name from stack
                if (!stack.empty()) {
                    std::string name = stack.top().toString();
                    stack.pop();
                    frame->curObject = ScriptEngine::instance().findObject(name.c_str());
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SETCURFIELD: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    const char* field = "";
                    if (idx < dso->globalStrings.size()) field = &dso->globalStrings[idx];
                    frame->curFieldName = field;
                }
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_SAVEFIELD_UINT:
            case (uint32_t)DSOOpcode::OP_SAVEFIELD_FLT:
            case (uint32_t)DSOOpcode::OP_SAVEFIELD_STR: {
                if (frame->curObject && !stack.empty()) {
                    ScriptEngine::instance().setObjectField(frame->curObject, frame->curFieldName, stack.top());
                    stack.pop();
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADFIELD_UINT:
            case (uint32_t)DSOOpcode::OP_LOADFIELD_FLT:
            case (uint32_t)DSOOpcode::OP_LOADFIELD_STR: {
                if (frame->curObject) {
                    if (const auto* value = findObjectField(frame->curObject, frame->curFieldName))
                        stack.push(*value);
                    else stack.push(VMValue(0));
                }
                break;
            }

            // ── Control flow (no-pop variants) ──
            case (uint32_t)DSOOpcode::OP_JMPIF_NP: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t target = opcodes[execIp + 1];
                    if (stack.top().toBool())
                        execIp = target;
                    else
                        execIp++;
                } else execIp++;
                continue;
            }

            case (uint32_t)DSOOpcode::OP_JMPIFNOT_NP: {
                if (execIp + 1 < opcodes.size()) {
                    uint32_t target = opcodes[execIp + 1];
                    if (!stack.top().toBool())
                        execIp = target;
                    else
                        execIp++;
                } else execIp++;
                continue;
            }

            // ── Bitwise shift and complement ──
            case (uint32_t)DSOOpcode::OP_SHR: {
                if (stack.size() >= 2) {
                    uint32_t b = (uint32_t)stack.top().toInt(); stack.pop();
                    uint32_t a = (uint32_t)stack.top().toInt(); stack.pop();
                    stack.push(VMValue((int32_t)(a >> b)));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SHL: {
                if (stack.size() >= 2) {
                    uint32_t b = (uint32_t)stack.top().toInt(); stack.pop();
                    uint32_t a = (uint32_t)stack.top().toInt(); stack.pop();
                    stack.push(VMValue((int32_t)(a << b)));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_ONESCOMPLEMENT: {
                if (!stack.empty())
                    stack.top() = VMValue(~stack.top().toInt());
                break;
            }

            // ── Array variable access ──
            case (uint32_t)DSOOpcode::OP_SETCURVAR_ARRAY:
            case (uint32_t)DSOOpcode::OP_SETCURVAR_ARRAY_CREATE: {
                // Pop array index from stack
                if (!stack.empty()) {
                    frame->curArrayKey = stack.top().toString();
                    stack.pop();
                }
                // Followed by the variable name (same as SETCURVAR)
                if (execIp + 1 < opcodes.size()) {
                    uint32_t idx = identifierIndex(execIp + 1, opcodes[execIp + 1]);
                    const char* name = "";
                    if (idx < dso->globalStrings.size()) name = &dso->globalStrings[idx];
                    if (name[0] == '$')
                        frame->curVarName = name + 1;
                    else if (name[0] == '%')
                        frame->curVarName = std::string("%") + (name + 1);
                    else
                        frame->curVarName = name;
                }
                execIp++;
                break;
            }

            // ── Internal object ──
            case (uint32_t)DSOOpcode::OP_SETCUROBJECT_INTERNAL: {
                // Treat like SETCUROBJECT but for internal fields
                if (!stack.empty()) {
                    std::string name = stack.top().toString();
                    stack.pop();
                    frame->curObject = ScriptEngine::instance().findObject(name.c_str());
                }
                break;
            }

            // ── Array field access ──
            case (uint32_t)DSOOpcode::OP_SETCURFIELD_ARRAY: {
                if (!stack.empty()) {
                    std::string idx = stack.top().toString();
                    stack.pop();
                    frame->curFieldName = frame->curFieldName + "[" + idx + "]";
                }
                break;
            }

            // ── String builder append ops ──
            case (uint32_t)DSOOpcode::OP_ADVANCE_STR_APPENDCHAR: {
                if (!stack.empty()) {
                    int32_t ch = stack.top().toInt();
                    stack.pop();
                    frame->strBuilder += (char)ch;
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_ADVANCE_STR_COMMA: {
                frame->strBuilder += ",";
                break;
            }

            case (uint32_t)DSOOpcode::OP_ADVANCE_STR_NUL: {
                // Null terminator - no-op for C++ strings
                break;
            }

            // ── Unit conversion ──
            case (uint32_t)DSOOpcode::OP_UNIT_CONVERSION: {
                // Stub: consumes args from stack, pushes result
                // Units in T2: feet/inches etc. - treat as no-op identity
                if (execIp + 2 < opcodes.size()) {
                    uint32_t type = opcodes[execIp + 1];
                    (void)type;
                    uint32_t numArgs = opcodes[execIp + 2];
                    if (numArgs > 0 && !stack.empty()) {
                        VMValue val = stack.top(); stack.pop();
                        stack.push(val);
                    }
                    execIp += 2;
                }
                break;
            }

            // ── Breakpoint ──
            case (uint32_t)DSOOpcode::OP_BREAK:
                Console::instance().printf(LogLevel::Debug, "VM: breakpoint at IP %zu", execIp);
                break;

            default:
                if (op != (uint32_t)DSOOpcode::OP_FUNC_DECL &&
                    op != (uint32_t)DSOOpcode::OP_UNUSED1 &&
                    op != (uint32_t)DSOOpcode::OP_UNUSED2 &&
                    op != (uint32_t)DSOOpcode::OP_UNUSED3) {
                    Console::instance().printf(LogLevel::Warn, "VM: unhandled opcode 0x%02X at IP %zu/%zu", op, execIp, opcodes.size());
                }
                break;
        }
        execIp++;
    }

    if (safety >= MAX_OPS) {
        Console::instance().printf(LogLevel::Warn, "VM: safety limit reached at IP %zu", execIp);
    }

    impl->callStack.pop();
    return {};
}

// === ScriptEngine ===
ScriptEngine* ScriptEngine::instance_ = nullptr;

ScriptEngine::ScriptEngine() : con(&Console::instance()) {
    instance_ = this;
}

ScriptEngine::~ScriptEngine() {
    if (vmInstance || tsInstance || !objects.empty()) shutdown();
    instance_ = nullptr;
}

// GameBase::scriptOnAdd/OnNewDataBlock/OnRemove: server GameBase objects
// report to their datablock's namespace (%data.onAdd(%obj)); which leaf
// classes call which comes from the engine's game/*.cc.
static bool callsScriptOnAdd(const std::string& cls) {
    for (const char* base : {"StaticShape", "Player", "Item", "FlyingVehicle", "HoverVehicle",
                             "WheeledVehicle", "ForceFieldBare"})
        if (EngineClasses::isA(cls, base)) return true;
    return false;
}
static bool callsScriptOnNewDataBlock(const std::string& cls) {
    if (callsScriptOnAdd(cls)) return true;
    for (const char* base : {"Projectile", "Debris", "Explosion", "FireballAtmosphere", "Lightning",
                             "MissionMarker", "ParticleEmissionDummy", "ParticleEmitter", "Precipitation",
                             "Shockwave", "Splash", "StationFXPersonal", "StationFXVehicle", "Trigger",
                             "Turret"})
        if (EngineClasses::isA(cls, base)) return true;
    return false;
}

std::string ScriptEngine::objectDataBlock(ScriptObject* object) {
    if (!object) return {};
    for (const auto& [field, value] : object->fields)
        if (strcasecmp(field.c_str(), "dataBlock") == 0) {
            // TypeGameBaseDataPtr: the field names a datablock (an object
            // sharing its name, such as an Item "Nexus" of ItemData Nexus,
            // is not one).
            const std::string ref = value.toString();
            int id = 0;
            ScriptObject* block = parseObjectId(ref, id) ? findObject(ref.c_str()) : findDataBlock(ref);
            return block ? std::to_string(objectId(block)) : std::string();
        }
    return {};
}

void ScriptEngine::objectAdded(ScriptObject* object) {
    if (!object || object->internals["__added"].toBool()) return;
    object->internals["__added"] = VMValue(1);
    if (EngineClasses::isA(object->className, "SimDataBlock")) registerDataBlock(object);
    EngineObjects::attach(object);
    if (tsInstance) {
        // %this is the object's id, named or not; onAdd resolves through the
        // object's namespace chain. GUI controls get their named onAdd from
        // the GUI renderer once they sit in their parent (callOnAddOnce), so
        // only their class callback runs here.
        const std::string& cls = object->className;
        if (EngineClasses::isA(cls, "GameBase")) {
            const std::string block = objectDataBlock(object);
            const VMValue self(objectId(object));
            if (!block.empty() && callsScriptOnNewDataBlock(cls))
                tsInstance->callObjectMethod(block, "onNewDataBlock", {self});
            if (!block.empty() && callsScriptOnAdd(cls))
                tsInstance->callObjectMethod(block, "onAdd", {self});
            if (auto* game = dynamic_cast<GameBase*>(object->engine.get())) game->onAdded();
            return;
        }
        const bool guiControl = cls.rfind("Gui", 0) == 0 || cls.rfind("Shell", 0) == 0 ||
                                cls.rfind("Hud", 0) == 0 || cls == "GameTSCtrl" ||
                                cls == "VirtualScrollCtrl" || cls == "VirtualScrollContentCtrl";
        const std::vector<std::string> spaces = guiControl ? std::vector<std::string>{cls}
                                                           : objectNamespaces(object);
        for (const std::string& space : spaces) {
            const std::string callback = space + "::onAdd";
            if (!tsInstance->hasFunction(callback)) continue;
            tsInstance->callFunction(callback, {VMValue(objectId(object))});
            break;
        }
    }
}

bool ScriptEngine::setObjectField(ScriptObject* object, const std::string& field,
                                  const VMValue& value) {
    if (!object || field.empty()) return false;
    auto it = object->fields.find(field);
    const VMValue old = it == object->fields.end() ? VMValue() : it->second;
    if (it == object->fields.end()) object->fields[field] = value;
    else it->second = value;
    if (!sameObjectValue(old, value) && tsInstance) {
        const std::string callback = object->className + "::onFieldModified";
        if (tsInstance->hasFunction(callback))
            tsInstance->callFunction(callback, {VMValue(object->name), VMValue(field), old, value});
    }
    return true;
}

bool ScriptEngine::addDeleteNotify(ScriptObject* listener, ScriptObject* target) {
    if (!listener || !target || listener == target) return false;
    const std::string key = objectKey(listener);
    if (std::find(target->deleteNotifyListeners.begin(), target->deleteNotifyListeners.end(), key) ==
        target->deleteNotifyListeners.end())
        target->deleteNotifyListeners.push_back(key);
    return true;
}

bool ScriptEngine::clearDeleteNotify(ScriptObject* listener, ScriptObject* target) {
    if (!listener || !target) return false;
    auto& listeners = target->deleteNotifyListeners;
    const auto oldSize = listeners.size();
    listeners.erase(std::remove(listeners.begin(), listeners.end(), objectKey(listener)), listeners.end());
    return oldSize != listeners.size();
}

// Drop `childKey` from a SimGroup's member list; false if it was not a member.
static bool removeSimGroupChild(ScriptObject* group, const std::string& childKey) {
    const int count = group->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i) {
        if (group->internals["__child" + std::to_string(i)].toString() != childKey) continue;
        for (int j = i + 1; j < count; ++j)
            group->internals["__child" + std::to_string(j - 1)] =
                group->internals["__child" + std::to_string(j)];
        group->internals.erase("__child" + std::to_string(count - 1));
        group->internals["__childCount"] = VMValue(count - 1);
        return true;
    }
    return false;
}

// SimSet::addObject / SimGroup::addObject. A set lists members (__childN)
// and each member records the sets it is in (__sets); an object is in one
// SimGroup at a time (__parent), so adding it to a group moves it.
bool ScriptEngine::isSimSet(ScriptObject* object) {
    return object && EngineClasses::isA(object->className, "SimSet");
}
bool ScriptEngine::isSimGroup(ScriptObject* object) {
    return object && EngineClasses::isA(object->className, "SimGroup");
}
static bool setHasMember(ScriptObject* set, const std::string& key) {
    const int count = set->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i)
        if (set->internals["__child" + std::to_string(i)].toString() == key) return true;
    return false;
}
static std::vector<std::string> memberSets(ScriptObject* object) {
    std::vector<std::string> sets;
    auto it = object->internals.find("__sets");
    if (it == object->internals.end()) return sets;
    std::string list = it->second.toString(), item;
    for (char c : list) {
        if (c == '\t') { if (!item.empty()) sets.push_back(item); item.clear(); }
        else item += c;
    }
    if (!item.empty()) sets.push_back(item);
    return sets;
}
static void storeMemberSets(ScriptObject* object, const std::vector<std::string>& sets) {
    std::string list;
    for (const auto& set : sets) list += (list.empty() ? "" : "\t") + set;
    if (list.empty()) object->internals.erase("__sets");
    else object->internals["__sets"] = VMValue(list);
}

bool ScriptEngine::addToSet(ScriptObject* set, ScriptObject* object) {
    if (!isSimSet(set) || !object || set == object) return false;
    const std::string key = objectKey(object);
    const std::string setKey = objectKey(set);
    if (isSimGroup(set)) {
        if (auto it = object->internals.find("__parent"); it != object->internals.end()) {
            if (it->second.toString() == setKey) return true;
            if (auto* old = findObject(it->second.toString().c_str())) {
                removeSimGroupChild(old, key);
                PathManager::membershipChanged(old, object);
            }
        }
        object->internals["__parent"] = VMValue(setKey);
        // A GuiControl's parent is its group: the declared link moves too.
        if (auto link = object->internals.find("parent"); link != object->internals.end())
            link->second = VMValue(setKey);
    } else {
        if (setHasMember(set, key)) return true;
        auto sets = memberSets(object);
        sets.push_back(setKey);
        storeMemberSets(object, sets);
    }
    const int count = set->internals["__childCount"].toInt();
    set->internals["__child" + std::to_string(count)] = VMValue(key);
    set->internals["__childCount"] = VMValue(count + 1);
    ++objectTreeRevision;
    PathManager::membershipChanged(set, object);
    return true;
}

bool ScriptEngine::removeFromSet(ScriptObject* set, ScriptObject* object) {
    if (!set || !object) return false;
    const std::string key = objectKey(object);
    const std::string setKey = objectKey(set);
    if (!removeSimGroupChild(set, key)) return false;
    if (auto it = object->internals.find("__parent"); it != object->internals.end() && it->second.toString() == setKey)
        object->internals.erase(it);
    if (auto link = object->internals.find("parent"); link != object->internals.end() && link->second.toString() == setKey)
        object->internals.erase(link);
    auto sets = memberSets(object);
    sets.erase(std::remove(sets.begin(), sets.end(), setKey), sets.end());
    storeMemberSets(object, sets);
    ++objectTreeRevision;
    PathManager::membershipChanged(set, object);
    return true;
}

bool ScriptEngine::deleteScriptObject(const std::string& name) {
    ScriptObject* object = findObject(name.c_str());
    if (!object) return false;
    const std::string objectName = objectKey(object);
    const std::string objectHandle = std::to_string(objectId(object));
    auto it = objects.find(objectName);
    if (it == objects.end() || it->second != object) return false;
    if (object->internals["__deleting"].toBool()) return false;
    object->internals["__deleting"] = VMValue(1);

    std::vector<std::string> children;
    for (const auto& [childName, child] : objects) {
        if (!child) continue;
        const auto parent = child->internals.find("parent");
        if (parent != child->internals.end() && findObject(parent->second.toString().c_str()) == object)
            children.push_back(childName);
    }
    // SimGroup::onRemove deletes the group's members too; a SimSet only
    // lets go of them.
    const int memberCount = object->internals["__childCount"].toInt();
    if (isSimGroup(object) || !isSimSet(object))
        for (int i = 0; i < memberCount; ++i)
            children.push_back(object->internals["__child" + std::to_string(i)].toString());
    else
        for (int i = 0; i < memberCount; ++i)
            if (auto* member = findObject(object->internals["__child" + std::to_string(i)].toString().c_str())) {
                auto sets = memberSets(member);
                sets.erase(std::remove(sets.begin(), sets.end(), objectName), sets.end());
                storeMemberSets(member, sets);
            }
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()), children.end());
    for (const auto& child : children) deleteScriptObject(child);

    if (tsInstance) {
        // Events may have been scheduled on the name or on the id.
        tsInstance->cancelEventsForObject(objectName);
        if (objectHandle != objectName) tsInstance->cancelEventsForObject(objectHandle);
        if (EngineClasses::isA(object->className, "GameBase")) {
            const std::string block = objectDataBlock(object);
            if (!block.empty() && callsScriptOnAdd(object->className))
                tsInstance->callObjectMethod(block, "onRemove", {VMValue(object->id)});
        } else {
            for (const std::string& space : objectNamespaces(object)) {
                const std::string callback = space + "::onRemove";
                if (!tsInstance->hasFunction(callback)) continue;
                tsInstance->callFunction(callback, {VMValue(object->id)});
                break;
            }
        }
    }
    if (object->engine) object->engine->onRemove();
    const auto listeners = object->deleteNotifyListeners;
    for (const auto& listenerName : listeners) {
        ScriptObject* listener = findObject(listenerName.c_str());
        if (listener && listener->engine) {
            auto state = listener->engine;
            state->onDeleteNotify(object);
            // The native notification may delete its listener through script.
            listener = findObject(listenerName.c_str());
        }
        if (listener && tsInstance) {
            const std::string callback = listener->className + "::onDeleteNotify";
            if (tsInstance->hasFunction(callback))
                tsInstance->callFunction(callback, {VMValue(listenerName), VMValue(object->id)});
        }
    }
    for (auto& [remainingName, remaining] : objects) {
        if (!remaining || remaining == object) continue;
        auto& notifications = remaining->deleteNotifyListeners;
        notifications.erase(std::remove(notifications.begin(), notifications.end(), objectName),
                            notifications.end());
    }
    // SimObject::unregisterObject leaves its group and every set.
    if (auto parent = object->internals.find("__parent"); parent != object->internals.end())
        if (auto* group = findObject(parent->second.toString().c_str())) {
            removeSimGroupChild(group, objectName);
            PathManager::membershipChanged(group, object);
        }
    for (const auto& setKey : memberSets(object))
        if (auto* set = findObject(setKey.c_str())) removeSimGroupChild(set, objectName);
    removeObject(object);
    delete object;
    return true;
}

ScriptEngine& ScriptEngine::instance() {
    return *instance_;
}

bool ScriptEngine::exists() { return instance_ != nullptr; }

static bool s_clientPrefsExportAllowed = false;
void allowClientPrefsExport(bool allowed) { s_clientPrefsExportAllowed = allowed; }
bool clientPrefsExportAllowed() { return s_clientPrefsExportAllowed; }

bool ScriptEngine::init() {
    if (vmInstance || tsInstance || !objects.empty()) shutdown();
    vmInstance = new VirtualMachine(this);
    tsInstance = new TorqueScript;

    // Register native functions in TorqueScript interpreter
    tsInstance->registerNative("echo", [](const auto& args) -> VMValue {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Info, "%s", msg.c_str());
        return VMValue();
    });

    tsInstance->registerNative("warn", [](const auto& args) -> VMValue {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        auto* ts = Engine::instance().script().ts();
        const std::string function = ts ? ts->currentFunction() : std::string();
        const std::string file = ts && !ts->dbgFile().empty() ? ts->dbgFile() : "<runtime>";
        const std::string where = "TS:" + file +
            (function.empty() ? ":" + std::to_string(ts ? ts->dbgLine() : 0)
                              : " [" + function + "]");
        Console::instance().printf(LogLevel::Warn, "%s: %s", where.c_str(), msg.c_str());
        return VMValue();
    });

    tsInstance->registerNative("error", [](const auto& args) -> VMValue {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        auto* ts = Engine::instance().script().ts();
        const std::string function = ts ? ts->currentFunction() : std::string();
        const std::string file = ts && !ts->dbgFile().empty() ? ts->dbgFile() : "<runtime>";
        const std::string where = "TS:" + file +
            (function.empty() ? ":" + std::to_string(ts ? ts->dbgLine() : 0)
                              : " [" + function + "]");
        Console::instance().printf(LogLevel::Error, "%s: %s", where.c_str(), msg.c_str());
        return VMValue();
    });

    tsInstance->registerNative("setFogDistance", setScriptFogDistance);
    tsInstance->registerNative("setFogDensity", setScriptFogDensity);
    tsInstance->registerNative("setFogColor", setScriptFogColor);
    tsInstance->registerNative("setFogTransition", setScriptFogTransition);
    tsInstance->registerNative("setSkyColor", setScriptSkyColor);
    tsInstance->registerNative("setSkyMaterial", setScriptSkyMaterialList);
    tsInstance->registerNative("setSkyMaterialList", setScriptSkyMaterialList);
    tsInstance->registerNative("setSunDirection", setScriptSunDirection);
    tsInstance->registerNative("setSunColor", setScriptSunColor);
    tsInstance->registerNative("setSunAmbient", setScriptSunAmbient);
    tsInstance->registerNative("setPrecipitation", setScriptPrecipitation);
    tsInstance->registerNative("setPrecipitationEnabled", setScriptPrecipitationEnabled);
    tsInstance->registerNative("setPrecipitationType", setScriptPrecipitationType);
    tsInstance->registerNative("setPrecipitationWind", setScriptPrecipitationWind);
    tsInstance->registerNative("setPrecipitationBox", setScriptPrecipitationBox);
    tsInstance->registerNative("setLightning", setScriptLightning);
    tsInstance->registerNative("strikeLightning", strikeScriptLightning);
    tsInstance->registerNative("Lightning::strike", strikeScriptLightning);
    tsInstance->registerNative("MissionCleanup", [](const auto&) -> VMValue {
        clearScriptMissionState();
        Engine::instance().game().world().cleanupMission();
        return VMValue(1);
    });
    auto setMissionObjectEnabled = [](const auto& args, bool enabled) -> VMValue {
        if (args.empty() || args[0].toString().empty()) return VMValue(0);
        return VMValue(Engine::instance().game().world().setMissionObjectEnabled(
            args[0].toString(), enabled) ? 1 : 0);
    };
    auto setMissionObjectHidden = [](const auto& args) -> VMValue {
        if (args.size() < 2 || args[0].toString().empty()) return VMValue(0);
        return VMValue(Engine::instance().game().world().setMissionObjectHidden(
            args[0].toString(), args[1].toBool()) ? 1 : 0);
    };
    auto setMissionObjectTransform = [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        return VMValue(Engine::instance().game().world().setMissionObjectTransform(
            args[0].toString(), args[1].toString()) ? 1 : 0);
    };
    auto mountMissionObjectImage = [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        return VMValue(Engine::instance().game().world().mountMissionObjectImage(
            args[0].toString(), args[1].toString(), args[2].toInt()) ? 1 : 0);
    };
    auto unmountMissionObjectImage = [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        return VMValue(Engine::instance().game().world().unmountMissionObjectImage(
            args[0].toString(), args[1].toInt()) ? 1 : 0);
    };
    tsInstance->registerNative("setHidden", setMissionObjectHidden);
    tsInstance->registerNative("setTransform", setMissionObjectTransform);
    tsInstance->registerNative("mountImage", mountMissionObjectImage);
    tsInstance->registerNative("unmountImage", unmountMissionObjectImage);
    tsInstance->registerNative("activate", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, true);
    });
    tsInstance->registerNative("deactivate", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, false);
    });
    const std::array<const char*, 6> lifecycleClasses = {
        "StaticShape", "TSStatic", "Turret", "Item", "Player", "Vehicle"};
    for (const char* className : lifecycleClasses) {
        const std::string prefix = std::string(className) + "::";
        tsInstance->registerNative(prefix + "setHidden", setMissionObjectHidden);
        tsInstance->registerNative(prefix + "setTransform", setMissionObjectTransform);
        tsInstance->registerNative(prefix + "mountImage", mountMissionObjectImage);
        tsInstance->registerNative(prefix + "unmountImage", unmountMissionObjectImage);
        tsInstance->registerNative(prefix + "activate", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, true);
        });
        tsInstance->registerNative(prefix + "deactivate", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, false);
        });
        tsInstance->registerNative(prefix + "enable", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, true);
        });
        tsInstance->registerNative(prefix + "disable", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, false);
        });
    }
    tsInstance->registerNative("setMissionObjectEnabled", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
    });
    tsInstance->registerNative("enable", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, true);
    });
    tsInstance->registerNative("disable", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, false);
    });
    tsInstance->registerNative("PhysicalZone::setActive", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
        });
    tsInstance->registerNative("PhysicalZone::setEnabled", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
        });
    tsInstance->registerNative("ForceFieldBare::setEnabled", [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
        });
    vmInstance->registerNativeFunction("PhysicalZone::setActive", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
    });
    vmInstance->registerNativeFunction("PhysicalZone::setEnabled", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
    });
    vmInstance->registerNativeFunction("ForceFieldBare::setEnabled", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, args.size() > 1 && args[1].toBool());
    });
    vmInstance->registerNativeFunction("setHidden", setMissionObjectHidden);
    vmInstance->registerNativeFunction("setTransform", setMissionObjectTransform);
    vmInstance->registerNativeFunction("mountImage", mountMissionObjectImage);
    vmInstance->registerNativeFunction("unmountImage", unmountMissionObjectImage);
    vmInstance->registerNativeFunction("activate", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, true);
    });
    vmInstance->registerNativeFunction("deactivate", [setMissionObjectEnabled](const auto& args) {
        return setMissionObjectEnabled(args, false);
    });
    for (const char* className : lifecycleClasses) {
        const std::string prefix = std::string(className) + "::";
        vmInstance->registerNativeFunction((prefix + "setHidden").c_str(), setMissionObjectHidden);
        vmInstance->registerNativeFunction((prefix + "setTransform").c_str(), setMissionObjectTransform);
        vmInstance->registerNativeFunction((prefix + "mountImage").c_str(), mountMissionObjectImage);
        vmInstance->registerNativeFunction((prefix + "unmountImage").c_str(), unmountMissionObjectImage);
        vmInstance->registerNativeFunction((prefix + "activate").c_str(), [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, true);
        });
        vmInstance->registerNativeFunction((prefix + "deactivate").c_str(), [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, false);
        });
        vmInstance->registerNativeFunction((prefix + "enable").c_str(), [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, true);
        });
        vmInstance->registerNativeFunction((prefix + "disable").c_str(), [setMissionObjectEnabled](const auto& args) {
            return setMissionObjectEnabled(args, false);
        });
    }
    tsInstance->registerNative("setWaterLevel", setScriptWaterLevel);
    tsInstance->registerNative("setWaterType", setScriptWaterType);
    tsInstance->registerNative("setLiquidType", setScriptWaterType);
    tsInstance->registerNative("setWaterOpacity", setScriptWaterOpacity);
    tsInstance->registerNative("setWaterColor", setScriptWaterColor);
    tsInstance->registerNative("WaterBlock::setWaterLevel", setScriptWaterLevel);
    tsInstance->registerNative("WaterBlock::setWaterType", setScriptWaterType);
    tsInstance->registerNative("WaterBlock::setLiquidType", setScriptWaterType);
    tsInstance->registerNative("WaterBlock::setWaterOpacity", setScriptWaterOpacity);
    tsInstance->registerNative("WaterBlock::setOpacity", setScriptWaterOpacity);
    tsInstance->registerNative("WaterBlock::setWaterColor", setScriptWaterColor);
    tsInstance->registerNative("WaterBlock::setColor", setScriptWaterColor);

    tsInstance->registerNative("expandFilename", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        if (path.empty()) return VMValue("");
        // Remove leading "./" if present
        if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
            path = path.substr(2);
        char* resolved = realpath(path.c_str(), nullptr);
        if (resolved) {
            std::string result(resolved);
            free(resolved);
            return VMValue(result);
        }
        // Fallback: prepend working directory
        char* cwd = getcwd(nullptr, 0);
        if (cwd) {
            std::string result = std::string(cwd) + "/" + path;
            free(cwd);
            return VMValue(result);
        }
        return VMValue(path);
    });
    tsInstance->registerNative("fileName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string path = args[0].toString();
        const size_t slash = path.find_last_of("/\\");
        return VMValue(slash == std::string::npos ? path : path.substr(slash + 1));
    });
    tsInstance->registerNative("filePath", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string path = args[0].toString();
        const size_t slash = path.find_last_of("/\\");
        return VMValue(slash == std::string::npos ? "" : path.substr(0, slash + 1));
    });
    tsInstance->registerNative("fileBase", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string name = args[0].toString();
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name.erase(0, slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) name.erase(dot);
        return VMValue(name);
    });
    tsInstance->registerNative("strToInt", [](const auto& args) -> VMValue {
        return VMValue(args.empty() ? 0 : args[0].toInt());
    });
    tsInstance->registerNative("strToFloat", [](const auto& args) -> VMValue {
        return VMValue(args.empty() ? 0.0f : args[0].toFloat());
    });

    // Register str functions
    tsInstance->registerNative("strLen", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue((int32_t)args[0].toString().size());
    });

    tsInstance->registerNative("strCmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        const int result = strcmp(args[0].toString().c_str(), args[1].toString().c_str());
        return VMValue(result < 0 ? -1 : result > 0 ? 1 : 0);
    });

    tsInstance->registerNative("strStr", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        auto pos = args[0].toString().find(args[1].toString());
        return VMValue(pos != std::string::npos ? (int32_t)pos : -1);
    });

    auto changeCase = [](const std::vector<VMValue>& args, bool upper) -> VMValue {
        std::string value = args.empty() ? "" : args[0].toString();
        for (char& c : value) {
            const unsigned char byte = (unsigned char)c;
            c = (char)(upper ? std::toupper(byte) : std::tolower(byte));
        }
        return VMValue(value);
    };
    tsInstance->registerNative("strLwr", [changeCase](const auto& args) {
        return changeCase(args, false);
    });
    tsInstance->registerNative("strUpr", [changeCase](const auto& args) {
        return changeCase(args, true);
    });
    // consoleFunctions.cc ltrim/rtrim/trim: space, newline and tab.
    static auto trimSpace = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
    tsInstance->registerNative("ltrim", [](const auto& args) -> VMValue {
        const std::string value = args.empty() ? "" : args[0].toString();
        size_t first = 0;
        while (first < value.size() && trimSpace(value[first])) ++first;
        return VMValue(value.substr(first));
    });
    tsInstance->registerNative("rtrim", [](const auto& args) -> VMValue {
        const std::string value = args.empty() ? "" : args[0].toString();
        size_t last = value.size();
        while (last > 0 && trimSpace(value[last - 1])) --last;
        return VMValue(value.substr(0, last));
    });
    tsInstance->registerNative("trim", [](const auto& args) -> VMValue {
        std::string value = args.empty() ? "" : args[0].toString();
        size_t first = 0;
        while (first < value.size() && trimSpace(value[first])) ++first;
        size_t last = value.size();
        while (last > first && trimSpace(value[last - 1])) --last;
        return VMValue(value.substr(first, last - first));
    });
    tsInstance->registerNative("strreplace", [](const auto& args) -> VMValue {
        if (args.size() < 3) return args.empty() ? VMValue("") : args[0];
        return VMValue(replaceAll(args[0].toString(), args[1].toString(), args[2].toString()));
    });

    tsInstance->registerNative("getWordCount", [](const auto& args) -> VMValue {
        return VMValue((int32_t)splitScriptWords(args.empty() ? "" : args[0].toString()).size());
    });
    tsInstance->registerNative("getWord", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        const int index = args[1].toInt();
        return index >= 0 && index < (int)words.size() ? VMValue(words[index]) : VMValue("");
    });
    tsInstance->registerNative("getWords", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        const int first = std::max(0, args[1].toInt());
        const int last = args.size() > 2 ? args[2].toInt() : INT_MAX;
        if (first >= (int)words.size() || last < first) return VMValue("");
        const int end = last == INT_MAX
            ? (int)words.size() : std::min((int)words.size(), last + 1);
        std::string result;
        for (int i = first; i < end; ++i) {
            if (!result.empty()) result += ' ';
            result += words[i];
        }
        return VMValue(result);
    });
    tsInstance->registerNative("getFieldCount", [](const auto& args) -> VMValue {
        return VMValue((int32_t)splitFields(args.empty() ? "" : args[0].toString()).size());
    });
    tsInstance->registerNative("getField", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const auto fields = splitFields(args[0].toString());
        const int index = args[1].toInt();
        return index >= 0 && index < (int)fields.size() ? VMValue(fields[index]) : VMValue("");
    });
    tsInstance->registerNative("getFields", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const auto fields = splitFields(args[0].toString());
        const int first = std::max(0, args[1].toInt());
        const int last = args.size() > 2 ? args[2].toInt() : INT_MAX;
        if (first >= (int)fields.size() || last < first) return VMValue("");
        const int end = last == INT_MAX
            ? (int)fields.size() : std::min((int)fields.size(), last + 1);
        std::string result;
        for (int i = first; i < end; ++i) {
            if (!result.empty()) result += '\t';
            result += fields[i];
        }
        return VMValue(result);
    });
    tsInstance->registerNative("firstWord", [](const auto& args) -> VMValue {
        return args.empty() ? VMValue("") : VMValue(splitScriptWords(args[0].toString()).empty() ? "" : splitScriptWords(args[0].toString())[0]);
    });
    tsInstance->registerNative("restWords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        std::string result;
        for (size_t i = 1; i < words.size(); ++i) {
            if (!result.empty()) result += ' ';
            result += words[i];
        }
        return VMValue(result);
    });

    tsInstance->registerNative("format", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string result = args[0].toString();
        for (size_t i = 1; i < args.size(); ++i)
            result = replaceAll(result, "%" + std::to_string(i), args[i].toString());
        return VMValue(result);
    });

    tsInstance->registerNative("strlen", [](const auto& args) -> VMValue {
        std::string v = args.empty() ? "" : args[0].toString();
        return VMValue((double)v.size());
    });
    // strcspn(str, reject): length of the prefix of str with no chars from reject
    tsInstance->registerNative("strcspn", [](const auto& args) -> VMValue {
        std::string v = args.empty() ? "" : args[0].toString();
        std::string reject = args.size() > 1 ? args[1].toString() : "";
        size_t len = 0;
        for (; len < v.size(); len++) {
            if (reject.find(v[len]) != std::string::npos) break;
        }
        return VMValue((double)len);
    });
    // True when the named skin is one of the Dynamix-provided skins listed
    // in the \$Skin[*, code] table (drives the Show: Dynamix/Custom popup).
    tsInstance->registerNative("isDynamixSkin", [](const auto& args) -> VMValue {
        std::string skin = args.empty() ? "" : args[0].toString();
        if (skin.empty()) return VMValue(0);
        auto* tsx = Engine::instance().script().ts();
        int count = 0;
        if (tsx) count = (int)tsx->getGlobal("$SkinCount").toDouble();
        int limit = count + 8; if (limit > 64) limit = 64;
        for (int i = 0; i < limit; i++) {
            // NOTE: key has NO space after the comma — the interpreter's
            // bracket-index assembly concatenates without whitespace.
            std::string key = "$Skin[" + std::to_string(i) + ",code]";
            std::string code = tsx ? tsx->getGlobal(key).toString() : "";
            if (!code.empty() && code == skin) return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getSubStr", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        auto s = args[0].toString();
        int start = args[1].toInt();
        int count = args[2].toInt();
        if (start < 0 || start >= (int)s.size() || count == 0) return VMValue("");
        return VMValue(s.substr(start, count < 0 ? std::string::npos : (size_t)count));
    });

    tsInstance->registerNative("isObject", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        if (name.empty()) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        char* end = nullptr;
        const long id = std::strtol(name.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (engine.objectState((int)id, state)) return VMValue(1);
        }
        if (engine.findObject(name.c_str())) return VMValue(1);
        auto* item = Console::instance().find(name.c_str());
        if (item) return VMValue(1);
        return VMValue(0);
    });

    auto getObjectField = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const std::string objectName = args[0].toString();
        const std::string fieldName = args[1].toString();
        if (auto* object = namedScriptObject(objectName)) {
            if (const auto* value = findObjectField(object, fieldName)) return *value;
            return VMValue("");
        }
        for (const auto& missionObject : ScriptEngine::instance().missionObjects()) {
            bool matches = missionObject.name == objectName;
            if (!matches) {
                char* end = nullptr;
                const long id = std::strtol(objectName.c_str(), &end, 10);
                matches = end && *end == '\0' && id > 0 && missionObject.id == id;
            }
            if (!matches) continue;
            for (const auto& [field, value] : missionObject.fields)
                if (sameFieldName(field, fieldName)) return value;
            return VMValue("");
        }
        int objectId = 0;
        ScriptObjectState state;
        if (providerObjectId(objectName, objectId) && ScriptEngine::instance().objectState(objectId, state))
            return providerField(state, fieldName);
        return VMValue("");
    };
    auto getObjectDataField = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        if (auto* object = namedScriptObject(args[0].toString())) {
            const auto* datablock = findObjectField(object, "datablock");
            if (!datablock) return VMValue("");
            if (auto* dataObject = namedScriptObject(datablock->toString())) {
                if (const auto* value = findObjectField(dataObject, args[1].toString())) return *value;
            }
            return VMValue("");
        }
        int objectId = 0;
        ScriptObjectState state;
        if (!providerObjectId(args[0].toString(), objectId) ||
            !ScriptEngine::instance().objectState(objectId, state)) return VMValue("");
        // The provider exposes the datablock id, but not its field table.
        // Returning an empty value is safer than inventing a datablock field.
        return VMValue("");
    };
    auto setObjectField = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* object = namedScriptObject(args[0].toString());
        if (!object) {
            int objectId = 0;
            if (!providerObjectId(args[0].toString(), objectId)) return VMValue(0);
            const std::string fieldName = args[1].toString();
            if (sameFieldName(fieldName, "health"))
                return VMValue(ScriptEngine::instance().mutateHealth(objectId, args[2].toFloat()) ? 1 : 0);
            if (sameFieldName(fieldName, "energy"))
                return VMValue(ScriptEngine::instance().mutateEnergy(objectId, args[2].toFloat()) ? 1 : 0);
            if (sameFieldName(fieldName, "repairRate"))
                return VMValue(ScriptEngine::instance().mutateRepairRate(objectId, args[2].toFloat()) ? 1 : 0);
            if (sameFieldName(fieldName, "team"))
                return VMValue(ScriptEngine::instance().mutateTeam(objectId, args[2].toInt()) ? 1 : 0);
            if (sameFieldName(fieldName, "velocity")) {
                Point3F velocity;
                if (!parseVector(args[2], velocity)) return VMValue(0);
                return VMValue(ScriptEngine::instance().mutateVelocity(objectId, velocity) ? 1 : 0);
            }
            return VMValue(0);
        }
        const std::string fieldName = args[1].toString();
        return VMValue(ScriptEngine::instance().setObjectField(object, fieldName, args[2]) ? 1 : 0);
    };
    auto setObjectDataField = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* object = namedScriptObject(args[0].toString());
        if (!object) return VMValue(0);
        const auto* datablock = findObjectField(object, "datablock");
        auto* dataObject = datablock ? namedScriptObject(datablock->toString()) : nullptr;
        if (!dataObject) return VMValue(0);
        const std::string fieldName = args[1].toString();
        return VMValue(ScriptEngine::instance().setObjectField(dataObject, fieldName, args[2]) ? 1 : 0);
    };
    auto getGroup = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue("");
        auto* object = namedScriptObject(args[0].toString());
        if (!object) return VMValue("");
        auto& engine = ScriptEngine::instance();
        if (const auto it = object->internals.find("__parent"); it != object->internals.end())
            if (auto* group = engine.findObject(it->second.toString().c_str()))
                return VMValue(engine.objectId(group));
        if (const auto it = object->internals.find("parent"); it != object->internals.end()) return it->second;
        return VMValue("");
    };
    auto getGroupCount = [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        const std::string group = engine.canonicalName(args[0].toString());
        ScriptObject* groupObject = namedScriptObject(group);
        if (!groupObject) return VMValue(0);
        // Members added with add() plus objects declared inside the group.
        // Members, plus objects linked only by a declared parent.
        int count = groupObject->internals["__childCount"].toInt();
        const std::string groupKey = engine.objectKey(groupObject);
        for (const auto& [name, object] : ScriptEngine::instance().objects) {
            if (!object) continue;
            const auto it = object->internals.find("parent");
            if (it == object->internals.end() || engine.findObject(it->second.toString().c_str()) != groupObject)
                continue;
            const auto member = object->internals.find("__parent");
            if (member != object->internals.end() && member->second.toString() == groupKey) continue;
            ++count;
        }
        return VMValue(count);
    };
    auto getFieldString = [getObjectField](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        int objectId = 0;
        if (namedScriptObject(args[0].toString()) || providerObjectId(args[0].toString(), objectId))
            return getObjectField(args);
        const int index = args[1].toInt();
        const auto fields = splitFields(args[0].toString());
        return index >= 0 && index < (int)fields.size() ? VMValue(fields[index]) : VMValue("");
    };
    auto setFieldValue = [setObjectField](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() >= 3 && namedScriptObject(args[0].toString())) return setObjectField(args);
        if (args.size() < 3) return VMValue("");
        auto fields = splitFields(args[0].toString());
        const int index = args[1].toInt();
        if (index < 0) return VMValue("");
        while ((int)fields.size() <= index) fields.emplace_back();
        fields[index] = args[2].toString();
        std::string result;
        for (const auto& field : fields) {
            if (!result.empty()) result += '\t';
            result += field;
        }
        return VMValue(result);
    };
    tsInstance->registerNative("getField", getFieldString);
    tsInstance->registerNative("getFieldValue", getObjectField);
    tsInstance->registerNative("getDataField", getObjectDataField);
    tsInstance->registerNative("setField", setFieldValue);
    tsInstance->registerNative("setFieldValue", setObjectField);
    tsInstance->registerNative("setDataField", setObjectDataField);
    tsInstance->registerNative("getGroup", getGroup);
    tsInstance->registerNative("getCount", getGroupCount);
    vmInstance->registerNativeFunction("getField", getFieldString);
    vmInstance->registerNativeFunction("getFieldValue", getObjectField);
    vmInstance->registerNativeFunction("getDataField", getObjectDataField);
    vmInstance->registerNativeFunction("setField", setFieldValue);
    vmInstance->registerNativeFunction("setFieldValue", setObjectField);
    vmInstance->registerNativeFunction("setDataField", setObjectDataField);
    vmInstance->registerNativeFunction("getGroup", getGroup);
    vmInstance->registerNativeFunction("getCount", getGroupCount);

    auto objectState = [](const std::vector<VMValue>& args, ScriptObjectState& state) {
        if (args.empty()) return false;
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        return end && *end == '\0' && id > 0 &&
               ScriptEngine::instance().objectState((int)id, state);
    };
    auto getDataBlock = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        return VMValue(objectState(args, state) ? state.datablockId : 0);
    };
    auto getClassName = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        return VMValue(objectState(args, state) ? state.className : "");
    };
    auto getShapeFile = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        return VMValue(objectState(args, state) ? state.shapeName : "");
    };
    auto getObjectName = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        if (objectState(args, state)) return VMValue(state.name);
        if (!args.empty()) {
            if (auto* object = namedScriptObject(args[0].toString())) return VMValue(scriptObjectName(object));
        }
        return VMValue("");
    };
    auto getPosition = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        if (!objectState(args, state)) {
            if (args.empty()) return VMValue("0 0 0");
            Point3F position;
            if (!Engine::instance().game().world().getMissionObjectPosition(args[0].toString(), position))
                return VMValue("0 0 0");
            char value[96];
            snprintf(value, sizeof(value), "%g %g %g", position.x, position.y, position.z);
            return VMValue(value);
        }
        char value[96];
        snprintf(value, sizeof(value), "%g %g %g", state.position.x,
                 state.position.y, state.position.z);
        return VMValue(value);
    };
    auto getTeam = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        return VMValue(objectState(args, state) ? state.teamId : 0);
    };
    auto getState = [objectState](const auto& args) -> VMValue {
        ScriptObjectState state;
        return VMValue(objectState(args, state) ? state.state : 0);
    };
    tsInstance->registerNative("getDataBlock", getDataBlock);
    tsInstance->registerNative("getClassName", getClassName);
    tsInstance->registerNative("getShapeFile", getShapeFile);
    tsInstance->registerNative("getName", getObjectName);
    tsInstance->registerNative("getPosition", getPosition);
    tsInstance->registerNative("getScale", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("1 1 1");
        Point3F scale;
        if (!Engine::instance().game().world().getMissionObjectScale(args[0].toString(), scale))
            return VMValue("1 1 1");
        char value[96];
        snprintf(value, sizeof(value), "%g %g %g", scale.x, scale.y, scale.z);
        return VMValue(value);
    });
    tsInstance->registerNative("getTeam", getTeam);
    tsInstance->registerNative("setTeam", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int objectId = args[0].toInt();
        const int team = args[1].toInt();
        if (objectId <= 0 || team < 0) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateTeam(objectId, team) ? 1 : 0);
    });
    tsInstance->registerNative("getState", getState);
    tsInstance->registerNative("getDamageState", getState);

    // call(%func, %a1, %a2, ...) — invoke the function named by the first
    // argument, forwarding the rest. T2 uses this for callbacks stored in
    // string variables (message dialogs, canned chat, save/load validation).
    // Dispatch mirrors the interpreter: natives → script/DSO functions → console commands.
    tsInstance->registerNative("call", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string fn = args[0].toString();
        std::vector<VMValue> callArgs(args.begin() + 1, args.end());
        auto* ts = Engine::instance().script().ts();
        if (!ts) return VMValue(0);
        std::string lower = fn;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        const auto& natives = ts->getNatives();
        auto nit = natives.find(lower);
        if (nit != natives.end()) return nit->second(callArgs);
        if (ts->hasFunction(fn)) return ts->callFunction(fn, callArgs);
        auto* item = Console::instance().find(fn.c_str());
        if (item && item->type == Console::ConsoleItem::Command) {
            std::vector<std::string> argStorage;
            argStorage.reserve(callArgs.size());
            for (auto& a : callArgs) argStorage.push_back(a.toString());
            std::vector<const char*> argv;
            argv.reserve(callArgs.size() + 1);
            argv.push_back(fn.c_str());
            for (auto& s : argStorage) argv.push_back(s.c_str());
            item->cmd((int32_t)argv.size(), argv.data());
            return VMValue(1);
        }
        return VMValue(0);
    });

    tsInstance->registerNative("isDemo", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isDemoMode() ? 1 : 0);
    });
    tsInstance->registerNative("isDemoPlaying", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isDemoPlaying() ? 1 : 0);
    });
    tsInstance->registerNative("isServer", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isServer() ? 1 : 0);
    });
    tsInstance->registerNative("isClient", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isClient() ? 1 : 0);
    });
    auto controlObject = [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().controlObjectId());
    };
    auto cameraObject = [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().cameraObjectId());
    };
    tsInstance->registerNative("getControlObject", controlObject);
    tsInstance->registerNative("getControlObjectId", controlObject);
    tsInstance->registerNative("ServerConnection::getControlObject", controlObject);
    tsInstance->registerNative("GameConnection::getControlObject", controlObject);
    tsInstance->registerNative("setControlObject", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int connectionId = args[0].toInt();
        const int objectId = args[1].toInt();
        if (connectionId < 0 || objectId <= 0) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateControlObject(connectionId, objectId) ? 1 : 0);
    });
    tsInstance->registerNative("ServerConnection::setControlObject", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int connectionId = args[0].toInt();
        const int objectId = args[1].toInt();
        if (connectionId < 0 || objectId <= 0) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateControlObject(connectionId, objectId) ? 1 : 0);
    });
    tsInstance->registerNative("GameConnection::setControlObject", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int connectionId = args[0].toInt();
        const int objectId = args[1].toInt();
        if (connectionId < 0 || objectId <= 0) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateControlObject(connectionId, objectId) ? 1 : 0);
    });
    tsInstance->registerNative("getCameraObject", cameraObject);
    tsInstance->registerNative("ServerConnection::getCameraObject", cameraObject);
    tsInstance->registerNative("GameConnection::getCameraObject", cameraObject);
    vmInstance->registerNativeFunction("isServer", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isServer() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("isClient", [](const auto&) -> VMValue {
        return VMValue(ScriptEngine::instance().isClient() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("getControlObject", controlObject);
    vmInstance->registerNativeFunction("getControlObjectId", controlObject);
    vmInstance->registerNativeFunction("ServerConnection::getControlObject", controlObject);
    vmInstance->registerNativeFunction("GameConnection::getControlObject", controlObject);
    vmInstance->registerNativeFunction("getCameraObject", cameraObject);
    vmInstance->registerNativeFunction("ServerConnection::getCameraObject", cameraObject);
    vmInstance->registerNativeFunction("GameConnection::getCameraObject", cameraObject);

    auto connectionState = [] { return ScriptEngine::instance().connectionState(); };
    tsInstance->registerNative("getServerAddress", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().serverAddress);
    });
    tsInstance->registerNative("getServerPort", [connectionState](const auto&) -> VMValue {
        return VMValue((int32_t)connectionState().serverPort);
    });
    tsInstance->registerNative("getClientName", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().clientName);
    });
    tsInstance->registerNative("getClientId", [connectionState](const auto&) -> VMValue {
        return VMValue((int32_t)connectionState().clientId);
    });
    tsInstance->registerNative("getMissionName", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().missionName);
    });
    tsInstance->registerNative("getMissionType", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().missionType);
    });
    tsInstance->registerNative("getMissionCRC", [connectionState](const auto&) -> VMValue {
        return VMValue((int32_t)connectionState().missionCrc);
    });
    tsInstance->registerNative("isOnline", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().online ? 1 : 0);
    });
    tsInstance->registerNative("isObserver", [connectionState](const auto&) -> VMValue {
        return VMValue(connectionState().observer ? 1 : 0);
    });

    // String utility functions
    auto wordCount = [](const std::string& s) -> int {
        int count = 0;
        bool inWord = false;
        for (char c : s) {
            if (c == ' ' || c == '\t') { inWord = false; }
            else if (!inWord) { inWord = true; count++; }
        }
        return count;
    };

    auto getWord = [&](const std::string& s, int idx) -> std::string {
        int count = 0;
        size_t start = 0;
        bool inWord = false;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ' ' || s[i] == '\t') {
                if (inWord) {
                    if (count == idx) return s.substr(start, i - start);
                    inWord = false;
                    count++;
                }
            } else if (!inWord) {
                inWord = true;
                start = i;
            }
        }
        return "";
    };

    tsInstance->registerNative("getWord", [getWord](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        return VMValue(getWord(args[0].toString(), args[1].toInt()));
    });

    tsInstance->registerNative("setWord", [getWord](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        std::string s = args[0].toString();
        int idx = args[1].toInt();
        std::string val = args[2].toString();
        std::string result;
        int count = 0;
        size_t start = 0;
        bool inWord = false;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ' ' || s[i] == '\t') {
                if (inWord) {
                    if (count == idx) {
                        result += val;
                    } else {
                        result += s.substr(start, i - start);
                    }
                    inWord = false;
                    count++;
                }
                if (i < s.size()) result += s[i];
            } else if (!inWord) {
                inWord = true;
                start = i;
            }
        }
        if (idx >= count) {
            if (!result.empty() && result.back() != ' ') result += ' ';
            result += val;
        }
        return VMValue(result);
    });

    tsInstance->registerNative("firstWord", [getWord](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        return VMValue(getWord(args[0].toString(), 0));
    });

    tsInstance->registerNative("restWords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        size_t pos = s.find_first_not_of(" \t");
        if (pos == std::string::npos) return VMValue("");
        pos = s.find_first_of(" \t", pos);
        if (pos == std::string::npos) return VMValue("");
        pos = s.find_first_not_of(" \t", pos);
        if (pos == std::string::npos) return VMValue("");
        return VMValue(s.substr(pos));
    });

    tsInstance->registerNative("getWordCount", [wordCount](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(wordCount(args[0].toString()));
    });

    tsInstance->registerNative("getFieldCount", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string s = args[0].toString();
        if (s.empty()) return VMValue(0);
        int count = 1;
        for (char c : s) if (c == '\t') count++;
        return VMValue(count);
    });

    tsInstance->registerNative("strReplace", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        std::string s = args[0].toString();
        std::string from = args[1].toString();
        std::string to = args[2].toString();
        if (from.empty()) return VMValue(s);
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
        return VMValue(s);
    });

    tsInstance->registerNative("strlwr", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        for (char& c : s) c = tolower(c);
        return VMValue(s);
    });

    tsInstance->registerNative("rtrim", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        return VMValue(s);
    });

    tsInstance->registerNative("strupr", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        for (char& c : s) c = toupper(c);
        return VMValue(s);
    });

    tsInstance->registerNative("collapseEscape", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        return args[0];
    });

    tsInstance->registerNative("setLogMode", [](const auto& args) -> VMValue {
        return VMValue(1);
    });

    tsInstance->registerNative("strchr", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        std::string s = args[0].toString();
        std::string ch = args[1].toString();
        if (ch.empty()) return VMValue(-1);
        auto pos = s.find(ch[0]);
        return VMValue(pos != std::string::npos ? (int32_t)pos : -1);
    });

    tsInstance->registerNative("stripChars", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        std::string s = args[0].toString();
        std::string chars = args[1].toString();
        std::string result;
        for (char c : s) {
            if (chars.find(c) == std::string::npos) result += c;
        }
        return VMValue(result);
    });


    tsInstance->registerNative("strcmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        const int result = strcmp(args[0].toString().c_str(), args[1].toString().c_str());
        return VMValue(result < 0 ? -1 : result > 0 ? 1 : 0);
    });

    // Canvas / window
    tsInstance->registerNative("createCanvas", [](const auto&) -> VMValue {
        // Create the GuiCanvas ScriptObject for the GUI renderer
        auto* canvas = new ScriptObject;
        canvas->className = "GuiCanvas";
        canvas->name = "Canvas";
        canvas->fields["extent"] = VMValue("1024 768");
        canvas->fields["position"] = VMValue("0 0");
        ScriptEngine::instance().addObject(canvas);
        Console::instance().printf(LogLevel::Info, "GUI: created GuiCanvas");
        return VMValue(1);
    });

    // Package management
    tsInstance->registerNative("activatePackage", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        auto* ts = ScriptEngine::instance().ts();
        if (!ts || !ts->activatePackage(name)) return VMValue(0);
        return VMValue(1);
    });
    tsInstance->registerNative("deactivatePackage", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ts = ScriptEngine::instance().ts();
        if (!ts || !ts->deactivatePackage(args[0].toString())) return VMValue(0);
        return VMValue(1);
    });
    tsInstance->registerNative("isActivePackage", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ts = ScriptEngine::instance().ts();
        if (ts) return VMValue(ts->isActivePackage(args[0].toString()) ? 1 : 0);
        const std::string wanted = args[0].toString();
        const int count = Console::instance().getIntVariable("$TotalNumberOfPackages", 0);
        for (int i = 0; i < count; ++i) {
            const std::string key = "$Package[" + std::to_string(i) + "]";
            if (Console::instance().getStringVariable(key.c_str(), "") == wanted)
                return VMValue(1);
        }
        return VMValue(0);
    });

    // WON init (defunct, return success)
    tsInstance->registerNative("WONInit", [](const auto&) -> VMValue {
        return VMValue(1);
    });

    tsInstance->registerNative("setRandomSeed", [](const auto& args) -> VMValue {
        if (!args.empty()) srand((unsigned int)args[0].toInt());
        return VMValue(1);
    });

    tsInstance->registerNative("fileBase", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        size_t slash = path.rfind('/');
        if (slash == std::string::npos) slash = path.rfind('\\');
        if (slash == std::string::npos) slash = 0; else slash++;
        size_t dot = path.rfind('.');
        if (dot == std::string::npos || dot < slash) dot = path.size();
        return VMValue(path.substr(slash, dot - slash));
    });

    tsInstance->registerNative("openForRead", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string objName = args[0].toString();
        std::string path = args.size() > 1 ? args[1].toString() : "";
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue(0);
        std::vector<uint8_t> data;
        if (!Engine::instance().fs().readFile(path.c_str(), data)) {
            const std::string outDir = Console::instance().getStringVariable("outputDir", "");
            const std::string modPath = Console::instance().getStringVariable("modPath", "base");
            if (outDir.empty()) return VMValue(0);
            std::ifstream file(outDir + "/" + modPath + "/" + path, std::ios::binary);
            if (!file) return VMValue(0);
            data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        if (path.find("prefs/") == 0) {
            const std::string content(data.begin(), data.end());
            if (content.find("// Tribes 2 Input Map File") == std::string::npos &&
                content.find(".bind(") != std::string::npos) {
                const std::string header = "// Tribes 2 Input Map File\n";
                data.insert(data.begin(), header.begin(), header.end());
            }
        }
        sobj->internals["__fo_data"] = VMValue(std::string(data.begin(), data.end()));
        sobj->internals["__fo_pos"] = VMValue(0);
        return VMValue(1);
    });
    tsInstance->registerNative("isEOF", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(1);
        std::string objName = args[0].toString();
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue(1);
        auto dit = sobj->internals.find("__fo_data");
        if (dit == sobj->internals.end()) return VMValue(1);
        auto pit = sobj->internals.find("__fo_pos");
        if (pit == sobj->internals.end()) return VMValue(1);
        return VMValue((int32_t)(pit->second.toInt() >= (int32_t)dit->second.str.size()));
    });
    tsInstance->registerNative("readLine", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string objName = args[0].toString();
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue("");
        auto dit = sobj->internals.find("__fo_data");
        if (dit == sobj->internals.end()) return VMValue("");
        auto pit = sobj->internals.find("__fo_pos");
        if (pit == sobj->internals.end()) return VMValue("");
        std::string& data = dit->second.str;
        int32_t& pos = pit->second.i;
        if (pos >= (int32_t)data.size()) return VMValue("");
        size_t end = data.find('\n', pos);
        std::string line;
        if (end == std::string::npos) {
            line = data.substr(pos);
            pos = (int32_t)data.size();
        } else {
            line = data.substr(pos, end - pos);
            pos = (int32_t)(end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
        }
        return VMValue(line);
    });
    tsInstance->registerNative("close", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(1);
        std::string objName = args[0].toString();
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (sobj) {
            auto dit = sobj->internals.find("__fo_data");
            auto pit = sobj->internals.find("__fo_path");
            if (dit != sobj->internals.end() && pit != sobj->internals.end()) {
                // FileObject: flush the accumulated buffer to disk. openForAppend
                // preloads existing content into the buffer, so a full-buffer
                // write is equivalent to appending.
                std::string path = pit->second.toString();
                std::string outDir = Console::instance().getStringVariable("outputDir", "");
                std::string modPath = Console::instance().getStringVariable("modPath", "base");
                std::string fullPath;
                if (!path.empty() && path[0] == '/') {
                    fullPath = path;
                } else if (!outDir.empty()) {
                    // Same root the engine uses for prefs/ exports
                    fullPath = outDir + "/" + modPath + "/" + path;
                } else {
                    fullPath = path;
                }
                auto slash = fullPath.rfind('/');
                if (slash != std::string::npos) {
                    std::string dir = fullPath.substr(0, slash);
                    struct stat st; if (stat(dir.c_str(), &st) != 0) mkdir(dir.c_str(), 0755);
                }
                FILE* f = fopen(fullPath.c_str(), "w");
                if (f) { fwrite(dit->second.str.data(), 1, dit->second.str.size(), f); fclose(f); }
            }
            sobj->internals.erase("__fo_data");
            sobj->internals.erase("__fo_pos");
            sobj->internals.erase("__fo_path");
        }
        return VMValue(1);
    });

    tsInstance->registerNative("stricmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string a = args[0].toString(), b = args[1].toString();
        for (auto& c : a) c = tolower((unsigned char)c);
        for (auto& c : b) c = tolower((unsigned char)c);
        return VMValue(strcmp(a.c_str(), b.c_str()));
    });

    tsInstance->registerNative("strpos", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        std::string haystack = args[0].toString();
        std::string needle = args[1].toString();
        size_t offset = args.size() > 2 ? std::max(0, args[2].toInt()) : 0;
        if (offset >= haystack.size()) return VMValue(-1);
        size_t pos = haystack.find(needle, offset);
        return VMValue(pos != std::string::npos ? (int32_t)pos : -1);
    });

    tsInstance->registerNative("getWords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        int startIdx = args.size() > 1 ? args[1].toInt() : 0;
        int endIdx = args.size() > 2 ? args[2].toInt() : -1;
        std::string result;
        size_t pos = 0;
        int count = 0;
        while (pos <= s.size() && count <= endIdx) {
            size_t start = s.find_first_not_of(" \t\n", pos);
            if (start == std::string::npos) break;
            size_t end = s.find_first_of(" \t\n", start);
            if (end == std::string::npos) end = s.size();
            if (count >= startIdx && (endIdx < 0 || count <= endIdx)) {
                if (!result.empty()) result += " ";
                result += s.substr(start, end - start);
            }
            count++;
            pos = end + 1;
        }
        return VMValue(result);
    });

    tsInstance->registerNative("deleteFile", [](const auto& args) -> VMValue {
        if (args.size() != 1 || !Engine::instance().filesys) return VMValue(0);
        return VMValue(Engine::instance().fs().removeFile(args[0].toString().c_str()) ? 1 : 0);
    });

    // Sim::getCurrentTime.
    tsInstance->registerNative("getSimTime", [](const auto&) -> VMValue {
        return VMValue((int32_t)(SimState::simTime() * 1000.0));
    });
    tsInstance->registerNative("getRealTime", [](const auto&) -> VMValue {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        // Torque exposes this platform clock as a 32-bit millisecond value.
        return VMValue((int32_t)(uint32_t)millis);
    });

    tsInstance->registerNative("strToPlayerName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        return VMValue(args[0].toString());
    });

    tsInstance->registerNative("stripTrailingSpaces", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        size_t end = s.find_last_not_of(" \t\r\n");
        return VMValue(end == std::string::npos ? "" : s.substr(0, end + 1));
    });

    tsInstance->registerNative("stripLeadingSpaces", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        size_t start = s.find_first_not_of(" \t\r\n");
        return VMValue(start == std::string::npos ? "" : s.substr(start));
    });

    tsInstance->registerNative("stripSpaces", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        std::string result;
        for (char c : s) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') result += c;
        return VMValue(result);
    });


    // Math functions
    tsInstance->registerNative("mSin", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(sin(args[0].toDouble()));
    });
    tsInstance->registerNative("mCos", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(cos(args[0].toDouble()));
    });
    tsInstance->registerNative("mTan", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(tan(args[0].toDouble()));
    });
    tsInstance->registerNative("mAsin", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(asin(args[0].toDouble()));
    });
    tsInstance->registerNative("mAcos", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(acos(args[0].toDouble()));
    });
    tsInstance->registerNative("mAtan", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        if (args.size() > 1) return VMValue(atan2(args[0].toDouble(), args[1].toDouble()));
        return VMValue(atan(args[0].toDouble()));
    });
    // math/mConsoleFunctions.cc: mRadToDeg / mDegToRad and the solvers
    // ("<count> <roots...>", unsolved roots 0).
    tsInstance->registerNative("mRadToDeg", [](const auto& args) -> VMValue {
        return VMValue(args.empty() ? 0.0f : (float)args[0].toDouble() * (180.0f / (float)M_PI));
    });
    tsInstance->registerNative("mDegToRad", [](const auto& args) -> VMValue {
        return VMValue(args.empty() ? 0.0f : (float)args[0].toDouble() * ((float)M_PI / 180.0f));
    });
    auto solverArg = [](const auto& args, size_t i) { return i < args.size() ? (float)args[i].toDouble() : 0.0f; };
    tsInstance->registerNative("mSolveQuadratic", [solverArg](const auto& args) -> VMValue {
        float x[2] = {0, 0};
        const uint32_t n = ProjectileAim::solveQuadratic(solverArg(args, 0), solverArg(args, 1), solverArg(args, 2), x);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%u %g %g", n, x[0], x[1]);
        return VMValue(buffer);
    });
    tsInstance->registerNative("mSolveCubic", [solverArg](const auto& args) -> VMValue {
        float x[3] = {0, 0, 0};
        const uint32_t n = ProjectileAim::solveCubic(solverArg(args, 0), solverArg(args, 1), solverArg(args, 2),
                                                     solverArg(args, 3), x);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%u %g %g %g", n, x[0], x[1], x[2]);
        return VMValue(buffer);
    });
    tsInstance->registerNative("mSolveQuartic", [solverArg](const auto& args) -> VMValue {
        float x[4] = {0, 0, 0, 0};
        const uint32_t n = ProjectileAim::solveQuartic(solverArg(args, 0), solverArg(args, 1), solverArg(args, 2),
                                                       solverArg(args, 3), solverArg(args, 4), x);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%u %g %g %g %g", n, x[0], x[1], x[2], x[3]);
        return VMValue(buffer);
    });
    tsInstance->registerNative("mSqrt", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(sqrt(args[0].toDouble()));
    });
    tsInstance->registerNative("mAbs", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(fabs(args[0].toDouble()));
    });
    // The retail mMod(num, div): dAtoi(num) % dAtoi(div).
    tsInstance->registerNative("mMod", [](const auto& args) -> VMValue {
        const int num = args.size() > 0 ? std::atoi(args[0].toString().c_str()) : 0;
        const int div = args.size() > 1 ? std::atoi(args[1].toString().c_str()) : 0;
        return VMValue(div != 0 ? num % div : 0);
    });
    tsInstance->registerNative("mFloor", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(floor(args[0].toDouble()));
    });
    tsInstance->registerNative("mCeil", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(ceil(args[0].toDouble()));
    });
    tsInstance->registerNative("mRound", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(round(args[0].toDouble()));
    });
    tsInstance->registerNative("mClampF", [](const auto& args) -> VMValue {
        if (args.size() < 3) return args.empty() ? VMValue(0.0) : args[0];
        double lo = args[1].toDouble();
        double hi = args[2].toDouble();
        if (lo > hi) std::swap(lo, hi);
        return VMValue(std::clamp(args[0].toDouble(), lo, hi));
    });
    tsInstance->registerNative("mClampI", [](const auto& args) -> VMValue {
        if (args.size() < 3) return args.empty() ? VMValue(0) : VMValue(args[0].toInt());
        int lo = args[1].toInt();
        int hi = args[2].toInt();
        if (lo > hi) std::swap(lo, hi);
        return VMValue(std::clamp(args[0].toInt(), lo, hi));
    });
    tsInstance->registerNative("mPow", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0.0);
        return VMValue(pow(args[0].toDouble(), args[1].toDouble()));
    });
    tsInstance->registerNative("mLog", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(log(args[0].toDouble()));
    });
    tsInstance->registerNative("mFloatLength", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        if (args.size() < 2) return VMValue(args[0].toDouble());
        double val = args[0].toDouble();
        int len = args[1].toInt();
        if (len == 0) return VMValue((int32_t)val);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", len, val);
        return VMValue(atof(buf));
    });
    // mFormatFloat(value, "fmt") — printf-style number formatting for HUD/options
    // text (e.g. mFormatFloat(%ping, "%4.0f")). The format string comes from
    // scripts; %f consumes a double in varargs.
    tsInstance->registerNative("mFormatFloat", [](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue("0") : VMValue(args[0].toString());
        double v = args[0].toDouble();
        std::string fmt = args[1].toString();
        char buf[128];
        snprintf(buf, sizeof(buf), fmt.c_str(), v);
        return VMValue(buf);
    });
    tsInstance->registerNative("getRandom", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue((double)rand() / RAND_MAX);
        int64_t from = 0;
        int64_t to = args[0].toInt();
        if (args.size() > 1) {
            from = args[0].toInt();
            to = args[1].toInt();
        }
        if (from > to) std::swap(from, to);
        if (from == to) return VMValue((int32_t)from);
        const uint64_t span = (uint64_t)(to - from) + 1u;
        return VMValue((int32_t)(from + (int64_t)((uint64_t)rand() % span)));
    });
    tsInstance->registerNative("getMax", [](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue(0.0) : args[0];
        return VMValue(std::max(args[0].toDouble(), args[1].toDouble()));
    });
    tsInstance->registerNative("getMin", [](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue(0.0) : args[0];
        return VMValue(std::min(args[0].toDouble(), args[1].toDouble()));
    });

    // Vector math functions
    auto parseVecStrict = [](const std::string& s, std::array<double, 3>& v) -> bool {
        const char* cursor = s.c_str();
        char* end = nullptr;
        for (double& component : v) {
            while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
            component = std::strtod(cursor, &end);
            if (end == cursor || !std::isfinite(component)) return false;
            cursor = end;
        }
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        if (*cursor != '\0') return false;
        return true;
    };
    auto parseVec = [parseVecStrict](const std::string& s) -> std::array<double, 3> {
        std::array<double, 3> v = {0, 0, 0};
        if (!parseVecStrict(s, v)) return {0, 0, 0};
        return v;
    };
    auto fmtVec = [](double x, double y, double z) -> std::string {
        char buf[128];
        snprintf(buf, sizeof(buf), "%g %g %g", x, y, z);
        return buf;
    };
    tsInstance->registerNative("setWindVelocity", [parseVecStrict](const auto& args) -> VMValue {
        if (args.size() != 1) return VMValue(0);
        std::array<double, 3> v{};
        if (!parseVecStrict(args[0].toString(), v)) return VMValue(0);
        setTorchWindVelocity(Math::torquePointToYUp({(float)v[0], (float)v[1], (float)v[2]}));
        return VMValue(1);
    });
    tsInstance->registerNative("getWindVelocity", [fmtVec](const auto&) -> VMValue {
        const Point3F wind = torchWindVelocityToTorque();
        return VMValue(fmtVec(wind.x, wind.y, wind.z));
    });
    tsInstance->registerNative("VectorNormalize", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("0 0 0");
        auto v = parseVec(args[0].toString());
        double len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (len > 0 && std::isfinite(len)) {
            v[0] /= len; v[1] /= len; v[2] /= len;
        } else {
            v = {0, 0, 0};
        }
        return VMValue(fmtVec(v[0], v[1], v[2]));
    });
    tsInstance->registerNative("VectorScale", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue("0 0 0") : args[0];
        auto v = parseVec(args[0].toString());
        double s = args[1].toDouble();
        return VMValue(fmtVec(v[0]*s, v[1]*s, v[2]*s));
    });
    // getBoxCenter (math/mathTypes.cc): "minx miny minz maxx maxy maxz".
    tsInstance->registerNative("getBoxCenter", [](const auto& args) -> VMValue {
        float b[6] = {0, 0, 0, 0, 0, 0};
        if (!args.empty())
            std::sscanf(args[0].toString().c_str(), "%f %f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", (b[0] + b[3]) * 0.5f, (b[1] + b[4]) * 0.5f,
                      (b[2] + b[5]) * 0.5f);
        return VMValue(buffer);
    });
    tsInstance->registerNative("VectorAdd", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue("0 0 0") : args[0];
        auto a = parseVec(args[0].toString());
        auto b = parseVec(args[1].toString());
        return VMValue(fmtVec(a[0]+b[0], a[1]+b[1], a[2]+b[2]));
    });
    tsInstance->registerNative("VectorSub", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue("0 0 0") : args[0];
        auto a = parseVec(args[0].toString());
        auto b = parseVec(args[1].toString());
        return VMValue(fmtVec(a[0]-b[0], a[1]-b[1], a[2]-b[2]));
    });
    tsInstance->registerNative("VectorDot", [parseVec](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0.0);
        auto a = parseVec(args[0].toString());
        auto b = parseVec(args[1].toString());
        return VMValue(a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
    });
    tsInstance->registerNative("VectorCross", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("0 0 0");
        auto a = parseVec(args[0].toString());
        auto b = parseVec(args[1].toString());
        return VMValue(fmtVec(a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]));
    });
    tsInstance->registerNative("VectorDist", [parseVec](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0.0);
        auto a = parseVec(args[0].toString());
        auto b = parseVec(args[1].toString());
        double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        return VMValue(sqrt(dx*dx + dy*dy + dz*dz));
    });

    // Register native functions for DSO VM
    vmInstance->registerNativeFunction("echo", [](const auto& args) {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Info, "%s", msg.c_str());
        return VMValue();
    });

    vmInstance->registerNativeFunction("warn", [](const auto& args) {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Warn, "%s", msg.c_str());
        return VMValue();
    });

    vmInstance->registerNativeFunction("error", [](const auto& args) {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Error, "%s", msg.c_str());
        return VMValue();
    });

    vmInstance->registerNativeFunction("setFogDistance", setScriptFogDistance);
    vmInstance->registerNativeFunction("setFogDensity", setScriptFogDensity);
    vmInstance->registerNativeFunction("setFogColor", setScriptFogColor);
    vmInstance->registerNativeFunction("setFogTransition", setScriptFogTransition);
    vmInstance->registerNativeFunction("setSkyColor", setScriptSkyColor);
    vmInstance->registerNativeFunction("setSkyMaterial", setScriptSkyMaterialList);
    vmInstance->registerNativeFunction("setSkyMaterialList", setScriptSkyMaterialList);
    vmInstance->registerNativeFunction("setSunDirection", setScriptSunDirection);
    vmInstance->registerNativeFunction("setSunColor", setScriptSunColor);
    vmInstance->registerNativeFunction("setSunAmbient", setScriptSunAmbient);
    vmInstance->registerNativeFunction("setPrecipitation", setScriptPrecipitation);
    vmInstance->registerNativeFunction("setPrecipitationEnabled", setScriptPrecipitationEnabled);
    vmInstance->registerNativeFunction("setPrecipitationType", setScriptPrecipitationType);
    vmInstance->registerNativeFunction("setPrecipitationWind", setScriptPrecipitationWind);
    vmInstance->registerNativeFunction("setPrecipitationBox", setScriptPrecipitationBox);
    vmInstance->registerNativeFunction("setLightning", setScriptLightning);
    vmInstance->registerNativeFunction("strikeLightning", strikeScriptLightning);
    vmInstance->registerNativeFunction("Lightning::strike", strikeScriptLightning);
    vmInstance->registerNativeFunction("MissionCleanup", [](const auto&) -> VMValue {
        clearScriptMissionState();
        Engine::instance().game().world().cleanupMission();
        return VMValue(1);
    });
    vmInstance->registerNativeFunction("setWaterLevel", setScriptWaterLevel);
    vmInstance->registerNativeFunction("setWaterType", setScriptWaterType);
    vmInstance->registerNativeFunction("setLiquidType", setScriptWaterType);
    vmInstance->registerNativeFunction("setWaterOpacity", setScriptWaterOpacity);
    vmInstance->registerNativeFunction("setWaterColor", setScriptWaterColor);
    vmInstance->registerNativeFunction("WaterBlock::setWaterLevel", setScriptWaterLevel);
    vmInstance->registerNativeFunction("WaterBlock::setWaterType", setScriptWaterType);
    vmInstance->registerNativeFunction("WaterBlock::setLiquidType", setScriptWaterType);
    vmInstance->registerNativeFunction("WaterBlock::setWaterOpacity", setScriptWaterOpacity);
    vmInstance->registerNativeFunction("WaterBlock::setOpacity", setScriptWaterOpacity);
    vmInstance->registerNativeFunction("WaterBlock::setWaterColor", setScriptWaterColor);
    vmInstance->registerNativeFunction("WaterBlock::setColor", setScriptWaterColor);

    vmInstance->registerNativeFunction("strLen", [](const auto& args) {
        if (args.empty()) return VMValue(0);
        return VMValue((int32_t)args[0].toString().size());
    });

    vmInstance->registerNativeFunction("strCmp", [](const auto& args) {
        if (args.size() < 2) return VMValue(-1);
        int cmp = strcmp(args[0].toString().c_str(), args[1].toString().c_str());
        return VMValue(cmp);
    });

    vmInstance->registerNativeFunction("strStr", [](const auto& args) {
        if (args.size() < 2) return VMValue(-1);
        auto pos = args[0].toString().find(args[1].toString());
        return VMValue(pos != std::string::npos ? (int32_t)pos : -1);
    });

    vmInstance->registerNativeFunction("getSubStr", [](const auto& args) {
        if (args.size() < 3) return VMValue("");
        auto s = args[0].toString();
        int start = args[1].toInt();
        int count = args[2].toInt();
        if (start < 0 || start >= (int)s.size() || count <= 0) return VMValue("");
        return VMValue(s.substr(start, count));
    });

    vmInstance->registerNativeFunction("strCat", [](const auto& args) {
        std::string result;
        for (auto& a : args) result += a.toString();
        return VMValue(result);
    });

    vmInstance->registerNativeFunction("getWords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        const int first = args.size() > 1 ? std::max(0, args[1].toInt()) : 0;
        const int last = args.size() > 2 ? args[2].toInt() : INT_MAX;
        if (first >= (int)words.size() || last < first) return VMValue("");
        const int end = last == INT_MAX ? (int)words.size()
                                        : std::min((int)words.size(), last + 1);
        std::string result;
        for (int i = first; i < end; ++i) {
            if (!result.empty()) result += ' ';
            result += words[i];
        }
        return VMValue(result);
    });
    vmInstance->registerNativeFunction("getFields", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto fields = splitFields(args[0].toString());
        const int first = args.size() > 1 ? std::max(0, args[1].toInt()) : 0;
        const int last = args.size() > 2 ? args[2].toInt() : INT_MAX;
        if (first >= (int)fields.size() || last < first) return VMValue("");
        const int end = last == INT_MAX ? (int)fields.size()
                                        : std::min((int)fields.size(), last + 1);
        std::string result;
        for (int i = first; i < end; ++i) {
            if (!result.empty()) result += '\t';
            result += fields[i];
        }
        return VMValue(result);
    });
    vmInstance->registerNativeFunction("getWord", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        const int index = args.size() > 1 ? args[1].toInt() : 0;
        return index >= 0 && index < (int)words.size() ? VMValue(words[index]) : VMValue("");
    });
    vmInstance->registerNativeFunction("getWordCount", [](const auto& args) -> VMValue {
        return VMValue((int32_t)splitScriptWords(args.empty() ? "" : args[0].toString()).size());
    });
    vmInstance->registerNativeFunction("firstWord", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        return words.empty() ? VMValue("") : VMValue(words.front());
    });
    vmInstance->registerNativeFunction("setWord", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        std::string s = args[0].toString();
        const int index = args[1].toInt();
        const std::string value = args[2].toString();
        std::string result;
        int count = 0;
        size_t start = 0;
        bool inWord = false;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ' ' || s[i] == '\t') {
                if (inWord) {
                    result += count == index ? value : s.substr(start, i - start);
                    inWord = false;
                    ++count;
                }
                if (i < s.size()) result += s[i];
            } else if (!inWord) {
                inWord = true;
                start = i;
            }
        }
        if (index >= count) {
            if (!result.empty() && result.back() != ' ') result += ' ';
            result += value;
        }
        return VMValue(result);
    });
    vmInstance->registerNativeFunction("restWords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string s = args[0].toString();
        size_t pos = s.find_first_not_of(" \t");
        if (pos == std::string::npos) return VMValue("");
        pos = s.find_first_of(" \t", pos);
        if (pos == std::string::npos) return VMValue("");
        pos = s.find_first_not_of(" \t", pos);
        return pos == std::string::npos ? VMValue("") : VMValue(s.substr(pos));
    });
    vmInstance->registerNativeFunction("getFieldCount", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string s = args[0].toString();
        if (s.empty()) return VMValue(0);
        return VMValue((int32_t)(1 + std::count(s.begin(), s.end(), '\t')));
    });
    vmInstance->registerNativeFunction("strReplace", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        std::string result = args[0].toString();
        const std::string from = args[1].toString();
        const std::string to = args[2].toString();
        if (from.empty()) return VMValue(result);
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
        return VMValue(result);
    });

    vmInstance->registerNativeFunction("stripChars", [](const auto& args) {
        if (args.size() < 2) return VMValue(args.empty() ? "" : args[0].toString());
        std::string s = args[0].toString();
        std::string chars = args[1].toString();
        s.erase(std::remove_if(s.begin(), s.end(), [&](char c) {
            return chars.find(c) != std::string::npos;
        }), s.end());
        return VMValue(s);
    });

    // ─── DTS shape loading functions for script compatibility ────
    vmInstance->registerNativeFunction("loadShape", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        if (name.empty()) return VMValue(0);

        auto& fs = Engine::instance().fs();
        DTSShape shape;
        shape.name = name;

        // Determine if it's an interior or shape
        bool isInterior = false;
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find(".dif") != std::string::npos) isInterior = true;
        shape.isInterior = isInterior;

        // Search for the file
        std::string dir = isInterior ? "interiors/" : "shapes/";
        std::vector<std::string> paths = {
            dir + name,
            dir + name + (isInterior ? ".dif" : ".dts"),
        };
        for (auto& p : paths) {
            auto data = fs.read(p.c_str());
            if (!data.empty()) {
                shape.load(data.data(), data.size());
                break;
            }
        }

        if (shape.loaded) {
            // Add to engine's shape cache (global)
            auto& ren = Engine::instance().renderer();
            ren.addShader(nullptr); // dummy to push shapes idea
            return VMValue(1);
        }
        return VMValue(0);
    });

    vmInstance->registerNativeFunction("getShapePath", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string name = args[0].toString();
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        bool isInterior = (lower.find(".dif") != std::string::npos);
        std::string dir = isInterior ? "interiors/" : "shapes/";
        return VMValue(dir + name);
    });

    vmInstance->registerNativeFunction("isShapeLoaded", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(1);
    });

    // GuiPlayerView::setModel(%shape, %skin) — called as %this.setModel(%shape, %skin)
    // Method dispatch puts objName as args[0], then script args
    vmInstance->registerNativeFunction("setmodel", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string objName = args[0].toString();
        std::string shape = args[1].toString();
        std::string skin = args.size() > 2 ? args[2].toString() : "";
        // Find the GUI control and store the shape/skin
        auto& gui = Engine::instance().guiRenderer();
        auto* ctl = gui.findControl(objName);
        if (ctl) {
            ctl->modelShape = shape;
            ctl->modelSkin = skin;
            ctl->modelYaw = 0.5f;
            ctl->modelPitch = 0.15f;
            Console::instance().printf(LogLevel::Info, "GuiPlayerView: setModel('%s', '%s') on '%s'", shape.c_str(), skin.c_str(), objName.c_str());
            return VMValue(1);
        }
        return VMValue(0);
    });
    vmInstance->registerNativeFunction("setseq", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString())) {
            ctl->modelSequence = std::clamp(args[1].toInt(), -1, 6);
            ctl->modelAnimTime = 0.0f;
            return VMValue(1);
        }
        return VMValue(0);
    });

    // GuiPlayerView::update() — just a no-op trigger; setModel does the work
    // The T2 script defines GMW_PlayerModel::update() which calls setModel internally

    // DSO script compatibility stubs
    vmInstance->registerNativeFunction("t2csri_glue_initChecks", [](const auto&) { return VMValue(1); });
    vmInstance->registerNativeFunction("Base64_CreateArray", [](const auto&) { return VMValue(""); });
    vmInstance->registerNativeFunction("DecToHex", [](const auto& args) {
        if (args.empty()) return VMValue("0");
        char buf[32]; snprintf(buf, sizeof(buf), "%X", args[0].toInt());
        return VMValue(buf);
    });
    vmInstance->registerNativeFunction("DecToBin", [](const auto& args) {
        return VMValue(ScriptConversionParity::decToBin(
            args.empty() ? 0 : args[0].toInt()));
    });
    vmInstance->registerNativeFunction("BinToDec", [](const auto& args) {
        return VMValue(ScriptConversionParity::binToDec(
            args.empty() ? std::string() : args[0].toString()));
    });

    // T2 compatibility stubs (functions called by startup scripts)
    vmInstance->registerNativeFunction("audioSetDriver", [](const auto& args) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        // platformLinux/audio.cc: detect() lists the one OpenAL driver,
        // "None"; setDriver maps "default" to it and rejects other names.
        auto& audio = Engine::instance().audio();
        const std::string driver = args.empty() ? "" : args[0].toString();
        if (driver.empty() || strcasecmp(driver.c_str(), "none") == 0 ||
            strcasecmp(driver.c_str(), "default") == 0) {
            const bool ok = audio.isInitialized() || audio.init();
            if (ok) Console::instance().setVariable("Audio::activeDriver", "None");
            return VMValue(ok ? 1 : 0);
        }
        Console::instance().printf(LogLevel::Error, "Unknown OpenAL audio driver '%s'", driver.c_str());
        return VMValue(0);
#endif
    });
    vmInstance->registerNativeFunction("audioDetect", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isInitialized() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("startAudio", [](const auto&) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        auto& audio = Engine::instance().audio();
        return VMValue(audio.isInitialized() || audio.init() ? 1 : 0);
#endif
    });
    // Render/settings stubs that store values
    auto prefVM = [&](const char* name, const char* prefKey) {
        vmInstance->registerNativeFunction(name, [prefKey](const auto& args) {
            if (!args.empty()) Console::instance().setVariable(prefKey, args[0].toString().c_str());
            return VMValue(1);
        });
    };
    prefVM("setZoomSpeed", "$pref::zoomSpeed");
    prefVM("setShadowDetailLevel", "$pref::shadowDetail");
    prefVM("setOpenGLTextureCompressionHint", "$pref::OpenGL::textureCompressionHint");
    prefVM("setOpenGLSkyMipReduction", "$pref::OpenGL::skyMipReduction");
    prefVM("setOpenGLMipReduction", "$pref::OpenGL::mipReduction");
    prefVM("setOpenGLInteriorMipReduction", "$pref::OpenGL::interiorMipReduction");
    prefVM("setOpenGLAnisotropy", "$pref::OpenGL::anisotropy");
    prefVM("setLogMode", "$pref::logMode");
    prefVM("enableWinConsole", "$pref::winConsole");
    prefVM("setDefaultFov", "$pref::defaultFov");

    // Also register these on tsInstance so the TorqueScript interpreter can find them
    tsInstance->registerNative("audioSetDriver", [](const auto& args) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        // platformLinux/audio.cc: detect() lists the one OpenAL driver,
        // "None"; setDriver maps "default" to it and rejects other names.
        auto& audio = Engine::instance().audio();
        const std::string driver = args.empty() ? "" : args[0].toString();
        if (driver.empty() || strcasecmp(driver.c_str(), "none") == 0 ||
            strcasecmp(driver.c_str(), "default") == 0) {
            const bool ok = audio.isInitialized() || audio.init();
            if (ok) Console::instance().setVariable("Audio::activeDriver", "None");
            return VMValue(ok ? 1 : 0);
        }
        Console::instance().printf(LogLevel::Error, "Unknown OpenAL audio driver '%s'", driver.c_str());
        return VMValue(0);
#endif
    });
    tsInstance->registerNative("audioDetect", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isInitialized() ? 1 : 0);
    });
    tsInstance->registerNative("startAudio", [](const auto&) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        auto& audio = Engine::instance().audio();
        return VMValue(audio.isInitialized() || audio.init() ? 1 : 0);
#endif
    });
    auto prefTS = [&](const char* name, const char* prefKey) {
        tsInstance->registerNative(name, [prefKey](const auto& args) {
            if (!args.empty()) Console::instance().setVariable(prefKey, args[0].toString().c_str());
            return VMValue(1);
        });
    };
    prefTS("setZoomSpeed", "$pref::zoomSpeed");
    prefTS("setShadowDetailLevel", "$pref::shadowDetail");
    prefTS("setOpenGLTextureCompressionHint", "$pref::OpenGL::textureCompressionHint");
    prefTS("setOpenGLSkyMipReduction", "$pref::OpenGL::skyMipReduction");
    prefTS("setOpenGLMipReduction", "$pref::OpenGL::mipReduction");
    prefTS("setOpenGLInteriorMipReduction", "$pref::OpenGL::interiorMipReduction");
    prefTS("setOpenGLAnisotropy", "$pref::OpenGL::anisotropy");
    prefTS("setLogMode", "$pref::logMode");
    prefTS("enableWinConsole", "$pref::winConsole");
    prefTS("setDefaultFov", "$pref::defaultFov");

    // GUI Canvas methods (called as Canvas.pushDialog() etc.)
    // These are registered globally so the dot-notation lookup finds them
    tsInstance->registerNative("pushDialog", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string name = args.back().toString();
            if (!name.empty()) {
                auto& gui = Engine::instance().guiRenderer();
                if (!gui.findControl(name)) {
                    std::string path = "gui/" + name + ".gui";
                    auto data = Engine::instance().fs().read(path.c_str());
                    if (!data.empty()) {
                        auto* ts = Engine::instance().script().ts();
                        if (ts) {
                            ts->executeNested(std::string((const char*)data.data(), data.size()), path);
                            gui.refresh();
                        }
                    }
                }
                gui.pushDialog(name);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("popDialog", [](const auto& args) -> VMValue {
        std::string name = args.empty() ? "" : args.back().toString();
        // Empty name pops everything (GuiRenderer::popDialog "" match).
        Engine::instance().guiRenderer().popDialog(name);
        return VMValue(1);
    });

    // GuiPlayerView::setModel(%shape, %skin) — also register on TS interpreter
    // so .cs scripts (which run through tsInstance, not vmInstance) can call it
    tsInstance->registerNative("setmodel", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string objName = args[0].toString();
        std::string shape = args[1].toString();
        std::string skin = args.size() > 2 ? args[2].toString() : "";
        auto& gui = Engine::instance().guiRenderer();
        auto* ctl = gui.findControl(objName);
        if (ctl) {
            ctl->modelShape = shape;
            ctl->modelSkin = skin;
            ctl->modelYaw = 0.5f;
            ctl->modelPitch = 0.15f;
            Console::instance().printf(LogLevel::Info, "GuiPlayerView: setModel('%s', '%s') on '%s'", shape.c_str(), skin.c_str(), objName.c_str());
            return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("setseq", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString())) {
            ctl->modelSequence = std::clamp(args[1].toInt(), -1, 6);
            ctl->modelAnimTime = 0.0f;
            return VMValue(1);
        }
        return VMValue(0);
    });
    // Mark a GUI control as persistent so setContent preserves it across panel swaps

    // Console functions used by ConsoleDlg.gui (ToggleConsole / ConsoleEntry::eval)
    tsInstance->registerNative("activateKeyboard", [](const auto&) -> VMValue {
        Engine::instance().platform().startTextInput();
        return VMValue(1);
    });
    tsInstance->registerNative("deactivateKeyboard", [](const auto&) -> VMValue {
        Engine::instance().platform().stopTextInput();
        return VMValue(1);
    });
    tsInstance->registerNative("eval", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string code = args[0].toString();
            auto* ts = ScriptEngine::instance().ts();
            if (ts) ts->execute(code, "eval");
        }
        return VMValue(1);
    });
    // Console history
    // GuiCanvas cursor commands (the shipped GuiCanvas): showCursor /
    // hideCursor draw or hide the software cursor, cursorOn / cursorOff turn
    // it on, setCursor names the default GuiCursor.
    auto canvasGui = []() -> GuiRenderer* {
        return Engine::instance().hasGuiRenderer() ? &Engine::instance().guiRenderer() : nullptr;
    };
    tsInstance->registerNative("showCursor", [canvasGui](const auto&) -> VMValue {
        if (auto* gui = canvasGui()) gui->setShowCursor(true);
        return VMValue("");
    });
    tsInstance->registerNative("hideCursor", [canvasGui](const auto&) -> VMValue {
        if (auto* gui = canvasGui()) gui->setShowCursor(false);
        return VMValue("");
    });
    tsInstance->registerNative("GuiCanvas::cursorOn", [canvasGui](const auto&) -> VMValue {
        if (auto* gui = canvasGui()) gui->setCursorOn(true);
        return VMValue("");
    });
    tsInstance->registerNative("GuiCanvas::cursorOff", [canvasGui](const auto&) -> VMValue {
        if (auto* gui = canvasGui()) gui->setCursorOn(false);
        return VMValue("");
    });
    tsInstance->registerNative("GuiCanvas::isCursorOn", [canvasGui](const auto&) -> VMValue {
        auto* gui = canvasGui();
        return VMValue(gui && gui->isCursorOn() ? 1 : 0);
    });
    registerCommanderNatives(*tsInstance);
    tsInstance->registerNative("GuiCanvas::updateCursorState", [canvasGui](const auto&) -> VMValue {
        if (auto* gui = canvasGui()) gui->updateCursorState();
        return VMValue("");
    });
    tsInstance->registerNative("GuiCanvas::setCursor", [canvasGui](const auto& args) -> VMValue {
        const std::string name = args.size() > 1 ? args[1].toString() : std::string();
        std::string key;
        if (!name.empty()) {
            ScriptObject* cursor = ScriptEngine::instance().findObject(name.c_str());
            if (!cursor || !EngineClasses::isA(cursor->className, "GuiCursor")) {
                Console::instance().printf(LogLevel::Info, "%s is not a valid cursor.", name.c_str());
                return VMValue("");
            }
            key = ScriptEngine::instance().objectKey(cursor);
        }
        if (auto* gui = canvasGui()) gui->setDefaultCursor(key);
        return VMValue("");
    });
    tsInstance->registerNative("GuiCanvas::getCursorPos", [](const auto&) -> VMValue {
        if (!Engine::instance().hasGuiRenderer()) return VMValue("0 0");
        auto& plat = Engine::instance().platform();
        int x = 0, y = 0;
        Engine::instance().guiRenderer().mapMouse(plat.input().mouseX, plat.input().mouseY, x, y);
        return VMValue(std::to_string(x) + " " + std::to_string(y));
    });
    tsInstance->registerNative("GuiCanvas::setCursorPos", [](const auto& args) -> VMValue {
        int x = 0, y = 0;
        if (args.size() > 2) {
            x = std::atoi(args[1].toString().c_str());
            y = std::atoi(args[2].toString().c_str());
        } else if (args.size() > 1) {
            std::sscanf(args[1].toString().c_str(), "%d %d", &x, &y);
        }
        Engine::instance().platform().setMousePos(x, y);
        return VMValue("");
    });
    tsInstance->registerNative("enableMouse", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().platform().enableMouse() ? 1 : 0);
    });
    tsInstance->registerNative("disableMouse", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().platform().disableMouse() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("enableMouse", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().platform().enableMouse() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("disableMouse", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().platform().disableMouse() ? 1 : 0);
    });
    tsInstance->registerNative("setContent", [](const auto& args) -> VMValue {
        // May be called directly: setContent("Gui") or as method: Canvas.setContent("Gui")
        // In method form args = ["Canvas", "Gui"], direct form args = ["Gui"]
        if (!args.empty()) {
            std::string name = args.back().toString();
            if (!name.empty()) {
                auto& gui = Engine::instance().guiRenderer();
                if (!gui.findControl(name)) {
                    std::string path = "gui/" + name + ".gui";
                    auto data = Engine::instance().fs().read(path.c_str());
                    auto* ts = Engine::instance().script().ts();
                    if (!data.empty() && ts) {
                        ts->executeNested(std::string((const char*)data.data(), data.size()), path);
                        gui.refresh();
                    }
                }
                gui.setContent(name);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("playGui", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            const std::string name = args.back().toString();
            if (!name.empty()) Engine::instance().guiRenderer().setContent(name);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("Show", [](const auto& args) -> VMValue {
        if (!args.empty())
            if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString())) {
                ctl->visible = true;
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["visible"] = VMValue("1");
            }
        return VMValue(1);
    });
    tsInstance->registerNative("Hide", [](const auto& args) -> VMValue {
        if (!args.empty())
            if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString())) {
                ctl->visible = false;
                if (Engine::instance().guiRenderer().getFocused() == ctl)
                    Engine::instance().guiRenderer().makeFirstResponder(ctl->name, false);
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["visible"] = VMValue("0");
            }
        return VMValue(1);
    });
    tsInstance->registerNative("isActive", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        return VMValue(ctl && ctl->active ? 1 : 0);
    });
    tsInstance->registerNative("makeFirstResponder", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string name = args[0].toString();
        const bool focus = args.size() < 2 || args[1].toBool();
        return VMValue(Engine::instance().guiRenderer().makeFirstResponder(name, focus) ? 1 : 0);
    });

    // File operations needed by startup scripts
    tsInstance->registerNative("isFile", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& fs = Engine::instance().fs();
        const std::string path = args[0].toString();
        if (fs.fileExists(path.c_str())) return VMValue(1);
        const std::string outDir = Console::instance().getStringVariable("outputDir", "");
        const std::string modPath = Console::instance().getStringVariable("modPath", "base");
        return VMValue(!outDir.empty() && std::filesystem::is_regular_file(
            std::filesystem::path(outDir) / modPath / path));
    });
    // consoleFunctions.cc fileExt: from the last '.', dot included.
    tsInstance->registerNative("fileExt", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string path = args[0].toString();
        const size_t dot = path.find_last_of('.');
        return VMValue(dot == std::string::npos ? std::string() : path.substr(dot));
    });
    tsInstance->registerNative("getFileName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        auto slash = path.rfind('/');
        if (slash != std::string::npos) return VMValue(path.substr(slash + 1));
        return VMValue(path);
    });
    tsInstance->registerNative("getFileModifyTime", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue((int32_t)Engine::instance().fs().fileModifyTime(args[0].toString().c_str()));
    });

    // Schedule: store callback for later execution
    // schedule(time, refObject, command, args...): a function call that is
    // cancelled if refObject is deleted.
    tsInstance->registerNative("schedule", [](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            double delay = args[0].toDouble() / 1000.0; // ms to seconds
            std::vector<VMValue> callbackArgs(args.begin() + 3, args.end());
            return VMValue(ScriptEngine::instance().ts()->scheduleEvent(
                SimState::simTime(), delay, args[1].toString(),
                args[2].toString(), callbackArgs));
        }
        return VMValue(0);
    });
    // %obj.schedule(time, method, args...): SimObject::schedule.
    // The shipped SimObject::setPersistent(bool): the object's "persistent"
    // flag (0x200 in mFlags, the mission saver's), set from dAtob.
    // SimSet::isMember / bringToFront / pushToBack (console/simBase.cc).
    auto setMembers = [](ScriptObject* set) {
        std::vector<VMValue> members;
        const int count = set->internals["__childCount"].toInt();
        for (int i = 0; i < count; ++i) members.push_back(set->internals["__child" + std::to_string(i)]);
        return members;
    };
    auto storeMembers = [](ScriptObject* set, const std::vector<VMValue>& members) {
        for (size_t i = 0; i < members.size(); ++i) set->internals["__child" + std::to_string(i)] = members[i];
    };
    tsInstance->registerNative("SimSet::isMember", [setMembers](const auto& args) -> VMValue {
        auto& engine = ScriptEngine::instance();
        ScriptObject* set = args.empty() ? nullptr : engine.findObject(args[0].toString().c_str());
        ScriptObject* test = args.size() > 1 ? engine.findObject(args[1].toString().c_str()) : nullptr;
        if (!test) {
            Console::instance().printf(LogLevel::Info, "SimSet::isMember: %s is not an object.",
                                       args.size() > 1 ? args[1].toString().c_str() : "");
            return VMValue(0);
        }
        if (!set) return VMValue(0);
        for (const auto& member : setMembers(set))
            if (engine.findObject(member.toString().c_str()) == test) return VMValue(1);
        return VMValue(0);
    });
    auto moveMember = [setMembers, storeMembers](const auto& args, bool toFront) -> VMValue {
        auto& engine = ScriptEngine::instance();
        ScriptObject* set = args.empty() ? nullptr : engine.findObject(args[0].toString().c_str());
        ScriptObject* object = args.size() > 1 ? engine.findObject(args[1].toString().c_str()) : nullptr;
        if (!set || !object) return VMValue("");
        auto members = setMembers(set);
        for (size_t i = 0; i < members.size(); ++i)
            if (engine.findObject(members[i].toString().c_str()) == object) {
                const VMValue key = members[i];
                members.erase(members.begin() + (long)i);
                if (toFront) members.insert(members.begin(), key);
                else members.push_back(key);
                storeMembers(set, members);
                break;
            }
        return VMValue("");
    };
    tsInstance->registerNative("SimSet::bringToFront", [moveMember](const auto& args) { return moveMember(args, true); });
    tsInstance->registerNative("SimSet::pushToBack", [moveMember](const auto& args) { return moveMember(args, false); });
    // SimObject::setName: assignName.
    tsInstance->registerNative("SimObject::setName", [](const auto& args) -> VMValue {
        auto& engine = ScriptEngine::instance();
        if (ScriptObject* object = args.empty() ? nullptr : engine.findObject(args[0].toString().c_str()))
            engine.setObjectName(object, args.size() > 1 ? args[1].toString() : std::string());
        return VMValue("");
    });
    // SceneObject::getForwardVector: the transform's y column.
    tsInstance->registerNative("SceneObject::getForwardVector", [](const auto& args) -> VMValue {
        ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
        auto* scene = object ? dynamic_cast<SceneObject*>(object->engine.get()) : nullptr;
        if (!scene) return VMValue("0 1 0");
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", scene->transform[1], scene->transform[5], scene->transform[9]);
        return VMValue(buffer);
    });
    tsInstance->registerNative("SimObject::setPersistent", [](const auto& args) -> VMValue {
        if (ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str()))
            object->internals["__persistent"] = VMValue(args.size() > 1 && args[1].toBool() ? 1 : 0);
        return VMValue("");
    });
    tsInstance->registerNative("SimObject::schedule", [](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            double delay = args[1].toDouble() / 1000.0;
            std::vector<VMValue> callbackArgs(args.begin() + 3, args.end());
            return VMValue(ScriptEngine::instance().ts()->scheduleEvent(
                SimState::simTime(), delay, args[0].toString(),
                args[2].toString(), callbackArgs, true));
        }
        return VMValue(0);
    });
    tsInstance->registerNative("deleteNotify", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        // Method dispatch supplies the target as %this and the listener as
        // the first explicit argument: target.deleteNotify(listener).
        auto* target = ScriptEngine::instance().findObject(args[0].toString().c_str());
        auto* listener = ScriptEngine::instance().findObject(args[1].toString().c_str());
        return VMValue(ScriptEngine::instance().addDeleteNotify(listener, target) ? 1 : 0);
    });
    tsInstance->registerNative("clearNotify", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* target = ScriptEngine::instance().findObject(args[0].toString().c_str());
        auto* listener = ScriptEngine::instance().findObject(args[1].toString().c_str());
        return VMValue(ScriptEngine::instance().clearDeleteNotify(listener, target) ? 1 : 0);
    });
    tsInstance->registerNative("isEventPending", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(ScriptEngine::instance().ts()->isEventPending(args[0].toInt()) ? 1 : 0);
    });

    // File search for .gui discovery
    {
        static std::vector<std::string> s_fileList;
        static size_t s_fileIdx = 0;
        tsInstance->registerNative("findFirstFile", [](const auto& args) -> VMValue {
            s_fileList.clear();
            s_fileIdx = 0;
            if (args.empty()) return VMValue("");
            std::string pattern = args[0].toString();
            Engine::instance().fs().listFiles(pattern.c_str(), s_fileList);
            const std::string outDir = Console::instance().getStringVariable("outputDir", "");
            const std::string modPath = Console::instance().getStringVariable("modPath", "base");
            if (!outDir.empty()) {
                const std::filesystem::path root = std::filesystem::path(outDir) / modPath;
                std::error_code ec;
                if (std::filesystem::exists(root, ec)) {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
                        if (!entry.is_regular_file()) continue;
                        const auto relative = std::filesystem::relative(entry.path(), root, ec).generic_string();
                        if (!ec && fnmatch(pattern.c_str(), relative.c_str(), FNM_PATHNAME) == 0)
                            s_fileList.push_back(relative);
                    }
                }
            }
            Console::instance().printf(LogLevel::Debug, "findFirstFile(\"%s\"): found %zu files, first=\"%s\"", pattern.c_str(), s_fileList.size(), s_fileList.empty() ? "" : s_fileList[0].c_str());
            // Sort to match T2 behavior
            std::sort(s_fileList.begin(), s_fileList.end());
            s_fileList.erase(std::unique(s_fileList.begin(), s_fileList.end()), s_fileList.end());
            return s_fileList.empty() ? VMValue("") : VMValue(s_fileList[0]);
        });
        tsInstance->registerNative("findNextFile", [](const auto&) -> VMValue {
            s_fileIdx++;
            return s_fileIdx < s_fileList.size() ? VMValue(s_fileList[s_fileIdx]) : VMValue("");
        });
    }

    tsInstance->registerNative("WONDisableFutureCalls", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("export", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string pattern = args[0].toString();
        std::string filePath = args[1].toString();
        // T2 passes false/true as STRINGS — toBool() treats any non-empty
        // string (including "False") as true, which made every export APPEND,
        // piling up contradictory pref blocks that fought on next load.
        bool append = false;
        if (args.size() > 2) {
            std::string a = args[2].toString();
            for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
            append = (a == "true" || a == "1");
        }
        // Build full path in outputDir
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (outDir.empty()) return VMValue(0);
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        // export always writes to modPath (not base) — output goes to active mod
        std::string fullPath = outDir + "/" + modPath + "/" + filePath;
        // Create parent directory
        auto slash = fullPath.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = fullPath.substr(0, slash);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        if (fullPath.find("ClientPrefs") != std::string::npos && !clientPrefsExportAllowed()) {
            Console::instance().printf(LogLevel::Info,
                "export: skipping ClientPrefs write (no init script loaded prefs)");
            return VMValue(1);
        }
        FILE* f = fopen(fullPath.c_str(), append ? "a" : "w");
        if (!f) return VMValue(0);
        // Match and write: simple glob pattern with * suffix matching
        // Chop trailing * for prefix match
        std::string prefix = pattern;
        bool prefixMatch = false;
        if (!prefix.empty() && prefix.back() == '*') {
            prefix.pop_back();
            prefixMatch = true;
        }
        Console::instance().forEach([&](const char* name, const Console::ConsoleItem& item) {
            if (item.type != Console::ConsoleItem::Variable) return;
            bool match = prefixMatch
                ? strncasecmp(name, prefix.c_str(), prefix.size()) == 0
                : strcasecmp(name, prefix.c_str()) == 0;
            if (match) {
                fprintf(f, "%s = \"%s\";\n", name, item.value.c_str());
            }
        });
        fclose(f);
        return VMValue(1);
    });

    // exec() - load and execute from modpath, with base fallback
    tsInstance->registerNative("exec", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string execPath = args[0].toString();
        std::string logicalPath = execPath;
        std::replace(logicalPath.begin(), logicalPath.end(), '\\', '/');
        const bool preferenceFile = logicalPath.size() >= 6 &&
            strncasecmp(logicalPath.c_str(), "prefs/", 6) == 0;
        const bool startupOverride = strcasecmp(logicalPath.c_str(), "autoexec.cs") == 0 ||
            strcasecmp(logicalPath.c_str(), "autojournal.cs") == 0;
        const bool optional = preferenceFile || startupOverride ||
            (args.size() > 1 && args[1].toBool());
        auto& fs = Engine::instance().fs();
        // Try bare path first
        auto data = fs.read(execPath.c_str());
        if (data.empty()) data = fs.read(("base/" + execPath).c_str());
        if (data.empty()) data = fs.read(("scripts/" + execPath).c_str());
        // Preferences are exported outside the game data search paths. Resolve
        // them through the active output/mod root after defaults have loaded,
        // preserving the stock script execution order.
        if (data.empty()) {
            std::string outDir = Console::instance().getStringVariable("outputDir", "");
            std::string modPath = Console::instance().getStringVariable("modPath", "base");
            std::filesystem::path current = std::filesystem::path(outDir) / modPath;
            std::string component;
            std::stringstream parts(execPath);
            while (std::getline(parts, component, '/') && !component.empty()) {
                std::filesystem::path match;
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
                    std::string name = entry.path().filename().string();
                    std::string wanted = component;
                    for (char& c : name) c = (char)std::tolower((unsigned char)c);
                    for (char& c : wanted) c = (char)std::tolower((unsigned char)c);
                    if (name == wanted) { match = entry.path(); break; }
                }
                if (match.empty()) { current.clear(); break; }
                current = match;
            }
            if (!current.empty()) {
                std::ifstream pref(current);
                if (pref) data.assign(std::istreambuf_iterator<char>(pref),
                                      std::istreambuf_iterator<char>());
            }
        }
        if (!data.empty()) {
            std::string src((const char*)data.data(), data.size());
            auto* ts = Engine::instance().script().ts();
            if (ts) { ts->executeNested(src, execPath); }
        } else {
            auto* ts = Engine::instance().script().ts();
            const std::string function = ts ? ts->currentFunction() : std::string();
            const std::string file = ts && !ts->dbgFile().empty() ? ts->dbgFile() : "<runtime>";
            const std::string where = "TS:" + file +
                (function.empty() ? ":" + std::to_string(ts ? ts->dbgLine() : 0)
                                  : " [" + function + "]");
            Console::instance().printf(optional ? LogLevel::Debug : LogLevel::Error,
                "%s: %s exec file not found: %s", where.c_str(),
                optional ? "optional" : "required", execPath.c_str());
            return VMValue(optional ? 1 : 0);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("autoExec", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string path = args[0].toString();
        auto data = Engine::instance().fs().read(path.c_str());
        if (data.empty()) data = Engine::instance().fs().read(("base/" + path).c_str());
        if (data.empty()) data = Engine::instance().fs().read(("scripts/" + path).c_str());
        if (data.empty()) return VMValue(0);
        auto* ts = Engine::instance().script().ts();
        if (!ts) return VMValue(0);
        ts->executeNested(std::string((const char*)data.data(), data.size()), path);
        return VMValue(1);
    });
    tsInstance->registerNative("addMessageCallback", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            std::string msgType = args[0].toString();
            std::string callback = args[1].toString();
            if (auto* ts = Engine::instance().script().ts())
                ts->registerMessageCallback(msgType, callback);
            Console::instance().printf(LogLevel::Debug, "addMessageCallback: type='%s' callback='%s'", msgType.c_str(), callback.c_str());
        }
        return VMValue(1);
    });


    // nameToID: the id of a named (or numbered) object, -1 when none.
    tsInstance->registerNative("nameToId", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(-1);
        auto& engine = ScriptEngine::instance();
        const std::string handle = args[0].toString();
        for (const auto& object : engine.missionObjects())
            if (!object.name.empty() && strcasecmp(object.name.c_str(), handle.c_str()) == 0)
                return VMValue(object.id);
        if (auto* object = engine.findObject(handle.c_str())) return VMValue(engine.objectId(object));
        return VMValue(-1);
    });

    tsInstance->registerNative("getRecord", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(std::string(""));
        std::string record = args[0].toString();
        int idx = args[1].toInt();
        // Tab-delimited record: get field at index
        size_t start = 0;
        for (int i = 0; i < idx && start != std::string::npos; i++) {
            start = record.find('\t', start);
            if (start != std::string::npos) start++;
        }
        if (start == std::string::npos) return VMValue(std::string(""));
        size_t end = record.find('\t', start);
        if (end == std::string::npos) end = record.size();
        return VMValue(record.substr(start, end - start));
    });

    tsInstance->registerNative("getRecordCount", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string record = args[0].toString();
        int count = 1;
        for (size_t i = 0; i < record.size(); i++) {
            if (record[i] == '\t') count++;
        }
        return VMValue(count);
    });

    tsInstance->registerNative("wonGetAuthInfo", [](const auto&) -> VMValue {
        return VMValue(std::string(""));
    });

    // Track named audio sources (name -> source mapping for alxCreateSource)
    static std::unordered_map<std::string, SoundSource*> s_audioSources;
    static std::unordered_map<std::string, SoundBuffer*> s_audioBuffers;
    static int s_nextAudioHandle = 1;

    auto audioProfilePath = [](const std::string& profile) -> std::string {
        auto* object = ScriptEngine::instance().findObject(profile.c_str());
        if (!object || object->className != "AudioProfile") return {};
        auto it = object->fields.find("filename");
        if (it == object->fields.end()) return {};
        std::string path = it->second.toString();
        if (path.empty()) return {};
        if (path.find('.') == std::string::npos) path += ".wav";
        if (path.rfind("audio/", 0) != 0 && path.rfind("sound/", 0) != 0)
            path = "audio/" + path;
        return path;
    };
    auto audioProfileSettings = [](const std::string& profile, SoundSource& source) {
        auto* object = ScriptEngine::instance().findObject(profile.c_str());
        if (!object || object->className != "AudioProfile") return;
        auto field = [object](const char* name, const char* fallback = nullptr) -> VMValue {
            auto it = object->fields.find(name);
            if (it != object->fields.end()) return it->second;
            if (fallback) {
                it = object->fields.find(fallback);
                if (it != object->fields.end()) return it->second;
            }
            return VMValue();
        };
        std::string description = field("description", "audioDescription").toString();
        if (auto* desc = ScriptEngine::instance().findObject(description.c_str());
            desc && desc->className == "AudioDescription") {
            auto value = [desc](const char* name) -> VMValue {
                auto it = desc->fields.find(name);
                return it == desc->fields.end() ? VMValue() : it->second;
            };
            auto volume = desc->fields.find("volume");
            if (volume != desc->fields.end()) source.setVolume((float)volume->second.toDouble());
            const bool looping = value("isLooping").toBool() || value("looping").toBool();
            source.setLooping(looping);
            if (looping && (desc->fields.find("loopCount") != desc->fields.end() ||
                            desc->fields.find("minLoopGap") != desc->fields.end() ||
                            desc->fields.find("maxLoopGap") != desc->fields.end())) {
                source.setLoopSchedule(value("loopCount").toInt(),
                                       value("minLoopGap").toInt(),
                                       value("maxLoopGap").toInt());
            }
            if (value("is3D").toBool() || value("is3d").toBool())
                source.setDistance((float)value("referenceDistance").toDouble(),
                                   (float)value("maxDistance").toDouble());
        }
        auto value = [object](const char* name) -> VMValue {
            auto it = object->fields.find(name);
            return it == object->fields.end() ? VMValue() : it->second;
        };
        if (object->fields.find("volume") != object->fields.end())
            source.setVolume((float)value("volume").toDouble());
        if (value("looping").toBool()) source.setLooping(true);
        if (value("is3D").toBool() || value("is3d").toBool()) {
            source.setDistance((float)value("referenceDistance").toDouble(),
                               (float)value("maxDistance").toDouble());
            source.setPosition(source.position);
        }
    };
    auto purgeAudioHandle = [](const std::string& handle) {
        auto it = s_audioSources.find(handle);
        if (it != s_audioSources.end() && !Engine::instance().audio().isSourceAlive(it->second)) {
            s_audioSources.erase(it);
            s_audioBuffers.erase(handle);
        }
    };
    tsInstance->registerNative("cleanupAudio", [](const auto&) -> VMValue {
        auto& audio = Engine::instance().audio();
        if (audio.isInitialized()) audio.shutdown();
        // shutdown destroys every source, so no script handle may retain one.
        s_audioSources.clear();
        s_audioBuffers.clear();
        s_nextAudioHandle = 1;
        return VMValue(1);
    });
    tsInstance->registerNative("alxPlay", [audioProfilePath, audioProfileSettings, purgeAudioHandle](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        const std::string profileName = name;
        // Numeric handle from alxCreateSource → play that source directly
        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
            auto it = s_audioSources.find(name);
            if (it != s_audioSources.end()) {
                purgeAudioHandle(name);
                it = s_audioSources.find(name);
                if (it == s_audioSources.end()) return VMValue(0);
                auto bit = s_audioBuffers.find(name);
                if (bit != s_audioBuffers.end()) it->second->play(bit->second);
                return VMValue(atoi(name.c_str()));
            }
        }
        std::string profilePath = audioProfilePath(name);
        if (!profilePath.empty()) name = profilePath;
        // Try creating a one-shot source from the profile path or sound/Name.
        auto& audio = Engine::instance().audio();
        auto loadAndPlay = [&](const std::string& path) -> VMValue {
            SoundBuffer* buf = audio.loadSound(path.c_str());
            if (!buf) return VMValue(0);
            SoundSource* src = audio.createSource();
            if (!src) return VMValue(0);
            audioProfileSettings(profileName, *src);
            src->play(buf);
            int h = s_nextAudioHandle++;
            s_audioSources[std::to_string(h)] = src;
            s_audioBuffers[std::to_string(h)] = buf;
            return VMValue(h);
        };
        if (profilePath.empty() == false) {
            VMValue handle = loadAndPlay(name);
            if (handle.toInt() != 0) return handle;
        }
        if (name.find('/') != std::string::npos) {
            VMValue handle = loadAndPlay(name);
            if (handle.toInt() != 0) return handle;
        }
        // Map T2 sound names to files
        std::string path = "sound/" + name + ".wav";
        VMValue handle = loadAndPlay(path);
        if (handle.toInt() != 0) return handle;
        path = "sound/" + name + ".ogg";
        handle = loadAndPlay(path);
        if (handle.toInt() != 0) return handle;
        // Try lowercase variants
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        path = "sound/" + name + ".wav";
        handle = loadAndPlay(path);
        if (handle.toInt() != 0) return handle;
        path = "sound/" + name + ".ogg";
        handle = loadAndPlay(path);
        if (handle.toInt() != 0) return handle;
        return VMValue(0);
    });

    tsInstance->registerNative("GetIRCServerList", [](const auto&) -> VMValue {
        return VMValue(0);
    });

    // compile(path) — compile a .cs/.gui file to .dso for caching
    tsInstance->registerNative("compile", [this](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string path = args[0].toString();
        std::string ext = path.size() > 3 ? path.substr(path.size() - 3) : "";
        if (ext != ".cs" && ext != ".gui") return VMValue(0);
        auto data = Engine::instance().fs().read(path.c_str());
        if (data.empty()) return VMValue(0);
        std::string src((const char*)data.data(), data.size());
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        if (outDir.empty()) return VMValue(0);
        std::string dsoPath = outDir + "/" + modPath + "/" + path + ".dso";
        // Invoke the external compiler without a shell. Failed compilation
        // must not create a valid-looking empty cache that hides source code.
        const auto dir = dsoPath.substr(0, dsoPath.rfind('/'));
        std::error_code fsError;
        std::filesystem::create_directories(dir, fsError);
        if (fsError) return VMValue(0);
        std::string tmpPath = dsoPath + ".src.tmp";
        { FILE* f = fopen(tmpPath.c_str(), "w"); if (f) { fwrite(src.data(), 1, src.size(), f); fclose(f); } }
        if (!std::filesystem::exists(tmpPath)) return VMValue(0);
        std::string cmd = Console::instance().getStringVariable("nodePath");
        if (cmd.empty()) cmd = "node";
        std::string compilerScript = Console::instance().getStringVariable("compilerScript");
        if (compilerScript.empty())
            compilerScript = Console::instance().getStringVariable("$compilerScript");
        if (compilerScript.empty()) compilerScript = "torque-dso.js";
        std::vector<char*> compilerArgv;
        compilerArgv.push_back(const_cast<char*>(cmd.c_str()));
        compilerArgv.push_back(const_cast<char*>(compilerScript.c_str()));
        compilerArgv.push_back(const_cast<char*>(tmpPath.c_str()));
        compilerArgv.push_back(const_cast<char*>(dsoPath.c_str()));
        static std::string target = "Tribes2";
        compilerArgv.push_back(const_cast<char*>(target.c_str()));
        compilerArgv.push_back(nullptr);
        pid_t pid = 0;
        const int spawnResult = posix_spawnp(&pid, cmd.c_str(), nullptr, nullptr,
                                             compilerArgv.data(), environ);
        int status = 0;
        const int waitResult = spawnResult == 0 ? waitpid(pid, &status, 0) : -1;
        unlink(tmpPath.c_str());
        if (spawnResult != 0 || waitResult < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0 || !std::filesystem::exists(dsoPath))
            return VMValue(0);
        if (!tsInstance->writeCompileDependencyManifest(dsoPath, path))
            return VMValue(0);
        return VMValue(1);
    });

    // Missing startup function stubs
    tsInstance->registerNative("activateDirectInput", [](const auto&) -> VMValue {
        Engine::instance().platform().setRelativeMouse(true);
        Engine::instance().platform().showMouse(false);
        return VMValue(1);
    });
    tsInstance->registerNative("deactivateDirectInput", [](const auto&) -> VMValue {
        Engine::instance().platform().setRelativeMouse(false);
        Engine::instance().platform().showMouse(true);
        return VMValue(1);
    });
    tsInstance->registerNative("setNetPort", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().setVariable("Pref::Net::Port", args[0].toInt());
            Console::instance().printf(LogLevel::Debug, "setNetPort: %d", args[0].toInt());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("queryMasterGameTypes", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("cancel", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(ScriptEngine::instance().ts()->cancelEvent(args[0].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("cancelEvent", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(ScriptEngine::instance().ts()->cancelEvent(args[0].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("cls", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    // Common GUI control method stubs
    auto getListCtrl = [](const std::string& name) -> GuiControl* {
        auto& g = Engine::instance().guiRenderer();
        auto* ctl = g.findControl(name);
        if (!ctl && ScriptEngine::instance().findObject(name.c_str()))
            ctl = g.soToGui(name, nullptr);
        return ctl;
    };
    tsInstance->registerNative("clear", [getListCtrl](const auto& args) -> VMValue {
        std::string cname = args.empty() ? "" : args[0].toString();
        auto* ctl = getListCtrl(cname);
        if (ctl) {
            ctl->listRows.clear();
            ctl->listRowIds.clear();
            ctl->selectedRow = -1;
            ctl->menuItems.clear();
            if (ctl->className == "GuiTreeView") {
                ctl->treeItems.clear();
                ctl->selectedTreeItem = 0;
                ctl->nextTreeItemId = 1;
            }
        } else if (auto* obj = ScriptEngine::instance().findObject(cname.c_str());
                   obj && obj->className == "MessageVector") {
            obj->internals.clear();
            obj->internals["__lineCount"] = VMValue(0);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("add", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* group = ScriptEngine::instance().findObject(args[0].toString().c_str());
            auto* childObject = ScriptEngine::instance().findObject(args[1].toString().c_str());
            if (group && childObject && ScriptEngine::instance().isSimSet(group)) {
                // A declared (nested) parent link gives way to real membership.
                childObject->internals.erase("parent");
                ScriptEngine::instance().addToSet(group, childObject);
                return VMValue(1);
            }
        }
        if (args.size() >= 2) {
            auto* parent = getListCtrl(args[0].toString());
            auto* child = getListCtrl(args[1].toString());
            if (parent && child && child != parent) parent->addChild(child);
            if (args.size() == 2) return VMValue(1);
        }
        if (args.size() < 3) return VMValue(1);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(1);
        // Blank display names (interpreter string-lifetime bug during
        // first-pass cascades) render as unselectable blank rows — skip.
        bool popupMenu = ctl->className.find("PopUpMenu") != std::string::npos;
        if (popupMenu && args[1].toString().empty()) return VMValue(1);
        // ShellLaunchMenu uses add(id, text), popup menus use add(text, id)
        std::string txt;
        int id;
        if (ctl->className == "ShellLaunchMenu") {
            id = (int)args[1].toDouble();
            txt = args[2].toString();
        } else {
            txt = args[1].toString();
            id = (int)args[2].toDouble();
        }
        ctl->menuItems.push_back({id, txt, false});
        return VMValue(1);
    });
    tsInstance->registerNative("remove", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(1);
        auto* group = ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (group && ScriptEngine::instance().isSimSet(group)) {
            auto* child = ScriptEngine::instance().findObject(args[1].toString().c_str());
            return VMValue(ScriptEngine::instance().removeFromSet(group, child) ? 1 : 0);
        }
        auto* parent = getListCtrl(args[0].toString());
        auto* child = getListCtrl(args[1].toString());
        if (!parent || !child) return VMValue(1);
        auto it = std::find(parent->children.begin(), parent->children.end(), child);
        if (it != parent->children.end()) {
            parent->children.erase(it);
            child->parent = nullptr;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addSeparator", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(1);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(1);
        ctl->menuItems.push_back({0, "", true});
        return VMValue(1);
    });

    auto getOrCreateCtrl = [](const std::string& name) -> GuiControl* {
        auto& g = Engine::instance().guiRenderer();
        auto* ctl = g.findControl(name);
        if (!ctl && ScriptEngine::instance().findObject(name.c_str())) {
            ctl = g.soToGui(name, nullptr);
        }
        return ctl;
    };

    tsInstance->registerNative("addLaunchTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        // args[0] = %this (object name), args[1] = text, args[2] = gui, args[3] = makeInactive
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getOrCreateCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        std::string txt = args[1].toString();
        std::string gui = args[2].toString();
        bool makeInactive = args.size() > 3 && args[3].toDouble() != 0;
        int id = (int)ctl->tabs.size();
        ctl->tabs.push_back({txt, !makeInactive});
        auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
        if (sobj) {
            std::string gk = "gui[" + std::to_string(id) + "]";
            sobj->fields[gk] = VMValue(gui);
            std::string kk = "key[" + std::to_string(id) + "]";
            sobj->fields[kk] = VMValue("0");
        }
        return VMValue(1);
    });

    tsInstance->registerNative("viewLastTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || ctl->tabs.empty()) return VMValue(0);
        int idx = (int)ctl->tabs.size() - 1;
        ctl->selectedTab = idx;
        auto& gr = Engine::instance().guiRenderer();
        for (int i = 0; i < (int)ctl->tabs.size(); ++i) {
            if (ctl->tabs[i].active) {
                ctl->selectedTab = i;
                auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
                if (sobj) {
                    std::string gk = "gui[" + std::to_string(i) + "]";
                    auto gi = sobj->fields.find(gk);
                    if (gi != sobj->fields.end() && !gi->second.toString().empty()) {
                        gr.setContent(gi->second.toString());
                        return VMValue(1);
                    }
                }
            }
        }
        return VMValue(1);
    });

    tsInstance->registerNative("closeCurrentTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || ctl->selectedTab < 0 || ctl->selectedTab >= (int)ctl->tabs.size()) return VMValue(0);
        ctl->tabs.erase(ctl->tabs.begin() + ctl->selectedTab);
        if (ctl->selectedTab >= (int)ctl->tabs.size())
            ctl->selectedTab = (int)ctl->tabs.size() - 1;
        return VMValue(1);
    });

    tsInstance->registerNative("closeTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getOrCreateCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        std::string gui = args[1].toString();
        std::string key = args[2].toString();
        int idx = -1;
        auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
        if (sobj) {
            for (int i = 0; i < (int)ctl->tabs.size(); ++i) {
                std::string gk = "gui[" + std::to_string(i) + "]";
                std::string kk = "key[" + std::to_string(i) + "]";
                auto gi = sobj->fields.find(gk);
                auto ki = sobj->fields.find(kk);
                if (gi != sobj->fields.end() && ki != sobj->fields.end() &&
                    gi->second.toString() == gui && ki->second.toString() == key) {
                    idx = i;
                    break;
                }
            }
        }
        if (idx < 0 || idx >= (int)ctl->tabs.size()) return VMValue(0);
        ctl->tabs.erase(ctl->tabs.begin() + idx);
        if (ctl->selectedTab >= (int)ctl->tabs.size())
            ctl->selectedTab = (int)ctl->tabs.size() - 1;
        return VMValue(1);
    });

    tsInstance->registerNative("removeTabByIndex", [getOrCreateCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getOrCreateCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        int idx = (int)args[1].toDouble();
        if (idx < 0 || idx >= (int)ctl->tabs.size()) return VMValue(0);
        ctl->tabs.erase(ctl->tabs.begin() + idx);
        if (ctl->selectedTab >= (int)ctl->tabs.size())
            ctl->selectedTab = (int)ctl->tabs.size() - 1;
        if (auto* ts = Engine::instance().script().ts()) {
            if (ts->hasFunction(ctl->name + "::onSelect") && ctl->selectedTab >= 0)
                ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(ctl->selectedTab), VMValue(ctl->tabs[ctl->selectedTab].text)});
        }
        return VMValue(1);
    });

    tsInstance->registerNative("addTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        std::string cname = args.empty() ? "" : args[0].toString();
        auto* ctl = getOrCreateCtrl(cname);
        if (ctl && args.size() >= 3) {
            int id = (int)args[1].toDouble();
            if (id < 0 || id > 65535) return VMValue(1); // reject absurd indices (alloc/overflow guard)
            std::string txt = args[2].toString();
            if (id >= (int)ctl->tabs.size()) ctl->tabs.resize(id + 1);
            ctl->tabs[id] = {txt, true, id};
            // Optional set argument: GM_TabView.addTab(3, "WARRIOR SETUP", 1)
            // puts that tab in an alternate skin set
            if (args.size() >= 4)
                ctl->tabs[id].set = (int)args[3].toDouble();
            // Store gui[i]/key[i] derived from tab text (only if script hasn't already stored them)
            if (ScriptObject* sobj = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
                std::string gk = "gui[" + std::to_string(id) + "]";
                if (sobj->fields.find(gk) == sobj->fields.end()) {
                    sobj->fields[gk] = VMValue(txt);
                    sobj->fields["key[" + std::to_string(id) + "]"] = VMValue("0");
                }
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("tabCount", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        return VMValue((int)(ctl ? ctl->tabs.size() : 0));
    });
    // ShellTabGroupCtrl tab sets: addSet(id, bitmapBase, ...) registers an
    // alternate skin; getTabSet(tabId) reports which set a tab belongs to
    // (GameGui.cs: GM_TabView.addSet(1,"gui/shll_horztabbuttonB",...) /
    // GM_TabFrame.setAltColor(%this.getTabSet(%id) != 0))
    tsInstance->registerNative("addSet", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 3)
            ctl->tabSets[(int)args[1].toDouble()] = args[2].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("getTabSet", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(0);
        int id = (int)args[1].toDouble();
        return VMValue(id >= 0 && id < (int)ctl->tabs.size() ? ctl->tabs[id].set : 0);
    });
    tsInstance->registerNative("getSelectedTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        return VMValue(ctl ? ctl->selectedTab : -1);
    });
    tsInstance->registerNative("setSelectedByIndex", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            int idx = (int)args[1].toDouble();
            ctl->selectedTab = idx;
            if (idx >= 0 && idx < (int)ctl->tabs.size()) {
                auto* ts = Engine::instance().script().ts();
                bool has = ts && ts->hasFunction(ctl->name + "::onSelect");
                if (ts && has)
                    ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(ctl->tabs[idx].id), VMValue(ctl->tabs[idx].text)});
            }
        }
        return VMValue(1);
    });
    // setSelected is the T2 alias for setSelectedByIndex.
    // For popup menus: set selectedRow and update displayed text.
    // For tab controls: set selectedTab and trigger onSelect.
    tsInstance->registerNative("setSelected", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            int idx = (int)args[1].toDouble();
            // Popup menu: stock setSelected is ID-based (ids survive sort)
            if (!ctl->menuItems.empty()) {
                int row = -1;
                for (int i = 0; i < (int)ctl->menuItems.size(); i++)
                    if (ctl->menuItems[i].id == idx) { row = i; break; }
                if (row == -1 && idx >= 0 && idx < (int)ctl->menuItems.size() && ctl->menuItems[idx].id == idx)
                    row = idx; // sequential ids: positional == id
                if (row != -1) {
                    ctl->selectedRow = row;
                    ctl->text = ctl->menuItems[row].text;
                }
            }
            // Tab control: update selected tab
            ctl->selectedTab = idx;
            if (idx >= 0 && idx < (int)ctl->tabs.size()) {
                auto* ts = Engine::instance().script().ts();
                bool has = ts && ts->hasFunction(ctl->name + "::onSelect");
                if (ts && has)
                    ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(ctl->tabs[idx].id), VMValue(ctl->tabs[idx].text)});
                return VMValue(1);
            }
            // Popup menu: stock T2's ShellPopupMenu::setSelected only
            // updates the display — it does NOT fire name::onSelect.
            // Script callbacks fire solely from real user clicks (the
            // renderer's hit-test path).  Firing here re-triggered
            // ::onSelect cascades during GUI wake (e.g. RaceGender
            // setSelected inside WarriorPopup::onSelect), which re-filled
            // lists and wrote the first-scanned skin over the player's
            // saved preference on every startup.
        }
        return VMValue(1);
    });

    // viewTab: args[0]=%this, args[1]=text, args[2]=gui, args[3]=key
    tsInstance->registerNative("viewTab", [getOrCreateCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getOrCreateCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        std::string text = args[1].toString();
        std::string guiArg = args.size() > 2 ? args[2].toString() : "";
        std::string keyArg = args.size() > 3 ? args[3].toString() : "";
        int foundIdx = -1;
        if (!text.empty()) {
            for (int i = 0; i < (int)ctl->tabs.size(); ++i) {
                if (ctl->tabs[i].text == text) { foundIdx = i; break; }
            }
        }
        if (foundIdx < 0 && !guiArg.empty()) {
            auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
            if (sobj) {
                for (int i = 0; i < (int)ctl->tabs.size(); ++i) {
                    std::string gk = "gui[" + std::to_string(i) + "]";
                    std::string kk = "key[" + std::to_string(i) + "]";
                    auto gi = sobj->fields.find(gk);
                    auto ki = sobj->fields.find(kk);
                    if (gi != sobj->fields.end() && ki != sobj->fields.end() &&
                        gi->second.toString() == guiArg && ki->second.toString() == keyArg) {
                        foundIdx = i;
                        break;
                    }
                }
            }
        }
        if (foundIdx < 0 || foundIdx >= (int)ctl->tabs.size()) return VMValue(0);
        ctl->selectedTab = foundIdx;
        auto* ts = Engine::instance().script().ts();
        if (ts && ts->hasFunction(ctl->name + "::onSelect"))
            ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(foundIdx), VMValue(ctl->tabs[foundIdx].text)});
        return VMValue(1);
    });


    // viewLastTab and closeCurrentTab have script implementations; let them run.
    tsInstance->registerNative("isTabActive", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            int idx = (int)args[1].toDouble();
            if (idx >= 0 && idx < (int)ctl->tabs.size()) return VMValue(ctl->tabs[idx].active ? 1 : 0);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setTabActive", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 3) {
            int idx = (int)args[1].toDouble();
            bool act = args[2].toBool();
            if (idx >= 0 && idx < (int)ctl->tabs.size()) ctl->tabs[idx].active = act;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("getSelected", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(0);
        // Popups report the selected entry's ID (ids survive sorting)
        if (!ctl->menuItems.empty()) {
            if (ctl->selectedRow >= 0 && ctl->selectedRow < (int)ctl->menuItems.size()) {
                return VMValue(ctl->menuItems[ctl->selectedRow].id);
            }
            return VMValue(-1);
        }
        return VMValue(ctl->selectedRow);
    });
    tsInstance->registerNative("getSelectedId", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(0);
        // For text lists, return the logical id of the selected row (the id
        // passed to addRow, e.g. the global mission id) rather than the row
        // index — stock T2 lookups (e.g. $HostMissionFile[<id>]) use this id.
        if (ctl->selectedRow >= 0 && ctl->selectedRow < (int)ctl->listRowIds.size())
            return VMValue(ctl->listRowIds[ctl->selectedRow]);
        return VMValue(ctl->selectedRow < 0 ? 0 : ctl->selectedRow);
    });
    tsInstance->registerNative("setSelectedById", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(0);
        int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); i++) {
            if (ctl->listRowIds[i] == id) { ctl->selectedRow = (int)i; return VMValue(1); }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getValue", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl) {
            if (ctl->className == "GuiRadioCtrl" || ctl->className == "ShellRadioButton" || ctl->className == "GuiCheckBoxCtrl")
                return VMValue((double)(ctl->checked ? 1 : 0));
            if (ctl->selectedRow >= 0 && ctl->selectedRow < (int)ctl->listRows.size())
                return VMValue(ctl->listRows[ctl->selectedRow]);
            return VMValue(ctl->text);
        }
        return VMValue(std::string(""));
    });
    tsInstance->registerNative("setSelectedRow", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) ctl->selectedRow = args[1].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("getRowTextById", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(std::string(""));
        int id = args[1].toInt();
        // Search menuItems first (popup menus), then listRows (text lists)
        for (auto& item : ctl->menuItems)
            if (item.id == id) return VMValue(item.text);
        for (size_t i = 0; i < ctl->listRowIds.size(); i++)
            if (ctl->listRowIds[i] == id) return VMValue(ctl->listRows[i]);
        return VMValue(std::string(""));
    });
    // getRowText(index) — return text of row at index
    tsInstance->registerNative("getRowText", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(std::string(""));
        int row = args[1].toInt();
        if (row >= 0 && row < (int)ctl->listRows.size())
            return VMValue(ctl->listRows[row]);
        return VMValue(std::string(""));
    });
    // rowCount() — return total number of rows
    tsInstance->registerNative("rowCount", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(0);
        if (!ctl->menuItems.empty()) return VMValue((int32_t)ctl->menuItems.size());
        return VMValue((int32_t)ctl->listRows.size());
    });
    tsInstance->registerNative("getRowCount", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        return ctl ? VMValue((int32_t)ctl->listRows.size()) : VMValue(0);
    });
    // getText() — return the currently selected/displayed text
    tsInstance->registerNative("getText", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(std::string(""));
        return VMValue(ctl->text);
    });
    // getTextById(id) — return text of item with matching id
    tsInstance->registerNative("getTextById", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(std::string(""));
        int id = args[1].toInt();
        for (auto& item : ctl->menuItems)
            if (item.id == id) return VMValue(item.text);
        for (size_t i = 0; i < ctl->listRowIds.size(); i++)
            if (ctl->listRowIds[i] == id) return VMValue(ctl->listRows[i]);
        return VMValue(std::string(""));
    });
    tsInstance->registerNative("size", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(0);
        // Check menuItems first (popup menus), then listRows (text lists)
        if (!ctl->menuItems.empty()) return VMValue((int32_t)ctl->menuItems.size());
        return VMValue((int32_t)ctl->listRows.size());
    });
    tsInstance->registerNative("findText", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(0);
        std::string search = args[1].toString();
        // Search menuItems first (popup menus), then listRows (text lists)
        for (int i = 0; i < (int)ctl->menuItems.size(); i++)
            if (ctl->menuItems[i].text == search) return VMValue(ctl->menuItems[i].id);
        for (int i = 0; i < (int)ctl->listRows.size(); i++)
            if (ctl->listRows[i] == search) return VMValue(i);
        return VMValue(-1);
    });
    tsInstance->registerNative("scrollToTag", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            std::string tag = args[1].toString();
            for (size_t i = 0; i < ctl->listRows.size(); i++) {
                if (ctl->listRows[i].find(tag) != std::string::npos) {
                    ctl->selectedRow = (int)i;
                    ctl->scrollY = (float)i * 20.0f;
                    break;
                }
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setVisible", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->visible = args[1].toBool();
                if (!ctl->visible && Engine::instance().guiRenderer().getFocused() == ctl)
                    Engine::instance().guiRenderer().makeFirstResponder(ctl->name, false);
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["visible"] = VMValue(ctl->visible ? "1" : "0");
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("isVisible", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        return VMValue(ctl ? (ctl->visible ? 1 : 0) : 0);
    });
    tsInstance->registerNative("getName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string objectName = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.name);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return VMValue(scriptObjectName(object));
        return VMValue(objectName);
    });
    tsInstance->registerNative("getClassName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string objectName = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.className);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return VMValue(object->className);
        return VMValue("");
    });
    tsInstance->registerNative("getShapeName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string objectName = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.shapeName);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return object->fields.count("shapeName") ? object->fields["shapeName"] :
                object->fields["shapeFile"];
        return VMValue("");
    });
    tsInstance->registerNative("getSkinName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string objectName = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.skinName);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return object->fields.count("skin") ? object->fields["skin"] :
                object->fields["skinName"];
        return VMValue("");
    });
    tsInstance->registerNative("getProfileName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string objectName = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.profileName);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return object->fields.count("profile") ? object->fields["profile"] :
                object->fields["profileName"];
        return VMValue("");
    });
    auto inventoryObject = [](const std::vector<VMValue>& args) -> ScriptObject* {
        return args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
    };
    tsInstance->registerNative("getInventory", [inventoryObject](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const auto state = ScriptEngine::instance().loadoutState();
        const auto item = state.inventory.find(args[1].toString());
        if (state.hasInventory)
            return VMValue(item == state.inventory.end() ? 0 : item->second);
        auto* object = inventoryObject(args);
        if (!object) return VMValue(0);
        return object->fields["inventory::" + args[1].toString()];
    });
    tsInstance->registerNative("setInventory", [inventoryObject](const auto& args) -> VMValue {
        if (args.size() < 3 || args[1].toString().empty()) return VMValue(0);
        (void)inventoryObject;
        return VMValue(ScriptEngine::instance().mutateInventory(
            args[0].toInt(), args[1].toString(), args[2].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("incInventory", [inventoryObject](const auto& args) -> VMValue {
        if (args.size() < 3 || args[1].toString().empty()) return VMValue(0);
        (void)inventoryObject;
        return VMValue(ScriptEngine::instance().mutateInventory(
            args[0].toInt(), args[1].toString(), args[2].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("decInventory", [inventoryObject](const auto& args) -> VMValue {
        if (args.size() < 3 || args[1].toString().empty()) return VMValue(0);
        (void)inventoryObject;
        return VMValue(ScriptEngine::instance().mutateInventory(
            args[0].toInt(), args[1].toString(), -args[2].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("maxInventory", [inventoryObject](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const auto state = ScriptEngine::instance().loadoutState();
        const auto item = state.maxInventory.find(args[1].toString());
        if (state.hasInventory)
            return VMValue(item == state.maxInventory.end() ? 0 : item->second);
        auto* object = inventoryObject(args);
        if (!object) return VMValue(0);
        return object->fields["maxInventory::" + args[1].toString()];
    });
    auto loadout = [](const auto& args) {
        const auto state = ScriptEngine::instance().loadoutState();
        const int slot = args.empty() ? -1 : args[0].toInt();
        auto weapon = state.weaponAmmo.find(slot);
        return std::pair<ScriptLoadoutState, int>(state,
            weapon == state.weaponAmmo.end() ? -1 : weapon->second);
    };
    tsInstance->registerNative("getWeaponAmmo", [loadout](const auto& args) -> VMValue {
        return VMValue(loadout(args).second);
    });
    tsInstance->registerNative("getAmmo", [loadout](const auto& args) -> VMValue {
        const auto state = ScriptEngine::instance().loadoutState();
        return VMValue(state.hasWeapons ? state.ammo : -1);
    });
    tsInstance->registerNative("getCurrentWeapon", [](const auto&) -> VMValue {
        const auto state = ScriptEngine::instance().loadoutState();
        return VMValue(state.hasWeapons ? state.currentWeapon : -1);
    });
    tsInstance->registerNative("getWeaponCount", [](const auto&) -> VMValue {
        const auto state = ScriptEngine::instance().loadoutState();
        return VMValue(state.hasWeapons ? state.weaponCount : 0);
    });
    tsInstance->registerNative("getBackpack", [](const auto&) -> VMValue {
        const auto state = ScriptEngine::instance().loadoutState();
        return VMValue(state.hasBackpack && state.backpackActive ? state.backpackIndex : -1);
    });
    tsInstance->registerNative("setWeaponAmmo", [](const auto& args) -> VMValue {
        if (args.size() < 3 || args[0].toInt() <= 0 || args[1].toInt() < 0 ||
            args[2].toInt() < -1) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        const int objectId = args[0].toInt();
        const int slot = args[1].toInt();
        const int ammo = args[2].toInt();
        return VMValue(engine.mutateWeaponAmmo(objectId, slot, ammo) ? 1 : 0);
    });
    tsInstance->registerNative("setCurrentWeapon", [](const auto& args) -> VMValue {
        if (args.size() < 2 || args[1].toInt() < 0) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        return VMValue(engine.mutateCurrentWeapon(args[0].toInt(), args[1].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("getDataBlock", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.datablockId);
        }
        // GameBase::getDataBlock: the datablock's id.
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
            const std::string block = ScriptEngine::instance().objectDataBlock(object);
            if (!block.empty()) return VMValue(std::atoi(block.c_str()));
        }
        return VMValue(0);
    });
    auto metadataObject = [](const std::vector<VMValue>& args, ScriptObjectState& state,
                             ScriptObject*& object) {
        object = nullptr;
        if (args.empty()) return false;
        const std::string name = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(name.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
            return true;
        object = ScriptEngine::instance().findObject(name.c_str());
        return object != nullptr;
    };
    auto classMatches = [](const std::string& actual, const std::string& wanted) {
        if (actual.empty() || wanted.empty()) return false;
        if (actual == wanted) return true;
        if (wanted == "SimObject") return true;
        if (wanted == "GameBase")
            return actual == "Player" || actual.ends_with("Vehicle") || actual.ends_with("Turret");
        if (wanted == "ShapeBase")
            return actual == "Player" || actual.ends_with("Vehicle") || actual.ends_with("Turret");
        return false;
    };
    tsInstance->registerNative("isMemberOfClass", [metadataObject, classMatches](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        ScriptObjectState state;
        ScriptObject* object = nullptr;
        if (!metadataObject(args, state, object)) return VMValue(0);
        const std::string actual = object ? object->className : state.className;
        return VMValue(classMatches(actual, args[1].toString()) ? 1 : 0);
    });
    tsInstance->registerNative("isTypeOf", [metadataObject, classMatches](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        ScriptObjectState state;
        ScriptObject* object = nullptr;
        if (!metadataObject(args, state, object)) return VMValue(0);
        if (args[1].type == VMValue::String) {
            const std::string actual = object ? object->className : state.className;
            return VMValue(classMatches(actual, args[1].toString()) ? 1 : 0);
        }
        if (object)
            return VMValue(object->fields.count("type") &&
                           object->fields.at("type").toInt() == args[1].toInt() ? 1 : 0);
        return VMValue(state.type == args[1].toInt() ? 1 : 0);
    });
    tsInstance->registerNative("objectCount", [](const auto&) -> VMValue {
        return VMValue((int32_t)ScriptEngine::instance().objects.size());
    });
    tsInstance->registerNative("getObjectCount", [](const auto&) -> VMValue {
        return VMValue((int32_t)ScriptEngine::instance().objects.size());
    });
    tsInstance->registerNative("getTarget", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(-1);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
            const int id = object->fields["target"].toInt();
            return s_scriptTargets.count(id) ? VMValue(id) : VMValue(-1);
        }
        return VMValue(-1);
    });

    // TargetManager.cs uses these engine natives for player, flag, waypoint,
    // and sensor HUD entries. Keep their mutable state separate from script
    // objects so targets remain valid after their owner changes fields.
    tsInstance->registerNative("createTarget", [](const auto& args) -> VMValue {
        if (args.size() < 6 || args[0].toString().empty() || args[5].toInt() < 0 || args[5].toInt() >= 32)
            return VMValue(-1);
        ScriptTarget target;
        target.objectName = args[0].toString();
        target.nameTag = args[1];
        target.skinTag = args[2];
        target.voiceTag = args[3];
        target.typeTag = args[4];
        target.sensorGroup = args[5].toInt();
        const int id = s_nextScriptTarget++;
        s_scriptTargets.emplace(id, std::move(target));
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            object->fields["target"] = VMValue(id);
        return VMValue(id);
    });
    tsInstance->registerNative("allocTarget", [](const auto& args) -> VMValue {
        if (args.size() < 5 || args[4].toInt() < 0 || args[4].toInt() >= 32) return VMValue(-1);
        ScriptTarget target;
        target.nameTag = args[0];
        target.skinTag = args[1];
        target.voiceTag = args[2];
        target.typeTag = args[3];
        target.sensorGroup = args[4].toInt();
        if (args.size() > 5) target.datablock = args[5];
        if (args.size() > 6 && args[6].toDouble() != 0.0) target.voicePitch = args[6].toDouble();
        if (args.size() > 7) target.skinTag = args[7];
        const int id = s_nextScriptTarget++;
        s_scriptTargets.emplace(id, std::move(target));
        return VMValue(id);
    });
    tsInstance->registerNative("allocClientTarget", [](const auto& args) -> VMValue {
        if (args.size() < 5 || args[0].toString().empty() || args[4].toInt() < 0 || args[4].toInt() >= 32)
            return VMValue(-1);
        ScriptTarget target;
        target.objectName = args[0].toString();
        target.nameTag = args[1];
        target.skinTag = args[2];
        target.voiceTag = args[3];
        target.typeTag = args[4];
        target.sensorGroup = args.size() > 5 ? args[5].toInt() : 0;
        if (args.size() > 7) target.voicePitch = args[7].toDouble();
        const int id = s_nextScriptTarget++;
        s_scriptTargets.emplace(id, std::move(target));
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            object->fields["target"] = VMValue(id);
        return VMValue(id);
    });
    tsInstance->registerNative("freeTarget", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const int id = args[0].toInt();
        auto it = s_scriptTargets.find(id);
        if (it == s_scriptTargets.end()) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(it->second.objectName.c_str())) {
            if (object->fields["target"].toInt() == id) object->fields["target"] = VMValue(-1);
        }
        s_scriptTargets.erase(it);
        return VMValue(1);
    });
    tsInstance->registerNative("clientResetTargets", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string objectName = args[0].toString();
        for (auto it = s_scriptTargets.begin(); it != s_scriptTargets.end(); ) {
            if (it->second.objectName == objectName) it = s_scriptTargets.erase(it);
            else ++it;
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            object->fields["target"] = VMValue(-1);
        return VMValue(1);
    });
    tsInstance->registerNative("setTargetRenderMask", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        if (args[1].toInt() < 0) return VMValue(0);
        it->second.renderMask = (uint32_t)args[1].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("getTargetRenderMask", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue(0) : VMValue((int32_t)it->second.renderMask);
    });
    tsInstance->registerNative("setTargetSkin", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        it->second.skinTag = args[1];
        return VMValue(1);
    });
    tsInstance->registerNative("setTargetName", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        it->second.nameTag = args[1];
        return VMValue(1);
    });
    tsInstance->registerNative("setTargetSensorData", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        it->second.sensorData = args[1];
        return VMValue(1);
    });
    tsInstance->registerNative("getTargetSensorData", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue(0) : it->second.sensorData;
    });
    tsInstance->registerNative("setTargetSensorGroup", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        if (args[1].toInt() < 0 || args[1].toInt() >= 32) return VMValue(0);
        it->second.sensorGroup = args[1].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("getTargetSensorGroup", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue(0) : VMValue(it->second.sensorGroup);
    });
    tsInstance->registerNative("setTargetAlwaysVisMask", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        if (args[1].toInt() < 0) return VMValue(0);
        it->second.alwaysVisMask = (uint32_t)args[1].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("getTargetName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue("") : it->second.nameTag;
    });
    tsInstance->registerNative("getTargetSkin", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue("") : it->second.skinTag;
    });
    tsInstance->registerNative("getTargetType", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue(0) : it->second.typeTag;
    });
    tsInstance->registerNative("getTargetAlwaysVisMask", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        return it == s_scriptTargets.end() ? VMValue(0) : VMValue((int32_t)it->second.alwaysVisMask);
    });
    tsInstance->registerNative("resetTargetManager", [](const auto&) -> VMValue {
        s_scriptTargets.clear();
        s_nextScriptTarget = 1;
        return VMValue(1);
    });
    tsInstance->registerNative("setTarget", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
            if (!s_scriptTargets.count(args[1].toInt())) return VMValue(0);
            object->fields["target"] = args[1];
            return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("setTargetObject", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
            if (!s_scriptTargets.count(object->fields["target"].toInt())) return VMValue(0);
            object->fields["targetObject"] = args[1];
            return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("setSensorGroupColor", [](const auto& args) -> VMValue {
        if (args.size() < 3) {
            Console::instance().printf(LogLevel::Warn,
                "setSensorGroupColor: expected group, target mask and color");
            return VMValue(0);
        }
        const int group = args[0].toInt();
        if (group < 0 || group >= 32) {
            Console::instance().printf(LogLevel::Warn,
                "setSensorGroupColor: invalid sensor group %d", group);
            return VMValue(0);
        }
        const uint32_t targetMask = args[1].toInt();
        std::istringstream values(args[2].toString());
        ColorF color{};
        if (!(values >> color.r >> color.g >> color.b >> color.a)) {
            Console::instance().printf(LogLevel::Warn,
                "setSensorGroupColor: invalid color '%s'", args[2].toString().c_str());
            return VMValue(0);
        }
        color.r /= 255.0f; color.g /= 255.0f;
        color.b /= 255.0f;
        color.a = color.a < 0.0f ? 1.0f : color.a / 255.0f;
        Engine::instance().game().setSensorGroupColor(group, targetMask, color);
        return VMValue(1);
    });
     tsInstance->registerNative("setTargetFriendlyMask", [](const auto& args) -> VMValue {
        if (args.size() < 2) {
            Console::instance().printf(LogLevel::Warn,
                "setTargetFriendlyMask: expected group and mask");
            return VMValue(0);
        }
        const int group = args[0].toInt();
        if (group < 0 || group >= 32) return VMValue(0);
        const std::string maskText = args[1].toString();
        char* end = nullptr;
        const unsigned long mask = std::strtoul(maskText.c_str(), &end, 0);
        if (!end || *end != '\0' || mask > 0xfffffffful) {
            Console::instance().printf(LogLevel::Warn,
                "setTargetFriendlyMask: invalid mask '%s'", maskText.c_str());
            return VMValue(0);
        }
        Engine::instance().game().setTargetFriendlyMask(group, (uint32_t)mask);
         return VMValue(1);
     });
    tsInstance->registerNative("setSensorGroupCount", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const int count = args[0].toInt();
        if (count < 0 || count > 32) return VMValue(0);
        Engine::instance().game().setSensorGroupCount(count);
        return VMValue(1);
    });
    auto setSensorGroupMask = [](const char* nativeName, const std::vector<VMValue>& args,
                                 auto setter) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int group = args[0].toInt();
        if (group < 0 || group >= 32) return VMValue(0);
        const std::string maskText = args[1].toString();
        char* end = nullptr;
        const unsigned long mask = std::strtoul(maskText.c_str(), &end, 0);
        if (!end || *end != '\0' || mask > 0xfffffffful) {
            Console::instance().printf(LogLevel::Warn,
                "%s: invalid mask '%s'", nativeName, maskText.c_str());
            return VMValue(0);
        }
        setter(group, (uint32_t)mask);
        return VMValue(1);
    };
    tsInstance->registerNative("setSensorGroupListenMask", [setSensorGroupMask](const auto& args) -> VMValue {
        return setSensorGroupMask("setSensorGroupListenMask", args,
            [](int group, uint32_t mask) {
                Engine::instance().game().setSensorGroupListenMask(group, mask);
            });
    });
    tsInstance->registerNative("setSensorGroupFriendlyMask", [setSensorGroupMask](const auto& args) -> VMValue {
        return setSensorGroupMask("setSensorGroupFriendlyMask", args,
            [](int group, uint32_t mask) {
                Engine::instance().game().setSensorGroupFriendlyMask(group, mask);
            });
    });
    tsInstance->registerNative("getMountedImage", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        ScriptObjectState state;
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        const int slot = args[1].toInt();
        if (slot < 0 || slot >= 8) return VMValue(0);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
            return VMValue(state.mountedImages[slot].datablockId);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["mountedImage::" + std::to_string(args[1].toInt())];
        std::string image;
        if (Engine::instance().game().world().getMissionObjectImage(
                args[0].toString(), slot, image)) return VMValue(image);
        return VMValue(0);
    });
    tsInstance->registerNative("setMountedImage", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        const int slot = args[1].toInt();
        const int datablock = args[2].toInt();
        if (!end || *end != '\0' || id <= 0 || slot < 0 || slot >= 8 || datablock < 0)
            return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateMountedImage(
            (int)id, slot, datablock) ? 1 : 0);
    });
    tsInstance->registerNative("getMountNodeObject", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        char* end = nullptr;
        const long id = std::strtol(args[0].toString().c_str(), &end, 10);
        const int slot = args[1].toInt();
        ScriptObjectState state;
        if (end && *end == '\0' && id > 0 && slot >= 0 && slot < 8 &&
            ScriptEngine::instance().objectState((int)id, state))
            return VMValue(state.mountedImages[slot].mountNodeObjectId);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["mountNode::" + args[1].toString()];
        return VMValue(0);
    });
    tsInstance->registerNative("getTransform", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        ScriptObjectState state;
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state)) {
            char value[160];
            snprintf(value, sizeof(value), "%g %g %g %g %g %g %g",
                     state.position.x, state.position.y, state.position.z,
                     state.rotation.x, state.rotation.y, state.rotation.z,
                     state.rotationW);
            return VMValue(value);
        }
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["transform"];
        std::string transform;
        if (Engine::instance().game().world().getMissionObjectTransform(
                args[0].toString(), transform))
            return VMValue(transform);
        return VMValue("");
    });
    // Mission object transforms are handled by World. Unknown/network-only
    // objects deliberately return failure rather than creating a shadow state.
    tsInstance->registerNative("getWorldBoxCenter", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str())) {
            auto it = object->fields.find("worldBoxCenter");
            if (it != object->fields.end()) return it->second;
            return object->fields["position"];
        }
        return VMValue("");
    });
    tsInstance->registerNative("getDamageState", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.state);
        }
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["damageState"];
        return VMValue("");
    });
    tsInstance->registerNative("setPoweredState", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (!object) return VMValue(0);
        object->fields["powered"] = VMValue(args[1].toBool() ? "1" : "0");
        return VMValue(1);
    });
    tsInstance->registerNative("isEnabled", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (!object) return VMValue(0);
        auto it = object->fields.find("enabled");
        return VMValue(it == object->fields.end() || it->second.toBool() ? 1 : 0);
    });
    tsInstance->registerNative("getDamageLevel", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0f);
        ScriptObjectState state;
        char* end = nullptr;
        const std::string name = args[0].toString();
        const long id = std::strtol(name.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 &&
            ScriptEngine::instance().objectState((int)id, state) && state.hasHealth) {
            const float maximum = state.hasMaxHealth ? state.maxHealth : 100.0f;
            return VMValue(ScriptStateParity::damageLevel(state.health, maximum));
        }
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["damageLevel"];
        return VMValue(0.0f);
    });
    tsInstance->registerNative("setDamageLevel", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        const float damage = std::clamp(args[1].toFloat(), 0.0f, 1.0f);
        return VMValue(engine.mutateHealth(args[0].toInt(), (1.0f - damage) * 100.0f) ? 1 : 0);
    });
    tsInstance->registerNative("setHealth", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int objectId = args[0].toInt();
        const float health = args[1].toFloat();
        if (objectId <= 0 || !std::isfinite(health)) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateHealth(
            objectId, std::clamp(health, 0.0f, 100.0f)) ? 1 : 0);
    });
    auto applyDamageOrRepair = [](const auto& args, bool repair) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int objectId = args[0].toInt();
        const float amount = args[1].toFloat();
        if (objectId <= 0 || !std::isfinite(amount) || amount < 0.0f) return VMValue(0);
        ScriptObjectState state;
        if (!ScriptEngine::instance().objectState(objectId, state) || !state.hasHealth)
            return VMValue(0);
        const float health = std::clamp(state.health + (repair ? amount : -amount),
                                       0.0f, state.hasMaxHealth ? state.maxHealth : 100.0f);
        if (!ScriptEngine::instance().mutateHealth(objectId, health)) return VMValue(0);
            if (auto* ts = ScriptEngine::instance().ts()) {
                const std::string callback = state.className + (repair ? "::onRepair" : "::onDamage");
            std::string nativeCallback = callback;
            for (char& c : nativeCallback) c = (char)std::tolower((unsigned char)c);
            if (!state.className.empty() &&
                (ts->hasFunction(callback) || ts->getNatives().count(nativeCallback)))
                ts->callFunction(callback, {VMValue(objectId), VMValue(amount)});
        }
        return VMValue(1);
    };
    tsInstance->registerNative("damage", [applyDamageOrRepair](const auto& args) {
        return applyDamageOrRepair(args, false);
    });
    tsInstance->registerNative("repair", [applyDamageOrRepair](const auto& args) {
        return applyDamageOrRepair(args, true);
    });
    tsInstance->registerNative("setEnergyLevel", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int objectId = args[0].toInt();
        const float energy = args[1].toFloat();
        if (objectId <= 0 || !std::isfinite(energy)) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateEnergy(
            objectId, std::clamp(energy, 0.0f, 100.0f)) ? 1 : 0);
    });
    tsInstance->registerNative("getRepairRate", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0f);
        ScriptObjectState state;
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
            return VMValue(state.repairRate);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["repairRate"];
        return VMValue(0.0f);
    });
    tsInstance->registerNative("getSensorGroup", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(-1);
        ScriptObjectState state;
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
            return VMValue(state.sensorGroup);
        if (auto* object = ScriptEngine::instance().findObject(value.c_str()))
            return object->fields["sensorGroup"];
        return VMValue(-1);
    });
    tsInstance->registerNative("setRepairRate", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const int objectId = args[0].toInt();
        const float rate = args[1].toFloat();
        if (objectId <= 0 || !std::isfinite(rate) || rate < 0.0f) return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateRepairRate(objectId, rate) ? 1 : 0);
    });
    tsInstance->registerNative("getControllingClient", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["controllingClient"];
        return VMValue(0);
    });
    tsInstance->registerNative("isMounted", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["mounted"].toBool() ? VMValue(1) : VMValue(0);
        return VMValue(0);
    });
    tsInstance->registerNative("getType", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string name = args[0].toString();
        char* end = nullptr;
        const long id = std::strtol(name.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state)) return VMValue(state.type);
        }
        if (auto* object = ScriptEngine::instance().findObject(name.c_str())) {
            auto field = object->fields.find("type");
            return field == object->fields.end() ? VMValue(0) : field->second;
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getVelocity", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        ScriptObjectState state;
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state) &&
            state.hasVelocity) {
            char value[96];
            snprintf(value, sizeof(value), "%g %g %g", state.velocity.x,
                     state.velocity.y, state.velocity.z);
            return VMValue(value);
        }
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["velocity"];
        return VMValue("");
    });
    auto setVelocity = [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        char* end = nullptr;
        const long id = std::strtol(args[0].toString().c_str(), &end, 10);
        if (!end || *end != '\0' || id <= 0) return VMValue(0);
        Point3F velocity;
        if (args.size() == 2 ? !parseVector(args[1], velocity) : args.size() == 4) {
            if (args.size() == 4) velocity = {args[1].toFloat(), args[2].toFloat(), args[3].toFloat()};
            else return VMValue(0);
        } else if (args.size() != 2) return VMValue(0);
        if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
            return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateVelocity((int)id, velocity) ? 1 : 0);
    };
    tsInstance->registerNative("setVelocity", setVelocity);
    auto threadMutation = [](const auto& args, int operation) -> VMValue {
        if (args.size() < 2 || args.size() > 3) return VMValue(0);
        char* end = nullptr;
        const long id = std::strtol(args[0].toString().c_str(), &end, 10);
        const int slot = args[1].toInt();
        if (!end || *end != '\0' || id <= 0 || slot < 0 || slot >= 4) return VMValue(0);
        if ((operation == 1 || operation == 4 || operation == 5) && args.size() < 3)
            return VMValue(0);
        return VMValue(ScriptEngine::instance().mutateThread(
            (int)id, slot, operation, args.size() > 2 ? args[2].toString() : "") ? 1 : 0);
    };
    tsInstance->registerNative("playThread", [threadMutation](const auto& args) {
        return threadMutation(args, 1);
    });
    tsInstance->registerNative("stopThread", [threadMutation](const auto& args) {
        return threadMutation(args, 2);
    });
    tsInstance->registerNative("pauseThread", [threadMutation](const auto& args) {
        return threadMutation(args, 3);
    });
    tsInstance->registerNative("setThreadDir", [threadMutation](const auto& args) {
        return threadMutation(args, 4);
    });
    tsInstance->registerNative("setThreadTimeScale", [threadMutation](const auto& args) {
        return threadMutation(args, 5);
    });
    tsInstance->registerNative("getThreadState", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        ScriptObjectState state;
        char* end = nullptr;
        const long id = std::strtol(args[0].toString().c_str(), &end, 10);
        const int slot = args[1].toInt();
        if (!end || *end != '\0' || id <= 0 || slot < 0 || slot >= 4 ||
            !ScriptEngine::instance().objectState((int)id, state) || !state.threads[slot].valid)
            return VMValue(0);
        return VMValue(state.threads[slot].state);
    });
    vmInstance->registerNativeFunction("setVelocity", setVelocity);
    vmInstance->registerNativeFunction("playThread", [threadMutation](const auto& args) {
        return threadMutation(args, 1);
    });
    vmInstance->registerNativeFunction("stopThread", [threadMutation](const auto& args) {
        return threadMutation(args, 2);
    });
    vmInstance->registerNativeFunction("pauseThread", [threadMutation](const auto& args) {
        return threadMutation(args, 3);
    });
    vmInstance->registerNativeFunction("setThreadDir", [threadMutation](const auto& args) {
        return threadMutation(args, 4);
    });
    vmInstance->registerNativeFunction("setThreadTimeScale", [threadMutation](const auto& args) {
        return threadMutation(args, 5);
    });
    vmInstance->registerNativeFunction("getThreadState", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        ScriptObjectState state;
        char* end = nullptr;
        const long id = std::strtol(args[0].toString().c_str(), &end, 10);
        const int slot = args[1].toInt();
        if (!end || *end != '\0' || id <= 0 || slot < 0 || slot >= 4 ||
            !ScriptEngine::instance().objectState((int)id, state) || !state.threads[slot].valid)
            return VMValue(0);
        return VMValue(state.threads[slot].state);
    });
    tsInstance->registerNative("getRotation", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("0 0 0 1");
        ScriptObjectState state;
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state)) {
            char value[128];
            snprintf(value, sizeof(value), "%g %g %g %g", state.rotation.x,
                     state.rotation.y, state.rotation.z, state.rotationW);
            return VMValue(value);
        }
        Point3F axis;
        float angle = 0.0f;
        if (Engine::instance().game().world().getMissionObjectRotation(objectName, axis, angle)) {
            char value[128];
            snprintf(value, sizeof(value), "%g %g %g %g", axis.x, axis.y, axis.z, angle);
            return VMValue(value);
        }
        if (auto* object = ScriptEngine::instance().findObject(objectName.c_str()))
            return object->fields["rotation"];
        return VMValue("0 0 0 1");
    });
    auto objectNumericState = [](const std::vector<VMValue>& args,
                                 float ScriptObjectState::*member,
                                 bool ScriptObjectState::*available,
                                 float fallback) -> VMValue {
        if (args.empty()) return VMValue(fallback);
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        ScriptObjectState state;
        if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
            return VMValue(state.*available ? state.*member : fallback);
        return VMValue(fallback);
    };
    tsInstance->registerNative("getHealth", [objectNumericState](const auto& args) {
        return objectNumericState(args, &ScriptObjectState::health,
                                   &ScriptObjectState::hasHealth, 0.0f);
    });
    tsInstance->registerNative("getMaxHealth", [objectNumericState](const auto& args) {
        return objectNumericState(args, &ScriptObjectState::maxHealth,
                                   &ScriptObjectState::hasMaxHealth, 0.0f);
    });
    tsInstance->registerNative("getEnergyLevel", [objectNumericState](const auto& args) {
        return objectNumericState(args, &ScriptObjectState::energy,
                                   &ScriptObjectState::hasEnergy, 0.0f);
    });
    tsInstance->registerNative("getEnergyPercent", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0f);
        ScriptObjectState state;
        char* end = nullptr;
        const std::string objectName = args[0].toString();
        const long id = std::strtol(objectName.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0 &&
            ScriptEngine::instance().objectState((int)id, state) && state.hasEnergy)
            return VMValue(ScriptStateParity::energyPercent(state.energy));
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["energyPercent"];
        return VMValue(0.0f);
    });
    tsInstance->registerNative("isTargetVisible", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        ScriptObjectState target;
        char* end = nullptr;
        const std::string targetName = args[0].toString();
        const long targetId = std::strtol(targetName.c_str(), &end, 10);
        if (!end || *end != '\0' || targetId <= 0 ||
            !ScriptEngine::instance().objectState((int)targetId, target)) return VMValue(0);
        int listenerGroup = 0;
        ScriptObjectState listener;
        const int controlId = ScriptEngine::instance().controlObjectId();
        if (controlId > 0 && ScriptEngine::instance().objectState(controlId, listener))
            listenerGroup = listener.sensorGroup >= 0 ? listener.sensorGroup : 0;
        if (args.size() > 1) {
            const std::string listenerName = args[1].toString();
            const long listenerId = std::strtol(listenerName.c_str(), &end, 10);
            if (end && *end == '\0' && listenerId > 0 &&
                ScriptEngine::instance().objectState((int)listenerId, listener))
                listenerGroup = listener.sensorGroup >= 0 ? listener.sensorGroup : 0;
            else listenerGroup = args[1].toInt();
        }
        return VMValue(Engine::instance().game().isSensorGroupTargetVisible(
            listenerGroup, target.sensorGroup) ? 1 : 0);
    });
    tsInstance->registerNative("getDamagePercent", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0f);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->fields["damagePercent"];
        return VMValue(0.0f);
    });
    tsInstance->registerNative("isAIControlled", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return VMValue(object->fields["aiControlled"].toBool() ? 1 : 0);
        return VMValue(0);
    });
    tsInstance->registerNative("setActive", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->active = args[1].toBool();
                if (!ctl->active && Engine::instance().guiRenderer().getFocused() == ctl)
                    Engine::instance().guiRenderer().makeFirstResponder(ctl->name, false);
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["active"] = VMValue(ctl->active ? "1" : "0");
                return VMValue(1);
            }
        }
        if (args.size() >= 2)
            return VMValue(Engine::instance().game().world().setMissionObjectEnabled(
                args[0].toString(), args[1].toBool()) ? 1 : 0);
        return VMValue(0);
    });
    tsInstance->registerNative("delete", [](const auto& args) -> VMValue {
        if (!args.empty()) {
             const std::string objName = args[0].toString();
             // An object the mission created is deleted as itself (the mission
             // file's records stand in only where the mission did not run).
             const auto missionOrder = ScriptEngine::instance().findObject(objName.c_str())
                 ? std::vector<std::string>{} : ScriptEngine::instance().missionDeletionOrder(objName);
             if (!missionOrder.empty()) {
                 for (const auto& name : missionOrder) {
                     auto it = std::find_if(ScriptEngine::instance().missionObjects().begin(),
                         ScriptEngine::instance().missionObjects().end(),
                         [&name](const ScriptMissionObject& object) { return object.name == name; });
                     if (it == ScriptEngine::instance().missionObjects().end()) continue;
                     const bool worldDeleted = ScriptEngine::instance().missionObjectsWorldBacked() &&
                         Engine::instance().game().deleteMissionObjectIfPresent(name);
                      if (!worldDeleted) {
                          const std::string callback = it->className + "::onRemove";
                          if (ScriptEngine::instance().ts()->hasFunction(callback))
                              ScriptEngine::instance().ts()->callFunction(callback, {VMValue(name)});
                      }
                      ScriptEngine::instance().ts()->cancelEventsForObject(name);
                  }
                 ScriptEngine::instance().removeMissionObjects(missionOrder);
                 return VMValue(1);
             }
              auto* obj = ScriptEngine::instance().findObject(objName.c_str());
               if (obj) {
                if (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 ||
                    obj->className.find("Hud") == 0) {
                    Engine::instance().guiRenderer().removeControl(objName);
                }
                if (obj->className.find("Profile") != std::string::npos) return VMValue(1);
                 ScriptEngine::instance().deleteScriptObject(objName);
            }
            // Mission objects are not ScriptObject instances. Remove them
            // through World so schedules and lifecycle callbacks agree.
            if (!obj && Engine::instance().g) Engine::instance().game().world().deleteMissionObject(objName);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setValue", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                if (ctl->className == "GuiRadioCtrl" || ctl->className == "ShellRadioButton" || ctl->className == "GuiCheckBoxCtrl")
                    ctl->checked = args[1].toBool();
                else if (ctl->className == "ShellTabButton" || ctl->className == "GuiTabButton")
                    ctl->selected = args[1].toBool();  // T2 tabs: setValue(1/0) = select/deselect
                else if (ctl->className.find("Slider") != std::string::npos) {
                    // GuiSliderCtrl::setValue: a float clamped to the range
                    // (its 'range' field, which onWake may set before the
                    // control is laid out).
                    const std::string range = GuiShared::field(*ctl, "range");
                    float lo = ctl->sliderMin, hi = ctl->sliderMax;
                    if (!range.empty() && sscanf(range.c_str(), "%f %f", &lo, &hi) == 2) {
                        ctl->sliderMin = lo;
                        ctl->sliderMax = hi;
                    }
                    ctl->sliderValue = std::clamp((float)args[1].toDouble(), ctl->sliderMin, ctl->sliderMax);
                }
                else if (ctl->className == "GuiProgressCtrl" || ctl->className.find("Hud") == 0) {
                    ctl->hudValue = (float)args[1].toDouble();
                    ctl->hudValueSet = true;
                    ctl->fields["value"] = args[1].toString();
                }
                else if (ctl->className.find("Window") != std::string::npos && args.size() >= 4) {
                    ctl->posX = (float)args[1].toInt();
                    ctl->posY = (float)args[2].toInt();
                    if (args.size() >= 5) { ctl->extentX = (float)args[3].toInt(); ctl->extentY = (float)args[4].toInt(); }
                }
                else
                    ctl->text = args[1].toString();
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["value"] = VMValue(args[1].toString());
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("replaceText", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        ctl->replaceMenuTextOnSelect = args[1].toBool();
        ctl->fields["replaceText"] = args[1].toString();
        if (auto* object = ScriptEngine::instance().findObject(ctl->name.c_str()))
            object->fields["replaceText"] = VMValue(args[1].toString());
        return VMValue(1);
    });
    tsInstance->registerNative("getValue", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        if (ctl->className.find("Slider") != std::string::npos)
            return VMValue(ctl->sliderValue);  // GuiSliderCtrl::getValue: the value in its range
        if (ctl->className == "GuiProgressCtrl" || ctl->className.find("Hud") == 0)
            return VMValue(ctl->hudValueSet ? ctl->hudValue : 0.0f);
        if (ctl->className.find("CheckBox") != std::string::npos || ctl->className.find("Radio") != std::string::npos || ctl->className.find("Toggle") != std::string::npos)
            return VMValue(ctl->checked ? 1 : 0);
        return VMValue(ctl->text);
    });
    tsInstance->registerNative("getExtent", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("0 0");
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue("0 0");
        return VMValue(std::to_string((int)ctl->extentX) + " " + std::to_string((int)ctl->extentY));
    });
    tsInstance->registerNative("getPosition", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("0 0");
        char* end = nullptr;
        const std::string value = args[0].toString();
        const long id = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState((int)id, state)) {
                char position[96];
                snprintf(position, sizeof(position), "%g %g %g", state.position.x,
                         state.position.y, state.position.z);
                return VMValue(position);
            }
            return VMValue("0 0 0");
        }
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue("0 0");
        return VMValue(std::to_string((int)ctl->posX) + " " + std::to_string((int)ctl->posY));
    });
    tsInstance->registerNative("getGroup", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        auto* ctl = getListCtrl(args[0].toString());
        if (ctl && ctl->parent) return VMValue(ctl->parent->name);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return object->internals["__parent"];
        return VMValue("");
    });
    tsInstance->registerNative("getChild", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = getListCtrl(args[0].toString());
        const int index = args[1].toInt();
        if (ctl && ctl->className == "GuiTreeView") {
            for (const auto& item : ctl->treeItems)
                if (item.parent == index) return VMValue(item.id);
            return VMValue(0);
        }
        if (!ctl || index < 0 || index >= (int)ctl->children.size()) return VMValue("");
        return VMValue(ctl->children[index]->name);
    });
    tsInstance->registerNative("getObject", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = getListCtrl(args[0].toString());
        const int index = args[1].toInt();
        if (ctl) {
            if (ctl->className == "GuiTreeView") {
                for (const auto& item : ctl->treeItems)
                    if (item.parent == index) return VMValue(item.id);
                return VMValue(0);
            }
            if (index < 0 || index >= (int)ctl->children.size()) return VMValue("");
            return VMValue(ctl->children[index]->name);
        }
        if (auto* group = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return group->internals["__child" + std::to_string(index)];
        return VMValue("");
    });
    tsInstance->registerNative("getCount", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) {
            if (auto* group = ScriptEngine::instance().findObject(args[0].toString().c_str()))
                return VMValue(group->internals["__childCount"].toInt());
            return VMValue(0);
        }
        if (ctl->className == "GuiTreeView") return VMValue((int32_t)ctl->treeItems.size());
        if (!ctl->children.empty()) return VMValue((int32_t)ctl->children.size());
        if (!ctl->menuItems.empty()) return VMValue((int32_t)ctl->menuItems.size());
        return VMValue((int32_t)ctl->listRows.size());
    });
    tsInstance->registerNative("getFirstRootItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        for (const auto& item : ctl->treeItems)
            if (item.parent == 0) return VMValue(item.id);
        return VMValue(0);
    });
    tsInstance->registerNative("insertItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 4) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        const int id = ctl->nextTreeItemId++;
        ctl->treeItems.push_back({id, args[1].toInt(), args[2].toString(), args[3].toString(), false});
        return VMValue(id);
    });
    tsInstance->registerNative("getNextSibling", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        int parent = 0; bool found = false;
        for (const auto& item : ctl->treeItems) {
            if (item.id == args[1].toInt()) { parent = item.parent; found = true; break; }
        }
        if (!found) return VMValue(0);
        bool after = false;
        for (const auto& item : ctl->treeItems) {
            if (item.parent != parent) continue;
            if (after) return VMValue(item.id);
            if (item.id == args[1].toInt()) after = true;
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getPrevSibling", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        int parent = -1; int previous = 0;
        for (const auto& item : ctl->treeItems)
            if (item.id == args[1].toInt()) { parent = item.parent; break; }
        if (parent < 0) return VMValue(0);
        for (const auto& item : ctl->treeItems) {
            if (item.parent != parent) continue;
            if (item.id == args[1].toInt()) return VMValue(previous);
            previous = item.id;
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getItemText", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue("");
        for (const auto& item : ctl->treeItems)
            if (item.id == args[1].toInt()) return VMValue(item.text);
        return VMValue("");
    });
    tsInstance->registerNative("getItemValue", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue("");
        for (const auto& item : ctl->treeItems)
            if (item.id == args[1].toInt()) return VMValue(item.data);
        return VMValue("");
    });
    tsInstance->registerNative("editItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        for (auto& item : ctl->treeItems) {
            if (item.id == args[1].toInt()) {
                item.text = args[2].toString();
                if (args.size() > 3) item.data = args[3].toString();
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("selectItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        ctl->selectedTreeItem = args[1].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("getSelectedItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        return ctl && ctl->className == "GuiTreeView" ? VMValue(ctl->selectedTreeItem) : VMValue(0);
    });
    tsInstance->registerNative("expandItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        for (auto& item : ctl->treeItems)
            if (item.id == args[1].toInt()) { item.expanded = true; return VMValue(1); }
        return VMValue(0);
    });
    tsInstance->registerNative("collapseItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        for (auto& item : ctl->treeItems)
            if (item.id == args[1].toInt()) { item.expanded = false; return VMValue(1); }
        return VMValue(0);
    });
    tsInstance->registerNative("removeItem", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        const int id = args[1].toInt();
        std::set<int> doomed{id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& item : ctl->treeItems)
                if (doomed.count(item.parent) && doomed.insert(item.id).second) changed = true;
        }
        ctl->treeItems.erase(std::remove_if(ctl->treeItems.begin(), ctl->treeItems.end(),
            [&](const GuiControl::TreeItem& item) { return doomed.count(item.id) != 0; }),
            ctl->treeItems.end());
        if (doomed.count(ctl->selectedTreeItem)) ctl->selectedTreeItem = 0;
        return VMValue(1);
    });
    tsInstance->registerNative("moveItemUp", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        const int id = args[1].toInt();
        int itemIndex = -1, parent = 0;
        for (int i = 0; i < (int)ctl->treeItems.size(); ++i)
            if (ctl->treeItems[i].id == id) { itemIndex = i; parent = ctl->treeItems[i].parent; break; }
        if (itemIndex < 0) return VMValue(0);
        auto subtreeEnd = [&](int start) {
            int end = start + 1;
            while (end < (int)ctl->treeItems.size()) {
                int ancestor = ctl->treeItems[end].parent;
                bool descendant = false;
                while (ancestor != 0) {
                    if (ancestor == ctl->treeItems[start].id) { descendant = true; break; }
                    auto it = std::find_if(ctl->treeItems.begin(), ctl->treeItems.end(),
                        [&](const GuiControl::TreeItem& item) { return item.id == ancestor; });
                    if (it == ctl->treeItems.end()) break;
                    ancestor = it->parent;
                }
                if (!descendant) break;
                ++end;
            }
            return end;
        };
        int previous = -1;
        for (int i = 0; i < itemIndex; ++i)
            if (ctl->treeItems[i].parent == parent) previous = i;
        if (previous >= 0) {
            const int previousEnd = subtreeEnd(previous);
            const int itemEnd = subtreeEnd(itemIndex);
            const auto previousBlockEnd = ctl->treeItems.begin() + previousEnd;
            const auto itemBlockBegin = ctl->treeItems.begin() + itemIndex;
            const auto itemBlockEnd = ctl->treeItems.begin() + itemEnd;
            std::vector<GuiControl::TreeItem> previousBlock(ctl->treeItems.begin() + previous,
                                                            previousBlockEnd);
            std::vector<GuiControl::TreeItem> itemBlock(itemBlockBegin, itemBlockEnd);
            ctl->treeItems.erase(ctl->treeItems.begin() + previous, itemBlockEnd);
            ctl->treeItems.insert(ctl->treeItems.begin() + previous, itemBlock.begin(), itemBlock.end());
            ctl->treeItems.insert(ctl->treeItems.begin() + previous + itemBlock.size(),
                                  previousBlock.begin(), previousBlock.end());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("moveItemDown", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || ctl->className != "GuiTreeView") return VMValue(0);
        const int id = args[1].toInt();
        int itemIndex = -1, parent = 0;
        for (int i = 0; i < (int)ctl->treeItems.size(); ++i)
            if (ctl->treeItems[i].id == id) { itemIndex = i; parent = ctl->treeItems[i].parent; break; }
        if (itemIndex < 0) return VMValue(0);
        auto subtreeEnd = [&](int start) {
            int end = start + 1;
            while (end < (int)ctl->treeItems.size()) {
                int ancestor = ctl->treeItems[end].parent;
                bool descendant = false;
                while (ancestor != 0) {
                    if (ancestor == ctl->treeItems[start].id) { descendant = true; break; }
                    auto it = std::find_if(ctl->treeItems.begin(), ctl->treeItems.end(),
                        [&](const GuiControl::TreeItem& item) { return item.id == ancestor; });
                    if (it == ctl->treeItems.end()) break;
                    ancestor = it->parent;
                }
                if (!descendant) break;
                ++end;
            }
            return end;
        };
        const int itemEnd = subtreeEnd(itemIndex);
        int next = -1;
        for (int i = itemEnd; i < (int)ctl->treeItems.size(); ++i)
            if (ctl->treeItems[i].parent == parent) { next = i; break; }
        if (next >= 0) {
            const int nextEnd = subtreeEnd(next);
            std::vector<GuiControl::TreeItem> itemBlock(ctl->treeItems.begin() + itemIndex,
                                                        ctl->treeItems.begin() + itemEnd);
            std::vector<GuiControl::TreeItem> nextBlock(ctl->treeItems.begin() + next,
                                                        ctl->treeItems.begin() + nextEnd);
            ctl->treeItems.erase(ctl->treeItems.begin() + itemIndex,
                                 ctl->treeItems.begin() + nextEnd);
            ctl->treeItems.insert(ctl->treeItems.begin() + itemIndex, nextBlock.begin(), nextBlock.end());
            ctl->treeItems.insert(ctl->treeItems.begin() + itemIndex + nextBlock.size(),
                                  itemBlock.begin(), itemBlock.end());
            return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("scrollToTop", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->scrollY = 0;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("scrollToBottom", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                float contentHeight = ctl->contentH;
                std::function<void(GuiControl*)> measure = [&](GuiControl* child) {
                    if (!child) return;
                    auto vectorIt = child->fields.find("messageVector");
                    if (vectorIt != child->fields.end()) {
                        if (auto* vector = ScriptEngine::instance().findObject(vectorIt->second.c_str()))
                            contentHeight = std::max(contentHeight,
                                vector->internals["__lineCount"].toInt() * 14.0f);
                    }
                    for (auto* nested : child->children) measure(nested);
                };
                measure(ctl);
                ctl->contentH = contentHeight;
                ctl->scrollY = std::max(0.0f, contentHeight - ctl->extentY);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->bitmap = args[1].toString();
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["bitmap"] = VMValue(ctl->bitmap);
            }
        }
        return VMValue(1);
    });
    auto hudSlot = [getListCtrl](const auto& args, int minArgs) -> std::pair<GuiControl*, int> {
        if ((int)args.size() < minArgs) return {nullptr, -1};
        auto* ctl = getListCtrl(args[0].toString());
        const int slot = args[1].toInt();
        if (!ctl || slot < 0 || slot >= 64) return {nullptr, -1};
        if ((int)ctl->hudSlots.size() <= slot) ctl->hudSlots.resize(slot + 1);
        return {ctl, slot};
    };
    tsInstance->registerNative("setWeaponBitmap", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        // Bitmap definitions cover every weapon type; only addWeapon marks a
        // slot visible when the player actually carries that weapon.
        ctl->hudSlots[slot].bitmap = args[2].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("setInventoryBitmap", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].bitmap = args[2].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("addWeapon", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].amount = args[2].toInt(); ctl->hudSlots[slot].visible = true;
        return VMValue(1);
    });
    tsInstance->registerNative("addInventory", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].amount = args[2].toInt(); ctl->hudSlots[slot].visible = true;
        return VMValue(1);
    });
    tsInstance->registerNative("removeWeapon", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 2);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].visible = false;
        return VMValue(1);
    });
    tsInstance->registerNative("removeInventory", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 2);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].visible = false;
        return VMValue(1);
    });
    tsInstance->registerNative("setAmmo", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].amount = args[2].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("setAmount", [hudSlot](const auto& args) -> VMValue {
        auto [ctl, slot] = hudSlot(args, 3);
        if (!ctl) return VMValue(0);
        ctl->hudSlots[slot].amount = args[2].toInt();
        return VMValue(1);
    });
    tsInstance->registerNative("setActiveWeapon", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* ctl = getListCtrl(args[0].toString())) {
            ctl->activeHudSlot = args[1].toInt();
            for (size_t i = 0; i < ctl->hudSlots.size(); ++i)
                ctl->hudSlots[i].active = (int)i == ctl->activeHudSlot;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setBackGroundBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) if (auto* ctl = getListCtrl(args[0].toString())) ctl->fields["backgroundBitmap"] = args[1].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("setHighLightBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) if (auto* ctl = getListCtrl(args[0].toString())) ctl->fields["highlightBitmap"] = args[1].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("setInfiniteAmmoBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) if (auto* ctl = getListCtrl(args[0].toString())) ctl->fields["infiniteAmmoBitmap"] = args[1].toString();
        return VMValue(1);
    });
    // HudWeaponInvBase::reset: no items and no active slot (recordings.cs
    // rebuilds the weapon and inventory HUDs from the demo settings).
    tsInstance->registerNative("reset", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl || !EngineClasses::isA(ctl->className, "HudWeaponInvBase")) return VMValue(0);
        ctl->hudSlots.clear();
        ctl->activeHudSlot = -1;
        return VMValue(1);
    });
    tsInstance->registerNative("setInfiniteAmountBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) if (auto* ctl = getListCtrl(args[0].toString())) ctl->fields["infiniteAmountBitmap"] = args[1].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("setActiveInventory", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* ctl = getListCtrl(args[0].toString())) {
            ctl->activeHudSlot = args[1].toInt();
            for (size_t i = 0; i < ctl->hudSlots.size(); ++i)
                ctl->hudSlots[i].active = (int)i == ctl->activeHudSlot;
        }
        return VMValue(1);
    });
    // GuiControl::isAwake: the control is in the canvas (its root is the
    // content or a pushed dialog).
    tsInstance->registerNative("isAwake", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& gui = Engine::instance().guiRenderer();
        GuiControl* ctl = gui.findControl(args[0].toString());
        if (!ctl) return VMValue(0);
        while (ctl->parent && ctl->parent->className != "GuiCanvas") ctl = ctl->parent;
        for (size_t i = 0; i < gui.dialogCount(); ++i)
            if (gui.getDialog(i) == ctl) return VMValue(1);
        return VMValue(0);
    });
    tsInstance->registerNative("clearAll", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) if (auto* ctl = getListCtrl(args[0].toString())) {
            ctl->hudSlots.clear(); ctl->activeHudSlot = -1;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setProfile", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->profileName = args[1].toString();
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["profile"] = VMValue(ctl->profileName);
            }
        }
        return VMValue(1);
    });
    // HudClock::setTime(minutes): the clock counts down from it;
    // getTime returns the minutes left.
    tsInstance->registerNative("setTime", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                const double minutes = args[1].toDouble();
                const double endSeconds = Engine::instance().timer().now() + minutes * 60.0;
                ctl->fields["clockEndSeconds"] = std::to_string(endSeconds);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("getTime", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = args.empty() ? nullptr : getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        auto end = ctl->fields.find("clockEndSeconds");
        if (end == ctl->fields.end()) return VMValue(0);
        const double left = std::atof(end->second.c_str()) - Engine::instance().timer().now();
        return VMValue((float)std::max(0.0, left / 60.0));
    });
    tsInstance->registerNative("setSeparators", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->fields["separators"] = args[1].toString();
        }
        return VMValue(1);
    });
    tsInstance->registerNative("enableHorzSeparator", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) {
            if (auto* ctl = getListCtrl(args[0].toString()))
                ctl->fields["horzSeparator"] = "1";
        }
        return VMValue(1);
    });
    tsInstance->registerNative("disableHorzSeparator", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) {
            if (auto* ctl = getListCtrl(args[0].toString()))
                ctl->fields["horzSeparator"] = "0";
        }
        return VMValue(1);
    });
    auto messageVectorCount = [](ScriptObject* vector) {
        return vector ? vector->internals["__lineCount"].toInt() : 0;
    };
    tsInstance->registerNative("MessageVector::getNumLines",
        [messageVectorCount](const auto& args) -> VMValue {
            if (args.empty()) return VMValue(0);
            return VMValue(messageVectorCount(
                ScriptEngine::instance().findObject(args[0].toString().c_str())));
        });
    tsInstance->registerNative("MessageVector::getLineText",
        [](const auto& args) -> VMValue {
            if (args.size() < 2) return VMValue("");
            auto* vector = ScriptEngine::instance().findObject(args[0].toString().c_str());
            const int index = args[1].toInt();
            if (!vector || index < 0 || index >= vector->internals["__lineCount"].toInt())
                return VMValue("");
            return vector->internals["__line" + std::to_string(index)];
        });
    tsInstance->registerNative("MessageVector::getLineTag",
        [](const auto& args) -> VMValue {
            if (args.size() < 2) return VMValue(0);
            auto* vector = ScriptEngine::instance().findObject(args[0].toString().c_str());
            const int index = args[1].toInt();
            if (!vector || index < 0 || index >= vector->internals["__lineCount"].toInt())
                return VMValue(0);
            return vector->internals["__lineTag" + std::to_string(index)];
        });
    tsInstance->registerNative("MessageVector::pushBackLine",
        [](const auto& args) -> VMValue {
            if (args.size() < 2) return VMValue(0);
            auto* vector = ScriptEngine::instance().findObject(args[0].toString().c_str());
            if (!vector) return VMValue(0);
            const int count = vector->internals["__lineCount"].toInt();
            vector->internals["__line" + std::to_string(count)] = args[1];
            vector->internals["__lineTag" + std::to_string(count)] =
                args.size() > 2 ? args[2] : VMValue(0);
            vector->internals["__lineCount"] = VMValue(count + 1);
            return VMValue(1);
        });
    tsInstance->registerNative("MessageVector::popFrontLine",
        [](const auto& args) -> VMValue {
            if (args.empty()) return VMValue(0);
            auto* vector = ScriptEngine::instance().findObject(args[0].toString().c_str());
            if (!vector) return VMValue(0);
            const int count = vector->internals["__lineCount"].toInt();
            if (count <= 0) return VMValue(1);
            for (int i = 1; i < count; ++i) {
                vector->internals["__line" + std::to_string(i - 1)] =
                    vector->internals["__line" + std::to_string(i)];
                vector->internals["__lineTag" + std::to_string(i - 1)] =
                    vector->internals["__lineTag" + std::to_string(i)];
            }
            vector->internals.erase("__line" + std::to_string(count - 1));
            vector->internals.erase("__lineTag" + std::to_string(count - 1));
            vector->internals["__lineCount"] = VMValue(count - 1);
            return VMValue(1);
        });
    tsInstance->registerNative("MessageVector::clear",
        [](const auto& args) -> VMValue {
            if (args.empty()) return VMValue(0);
            auto* vector = ScriptEngine::instance().findObject(args[0].toString().c_str());
            if (!vector) return VMValue(0);
            vector->internals.clear();
            vector->internals["__lineCount"] = VMValue(0);
            return VMValue(1);
        });
    tsInstance->registerNative("attach", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (auto* ctl = getListCtrl(args[0].toString()))
            ctl->fields["messageVector"] = args[1].toString();
        return VMValue(1);
    });
    // SDL's normal frame loop redraws continuously. Rebuilding the entire GUI
    // tree here made stock per-object loading callbacks do an O(controls)
    // refresh for every ghost during mission transfer.
    tsInstance->registerNative("repaint", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("getRowNumById", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(-1);
        int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); i++)
            if (ctl->listRowIds[i] == id) return VMValue((int32_t)i);
        return VMValue(-1);
    });
    tsInstance->registerNative("setRowColor", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                int row = args[1].toInt();
                std::string color = args[2].toString();
                ctl->fields["rowColor" + std::to_string(row)] = color;
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setRowStyle", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                int row = args[1].toInt();
                std::string style = args[2].toString();
                ctl->fields["rowStyle" + std::to_string(row)] = style;
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addStyleSet", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 4) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const std::string prefix = "styleSet" + std::to_string(args[1].toInt());
        ctl->fields[prefix + ".fontType"] = args[2].toString();
        ctl->fields[prefix + ".fontSize"] = args[3].toString();
        if (args.size() > 4) ctl->fields[prefix + ".fontColor"] = args[4].toString();
        if (args.size() > 5) ctl->fields[prefix + ".fontColorHL"] = args[5].toString();
        if (args.size() > 6) ctl->fields[prefix + ".fontColorSEL"] = args[6].toString();
        return VMValue(1);
    });
    // ShellFancyArray::addStyle(id, font, size, color, colorHL, colorSEL):
    // the same style record a row selects with setRowStyle.
    tsInstance->registerNative("addStyle", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 4) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const std::string prefix = "styleSet" + std::to_string(args[1].toInt());
        ctl->fields[prefix + ".fontType"] = args[2].toString();
        ctl->fields[prefix + ".fontSize"] = args[3].toString();
        if (args.size() > 4) ctl->fields[prefix + ".fontColor"] = args[4].toString();
        if (args.size() > 5) ctl->fields[prefix + ".fontColorHL"] = args[5].toString();
        if (args.size() > 6) ctl->fields[prefix + ".fontColorSEL"] = args[6].toString();
        return VMValue(1);
    });
    tsInstance->registerNative("setRowStyleById", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); ++i) {
            if (ctl->listRowIds[i] == id) {
                ctl->fields["rowStyle" + std::to_string(i)] = args[2].toString();
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("setRowColorById", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); ++i) {
            if (ctl->listRowIds[i] == id) {
                ctl->fields["rowColor" + std::to_string(i)] = args[2].toString();
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("removeRow", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int row = args[1].toInt();
        if (row < 0 || row >= (int)ctl->listRows.size()) return VMValue(0);
        const int oldCount = (int)ctl->listRows.size();
        ctl->listRows.erase(ctl->listRows.begin() + row);
        if (row < (int)ctl->listRowIds.size()) ctl->listRowIds.erase(ctl->listRowIds.begin() + row);
        for (int i = row; i < oldCount - 1; ++i) {
            for (const char* prefix : {"rowColor", "rowStyle"}) {
                const std::string from = std::string(prefix) + std::to_string(i + 1);
                const std::string to = std::string(prefix) + std::to_string(i);
                auto value = ctl->fields.find(from);
                if (value == ctl->fields.end()) ctl->fields.erase(to);
                else ctl->fields[to] = value->second;
            }
        }
        ctl->fields.erase("rowColor" + std::to_string(oldCount - 1));
        ctl->fields.erase("rowStyle" + std::to_string(oldCount - 1));
        if (ctl->selectedRow == row) ctl->selectedRow = -1;
        else if (ctl->selectedRow > row) --ctl->selectedRow;
        return VMValue(1);
    });
    tsInstance->registerNative("setText", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->text = args[1].toString();
                if (auto* obj = ScriptEngine::instance().findObject(ctl->name.c_str()))
                    obj->fields["text"] = VMValue(ctl->text);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("cancelServerQuery", [](const auto&) -> VMValue {
        Engine::instance().network().stopServerQuery();
        return VMValue(1);
    });
    tsInstance->registerNative("addColumn", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 3) {
            const int id = args[1].toInt();
            const std::string colName = args[2].toString();
            const float colWidth = args.size() > 3 ? (float)args[3].toDouble() : 0.0f;
            const float minWidth = args.size() > 4 ? (float)args[4].toDouble() : colWidth;
            const float maxWidth = args.size() > 5 ? (float)args[5].toDouble() : colWidth;
            const std::string format = args.size() > 6 ? args[6].toString() : "";
            ctl->listColumns.push_back({id, colName, colWidth, minWidth, maxWidth, format});
            ctl->sbColumns.push_back({colName, colWidth, true});
        }
        return VMValue(1);
    });
    // ShellFancyArray::clearColumns (LobbyPlayerList is a ShellFancyTextList):
    // the columns go; initColumns adds them
    // again (LobbyPlayerList).
    tsInstance->registerNative("clearColumns", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl) {
            ctl->listColumns.clear();
            ctl->sbColumns.clear();
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setSortColumn", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            std::string colName = args[1].toString();
            for (size_t i = 0; i < ctl->sbColumns.size(); i++)
                if (ctl->sbColumns[i].name == colName) { ctl->sbSortCol = (int)i; break; }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setSortIncreasing", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) ctl->sbSortInc = args[1].toBool();
        return VMValue(1);
    });

    // Chat system — store chat messages for HUD rendering
    static std::vector<std::string> s_chatMessages;
    tsInstance->registerNative("addChat", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            std::string msg = args[0].toString() + ": " + args[1].toString();
            s_chatMessages.push_back(msg);
            if (s_chatMessages.size() > 100) s_chatMessages.erase(s_chatMessages.begin());
            Console::instance().setVariable("HUD::lastChat", msg.c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("installChatItem", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            Console::instance().printf(LogLevel::Debug, "installChatItem: %s = %s", args[0].toString().c_str(), args[1].toString().c_str());
            Console::instance().setVariable(("HUD::chatItem::" + args[0].toString()).c_str(), args[1].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("startChatMenu", [](const auto&) -> VMValue {
        Console::instance().setVariable("HUD::chatOpen", "1");
        return VMValue(1);
    });
    tsInstance->registerNative("endChatMenu", [](const auto&) -> VMValue {
        Console::instance().setVariable("HUD::chatOpen", "0");
        return VMValue(1);
    });
    tsInstance->registerNative("ChatRoomMemberList_refresh", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Debug, "ChatRoomMemberList_refresh: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("ChannelBannedList_refresh", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Debug, "ChannelBannedList_refresh: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("createFlagTossGauge", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Debug, "createFlagTossGauge: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("cancelChatMenu", [](const auto&) -> VMValue {
        Console::instance().setVariable("HUD::chatOpen", "0");
        return VMValue(1);
    });
    tsInstance->registerNative("setChatPage", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("HUD::chatPage", args[0].toString().c_str());
        return VMValue(1);
    });

    // loadGui — look up and call the TS function (natives take priority over TS functions)
    tsInstance->registerNative("loadGui", [](const auto& args) -> VMValue {
        auto* ts = Engine::instance().script().ts();
        if (ts) {
            std::string guiName = args.empty() ? "" : args.back().toString();
            if (!guiName.empty()) {
                std::string execPath = "gui/" + guiName + ".gui";
                auto data = Engine::instance().fs().read(execPath.c_str());
                if (data.empty()) data = Engine::instance().fs().read(("base/" + execPath).c_str());
                if (!data.empty()) {
                    std::string src((const char*)data.data(), data.size());
                    ts->executeNested(src, execPath);
                }
            }
        }
        return VMValue(1);
    });

    tsInstance->registerNative("alxStopAll", [](const auto&) -> VMValue {
        auto& audio = Engine::instance().audio();
        audio.stopAll();
        std::set<SoundSource*> scriptSources;
        for (const auto& [name, source] : s_audioSources)
            if (audio.isSourceAlive(source)) scriptSources.insert(source);
        for (auto* source : scriptSources) audio.releaseSource(source);
        s_audioSources.clear();
        s_audioBuffers.clear();
        s_nextAudioHandle = 1;
        return VMValue(1);
    });
    tsInstance->registerNative("alxDestroySource", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it == s_audioSources.end()) return VMValue(0);
        auto* source = it->second;
        auto& audio = Engine::instance().audio();
        for (auto alias = s_audioSources.begin(); alias != s_audioSources.end(); ) {
            if (alias->second == source) {
                s_audioBuffers.erase(alias->first);
                alias = s_audioSources.erase(alias);
            } else ++alias;
        }
        if (audio.isSourceAlive(source)) audio.releaseSource(source);
        return VMValue(1);
    });
    tsInstance->registerNative("alxPause", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_audioSources.find(args[0].toString());
        if (it == s_audioSources.end() || !Engine::instance().audio().isSourceAlive(it->second)) return VMValue(0);
        it->second->pause();
        return VMValue(1);
    });
    tsInstance->registerNative("alxResume", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto it = s_audioSources.find(args[0].toString());
        if (it == s_audioSources.end() || !Engine::instance().audio().isSourceAlive(it->second)) return VMValue(0);
        it->second->resume();
        return VMValue(1);
    });
    tsInstance->registerNative("alxListenerf", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const std::string param = args[0].toString();
        if (param == "AL_GAIN_LINEAR" || param == "AL_GAIN")
            Engine::instance().audio().setListenerGain((float)args[1].toDouble());
        return VMValue(1);
    });
    tsInstance->registerNative("alxListener3f", [](const auto& args) -> VMValue {
        if (args.size() < 4) return VMValue(0);
        const std::string param = args[0].toString();
        if (param == "AL_POSITION" || param == "AL_VELOCITY") {
            const Point3F value{args[1].toFloat(), args[2].toFloat(), args[3].toFloat()};
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
                return VMValue(0);
            if (param == "AL_POSITION") Engine::instance().audio().setListenerPosition(value);
            else Engine::instance().audio().setListenerVelocity(value);
        }
        return VMValue((param == "AL_POSITION" || param == "AL_VELOCITY") ? 1 : 0);
    });
    tsInstance->registerNative("alxListenerfv", [](const auto& args) -> VMValue {
        if (args.size() == 2) {
            float values[6]{};
            if (sscanf(args[1].toString().c_str(), "%f %f %f %f %f %f",
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6)
                return VMValue(0);
            const Point3F forward{values[0], values[1], values[2]};
            const Point3F up{values[3], values[4], values[5]};
            if (args[0].toString() != "AL_ORIENTATION") return VMValue(0);
            Engine::instance().audio().setListenerOrientation(forward, up);
            return VMValue(1);
        }
        if (args.size() != 7 || args[0].toString() != "AL_ORIENTATION") return VMValue(0);
        const Point3F forward{args[1].toFloat(), args[2].toFloat(), args[3].toFloat()};
        const Point3F up{args[4].toFloat(), args[5].toFloat(), args[6].toFloat()};
        if (!std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z) ||
            !std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z)) return VMValue(0);
        Engine::instance().audio().setListenerOrientation(forward, up);
        return VMValue(1);
    });
    tsInstance->registerNative("alxEnableEnvironmental", [](const auto& args) -> VMValue {
        Engine::instance().audio().setEnvironmental(!args.empty() && args[0].toBool());
        return VMValue(1);
    });
    tsInstance->registerNative("alxEnvironmenti", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        return VMValue(Engine::instance().audio().setEnvironmenti(
            args[0].toString(), args[1].toInt()) ? 1 : 0);
    });
    tsInstance->registerNative("alxEnvironmentf", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        return VMValue(Engine::instance().audio().setEnvironmentf(
            args[0].toString(), args[1].toFloat()) ? 1 : 0);
    });
    tsInstance->registerNative("alxGetEnvironmenti", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue(Engine::instance().audio().environmenti(args[0].toString()));
    });
    tsInstance->registerNative("alxGetEnvironmentf", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0f);
        return VMValue(Engine::instance().audio().environmentf(args[0].toString()));
    });
    // alx context queries feed the Settings→Audio page. Scripts pass bare
    // ALC_* identifiers, which the interpreter evaluates to their own name
    // strings, so we dispatch on args[0] here. One dummy provider/speaker
    // keeps the provider/speaker dropdowns populated and functional.
    static int s_audioProvider = 0;
    static int s_audioSpeaker = 0;
    tsInstance->registerNative("alxGetContexti", [](const auto& args) -> VMValue {
        std::string which = args.empty() ? "" : args[0].toString();
        if (which == "ALC_PROVIDER_COUNT" || which == "ALC_SPEAKER_COUNT") return VMValue(1);
        if (which == "ALC_SPEAKER") return VMValue(s_audioSpeaker);
        return VMValue(s_audioProvider);
    });
    tsInstance->registerNative("alxGetContextstr", [](const auto& args) -> VMValue {
        std::string which = args.empty() ? "" : args[0].toString();
        int idx = args.size() > 1 ? (int)args[1].toDouble() : 0;
        if (which == "ALC_SPEAKER_NAME") {
            if (idx <= 0) return VMValue("Generic Speakers");
            return VMValue("Generic Speakers %" + std::to_string(idx));
        }
        if (idx <= 0) return VMValue("Generic Software");
        return VMValue("Generic Software %" + std::to_string(idx));
    });
    tsInstance->registerNative("alxContexti", [](const auto& args) -> VMValue {
        // Setter: ALC_PROVIDER / ALC_SPEAKER / ALC_BUFFER_DYNAMIC_MEMORY_SIZE
        std::string which = args.empty() ? "" : args[0].toString();
        int val = args.size() > 1 ? (int)args[1].toDouble() : 0;
        if (which == "ALC_PROVIDER") s_audioProvider = val;
        else if (which == "ALC_SPEAKER") s_audioSpeaker = val;
        return VMValue(1);
    });
    tsInstance->registerNative("alxSetCaptureGainScale", [](const auto& args) -> VMValue {
        if (!args.empty())
            Engine::instance().audio().setCaptureGainScale(args[0].toFloat());
        return VMValue("");
    });
    tsInstance->registerNative("alxGetCaptureGainScale", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().captureGainScale());
    });
    tsInstance->registerNative("alxCaptureInit", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().initCapture() ? 1 : 0);
    });
    tsInstance->registerNative("alxCaptureDestroy", [](const auto&) -> VMValue {
        Engine::instance().game().stopVoiceCapture();
        Engine::instance().audio().destroyCapture();
        return VMValue("");
    });
    tsInstance->registerNative("alxCaptureStart", [](const auto& args) -> VMValue {
        const bool local = !args.empty() && args[0].toBool();
        return VMValue(Engine::instance().game().startVoiceCapture(local) ? 1 : 0);
    });
    tsInstance->registerNative("alxCaptureStop", [](const auto&) -> VMValue {
        Engine::instance().game().stopVoiceCapture();
        return VMValue("");
    });
    tsInstance->registerNative("alxIsCapturing", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isCapturing() ? 1 : 0);
    });
    // audio.cc: forced outer falloffs and the inner falloff scale.
    tsInstance->registerNative("alxDisableOuterFalloffs", [](const auto& args) -> VMValue {
        const std::string value = args.empty() ? std::string() : args[0].toString();
        // dAtob: "true" or a non-zero number.
        Engine::instance().audio().disableOuterFalloffs(strcasecmp(value.c_str(), "true") == 0 ||
                                                        std::atof(value.c_str()) != 0.0);
        return VMValue("");
    });
    tsInstance->registerNative("alxSetInnerFalloffScale", [](const auto& args) -> VMValue {
        Engine::instance().audio().setInnerFalloffScale(args.empty() ? 1.0f : args[0].toFloat());
        return VMValue("");
    });
    tsInstance->registerNative("alxGetInnerFalloffScale", [](const auto&) -> VMValue {
        return VMValue(AudioSystem::innerFalloffScale());
    });
    // alxSetChannelVolume(channel, volume) / alxGetChannelVolume(channel):
    // the channel is an Audio::AudioTypes value ($MusicAudioType...).
    tsInstance->registerNative("alxSetChannelVolume", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const int type = std::atoi(args[0].toString().c_str());
        if (!Engine::instance().audio().setChannelVolume(type, (float)std::atof(args[1].toString().c_str())))
            Console::instance().printf(LogLevel::Error, "cAudio_alxSetChannelVolume: invalid channel '%d'", type);
        return VMValue("");
    });
    tsInstance->registerNative("alxGetChannelVolume", [](const auto& args) -> VMValue {
        const int type = args.empty() ? 0 : std::atoi(args[0].toString().c_str());
        if (type < 0 || type >= AudioSystem::NumAudioTypes) {
            Console::instance().printf(LogLevel::Error, "cAudio_alxGetChannelVolume: invalid channel '%d'", type);
            return VMValue(0.0f);
        }
        return VMValue(Engine::instance().audio().channelVolume(type));
    });
    // alxPlayMusic(file): the shipped client streams the file (a game
    // directory path, "base\\music\\lush.mp3"); finishedMusicStream(stopped)
    // follows its end or a stop.
    tsInstance->registerNative("alxPlayMusic", [this](const auto& args) -> VMValue {
        std::string path = args.empty() ? std::string() : args[0].toString();
        for (char& c : path) if (c == '\\') c = '/';
        auto& audio = Engine::instance().audio();
        audio.onMusicFinished = [this](bool stopped) {
            if (tsInstance) tsInstance->callFunction("finishedMusicStream", {VMValue(stopped ? "true" : "false")});
        };
        audio.playMusic(path);
        return VMValue("");
    });
    tsInstance->registerNative("alxStopMusic", [](const auto&) -> VMValue {
        Engine::instance().audio().stopMusic();
        return VMValue("");
    });
    tsInstance->registerNative("alxCreateSource", [audioProfilePath, audioProfileSettings](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string name = args[0].toString();
        std::string soundName = args[1].toString();
        const std::string profileName = soundName;
        const std::string profilePath = audioProfilePath(soundName);
        if (!profilePath.empty()) soundName = profilePath;
        auto& audio = Engine::instance().audio();
        SoundBuffer* buf = nullptr;
        auto tryLoad = [&](const std::string& path) {
            buf = audio.loadSound(path.c_str());
            return buf != nullptr;
        };
        std::string path = "sound/" + soundName + ".wav";
        if (!tryLoad(path)) { path = "sound/" + soundName + ".ogg"; tryLoad(path); }
        // Full relative paths (e.g. "voice/Male1/gbl.hi.wav" from voice.vl2)
        if (!buf && soundName.find('/') != std::string::npos) {
            tryLoad(soundName);
            if (!buf) {
                std::string lower = soundName;
                for (auto& c : lower) c = (char)tolower((unsigned char)c);
                tryLoad(lower);
            }
            if (buf) path = soundName;
        }
        if (!buf) {
            for (auto& c : soundName) c = (char)tolower((unsigned char)c);
            path = "sound/" + soundName + ".wav";
            if (!tryLoad(path)) { path = "sound/" + soundName + ".ogg"; tryLoad(path); }
        }
        if (!buf) return VMValue(0);
        SoundSource* src = audio.createSource(true);
        if (!src) return VMValue(0);
        // alxCreateSource allocates a stopped, reusable source.
        audioProfileSettings(profileName, *src);
        auto old = s_audioSources.find(name);
        if (old != s_audioSources.end()) {
            SoundSource* oldSource = old->second;
            for (auto alias = s_audioSources.begin(); alias != s_audioSources.end(); ) {
                if (alias->second == oldSource) {
                    s_audioBuffers.erase(alias->first);
                    alias = s_audioSources.erase(alias);
                } else {
                    ++alias;
                }
            }
            if (audio.isSourceAlive(oldSource)) audio.releaseSource(oldSource);
        }
        s_audioSources[name] = src;
        s_audioBuffers[name] = buf;
        // Numeric handle so script-side alxPlay(%handle) can resolve the source
        int h = s_nextAudioHandle++;
        s_audioSources[std::to_string(h)] = src;
        s_audioBuffers[std::to_string(h)] = buf;
        return VMValue((double)h);
    });
    tsInstance->registerNative("alxGetWaveLen", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it != s_audioSources.end()) {
            if (!Engine::instance().audio().isSourceAlive(it->second)) {
                s_audioSources.erase(it);
                s_audioBuffers.erase(name);
                it = s_audioSources.end();
            }
        }
        if (it != s_audioSources.end()) {
            auto buffer = s_audioBuffers.find(name);
            if (buffer != s_audioBuffers.end()) return VMValue((int)buffer->second->durationMs);
        }
        auto& audio = Engine::instance().audio();
        SoundBuffer* buffer = audio.loadSound(name.c_str());
        if (!buffer && name.rfind("audio/", 0) != 0)
            buffer = audio.loadSound(("audio/" + name).c_str());
        return buffer ? VMValue((int)buffer->durationMs) : VMValue(0);
    });
    tsInstance->registerNative("alxSourcef", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(1);
        std::string name = args[0].toString();
        std::string param = args[1].toString();
        float val = (float)args[2].toDouble();
        auto it = s_audioSources.find(name);
        if (it == s_audioSources.end()) return VMValue(1);
        if (!Engine::instance().audio().isSourceAlive(it->second)) {
            s_audioSources.erase(it);
            s_audioBuffers.erase(name);
            return VMValue(0);
        }
        auto* src = it->second;
        if (param == "volume" || param == "AL_GAIN") src->setVolume(val);
        else if (param == "pitch" || param == "AL_PITCH") src->setPitch(val);
        else if (param == "looping" || param == "AL_LOOPING") src->setLooping(val != 0);
        else if (param == "AL_REFERENCE_DISTANCE" || param == "minDistance")
            src->setDistance(val, src->maxDistance);
        else if (param == "AL_MAX_DISTANCE" || param == "maxDistance")
            src->setDistance(src->referenceDistance, val);
        else if (param == "rolloff" || param == "rolloffFactor" || param == "AL_ROLLOFF_FACTOR")
            src->setRolloff(val);
        else if (param == "offset" || param == "AL_SEC_OFFSET") {
            const auto buffer = s_audioBuffers.find(name);
            const uint32_t duration = buffer == s_audioBuffers.end()
                ? 0 : buffer->second->durationMs;
            src->setOffsetSeconds(SoundSource::clampOffsetSeconds(val, duration));
        }
        return VMValue(1);
    });
    tsInstance->registerNative("alxSource3f", [](const auto& args) -> VMValue {
        if (args.size() < 5) return VMValue(0);
        const std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it == s_audioSources.end() || !Engine::instance().audio().isSourceAlive(it->second))
            return VMValue(0);
        const Point3F value{args[2].toFloat(), args[3].toFloat(), args[4].toFloat()};
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            return VMValue(0);
        if (args[1].toString() == "AL_POSITION") it->second->setPosition(value);
        else if (args[1].toString() == "AL_VELOCITY") it->second->setVelocity(value);
        return VMValue(1);
    });
    tsInstance->registerNative("alxSourcei", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto it = s_audioSources.find(args[0].toString());
        if (it == s_audioSources.end() || !Engine::instance().audio().isSourceAlive(it->second))
            return VMValue(0);
        const std::string param = args[1].toString();
        if (param == "AL_LOOPING") it->second->setLooping(args[2].toBool());
        else if (param == "AL_SOURCE_RELATIVE") it->second->setRelative(args[2].toBool());
        else return VMValue(0);
        return VMValue(1);
    });
    tsInstance->registerNative("alxSource3i", [](const auto& args) -> VMValue {
        if (args.size() < 5) return VMValue(0);
        auto it = s_audioSources.find(args[0].toString());
        if (it == s_audioSources.end() || !Engine::instance().audio().isSourceAlive(it->second)) return VMValue(0);
        if (args[1].toString() != "AL_AUXILIARY_SEND_FILTER") return VMValue(0);
        const int send = args[3].toInt();
        const int filter = args[4].toInt();
        if (send < 0 || filter < 0) return VMValue(0);
        it->second->setAuxiliarySend((uint32_t)args[2].toInt(), send, (uint32_t)filter);
        return VMValue(1);
    });
    tsInstance->registerNative("alxStop", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(1);
        std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it != s_audioSources.end()) {
            if (Engine::instance().audio().isSourceAlive(it->second)) it->second->stop();
            else { s_audioSources.erase(it); s_audioBuffers.erase(name); }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("playAudio", [audioProfilePath](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        const std::string object = args[0].toString();
        const std::string channel = args[1].toString();
        std::string path = audioProfilePath(args[2].toString());
        if (path.empty()) return VMValue(0);
        auto& audio = Engine::instance().audio();
        auto* buffer = audio.loadSound(path.c_str());
        if (!buffer) return VMValue(0);
        const std::string key = object + ":" + channel;
        auto old = s_audioSources.find(key);
        if (old != s_audioSources.end() && audio.isSourceAlive(old->second)) {
            old->second->stop();
            audio.releaseSource(old->second);
            s_audioSources.erase(old);
            s_audioBuffers.erase(key);
        }
        auto* source = audio.createSource(true);
        if (!source) return VMValue(0);
        source->play(buffer);
        s_audioSources[key] = source;
        s_audioBuffers[key] = buffer;
        return VMValue(1);
    });
    tsInstance->registerNative("stopAudio", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const std::string prefix = args[0].toString() + ":" + args[1].toString();
        auto it = s_audioSources.find(prefix);
        if (it == s_audioSources.end()) return VMValue(1);
        if (Engine::instance().audio().isSourceAlive(it->second)) {
            it->second->stop();
            Engine::instance().audio().releaseSource(it->second);
        }
        s_audioSources.erase(it);
        s_audioBuffers.erase(prefix);
        return VMValue(1);
    });

    tsInstance->registerNative("alxEnableForceFeedback", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("setGravity", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        Engine::instance().game().setGravity(args[0].toFloat());
        return VMValue(1);
    });
    tsInstance->registerNative("setTimeScale", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        Engine::instance().game().setTimeScale(args[0].toFloat());
        return VMValue(1);
    });
    tsInstance->registerNative("setSensorGroupCount", [](const auto& args) -> VMValue {
        if (args.empty() || args[0].toInt() < 0 || args[0].toInt() > 32) return VMValue(0);
        Engine::instance().game().setSensorGroupCount(args[0].toInt());
        return VMValue(1);
    });
    tsInstance->registerNative("setSensorGroupListenMask", [](const auto& args) -> VMValue {
        if (args.size() < 2 || args[0].toInt() < 0 || args[0].toInt() >= 32) return VMValue(0);
        char* end = nullptr;
        const std::string text = args[1].toString();
        const unsigned long mask = std::strtoul(text.c_str(), &end, 0);
        if (!end || *end != '\0' || mask > 0xfffffffful) return VMValue(0);
        Engine::instance().game().setSensorGroupListenMask(args[0].toInt(),
                                                            (uint32_t)mask);
        return VMValue(1);
    });
    tsInstance->registerNative("setSensorGroupFriendlyMask", [](const auto& args) -> VMValue {
        if (args.size() < 2 || args[0].toInt() < 0 || args[0].toInt() >= 32) return VMValue(0);
        char* end = nullptr;
        const std::string text = args[1].toString();
        const unsigned long mask = std::strtoul(text.c_str(), &end, 0);
        if (!end || *end != '\0' || mask > 0xfffffffful) return VMValue(0);
        Engine::instance().game().setSensorGroupFriendlyMask(args[0].toInt(),
                                                              (uint32_t)mask);
        return VMValue(1);
    });
    tsInstance->registerNative("setTargetFriendlyMask", [](const auto& args) -> VMValue {
        if (args.size() < 2 || args[0].toInt() < 0 || args[0].toInt() >= 32) return VMValue(0);
        char* end = nullptr;
        const std::string text = args[1].toString();
        const unsigned long mask = std::strtoul(text.c_str(), &end, 0);
        if (!end || *end != '\0' || mask > 0xfffffffful) return VMValue(0);
        Engine::instance().game().setTargetFriendlyMask(args[0].toInt(),
                                                         (uint32_t)mask);
        return VMValue(1);
    });
    tsInstance->registerNative("setSensorGroupColor", [](const auto& args) -> VMValue {
        if (args.size() < 3 || args[0].toInt() < 0 || args[0].toInt() >= 32) return VMValue(0);
        std::istringstream color(args[2].toString());
        ColorF value{};
        if (!(color >> value.r >> value.g >> value.b >> value.a)) return VMValue(0);
        if (value.a > 1.0f) {
            value.r /= 255.0f; value.g /= 255.0f;
            value.b /= 255.0f; value.a /= 255.0f;
        }
        Engine::instance().game().setSensorGroupColor(args[0].toInt(),
                                                       (uint32_t)args[1].toInt(), value);
        return VMValue(1);
    });
    tsInstance->registerNative("setTargetSensorData", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto it = s_scriptTargets.find(args[0].toInt());
        if (it == s_scriptTargets.end()) return VMValue(0);
        it->second.sensorData = args[1];
        return VMValue(1);
    });
    // WON/Login stubs — store login info so login flow can proceed
    // WONLoginResult: "status \t code \t codeText \t error" for the
    // StartupGui::checkLoginDone poll. There is no WON account service, so an
    // offline login completes at once and the stock success path
    // (LoginDone -> CleanUpAndGo -> console_end.cs) takes over.
    tsInstance->registerNative("WONLoginResult", [](const auto&) -> VMValue {
        return VMValue(std::string("OK\t0\t\t"));
    });
    tsInstance->registerNative("WONServerLogin", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            Console::instance().setVariable("WON::username", args[0].toString().c_str());
            Console::instance().setVariable("WON::password", args[1].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("WONStartLogin", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("WONStartEmailFetch", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("WONStartCreateAccount", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("WONStartUpdateAccount", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("WONStartLoginInfoFetch", [](const auto&) -> VMValue { return VMValue(1); });

    // Journal stubs — journal files store demo/input replay data
    tsInstance->registerNative("loadJournal", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Info, "loadJournal: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("saveJournal", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Info, "saveJournal: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("playJournal", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().printf(LogLevel::Info, "playJournal: %s", args[0].toString().c_str());
        return VMValue(1);
    });

    // Recordings screen queries. .rec header layout (see demo.cpp):
    //   U8 strlen + "Tribes2 Recording" + U32 protocol + U32 lengthMs + U32 ibSize
    tsInstance->registerNative("getDemoVersion", [](const auto&) -> VMValue {
        // Protocol version this engine records/plays (matches .rec writer + DemoParser).
        return VMValue((int32_t)T2Demo::ProtocolV25034);
    });
    tsInstance->registerNative("getDemoVersionLength", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("-1\t-1");
        auto data = Engine::instance().fs().read(args[0].toString().c_str());
        static const char* sig = "Tribes2 Recording";
        if (data.size() < 26 || data[0] != (uint8_t)strlen(sig) ||
            std::string((const char*)data.data() + 1, strlen(sig)) != sig)
            return VMValue("-1\t-1");
        auto u32 = [&](size_t off) -> uint32_t {
            return (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8) |
                   ((uint32_t)data[off + 2] << 16) | ((uint32_t)data[off + 3] << 24);
        };
        size_t p = 1 + strlen(sig);
        return VMValue(std::to_string(u32(p)) + "\t" + std::to_string(u32(p + 4)));
    });

    // EffectProfile is called by audio scripts
    tsInstance->registerNative("EffectProfile", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            std::string name = args[0].toString();
            std::string props = args[1].toString();
            Console::instance().printf(LogLevel::Debug, "EffectProfile: %s = %s", name.c_str(), props.c_str());
            Console::instance().setVariable(("SFX::" + name).c_str(), props.c_str());
        }
        return VMValue(1);
    });

    // addMaterialMapping is called by material scripts
    tsInstance->registerNative("addMaterialMapping", [](const auto& args) -> VMValue {
        std::vector<std::string> values;
        for (const auto& arg : args) values.push_back(arg.toString());
        MaterialPropertyMap::instance().addMapping(values);
        return VMValue(1);
    });

    // Networking: route commands through the game's connection
    tsInstance->registerNative("commandToClient", [](const auto& args) -> VMValue {
        // commandToClient(client, funcName, arg1, arg2, ...)
        if (args.size() < 2) return VMValue(0);
        std::string func = args[1].toString();
        std::vector<std::string> callbackArgs;
        for (size_t i = 2; i < args.size(); i++) callbackArgs.push_back(args[i].toString());
        Console::instance().printf(LogLevel::Debug, "commandToClient: %s (%zu args)",
                                   func.c_str(), callbackArgs.size());
        // Send over wire if connected (server to client)
        auto* conn = Engine::instance().game().activeConnection();
        if (conn && conn->isConnected()) {
            conn->sendRemoteCommand(func, callbackArgs);
        } else {
            // Local fallback: execute directly
            auto* ts = Engine::instance().script().ts();
            if (ts && ts->hasFunction(func)) {
                std::vector<VMValue> callbackArgs;
                for (size_t i = 2; i < args.size(); ++i)
                    callbackArgs.push_back(args[i]);
                ts->callFunction(func, callbackArgs);
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("commandToServer", [](const auto& args) -> VMValue {
        // commandToServer(funcName, arg1, arg2, ...)
        if (args.empty()) return VMValue(0);
        // game/net.cc: a RemoteCommandEvent on the ServerConnection.
        if (auto* server = EngineObjects::get<GameConnection>("ServerConnection"); server && !server->isServer) {
            std::vector<std::string> argv;
            const std::string command = args[0].toString();
            argv.push_back(NetStrings::isTag(command) ? command : NetStrings::literal(command));
            for (size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].toString());
            server->sendRemoteCommand(argv);
            return VMValue("");
        }
        std::string func = args[0].toString();
        std::vector<std::string> callbackArgs;
        for (size_t i = 1; i < args.size(); i++) callbackArgs.push_back(args[i].toString());
        std::string cmd = func;
        for (size_t i = 1; i < args.size(); i++)
            cmd += " " + args[i].toString();
        Console::instance().printf(LogLevel::Debug, "commandToServer: %s", cmd.c_str());
        if (func == "getScores") return VMValue(1);
        // Send over wire if connected (client to server)
        auto* conn = Engine::instance().game().activeConnection();
        if (conn && conn->isConnected()) {
            conn->sendRemoteCommand(func, callbackArgs);
        } else {
            // Local commands do not need to be reparsed as TorqueScript source.
            // In particular, `cycleWeapon next` is console command syntax, not
            // a valid TorqueScript function call.
            if (auto* item = Console::instance().find(func.c_str()); item && item->cmd) {
                std::vector<std::string> words{func};
                for (size_t i = 1; i < args.size(); i++) words.push_back(args[i].toString());
                std::vector<const char*> argv;
                for (auto& word : words) argv.push_back(word.c_str());
                item->cmd((int32_t)argv.size(), argv.data());
            } else {
                Console::instance().execute(cmd.c_str());
            }
        }
        return VMValue(1);
    });

    // Server browser networking
    tsInstance->registerNative("queryLanServers", [](const auto& args) -> VMValue {
        auto& net = Engine::instance().network();
        net.queryLanServers();
        return VMValue(1);
    });
    tsInstance->registerNative("stopServerQuery", [](const auto&) -> VMValue {
        Engine::instance().network().stopServerQuery();
        return VMValue(1);
    });
    tsInstance->registerNative("getLiveMissionCRC", [](const auto&) -> VMValue {
        return VMValue((int32_t)Engine::instance().game().getLiveMissionCrc());
    });
    tsInstance->registerNative("getLiveTargetInfo", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const int targetId = std::clamp(args[0].toInt(), 0, 0x1ff);
        const auto* target = Engine::instance().game().getLiveTarget(
            (uint16_t)targetId);
        if (!target) return VMValue("");
        return VMValue(target->name + "\t" + target->skin + "\t" + target->type);
    });
    tsInstance->registerNative("queryMasterServer", [](const auto& args) -> VMValue {
        std::string masterUrl = args.empty() ? "" : args[0].toString();
        bool numericLegacyPort = !masterUrl.empty();
        for (unsigned char c : masterUrl)
            if (c < '0' || c > '9') { numericLegacyPort = false; break; }
        if (numericLegacyPort) masterUrl.clear();
        if (!masterUrl.empty()) {
            Console::instance().printf(LogLevel::Info, "queryMasterServer: %s", masterUrl.c_str());
            Engine::instance().network().queryMasterServer(masterUrl.c_str());
        } else {
            const char* envMaster = getenv("TORCH_MASTER_SERVER");
            const std::string selected = selectMasterServerUrl(
                Engine::instance().demoMode,
                "",
                Console::instance().getStringVariable("demoMasterServer", ""),
                envMaster ? envMaster : "");
            if (selected.empty() && Engine::instance().demoMode)
                Console::instance().printf(LogLevel::Warn,
                    "Demo master server is empty; falling back to LAN discovery");
            Engine::instance().network().queryMasterServer(selected.c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addRow", [getListCtrl](const auto& args) -> VMValue {
        std::string cname = args.empty() ? "" : args[0].toString();
        auto* ctl = getListCtrl(cname);
        if (ctl && args.size() >= 3) {
            int id = args[1].toInt();
            if (id < 0 || id > 65535) return VMValue(1); // reject absurd indices (alloc/overflow guard)
            std::string txt = args[2].toString();
            size_t insertAt = ctl->listRows.size();
            if (args.size() >= 4)
                insertAt = std::min<size_t>(std::max(0, args[3].toInt()), ctl->listRows.size());
            for (int row = (int)ctl->listRows.size(); row > (int)insertAt; --row) {
                for (const char* prefix : {"rowColor", "rowStyle", "rowActive"}) {
                    const std::string from = std::string(prefix) + std::to_string(row - 1);
                    const std::string to = std::string(prefix) + std::to_string(row);
                    auto value = ctl->fields.find(from);
                    if (value == ctl->fields.end()) ctl->fields.erase(to);
                    else ctl->fields[to] = value->second;
                }
            }
            for (const char* prefix : {"rowColor", "rowStyle", "rowActive"})
                ctl->fields.erase(std::string(prefix) + std::to_string(insertAt));
            ctl->listRows.insert(ctl->listRows.begin() + insertAt, txt);
            ctl->listRowIds.insert(ctl->listRowIds.begin() +
                                   std::min(insertAt, ctl->listRowIds.size()), id);
            auto* font = Engine::instance().renderer().getFont();
            float lineH = font ? font->charHeight + 2 : 14;
            ctl->extentY = std::max(ctl->extentY, (float)ctl->listRows.size() * lineH);
            Console::instance().printf(LogLevel::Debug, "addRow2: ctl='%s' id=%d txt='%s' rows=%zu", cname.c_str(), id, txt.c_str(), ctl->listRows.size());
        } else {
            Console::instance().printf(LogLevel::Debug, "addRow2: FAIL ctl=%p cname='%s' args=%zu", (void*)ctl, cname.c_str(), args.size());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("clearList", [getListCtrl](const auto& args) -> VMValue {
        std::string cname = args.empty() ? "" : args[0].toString();
        auto* ctl = getListCtrl(cname);
        if (ctl) {
            ctl->listRows.clear();
            ctl->listRowIds.clear();
            ctl->selectedRow = -1;
            Console::instance().printf(LogLevel::Debug, "clearList: ctl='%s' ok", cname.c_str());
        }
        else Console::instance().printf(LogLevel::Debug, "clearList: FAIL ctl=NULL cname='%s'", cname.c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("setRowById", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int id = args[1].toInt();
        const std::string text = args[2].toString();
        for (size_t i = 0; i < ctl->listRowIds.size(); ++i) {
            if (ctl->listRowIds[i] == id && i < ctl->listRows.size()) {
                ctl->listRows[i] = text;
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    tsInstance->registerNative("sort", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(1);
        // Sort menuItems (popup menus) if present, otherwise listRows
        if (!ctl->menuItems.empty()) {
            std::sort(ctl->menuItems.begin(), ctl->menuItems.end(),
                [](const GuiControl::MenuItem& a, const GuiControl::MenuItem& b) {
                    return strcasecmp(a.text.c_str(), b.text.c_str()) < 0;
                });
        } else if (!ctl->listRows.empty()) {
            int col = args.size() > 1 ? args[1].toInt() : 0;
            auto getField = [](const std::string& s, int f) -> std::string {
                size_t start = 0;
                for (int i = 0; i < f; i++) {
                    size_t tab = s.find('\t', start);
                    if (tab == std::string::npos) return "";
                    start = tab + 1;
                }
                size_t end = s.find('\t', start);
                return s.substr(start, end - start);
            };
            // Sort indices so listRowIds travel with listRows for text lists.
            std::vector<size_t> idx(ctl->listRows.size());
            for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
            std::stable_sort(idx.begin(), idx.end(),
                [&](size_t a, size_t b) {
                    return strcasecmp(getField(ctl->listRows[a], col).c_str(),
                                      getField(ctl->listRows[b], col).c_str()) < 0;
                });
            std::vector<std::string> sortedRows(ctl->listRows.size());
            std::vector<int> sortedIds(ctl->listRowIds.size());
            std::map<std::string, std::string> sortedStyles;
            for (size_t i = 0; i < idx.size(); i++) {
                sortedRows[i] = ctl->listRows[idx[i]];
                if (i < sortedIds.size()) sortedIds[i] = ctl->listRowIds[idx[i]];
                for (const char* prefix : {"rowColor", "rowStyle"}) {
                    const auto oldField = ctl->fields.find(
                        std::string(prefix) + std::to_string(idx[i]));
                    if (oldField != ctl->fields.end())
                        sortedStyles[std::string(prefix) + std::to_string(i)] = oldField->second;
                }
            }
            ctl->listRows.swap(sortedRows);
            ctl->listRowIds.swap(sortedIds);
            for (auto it = ctl->fields.begin(); it != ctl->fields.end();) {
                if (it->first.rfind("rowColor", 0) == 0 || it->first.rfind("rowStyle", 0) == 0)
                    it = ctl->fields.erase(it);
                else
                    ++it;
            }
            ctl->fields.insert(sortedStyles.begin(), sortedStyles.end());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("refreshSelectedServer", [](const auto&) -> VMValue {
        auto* browser = Engine::instance().guiRenderer().findControl("GMJ_Browser");
        if (browser && browser->sbSelected >= 0 && browser->sbSelected < (int)browser->sbServers.size())
            Engine::instance().network().querySingleServer(browser->sbServers[browser->sbSelected].addr.toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("querySingleServer", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        Engine::instance().network().querySingleServer(args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("insertIPAddress", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().setVariable("HUD::serverAddress", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("findNextServer", [](const auto&) -> VMValue {
        auto* browser = Engine::instance().guiRenderer().findControl("GMJ_Browser");
        if (!browser || browser->sbServers.empty()) return VMValue(0);
        browser->sbSelected = (browser->sbSelected + 1) % (int)browser->sbServers.size();
        return VMValue(1);
    });
    tsInstance->registerNative("getServerInfoString", [](const auto&) -> VMValue {
        auto& renderer = Engine::instance().guiRenderer();
        auto* browser = renderer.findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue("");
        auto& srv = browser->sbServers[browser->sbSelected];
        std::string flags;
        if (srv.password) flags += "Password ";
        if (srv.tournament) flags += "Tournament ";
        char serverInfo[160];
        snprintf(serverInfo, sizeof(serverInfo), "Players: %d/%d  Bots: %d  Ping: %d ms",
                 srv.numPlayers, srv.maxPlayers, srv.numBots, srv.ping);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s\t%s\t\t%s\t%s\t%s\t%s\t",
                 srv.name.c_str(), srv.addr.toString().c_str(), flags.c_str(),
                 srv.gameType.c_str(), srv.map.c_str(), serverInfo);
        return VMValue(buf);
    });
    tsInstance->registerNative("getServerStatus", [](const auto&) -> VMValue {
        auto& renderer = Engine::instance().guiRenderer();
        auto* browser = renderer.findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue("invalid");
        return VMValue("responded");
    });
    tsInstance->registerNative("getServerContentString", [](const auto&) -> VMValue {
        auto* browser = Engine::instance().guiRenderer().findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue("");
        const auto& server = browser->sbServers[browser->sbSelected];
        char text[256];
        snprintf(text, sizeof(text), "Players %d/%d\nBots %d\nPing %d ms",
                 server.numPlayers, server.maxPlayers, server.numBots, server.ping);
        return VMValue(text);
    });
    tsInstance->registerNative("setTitle", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->text = args[1].toString();
                Console::instance().printf(LogLevel::Debug, "setTitle: ctl='%s' title='%s'", args[0].toString().c_str(), ctl->text.c_str());
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setAltColor", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                // GM_TabFrame.setAltColor(bool): toggles the gold highlight frame
                ctl->fields["altColor"] = args[1].toBool() ? "1" : "0";
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setHeader", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                ctl->text = args[1].toString();
                Console::instance().printf(LogLevel::Debug, "setHeader: ctl='%s' header='%s'", args[0].toString().c_str(), args[1].toString().c_str());
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addServerQueryRow", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            std::string addr = args[0].toString();
            std::string info = args[1].toString();
            Console::instance().printf(LogLevel::Debug, "addServerQueryRow: addr='%s' info='%s'", addr.c_str(), info.c_str());
        }
        return VMValue(1);
    });

    // Container/spatial queries backed by the native world collision/object data.
    struct ContainerHit { std::string id; float distance; };
    static std::vector<ContainerHit> s_containerHits;
    static size_t s_containerIndex = 0;
    static float s_containerCurrentDistance = 0.0f;
    auto parsePoint = [](const std::string& value) {
        Point3F point{};
        std::istringstream stream(value);
        stream >> point.x >> point.y >> point.z;
        return point;
    };
    tsInstance->registerNative("InitContainerRadiusSearch", [parsePoint](const auto& args) -> VMValue {
        s_containerHits.clear();
        s_containerIndex = 0;
        s_containerCurrentDistance = 0.0f;
        if (args.size() < 2) return VMValue(0);
        const Point3F center = parsePoint(args[0].toString());
        const float radius = std::max(0.0f, args[1].toFloat());
        const auto& player = Engine::instance().game().player();
        const Point3F playerPos = player.position();
        const float playerDistance = std::sqrt(
            (playerPos.x - center.x) * (playerPos.x - center.x) +
            (playerPos.y - center.y) * (playerPos.y - center.y) +
            (playerPos.z - center.z) * (playerPos.z - center.z));
        if (playerDistance <= radius)
            s_containerHits.push_back({"Player", playerDistance});
        for (const auto& object : Engine::instance().game().world().objects()) {
            const float dx = object.pos.x - center.x;
            const float dy = object.pos.y - center.y;
            const float dz = object.pos.z - center.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance <= radius)
                s_containerHits.push_back({object.label.empty() ? object.shapeName : object.label, distance});
        }
        std::stable_sort(s_containerHits.begin(), s_containerHits.end(),
            [](const ContainerHit& left, const ContainerHit& right) {
                return left.distance < right.distance;
            });
        return VMValue(1);
    });
    tsInstance->registerNative("containerSearchNext", [](const auto&) -> VMValue {
        if (s_containerIndex >= s_containerHits.size()) return VMValue(0);
        s_containerCurrentDistance = s_containerHits[s_containerIndex].distance;
        return VMValue(s_containerHits[s_containerIndex++].id);
    });
    tsInstance->registerNative("containerRayCast", [parsePoint](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const Point3F origin = parsePoint(args[0].toString());
        const Point3F end = parsePoint(args[1].toString());
        Point3F direction{end.x - origin.x, end.y - origin.y, end.z - origin.z};
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (length <= 0.0001f) return VMValue("");
        direction.x /= length; direction.y /= length; direction.z /= length;
        float distance = length;
        Point3F hit{}, normal{};
        std::string hitId;
        if (Engine::instance().game().world().collision().raycast(origin, direction, length,
                                                                  distance, hit, normal))
            hitId = "Terrain";
        auto raySphere = [&](const Point3F& center, float radius, const char* id) {
            const Point3F offset{origin.x - center.x, origin.y - center.y, origin.z - center.z};
            const float projection = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
            const float discriminant = projection * projection -
                (offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - radius * radius);
            if (discriminant < 0.0f) return;
            const float firstHit = -projection - std::sqrt(discriminant);
            if (firstHit < 0.0f || firstHit > distance) return;
            distance = firstHit;
            hit = {origin.x + direction.x * distance,
                   origin.y + direction.y * distance,
                   origin.z + direction.z * distance};
            normal = {hit.x - center.x, hit.y - center.y, hit.z - center.z};
            const float normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (normalLength > 0.0001f) {
                normal.x /= normalLength; normal.y /= normalLength; normal.z /= normalLength;
            }
            hitId = id;
        };
        raySphere(Engine::instance().game().player().position(), 0.5f, "Player");
        for (const auto& object : Engine::instance().game().world().objects()) {
            const float radius = std::max(0.5f, object.boundsRadius);
            const std::string id = object.label.empty() ? object.shapeName : object.label;
            raySphere(object.pos, radius, id.empty() ? "WorldObject" : id.c_str());
        }
        if (hitId.empty())
            return VMValue("");
        char result[160];
        snprintf(result, sizeof(result), "%s\t%.3f %.3f %.3f\t%.3f %.3f %.3f",
                 hitId.c_str(), hit.x, hit.y, hit.z, normal.x, normal.y, normal.z);
        return VMValue(result);
    });
    tsInstance->registerNative("containerSearchCurrDist", [](const auto&) -> VMValue {
        return VMValue((double)s_containerCurrentDistance);
    });
    tsInstance->registerNative("containerSearchCurrRadDamageDist", [](const auto&) -> VMValue {
        return VMValue((double)s_containerCurrentDistance);
    });
    tsInstance->registerNative("calcExplosionCoverage", [parsePoint](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(1.0);
        const Point3F origin = parsePoint(args[0].toString());
        const std::string targetId = args[1].toString();
        Point3F target = Engine::instance().game().player().position();
        if (targetId != "Player") {
            bool found = false;
            for (const auto& object : Engine::instance().game().world().objects()) {
                const std::string id = object.label.empty() ? object.shapeName : object.label;
                if (id == targetId) {
                    target = object.pos;
                    found = true;
                    break;
                }
            }
            if (!found) return VMValue(1.0);
        }
        Point3F direction{target.x - origin.x, target.y - origin.y, target.z - origin.z};
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (length <= 0.0001f) return VMValue(1.0);
        direction.x /= length; direction.y /= length; direction.z /= length;
        float hitDistance = 0.0f;
        Point3F hit{}, normal{};
        return VMValue(Engine::instance().game().world().collision().raycast(
            origin, direction, std::max(0.0f, length - 0.05f), hitDistance, hit, normal) ? 0.0 : 1.0);
    });

    // Misc startup stubs
    tsInstance->registerNative("setModPaths", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().setVariable("modPath", args[0].toString().c_str());
            Console::instance().printf(LogLevel::Info, "setModPaths: %s", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    // ResManager::getModPaths: the path list setModPaths stored.
    tsInstance->registerNative("getModPaths", [](const auto&) -> VMValue {
        return VMValue(std::string(Console::instance().getStringVariable("modPath")));
    });
    tsInstance->registerNative("setEchoFileLoads", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("echoFileLoads", args[0].toInt() ? "1" : "0");
        return VMValue(1);
    });
    tsInstance->registerNative("isFunction", [this](const auto& args) -> VMValue {
        return VMValue(!args.empty() && tsInstance->isFunction(args[0].toString()) ? 1 : 0);
    });
    tsInstance->registerNative("setPureServer", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("pref::pureServer", args[0].toInt() ? "1" : "0");
        return VMValue(1);
    });
    tsInstance->registerNative("telnetSetParameters", [](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            Console::instance().setVariable("Telnet::listenPort", args[0].toString().c_str());
            Console::instance().setVariable("Telnet::listenPassword", args[1].toString().c_str());
            Console::instance().setVariable("Telnet::region", args[2].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addCardProfile", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().printf(LogLevel::Debug, "addCardProfile: %s", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addCreditsLine", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().printf(LogLevel::Debug, "addCreditsLine: %s", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("enableImmersion", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("pref::immersion", args[0].toInt() ? "1" : "0");
        return VMValue(1);
    });
    tsInstance->registerNative("isT2UkBuild", [](const auto&) -> VMValue { return VMValue(0); });
    tsInstance->registerNative("isKoreanBuild", [](const auto&) -> VMValue { return VMValue(0); });
    // Renderer supports windowed mode, so no driver is fullscreen-only.
    tsInstance->registerNative("isDeviceFullScreenOnly", [](const auto&) -> VMValue { return VMValue(0); });
    tsInstance->registerNative("isJoystickDetected", [](const auto&) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        return VMValue(Engine::instance().platform().hasJoystick() ? 1 : 0);
#endif
    });
    tsInstance->registerNative("enableJoystick", [this](const auto&) -> VMValue {
#ifdef TORCH_DEDICATED
        return VMValue(0);
#else
        if (!Engine::instance().platform().hasJoystick()) return VMValue(0);
        tsInstance->setGlobal("$pref::Input::JoystickEnabled", VMValue(1));
        return VMValue(1);
#endif
    });
    tsInstance->registerNative("disableJoystick", [this](const auto&) -> VMValue {
        tsInstance->setGlobal("$pref::Input::JoystickEnabled", VMValue(0));
        return VMValue(1);
    });
    // Driver-info dialog: VENDOR\RENDERER\VERSION\EXTENSIONS captured at renderer init.
    tsInstance->registerNative("getVideoDriverInfo", [](const auto&) -> VMValue {
        const std::string& info = Engine::instance().renderer().gpuDriverInfo();
        if (!info.empty()) return VMValue(info);
        return VMValue("Unknown\tUnknown\tUnknown\t");
    });
    tsInstance->registerNative("getControlObjectSpeed", [](const auto&) -> VMValue {
        const auto velocity = Engine::instance().game().player().velocity();
        return VMValue(std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
                                 velocity.z * velocity.z));
    });
    tsInstance->registerNative("getControlObjectAltitude", [](const auto&) -> VMValue {
        const auto position = Engine::instance().game().player().position();
        const float ground = Engine::instance().game().world().getHeight(position.x, position.z);
        return VMValue(position.y - ground);
    });
    tsInstance->registerNative("getDamageLevel", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            ScriptObjectState state;
            char* end = nullptr;
            const std::string value = args[0].toString();
            const long id = std::strtol(value.c_str(), &end, 10);
            if (end && *end == '\0' && id > 0 && ScriptEngine::instance().objectState((int)id, state))
                return VMValue(state.hasMaxHealth
                    ? damageLevelForHealth(state.health, state.maxHealth) : 0.0f);
            if (auto* object = ScriptEngine::instance().findObject(value.c_str()))
                return object->fields["damageLevel"];
            return VMValue(0.0f);
        }
        const auto& player = Engine::instance().game().player();
        return VMValue(damageLevelForHealth(player.health(), player.maxHealth()));
    });
    tsInstance->registerNative("lockMouse", [](const auto& args) -> VMValue {
        const bool locked = !args.empty() && args[0].toBool();
        Engine::instance().platform().setRelativeMouse(locked);
        Engine::instance().platform().showMouse(!locked);
        return VMValue(1);
    });
    // Save/validate dialogs ask whether a file name could be written. The
    // scripts pass either "prefs/xxx" or "base/prefs/xxx", so strip any data-
    // root prefix ("base/", "data/") before joining against write roots.
    // Check (in order): existing files must be writable; new files need a
    // writable parent directory. Candidate roots are outputDir/base,
    // outputDir, dataDir, and the raw path.
    tsInstance->registerNative("isWriteableFileName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string path = args[0].toString();
        if (path.empty() || path.back() == '/' || path.back() == '\\') return VMValue(0);
        std::string stripped = path;
        if (stripped.rfind("base/", 0) == 0 || stripped.rfind("data/", 0) == 0)
            stripped = stripped.substr(stripped.find('/') + 1);
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        std::string dataDir = Console::instance().getStringVariable("dataDir", "");
        std::vector<std::string> candidates;
        if (!outDir.empty()) candidates.push_back(outDir + "/base/" + stripped);
        if (!outDir.empty()) candidates.push_back(outDir + "/" + stripped);
        if (!dataDir.empty()) candidates.push_back(dataDir + "/" + stripped);
        if (!outDir.empty()) candidates.push_back(outDir + "/base/" + path);
        if (!dataDir.empty()) candidates.push_back(dataDir + "/" + path);
        candidates.push_back(path);
        for (auto& c : candidates) {
            struct stat st;
            if (stat(c.c_str(), &st) == 0)
                return access(c.c_str(), W_OK) == 0 ? VMValue(1) : VMValue(0);
            auto slash = c.rfind('/');
            std::string dir = slash == std::string::npos ? "." : c.substr(0, slash);
            if (dir.empty()) dir = ".";
            if (stat(dir.c_str(), &st) == 0 && access(dir.c_str(), W_OK) == 0) return VMValue(1);
        }
        return VMValue(0);
    });
    // The stock save routine calls this with the current active config. An
    // empty active name otherwise becomes prefs/.cs and is recreated on every
    // save. Existing legacy bind-only files are safe to replace with a proper
    // ActionMap file.
    tsInstance->registerNative("isValidMapFileSaveName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string path = args[0].toString();
        const auto slash = path.find_last_of("/\\");
        const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (name.empty() || name == ".cs") return VMValue(0);
        return VMValue(1);
    });
    tsInstance->registerNative("videoSetGammaCorrection", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("pref::gammaCorrection", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("enableWinConsole", [](const auto& args) -> VMValue {
        if (!args.empty() && args[0].toBool()) {
            static bool enabled = false;
            if (!enabled) { enabled = true; }
        }
        return VMValue(1);
    });

    // ===== ActionMap bind/bindCmd/unbind/copyBind =====
    // T2 ActionMap method signatures:
    //   bind(device, key, [flags, modifier,] command)
    //   bindCmd(device, key, cmdOn, cmdOff)
    //   unbind(device, key)
    //   copyBind(sourceMap, command)
    // When called as obj.method(), the VM passes the object name as args[0].

    // Map: (objectName, device, keyName) → command string
    // Storage lives in the shared actionBindingStore() (see script_engine.h).
    auto& s_actionBinds = actionBindingStore();

    auto parseDevice = [](const std::string& s) -> int {
        if (s == "keyboard" || s == "0") return 0;
        if (s == "mouse" || s == "1") return 1;
        if (s == "joystick" || s == "2") return 2;
        // Try as number
        char* end;
        long n = strtol(s.c_str(), &end, 10);
        if (*end == 0) return (int)n;
        return -1;
    };

    tsInstance->registerNative("bind", [&s_actionBinds, parseDevice](const auto& args) -> VMValue {
        // Method call: args[0] = objectName, rest = T2 bind args
        // Standalone: args = T2 bind args (no objectName)
        size_t start = 0;
        std::string objName;
        // Detect method call: if first arg is an existing ScriptObject, it's the object name
        if (!args.empty()) {
            auto* obj = ScriptEngine::instance().findObject(args[0].toString().c_str());
            const std::string candidate = args[0].toString();
            if (obj || candidate == "moveMap" || candidate == "GlobalActionMap" ||
                candidate == "observerMap") {
                objName = args[0].toString();
                start = 1;
            }
        }
        // T2 bind(device, key, [flags, modifier,] command)
        if (args.size() - start < 3) {
            Console::instance().printf(LogLevel::Debug, "TS: bind() needs at least 3 args (device, key, command)");
            return VMValue(0);
        }
        int device = parseDevice(args[start].toString());
        if (device < 0) { Console::instance().printf(LogLevel::Debug, "TS: bind unknown device '%s'", args[start].toString().c_str()); return VMValue(0); }
        std::string keyName = args[start + 1].toString();
        // The command is always the last non-flag arg. Simple approach: last arg is always the command.
        std::string command = args.back().toString();
        // A remap replaces the previous key for this command on the same map.
        // Keeping both entries makes getBinding() return a stale key and causes
        // saved maps to lose the newly selected binding.
        for (auto it = s_actionBinds.begin(); it != s_actionBinds.end();) {
            const auto& [existingKey, existingBind] = *it;
            const auto& [existingMap, existingDevice, existingName] = existingKey;
            if (existingMap == objName && existingDevice == device &&
                (existingBind.cmdOn == command || existingName == keyName))
                it = s_actionBinds.erase(it);
            else
                ++it;
        }
        // Store binding
        ActionBinding bound;
        if (!makeActionBind(args, start, bound)) return VMValue(0);
        auto key = std::make_tuple(objName, device, keyName);
        s_actionBinds[key] = bound;
        if (objName == "moveMap" || objName == "GlobalActionMap") {
            const char* action = nullptr;
            if (command == "moveforward") action = "forward";
            else if (command == "movebackward") action = "backward";
            else if (command == "moveleft") action = "left";
            else if (command == "moveright") action = "right";
            else if (command == "jump") action = "jump";
            else if (command == "jet") action = "jet";
            else if (command == "reload") action = "reload";
            else if (command == "toggleConsole") action = "console";
            if (action) {
                int sc = -1;
                bool valid = false;
                if (device == 0) {
                    sc = Engine::instance().nameToScancode(keyName.c_str());
                    valid = sc >= 0;
                } else if (device == 1 && keyName.rfind("button", 0) == 0) {
                    const int button = atoi(keyName.c_str() + 6);
                    sc = button == 0 ? -1 : button == 1 ? -3 : -button;
                    valid = button >= 0 && button < 8;
                }
                if (valid) {
                    Engine::instance().setBind(action, sc);
                }
            }
        }
        Console::instance().printf(LogLevel::Debug, "TS: bind(%s, %d, '%s') = '%s'",
            objName.empty() ? "?" : objName.c_str(), device, keyName.c_str(), command.c_str());
        return VMValue(1);
    });

    // ActionMap::isInverted / getDeadZone / getScale (device, action).
    auto findBind = [&s_actionBinds, parseDevice](const auto& args) -> const ActionBinding* {
        if (args.size() < 3) return nullptr;
        const std::string map = args[0].toString();
        const int device = parseDevice(args[1].toString());
        const std::string action = args[2].toString();
        ScriptObject* mapObject = ScriptEngine::instance().findObject(map.c_str());
        for (const auto& [key, bind] : s_actionBinds) {
            const auto& [bindMap, bindDevice, bindName] = key;
            if (bindDevice != device || strcasecmp(bindName.c_str(), action.c_str()) != 0) continue;
            ScriptObject* bindObject = ScriptEngine::instance().findObject(bindMap.c_str());
            if (bindObject == mapObject || strcasecmp(bindMap.c_str(), map.c_str()) == 0) return &bind;
        }
        Console::instance().printf(LogLevel::Error, "The input event specified by %s %s is not in this action map!",
                                   args[1].toString().c_str(), action.c_str());
        return nullptr;
    };
    tsInstance->registerNative("ActionMap::isInverted", [findBind](const auto& args) -> VMValue {
        const ActionBinding* bind = findBind(args);
        return VMValue(bind && (bind->flags & ActionBinding::Inverted) ? 1 : 0);
    });
    tsInstance->registerNative("ActionMap::getScale", [findBind](const auto& args) -> VMValue {
        const ActionBinding* bind = findBind(args);
        return VMValue(bind && (bind->flags & ActionBinding::HasScale) ? bind->scaleFactor : 1.0f);
    });
    tsInstance->registerNative("ActionMap::getDeadZone", [findBind](const auto& args) -> VMValue {
        const ActionBinding* bind = findBind(args);
        if (!bind) return VMValue("");
        if (!(bind->flags & ActionBinding::HasDeadZone)) return VMValue("0 0");
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%f %f", bind->deadZoneBegin, bind->deadZoneEnd);
        return VMValue(buffer);
    });
    tsInstance->registerNative("bindcmd", [&s_actionBinds, parseDevice](const auto& args) -> VMValue {
        size_t start = 0;
        std::string objName;
        if (!args.empty()) {
            auto* obj = ScriptEngine::instance().findObject(args[0].toString().c_str());
            const std::string candidate = args[0].toString();
            if (obj || candidate == "moveMap" || candidate == "GlobalActionMap" ||
                candidate == "observerMap") { objName = candidate; start = 1; }
        }
        if (args.size() - start < 4) return VMValue(0);
        int device = parseDevice(args[start].toString());
        if (device < 0) return VMValue(0);
        std::string keyName = args[start + 1].toString();
        std::string cmdOn = args[start + 2].toString();
        std::string cmdOff = args[start + 3].toString();
        auto key = std::make_tuple(objName, device, keyName);
        s_actionBinds[key] = {cmdOn, cmdOff, true};
        Console::instance().printf(LogLevel::Debug, "TS: bindcmd(%s, %d, '%s') on='%s' off='%s'",
            objName.empty() ? "?" : objName.c_str(), device, keyName.c_str(), cmdOn.c_str(), cmdOff.c_str());
        return VMValue(1);
    });

    tsInstance->registerNative("unbind", [&s_actionBinds, parseDevice](const auto& args) -> VMValue {
        size_t start = 0;
        std::string objName;
        if (!args.empty()) {
            auto* obj = ScriptEngine::instance().findObject(args[0].toString().c_str());
            const std::string candidate = args[0].toString();
            if (obj || candidate == "moveMap" || candidate == "GlobalActionMap" ||
                candidate == "observerMap") { objName = candidate; start = 1; }
        }
        if (args.size() - start < 2) return VMValue(0);
        int device = parseDevice(args[start].toString());
        std::string keyName = args[start + 1].toString();
        auto key = std::make_tuple(objName, device, keyName);
        s_actionBinds.erase(key);
        return VMValue(1);
    });

    tsInstance->registerNative("copybind", [&s_actionBinds](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(0);
        const std::string dest = args[0].toString();
        const std::string source = args[1].toString();
        const std::string command = args[2].toString();
        for (const auto& [key, binding] : s_actionBinds) {
            const auto& [map, device, keyName] = key;
            if (map == source && binding.cmdOn == command) {
                s_actionBinds[{dest, device, keyName}] = binding;
                return VMValue(1);
            }
        }
        return VMValue(0);
    });
    auto setActionMapPushed = [](const auto& args, bool pushed) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (!object || object->className != "ActionMap") return VMValue(0);
        object->internals["__pushed"] = VMValue(pushed ? 1 : 0);
        return VMValue(1);
    };
    tsInstance->registerNative("push", [setActionMapPushed](const auto& args) {
        return setActionMapPushed(args, true);
    });
    tsInstance->registerNative("pop", [setActionMapPushed](const auto& args) {
        return setActionMapPushed(args, false);
    });

    // ActionMap::save(fileName, isAppend) — serialize this map's binds into a
    // reloadable .cs (exec-able). Header line must be exactly
    // "// Tribes 2 Input Map File" for isMapFile() validation.
    auto deviceName = [](int d) -> const char* {
        switch (d) { case 0: return "keyboard"; case 1: return "mouse"; case 2: return "joystick"; default: return "keyboard"; }
    };
    auto actionMapWrite = [&s_actionBinds, deviceName](const std::string& objName, const std::string& path, bool append) -> int {
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (outDir.empty()) return 0;
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string fullPath = outDir + "/" + modPath + "/" + path;
        auto slash = fullPath.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = fullPath.substr(0, slash);
            struct stat st; if (stat(dir.c_str(), &st) != 0) mkdir(dir.c_str(), 0755);
        }
        FILE* f = fopen(fullPath.c_str(), append ? "a" : "w");
        if (!f) return 0;
        if (!append) fprintf(f, "// Tribes 2 Input Map File\n// ActionMap: %s\n", objName.c_str());
        // Native controls use the same movement state as the ActionMap. If a
        // script-side map was only partially reconstructed, materialize the
        // native movement bindings before serializing so the T2 prefs file is
        // still complete and reloadable.
        if (objName == "moveMap") {
            const std::pair<const char*, const char*> actions[] = {
                {"forward", "moveforward"}, {"backward", "movebackward"},
                {"left", "moveleft"}, {"right", "moveright"},
                {"jump", "jump"}, {"jet", "jet"},
            };
            for (const auto& [action, command] : actions) {
                bool present = false;
                for (const auto& [key, binding] : s_actionBinds) {
                    if (std::get<0>(key) == objName && binding.cmdOn == command) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    const int sc = Engine::instance().getBind(action);
                    const char* keyName = GuiRenderer::scancodeToKeyName(sc);
                    if (keyName && *keyName) {
                        const auto key = std::make_tuple(objName, 0, std::string(keyName));
                        if (s_actionBinds.find(key) == s_actionBinds.end())
                            s_actionBinds[key] = {command, "", false};
                    }
                }
            }
        }
        for (auto& [k, be] : s_actionBinds) {
            const auto& [obj, dev, key] = k;
            if (obj != objName) continue;
            if (be.isCmd) {
                // Commands are stored as script text; quote them (escaping any
                // embedded quotes) so the line is valid, reloadable TS.
                auto q = [](const std::string& s) -> std::string {
                    std::string r = "\"";
                    for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
                    return r + "\"";
                };
                fprintf(f, "%s.bindCmd(%s, \"%s\", %s, %s);\n", obj.c_str(),
                    deviceName(dev), key.c_str(), q(be.cmdOn).c_str(), q(be.cmdOff).c_str());
            } else {
                fprintf(f, "%s.bind(%s, \"%s\", %s);\n", obj.c_str(),
                    deviceName(dev), key.c_str(), be.cmdOn.c_str());
            }
        }
        fprintf(f, "// End\n");
        fclose(f);
        return 1;
    };
    tsInstance->registerNative("actionmap::save", [actionMapWrite](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string objName = args[0].toString();
        std::string path = args.size() > 1 ? args[1].toString() : "";
        if (path.empty()) return VMValue(0);
        bool append = false;
        if (args.size() > 2) {
            std::string a = args[2].toString();
            for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
            append = (a == "true" || a == "1");
        }
        const int result = actionMapWrite(objName, path, append);
        return VMValue(result);
    });
    // The stock saveMapFile() wrapper can be shadowed by partially loaded
    // shell scripts. Keep the persisted format native, but write the same T2
    // map file that the wrapper is expected to produce.
    tsInstance->registerNative("saveMapFile", [actionMapWrite](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        if (name.empty() || name == "." || name.find_first_of("\\/?*\"'<>|") != std::string::npos)
            return VMValue(0);
        const std::string path = "prefs/" + name + ".cs";
        if (!actionMapWrite("moveMap", path, false)) return VMValue(0);
        if (!actionMapWrite("observerMap", path, true)) return VMValue(0);
        if (!actionMapWrite("GlobalActionMap", path, true)) return VMValue(0);
        Console::instance().setVariable("$pref::Input::ActiveConfig", name.c_str());
        Console::instance().printf(LogLevel::Info, "Saved input config: %s", path.c_str());
        return VMValue(1);
    });

    // ActionMap::getBinding(action) — return "flags key" for the bound action
    // (see saveMapFile: getField(%bind, 1) yields the key name)
    auto sameRemappableCommand = [](const std::string& left, const std::string& right) {
        return left == right ||
            (left == "jet" && right == "mouseJet") ||
            (left == "mouseJet" && right == "jet");
    };
    tsInstance->registerNative("actionmap::getbinding", [&s_actionBinds, sameRemappableCommand](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        std::string objName = args[0].toString();
        std::string action = args[1].toString();
        for (auto& [k, be] : s_actionBinds) {
            const auto& [obj, dev, key] = k;
            if (obj == objName && sameRemappableCommand(be.cmdOn, action))
                return VMValue(std::string("keyboard\t") + key);
        }
        return VMValue("");
    });

    tsInstance->registerNative("actionmap::getcommand", [&s_actionBinds, parseDevice, sameRemappableCommand](const auto& args) -> VMValue {
        // getCommand(device, key) — return the command bound to (thisMap, device, key),
        // or "" if the key is not bound on the calling map.
        if (args.size() < 3) return VMValue("");
        std::string objName = args[0].toString();
        int device = parseDevice(args[1].toString());
        std::string keyName = args[2].toString();
        if (device < 0) return VMValue("");
        auto key = std::make_tuple(objName, device, keyName);
        auto it = s_actionBinds.find(key);
        if (it != s_actionBinds.end())
            return VMValue(sameRemappableCommand(it->second.cmdOn, "mouseJet") ? "mouseJet" : it->second.cmdOn);
        return VMValue("");
    });

    // getResolution — returns "width height"
    tsInstance->registerNative("getResolution", [](const auto&) -> VMValue {
        auto& gui = Engine::instance().guiRenderer();
        auto* canvas = gui.findControl("GuiCanvas");
        if (canvas)
            return VMValue(std::to_string((int)canvas->extentX) + " " + std::to_string((int)canvas->extentY));
        return VMValue("1024 768");
    });

    // getWord — extract Nth space-delimited word from a string
    tsInstance->registerNative("getWord", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        const int index = args.size() > 1 ? args[1].toInt() : 0;
        return index >= 0 && index < (int)words.size() ? VMValue(words[index]) : VMValue("");
    });

    tsInstance->registerNative("getWordCount", [](const auto& args) -> VMValue {
        return VMValue((int32_t)splitScriptWords(args.empty() ? "" : args[0].toString()).size());
    });

    // firstWord — returns first word
    tsInstance->registerNative("firstWord", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const auto words = splitScriptWords(args[0].toString());
        return words.empty() ? VMValue("") : VMValue(words.front());
    });

    // ─── Warrior setup / options missing natives ──────────────────────

    // quit() — exit the engine
    tsInstance->registerNative("quit", [](const auto&) -> VMValue {
        Engine::instance().quit();
        return VMValue(1);
    });

    // Canvas.getContent() — return name of the base content panel
    tsInstance->registerNative("getContent", [](const auto&) -> VMValue {
        auto& gui = Engine::instance().guiRenderer();
        if (auto* c = gui.getDialog(0))
            return VMValue(c->name);
        return VMValue("");
    });

    // Canvas.repaint() — no-op (continuous rendering)
    // (already registered at line 2730)

    // addScheme(id, r, g, b) — store a color scheme on a popup menu control
    // Called as %this.addScheme(idx, r, g, b) — args[0] is objName
    tsInstance->registerNative("addScheme", [](const auto& args) -> VMValue {
        if (args.size() < 5) return VMValue(0);
        std::string objName = args[0].toString();
        int idx = args[1].toInt();
        int r = args[2].toInt(), g = args[3].toInt(), b = args[4].toInt();
        auto* obj = ScriptEngine::instance().findObject(objName.c_str());
        if (obj) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d %d %d", r, g, b);
            obj->fields["schemeColor" + std::to_string(idx)] = VMValue(std::string(buf));
        }
        return VMValue(1);
    });

    tsInstance->registerNative("MessageBoxOK", [](const auto& args) -> VMValue {
        std::string title = args.size() > 0 ? args[0].toString() : "Message";
        std::string msg = args.size() > 1 ? args[1].toString() : "";
        auto& gui = Engine::instance().guiRenderer();
        if (auto* frame = gui.findControl("MBOKFrame")) frame->text = title;
        if (auto* text = gui.findControl("MBOKText")) text->text = "<just:center>" + msg;
        const std::string command = (args.size() > 2 ? args[2].toString() + " " : "")
            + "Canvas.popDialog(MessageBoxOKDlg);";
        setGuiCommand("MBOKButton", command);
        if (!gui.findControl("MessageBoxOKDlg")) return VMValue(0);
        gui.pushDialog("MessageBoxOKDlg");
        return VMValue(1);
    });

    tsInstance->registerNative("MessageBoxOkCancel", [](const auto& args) -> VMValue {
        std::string title = args.size() > 0 ? args[0].toString() : "Message";
        std::string msg = args.size() > 1 ? args[1].toString() : "";
        auto& gui = Engine::instance().guiRenderer();
        if (auto* frame = gui.findControl("MBOKCancelFrame")) frame->text = title;
        if (auto* text = gui.findControl("MBOKCancelText")) text->text = "<just:center>" + msg;
        setGuiCommand("MBOKCancelButtonOK", (args.size() > 2 ? args[2].toString() + " " : "")
            + "Canvas.popDialog(MessageBoxOKCancelDlg);");
        setGuiCommand("MBOKCancelButtonCancel", (args.size() > 3 ? args[3].toString() + " " : "")
            + "Canvas.popDialog(MessageBoxOKCancelDlg);");
        if (!gui.findControl("MessageBoxOKCancelDlg")) return VMValue(0);
        gui.pushDialog("MessageBoxOKCancelDlg");
        return VMValue(1);
    });

    tsInstance->registerNative("MessageBoxYesNo", [](const auto& args) -> VMValue {
        std::string title = args.size() > 0 ? args[0].toString() : "Message";
        std::string msg = args.size() > 1 ? args[1].toString() : "";
        auto& gui = Engine::instance().guiRenderer();
        if (auto* frame = gui.findControl("MBYesNoFrame")) frame->text = title;
        if (auto* text = gui.findControl("MBYesNoText")) text->text = "<just:center>" + msg;
        setGuiCommand("MBYesNoButtonYes", (args.size() > 2 ? args[2].toString() + " " : "")
            + "Canvas.popDialog(MessageBoxYesNoDlg);");
        setGuiCommand("MBYesNoButtonNo", (args.size() > 3 ? args[3].toString() + " " : "")
            + "Canvas.popDialog(MessageBoxYesNoDlg);");
        if (!gui.findControl("MessageBoxYesNoDlg")) return VMValue(0);
        gui.pushDialog("MessageBoxYesNoDlg");
        return VMValue(1);
    });

    tsInstance->registerNative("MessagePopup", [](const auto& args) -> VMValue {
        auto& gui = Engine::instance().guiRenderer();
        if (auto* frame = gui.findControl("MessagePopFrame"))
            frame->text = args.size() > 0 ? args[0].toString() : "Message";
        if (auto* text = gui.findControl("MessagePopText"))
            text->text = args.size() > 1 ? "<just:center>" + args[1].toString() : "";
        gui.pushDialog("MessagePopupDlg");
        return VMValue(1);
    });

    tsInstance->registerNative("CloseMessagePopup", [](const auto&) -> VMValue {
        Engine::instance().guiRenderer().popDialog("MessagePopupDlg");
        return VMValue(1);
    });

    // getDesktopResolution() — return the native window resolution, not the
    // stock logical GUI canvas size.
    tsInstance->registerNative("getDesktopResolution", [](const auto&) -> VMValue {
        return VMValue(std::to_string(Engine::instance().platform().width()) + " " +
                       std::to_string(Engine::instance().platform().height()));
    });

    // getResolutionList() returns Torque's TAB-delimited width/height/bpp rows.
    tsInstance->registerNative("getResolutionList", [](const auto&) -> VMValue {
        const int width = Engine::instance().platform().width();
        const int height = Engine::instance().platform().height();
        const std::vector<std::pair<int, int>> modes = {
            {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1920, 1080}
        };
        std::string result;
        for (const auto& [modeWidth, modeHeight] : modes) {
            if (!result.empty()) result += '\t';
            result += std::to_string(modeWidth) + " " + std::to_string(modeHeight) + " 32";
        }
        if (std::find(modes.begin(), modes.end(), std::pair<int, int>{width, height}) == modes.end())
            result += '\t' + std::to_string(width) + " " + std::to_string(height) + " 32";
        return VMValue(result);
    });

    // getDisplayDeviceList() — return "OpenGL"
    tsInstance->registerNative("getDisplayDeviceList", [](const auto&) -> VMValue {
        return VMValue("OpenGL");
    });

    tsInstance->registerNative("setScreenMode", [](const auto& args) -> VMValue {
        if (args.size() < 4) return VMValue(0);
        auto& config = Engine::instance().renderer().config();
        config.width = args[0].toInt();
        config.height = args[1].toInt();
        config.fullscreen = args[3].toBool();
        return VMValue(Engine::instance().platform().setVideoMode(
            config.width, config.height, config.fullscreen, config.vsync) ? 1 : 0);
    });

    tsInstance->registerNative("setDisplayDevice", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string device = args[0].toString();
        for (char& c : device) c = (char)std::tolower((unsigned char)c);
        return VMValue(device == "opengl" ? 1 : 0);
    });

    tsInstance->registerNative("setVerticalSync", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& config = Engine::instance().renderer().config();
        config.vsync = args[0].toBool();
        return VMValue(Engine::instance().platform().setVideoMode(
            Engine::instance().platform().width(), Engine::instance().platform().height(),
            config.fullscreen, config.vsync) ? 1 : 0);
    });

    // writeLine(objName, line) — write a line to a FileObject's buffer
    tsInstance->registerNative("writeLine", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string objName = args[0].toString();
        std::string line = args[1].toString();
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue(0);
        // Append to the internal buffer (same as read data)
        auto dit = sobj->internals.find("__fo_data");
        if (dit != sobj->internals.end()) {
            dit->second.str += line + "\n";
        } else {
            sobj->internals["__fo_data"] = VMValue(line + "\n");
            sobj->internals["__fo_pos"] = VMValue(0);
        }
        return VMValue(1);
    });

    // openForWrite(objName, path) — create an empty file object for writing
    tsInstance->registerNative("openForWrite", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string objName = args[0].toString();
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue(0);
        sobj->internals["__fo_data"] = VMValue(std::string());
        sobj->internals["__fo_pos"] = VMValue(0);
        sobj->internals["__fo_path"] = VMValue(args.size() > 1 ? args[1].toString() : "");
        return VMValue(1);
    });

    // openForAppend(objName, path) — open for appending
    tsInstance->registerNative("openForAppend", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string objName = args[0].toString();
        std::string path = args.size() > 1 ? args[1].toString() : "";
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue(0);
        // Try to read existing content
        std::vector<uint8_t> data;
        if (Engine::instance().fs().readFile(path.c_str(), data)) {
            sobj->internals["__fo_data"] = VMValue(std::string(data.begin(), data.end()));
        } else {
            sobj->internals["__fo_data"] = VMValue(std::string());
        }
        sobj->internals["__fo_pos"] = VMValue(0);
        sobj->internals["__fo_path"] = VMValue(path);
        return VMValue(1);
    });

    // new ActionMap() — create a new ScriptObject for ActionMap
    tsInstance->registerNative("ActionMap", [](const auto& args) -> VMValue {
        static int mapCount = 0;
        std::string name;
        if (!args.empty()) {
            name = args[0].toString();
        } else {
            name = "ActionMap_" + std::to_string(mapCount++);
        }
        // Create the object if it doesn't exist
        auto& engine = ScriptEngine::instance();
        if (!engine.findObject(name.c_str())) {
            auto* obj = new ScriptObject;
            obj->name = name;
            obj->className = "ActionMap";
            engine.addObject(obj);
        }
        return VMValue(name);
    });

    // Ensure the always-present GlobalActionMap singleton exists from the
    // start, so its binds record under "GlobalActionMap" (ActionMap::save /
    // getBinding key off the object name).
    {
        auto& engine = ScriptEngine::instance();
        if (!engine.findObject("GlobalActionMap")) {
            auto* obj = new ScriptObject;
            obj->name = "GlobalActionMap";
            obj->className = "ActionMap";
            engine.addObject(obj);
        }
        // Stock input scripts copy defaults into these maps before they
        // populate them.  They are engine singletons in Torque, not objects
        // that depend on a particular prefs file being present.
        for (const char* name : {"moveMap", "observerMap"}) {
            if (engine.findObject(name)) continue;
            auto* obj = new ScriptObject;
            obj->name = name;
            obj->className = "ActionMap";
            engine.addObject(obj);
        }
    }


    // getRecords(objName, tag) — return concatenation of all tagged fields
    tsInstance->registerNative("getRecords", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string objName = args[0].toString();
        std::string tag = args.size() > 1 ? args[1].toString() : "";
        auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
        if (!sobj) return VMValue("");
        std::string result;
        for (auto& [k, v] : sobj->fields) {
            if (tag.empty() || k.find(tag) == 0) {
                if (!result.empty()) result += "\t";
                result += v.toString();
            }
        }
        return VMValue(result);
    });

    // getColumnName(objName, colIdx) — return live column name
    tsInstance->registerNative("getColumnName", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        const int index = args[1].toInt();
        if (!ctl || index < 0 || index >= (int)ctl->sbColumns.size()) return VMValue("");
        return VMValue(ctl->sbColumns[index].name);
    });

    // getColumnKey(objName, colIdx) — return the live column key
    tsInstance->registerNative("getColumnKey", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        const int index = args[1].toInt();
        if (!ctl || index < 0 || index >= (int)ctl->sbColumns.size()) return VMValue("");
        return VMValue(ctl->sbColumns[index].name);
    });

    // getNumColumns(objName) — return the live column count
    tsInstance->registerNative("getNumColumns", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        return VMValue(ctl ? (int32_t)ctl->sbColumns.size() : 0);
    });

    // getRowId(objName, rowIdx) — return logical row id
    tsInstance->registerNative("getRowId", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        auto& g = Engine::instance().guiRenderer();
        auto* ctl = g.findControl(name);
        if (!ctl && ScriptEngine::instance().findObject(name.c_str()))
            ctl = g.soToGui(name, nullptr);
        if (!ctl || args.size() < 2) return VMValue(0);
        int row = args[1].toInt();
        if (row >= 0 && row < (int)ctl->listRowIds.size())
            return VMValue(ctl->listRowIds[row]);
        return VMValue(row);
    });

    tsInstance->registerNative("getRowStyle", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!ctl) return VMValue(0);
        return VMValue(atoi(ctl->fields["rowStyle" + std::to_string(args[1].toInt())].c_str()));
    });
    tsInstance->registerNative("getRowStyleById", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); ++i)
            if (ctl->listRowIds[i] == id)
                return VMValue(atoi(ctl->fields["rowStyle" + std::to_string(i)].c_str()));
        return VMValue(0);
    });
    tsInstance->registerNative("removeRowById", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!ctl) return VMValue(0);
        const int id = args[1].toInt();
        for (size_t i = 0; i < ctl->listRowIds.size(); ++i) {
            if (ctl->listRowIds[i] != id) continue;
            const int oldCount = (int)ctl->listRows.size();
            ctl->listRows.erase(ctl->listRows.begin() + i);
            ctl->listRowIds.erase(ctl->listRowIds.begin() + i);
            for (int row = (int)i; row < oldCount - 1; ++row) {
                for (const char* prefix : {"rowColor", "rowStyle"}) {
                    const std::string from = std::string(prefix) + std::to_string(row + 1);
                    const std::string to = std::string(prefix) + std::to_string(row);
                    auto value = ctl->fields.find(from);
                    if (value == ctl->fields.end()) ctl->fields.erase(to);
                    else ctl->fields[to] = value->second;
                }
            }
            ctl->fields.erase("rowColor" + std::to_string(oldCount - 1));
            ctl->fields.erase("rowStyle" + std::to_string(oldCount - 1));
            ctl->selectedRow = -1;
            return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("clearSelection", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString()))
            ctl->selectedRow = -1;
        return VMValue(1);
    });

    tsInstance->registerNative("isRowActive", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!ctl) return VMValue(0);
        auto it = ctl->fields.find("rowActive" + std::to_string(args[1].toInt()));
        return VMValue(it == ctl->fields.end() || it->second == "1" || it->second == "true");
    });

    tsInstance->registerNative("setRowActive", [](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            if (auto* ctl = Engine::instance().guiRenderer().findControl(args[0].toString()))
                ctl->fields["rowActive" + std::to_string(args[1].toInt())] =
                    args[2].toBool() ? "1" : "0";
        }
        return VMValue(1);
    });

    // selectRowByAddress(objName, addr) — select a native browser result
    tsInstance->registerNative("selectRowByAddress", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* browser = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!browser) return VMValue(0);
        const std::string address = args[1].toString();
        for (size_t i = 0; i < browser->sbServers.size(); ++i) {
            if (browser->sbServers[i].addr.toString() == address) {
                browser->sbSelected = (int)i;
                return VMValue(1);
            }
        }
        Engine::instance().network().querySingleServer(address.c_str());
        return VMValue(1);
    });
    // findServer(objName, pattern) — select the next matching native result
    tsInstance->registerNative("findServer", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        auto* browser = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!browser) return VMValue(0);
        std::string pattern = args[1].toString();
        std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const int start = browser->sbSelected < 0 ? 0 : browser->sbSelected + 1;
        for (int offset = 0; offset < (int)browser->sbServers.size(); ++offset) {
            const int i = (start + offset) % (int)browser->sbServers.size();
            std::string haystack = browser->sbServers[i].name + " " +
                browser->sbServers[i].map + " " + browser->sbServers[i].addr.toString();
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (haystack.find(pattern) != std::string::npos) {
                browser->sbSelected = i;
                return VMValue(1);
            }
        }
        return VMValue(0);
    });

    // resize(objName, x, y, w, h) — set position and extent
    tsInstance->registerNative("resize", [](const auto& args) -> VMValue {
        if (args.size() < 5) return VMValue(0);
        std::string objName = args[0].toString();
        auto* ctl = Engine::instance().guiRenderer().findControl(objName);
        if (ctl) {
            GuiRenderer::resizeControl(ctl, args[1].toInt(), args[2].toInt(),
                                       args[3].toInt(), args[4].toInt());
            if (auto* ts = Engine::instance().script().ts()) {
                const std::string callback = ctl->name + "::onResize";
                if (ts->hasFunction(callback))
                    ts->callFunction(callback, {VMValue(ctl->name), VMValue(ctl->extentX), VMValue(ctl->extentY)});
            }
            return VMValue(1);
        }
        // Also set on the script object
        auto* obj = ScriptEngine::instance().findObject(objName.c_str());
        if (obj) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d %d", args[1].toInt(), args[2].toInt());
            obj->fields["position"] = VMValue(std::string(buf));
            snprintf(buf, sizeof(buf), "%d %d", args[3].toInt(), args[4].toInt());
            obj->fields["extent"] = VMValue(std::string(buf));
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setPosition", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const std::string name = args[0].toString();
        Point3F missionPosition;
        if (args.size() == 2 && parseVector(args[1], missionPosition)) {
            const auto& objects = Engine::instance().game().world().objects();
            for (const auto& object : objects)
                if (object.objectName == name)
                    return VMValue(Engine::instance().game().world().setMissionObjectPosition(
                        name, missionPosition) ? 1 : 0);
        }
        auto* ctl = Engine::instance().guiRenderer().findControl(name);
        const std::string value = args.size() >= 3
            ? args[1].toString() + " " + args[2].toString() : args[1].toString();
        float x = 0, y = 0;
        if (args.size() >= 3) { x = args[1].toFloat(); y = args[2].toFloat(); }
        else sscanf(value.c_str(), "%f %f", &x, &y);
        if (ctl) { ctl->posX = x; ctl->posY = y; }
        if (ctl) {
            if (auto* ts = Engine::instance().script().ts()) {
                const std::string callback = ctl->name + "::onResize";
                if (ts->hasFunction(callback))
                    ts->callFunction(callback, {VMValue(ctl->name), VMValue(ctl->extentX), VMValue(ctl->extentY)});
            }
        }
        if (auto* obj = ScriptEngine::instance().findObject(name.c_str()))
            obj->fields["position"] = VMValue(value);
        return VMValue(1);
    });
    tsInstance->registerNative("setRotation", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        Point3F axis;
        float angle = 0.0f;
        if (sscanf(args[1].toString().c_str(), "%f %f %f %f", &axis.x, &axis.y, &axis.z, &angle) != 4)
            return VMValue(0);
        return VMValue(Engine::instance().game().world().setMissionObjectRotation(
            args[0].toString(), axis, angle) ? 1 : 0);
    });
    tsInstance->registerNative("setScale", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        Point3F scale;
        if (!parseVector(args[1], scale)) return VMValue(0);
        return VMValue(Engine::instance().game().world().setMissionObjectScale(
            args[0].toString(), scale) ? 1 : 0);
    });
    tsInstance->registerNative("setExtent", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        const std::string name = args[0].toString();
        auto* ctl = Engine::instance().guiRenderer().findControl(name);
        const std::string value = args.size() >= 3
            ? args[1].toString() + " " + args[2].toString() : args[1].toString();
        float w = 0, h = 0;
        if (args.size() >= 3) { w = args[1].toFloat(); h = args[2].toFloat(); }
        else sscanf(value.c_str(), "%f %f", &w, &h);
        if (ctl) { ctl->extentX = w; ctl->extentY = h; }
        if (ctl) {
            if (auto* ts = Engine::instance().script().ts()) {
                const std::string callback = ctl->name + "::onResize";
                if (ts->hasFunction(callback))
                    ts->callFunction(callback, {VMValue(ctl->name), VMValue(ctl->extentX), VMValue(ctl->extentY)});
            }
        }
        if (auto* obj = ScriptEngine::instance().findObject(name.c_str()))
            obj->fields["extent"] = VMValue(value);
        return VMValue(1);
    });

    // queryFavoriteServers() — probe the stock preference favorites
    tsInstance->registerNative("queryFavoriteServers", [](const auto&) -> VMValue {
        auto* ts = ScriptEngine::instance().ts();
        if (!ts) return VMValue(0);
        const int count = std::clamp(ts->getGlobal("$pref::ServerBrowser::FavoriteCount").toInt(), 0, 512);
        int queried = 0;
        for (int i = 0; i < count; ++i) {
            const std::string entry = ts->getGlobal(
                "$pref::ServerBrowser::Favorite[" + std::to_string(i) + "]").toString();
            const auto tab = entry.find('\t');
            const std::string address = tab == std::string::npos ? entry : entry.substr(tab + 1);
            if (address.empty()) continue;
            Engine::instance().network().querySingleServer(address.c_str());
            ++queried;
        }
        return VMValue(queried > 0 ? 1 : 0);
    });

    // isServerQueryActive() — expose the native query lifecycle
    tsInstance->registerNative("isServerQueryActive", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().network().isServerQueryActive() ? 1 : 0);
    });

    // getT2VersionNumber() — return version string
    tsInstance->registerNative("getT2VersionNumber", [](const auto&) -> VMValue {
        return VMValue("0.1.0 (Torch)");
    });

    // DatabaseQueryArray() — stub
    tsInstance->registerNative("DatabaseQueryArray", [](const auto&) -> VMValue {
        return VMValue(0);
    });

    // alxIsEnabled() — return whether audio is initialized
    tsInstance->registerNative("alxIsEnabled", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isInitialized() ? 1 : 0);
    });

    tsInstance->registerNative("connectSpectator", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string host;
        uint16_t port = T2Protocol::DEFAULT_PORT;
        if (!TorchMaster::parseAddressLine(args[0].toString(), host, port)) {
            host = args[0].toString();
            if (host.empty()) return VMValue(0);
            if (args.size() > 1 && args[1].toInt() > 0)
                port = (uint16_t)args[1].toInt();
        }
        Engine::instance().game().connectToServer(host.c_str(), port);
        return VMValue(1);
    });
    tsInstance->registerNative("watchServer", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string host;
        uint16_t port = 0;
        if (!parseConsoleHostPort(args[0].toString(), host, port)) return VMValue(0);
        Engine::instance().game().connectToServer(host.c_str(), port);
        return VMValue(1);
    });
    tsInstance->registerNative("watchSelectedServer", [](const auto&) -> VMValue {
        auto* browser = Engine::instance().guiRenderer().findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue(0);
        const std::string address = browser->sbServers[browser->sbSelected].addr.toString();
        Engine::instance().game().connectToServer(
            address.substr(0, address.rfind(':')).c_str(),
            browser->sbServers[browser->sbSelected].addr.port);
        return VMValue(1);
    });

    // disconnect() — native client connection entry point
    tsInstance->registerNative("disconnect", [](const auto&) -> VMValue {
        if (auto* connection = Engine::instance().game().activeConnection())
            connection->disconnect();
        return VMValue(1);
    });
    tsInstance->registerNative("stopDemoPlayback", [](const auto&) -> VMValue {
        Engine::instance().game().stopDemoPlayback();
        return VMValue(1);
    });

    tsInstance->registerNative("disconnectedCleanup", [](const auto&) -> VMValue {
        Engine::instance().game().disconnectedCleanup();
        return VMValue(1);
    });

    // These names are also used by GUI-specific helpers registered above.
    // Install the object-aware forms last so ordinary SimObjects never enter
    // a GUI lookup path that may not have a renderer/control tree.
    tsInstance->registerNative("getName", getObjectName);
    tsInstance->registerNative("getGroup", getGroup);
    tsInstance->registerNative("getCount", getGroupCount);
    tsInstance->registerNative("getField", getFieldString);
    tsInstance->registerNative("getFieldValue", getObjectField);
    tsInstance->registerNative("getDataField", getObjectDataField);
    tsInstance->registerNative("setField", setFieldValue);
    tsInstance->registerNative("setFieldValue", setObjectField);
    tsInstance->registerNative("setDataField", setObjectDataField);

    auto missionObject = [](const std::string& value) -> const ScriptMissionObject* {
        auto& objects = ScriptEngine::instance().missionObjects();
        if (objects.empty()) return nullptr;
        // The mission file's records stand in only where the mission did not
        // run as script: an object the mission created answers for itself.
        if (ScriptEngine::instance().findObject(value.c_str())) return nullptr;
        char* end = nullptr;
        const long id = std::strtol(value.c_str(), &end, 10);
        for (const auto& object : objects) {
            if ((end && *end == '\0' && id > 0 && object.id == id) || object.name == value)
                return &object;
        }
        return nullptr;
    };
    auto missionChildren = [missionObject](const std::string& group) {
        std::vector<const ScriptMissionObject*> children;
        const auto* parent = missionObject(group);
        const std::string parentName = parent ? parent->name : group;
        for (const auto& object : ScriptEngine::instance().missionObjects())
            if (object.parentName == parentName) children.push_back(&object);
        return children;
    };
    // SimObject::getId: mission objects, script objects, then ghosts (whose
    // handle is their id).
    tsInstance->registerNative("getId", [missionObject](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (const auto* object = missionObject(args[0].toString())) return VMValue(object->id);
        auto& engine = ScriptEngine::instance();
        if (auto* object = engine.findObject(args[0].toString().c_str())) return VMValue(engine.objectId(object));
        int id = 0;
        ScriptObjectState state;
        if (providerObjectId(args[0].toString(), id) && engine.objectState(id, state)) return VMValue(id);
        return VMValue(0);
    });
    tsInstance->registerNative("getName", [missionObject](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        if (const auto* object = missionObject(args[0].toString())) return VMValue(object->name);
        int id = 0;
        ScriptObjectState state;
        if (providerObjectId(args[0].toString(), id) && ScriptEngine::instance().objectState(id, state))
            return VMValue(state.name);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return VMValue(scriptObjectName(object));
        return VMValue("");
    });
    tsInstance->registerNative("getClassName", [missionObject](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        if (const auto* object = missionObject(args[0].toString())) return VMValue(object->className);
        int id = 0;
        ScriptObjectState state;
        if (providerObjectId(args[0].toString(), id) && ScriptEngine::instance().objectState(id, state))
            return VMValue(state.className);
        if (auto* object = ScriptEngine::instance().findObject(args[0].toString().c_str()))
            return VMValue(object->className);
        return VMValue("");
    });
    tsInstance->registerNative("isObject", [missionObject](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const std::string value = args[0].toString();
        if (missionObject(value) || ScriptEngine::instance().findObject(value.c_str())) return VMValue(1);
        int id = 0;
        if (providerObjectId(value, id)) {
            ScriptObjectState state;
            if (ScriptEngine::instance().objectState(id, state)) return VMValue(1);
        }
        return VMValue(0);
    });
    tsInstance->registerNative("getGroup", [missionObject, getGroup](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        const auto* object = missionObject(args[0].toString());
        if (!object) return getGroup(args);
        if (object->parentName.empty()) return VMValue(0);
        if (const auto* parent = missionObject(object->parentName)) return VMValue(parent->id);
        return VMValue(0);
    });
    tsInstance->registerNative("getCount", [missionObject, missionChildren, getGroupCount](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        if (!missionObject(args[0].toString())) return getGroupCount(args);
        return VMValue((int32_t)missionChildren(args[0].toString()).size());
    });
    auto getMissionChild = [missionObject, missionChildren](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        if (!missionObject(args[0].toString())) {
            auto& engine = ScriptEngine::instance();
            auto* group = engine.findObject(args[0].toString().c_str());
            if (!group) return VMValue(0);
            const std::string key = group->internals["__child" + std::to_string(args[1].toInt())].toString();
            if (auto* child = engine.findObject(key.c_str())) return VMValue(engine.objectId(child));
            return VMValue(0);
        }
        const int index = args[1].toInt();
        const auto children = missionChildren(args[0].toString());
        return index >= 0 && index < (int)children.size() ? VMValue(children[index]->id) : VMValue(0);
    };
    tsInstance->registerNative("getObject", getMissionChild);
    tsInstance->registerNative("getChild", getMissionChild);
    auto sibling = [missionObject](const auto& args, int direction) -> VMValue {
        if (args.empty()) return VMValue(0);
        const auto* object = missionObject(args[0].toString());
        if (!object) return VMValue(0);
        std::vector<const ScriptMissionObject*> siblings;
        for (const auto& candidate : ScriptEngine::instance().missionObjects())
            if (candidate.parentName == object->parentName) siblings.push_back(&candidate);
        for (size_t i = 0; i < siblings.size(); ++i)
            if (siblings[i] == object) {
                const int next = (int)i + direction;
                return next >= 0 && next < (int)siblings.size() ? VMValue(siblings[next]->id) : VMValue(0);
            }
        return VMValue(0);
    };
    tsInstance->registerNative("nextObject", [sibling](const auto& args) { return sibling(args, 1); });
    tsInstance->registerNative("prevObject", [sibling](const auto& args) { return sibling(args, -1); });
    tsInstance->registerNative("previousObject", [sibling](const auto& args) { return sibling(args, -1); });
    tsInstance->registerNative("getFieldValue", [missionObject, getObjectField](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        const auto* object = missionObject(args[0].toString());
        if (!object) return getObjectField(args);
        for (const auto& [name, value] : object->fields)
            if (sameFieldName(name, args[1].toString())) return value;
        return VMValue("");
    });

    registerSimNatives(*tsInstance);
    ensureEngineGroups();

    // Copy all TS-registered natives to DSO VM so DSO functions can find them
    for (auto& entry : tsInstance->getNatives()) {
        vmInstance->registerNativeFunction(entry.first.c_str(), entry.second);
    }
    Console::instance().printf(LogLevel::Info, "ScriptEngine initialized with %zu native functions + DTS support", 12);
    return true;
}

void ScriptEngine::shutdown() {
    // Use the same lifecycle path as an explicit delete.  This keeps
    // onRemove/delete-notify callbacks and object-owned schedules consistent
    // during both mission teardown and process shutdown.
    std::vector<std::string> names;
    names.reserve(objects.size());
    for (const auto& [name, obj] : objects)
        if (obj) names.push_back(name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) deleteScriptObject(name);
    objects.clear();
    objectsById.clear();
    nextDatablockObjectId_ = 3;
    nextDynamicObjectId_ = 2051;
    if (tsInstance) tsInstance->clearScheduledEvents();
    missionObjects_.clear();
    missionObjectsWorldBacked_ = false;
    s_scriptTargets.clear();
    delete tsInstance;
    tsInstance = nullptr;
    delete vmInstance;
    vmInstance = nullptr;
}

void ScriptEngine::setMissionObjects(std::vector<ScriptMissionObject> objects, bool worldBacked) {
    missionObjects_ = std::move(objects);
    missionObjectsWorldBacked_ = worldBacked;
    // Mission objects share the dynamic SimObject id range.
    for (auto& object : missionObjects_)
        object.id = allocateObjectId(false);
}

void ScriptEngine::clearMissionObjects() {
    missionObjects_.clear();
    missionObjectsWorldBacked_ = false;
}

void ScriptEngine::cancelMissionEvents() {
    if (!tsInstance) return;
    for (const auto& object : missionObjects_)
        tsInstance->cancelEventsForObject(object.name);
}

void ScriptEngine::dispatchMissionObjectRemovalCallbacks() {
    if (!tsInstance) return;
    std::set<std::string> dispatched;
    auto dispatch = [&](const std::string& name) {
        if (!dispatched.insert(name).second) return;
        const auto it = std::find_if(missionObjects_.begin(), missionObjects_.end(),
            [&](const ScriptMissionObject& object) { return object.name == name; });
        if (it == missionObjects_.end()) return;
        const std::string callback = it->className + "::onRemove";
        if (tsInstance->hasFunction(callback)) tsInstance->callFunction(callback, {VMValue(name)});
    };
    for (const auto& object : missionObjects_)
        if (object.parentName.empty())
            for (const auto& name : missionDeletionOrder(object.name)) dispatch(name);
    for (const auto& object : missionObjects_)
        dispatch(object.name);
}

std::vector<std::string> ScriptEngine::missionDeletionOrder(const std::string& name) const {
    std::string resolved = name;
    char* end = nullptr;
    const long id = std::strtol(name.c_str(), &end, 10);
    if (end && *end == '\0' && id > 0) {
        for (const auto& object : missionObjects_)
            if (object.id == id) { resolved = object.name; break; }
    }
    std::vector<std::string> result;
    std::set<std::string> visited;
    std::function<void(const std::string&)> visit = [&](const std::string& parent) {
        if (!visited.insert(parent).second) return;
        for (const auto& object : missionObjects_)
            if (object.parentName == parent) visit(object.name);
        result.push_back(parent);
    };
    for (const auto& object : missionObjects_) {
        if (object.name == resolved) {
            visit(resolved);
            break;
        }
    }
    return result;
}

void ScriptEngine::removeMissionObjects(const std::vector<std::string>& names) {
    std::set<std::string> removed(names.begin(), names.end());
    missionObjects_.erase(std::remove_if(missionObjects_.begin(), missionObjects_.end(),
        [&removed](const ScriptMissionObject& object) { return removed.count(object.name) != 0; }),
        missionObjects_.end());
}

void ScriptEngine::registerFunction(const char* name, NativeFunc fn) {
    if (vmInstance) vmInstance->registerNativeFunction(name, std::move(fn));
}

// A numeric object handle ("1027"); Torque object names never start with
// a digit.
static bool parseObjectId(const std::string& handle, int& id) {
    if (handle.empty() || handle.size() > 10) return false;
    for (char c : handle)
        if (c < '0' || c > '9') return false;
    const long value = std::strtol(handle.c_str(), nullptr, 10);
    if (value <= 0 || value > std::numeric_limits<int>::max()) return false;
    id = (int)value;
    return true;
}

int ScriptEngine::allocateObjectId(bool datablock) {
    int& next = datablock ? nextDatablockObjectId_ : nextDynamicObjectId_;
    while (objectsById.count(next)) ++next;
    return next++;
}

int ScriptEngine::objectId(ScriptObject* object) {
    if (!object) return 0;
    if (object->id == 0) object->id = allocateObjectId(false);
    objectsById[object->id] = object;
    return object->id;
}

void ScriptEngine::assignDatablockId(ScriptObject* object) {
    if (!object || object->id != 0) return;
    object->id = allocateObjectId(true);
    objectsById[object->id] = object;
}

void ScriptEngine::ensureEngineGroups() {
    for (const char* name : {"ClientGroup", "DataBlockGroup"}) {
        if (findObject(name)) continue;
        auto* group = new ScriptObject;
        group->name = name;
        group->className = "SimGroup";
        addObject(group);
        if (tsInstance) tsInstance->setGlobal(name, VMValue(name));
    }
}

ScriptObject* ScriptEngine::createEngineObject(const std::string& className, const std::string& name) {
    auto* object = new ScriptObject;
    object->className = className;
    object->name = name;
    addObject(object);
    if (tsInstance && !name.empty()) tsInstance->setGlobal(name, VMValue(name));
    objectAdded(object);
    return object;
}

void ScriptEngine::registerDataBlock(ScriptObject* object) {
    if (!object || object->internals.count("__datablockKey")) return;
    object->internals["__datablock"] = VMValue(1);
    // GameBaseData::onAdd: a className not given is the C++ class name.
    if (EngineClasses::isA(object->className, "GameBaseData")) {
        const VMValue* value = findObjectField(object, "className");
        if (!value || value->toString().empty()) object->fields["className"] = VMValue(object->className);
    }
    if (object->id < 3 || object->id > 2050) {
        // The registry key is the id: a registered object moves with it.
        const bool registered = object->id && objects.count(std::to_string(object->id)) &&
                                objects[std::to_string(object->id)] == object;
        if (registered) removeObject(object);
        else if (object->id) objectsById.erase(object->id);
        object->id = 0;
        object->id = allocateObjectId(true);
        objectsById[object->id] = object;
        if (registered) addObject(object);
    }
    // SimDataBlock::onAdd takes sNextModifiedKey after onStaticModified has
    // bumped it for the static fields the datablock set, so a datablock's
    // key is above a connection's initial 0.
    object->internals["__datablockKey"] = VMValue(++nextDataBlockModifiedKey_);
    ensureEngineGroups();
    if (ScriptObject* group = findObject("DataBlockGroup")) addToSet(group, object);
}

ScriptObject* ScriptEngine::findDataBlock(const std::string& name) const {
    auto named = nameDictionary_.find(lowerName(name));
    if (named == nameDictionary_.end()) return nullptr;
    for (auto it = named->second.rbegin(); it != named->second.rend(); ++it)
        if ((*it)->internals.count("__datablockKey")) return *it;
    return nullptr;
}

void ScriptEngine::dataBlockModified(ScriptObject* object) {
    if (object && object->internals.count("__datablockKey"))
        object->internals["__datablockKey"] = VMValue(++nextDataBlockModifiedKey_);
}

void ScriptEngine::deleteDataBlocks() {
    ScriptObject* group = findObject("DataBlockGroup");
    if (group) {
        std::vector<std::string> members;
        const int count = group->internals["__childCount"].toInt();
        for (int i = 0; i < count; ++i) members.push_back(group->internals["__child" + std::to_string(i)].toString());
        for (auto it = members.rbegin(); it != members.rend(); ++it) deleteScriptObject(*it);
    }
    nextDatablockObjectId_ = 3;
    nextDataBlockModifiedKey_ = 0;
}

void ScriptEngine::forgetObject(ScriptObject* object) {
    if (!object || object->id == 0) return;
    auto it = objectsById.find(object->id);
    if (it != objectsById.end() && it->second == object) objectsById.erase(it);
}

std::string ScriptEngine::objectKey(ScriptObject* object) {
    if (!object) return {};
    return std::to_string(objectId(object));
}

std::string ScriptEngine::nameOrId(const std::string& handle) {
    ScriptObject* object = findObject(handle.c_str());
    if (!object) return handle;
    return object->name.empty() ? std::to_string(objectId(object)) : object->name;
}

std::string ScriptEngine::canonicalName(const std::string& handle) {
    ScriptObject* object = findObject(handle.c_str());
    return object ? objectKey(object) : handle;
}

void ScriptEngine::addObject(ScriptObject* object) {
    if (!object) return;
    const std::string key = objectKey(object);
    auto existing = objects.find(key);
    if (existing != objects.end() && existing->second == object) return;
    objects[key] = object;
    ++objectTreeRevision;
    if (!object->name.empty()) {
        auto& taken = nameDictionary_[lowerName(object->name)];
        taken.erase(std::remove(taken.begin(), taken.end(), object), taken.end());
        taken.push_back(object);
    }
}

void ScriptEngine::removeObject(ScriptObject* object) {
    if (!object) return;
    auto it = objects.find(std::to_string(object->id));
    if (it != objects.end() && it->second == object) {
        objects.erase(it);
        ++objectTreeRevision;
    }
    if (!object->name.empty()) {
        auto named = nameDictionary_.find(lowerName(object->name));
        if (named != nameDictionary_.end()) {
            auto& taken = named->second;
            taken.erase(std::remove(taken.begin(), taken.end(), object), taken.end());
            if (taken.empty()) nameDictionary_.erase(named);
        }
    }
    forgetObject(object);
}

void ScriptEngine::setObjectName(ScriptObject* object, const std::string& name) {
    if (!object) return;
    const bool registered = objects.count(std::to_string(object->id)) != 0;
    if (registered && !object->name.empty()) {
        auto named = nameDictionary_.find(lowerName(object->name));
        if (named != nameDictionary_.end()) {
            auto& taken = named->second;
            taken.erase(std::remove(taken.begin(), taken.end(), object), taken.end());
            if (taken.empty()) nameDictionary_.erase(named);
        }
    }
    object->name = name;
    ++objectTreeRevision;
    if (registered && !name.empty()) nameDictionary_[lowerName(name)].push_back(object);
}

ScriptObject* ScriptEngine::findObjectByName(const std::string& name) const {
    auto named = nameDictionary_.find(lowerName(name));
    return named == nameDictionary_.end() || named->second.empty() ? nullptr : named->second.back();
}

std::vector<std::string> ScriptEngine::objectNamespaces(ScriptObject* object) {
    // Namespace linking: a datablock links name -> className -> its C++
    // class (GameBaseData::onAdd); a ScriptObject links class -> superClass
    // (ScriptObject::onAdd); every object ends in its C++ class chain.
    std::vector<std::string> spaces;
    if (!object) return spaces;
    auto add = [&](const std::string& space) {
        if (space.empty()) return;
        for (const auto& existing : spaces)
            if (strcasecmp(existing.c_str(), space.c_str()) == 0) return;
        spaces.push_back(space);
    };
    if (!scriptObjectName(object).empty()) add(object->name);
    // An engine object linked under a name it is not registered by (an
    // AITask: every bot's task of a kind shares the kind's namespace).
    if (const auto linked = object->internals.find("__namespace"); linked != object->internals.end())
        add(linked->second.toString());
    const auto marker = object->internals.find("__datablock");
    const bool datablock = marker != object->internals.end() && marker->second.toBool();
    if (datablock) {
        if (const auto* value = findObjectField(object, "className")) add(value->toString());
    }
    for (const char* field : {"class", "superClass"})
        if (const auto* value = findObjectField(object, field)) add(value->toString());
    if (!object->className.empty())
        for (const auto& space : EngineClasses::chain(object->className)) add(space);
    return spaces;
}

ScriptObject* ScriptEngine::findObject(const char* name) {
    // An empty reference never names an object (anonymous objects must not
    // answer to "").
    if (!name || !*name) return nullptr;
    // Sim::findObject paths: "Name/child/...", "<id>/child", "/Root/..."; each
    // segment names a member of the set before it.
    if (std::strchr(name, '/')) {
        const std::string path(name);
        size_t pos = path[0] == '/' ? 1 : 0;
        size_t slash = path.find('/', pos);
        ScriptObject* current = findObject(path.substr(pos, slash - pos).c_str());
        while (current && slash != std::string::npos) {
            pos = slash + 1;
            slash = path.find('/', pos);
            const std::string segment = path.substr(pos, slash - pos);
            ScriptObject* next = nullptr;
            auto countIt = current->internals.find("__childCount");
            const int count = countIt == current->internals.end() ? 0 : countIt->second.toInt();
            for (int i = 0; i < count && !next; ++i) {
                auto child = current->internals.find("__child" + std::to_string(i));
                if (child == current->internals.end()) continue;
                ScriptObject* member = findObject(child->second.toString().c_str());
                if (member && !member->name.empty() && strcasecmp(member->name.c_str(), segment.c_str()) == 0)
                    next = member;
            }
            current = next;
        }
        return current;
    }
    // A reference starting with a digit is an id: dAtoi of the whole
    // string, so a raycast result ("<id> x y z nx ny nz") names its object.
    if (*name >= '0' && *name <= '9') {
        auto byId = objectsById.find(std::atoi(name));
        return byId != objectsById.end() ? byId->second : nullptr;
    }
    return findObjectByName(name);
}

void ScriptEngine::executeString(const char* script) {
    if (tsInstance) {
        tsInstance->execute(script);
    } else {
        Console::instance().execute(script);
    }
}

void ScriptEngine::executeFile(const char* path) {
    if (tsInstance) {
        tsInstance->executeFile(path);
    } else {
        Console::instance().executeFile(path);
    }
}

bool makeActionBind(const std::vector<VMValue>& args, size_t start, ActionBinding& bind) {
    bind = ActionBinding{};
    bind.cmdOn = args.back().toString();
    const size_t argc = args.size() - start;
    if (argc == 3) return true;
    // We have the following: "[DSIR]" [deadZone] [scale]
    const std::string spec = args[start + 2].toString();
    for (char c : spec) {
        switch (c) {
        case 'r': case 'R': case 's': case 'S': bind.flags |= ActionBinding::HasScale; break;
        case 'd': case 'D': bind.flags |= ActionBinding::HasDeadZone; break;
        case 'i': case 'I': bind.flags |= ActionBinding::Inverted; break;
        default: break;
        }
    }
    size_t cur = 3;
    if (bind.flags & ActionBinding::HasDeadZone) {
        if (cur < argc) std::sscanf(args[start + cur].toString().c_str(), "%f %f", &bind.deadZoneBegin, &bind.deadZoneEnd);
        cur++;
    }
    if (bind.flags & ActionBinding::HasScale) {
        if (cur < argc) bind.scaleFactor = (float)std::atof(args[start + cur].toString().c_str());
        cur++;
    }
    if (cur != argc - 1) {
        Console::instance().printf(LogLevel::Info, "Improperly specified bind for key: %s", spec.c_str());
        return false;
    }
    return true;
}
