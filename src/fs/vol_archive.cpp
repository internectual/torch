#include "fs/vol_archive.h"
#include "core/console.h"
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <algorithm>

struct VolEntry {
    uint32_t crc;
    uint32_t offset;
    uint32_t size;
    char name[128];
};

struct VolArchive::Impl {
    std::ifstream file;
    std::unordered_map<std::string, VolEntry> entries;
    std::string path;
};

VolArchive::VolArchive() : impl(new Impl) {}
VolArchive::~VolArchive() { if (impl->file.is_open()) impl->file.close(); delete impl; }

bool VolArchive::open(const char* path) {
    impl->file.open(path, std::ios::binary);
    if (!impl->file) {
        Console::instance().printf(LogLevel::Warn, "Cannot open VOL: %s", path);
        return false;
    }

    impl->path = path;
    char sig[4];
    impl->file.read(sig, 4);
    if (memcmp(sig, "VOL\0", 4) != 0) {
        Console::instance().printf(LogLevel::Warn, "Bad VOL signature: %s", path);
        return false;
    }

    uint32_t version = 0, numEntries = 0;
    impl->file.read((char*)&version, 4);
    impl->file.read((char*)&numEntries, 4);
    // Bound the entry count to avoid runaway loops on malformed/truncated files.
    if (numEntries > (1u << 20)) numEntries = (1u << 20);

    for (uint32_t i = 0; i < numEntries; i++) {
        VolEntry ent{};
        impl->file.read((char*)&ent.crc, 4);
        impl->file.read((char*)&ent.offset, 4);
        impl->file.read((char*)&ent.size, 4);
        uint8_t nameLen = 0;
        impl->file.read((char*)&nameLen, 1);
        if (!impl->file) break;
        // name[] is only 128 bytes; clamp to avoid overflow.
        if (nameLen >= sizeof(ent.name)) nameLen = (uint8_t)(sizeof(ent.name) - 1);
        impl->file.read(ent.name, nameLen);
        if (!impl->file) break;
        ent.name[nameLen] = 0;

        // Normalize path separators
        for (char* p = ent.name; *p; p++)
            if (*p == '\\') *p = '/';

        impl->entries[ent.name] = ent;
    }

    Console::instance().printf(LogLevel::Info, "VOL: %s - %zu entries", path, impl->entries.size());
    return true;
}

bool VolArchive::readFile(const char* path, std::vector<uint8_t>& data) {
    auto it = impl->entries.find(path);
    if (it == impl->entries.end()) return false;

    impl->file.seekg(it->second.offset);
    data.resize(it->second.size);
    impl->file.read((char*)data.data(), data.size());
    return true;
}

bool VolArchive::fileExists(const char* path) const {
    return impl->entries.find(path) != impl->entries.end();
}

void VolArchive::listFiles(const char* pattern, std::vector<std::string>& out) const {
    for (auto& [name, ent] : impl->entries) {
        if (!pattern || name.find(pattern) != std::string::npos)
            out.push_back(name);
    }
}
