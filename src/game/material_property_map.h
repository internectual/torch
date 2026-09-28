#pragma once
// MaterialPropertyMap (dgl/materialPropertyMap.cc): the per-material foot
// puff colours, footstep sound and environment map that the stock
// scripts/*PropMap.cs register with addMaterialMapping.
#include "core/math.h"
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

struct MaterialProperties {
    int sound = -1;
    ColorF puffColor[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    std::string environmentMap;
    float environmentFactor = 0.0f;
};

class MaterialPropertyMap {
public:
    static MaterialPropertyMap& instance() {
        static MaterialPropertyMap map;
        return map;
    }

    static std::string key(std::string name) {
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        return name;
    }

    // addMaterialMapping("name", "color: r g b a0 a1", "sound: n", ...)
    void addMapping(const std::vector<std::string>& args) {
        if (args.empty()) return;
        MaterialProperties entry;
        auto startsWith = [](const std::string& s, const char* prefix) {
            size_t n = 0;
            while (prefix[n]) {
                if (n >= s.size() || std::tolower((unsigned char)s[n]) != prefix[n]) return false;
                ++n;
            }
            return true;
        };
        auto after = [](const std::string& s) {
            size_t colon = s.find(':');
            if (colon == std::string::npos) return std::string();
            size_t i = colon + 1;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return s.substr(i);
        };
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& param = args[i];
            if (startsWith(param, "color:")) {
                float v[5] = {0, 0, 0, 0, 0};
                const std::string rest = after(param);
                const char* p = rest.c_str();
                for (float& value : v) {
                    char* end = nullptr;
                    value = std::strtof(p, &end);
                    if (end == p) break;
                    p = end;
                }
                entry.puffColor[0] = {v[0], v[1], v[2], v[3]};
                entry.puffColor[1] = {v[0], v[1], v[2], v[4]};
            } else if (startsWith(param, "sound:")) {
                entry.sound = std::atoi(after(param).c_str());
            } else if (startsWith(param, "environment:")) {
                const std::string rest = after(param);
                const size_t space = rest.find(' ');
                entry.environmentMap = rest.substr(0, space);
                if (space != std::string::npos) entry.environmentFactor = (float)std::atof(rest.c_str() + space + 1);
            }
        }
        entries[key(args[0])] = entry;
    }

    const MaterialProperties* find(const std::string& name) const {
        auto it = entries.find(key(name));
        return it == entries.end() ? nullptr : &it->second;
    }

    // TerrainBlock::onAdd: the terrain's mapping is "terrain/" + its first
    // material file name, less any "terrain." prefix.
    const MaterialProperties* terrain(std::string firstMaterial) const {
        if (key(firstMaterial).rfind("terrain.", 0) == 0) firstMaterial = firstMaterial.substr(8);
        return find("terrain/" + firstMaterial);
    }

private:
    std::unordered_map<std::string, MaterialProperties> entries;
};
