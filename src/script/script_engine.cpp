#include "script/script_engine.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/string_table.h"
#include <fstream>
#include <stack>
#include <cstring>
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
    std::vector<VMValue> locals;
    std::string curVarName;
    ScriptObject* curObject{};
    std::string curFieldName;
    VMValue result;

    // String builder
    std::string strBuilder;
    int strBuilderLen{};
};

struct VirtualMachine::Impl {
    ScriptEngine* engine;
    std::vector<DSOFile*> loaded;
    std::stack<VMContext> callStack;
    std::unordered_map<std::string, VMValue> globals;
    std::unordered_map<std::string, NativeFunc> natives;

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
    const uint8_t* codeBase = code;
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
                }
                i += 8; // skip func decl args
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
            execute(dso, fit->second->startIp, args);
            return impl->callStack.empty() ? VMValue(0) : impl->callStack.top().result;
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

void VirtualMachine::execute(DSOFile* dso, uint32_t startIp, const std::vector<VMValue>& args) {
    VMContext ctx;
    ctx.dso = dso;
    ctx.ip = startIp;
    ctx.locals = args;

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
                if (!stack.empty()) {
                    frame->result = stack.top();
                }
                impl->callStack.pop();
                return;
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
                // Push current locals/frame
                break;
            }

            case (uint32_t)DSOOpcode::OP_PUSH_FRAME: {
                // Start a new argument frame
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
                // Load current variable onto stack
                if (!frame->curVarName.empty()) {
                    if (frame->curVarName[0] == '%') {
                        // Local variable
                        std::string localName = frame->curVarName.substr(1);
                        for (auto& lv : frame->locals) {
                            // Match by name stored in local
                            stack.push(lv);
                            break;
                        }
                    } else {
                        // Global
                        stack.push(impl->globals[frame->curVarName]);
                    }
                }
                break;
            }

            case (uint32_t)DSOOpcode::OP_SAVEVAR_UINT:
            case (uint32_t)DSOOpcode::OP_SAVEVAR_FLT:
            case (uint32_t)DSOOpcode::OP_SAVEVAR_STR: {
                if (!frame->curVarName.empty() && !stack.empty()) {
                    VMValue val = stack.top(); stack.pop();
                    if (frame->curVarName[0] == '%') {
                        // Local - store in locals
                        // For now, skip local storage
                    } else {
                        impl->globals[frame->curVarName] = val;
                        Console::instance().setVariable(frame->curVarName.c_str(), val.toString().c_str());
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
                    uint32_t callType = opcodes[execIp + 3];

                    const char* name = "";
                    if (nameIdx < dso->functionStrings.size()) name = &dso->functionStrings[nameIdx];
                    else if (nameIdx < dso->globalStrings.size()) name = &dso->globalStrings[nameIdx];

                    const char* ns = "";
                    if (nsIdx < dso->functionStrings.size()) ns = &dso->functionStrings[nsIdx];
                    else if (nsIdx < dso->globalStrings.size()) ns = &dso->globalStrings[nsIdx];

                    // Collect arguments from stack (pushed before call)
                    std::vector<VMValue> callArgs;
                    if (callType == 0) {
                        // Normal call - collect from current frame's arguments
                        // In TGE, arguments are pushed on the stack
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
                                execute(ds, fit->second->startIp, callArgs);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            Console::instance().printf(LogLevel::Debug, "VM: calling unknown func %s", fullName.c_str());
                            stack.push(VMValue(0));
                        } else {
                            // Get result from completed call
                            stack.push(VMValue(0)); // placeholder
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
                    uint32_t isDataBlock = opcodes[execIp + 2];
                    uint32_t failAddr = opcodes[execIp + 3];

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

            case (uint32_t)DSOOpcode::OP_BREAK:
                Console::instance().printf(LogLevel::Debug, "VM: breakpoint at IP %zu", execIp);
                break;

            default:
                if (op != (uint32_t)DSOOpcode::OP_FUNC_DECL &&
                    op != (uint32_t)DSOOpcode::OP_SETCURVAR_ARRAY &&
                    op != (uint32_t)DSOOpcode::OP_SETCURVAR_ARRAY_CREATE &&
                    op != (uint32_t)DSOOpcode::OP_SETCURFIELD_ARRAY &&
                    op != (uint32_t)DSOOpcode::OP_SETCUROBJECT_INTERNAL &&
                    op != (uint32_t)DSOOpcode::OP_ADVANCE_STR_APPENDCHAR &&
                    op != (uint32_t)DSOOpcode::OP_ADVANCE_STR_COMMA &&
                    op != (uint32_t)DSOOpcode::OP_ADVANCE_STR_NUL &&
                    op != (uint32_t)DSOOpcode::OP_UNIT_CONVERSION &&
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

    // Register native functions for TorqueScript
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

    Console::instance().printf(LogLevel::Info, "ScriptEngine initialized with %zu native functions", 9);
    return true;
}

void ScriptEngine::shutdown() {
    // Clean up objects
    for (auto& [name, obj] : objects) delete obj;
    objects.clear();
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
    Console::instance().execute(script);
}

void ScriptEngine::executeFile(const char* path) {
    Console::instance().executeFile(path);
}
