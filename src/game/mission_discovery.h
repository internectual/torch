#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <filesystem>
#include "fs/file_system.h"

struct MissionMetadata {
    std::string file;
    std::string displayName;
    std::string types;
};

inline std::string missionLower(std::string value) {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    return value;
}

inline bool isMissionFile(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string extension = missionLower(path.substr(dot));
    return extension == ".mis" || extension == ".mispk";
}

inline std::string missionRelativeFile(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::string folded = missionLower(path);
    if (folded.starts_with("base/missions/")) return path.substr(14);
    if (folded.starts_with("missions/")) return path.substr(9);
    return path;
}

inline std::string missionMapName(std::string path) {
    path = missionRelativeFile(std::move(path));
    if (isMissionFile(path)) path.resize(path.rfind('.'));
    return path;
}

inline bool isSafeMissionMapName(const std::string& name) {
    if (name.empty() || name.find('\\') != std::string::npos) return false;
    const std::filesystem::path path(name);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& component : path)
        if (component == "." || component == "..") return false;
    return true;
}

inline std::string missionLoadPath(const std::string& name) {
    const std::string map = missionMapName(name);
    return isSafeMissionMapName(map) ? map : std::string();
}

inline std::vector<std::string> missionFileCandidates(const std::string& name) {
    const std::string map = missionLoadPath(name);
    if (map.empty()) return {};
    return {"missions/" + map + ".mis", "missions/" + map + ".misPK"};
}

inline bool resolveMissionFile(FileSystem& fs, const std::string& name,
                               std::string& resolvedPath, std::string& content) {
    const std::string requested = missionLoadPath(name);
    if (requested.empty()) return false;

    for (const auto& candidate : missionFileCandidates(requested)) {
        if (fs.readTextFile(candidate.c_str(), content) && !content.empty()) {
            resolvedPath = candidate;
            return true;
        }
    }

    // Demo headers sometimes contain only a basename. Use a unique mounted
    // mission match without discarding authored subdirectories.
    std::vector<std::string> mounted;
    fs.listFiles(nullptr, mounted);
    const std::string requestedLower = missionLower(requested);
    const size_t requestedSlash = requested.rfind('/');
    const std::string requestedBase = missionLower(
        requested.substr(requestedSlash == std::string::npos ? 0 : requestedSlash + 1));
    std::string match;
    for (const auto& candidate : mounted) {
        const std::string candidateLower = missionLower(candidate);
        if (!candidateLower.starts_with("missions/") || !isMissionFile(candidate)) continue;
        const std::string map = missionMapName(candidate);
        const std::string mapLower = missionLower(map);
        const size_t slash = map.rfind('/');
        const std::string base = missionLower(
            map.substr(slash == std::string::npos ? 0 : slash + 1));
        if (mapLower != requestedLower && base != requestedBase) continue;
        if (!match.empty()) return false;
        match = candidate;
    }
    if (match.empty() || !fs.readTextFile(match.c_str(), content) || content.empty()) return false;
    resolvedPath = match;
    return true;
}

inline MissionMetadata parseMissionMetadata(const std::string& file,
                                            const std::string& content) {
    MissionMetadata result;
    result.file = missionMapName(file);
    const size_t slash = result.file.rfind('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = result.file.rfind('.');
    const size_t length = dot == std::string::npos ? std::string::npos : dot - start;
    result.displayName = result.file.substr(start, length);
    const std::string defaultName = result.displayName;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t' || line[begin] == '\r')) ++begin;
        if (line.compare(begin, 2, "//") != 0) continue;
        begin += 2;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
        const size_t equal = line.find('=', begin);
        if (equal == std::string::npos) continue;
        size_t keyEnd = equal;
        while (keyEnd > begin && (line[keyEnd - 1] == ' ' || line[keyEnd - 1] == '\t')) --keyEnd;
        const std::string key = missionLower(line.substr(begin, keyEnd - begin));
        size_t valueBegin = equal + 1;
        while (valueBegin < line.size() && (line[valueBegin] == ' ' || line[valueBegin] == '\t')) ++valueBegin;
        std::string value = line.substr(valueBegin);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (result.displayName == defaultName && (key == "displayname" || key == "missionname"))
            result.displayName = value;
        if (result.types.empty() && (key == "missiontypes" || key == "missiontype" ||
                                     key == "gametypes" || key == "gametype"))
            result.types = value;
        if (!result.types.empty() && result.displayName != defaultName) break;
    }
    return result;
}

inline bool missionIsSinglePlayer(const MissionMetadata& mission) {
    std::istringstream words(mission.types);
    std::string word;
    while (words >> word) {
        while (!word.empty() && (word.front() == '"' || word.front() == '(')) word.erase(word.begin());
        while (!word.empty() && (word.back() == '"' || word.back() == ')' || word.back() == ',')) word.pop_back();
        if (missionLower(word) == "singleplayer") return true;
    }
    return false;
}

inline std::vector<std::string> missionPreviewCandidates(const std::string& file) {
    const std::string base = missionLoadPath(file);
    if (base.empty()) return {};
    return {"missions/" + base + ".jpg", "missions/" + base + ".png",
            "missions/" + base + ".bmp"};
}
