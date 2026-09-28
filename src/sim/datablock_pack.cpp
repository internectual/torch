#include "sim/datablock_pack.h"
#include "net/v12_registry.h"
#include "script/script_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <strings.h>
#include <vector>

namespace DataBlockPack {

static const VMValue* field(const ScriptObject* data, const char* name) {
    if (!data) return nullptr;
    for (const auto& [key, value] : data->fields)
        if (strcasecmp(key.c_str(), name) == 0) return &value;
    return nullptr;
}

static std::vector<float> floats(const std::string& text) {
    std::vector<float> out;
    std::istringstream in(text);
    float v;
    while (in >> v) out.push_back(v);
    return out;
}

bool Context::has(const char* name) const { return field(data, name) != nullptr; }

std::string Context::str(const char* name, const std::string& fallback) const {
    const VMValue* v = field(data, name);
    return v ? v->toString() : fallback;
}

float Context::f32(const char* name, float fallback) const {
    const VMValue* v = field(data, name);
    return v ? (float)std::atof(v->toString().c_str()) : fallback;
}

int32_t Context::s32(const char* name, int32_t fallback) const {
    const VMValue* v = field(data, name);
    return v ? (int32_t)std::atoi(v->toString().c_str()) : fallback;
}

bool Context::boolean(const char* name, bool fallback) const {
    const VMValue* v = field(data, name);
    if (!v) return fallback;
    const std::string text = v->toString();
    return strcasecmp(text.c_str(), "true") == 0 || std::atof(text.c_str()) != 0.0;
}

std::array<float, 4> Context::colorF(const char* name, std::array<float, 4> fallback) const {
    const VMValue* v = field(data, name);
    if (!v) return fallback;
    const auto f = floats(v->toString());
    std::array<float, 4> out{0, 0, 0, 1};
    for (size_t i = 0; i < f.size() && i < 4; ++i) out[i] = f[i];
    return out;
}

std::array<float, 3> Context::point(const char* name, std::array<float, 3> fallback) const {
    const VMValue* v = field(data, name);
    if (!v) return fallback;
    const auto f = floats(v->toString());
    std::array<float, 3> out{0, 0, 0};
    for (size_t i = 0; i < f.size() && i < 3; ++i) out[i] = f[i];
    return out;
}

int Context::enumValue(const char* name, std::initializer_list<const char*> names, int fallback) const {
    const VMValue* v = field(data, name);
    if (!v) return fallback;
    const std::string text = v->toString();
    int i = 0;
    for (const char* candidate : names) {
        if (strcasecmp(text.c_str(), candidate) == 0) return i;
        ++i;
    }
    return fallback;
}

uint32_t Context::ref(const char* name) const {
    const VMValue* v = field(data, name);
    if (!v || !resolver) return 0;
    const std::string text = v->toString();
    return text.empty() ? 0 : resolver(text);
}

void Context::writeRef(uint32_t id) {
    if (w.writeFlag(id >= ObjectIdFirst && id <= ObjectIdLast)) w.writeRangedU32(id, ObjectIdFirst, ObjectIdLast);
}

void Context::writeColorI(const std::array<float, 4>& c) {
    for (float v : c) w.writeInt((int32_t)std::clamp((int)(v * 255.0f + 0.5f), 0, 255), 8);
}

static std::map<std::string, PackFn>& table() {
    static std::map<std::string, PackFn> t;
    return t;
}

void registerClass(const std::string& className, PackFn fn) { table()[className] = std::move(fn); }

const PackFn* find(const std::string& className) {
    auto it = table().find(className);
    return it == table().end() ? nullptr : &it->second;
}

int classIndex(const std::string& className) {
    for (size_t i = 0; i < V12::DataBlockClassCount; ++i)
        if (V12::DataBlockClassNames[i] == className) return (int)i;
    return -1;
}

void registerAll() {
    static bool done = false;
    if (done) return;
    done = true;
    registerShapes();
    registerProjectiles();
    registerEffects();
    registerMisc();
}

} // namespace DataBlockPack
