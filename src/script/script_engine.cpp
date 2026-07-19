#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/string_table.h"
#include <fstream>
#include <stack>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <cmath>
#include <algorithm>
#include <array>

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
        case String: return !str.empty() && str != "0" && str != "false";
        default: return false;
    }
}

// === ScriptObject ===
ScriptObject* findScriptObject(const char* name) {
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(StringTable::instance().insert(name));
    if (it != objs.end()) return it->second;
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
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(name);
    return it != objs.end() ? it->second : nullptr;
}

void VirtualMachine::addObject(ScriptObject* obj) {
    if (obj) ScriptEngine::instance().objects[obj->name] = obj;
}

bool VirtualMachine::loadScript(const uint8_t* data, size_t size, const char* name) {
    auto* dso = new DSOFile;
    DSOReader reader;
    if (!reader.read(data, size, *dso)) {
        Console::instance().printf(LogLevel::Warn, "VM: failed to load DSO: %s", name ? name : "unknown");
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
                // Args: nameIdx, nsIdx, packageIdx, hasBody, endAddr, argc, [varArgs], [argNameIdxs...]
                if (i + 6 < opcodes.size()) {
                    uint32_t nameIdx = opcodes[i + 1];
                    uint32_t nsIdx = opcodes[i + 2];
                    uint32_t pkgIdx = opcodes[i + 3];
                    uint32_t hasBody = opcodes[i + 4];
                    uint32_t endAddr = opcodes[i + 5];
                    uint32_t argc = opcodes[i + 6];
                    uint32_t varArg = (i + 7 < opcodes.size()) ? opcodes[i + 7] : 0;

                    fn.name = reader.globalString(*dso, nameIdx);
                    fn.ns = reader.globalString(*dso, nsIdx);
                    fn.package = reader.globalString(*dso, pkgIdx);
                    fn.startIp = curIp;
                    fn.endIp = endAddr;
                    fn.argc = argc;
                    fn.hasVarArgs = varArg != 0;

                    // Read arg names
                    uint32_t argStart = i + 7 + (hasBody ? 1 : 0);
                    for (uint32_t a = 0; a < argc && argStart + a < opcodes.size(); a++) {
                        fn.argNames.push_back(reader.globalString(*dso, opcodes[argStart + a]));
                    }

                    dso->functions.push_back(fn);
                    dso->funcMap[fn.name] = &dso->functions.back();

                    Console::instance().printf(LogLevel::Debug, "VM: func %s (ip=%u, end=%u, argc=%u%s)%s%s%s",
                        fn.name.c_str(), fn.startIp, fn.endIp, fn.argc, fn.hasVarArgs ? "+" : "",
                        fn.ns.empty() ? "" : (" ns:" + fn.ns).c_str(),
                        fn.package.empty() ? "" : (" pkg:" + fn.package).c_str(),
                        hasBody ? "" : " [ext]");
                    // Skip func decl: opcode(1) + name+ns+pkg+hasBody+endAddr+argc(6)
                    i += 7;
                    if (hasBody) i++; // varArgs flag
                    i += argc; // arg names
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
    Console::instance().printf(LogLevel::Info, "VM: loaded '%s' v%u (%zu funcs)", name, dso->version, dso->functions.size());
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
            return execute(dso, fit->second->startIp, args);
        }
    }

    return {};
}

VMValue VirtualMachine::callMethod(const char* objName, const char* method, const std::vector<VMValue>& args) {
    // objName::method lookup
    std::string fullName = std::string(objName) + "::" + method;
    return callFunction(fullName.c_str(), args);
}

