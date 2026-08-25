#include "fs/file_system.h"
#include "core/console.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <fnmatch.h>
#include <dirent.h>
#include <strings.h>

namespace fs = std::filesystem;

struct FileSystem::Impl {
    std::vector<Archive*> archives;
    std::vector<std::string> searchPaths;
};

FileSystem::FileSystem() : impl(new Impl) {}
FileSystem::~FileSystem() { delete impl; }

bool FileSystem::init(const std::vector<std::string>& dataPaths) {
    for (auto& p : dataPaths) {
        if (fs::is_directory(p))
            impl->searchPaths.push_back(p);
    }
    Console::instance().printf(LogLevel::Info, "FileSystem: %zu paths, %zu archives",
        impl->searchPaths.size(), impl->archives.size());
    return true;
}

void FileSystem::shutdown() {
    for (auto a : impl->archives) delete a;
    impl->archives.clear();
}

void FileSystem::addArchive(Archive* archive) {
    impl->archives.push_back(archive);
}

void FileSystem::addPath(const char* path) {
    impl->searchPaths.push_back(path);
}

bool FileSystem::readFile(const char* path, std::vector<uint8_t>& data) {
    // Check archives
    for (auto a : impl->archives) {
        if (a->readFile(path, data)) return true;
    }

    // Check filesystem paths
    for (auto& p : impl->searchPaths) {
        std::string full = p + "/" + path;
        std::ifstream f(full, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            data.resize(f.tellg());
            f.seekg(0);
            f.read((char*)data.data(), data.size());
            return true;
        }
    }

    // Case-insensitive fallback: T2 content mixes naming conventions
    // (clientPrefs.cs vs ClientPrefs.cs) and Linux is case-sensitive.
    for (auto& p : impl->searchPaths) {
        std::string full = p + "/" + path;
        auto slash = full.rfind('/');
        if (slash == std::string::npos) continue;
        std::string dir = full.substr(0, slash);
        std::string base = full.substr(slash + 1);
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* e;
        std::string real;
        bool found = false;
        while ((e = readdir(d)) != nullptr) {
            if (strcasecmp(e->d_name, base.c_str()) == 0) { real = dir + "/" + e->d_name; found = true; break; }
        }
        closedir(d);
        if (found) {
            std::ifstream f(real, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                data.resize(f.tellg());
                f.seekg(0);
                f.read((char*)data.data(), data.size());
                return true;
            }
        }
    }
    return false;
}

bool FileSystem::readTextFile(const char* path, std::string& text) {
    std::vector<uint8_t> data;
    if (!readFile(path, data)) return false;
    text.assign((char*)data.data(), data.size());
    return true;
}

bool FileSystem::fileExists(const char* path) const {
    for (auto a : impl->archives)
        if (a->fileExists(path)) return true;
    for (auto& p : impl->searchPaths)
        if (fs::exists(p + "/" + path)) return true;
    return false;
}

void FileSystem::listFiles(const char* pattern, std::vector<std::string>& out) const {
    for (auto a : impl->archives) a->listFiles(pattern, out);
    for (auto& p : impl->searchPaths) {
        if (fs::exists(p)) {
            for (auto& e : fs::recursive_directory_iterator(p)) {
                if (e.is_regular_file()) {
                    auto rp = e.path().string().substr(p.length() + 1);
                    if (!pattern || fnmatch(pattern, rp.c_str(), 0) == 0)
                        out.push_back(rp);
                }
            }
        }
    }
}

std::vector<uint8_t> FileSystem::read(const char* path) {
    std::vector<uint8_t> data;
    readFile(path, data);
    return data;
}

std::string FileSystem::readText(const char* path) {
    std::string text;
    readTextFile(path, text);
    return text;
}
