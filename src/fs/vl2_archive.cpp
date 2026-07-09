#include "fs/vl2_archive.h"
#include "core/console.h"
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <zlib.h>
#include <fnmatch.h>

struct Entry {
    uint32_t offset;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t compression;
};

struct Vl2Archive::Impl {
    std::ifstream file;
    std::string filePath;
    std::unordered_map<std::string, Entry> entries;

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    }
};

Vl2Archive::Vl2Archive() : impl(new Impl) {}
Vl2Archive::~Vl2Archive() { if (impl->file.is_open()) impl->file.close(); delete impl; }

bool Vl2Archive::open(const char* path) {
    impl->file.open(path, std::ios::binary);
    if (!impl->file) {
        Console::instance().printf(LogLevel::Warn, "Cannot open VL2: %s", path);
        return false;
    }
    impl->filePath = path;

    // Scan local file headers to build index (no decompression during mount)
    char sig[4];
    while (impl->file.read(sig, 4)) {
        if (memcmp(sig, "PK\3\4", 4) != 0) break;

        uint16_t version = 0, flags = 0, compression = 0, modTime = 0, modDate = 0;
        uint32_t crc = 0, compressedSize = 0, uncompressedSize = 0;
        uint16_t nameLen = 0, extraLen = 0;

        impl->file.read((char*)&version, 2);
        impl->file.read((char*)&flags, 2);
        impl->file.read((char*)&compression, 2);
        impl->file.read((char*)&modTime, 2);
        impl->file.read((char*)&modDate, 2);
        impl->file.read((char*)&crc, 4);
        impl->file.read((char*)&compressedSize, 4);
        impl->file.read((char*)&uncompressedSize, 4);
        impl->file.read((char*)&nameLen, 2);
        impl->file.read((char*)&extraLen, 2);

        std::string name(nameLen, '\0');
        impl->file.read(name.data(), nameLen);
        impl->file.seekg(extraLen, std::ios::cur);

        for (auto& c : name) if (c == '\\') c = '/';

        Entry entry;
        entry.offset = (uint32_t)impl->file.tellg();
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.compression = compression;
        impl->entries[name] = entry;

        // Skip data — read on demand
        if (compression == 0)
            impl->file.seekg(uncompressedSize, std::ios::cur);
        else if (compression == 8)
            impl->file.seekg(compressedSize, std::ios::cur);
    }

    Console::instance().printf(LogLevel::Info, "VL2: %s - %zu entries", path, impl->entries.size());
    return true;
}

bool Vl2Archive::readFile(const char* path, std::vector<uint8_t>& data) {
    auto it = impl->entries.find(path);
    if (it == impl->entries.end()) {
        for (auto& [name, e] : impl->entries)
            if (Impl::iequals(name, path)) { it = impl->entries.find(name); break; }
        if (it == impl->entries.end()) return false;
    }
    const auto& e = it->second;
    // Determine total file size to validate entry bounds.
    impl->file.clear();
    impl->file.seekg(0, std::ios::end);
    std::streamoff fileSize = impl->file.tellg();
    impl->file.seekg(e.offset, std::ios::beg);

    const size_t MAX_UNCOMP = 1u << 28; // 256 MB sanity cap
    if (e.uncompressedSize > MAX_UNCOMP) return false;

    if (e.compression == 0) {
        if ((std::streamoff)e.offset + (std::streamoff)e.uncompressedSize > fileSize) return false;
        data.resize(e.uncompressedSize);
        impl->file.read((char*)data.data(), e.uncompressedSize);
        if (!impl->file) return false;
    } else if (e.compression == 8) {
        if (e.compressedSize > MAX_UNCOMP) return false;
        if ((std::streamoff)e.offset + (std::streamoff)e.compressedSize > fileSize) return false;
        std::vector<uint8_t> compressed(e.compressedSize);
        impl->file.read((char*)compressed.data(), e.compressedSize);
        if (!impl->file) return false;
        data.resize(e.uncompressedSize);
        z_stream strm{};
        inflateInit2(&strm, -MAX_WBITS);
        strm.next_in = compressed.data();
        strm.avail_in = e.compressedSize;
        strm.next_out = data.data();
        strm.avail_out = e.uncompressedSize;
        int zr = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (zr != Z_STREAM_END) return false;
    } else {
        return false;
    }
    return true;
}

bool Vl2Archive::fileExists(const char* path) const {
    if (impl->entries.find(path) != impl->entries.end()) return true;
    for (auto& [name, _] : impl->entries)
        if (Impl::iequals(name, path)) return true;
    return false;
}

void Vl2Archive::listFiles(const char* pattern, std::vector<std::string>& out) const {
    for (auto& [name, _] : impl->entries)
        if (!pattern || fnmatch(pattern, name.c_str(), 0) == 0)
            out.push_back(name);
}
