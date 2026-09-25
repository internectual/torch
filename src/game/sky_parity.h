#pragma once

#include <algorithm>
#include <string>
#include <vector>

struct SkyMaterialListEntries {
    std::vector<std::string> faces;
    std::string environment;
    std::vector<std::string> clouds;
};

// Torque material lists are line-oriented, but shipped lists commonly contain
// blank lines and both full-line and trailing comments. Keep comments from
// changing the fixed face/environment/cloud slot positions.
inline SkyMaterialListEntries parseSkyMaterialList(const std::string& content) {
    SkyMaterialListEntries result;
    size_t pos = 0;
    size_t entry = 0;
    while (pos < content.size() && entry < 10) {
        const size_t end = content.find('\n', pos);
        std::string line = content.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? content.size() : end + 1;

        const size_t slash = line.find("//");
        const size_t hash = line.find('#');
        const size_t semicolon = line.find(';');
        size_t comment = std::string::npos;
        for (size_t marker : {slash, hash, semicolon})
            if (marker != std::string::npos) comment =
                comment == std::string::npos ? marker : std::min(comment, marker);
        if (comment != std::string::npos) line.resize(comment);
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const size_t last = line.find_last_not_of(" \t\r");
        line = line.substr(first, last - first + 1);
        if (line.empty()) continue;

        if (entry < 6) result.faces.push_back(line);
        else if (entry == 6) result.environment = line;
        else result.clouds.push_back(line);
        ++entry;
    }
    return result;
}
