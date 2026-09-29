#pragma once
// NetObject / SceneObject (sim/netObject.cc, sim/sceneObject.cc): ghosting
// flags, dirty masks and packUpdate; the object's transform and scale from
// its persist fields.
#include "sim/engine_object.h"
#include "net/torque_bit_writer.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

class GameConnection;

class NetObject : public EngineObject {
public:
    bool ghostable = true;
    bool scopeAlways = false;
    // Retail net class (V12::GhostClassNames) this object ghosts as.
    virtual const char* netClassName() const { return nullptr; }
    int netClassId() const;
    // Called once the script object's fields are set (onAdd).
    virtual void readFields() {}
    // Writes the state under `mask`; returns the bits still to send.
    virtual uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) { return 0; }
    // NetObject::setMaskBits: mark state dirty for every connection ghosting it.
    void setMaskBits(uint32_t mask);
    uint32_t pendingMask = 0; // or'ed into each ghost's update mask
};

class SceneObject : public NetObject {
public:
    std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // row-major
    float scale[3] = {1, 1, 1};
    void readFields() override;
    void writeTransform(TorqueBitWriter& w) const;       // mathWrite(MatrixF)
    void writeAffineTransform(TorqueBitWriter& w) const; // position, quaternion x/y/z, w sign
    void writeScale(TorqueBitWriter& w) const;
    // The persist fields that read the live transform (TypeMatrixPosition,
    // TypeMatrixRotation, scale); false for any other field.
    bool liveField(const std::string& name, std::string& out) const;
};

// Typed reads of a script object's persist fields (console type parsing).
namespace Fields {
std::string string(const class ScriptObject* object, const char* name, const std::string& fallback = {});
float f32(const ScriptObject* object, const char* name, float fallback);
int32_t s32(const ScriptObject* object, const char* name, int32_t fallback);
bool boolean(const ScriptObject* object, const char* name, bool fallback);
std::array<float, 4> color(const ScriptObject* object, const char* name, std::array<float, 4> fallback);
std::array<float, 3> point(const ScriptObject* object, const char* name, std::array<float, 3> fallback);
std::vector<int32_t> s32Vector(const ScriptObject* object, const char* name);
bool present(const ScriptObject* object, const char* name);
} // namespace Fields

void registerSceneObjectClasses();
