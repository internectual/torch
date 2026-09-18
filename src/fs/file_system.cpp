#include "fs/file_system.h"
#include "fs/asset_policy.h"
#include "fs/path_policy.h"
#include "core/console.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <fnmatch.h>
#include <strings.h>
#include <chrono>
#include <set>

namespace fs = std::filesystem;

struct FileSystem::Impl {
    std::vector<Archive*> archives;
    std::vector<std::string> searchPaths;
};

// Resolve every path component, not just the leaf. Extracted T2 trees retain
// the archive's case, while Linux lookups are case-sensitive.
static bool resolveExtractedPath(const std::string& root, const char* logical,
                                 std::string& resolved) {
    fs::path current(root);
    for (const auto& component : fs::path(logical)) {
        const std::string wanted = component.string();
        fs::path exact = current / wanted;
        std::error_code error;
        if (fs::exists(exact, error)) {
            current = exact;
            continue;
        }
        std::vector<std::string> matches;
        for (fs::directory_iterator it(current, error), end; !error && it != end; it.increment(error)) {
            const std::string candidate = it->path().filename().string();
            if (candidate.size() == wanted.size() &&
                strcasecmp(candidate.c_str(), wanted.c_str()) == 0)
                matches.push_back(candidate);
        }
        if (matches.empty()) return false;
        std::sort(matches.begin(), matches.end());
        current /= matches.front();
    }
    resolved = current.string();
    return true;
}

static std::vector<std::string> directoryBundles(const std::string& root) {
    std::vector<std::string> bundles;
    const fs::path container = fs::path(root) / "@vl2";
    std::error_code error;
    if (!fs::is_directory(container, error)) return bundles;
    for (fs::recursive_directory_iterator it(container, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_directory(error)) {
            const auto name = it->path().filename().string();
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".vl2") == 0) {
                bundles.push_back(it->path().string());
                it.disable_recursion_pending();
            }
        }
    }
    std::sort(bundles.begin(), bundles.end());
    return bundles;
}