VMValue VirtualMachine::execute(DSOFile* dso, uint32_t startIp, const std::vector<VMValue>& args) {
    VMContext ctx;
    ctx.dso = dso;
    ctx.ip = startIp;

    // Map passed arguments to local variable names by matching to function declaration
    for (auto& fn : dso->functions) {
        if (fn.startIp == startIp) {
            // For namespaced functions, %this is the method's target object
            // (passed as args[0] from the method dispatch, before script-level args)
            bool isMethod = (!fn.ns.empty());
            uint32_t argOfs = 0;
            if (isMethod && args.size() > 0) {
                ctx.locals["%this"] = args[0];
                argOfs = 1;
            }
            for (uint32_t ai = 0; ai < fn.argc && ai + argOfs < (uint32_t)args.size(); ai++) {
                std::string localName = "%" + fn.argNames[ai];
                ctx.locals[localName] = args[ai + argOfs];
            }
            if (fn.hasVarArgs && fn.argc < (uint32_t)args.size()) {
                std::string varargStr;
                for (uint32_t ai = fn.argc; ai < (uint32_t)args.size(); ai++) {
                    if (ai > fn.argc) varargStr += "\t";
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

    size_t execIp = startIp;
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
                    uint32_t idx = opcodes[execIp + 1];
                    const char* s = "";
                    if (idx < dso->functionStrings.size()) s = &dso->functionStrings[idx];
                    else if (idx < dso->globalStrings.size()) s = &dso->globalStrings[idx];
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
                    uint32_t idx = opcodes[execIp + 1];
                    const char* name = "";
                    if (idx < dso->functionStrings.size()) name = &dso->functionStrings[idx];
                    else if (idx < dso->globalStrings.size()) name = &dso->globalStrings[idx];

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
                    // String concatenation if either is string
                    if (a.type == VMValue::String || b.type == VMValue::String)
                        stack.push(VMValue(a.toString() + b.toString()));
                    else
                        stack.push(VMValue(a.toDouble() + b.toDouble()));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SUB: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(a - b));
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
                    if (b != 0) stack.push(VMValue(a / b));
                    else stack.push(VMValue(0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_MOD: {
                if (stack.size() >= 2) {
                    int32_t b = stack.top().toInt(); stack.pop();
                    int32_t a = stack.top().toInt(); stack.pop();
                    if (b != 0) stack.push(VMValue(a % b));
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
                    stack.push(VMValue(a > b ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPGE: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(a >= b ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPLT: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(a < b ? 1 : 0));
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_CMPLE: {
                if (stack.size() >= 2) {
                    double b = stack.top().toDouble(); stack.pop();
                    double a = stack.top().toDouble(); stack.pop();
                    stack.push(VMValue(a <= b ? 1 : 0));
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
                    uint32_t nameIdx = opcodes[execIp + 1];
                    uint32_t nsIdx = opcodes[execIp + 2];
                    (void)opcodes[execIp + 3];

                    const char* name = "";
                    if (nameIdx < dso->functionStrings.size()) name = &dso->functionStrings[nameIdx];
                    else if (nameIdx < dso->globalStrings.size()) name = &dso->globalStrings[nameIdx];

                    const char* ns = "";
                    if (nsIdx < dso->functionStrings.size()) ns = &dso->functionStrings[nsIdx];
                    else if (nsIdx < dso->globalStrings.size()) ns = &dso->globalStrings[nsIdx];

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
                    auto findDSO = [&](const std::string& fn) -> bool {
                        for (auto* ds : impl->loaded) {
                            auto fit = ds->funcMap.find(fn);
                            if (fit != ds->funcMap.end()) {
                                stack.push(execute(ds, fit->second->startIp, callArgs));
                                return true;
                            }
                        }
                        return false;
                    };
                    if (!findNative(fullName)) {
                        if (!findNative(name)) { // bare name fallback (e.g. "someField" from "SomeCtrl::someField")
                            if (!findDSO(fullName) && !findDSO(name)) {
                                Console::instance().printf(LogLevel::Debug, "VM: calling unknown func %s", fullName.c_str());
                                stack.push(VMValue(0));
                            }
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
                // Start string concatenation
                if (!stack.empty()) {
                    frame->strBuilder = stack.top().toString();
                    stack.pop();
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_REWIND_STR:
            case (uint32_t)DSOOpcode::OP_TERMINATE_REWIND_STR: {
                // Push finished concatenation
                if (!frame->strBuilder.empty()) {
                    stack.push(VMValue(frame->strBuilder));
                    frame->strBuilder.clear();
                }
                break;
            }

            // === Object operations ===
            case (uint32_t)DSOOpcode::OP_CREATE_OBJECT: {
                // Create a new script object
                if (execIp + 3 < opcodes.size()) {
                    uint32_t parentIdx = opcodes[execIp + 1];
                    (void)opcodes[execIp + 3];

                    const char* parent = "";
                    if (parentIdx < dso->functionStrings.size()) parent = &dso->functionStrings[parentIdx];
                    else if (parentIdx < dso->globalStrings.size()) parent = &dso->globalStrings[parentIdx];

                    auto* obj = new ScriptObject;
                    obj->className = parent;
                    frame->curObject = obj;
                    execIp += 3;
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_ADD_OBJECT: {
                if (frame->curObject) {
                    ScriptEngine::instance().objects[frame->curObject->name] = frame->curObject;
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
                    uint32_t idx = opcodes[execIp + 1];
                    const char* field = "";
                    if (idx < dso->functionStrings.size()) field = &dso->functionStrings[idx];
                    else if (idx < dso->globalStrings.size()) field = &dso->globalStrings[idx];
                    frame->curFieldName = field;
                }
                execIp++;
                break;
            }

            case (uint32_t)DSOOpcode::OP_SAVEFIELD_UINT:
            case (uint32_t)DSOOpcode::OP_SAVEFIELD_FLT:
            case (uint32_t)DSOOpcode::OP_SAVEFIELD_STR: {
                if (frame->curObject && !stack.empty()) {
                    frame->curObject->fields[frame->curFieldName] = stack.top();
                    stack.pop();
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_LOADFIELD_UINT:
            case (uint32_t)DSOOpcode::OP_LOADFIELD_FLT:
            case (uint32_t)DSOOpcode::OP_LOADFIELD_STR: {
                if (frame->curObject) {
                    auto it = frame->curObject->fields.find(frame->curFieldName);
                    if (it != frame->curObject->fields.end())
                        stack.push(it->second);
                    else
                        stack.push(VMValue(0));
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
                    uint32_t idx = opcodes[execIp + 1];
                    const char* name = "";
                    if (idx < dso->functionStrings.size()) name = &dso->functionStrings[idx];
                    else if (idx < dso->globalStrings.size()) name = &dso->globalStrings[idx];
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
    instance_ = nullptr;
}

ScriptEngine& ScriptEngine::instance() {
    return *instance_;
}

bool ScriptEngine::init() {
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
        return VMValue(1);
    });

    tsInstance->registerNative("warn", [](const auto& args) -> VMValue {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Warn, "%s", msg.c_str());
        return VMValue(1);
    });

    tsInstance->registerNative("error", [](const auto& args) -> VMValue {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Error, "%s", msg.c_str());
        return VMValue(1);
    });

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

    // Register str functions
    tsInstance->registerNative("strLen", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        return VMValue((int32_t)args[0].toString().size());
    });

    tsInstance->registerNative("strCmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        return VMValue(strcmp(args[0].toString().c_str(), args[1].toString().c_str()));
    });

    tsInstance->registerNative("strStr", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        auto pos = args[0].toString().find(args[1].toString());
        return VMValue(pos != std::string::npos ? (int32_t)pos : -1);
    });

    tsInstance->registerNative("getSubStr", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        auto s = args[0].toString();
        int start = args[1].toInt();
        int count = args[2].toInt();
        if (start < 0 || start >= (int)s.size() || count <= 0) return VMValue("");
        return VMValue(s.substr(start, count));
    });

    tsInstance->registerNative("isObject", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        if (name.empty()) return VMValue(0);
        auto& engine = ScriptEngine::instance();
        if (engine.findObject(name.c_str())) return VMValue(1);
        auto* item = Console::instance().find(name.c_str());
        if (item) return VMValue(1);
        return VMValue(0);
    });

    tsInstance->registerNative("isDemo", [](const auto&) -> VMValue {
        return VMValue(0);
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

    tsInstance->registerNative("getField", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        std::string s = args[0].toString();
        int idx = args[1].toInt();
        size_t start = 0;
        int count = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == '\t') {
                if (count == idx) return VMValue(s.substr(start, i - start));
                count++;
                start = i + 1;
            }
        }
        return VMValue("");
    });

    tsInstance->registerNative("setField", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue("");
        std::string s = args[0].toString();
        int idx = args[1].toInt();
        std::string val = args[2].toString();
        std::string result;
        size_t start = 0;
        int count = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == '\t') {
                if (count == idx) {
                    result += val;
                } else {
                    result += s.substr(start, i - start);
                }
                count++;
                if (i < s.size()) result += '\t';
                start = i + 1;
            }
        }
        if (idx >= count) {
            if (!result.empty()) result += '\t';
            result += val;
        }
        return VMValue(result);
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

    tsInstance->registerNative("detag", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string s = args[0].toString();
        std::string result;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i+1] == 'c') {
                i += 2;
                if (i < s.size() && s[i] >= '0' && s[i] <= '9') continue;
                if (i < s.size() && s[i] == 'o') continue;
                i--;
                continue;
            }
            result += s[i];
        }
        return VMValue(result);
    });

    tsInstance->registerNative("strcmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        return VMValue(strcmp(args[0].toString().c_str(), args[1].toString().c_str()));
    });

    // Canvas / window
    tsInstance->registerNative("createCanvas", [](const auto&) -> VMValue {
        // Create the GuiCanvas ScriptObject for the GUI renderer
        auto* canvas = new ScriptObject;
        canvas->className = "GuiCanvas";
        canvas->name = "Canvas";
        canvas->fields["extent"] = VMValue("1024 768");
        canvas->fields["position"] = VMValue("0 0");
        ScriptEngine::instance().objects["Canvas"] = canvas;
        Console::instance().printf(LogLevel::Info, "GUI: created GuiCanvas");
        return VMValue(1);
    });

    // Package management
    tsInstance->registerNative("activatePackage", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        // Check if already active
        auto* total = Console::instance().find("$TotalNumberOfPackages");
        int count = (total && total->type == Console::ConsoleItem::Variable) ? atoi(total->value.c_str()) : 0;
        char buf[64];
        for (int i = 0; i < count; i++) {
            snprintf(buf, sizeof(buf), "$Package[%d]", i);
            auto* pkg = Console::instance().find(buf);
            if (pkg && pkg->type == Console::ConsoleItem::Variable && pkg->value == name) {
                return VMValue(1); // already active
            }
        }
        // Add to list
        snprintf(buf, sizeof(buf), "$Package[%d]", count);
        Console::instance().setVariable(buf, name.c_str());
        Console::instance().setVariable("$TotalNumberOfPackages", std::to_string(count + 1).c_str());
        return VMValue(1);
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
        if (!Engine::instance().fs().readFile(path.c_str(), data)) return VMValue(0);
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
            sobj->internals.erase("__fo_data");
            sobj->internals.erase("__fo_pos");
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
        if (args.empty()) return VMValue(0);
        int ret = std::remove(args[0].toString().c_str());
        return VMValue(ret == 0 ? 1 : 0);
    });

    tsInstance->registerNative("getSimTime", [](const auto&) -> VMValue {
        return VMValue((int32_t)(Timer::now() * 1000.0));
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

    {
        static int nextTagId = 1;
        tsInstance->registerNative("addTaggedString", [](const auto& args) -> VMValue {
            if (args.empty()) return VMValue(0);
            return VMValue(nextTagId++);
        });
    }

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
        return VMValue(atan(args[0].toDouble()));
    });
    tsInstance->registerNative("mSqrt", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(sqrt(args[0].toDouble()));
    });
    tsInstance->registerNative("mAbs", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(fabs(args[0].toDouble()));
    });
    tsInstance->registerNative("mFloor", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(floor(args[0].toDouble()));
    });
    tsInstance->registerNative("mCeil", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0.0);
        return VMValue(ceil(args[0].toDouble()));
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
        if (args.size() < 2) return VMValue(args[0].toDouble());
        double val = args[0].toDouble();
        int len = args[1].toInt();
        if (len == 0) return VMValue((int32_t)val);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", len, val);
        return VMValue(atof(buf));
    });
    tsInstance->registerNative("getRandom", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue((double)rand() / RAND_MAX);
        if (args.size() == 1) return VMValue((int32_t)(rand() % (args[0].toInt() + 1)));
        int from = args[0].toInt();
        int to = args[1].toInt();
        if (from > to) std::swap(from, to);
        return VMValue((int32_t)(from + (rand() % (to - from + 1))));
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
    auto parseVec = [](const std::string& s) -> std::array<double, 3> {
        std::array<double, 3> v = {0,0,0};
        size_t pos = 0;
        for (int i = 0; i < 3 && pos < s.size(); i++) {
            size_t end = s.find_first_of(" \t", pos);
            if (end == std::string::npos) end = s.size();
            v[i] = atof(s.substr(pos, end - pos).c_str());
            pos = end + 1;
        }
        return v;
    };
    auto fmtVec = [](double x, double y, double z) -> std::string {
        char buf[128];
        snprintf(buf, sizeof(buf), "%g %g %g", x, y, z);
        return buf;
    };
    tsInstance->registerNative("VectorNormalize", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("0 0 0");
        auto v = parseVec(args[0].toString());
        double len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (len > 0) { v[0] /= len; v[1] /= len; v[2] /= len; }
        return VMValue(fmtVec(v[0], v[1], v[2]));
    });
    tsInstance->registerNative("VectorScale", [parseVec, fmtVec](const auto& args) -> VMValue {
        if (args.size() < 2) return args.empty() ? VMValue("0 0 0") : args[0];
        auto v = parseVec(args[0].toString());
        double s = args[1].toDouble();
        return VMValue(fmtVec(v[0]*s, v[1]*s, v[2]*s));
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
        return VMValue(1);
    });

    vmInstance->registerNativeFunction("warn", [](const auto& args) {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Warn, "%s", msg.c_str());
        return VMValue(1);
    });

    vmInstance->registerNativeFunction("error", [](const auto& args) {
        std::string msg;
        for (auto& a : args) {
            if (!msg.empty()) msg += " ";
            msg += a.toString();
        }
        Console::instance().printf(LogLevel::Error, "%s", msg.c_str());
        return VMValue(1);
    });

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
            dir + name + ".glb"
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
        if (args.empty()) return VMValue("0");
        int v = args[0].toInt();
        std::string r;
        for (int i = 31; i >= 0; i--) r += (v & (1 << i)) ? '1' : '0';
        // Trim leading zeros
        auto p = r.find_first_not_of('0');
        return VMValue(p != std::string::npos ? r.substr(p) : "0");
    });
    vmInstance->registerNativeFunction("BinToDec", [](const auto& args) {
        if (args.empty()) return VMValue(0);
        return VMValue((int32_t)std::stoul(args[0].toString(), nullptr, 2));
    });

    // T2 compatibility stubs (functions called by startup scripts)
    auto stubVM = [&](const char* name) {
        vmInstance->registerNativeFunction(name, [](const auto&) { return VMValue(1); });
    };
    auto stubVMS = [&](const char* name) {
        vmInstance->registerNativeFunction(name, [](const auto& args) {
            return args.empty() ? VMValue(1) : args[0];
        });
    };
    stubVM("audioSetDriver");
    vmInstance->registerNativeFunction("audioDetect", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isInitialized() ? 1 : 0);
    });
    vmInstance->registerNativeFunction("startAudio", [](const auto&) -> VMValue {
        auto& audio = Engine::instance().audio();
        if (!audio.isInitialized()) audio.init();
        return VMValue(1);
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
    auto stubTS = [&](const char* name) {
        tsInstance->registerNative(name, [](const auto&) { return VMValue(1); });
    };
    auto stubTSS = [&](const char* name) {
        tsInstance->registerNative(name, [](const auto& args) {
            return args.empty() ? VMValue(1) : args[0];
        });
    };
    stubTS("audioSetDriver");
    tsInstance->registerNative("audioDetect", [](const auto&) -> VMValue {
        return VMValue(Engine::instance().audio().isInitialized() ? 1 : 0);
    });
    tsInstance->registerNative("startAudio", [](const auto&) -> VMValue {
        auto& audio = Engine::instance().audio();
        if (!audio.isInitialized()) audio.init();
        return VMValue(1);
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
            if (!name.empty()) Engine::instance().guiRenderer().pushDialog(name);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("popDialog", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string name = args.back().toString();
            if (!name.empty()) Engine::instance().guiRenderer().popDialog(name);
        }
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
    // Mark a GUI control as persistent so setContent preserves it across panel swaps

    // Console functions used by ConsoleDlg.gui (ToggleConsole / ConsoleEntry::eval)
    tsInstance->registerNative("activateKeyboard", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("deactivateKeyboard", [](const auto&) -> VMValue { return VMValue(1); });
    tsInstance->registerNative("eval", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string code = args[0].toString();
            auto* ts = Engine::instance().script().ts();
            if (ts) ts->execute(code, "eval");
        }
        return VMValue(1);
    });
    // Console history
    tsInstance->registerNative("showCursor", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("hideCursor", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("setContent", [](const auto& args) -> VMValue {
        // May be called directly: setContent("Gui") or as method: Canvas.setContent("Gui")
        // In method form args = ["Canvas", "Gui"], direct form args = ["Gui"]
        if (!args.empty()) {
            std::string name = args.back().toString();
            if (!name.empty()) Engine::instance().guiRenderer().setContent(name);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("playGui", [](const auto& args) -> VMValue {
        if (!args.empty())
            Console::instance().printf(LogLevel::Info, "playGui: %s", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("Show", [](const auto& args) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("Hide", [](const auto& args) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("isActive", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("makeFirstResponder", [](const auto&) -> VMValue {
        return VMValue(1);
    });

    // File operations needed by startup scripts
    tsInstance->registerNative("isFile", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto& fs = Engine::instance().fs();
        return VMValue(fs.fileExists(args[0].toString().c_str()) ? 1 : 0);
    });
    tsInstance->registerNative("fileExt", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        auto dot = path.rfind('.');
        if (dot != std::string::npos) return VMValue(path.substr(dot + 1));
        return VMValue("");
    });
    tsInstance->registerNative("getFileName", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        auto slash = path.rfind('/');
        if (slash != std::string::npos) return VMValue(path.substr(slash + 1));
        return VMValue(path);
    });

    // Schedule: store callback for later execution
    tsInstance->registerNative("schedule", [](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            double delay = args[0].toDouble() / 1000.0; // ms to seconds
            std::string command = args[2].toString();
            // Build argument string
            for (size_t i = 3; i < args.size(); i++) {
                command += " " + args[i].toString();
            }
            Engine::instance().guiRenderer().addSchedule(delay, command);
        }
        return VMValue(1);
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
            Console::instance().printf(LogLevel::Debug, "findFirstFile(\"%s\"): found %zu files, first=\"%s\"", pattern.c_str(), s_fileList.size(), s_fileList.empty() ? "" : s_fileList[0].c_str());
            // Sort to match T2 behavior
            std::sort(s_fileList.begin(), s_fileList.end());
            return s_fileList.empty() ? VMValue("") : VMValue(s_fileList[0]);
        });
        tsInstance->registerNative("findNextFile", [](const auto&) -> VMValue {
            s_fileIdx++;
            return s_fileIdx < s_fileList.size() ? VMValue(s_fileList[s_fileIdx]) : VMValue("");
        });
    }

    tsInstance->registerNative("cleanupAudio", [](const auto&) -> VMValue {
        auto& audio = Engine::instance().audio();
        if (audio.isInitialized()) audio.shutdown();
        return VMValue(1);
    });
    tsInstance->registerNative("WONDisableFutureCalls", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("export", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string pattern = args[0].toString();
        std::string filePath = args[1].toString();
        bool append = args.size() > 2 && args[2].toBool();
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
            struct stat st; if (stat(dir.c_str(), &st) != 0) mkdir(dir.c_str(), 0755);
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
            bool match = prefixMatch ? strncmp(name, prefix.c_str(), prefix.size()) == 0 : name == prefix;
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
        auto& fs = Engine::instance().fs();
        // Try bare path first
        auto data = fs.read(execPath.c_str());
        if (data.empty()) data = fs.read(("base/" + execPath).c_str());
        if (!data.empty()) {
            std::string src((const char*)data.data(), data.size());
            auto* ts = Engine::instance().script().ts();
            if (ts) { ts->executeNested(src, execPath); }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("addMessageCallback", [](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            std::string msgType = args[0].toString();
            std::string callback = args[1].toString();
            Console::instance().printf(LogLevel::Debug, "addMessageCallback: type='%s' callback='%s'", msgType.c_str(), callback.c_str());
        }
        return VMValue(1);
    });

    tsInstance->registerNative("getTaggedString", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(std::string(""));
        return args[0];
    });

    tsInstance->registerNative("nameToId", [](const auto&) -> VMValue {
        return VMValue(0);
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

    tsInstance->registerNative("alxPlay", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        // Try creating a one-shot source from sound/Name.wav or sound/Name.ogg
        auto& audio = Engine::instance().audio();
        auto loadAndPlay = [&](const std::string& path) -> bool {
            SoundBuffer* buf = audio.loadSound(path.c_str());
            if (!buf) return false;
            SoundSource* src = audio.createSource();
            if (!src) return false;
            src->play(buf);
            return true;
        };
        // Map T2 sound names to files
        std::string path = "sound/" + name + ".wav";
        if (loadAndPlay(path)) return VMValue(1);
        path = "sound/" + name + ".ogg";
        if (loadAndPlay(path)) return VMValue(1);
        // Try lowercase variants
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        path = "sound/" + name + ".wav";
        if (loadAndPlay(path)) return VMValue(1);
        path = "sound/" + name + ".ogg";
        if (loadAndPlay(path)) return VMValue(1);
        return VMValue(0);
    });

    tsInstance->registerNative("GetIRCServerList", [](const auto&) -> VMValue {
        return VMValue(0);
    });

    // compile(path) — compile a .cs/.gui file to .dso for caching
    tsInstance->registerNative("compile", [](const auto& args) -> VMValue {
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
        // Try turd compiler, fall back to source cache
        auto dir = dsoPath.substr(0, dsoPath.rfind('/'));
        struct stat st; if (stat(dir.c_str(), &st) != 0) { std::string cmd = "mkdir -p " + dir; system(cmd.c_str()); }
        std::string tmpPath = dsoPath + ".src.tmp";
        { FILE* f = fopen(tmpPath.c_str(), "w"); if (f) { fwrite(src.data(), 1, src.size(), f); fclose(f); } }
        std::string cmd = Console::instance().getStringVariable("nodePath");
        if (cmd.empty()) cmd = "node";
        std::string compilerScript = Console::instance().getStringVariable("compilerScript");
        if (compilerScript.empty()) compilerScript = "torque-dso.js";
        cmd += " " + compilerScript + " " + tmpPath + " " + dsoPath + " 2>/dev/null";
        int ret = system(cmd.c_str());
        unlink(tmpPath.c_str());
        if (ret != 0) {
            // Fall back: write minimal source-cache DSO
            FILE* f = fopen(dsoPath.c_str(), "wb");
            if (f) { uint32_t ver = 0x54534F01, cnt = 0; fwrite(&ver, 4, 1, f); fwrite(&cnt, 4, 1, f); fclose(f); }
        }
        return VMValue(1);
    });

    // Missing startup function stubs
    tsInstance->registerNative("deactivateDirectInput", [](const auto&) -> VMValue {
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
    tsInstance->registerNative("cancel", [](const auto&) -> VMValue {
        return VMValue(1);
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
            ctl->selectedRow = -1;
            ctl->menuItems.clear();
        }
        return VMValue(1);
    });
    tsInstance->registerNative("add", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(1);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(1);
        int id = (int)args[1].toDouble();
        std::string txt = args[2].toString();
        ctl->menuItems.push_back({id, txt, false});
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
            ctl->tabs[id] = {txt, true};
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
                if (ts && ts->hasFunction(ctl->name + "::onSelect"))
                    ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(idx), VMValue(ctl->tabs[idx].text)});
            }
        }
        return VMValue(1);
    });
    // setSelected is the T2 alias for setSelectedByIndex.
    tsInstance->registerNative("setSelected", [getOrCreateCtrl](const auto& args) -> VMValue {
        auto* ctl = getOrCreateCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 2) {
            int idx = (int)args[1].toDouble();
            ctl->selectedTab = idx;
            if (idx >= 0 && idx < (int)ctl->tabs.size()) {
                auto* ts = Engine::instance().script().ts();
                if (ts && ts->hasFunction(ctl->name + "::onSelect"))
                    ts->callFunction(ctl->name + "::onSelect", {VMValue(ctl->name), VMValue(idx), VMValue(ctl->tabs[idx].text)});
            }
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
        return VMValue(ctl ? ctl->selectedRow : 0);
    });
    tsInstance->registerNative("getSelectedId", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl) return VMValue(0);
        return VMValue(ctl->selectedRow);
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
        if (id >= 0 && id < (int)ctl->listRows.size())
            return VMValue(ctl->listRows[id]);
        return VMValue(std::string(""));
    });
    tsInstance->registerNative("size", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        return VMValue((int32_t)(ctl ? (int32_t)ctl->listRows.size() : 0));
    });
    tsInstance->registerNative("findText", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(0);
        std::string search = args[1].toString();
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
                ctl->visible = args[1].toInt() != 0;
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setActive", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->active = args[1].toInt() != 0;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("delete", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string objName = args[0].toString();
            auto* obj = ScriptEngine::instance().findObject(objName.c_str());
            if (obj) {
                // Don't delete GUI objects — they're managed by C++ renderer
                if (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 ||
                    obj->className.find("Sim") == 0 || objName.find("Profile") != std::string::npos) {
                    Console::instance().printf(LogLevel::Debug, "delete: skipped GUI object '%s' (class=%s)", objName.c_str(), obj->className.c_str());
                    return VMValue(1);
                }
                Console::instance().printf(LogLevel::Debug, "delete: removing ScriptObject '%s'", objName.c_str());
                ScriptEngine::instance().objects.erase(objName);
                delete obj;
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setValue", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                if (ctl->className == "GuiRadioCtrl" || ctl->className == "ShellRadioButton" || ctl->className == "GuiCheckBoxCtrl")
                    ctl->checked = args[1].toBool();
                else if (ctl->className.find("Slider") != std::string::npos)
                    ctl->sliderValue = (float)args[1].toInt() / 1000.0f;  // T2 convention: value * 1000
                else if (ctl->className.find("Window") != std::string::npos && args.size() >= 4) {
                    ctl->posX = (float)args[1].toInt();
                    ctl->posY = (float)args[2].toInt();
                    if (args.size() >= 5) { ctl->extentX = (float)args[3].toInt(); ctl->extentY = (float)args[4].toInt(); }
                }
                else
                    ctl->text = args[1].toString();
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("getValue", [getListCtrl](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue(0);
        if (ctl->className.find("Slider") != std::string::npos)
            return VMValue((int)(ctl->sliderValue * 1000.0f));  // T2 convention
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
        auto* ctl = getListCtrl(args[0].toString());
        if (!ctl) return VMValue("0 0");
        return VMValue(std::to_string((int)ctl->posX) + " " + std::to_string((int)ctl->posY));
    });
    tsInstance->registerNative("scrollToTop", [getListCtrl](const auto& args) -> VMValue {
        if (!args.empty()) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->scrollY = 0;
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setBitmap", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->bitmap = args[1].toString();
        }
        return VMValue(1);
    });
    tsInstance->registerNative("repaint", [](const auto&) -> VMValue {
        Engine::instance().guiRenderer().refresh();
        return VMValue(1);
    });
    tsInstance->registerNative("getRowNumById", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (!ctl || args.size() < 2) return VMValue(-1);
        int id = args[1].toInt();
        for (int i = 0; i < (int)ctl->listRows.size(); i++)
            if (i == id) return VMValue(i);
        return VMValue(-1);
    });
    tsInstance->registerNative("setRowColor", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 3) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) {
                int row = args[1].toInt();
                std::string color = args[2].toString();
                Console::instance().printf(LogLevel::Debug, "setRowColor: ctl='%s' row=%d color='%s'", args[0].toString().c_str(), row, color.c_str());
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
                Console::instance().printf(LogLevel::Debug, "setRowStyle: ctl='%s' row=%d style='%s'", args[0].toString().c_str(), row, style.c_str());
            }
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setText", [getListCtrl](const auto& args) -> VMValue {
        if (args.size() >= 2) {
            auto* ctl = getListCtrl(args[0].toString());
            if (ctl) ctl->text = args[1].toString();
        }
        return VMValue(1);
    });
    tsInstance->registerNative("cancelServerQuery", [](const auto&) -> VMValue {
        Console::instance().printf(LogLevel::Debug, "cancelServerQuery");
        return VMValue(1);
    });
    tsInstance->registerNative("localConnect", [](const auto& args) -> VMValue {
        std::string mission = args.empty() ? "" : args[0].toString();
        Console::instance().printf(LogLevel::Info, "localConnect: starting local game '%s'", mission.c_str());
        Engine::instance().game().startLocalGame(mission.empty() ? nullptr : mission.c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("addColumn", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && args.size() >= 3) {
            std::string colName = args[1].toString();
            float colWidth = (float)args[2].toDouble();
            ctl->sbColumns.push_back({colName, colWidth, true});
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
        Engine::instance().audio().stopAll();
        s_audioSources.clear();
        return VMValue(1);
    });
    tsInstance->registerNative("alxListenerf", [](const auto&) -> VMValue {
        // Listener float params — T2 sets these, we can ignore for now
        return VMValue(1);
    });
    tsInstance->registerNative("alxSetChannelVolume", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(1);
        std::string channel = args[0].toString();
        float vol = (float)args[1].toDouble();
        auto& cfg = Engine::instance().audio().config();
        if (channel == "Master") cfg.masterVolume = vol;
        else if (channel == "SFX" || channel == "SoundEffects") cfg.sfxVolume = vol;
        else if (channel == "Music") cfg.musicVolume = vol;
        return VMValue(1);
    });
    tsInstance->registerNative("alxCreateSource", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(0);
        std::string name = args[0].toString();
        std::string soundName = args[1].toString();
        auto& audio = Engine::instance().audio();
        SoundBuffer* buf = nullptr;
        auto tryLoad = [&](const std::string& path) {
            buf = audio.loadSound(path.c_str());
            return buf != nullptr;
        };
        std::string path = "sound/" + soundName + ".wav";
        if (!tryLoad(path)) { path = "sound/" + soundName + ".ogg"; tryLoad(path); }
        if (!buf) {
            for (auto& c : soundName) c = (char)tolower((unsigned char)c);
            path = "sound/" + soundName + ".wav";
            if (!tryLoad(path)) { path = "sound/" + soundName + ".ogg"; tryLoad(path); }
        }
        if (!buf) return VMValue(0);
        SoundSource* src = audio.createSource();
        if (!src) return VMValue(0);
        src->play(buf);
        src->stop(); // created but not playing yet
        s_audioSources[name] = src;
        return VMValue(1);
    });
    tsInstance->registerNative("alxGetWaveLen", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it == s_audioSources.end()) return VMValue(0);
        // Return 1000 as a default reasonable length (T2 stubs often return this)
        return VMValue(1000.0);
    });
    tsInstance->registerNative("alxSourcef", [](const auto& args) -> VMValue {
        if (args.size() < 3) return VMValue(1);
        std::string name = args[0].toString();
        std::string param = args[1].toString();
        float val = (float)args[2].toDouble();
        auto it = s_audioSources.find(name);
        if (it == s_audioSources.end()) return VMValue(1);
        auto* src = it->second;
        if (param == "volume") src->setVolume(val);
        else if (param == "pitch") src->setPitch(val);
        else if (param == "looping") src->setLooping(val != 0);
        return VMValue(1);
    });
    tsInstance->registerNative("alxStop", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(1);
        std::string name = args[0].toString();
        auto it = s_audioSources.find(name);
        if (it != s_audioSources.end()) it->second->stop();
        return VMValue(1);
    });

    // Display notifications — store messages in console variables for HUD to render
    tsInstance->registerNative("bottomPrint", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("HUD::bottomPrint", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("centerPrint", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("HUD::centerPrint", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("clearBottomPrint", [](const auto&) -> VMValue {
        Console::instance().setVariable("HUD::bottomPrint", "");
        return VMValue(1);
    });
    tsInstance->registerNative("clearCenterPrint", [](const auto&) -> VMValue {
        Console::instance().setVariable("HUD::centerPrint", "");
        return VMValue(1);
    });
    tsInstance->registerNative("bottomPrintAll", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("HUD::bottomPrint", args[0].toString().c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("centerPrintAll", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("HUD::centerPrint", args[0].toString().c_str());
        return VMValue(1);
    });

    // WON/Login stubs — store login info so login flow can proceed
    tsInstance->registerNative("WONLoginResult", [](const auto& args) -> VMValue {
        Console::instance().setVariable("WON::loginResult", args.empty() ? "0" : args[0].toString().c_str());
        return VMValue(0);
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
        if (args.size() >= 2) {
            std::string material = args[0].toString();
            std::string sound = args[1].toString();
            Console::instance().printf(LogLevel::Debug, "addMaterialMapping: %s -> %s", material.c_str(), sound.c_str());
            Console::instance().setVariable(("MaterialMap::" + material).c_str(), sound.c_str());
        }
        return VMValue(1);
    });

    // Networking: route commands through the game's connection
    tsInstance->registerNative("commandToClient", [](const auto& args) -> VMValue {
        // commandToClient(client, funcName, arg1, arg2, ...)
        if (args.size() < 2) return VMValue(0);
        std::string func = args[1].toString();
        std::string cmd = func;
        for (size_t i = 2; i < args.size(); i++)
            cmd += " " + args[i].toString();
        Console::instance().printf(LogLevel::Debug, "commandToClient: %s", cmd.c_str());
        // Send over wire if connected (server to client)
        auto* conn = Engine::instance().game().activeConnection();
        if (conn && conn->isConnected()) {
            conn->sendCommandPacket(cmd.c_str());
        } else {
            // Local fallback: execute directly
            auto* ts = Engine::instance().script().ts();
            if (ts && ts->hasFunction(func))
                ts->callFunction(func, {});
        }
        return VMValue(1);
    });
    tsInstance->registerNative("commandToServer", [](const auto& args) -> VMValue {
        // commandToServer(funcName, arg1, arg2, ...)
        if (args.empty()) return VMValue(0);
        std::string func = args[0].toString();
        std::string cmd = func;
        for (size_t i = 1; i < args.size(); i++)
            cmd += " " + args[i].toString();
        Console::instance().printf(LogLevel::Debug, "commandToServer: %s", cmd.c_str());
        // Send over wire if connected (client to server)
        auto* conn = Engine::instance().game().activeConnection();
        if (conn && conn->isConnected()) {
            conn->sendCommandPacket(cmd.c_str());
        } else {
            // Local fallback: execute directly
            Console::instance().execute(cmd.c_str());
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
        return VMValue(1);
    });
    tsInstance->registerNative("queryMasterServer", [](const auto& args) -> VMValue {
        std::string masterUrl = args.empty() ? "" : args[0].toString();
        if (!masterUrl.empty()) {
            Console::instance().printf(LogLevel::Info, "queryMasterServer: %s", masterUrl.c_str());
            Engine::instance().network().queryMasterServer(masterUrl.c_str());
        } else {
            // Default TribesNext master
            Engine::instance().network().queryMasterServer("tribesnext.com:28001");
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
            if (id >= (int)ctl->listRows.size()) ctl->listRows.resize(id + 1);
            ctl->listRows[id] = txt;
            Console::instance().printf(LogLevel::Debug, "addRow2: ctl='%s' id=%d txt='%s' rows=%zu", cname.c_str(), id, txt.c_str(), ctl->listRows.size());
        } else {
            Console::instance().printf(LogLevel::Debug, "addRow2: FAIL ctl=%p cname='%s' args=%zu", (void*)ctl, cname.c_str(), args.size());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("clearList", [getListCtrl](const auto& args) -> VMValue {
        std::string cname = args.empty() ? "" : args[0].toString();
        auto* ctl = getListCtrl(cname);
        if (ctl) { ctl->listRows.clear(); ctl->selectedRow = -1; Console::instance().printf(LogLevel::Debug, "clearList: ctl='%s' ok", cname.c_str()); }
        else Console::instance().printf(LogLevel::Debug, "clearList: FAIL ctl=NULL cname='%s'", cname.c_str());
        return VMValue(1);
    });
    tsInstance->registerNative("sort", [getListCtrl](const auto& args) -> VMValue {
        auto* ctl = getListCtrl(args.empty() ? "" : args[0].toString());
        if (ctl && !ctl->listRows.empty()) {
            int col = args.size() > 1 ? args[1].toInt() : 0;
            std::sort(ctl->listRows.begin(), ctl->listRows.end(),
                [col](const std::string& a, const std::string& b) {
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
                    return getField(a, col) < getField(b, col);
                });
        }
        return VMValue(1);
    });
    tsInstance->registerNative("refreshSelectedServer", [](const auto&) -> VMValue {
        Console::instance().printf(LogLevel::Debug, "refreshSelectedServer");
        return VMValue(1);
    });
    tsInstance->registerNative("insertIPAddress", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().setVariable("HUD::serverAddress", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("findNextServer", [](const auto&) -> VMValue {
        return VMValue(0);
    });
    tsInstance->registerNative("getServerInfoString", [](const auto&) -> VMValue {
        auto& renderer = Engine::instance().guiRenderer();
        auto* browser = renderer.findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue("");
        auto& srv = browser->sbServers[browser->sbSelected];
        char buf[512];
        snprintf(buf, sizeof(buf), "%s\t%s\t\t%d\t%d\t%d\t%s\t", srv.name.c_str(), srv.addr.toString().c_str(), srv.ping, srv.numPlayers, srv.maxPlayers, srv.gameType.c_str());
        return VMValue(buf);
    });
    tsInstance->registerNative("getServerStatus", [](const auto&) -> VMValue {
        auto& renderer = Engine::instance().guiRenderer();
        auto* browser = renderer.findControl("GMJ_Browser");
        if (!browser || browser->sbSelected < 0 || browser->sbSelected >= (int)browser->sbServers.size())
            return VMValue("invalid");
        return VMValue("responded");
    });
    tsInstance->registerNative("joinSelectedGame", [](const auto&) -> VMValue {
        std::string addr = Console::instance().getStringVariable("JoinGameAddress", "");
        if (!addr.empty()) {
            Console::instance().printf(LogLevel::Info, "joinSelectedGame: connecting to %s", addr.c_str());
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                std::string host = addr.substr(0, colon);
                int port = atoi(addr.substr(colon + 1).c_str());
                Engine::instance().game().connectToServer(host.c_str(), (uint16_t)port);
            }
        }
        return VMValue(1);
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
            if (ctl && args.size() >= 2) {
                Console::instance().printf(LogLevel::Debug, "setAltColor: ctl='%s' color='%s'", args[0].toString().c_str(), args[1].toString().c_str());
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

    // Container/spatial query stubs (basic implementations)
    static struct { int nextIdx; bool active; } s_containerSearch = {0, false};
    tsInstance->registerNative("InitContainerRadiusSearch", [](const auto&) -> VMValue {
        s_containerSearch = {0, true};
        return VMValue(1);
    });
    tsInstance->registerNative("containerSearchNext", [](const auto&) -> VMValue {
        if (!s_containerSearch.active) return VMValue(0);
        if (s_containerSearch.nextIdx == 0) { s_containerSearch.nextIdx++; return VMValue("Player"); }
        s_containerSearch.active = false;
        return VMValue(0);
    });
    tsInstance->registerNative("containerRayCast", [](const auto&) -> VMValue {
        return VMValue("");
    });
    tsInstance->registerNative("containerSearchCurrDist", [](const auto&) -> VMValue { return VMValue(0.0); });
    tsInstance->registerNative("containerSearchCurrRadDamageDist", [](const auto&) -> VMValue { return VMValue(0.0); });
    tsInstance->registerNative("calcExplosionCoverage", [](const auto&) -> VMValue { return VMValue(1.0); });

    // Misc startup stubs
    tsInstance->registerNative("setModPaths", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            Console::instance().setVariable("modPath", args[0].toString().c_str());
            Console::instance().printf(LogLevel::Info, "setModPaths: %s", args[0].toString().c_str());
        }
        return VMValue(1);
    });
    tsInstance->registerNative("setEchoFileLoads", [](const auto& args) -> VMValue {
        if (!args.empty()) Console::instance().setVariable("echoFileLoads", args[0].toInt() ? "1" : "0");
        return VMValue(1);
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
    struct BindEntry { std::string cmdOn; std::string cmdOff; bool isCmd = false; };
    static std::map<std::tuple<std::string, int, std::string>, BindEntry> s_actionBinds;

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
            if (obj) {
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
        // The command is the last arg; flags/modifier are ignored for now
        std::string command = args[start + args.size() - start - 1 + start - 1].toString();
        // Actually, the command is args[start + 2] for simple form, or last arg for flag form
        // T2 forms: bind(dev, key, cmd) | bind(dev, key, flags, modifier, cmd) | bind(dev, key, flags, modifier, deadZone, sens, cmd)
        // The command is always the last non-flag arg. Simple approach: last arg is always the command.
        command = args.back().toString();
        // Store binding
        auto key = std::make_tuple(objName, device, keyName);
        s_actionBinds[key] = {command, "", false};
        // Also store in simple action map for key lookups
        std::string actionKey = keyName;
        Console::instance().printf(LogLevel::Debug, "TS: bind(%s, %d, '%s') = '%s'",
            objName.empty() ? "?" : objName.c_str(), device, keyName.c_str(), command.c_str());
        return VMValue(1);
    });

    tsInstance->registerNative("bindcmd", [&s_actionBinds, parseDevice](const auto& args) -> VMValue {
        size_t start = 0;
        std::string objName;
        if (!args.empty()) {
            auto* obj = ScriptEngine::instance().findObject(args[0].toString().c_str());
            if (obj) { objName = args[0].toString(); start = 1; }
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
            if (obj) { objName = args[0].toString(); start = 1; }
        }
        if (args.size() - start < 2) return VMValue(0);
        int device = parseDevice(args[start].toString());
        std::string keyName = args[start + 1].toString();
        auto key = std::make_tuple(objName, device, keyName);
        s_actionBinds.erase(key);
        return VMValue(1);
    });

    tsInstance->registerNative("copybind", [&s_actionBinds](const auto& args) -> VMValue {
        // copyBind(sourceMap, command) — copy a binding from sourceMap to this map
        // For now, just stub it
        return VMValue(1);
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
        std::string str = args[0].toString();
        int idx = args.size() > 1 ? args[1].toInt() : 0;
        int word = 0;
        size_t start = 0;
        while (start < str.size()) {
            size_t pos = str.find(' ', start);
            if (word == idx) {
                size_t end = (pos != std::string::npos) ? pos : str.size();
                return VMValue(str.substr(start, end - start));
            }
            if (pos == std::string::npos) break;
            start = pos + 1;
            word++;
        }
        return VMValue("");
    });

    tsInstance->registerNative("getWordCount", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue(0);
        std::string str = args[0].toString();
        if (str.empty()) return VMValue(0);
        int count = 1;
        for (char c : str) if (c == ' ') count++;
        return VMValue(count);
    });

    // firstWord — returns first word
    tsInstance->registerNative("firstWord", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string str = args[0].toString();
        size_t pos = str.find(' ');
        return VMValue(pos != std::string::npos ? str.substr(0, pos) : str);
    });

    // Copy all TS-registered natives to DSO VM so DSO functions can find them
    for (auto& entry : tsInstance->getNatives()) {
        vmInstance->registerNativeFunction(entry.first.c_str(), entry.second);
    }
    Console::instance().printf(LogLevel::Info, "ScriptEngine initialized with %zu native functions + DTS support", 12);
    return true;
}

void ScriptEngine::shutdown() {
    // Clean up objects
    for (auto& [name, obj] : objects) delete obj;
    objects.clear();
    delete tsInstance;
    tsInstance = nullptr;
    delete vmInstance;
    vmInstance = nullptr;
}

void ScriptEngine::registerFunction(const char* name, NativeFunc fn) {
    if (vmInstance) vmInstance->registerNativeFunction(name, std::move(fn));
}

ScriptObject* ScriptEngine::findObject(const char* name) {
    auto it = objects.find(name);
    return it != objects.end() ? it->second : nullptr;
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
