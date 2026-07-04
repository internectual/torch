#pragma once
#include "script/dso_reader.h"
#include "core/console.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <stack>
#include <functional>
#include <cstdint>

class ScriptEngine;
class TorqueScript;

struct VMValue {
    enum Type { None, Int, Float, String };
    Type type = None;
    union { int32_t i = 0; float f; };
    double dbl = 0; // for float table loading
    std::string str;

    VMValue() = default;
    VMValue(int32_t v) : type(Int), i(v) {}
    VMValue(float v) : type(Float), f(v), dbl(v) {}
    VMValue(double v) : type(Float), f((float)v), dbl(v) {}
    VMValue(const char* v) : type(String), str(v ? v : "") {}
    VMValue(const std::string& v) : type(String), str(v) {}

    int32_t toInt() const;
    float toFloat() const;
    double toDouble() const;
    std::string toString() const;
    bool toBool() const;
};

struct ScriptObject {
    std::string className;
    std::string name;
    std::unordered_map<std::string, VMValue> fields;
    std::unordered_map<std::string, VMValue> internals;
};

class VirtualMachine {
public:
    using NativeFunc = std::function<VMValue(const std::vector<VMValue>&)>;

    VirtualMachine(ScriptEngine* engine);
    ~VirtualMachine();

    bool loadScript(const uint8_t* data, size_t size, const char* name = nullptr);
    bool loadScriptFile(const char* path);

    VMValue callFunction(const char* name, const std::vector<VMValue>& args = {});
    VMValue callMethod(const char* objName, const char* method, const std::vector<VMValue>& args = {});

    void setVariable(const char* name, const VMValue& val);
    VMValue getVariable(const char* name);

    ScriptObject* getObject(const char* name);
    void addObject(ScriptObject* obj);

    void registerNativeFunction(const char* name, NativeFunc fn);

    VMValue execute(DSOFile* dso, uint32_t startIp, const std::vector<VMValue>& args);
    const std::vector<DSOFile*>& loadedScripts() const;

private:
    struct Impl;
    Impl* impl;
};

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    static ScriptEngine& instance();

    bool init();
    void shutdown();

    void executeString(const char* script);
    void executeFile(const char* path);

    using NativeFunc = VirtualMachine::NativeFunc;
    void registerFunction(const char* name, NativeFunc fn);

    VirtualMachine* vm() { return vmInstance; }
    TorqueScript* ts() { return tsInstance; }

    ScriptObject* findObject(const char* name);

    // Global object registry
    std::unordered_map<std::string, ScriptObject*> objects;

private:
    static ScriptEngine* instance_;
    VirtualMachine* vmInstance{};
    TorqueScript* tsInstance{};
    Console* con{};
};
