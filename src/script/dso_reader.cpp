#include "script/dso_reader.h"
#include "core/console.h"
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>

DSOReader::DSOReader() {}

uint32_t DSOReader::readU32(const uint8_t*& ptr, size_t& remaining) {
    if (remaining < 4) return 0;
    uint32_t v; memcpy(&v, ptr, 4);
    ptr += 4; remaining -= 4;
    return v;
}

double DSOReader::readF64(const uint8_t*& ptr, size_t& remaining) {
    if (remaining < 8) return 0;
    double v; memcpy(&v, ptr, 8);
    ptr += 8; remaining -= 8;
    return v;
}

std::string DSOReader::readString(const uint8_t*& ptr, size_t& remaining, uint32_t maxLen) {
    if (remaining == 0) return "";
    uint32_t len = 0;
    const uint8_t* start = ptr;
    while (ptr < start + remaining && *ptr) { ptr++; len++; if (maxLen && len >= maxLen) break; }
    std::string s((const char*)start, len);
    // Advance past the trailing NUL without underflowing `remaining`.
    if (len + 1 <= remaining) { ptr++; remaining -= (len + 1); }
    else { ptr += remaining; remaining = 0; }
    return s;
}

uint32_t DSOReader::readOpcodeSlot(const uint8_t*& ptr, size_t& remaining) {
    if (remaining < 1) return 0xFF; // HALT
    uint32_t op = *ptr++;
    remaining--;
    if (op == 0xFF) {
        // Extended opcode: read u32
        if (remaining < 4) return 0xFF;
        uint32_t ext; memcpy(&ext, ptr, 4); op = ext;
        ptr += 4; remaining -= 4;
    }
    return op;
}

bool DSOReader::readStringTable(const uint8_t*& ptr, size_t& remaining, std::vector<char>& out) {
    uint32_t totalLen = readU32(ptr, remaining);
    if (remaining < totalLen) {
        Console::instance().printf(LogLevel::Warn, "DSO: string table truncated (need %u, have %zu)", totalLen, remaining);
        return false;
    }
    out.assign((const char*)ptr, (const char*)ptr + totalLen);
    ptr += totalLen; remaining -= totalLen;
    return true;
}

bool DSOReader::readFloatTable(const uint8_t*& ptr, size_t& remaining, std::vector<double>& out) {
    uint32_t count = readU32(ptr, remaining);
    size_t cap = count; if (cap > (1u << 20)) cap = (1u << 20);
    out.reserve(cap);
    for (uint32_t i = 0; i < count; i++) {
        if (remaining < 8) break; // truncated; stop consuming
        out.push_back(readF64(ptr, remaining));
    }
    return true;
}

bool DSOReader::readCodeStream(const uint8_t*& ptr, size_t& remaining, uint32_t& outCodeSize, uint32_t& outLineBreakCount, DSOFile& out) {
    outCodeSize = readU32(ptr, remaining);
    outLineBreakCount = readU32(ptr, remaining);
    out.codeSize = outCodeSize;
    // Reserve at most a sane cap; the decode loop below is bounded by `remaining`.
    size_t cap = outCodeSize; if (cap > (1u << 26)) cap = (1u << 26);
    out.code.reserve(cap);

    for (uint32_t i = 0; i < outCodeSize && remaining > 0; i++) {
        uint8_t op = *ptr++; remaining--;
        out.code.push_back(op);
        if (op == 0xFF) {
            // Extended opcode: read u32 little-endian, bounded by remaining.
            if (remaining >= 4) {
                uint32_t ext = 0;
                for (int j = 0; j < 4; j++) { ext |= ((uint32_t)*ptr++) << (8 * j); remaining--; }
                out.code.push_back((uint8_t)(ext & 0xFF));
                out.code.push_back((uint8_t)((ext >> 8) & 0xFF));
                out.code.push_back((uint8_t)((ext >> 16) & 0xFF));
                out.code.push_back((uint8_t)((ext >> 24) & 0xFF));
            } else {
                for (int j = 0; j < 4; j++) out.code.push_back(0);
                break; // not enough data
            }
        }
    }

    // Read line break pairs
    for (uint32_t i = 0; i < outLineBreakCount; i++) {
        if (remaining < 8) break;
        uint32_t line = readU32(ptr, remaining);
        uint32_t ip = readU32(ptr, remaining);
        out.lineBreaks.emplace_back(line, ip);
    }
    return true;
}

bool DSOReader::readIdentTable(const uint8_t*& ptr, size_t& remaining, DSOFile& out) {
    uint32_t count = readU32(ptr, remaining);
    for (uint32_t i = 0; i < count; i++) {
        if (remaining < 4) break; // truncated; stop consuming
        uint32_t strIdx = readU32(ptr, remaining);
        if (remaining < 4) break;
        uint32_t posCount = readU32(ptr, remaining);
        for (uint32_t j = 0; j < posCount; j++) {
            if (remaining < 4) break;
            uint32_t ip = readU32(ptr, remaining);
            out.identTable[ip] = strIdx;
        }
    }
    return true;
}

const char* DSOReader::globalString(DSOFile& file, uint32_t index) {
    if (index < file.globalStrings.size())
        return &file.globalStrings[index];
    return "";
}

const char* DSOReader::functionString(DSOFile& file, uint32_t index) {
    if (index < file.functionStrings.size())
        return &file.functionStrings[index];
    return "";
}

bool DSOReader::read(const uint8_t* data, size_t size, DSOFile& out) {
    const uint8_t* ptr = data;
    size_t remaining = size;

    // Version (u32 LE)
    out.version = readU32(ptr, remaining);
    Console::instance().printf(LogLevel::Debug, "DSO version: %u", out.version);

    // Global string table
    if (!readStringTable(ptr, remaining, out.globalStrings)) return false;

    // Global float table
    if (!readFloatTable(ptr, remaining, out.globalFloats)) return false;

    // Function string table
    if (!readStringTable(ptr, remaining, out.functionStrings)) return false;

    // Function float table
    if (!readFloatTable(ptr, remaining, out.functionFloats)) return false;

    // Code stream (includes code size, line break count, code slots, and line break pairs)
    uint32_t codeSize = 0, lineBreakCount = 0;
    if (!readCodeStream(ptr, remaining, codeSize, lineBreakCount, out)) return false;

    // Identifier table
    if (!readIdentTable(ptr, remaining, out)) return false;

    Console::instance().printf(LogLevel::Info, "DSO: v%u, %zu global strings, %zu global floats, %zu func strings, %zu func floats, %u code slots, %zu line breaks, %zu idents",
        out.version,
        out.globalStrings.size(), out.globalFloats.size(),
        out.functionStrings.size(), out.functionFloats.size(),
        out.codeSize, out.lineBreaks.size(), out.identTable.size());

    return true;
}

bool DSOReader::readFromFile(const char* path, DSOFile& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    return read(data.data(), data.size(), out);
}

void DSOReader::dumpInfo(DSOFile& file) {
    Console::instance().printf(LogLevel::Info, "=== DSO: %u functions ===", file.functions.size());
    for (auto& fn : file.functions) {
        Console::instance().printf(LogLevel::Info, "  %s(%u args, IP %u-%u)%s",
            fn.name.c_str(), fn.argc, fn.startIp, fn.endIp,
            fn.hasVarArgs ? " ..." : "");
    }
}