static bool isDirectoryBundlePath(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root).generic_string();
    return relative == "@vl2" || relative.starts_with("@vl2/");
}

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
    data.clear();
    if (!TorchPath::isSafeLogicalPath(path)) {
        Console::instance().printf(LogLevel::Warn, "Unsafe asset path rejected: %s", path ? path : "<null>");
        return false;
    }
    if (originalOnly && !TorchAssets::isOriginalRuntimePath(path)) {
        Console::instance().printf(LogLevel::Error,
            "Asset rejected by original-only policy: %s", path);
        return false;
    }
    if (path[0] == '/') {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            data.resize(f.tellg());
            f.seekg(0);
            f.read((char*)data.data(), data.size());
            return true;
        }
    }
    // Loose files are the authoritative extracted form. This also lets a
    // repaired native asset override the copy in a VL2 without conversions.
    for (auto& p : impl->searchPaths) {
        std::string full = p + "/" + path;
        if (!TorchPath::staysWithinRoot(p.c_str(), full.c_str())) continue;
        std::ifstream f(full, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            data.resize(f.tellg());
            f.seekg(0);
            f.read((char*)data.data(), data.size());
            return true;
        }
    }

    // Case-insensitive fallback: T2 content mixes naming conventions and
    // Linux is case-sensitive. Resolve intermediate directories too.
    for (auto& p : impl->searchPaths) {
        std::string real;
        if (resolveExtractedPath(p, path, real) &&
            TorchPath::staysWithinRoot(p.c_str(), real.c_str())) {
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
    // Extracted VL2 bundles live below @vl2 but retain their archive-relative
    // paths, so resolve them without exposing the extraction prefix.
    for (auto root = impl->searchPaths.rbegin(); root != impl->searchPaths.rend(); ++root) {
        auto bundles = directoryBundles(*root);
        for (auto bundle = bundles.rbegin(); bundle != bundles.rend(); ++bundle) {
            std::string real;
            if (!resolveExtractedPath(*bundle, path, real) ||
                !TorchPath::staysWithinRoot(bundle->c_str(), real.c_str())) continue;
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
    // Later mounts override earlier archives, matching the game's patch
    // layering while keeping extracted content higher priority than VL2.
    for (auto it = impl->archives.rbegin(); it != impl->archives.rend(); ++it)
        if ((*it)->readFile(path, data)) return true;
    // Torque's standard texture loader probes these suffixes for a resource
    // name without one. Do not probe arbitrary extensions: extensionless
    // script/resource names must retain their normal exact-file semantics.
    const std::string requested(path);
    const size_t slash = requested.find_last_of('/');
    const size_t dot = requested.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        static constexpr const char* textureExtensions[] = {
            ".jpg", ".png", ".gif", ".bmp", ".bm8", ".jpeg", ".tga", ".dds"};
        for (const char* extension : textureExtensions) {
            const std::string candidate = requested + extension;
            if (readFile(candidate.c_str(), data)) return true;
        }
    }
    return false;
}

bool FileSystem::readTextureFile(const char* path, std::vector<uint8_t>& data,
                                 std::string* resolvedPath) {
    data.clear();
    if (!TorchPath::isSafeLogicalPath(path) ||
        (originalOnly && !TorchAssets::isOriginalRuntimePath(path))) return false;
    if (readFile(path, data)) {
        if (resolvedPath) {
            *resolvedPath = path;
            const size_t slash = std::string(path).find_last_of('/');
            const size_t dot = std::string(path).find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
                static constexpr const char* textureExtensions[] = {
                    ".jpg", ".png", ".gif", ".bmp", ".bm8", ".jpeg", ".tga", ".dds"};
                for (const char* extension : textureExtensions) {
                    std::vector<uint8_t> candidateData;
                    const std::string candidate = std::string(path) + extension;
                    if (readFile(candidate.c_str(), candidateData) && candidateData == data) {
                        *resolvedPath = candidate;
                        break;
                    }
                }
            }
        }
        return true;
    }
    const std::string requested(path);
    const size_t slash = requested.find_last_of('/');
    const size_t dot = requested.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return false;
    static constexpr const char* textureExtensions[] = {
        ".jpg", ".png", ".gif", ".bmp", ".bm8", ".jpeg", ".tga", ".dds"};
    for (const char* extension : textureExtensions) {
        const std::string candidate = requested + extension;
        if (readFile(candidate.c_str(), data)) {
            if (resolvedPath) *resolvedPath = candidate;
            return true;
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
    if (!TorchPath::isSafeLogicalPath(path)) return false;
    if (originalOnly && !TorchAssets::isOriginalRuntimePath(path)) return false;
    for (auto& p : impl->searchPaths)
        if (TorchPath::staysWithinRoot(p.c_str(), (p + "/" + path).c_str()) &&
            fs::is_regular_file(p + "/" + path)) return true;
    for (auto& p : impl->searchPaths) {
        std::string real;
        if (resolveExtractedPath(p, path, real) &&
            TorchPath::staysWithinRoot(p.c_str(), real.c_str()) && fs::is_regular_file(real))
            return true;
    }
    for (auto root = impl->searchPaths.rbegin(); root != impl->searchPaths.rend(); ++root) {
        auto bundles = directoryBundles(*root);
        for (auto bundle = bundles.rbegin(); bundle != bundles.rend(); ++bundle) {
            std::string real;
            if (resolveExtractedPath(*bundle, path, real) &&
                TorchPath::staysWithinRoot(bundle->c_str(), real.c_str()) &&
                fs::is_regular_file(real)) return true;
        }
    }
    for (auto it = impl->archives.rbegin(); it != impl->archives.rend(); ++it)
        if ((*it)->fileExists(path)) return true;
    const std::string requested(path);
    const size_t slash = requested.find_last_of('/');
    const size_t dot = requested.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        static constexpr const char* textureExtensions[] = {
            ".jpg", ".png", ".gif", ".bmp", ".bm8", ".jpeg", ".tga", ".dds"};
        for (const char* extension : textureExtensions)
            if (fileExists((requested + extension).c_str())) return true;
    }
    return false;
}

int64_t FileSystem::fileModifyTime(const char* path) const {
    if (!TorchPath::isSafeLogicalPath(path) ||
        (originalOnly && !TorchAssets::isOriginalRuntimePath(path))) return 0;
    for (const auto& root : impl->searchPaths) {
        std::string resolved;
        if (!resolveExtractedPath(root, path, resolved)) continue;
        const auto file = fs::path(resolved);
        if (!TorchPath::staysWithinRoot(root.c_str(), file.c_str()) || !fs::is_regular_file(file)) continue;
        std::error_code error;
        const auto stamp = fs::last_write_time(file, error);
        if (error) continue;
        const auto systemStamp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            stamp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::duration_cast<std::chrono::seconds>(systemStamp.time_since_epoch()).count();
    }
    return 0;
}

void FileSystem::listFiles(const char* pattern, std::vector<std::string>& out) const {
    if (pattern && !TorchPath::isSafeLogicalPath(pattern)) return;
    std::string foldedPattern = pattern ? pattern : "";
    for (char& c : foldedPattern) c = (char)std::tolower((unsigned char)c);
    std::set<std::string> seen;
    auto add = [&](const std::string& name) {
        if (originalOnly && !TorchAssets::isOriginalRuntimePath(name)) return;
        std::string folded = name;
        for (char& c : folded) c = (char)std::tolower((unsigned char)c);
        if (seen.insert(folded).second) out.push_back(name);
    };
    for (auto& p : impl->searchPaths) {
        if (fs::exists(p)) {
            std::error_code error;
            for (fs::recursive_directory_iterator it(p, error), end;
                 !error && it != end; it.increment(error)) {
                const auto& e = *it;
                if (isDirectoryBundlePath(p, e.path())) {
                    if (e.is_directory(error)) it.disable_recursion_pending();
                    continue;
                }
                if (e.is_regular_file()) {
                    auto rp = e.path().string().substr(p.length() + 1);
                    // FNM_PATHNAME: '*' must not cross '/' — stock T2 findFirstFile() matches
                    // per path component ("textures/skins/*.lmale.png" is direct
                    // children only), and wildcard-crossing made overlapping scans
                    // (textures/* vs textures/skins/*) yield duplicates.
                    std::string foldedPath = rp;
                    for (char& c : foldedPath) c = (char)std::tolower((unsigned char)c);
                    if (!pattern || fnmatch(foldedPattern.c_str(), foldedPath.c_str(), FNM_PATHNAME) == 0)
                        add(rp);
                }
            }
        }
    }
    std::vector<std::string> bundleFiles;
    for (auto root = impl->searchPaths.rbegin(); root != impl->searchPaths.rend(); ++root) {
        for (const auto& bundle : directoryBundles(*root)) {
            bundleFiles.clear();
            std::error_code error;
            for (fs::recursive_directory_iterator it(bundle, error), end;
                 !error && it != end; it.increment(error)) {
                if (it->is_regular_file(error)) {
                    const auto name = it->path().lexically_relative(bundle).generic_string();
                    std::string foldedPath = name;
                    for (char& c : foldedPath) c = (char)std::tolower((unsigned char)c);
                    if (!pattern || fnmatch(foldedPattern.c_str(), foldedPath.c_str(), FNM_PATHNAME) == 0)
                        bundleFiles.push_back(name);
                }
            }
            std::sort(bundleFiles.begin(), bundleFiles.end());
            for (const auto& name : bundleFiles) add(name);
        }
    }
    std::vector<std::string> archiveFiles;
    for (auto it = impl->archives.rbegin(); it != impl->archives.rend(); ++it) {
        archiveFiles.clear();
        (*it)->listFiles(pattern, archiveFiles);
        for (const auto& name : archiveFiles) add(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        std::string af = a, bf = b;
        for (char& c : af) c = (char)std::tolower((unsigned char)c);
        for (char& c : bf) c = (char)std::tolower((unsigned char)c);
        return af == bf ? a < b : af < bf;
    });
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
