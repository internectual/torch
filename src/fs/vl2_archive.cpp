#include "fs/vl2_archive.h"
#include "core/console.h"
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <zlib.h>

struct Vl2Archive::Impl {
    std::ifstream file;
    std::string filePath;

    struct Entry {
        uint32_t offset;       // data offset in file
        uint32_t compressedSize;
        uint32_t uncompressedSize;
        uint16_t compression;  // 0=stored, 8=deflate
        uint16_t nameLen;
    };
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

    // Use ZIP central directory for fast indexing (skip local file headers)
    // Find the End of Central Directory record
    impl->file.seekg(0, std::ios::end);
    long fileSize = (long)impl->file.tellg();
    if (fileSize < 22) return false;

    // Search backward for EOCD signature (PK\5\6)
    long eocdPos = fileSize - 22;
    char sig[4];
    while (eocdPos >= 0) {
        impl->file.seekg(eocdPos);
        impl->file.read(sig, 4);
        if (memcmp(sig, "PK\5\6", 4) == 0) break;
        eocdPos--;
    }
    if (eocdPos < 0) return false;

    // Read EOCD
    impl->file.seekg(eocdPos + 8); // skip sig + disk info
    uint16_t diskNum, diskStart;
    uint16_t totalEntriesDisk, totalEntries;
    uint32_t centralDirSize, centralDirOffset;
    uint16_t commentLen;

    impl->file.read((char*)&diskNum, 2);
    impl->file.read((char*)&diskStart, 2);
    impl->file.read((char*)&totalEntriesDisk, 2);
    impl->file.read((char*)&totalEntries, 2);
    impl->file.read((char*)&centralDirSize, 4);
    impl->file.read((char*)&centralDirOffset, 4);
    impl->file.read((char*)&commentLen, 2);

    if (totalEntries == 0) return true;

    // Read central directory entries
    impl->file.seekg(centralDirOffset);
    for (uint16_t i = 0; i < totalEntries; i++) {
        impl->file.read(sig, 4);
        if (memcmp(sig, "PK\1\2", 4) != 0) break;

        uint16_t versionMade, versionNeeded, flags, compression, modTime, modDate;
        uint32_t crc, compressedSize, uncompressedSize;
        uint16_t nameLen, extraLen, commentLen, diskStartLocal;
        uint16_t internalAttrs;
        uint32_t externalAttrs;
        uint32_t localHeaderOffset;

        impl->file.read((char*)&versionMade, 2);
        impl->file.read((char*)&versionNeeded, 2);
        impl->file.read((char*)&flags, 2);
        impl->file.read((char*)&compression, 2);
        impl->file.read((char*)&modTime, 2);
        impl->file.read((char*)&modDate, 2);
        impl->file.read((char*)&crc, 4);
        impl->file.read((char*)&compressedSize, 4);
        impl->file.read((char*)&uncompressedSize, 4);
        impl->file.read((char*)&nameLen, 2);
        impl->file.read((char*)&extraLen, 2);
        impl->file.read((char*)&commentLen, 2);
        impl->file.read((char*)&diskStartLocal, 2);
        impl->file.read((char*)&internalAttrs, 2);
        impl->file.read((char*)&externalAttrs, 4);
        impl->file.read((char*)&localHeaderOffset, 4);

        std::string name(nameLen, '\0');
        impl->file.read(name.data(), nameLen);
        impl->file.seekg(extraLen, std::ios::cur);
        impl->file.seekg(commentLen, std::ios::cur);

        for (auto& c : name) if (c == '\\') c = '/';

        // Compute data offset from local file header
        uint32_t dataOffset = localHeaderOffset + 30 + nameLen + extraLen;

        Impl::Entry entry;
        entry.offset = dataOffset;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.compression = compression;
        entry.nameLen = nameLen;
        impl->entries[name] = entry;
    }

    Console::instance().printf(LogLevel::Info, "VL2: %s - %zu entries", path, impl->entries.size());
    return true;
}

bool Vl2Archive::readFile(const char* path, std::vector<uint8_t>& data) {
    auto it = impl->entries.find(path);
    if (it == impl->entries.end()) {
        for (auto& [name, entry] : impl->entries) {
            if (Impl::iequals(name, path)) {
                it = impl->entries.find(name);
                break;
            }
        }
        if (it == impl->entries.end()) return false;
    }

    const auto& entry = it->second;
    impl->file.seekg(entry.offset);
    if (entry.compression == 0) {
        data.resize(entry.uncompressedSize);
        impl->file.read((char*)data.data(), entry.uncompressedSize);
    } else if (entry.compression == 8) {
        std::vector<uint8_t> compressed(entry.compressedSize);
        impl->file.read((char*)compressed.data(), entry.compressedSize);
        data.resize(entry.uncompressedSize);
        z_stream strm{};
        inflateInit2(&strm, -MAX_WBITS);
        strm.next_in = compressed.data();
        strm.avail_in = entry.compressedSize;
        strm.next_out = data.data();
        strm.avail_out = entry.uncompressedSize;
        inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
    } else {
        return false;
    }

    return true;
}

bool Vl2Archive::fileExists(const char* path) const {
    if (impl->entries.find(path) != impl->entries.end()) return true;
    for (auto& [name, _] : impl->entries) {
        if (Impl::iequals(name, path)) return true;
    }
    return false;
}

void Vl2Archive::listFiles(const char* pattern, std::vector<std::string>& out) const {
    for (auto& [name, _] : impl->entries) {
        if (!pattern || name.find(pattern) != std::string::npos)
            out.push_back(name);
    }
}
