#pragma once
// Datablock writer round trips: pack a datablock from fields, read it back
// with Torch's reader (as the local client does, string buffer on, classes
// offset by DataBlockClassFirst = 128) and compare bits consumed/written.
#include "sim/datablock_pack.h"
#include "net/v12_bitstream.h"
#include "net/v12_datablocks.h"
#include "script/script_engine.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

struct PackRoundTrip {
    size_t writtenBits = 0;
    size_t consumedBits = 0;
    bool readOk = false;
    V12::DecodedDataBlock decoded;
    bool exact() const { return readOk && writtenBits == consumedBits; }
};

inline PackRoundTrip packRoundTrip(const std::string& className,
                                   const std::map<std::string, std::string>& fields,
                                   const std::map<std::string, uint32_t>& refs = {}) {
    ScriptObject object;
    object.className = className;
    for (const auto& [key, value] : fields) object.fields[key] = VMValue(value);
    TorqueBitWriter w;
    w.setStringBuffer(true);
    DataBlockPack::Context context(&object, w, [&](const std::string& name) -> uint32_t {
        auto it = refs.find(name);
        return it == refs.end() ? 0u : it->second;
    });
    const DataBlockPack::PackFn* pack = DataBlockPack::find(className);
    if (!pack) { fprintf(stderr, "no writer for %s\n", className.c_str()); assert(pack); }
    (*pack)(context);
    PackRoundTrip result;
    result.writtenBits = w.bitPosition();
    const auto& bytes = w.data();
    // A class that writes no bits still reads from a (zero-length) buffer.
    static const uint8_t none = 0;
    V12BitStream stream(bytes.empty() ? &none : bytes.data(), bytes.size());
    stream.setStringBuffer(true);
    const int index = DataBlockPack::classIndex(className);
    assert(index >= 0);
    result.readOk = V12::readDataBlockPayload(stream, (size_t)index + 128, &result.decoded) && !stream.failed();
    result.consumedBits = stream.position();
    if (!result.exact())
        fprintf(stderr, "%s: wrote %zu bits, reader consumed %zu (ok=%d)\n", className.c_str(),
                result.writtenBits, result.consumedBits, (int)result.readOk);
    return result;
}
