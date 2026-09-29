#include "sim/net_object.h"
#include "sim/torque_math.h"
#include <cstdio>
#include "net/v12_registry.h"
#include "script/script_engine.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <strings.h>

int NetObject::netClassId() const {
    const char* name = netClassName();
    if (!name) return -1;
    for (size_t i = 0; i < V12::GhostClassCount; ++i)
        if (V12::GhostClassNames[i] == name) return (int)i;
    return -1;
}

void NetObject::setMaskBits(uint32_t mask) { pendingMask |= mask; }

namespace Fields {

static const VMValue* find(const ScriptObject* object, const char* name) {
    if (!object) return nullptr;
    for (const auto& [field, value] : object->fields)
        if (strcasecmp(field.c_str(), name) == 0) return &value;
    return nullptr;
}

bool present(const ScriptObject* object, const char* name) { return find(object, name) != nullptr; }

std::string string(const ScriptObject* object, const char* name, const std::string& fallback) {
    const VMValue* value = find(object, name);
    return value ? value->toString() : fallback;
}

float f32(const ScriptObject* object, const char* name, float fallback) {
    const VMValue* value = find(object, name);
    return value ? (float)std::atof(value->toString().c_str()) : fallback;
}

int32_t s32(const ScriptObject* object, const char* name, int32_t fallback) {
    const VMValue* value = find(object, name);
    return value ? (int32_t)std::atoi(value->toString().c_str()) : fallback;
}

// consoleTypes.cc TypeBool: "true" or a nonzero number.
bool boolean(const ScriptObject* object, const char* name, bool fallback) {
    const VMValue* value = find(object, name);
    if (!value) return fallback;
    const std::string text = value->toString();
    return strcasecmp(text.c_str(), "true") == 0 || std::atof(text.c_str()) != 0.0;
}

static std::vector<float> floats(const std::string& text) {
    std::vector<float> out;
    std::istringstream in(text);
    float v;
    while (in >> v) out.push_back(v);
    return out;
}

std::array<float, 4> color(const ScriptObject* object, const char* name, std::array<float, 4> fallback) {
    const VMValue* value = find(object, name);
    if (!value) return fallback;
    const auto v = floats(value->toString());
    std::array<float, 4> out{0, 0, 0, 1};
    for (size_t i = 0; i < v.size() && i < 4; ++i) out[i] = v[i];
    return out;
}

std::array<float, 3> point(const ScriptObject* object, const char* name, std::array<float, 3> fallback) {
    const VMValue* value = find(object, name);
    if (!value) return fallback;
    const auto v = floats(value->toString());
    std::array<float, 3> out{0, 0, 0};
    for (size_t i = 0; i < v.size() && i < 3; ++i) out[i] = v[i];
    return out;
}

std::vector<int32_t> s32Vector(const ScriptObject* object, const char* name) {
    std::vector<int32_t> out;
    const VMValue* value = find(object, name);
    if (!value) return out;
    std::istringstream in(value->toString());
    int64_t v;
    while (in >> v) out.push_back((int32_t)v);
    return out;
}

} // namespace Fields

// SceneObject persist fields: position, rotation (TypeAngAxisF, degrees),
// scale. AngAxisF -> QuatF -> MatrixF (m_quatF_set_matF, row-major).
void SceneObject::readFields() {
    const auto position = Fields::point(script, "position", {0, 0, 0});
    std::array<float, 4> rotation{1, 0, 0, 0};
    if (Fields::present(script, "rotation")) {
        const auto v = Fields::color(script, "rotation", {1, 0, 0, 0});
        rotation = v;
    }
    float ax = rotation[0], ay = rotation[1], az = rotation[2];
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 0) { ax /= len; ay /= len; az /= len; } else { ax = 1; ay = 0; az = 0; }
    const float angle = rotation[3] * (float)M_PI / 180.0f;
    const float s = std::sin(angle * 0.5f);
    const float qx = ax * s, qy = ay * s, qz = az * s, qw = std::cos(angle * 0.5f);
    const float xx = qx * qx, yy = qy * qy, zz = qz * qz, xy = qx * qy, xz = qx * qz,
                yz = qy * qz, wx = qw * qx, wy = qw * qy, wz = qw * qz;
    transform = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), position[0],
                 2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), position[1],
                 2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), position[2],
                 0, 0, 0, 1};
    const auto sc = Fields::point(script, "scale", {1, 1, 1});
    scale[0] = sc[0]; scale[1] = sc[1]; scale[2] = sc[2];
}

void SceneObject::writeTransform(TorqueBitWriter& w) const {
    for (float v : transform) w.writeF32(v);
}

// BitStream::writeAffineTransform: position, then QuatF(matrix) normalized,
// x/y/z and the sign of w.
void SceneObject::writeAffineTransform(TorqueBitWriter& w) const {
    const auto& m = transform;
    w.writePoint({m[3], m[7], m[11]});
    // QuatF::set(MatrixF) on the transposed convention of m_quatF_set_matF.
    const float trace = m[0] + m[5] + m[10];
    float qx, qy, qz, qw;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (m[6] - m[9]) / s;
        qy = (m[8] - m[2]) / s;
        qz = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2.0f;
        qw = (m[6] - m[9]) / s;
        qx = 0.25f * s;
        qy = (m[4] + m[1]) / s;
        qz = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2.0f;
        qw = (m[8] - m[2]) / s;
        qx = (m[4] + m[1]) / s;
        qy = 0.25f * s;
        qz = (m[9] + m[6]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2.0f;
        qw = (m[1] - m[4]) / s;
        qx = (m[8] + m[2]) / s;
        qy = (m[9] + m[6]) / s;
        qz = 0.25f * s;
    }
    const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (n > 0) { qx /= n; qy /= n; qz /= n; qw /= n; }
    w.writeF32(qx);
    w.writeF32(qy);
    w.writeF32(qz);
    w.writeFlag(qw < 0.0f);
}

void SceneObject::writeScale(TorqueBitWriter& w) const {
    w.writePoint({scale[0], scale[1], scale[2]});
}

bool SceneObject::liveField(const std::string& name, std::string& out) const {
    char buffer[128];
    if (strcasecmp(name.c_str(), "position") == 0) {
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", transform[3], transform[7], transform[11]);
    } else if (strcasecmp(name.c_str(), "rotation") == 0) {
        // AngAxisF(mat), axis normalized, angle in degrees.
        const auto aa = TorqueMath::angAxis(TorqueMath::quat(transform));
        float x = aa.x, y = aa.y, z = aa.z;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 0) { x /= len; y /= len; z /= len; }
        std::snprintf(buffer, sizeof(buffer), "%g %g %g %g", x, y, z, aa.angle * 180.0f / (float)M_PI);
    } else if (strcasecmp(name.c_str(), "scale") == 0) {
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", scale[0], scale[1], scale[2]);
    } else {
        return false;
    }
    out = buffer;
    return true;
}
