#include "fs/zip_archive.h"

#include "core/console.h"
#include "fs/path_policy.h"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <memory>
#include <unordered_map>
#include <zip.h>

namespace {
constexpr zip_uint64_t MaxZipEntrySize = 1ull << 30;

std::string normalizeArchivePath(std::string value) {
    for (char& c : value) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!value.empty() && value.front() == '/') value.erase(value.begin());
    return value;
}

bool normalizePrefix(const char* prefix, std::string& result) {
    result = prefix ? prefix : "";
    for (char& c : result) if (c == '\\') c = '/';
    while (!result.empty() && result.front() == '/') result.erase(result.begin());
    while (!result.empty() && result.back() == '/') result.pop_back();
    if (!result.empty() && !TorchPath::isSafeLogicalPath(result.c_str())) return false;
    if (!result.empty()) result.push_back('/');
    return true;
}
}

struct ZipArchive::Impl {
    zip_t* archive = nullptr;
    std::shared_ptr<std::vector<uint8_t>> backing;
    std::string prefix;
    std::string description;
    std::unordered_map<std::string, zip_uint64_t> entries;
    std::unordered_map<std::string, std::string> names;

    ~Impl() {
        if (archive) zip_close(archive);
    }

    void indexEntries() {
        entries.clear();
        names.clear();
        const zip_int64_t count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
        for (zip_int64_t i = 0; i < count; ++i) {
            const char* rawName = zip_get_name(archive, static_cast<zip_uint64_t>(i), ZIP_FL_UNCHANGED);
            if (!rawName) continue;
            std::string name(rawName);
            for (char& c : name) if (c == '\\') c = '/';
            if (name.empty() || name.back() == '/') continue;
            if (!prefix.empty()) {
                const std::string foldedName = normalizeArchivePath(name);
                const std::string foldedPrefix = normalizeArchivePath(prefix);
                if (foldedName.rfind(foldedPrefix, 0) != 0) continue;
                name.erase(0, prefix.size());
            }
            if (!TorchPath::isSafeLogicalPath(name.c_str())) continue;
            const std::string key = normalizeArchivePath(name);
            entries[key] = static_cast<zip_uint64_t>(i);
            names[key] = std::move(name);
        }
    }
};

ZipArchive::ZipArchive() : impl(std::make_shared<Impl>()) {}
ZipArchive::~ZipArchive() = default;

bool ZipArchive::open(const char* path) {
    return open(path, "");
}

bool ZipArchive::open(const char* path, const char* rootPrefix) {
    if (!path || !*path) return false;
    std::string normalizedPrefix;
    if (!normalizePrefix(rootPrefix, normalizedPrefix)) {
        Console::instance().printf(LogLevel::Error, "Unsafe ZIP root prefix: %s", rootPrefix ? rootPrefix : "");
        return false;
    }
    int errorCode = 0;
    zip_t* archive = zip_open(path, ZIP_RDONLY, &errorCode);
    if (!archive) {
        zip_error_t error;
        zip_error_init_with_code(&error, errorCode);
        Console::instance().printf(LogLevel::Warn, "Cannot open ZIP %s: %s", path, zip_error_strerror(&error));
        zip_error_fini(&error);
        return false;
    }

    auto state = std::make_shared<Impl>();
    state->archive = archive;
    state->description = path;
    if (!normalizedPrefix.empty()) state->description += "#" + normalizedPrefix;
    state->prefix = std::move(normalizedPrefix);
    state->indexEntries();
    if (state->entries.empty()) {
        Console::instance().printf(LogLevel::Warn, "ZIP root is empty or missing: %s#%s",
                                   path, rootPrefix ? rootPrefix : "");
        return false;
    }
    Console::instance().printf(LogLevel::Info, "ZIP: %s#%s - %zu entries",
                               path, rootPrefix ? rootPrefix : "", state->entries.size());
    impl = std::move(state);
    return true;
}

bool ZipArchive::openMember(ZipArchive& parent, const char* memberPath) {
    if (!parent.impl || !parent.impl->archive || !memberPath ||
        !TorchPath::isSafeLogicalPath(memberPath)) return false;
    const std::string key = normalizeArchivePath(memberPath);
    const auto entry = parent.impl->entries.find(key);
    if (entry == parent.impl->entries.end()) return false;

    // ZIP members can themselves be deflated, and libzip's nested-archive
    // source is not seekable in that case. Keep the expanded nested VL2 in
    // memory as a seekable source; it is never extracted to disk.
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (!parent.readFile(memberPath, *bytes) || bytes->empty()) return false;
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_buffer_create(bytes->data(), bytes->size(), 0, &error);
    if (!source) {
        Console::instance().printf(LogLevel::Warn, "Cannot create nested ZIP source for %s: %s",
                                   memberPath, zip_error_strerror(&error));
        zip_error_fini(&error);
        return false;
    }
    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (!archive) {
        Console::instance().printf(LogLevel::Warn, "Cannot mount nested archive %s: %s",
                                   memberPath, zip_error_strerror(&error));
        zip_source_free(source);
        zip_error_fini(&error);
        return false;
    }
    zip_error_fini(&error);

    auto state = std::make_shared<Impl>();
    state->archive = archive;
    state->backing = std::move(bytes); // Keeps the source buffer alive for nested reads.
    state->description = parent.impl->description;
    if (!state->description.empty() && state->description.back() != '/')
        state->description.push_back('/');
    state->description += memberPath;
    state->indexEntries();
    if (state->entries.empty()) return false;
    Console::instance().printf(LogLevel::Info, "Mounted nested VL2: %s (%zu entries)",
                               state->description.c_str(), state->entries.size());
    impl = std::move(state);
    return true;
}

bool ZipArchive::readFile(const char* path, std::vector<uint8_t>& data) {
    data.clear();
    if (!impl || !impl->archive || !TorchPath::isSafeLogicalPath(path)) return false;
    const auto entry = impl->entries.find(normalizeArchivePath(path));
    if (entry == impl->entries.end()) return false;

    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(impl->archive, entry->second, ZIP_FL_UNCHANGED, &stat) != 0 ||
        !(stat.valid & ZIP_STAT_SIZE) || stat.size > MaxZipEntrySize) return false;
    zip_file_t* file = zip_fopen_index(impl->archive, entry->second, ZIP_FL_UNCHANGED);
    if (!file) return false;
    data.resize(static_cast<size_t>(stat.size));
    size_t offset = 0;
    while (offset < data.size()) {
        const zip_int64_t count = zip_fread(file, data.data() + offset, data.size() - offset);
        if (count <= 0) {
            zip_fclose(file);
            data.clear();
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    if (zip_fclose(file) != 0) {
        data.clear();
        return false;
    }
    return true;
}

bool ZipArchive::fileExists(const char* path) const {
    return impl && impl->archive && TorchPath::isSafeLogicalPath(path) &&
        impl->entries.find(normalizeArchivePath(path)) != impl->entries.end();
}

void ZipArchive::listFiles(const char* pattern, std::vector<std::string>& out) const {
    if (!impl) return;
    std::string foldedPattern = pattern ? normalizeArchivePath(pattern) : std::string();
    for (const auto& [key, _] : impl->entries) {
        if (!pattern || fnmatch(foldedPattern.c_str(), key.c_str(), FNM_PATHNAME) == 0)
            out.push_back(impl->names.at(key));
    }
}
