#include "script/dso_reader.h"

#include <cassert>
#include <cstdint>
#include <vector>

static void functionStorageStaysStable() {
    DSOFile file;
    file.functions.push_back(DSOFunction{"first"});
    DSOFunction* first = &file.functions.front();
    for (int i = 0; i < 256; ++i)
        file.functions.push_back(DSOFunction{"later" + std::to_string(i)});
    assert(first == &file.functions.front());
}

static void put32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back((uint8_t)value);
    data.push_back((uint8_t)(value >> 8));
    data.push_back((uint8_t)(value >> 16));
    data.push_back((uint8_t)(value >> 24));
}

static std::vector<uint8_t> minimalDso() {
    std::vector<uint8_t> data;
    put32(data, 1); // version
    put32(data, 0); // global strings
    put32(data, 0); // global floats
    put32(data, 0); // function strings
    put32(data, 0); // function floats
    put32(data, 0); // code slots
    put32(data, 0); // line breaks
    put32(data, 0); // identifiers
    return data;
}

int main() {
    functionStorageStaysStable();
    DSOReader reader;
    DSOFile file;
    auto valid = minimalDso();
    assert(reader.read(valid.data(), valid.size(), file));

    auto trailing = valid;
    trailing.push_back(0xff);
    assert(!reader.read(trailing.data(), trailing.size(), file));

    std::vector<uint8_t> truncatedFloats;
    put32(truncatedFloats, 1);
    put32(truncatedFloats, 0);
    put32(truncatedFloats, 1); // one global float, but no eight-byte payload
    assert(!reader.read(truncatedFloats.data(), truncatedFloats.size(), file));

    auto truncatedCode = minimalDso();
    truncatedCode.resize(24);
    truncatedCode[20] = 1; // one code slot, but no opcode
    assert(!reader.read(truncatedCode.data(), truncatedCode.size(), file));

    auto oversizedCode = minimalDso();
    oversizedCode[20] = 0;
    oversizedCode[21] = 0;
    oversizedCode[22] = 0;
    oversizedCode[23] = 4; // 64M code slots, with only the trailing header
    assert(!reader.read(oversizedCode.data(), oversizedCode.size(), file));

    auto truncatedIdentifiers = minimalDso();
    truncatedIdentifiers[28] = 1; // one identifier, but no pair
    truncatedIdentifiers.resize(32);
    assert(!reader.read(truncatedIdentifiers.data(), truncatedIdentifiers.size(), file));

    assert(!reader.read(nullptr, 0, file));
    return 0;
}
