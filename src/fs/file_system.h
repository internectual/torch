#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct FileEntry {
    std::string path;
    uint32_t offset;
    uint32_t size;
    uint32_t compressedSize;
    bool compressed;
};

class Archive {
public:
    virtual ~Archive() = default;
    virtual bool open(const char* path) = 0;
    virtual bool readFile(const char* path, std::vector<uint8_t>& data) = 0;
    virtual bool fileExists(const char* path) const = 0;
    virtual void listFiles(const char* pattern, std::vector<std::string>& out) const = 0;
};

class FileSystem {
public:
    FileSystem();
    ~FileSystem();

    bool init(const std::vector<std::string>& dataPaths);
    void shutdown();

    // Production always uses the stock Tribes 2 resource set. Diagnostic
    // tools may disable this only when they intentionally inspect a format.
    void setOriginalOnly(bool enabled) { originalOnly = enabled; }
    bool isOriginalOnly() const { return originalOnly; }

    void addArchive(Archive* archive);
    void addPath(const char* path);

    bool readFile(const char* path, std::vector<uint8_t>& data);
    bool readTextFile(const char* path, std::string& text);
    bool fileExists(const char* path) const;
    void listFiles(const char* pattern, std::vector<std::string>& out) const;

    // High-level asset loading
    std::vector<uint8_t> read(const char* path);
    std::string readText(const char* path);

private:
    struct Impl;
    Impl* impl;
    bool originalOnly = true;
};
