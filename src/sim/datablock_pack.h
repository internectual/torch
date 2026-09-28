#pragma once
// SimDataBlock::packData for every retail datablock class, written in the
// form the Tribes 2 client reads (Torch's reader: src/net/v12_datablocks.cpp;
// field names and conditions: the engine's packData/initPersistFields and the
// reference parser's DataBlockParsers.js). Values come from the datablock
// script object's persist fields with the engine's constructor defaults.
#include "net/torque_bit_writer.h"
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

struct ScriptObject;

namespace DataBlockPack {

// Tribes 2 datablock ids (11-bit): 3..2050.
constexpr uint32_t ObjectIdFirst = 3;
constexpr uint32_t ObjectIdLast = 2050;

// Resolves a datablock name or id to its object id (0 when not a datablock).
using Resolver = std::function<uint32_t(const std::string&)>;

class Context {
public:
    Context(const ScriptObject* data, TorqueBitWriter& w, Resolver resolver)
        : data(data), w(w), resolver(std::move(resolver)) {}
    const ScriptObject* data;
    TorqueBitWriter& w;

    // Persist fields, parsed like the engine's console types; `fallback` is
    // the engine constructor's default when the field is not set.
    bool has(const char* field) const;
    std::string str(const char* field, const std::string& fallback = {}) const;
    float f32(const char* field, float fallback) const;
    int32_t s32(const char* field, int32_t fallback) const;
    bool boolean(const char* field, bool fallback) const;   // "true" or nonzero
    std::array<float, 4> colorF(const char* field, std::array<float, 4> fallback) const;
    std::array<float, 3> point(const char* field, std::array<float, 3> fallback) const;
    // Enum field: index of the value among `names` (case-insensitive).
    int enumValue(const char* field, std::initializer_list<const char*> names, int fallback) const;
    // A datablock-typed field: the referenced datablock's id, 0 when unset.
    uint32_t ref(const char* field) const;

    // if (writeFlag(id)) writeRangedU32(id, DataBlockObjectIdFirst, DataBlockObjectIdLast)
    void writeRef(uint32_t id);
    // Stream::write(ColorF) sends a ColorI: four bytes r, g, b, a (U32 LE).
    void writeColorI(const std::array<float, 4>& color);
    // Stream::write(bool) is a byte.
    void writeBoolByte(bool value) { w.writeInt(value ? 1 : 0, 8); }

private:
    Resolver resolver;
};

using PackFn = std::function<void(Context&)>;
void registerClass(const std::string& className, PackFn fn);
const PackFn* find(const std::string& className);
// Index of a class in V12::DataBlockClassNames (-1 when not a retail class).
int classIndex(const std::string& className);

// Each group of classes registers itself (src/sim/datablocks/*.cpp).
void registerShapes();      // ShapeBaseData family, images, players, vehicles, items, turrets
void registerProjectiles(); // ProjectileData family
void registerEffects();     // explosions, debris, splashes, shockwaves, particles, decals
void registerMisc();        // audio, camera, sensor, trigger, force fields, markers, ...
void registerAll();

} // namespace DataBlockPack
