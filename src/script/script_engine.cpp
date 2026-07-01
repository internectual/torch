#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/string_table.h"
#include <fstream>
#include <stack>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

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

    Console::instance().printf(LogLevel::Warn, "VM: function not found: %s", name);
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
            for (uint32_t ai = 0; ai < fn.argc && ai < (uint32_t)args.size(); ai++) {
                std::string localName = "%" + fn.argNames[ai];
                ctx.locals[localName] = args[ai];
            }
            // Also store varargs if present
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
        void push(const VMValue& x) { v.push_back(x); }
        VMValue pop() { if (v.empty()) return {}; VMValue x = v.back(); v.pop_back(); return x; }
        VMValue& top() { static VMValue def; if (v.empty()) { def = {}; return def; } return v.back(); }
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

                    // Check native first
                    auto nit = impl->natives.find(fullName);
                    if (nit != impl->natives.end()) {
                        stack.push(nit->second(callArgs));
                    } else {
                        // Look up in loaded DSOs
                        bool found = false;
                        for (auto* ds : impl->loaded) {
                            auto fit = ds->funcMap.find(fullName);
                            if (fit != ds->funcMap.end()) {
                                VMValue result = execute(ds, fit->second->startIp, callArgs);
                                stack.push(result);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
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
                    frame->curFieldName = stack.top().toString();
                    stack.pop();
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
        return VMValue(Engine::instance().game().isDemoPlaying() ? 1 : 0);
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

    tsInstance->registerNative("enableWinConsole", [](const auto& args) -> VMValue {
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

    tsInstance->registerNative("enableWinConsole", [](const auto&) -> VMValue {
        return VMValue(1);
    });

    tsInstance->registerNative("fileExt", [](const auto& args) -> VMValue {
        if (args.empty()) return VMValue("");
        std::string path = args[0].toString();
        size_t dot = path.rfind('.');
        if (dot == std::string::npos) return VMValue("");
        return VMValue(path.substr(dot + 1));
    });

    tsInstance->registerNative("stricmp", [](const auto& args) -> VMValue {
        if (args.size() < 2) return VMValue(-1);
        std::string a = args[0].toString(), b = args[1].toString();
        for (auto& c : a) c = tolower((unsigned char)c);
        for (auto& c : b) c = tolower((unsigned char)c);
        return VMValue(strcmp(a.c_str(), b.c_str()));
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
    stubVM("audioDetect");
    stubVM("startAudio");
    stubVM("setZoomSpeed");
    stubVM("setShadowDetailLevel");
    stubVM("setOpenGLTextureCompressionHint");
    stubVM("setOpenGLSkyMipReduction");
    stubVM("setOpenGLMipReduction");
    stubVM("setOpenGLInteriorMipReduction");
    stubVM("setOpenGLAnisotropy");
    stubVM("setLogMode");
    stubVM("enableWinConsole");
    stubVMS("setDefaultFov");

    // GUI Canvas methods (called as Canvas.pushDialog() etc.)
    // These are registered globally so the dot-notation lookup finds them
    tsInstance->registerNative("pushDialog", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string name = args[0].toString();
            if (!name.empty()) Engine::instance().guiRenderer().pushDialog(name);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("popDialog", [](const auto& args) -> VMValue {
        if (!args.empty()) {
            std::string name = args[0].toString();
            Engine::instance().guiRenderer().popDialog(name);
        }
        return VMValue(1);
    });
    tsInstance->registerNative("showCursor", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("hideCursor", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("setContent", [](const auto&) -> VMValue {
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
    tsInstance->registerNative("setVisible", [](const auto& args) -> VMValue {
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
        if (dot != std::string::npos) return VMValue(path.substr(dot));
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
            // Strip "*" prefix/suffix for listFiles matching
            std::string clean = pattern;
            if (clean.size() > 1 && clean[0] == '*') clean = clean.substr(1);
            if (clean.size() > 1 && clean.back() == '*') clean.pop_back();
            Engine::instance().fs().listFiles(clean.c_str(), s_fileList);
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

    // Login flow stubs (called by T2 startup scripts)
    tsInstance->registerNative("CleanUpAndGo", [](const auto&) -> VMValue {
        Console::instance().printf(LogLevel::Info, "Login complete: CleanUpAndGo");
        return VMValue(1);
    });
    tsInstance->registerNative("cleanupAudio", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("WONDisableFutureCalls", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("export", [](const auto&) -> VMValue {
        return VMValue(1);
    });
    tsInstance->registerNative("addMessageCallback", [](const auto&) -> VMValue {
        return VMValue(1);
    });

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
