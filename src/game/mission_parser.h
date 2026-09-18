#pragma once

#include "core/math.h"
#include "core/console.h"
#include "fs/file_system.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>

// Minimal .mis file parser
// Extracts object definitions and their properties

struct MisProp {
    std::string name;
    std::string value;
};

struct MisObject {
    std::string className;
    std::string objName;
    std::string parentName;
    int teamId = 0;
    std::vector<MisProp> props;
    std::vector<MisObject> children;
};

struct MisParseBudget {
    static constexpr size_t MaxContentBytes = 64u * 1024u * 1024u;
    static constexpr size_t MaxObjects = 100000;
    static constexpr size_t MaxPropertiesPerObject = 4096;
    static constexpr size_t MaxValueBytes = 1u * 1024u * 1024u;
    static constexpr size_t MaxDepth = 128;
    size_t objects = 0;
    bool exceeded = false;
};

static std::string trim(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) start++;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r')) end--;
    return s.substr(start, end - start);
}

static int missionTeamId(const std::string& name) {
    if (name.size() != 5 || (name[0] != 'T' && name[0] != 't') ||
        (name[1] != 'e' && name[1] != 'E') ||
        (name[2] != 'a' && name[2] != 'A') ||
        (name[3] != 'm' && name[3] != 'M') ||
        (name[4] < '1' || name[4] > '9')) return 0;
    return name[4] - '0';
}

static MisObject parseMisObject(const std::string& input, size_t& pos,
                                MisParseBudget& budget, size_t depth = 0) {
    MisObject obj;
    if (depth > MisParseBudget::MaxDepth || budget.objects >= MisParseBudget::MaxObjects) {
        budget.exceeded = true;
        return obj;
    }
    budget.objects++;
    // Skip whitespace
    while (pos < input.size() && input[pos] <= ' ') pos++;

    // Expect "new"
    if (pos + 3 > input.size() || input.substr(pos, 3) != "new" ||
        (pos + 3 < input.size() && input[pos + 3] > ' ')) return obj;
    pos += 3;
    while (pos < input.size() && input[pos] <= ' ') pos++;

    // Read class name
    size_t start = pos;
    while (pos < input.size() && input[pos] > ' ' && input[pos] != '(' && input[pos] != '{') pos++;
    obj.className = input.substr(start, pos - start);

    // Read optional (Name)
    while (pos < input.size() && input[pos] <= ' ') pos++;
    if (pos < input.size() && input[pos] == '(') {
        pos++;
        start = pos;
        while (pos < input.size() && input[pos] != ')') pos++;
        obj.objName = input.substr(start, pos - start);
        if (pos < input.size()) pos++; // skip ')'
    }
    obj.teamId = missionTeamId(obj.objName);

    // Read optional : parent (skip for now - TorqueScript inheritance)
    while (pos < input.size() && input[pos] <= ' ') pos++;
    if (pos < input.size() && input[pos] == ':') {
        pos++;
        while (pos < input.size() && input[pos] > ' ') pos++; // skip parent name
    }

    // Expect {
    while (pos < input.size() && input[pos] <= ' ') pos++;
    if (pos >= input.size() || input[pos] != '{') return obj;
    pos++; // skip {

    // Read properties and children
    while (pos < input.size()) {
        while (pos < input.size() && input[pos] <= ' ') pos++;
        if (pos >= input.size()) break;

        // Check for } (end of this object)
        if (input[pos] == '}') { pos++; break; }

        // Check for nested object (starts with "new")
        if (pos + 3 <= input.size() && input.substr(pos, 3) == "new" &&
            (pos + 3 == input.size() || input[pos + 3] <= ' ')) {
            MisObject child = parseMisObject(input, pos, budget, depth + 1);
            if (!child.className.empty()) {
                child.parentName = obj.objName;
                if (obj.teamId != 0) child.teamId = obj.teamId;
                obj.children.push_back(std::move(child));
            }
            // Consume ; after child's }
            while (pos < input.size() && input[pos] <= ' ') pos++;
            if (pos < input.size() && input[pos] == ';') pos++;
            continue;
        }

        if (obj.props.size() >= MisParseBudget::MaxPropertiesPerObject) {
            budget.exceeded = true;
            return MisObject{};
        }

        // Read property name (until = or { or space)
        start = pos;
        while (pos < input.size() && input[pos] != '=' && input[pos] > ' ' && input[pos] != '{') pos++;
        std::string propName = trim(input.substr(start, pos - start));
        if (propName.empty()) { pos++; continue; }

        // Skip =
        while (pos < input.size() && input[pos] <= ' ') pos++;
        if (pos < input.size() && input[pos] == '=') pos++;

        // Read value (until ; or } or newline with indentation)
        while (pos < input.size() && input[pos] <= ' ') pos++;

        std::string propValue;
        if (pos < input.size() && input[pos] == '"') {
            // Quoted string
            pos++; // skip opening "
            start = pos;
            while (pos < input.size() && input[pos] != '"') {
                if (input[pos] == '\\') { pos++; if (pos < input.size()) { propValue += input[pos]; pos++; } }
                else { propValue += input[pos]; pos++; }
            }
            if (pos < input.size()) pos++; // skip closing "
        } else {
            // Unquoted value (until ; or } or end of line)
            start = pos;
            while (pos < input.size() && input[pos] != ';' && input[pos] != '}') pos++;
            propValue = trim(input.substr(start, pos - start));
        }

        if (propValue.size() > MisParseBudget::MaxValueBytes) {
            budget.exceeded = true;
            return MisObject{};
        }

        // Skip ;
        if (pos < input.size() && input[pos] == ';') pos++;

        if (!propName.empty()) {
            // Convert to lowercase for case-insensitive T2 property names
            for (auto& c : propName) if (c >= 'A' && c <= 'Z') c += 32;
            obj.props.push_back({propName, propValue});
        }
    }

    return obj;
}

static std::vector<MisObject> parseMisFile(const std::string& content) {
    std::vector<MisObject> objects;
    MisParseBudget budget;
    if (content.size() > MisParseBudget::MaxContentBytes) return objects;
    size_t pos = 0;

    // Remove comments without touching quoted TorqueScript strings.
    std::string clean;
    clean.reserve(content.size());
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < content.size(); i++) {
        const char c = content[i];
        if (c == '"' && !escaped) quoted = !quoted;
        if (!quoted && c == '/' && i + 1 < content.size() && content[i + 1] == '/') {
            while (i < content.size() && content[i] != '\n') i++;
            if (i < content.size()) clean += '\n';
            escaped = false;
            continue;
        }
        clean += c;
        escaped = c == '\\' && !escaped;
        if (c != '\\') escaped = false;
    }

    // Parse objects and datablocks
    while (pos < clean.size()) {
        // Look for "new" or "datablock"
        auto findKeyword = [&](const char* keyword) {
            const size_t length = std::strlen(keyword);
            for (size_t at = pos; at + length <= clean.size(); ++at) {
                if (clean.compare(at, length, keyword) == 0 &&
                    (at == 0 || clean[at - 1] <= ' ') &&
                    (at + length == clean.size() || clean[at + length] <= ' '))
                    return at;
            }
            return std::string::npos;
        };
        size_t found = findKeyword("new");
        size_t dbFound = findKeyword("datablock");
        if (dbFound != std::string::npos && (found == std::string::npos || dbFound < found)) {
            if (budget.objects >= MisParseBudget::MaxObjects) return {};
            budget.objects++;
            // Parse datablock definition
            pos = dbFound + 10; // skip "datablock "
            while (pos < clean.size() && clean[pos] <= ' ') pos++;
            // Read class name
            size_t start = pos;
            while (pos < clean.size() && clean[pos] > ' ' && clean[pos] != '(' && clean[pos] != '{') pos++;
            std::string className = clean.substr(start, pos - start);
            // Read optional (InstanceName)
            std::string objName;
            while (pos < clean.size() && clean[pos] <= ' ') pos++;
            if (pos < clean.size() && clean[pos] == '(') {
                pos++;
                start = pos;
                while (pos < clean.size() && clean[pos] != ')') pos++;
                objName = clean.substr(start, pos - start);
                if (pos < clean.size()) pos++;
            }
            // Skip inheritance
            while (pos < clean.size() && clean[pos] <= ' ') pos++;
            if (pos < clean.size() && clean[pos] == ':') {
                pos++;
                while (pos < clean.size() && clean[pos] > ' ') pos++;
            }
            // Expect {
            while (pos < clean.size() && clean[pos] <= ' ') pos++;
            if (pos >= clean.size() || clean[pos] != '{') { pos++; continue; }
            pos++;
            // Parse properties until }
            MisObject obj;
            obj.className = className;
            obj.objName = objName;
            obj.teamId = missionTeamId(objName);
            while (pos < clean.size()) {
                while (pos < clean.size() && clean[pos] <= ' ') pos++;
                if (pos >= clean.size() || clean[pos] == '}') { if (pos < clean.size()) pos++; break; }
                if (obj.props.size() >= MisParseBudget::MaxPropertiesPerObject) {
                    budget.exceeded = true;
                    return {};
                }
                // Read property name
                start = pos;
                while (pos < clean.size() && clean[pos] != '=' && clean[pos] > ' ' && clean[pos] != '{') pos++;
                std::string propName = trim(clean.substr(start, pos - start));
                if (propName.empty()) { pos++; continue; }
                while (pos < clean.size() && clean[pos] <= ' ') pos++;
                if (pos < clean.size() && clean[pos] == '=') pos++;
                while (pos < clean.size() && clean[pos] <= ' ') pos++;
                std::string propValue;
                if (pos < clean.size() && clean[pos] == '"') {
                    pos++;
                    start = pos;
                    while (pos < clean.size() && clean[pos] != '"') {
                        if (clean[pos] == '\\') { pos++; if (pos < clean.size()) { propValue += clean[pos]; pos++; } }
                        else { propValue += clean[pos]; pos++; }
                    }
                    if (pos < clean.size()) pos++;
                } else {
                    start = pos;
                    while (pos < clean.size() && clean[pos] != ';' && clean[pos] != '}') pos++;
                    propValue = trim(clean.substr(start, pos - start));
                }
                if (propValue.size() > MisParseBudget::MaxValueBytes) {
                    budget.exceeded = true;
                    return {};
                }
                if (pos < clean.size() && clean[pos] == ';') pos++;
                for (auto& c : propName) if (c >= 'A' && c <= 'Z') c += 32;
                obj.props.push_back({propName, propValue});
            }
            // Skip ;
            while (pos < clean.size() && clean[pos] <= ' ') pos++;
            if (pos < clean.size() && clean[pos] == ';') pos++;
            objects.push_back(std::move(obj));
            continue;
        }
        if (found == std::string::npos) break;
        pos = found;

        MisObject obj = parseMisObject(clean, pos, budget);
        if (!obj.className.empty()) {
            objects.push_back(std::move(obj));
        }
        // Consume ; after top-level }
        while (pos < clean.size() && clean[pos] <= ' ') pos++;
        if (pos < clean.size() && clean[pos] == ';') pos++;
    }

    if (budget.exceeded) return {};

    // Flatten children into top-level objects (recursive breadth-first)
    size_t i = 0;
    while (i < objects.size()) {
        if (!objects[i].children.empty()) {
            for (auto& child : objects[i].children) {
                if (objects.size() >= MisParseBudget::MaxObjects) return {};
                objects.push_back(std::move(child));
            }
            objects[i].children.clear();
        }
        i++;
    }

    return objects;
}

// Find a property value by name (case-insensitive)
static std::string getProp(const std::vector<MisProp>& props, const std::string& name) {
    std::string lower;
    for (auto& c : name) lower += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    for (auto& p : props) {
        if (p.name == lower) return p.value;
    }
    return "";
}

// Find first object of a given class
inline MisObject* findObject(std::vector<MisObject>& objects, const std::string& className) {
    for (auto& obj : objects) {
        if (obj.className == className) return &obj;
    }
    return nullptr;
}

// Parse position string "x y z" to Point3F
static Point3F parsePos(const std::string& s) {
    Point3F p{0,0,0};
    float vals[3] = {0,0,0};
    int count = sscanf(s.c_str(), "%f %f %f", &vals[0], &vals[1], &vals[2]);
    if (count >= 1) p.x = vals[0];
    if (count >= 2) p.y = vals[1];
    if (count >= 3) p.z = vals[2];
    return p;
}

struct AuthoredMissionMarker {
    Point3F position{};
    Point3F rotation{0, 0, 1};
    float rotationAngleDeg = 0.0f;
    Point3F scale{1, 1, 1};
    float radius = 0.0f;
    int teamId = 0;
    std::string label;
};

struct AuthoredMissionObjective {
    AuthoredMissionMarker marker;
    std::string description;
    std::string mode;
    std::string targetObject;
    int targetObjectId = -1;
    float weight[4]{};
    bool offense = false;
    bool defense = false;
    bool locked = false;
    bool invalid = false;
};

struct AuthoredNavigationGraph {
    std::string graphFile;
    Point3F customArea{};
    float customAreaWidth = 0.0f;
    float customAreaHeight = 0.0f;
    float conjoinAngleDev = 0.0f;
    float conjoinBowlDev = 0.0f;
    float cullDensity = 0.0f;
    float coverage = 0.0f;
    Point3F position{};
};

[[maybe_unused]] static bool authoredBool(const MisObject& object, const char* name,
                                          bool defaultValue = false) {
    const std::string value = getProp(object.props, name);
    if (value.empty()) return defaultValue;
    return value == "1" || value == "true" || value == "TRUE";
}

// These are the object-level switches supported by the local V12 mapper.  A
// hidden object remains collidable; collision is controlled independently.
[[maybe_unused]] static bool authoredVisible(const MisObject& object) {
    if (getProp(object.props, "hidden").size())
        return !authoredBool(object, "hidden");
    return authoredBool(object, "visible", true);
}

[[maybe_unused]] static bool authoredCollidable(const MisObject& object, bool defaultValue) {
    if (authoredBool(object, "disablecollision")) return false;
    const std::string type = getProp(object.props, "collisiontype");
    std::string lower = type;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower == "none" || lower == "no collision" || lower == "nocollision") return false;
    return defaultValue;
}

[[maybe_unused]] static std::string authoredSequence(const MisObject& object) {
    std::string sequence = getProp(object.props, "sequence");
    if (sequence.empty()) sequence = getProp(object.props, "animation");
    return sequence;
}

// Keep authored marker interpretation in one place for the local mapper and
// the deterministic parser tests. Torque stores rotations as axis-angle.
[[maybe_unused]] static AuthoredMissionMarker authoredMissionMarker(const MisObject& object) {
    AuthoredMissionMarker marker;
    marker.position = parsePos(getProp(object.props, "position"));
    const std::string rotation = getProp(object.props, "rotation");
    float angle = 0.0f;
    if (sscanf(rotation.c_str(), "%f %f %f %f", &marker.rotation.x,
               &marker.rotation.y, &marker.rotation.z, &angle) >= 3)
        marker.rotationAngleDeg = angle;
    const std::string scale = getProp(object.props, "scale");
    if (sscanf(scale.c_str(), "%f %f %f", &marker.scale.x, &marker.scale.y,
               &marker.scale.z) != 3)
        marker.scale = {1, 1, 1};
    if (object.className == "SpawnSphere") {
        const std::string radius = getProp(object.props, "radius");
        marker.radius = radius.empty() ? 100.0f : std::max(0.0f, (float)std::atof(radius.c_str()));
    }
    const std::string team = getProp(object.props, "team");
    marker.teamId = team.empty() ? object.teamId : std::atoi(team.c_str());
    marker.label = getProp(object.props, "nametag");
    if (marker.label.empty()) marker.label = getProp(object.props, "name");
    if (marker.label.empty() && (object.className == "Marker" ||
                                 object.className == "MissionMarker" ||
                                 object.className == "SpawnSphere"))
        marker.label = object.objName;
    return marker;
}

[[maybe_unused]] static AuthoredMissionObjective authoredMissionObjective(const MisObject& object) {
    AuthoredMissionObjective objective;
    objective.marker = authoredMissionMarker(object);
    objective.description = getProp(object.props, "description");
    objective.mode = getProp(object.props, "mode");
    objective.targetObject = getProp(object.props, "targetobject");
    const std::string targetId = getProp(object.props, "targetobjectid");
    if (!targetId.empty()) objective.targetObjectId = std::atoi(targetId.c_str());
    for (int i = 0; i < 4; ++i) {
        const std::string value = getProp(object.props, "weightlevel" + std::to_string(i + 1));
        if (!value.empty()) objective.weight[i] = (float)std::atof(value.c_str());
    }
    objective.offense = authoredBool(object, "offense");
    objective.defense = authoredBool(object, "defense");
    objective.locked = authoredBool(object, "locked");
    objective.invalid = authoredBool(object, "isInvalid");
    if (objective.marker.label.empty())
        objective.marker.label = objective.description.empty() ? object.objName : objective.description;
    return objective;
}

[[maybe_unused]] static AuthoredNavigationGraph authoredNavigationGraph(const MisObject& object) {
    AuthoredNavigationGraph graph;
    graph.graphFile = getProp(object.props, "graphfile");
    float area[4] = {};
    if (sscanf(getProp(object.props, "customarea").c_str(), "%f %f %f %f",
               &area[0], &area[1], &area[2], &area[3]) == 4) {
        graph.customArea = {area[0], area[1], 0};
        graph.customAreaWidth = area[2];
        graph.customAreaHeight = area[3];
    }
    graph.conjoinAngleDev = (float)std::atof(getProp(object.props, "conjoinangledev").c_str());
    graph.conjoinBowlDev = (float)std::atof(getProp(object.props, "conjoinbowldev").c_str());
    graph.cullDensity = (float)std::atof(getProp(object.props, "culldensity").c_str());
    graph.coverage = (float)std::atof(getProp(object.props, "coverage").c_str());
    graph.position = parsePos(getProp(object.props, "position"));
    return graph;
}

[[maybe_unused]] static const MisObject* selectAuthoredSpawn(const std::vector<MisObject>& objects,
                                                            int teamId) {
    const MisObject* selected = nullptr;
    auto better = [](const MisObject* left, const MisObject* right) {
        if (!right) return true;
        if (left->objName != right->objName) return left->objName < right->objName;
        const Point3F a = parsePos(getProp(left->props, "position"));
        const Point3F b = parsePos(getProp(right->props, "position"));
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    };
    // Prefer team-specific spheres over neutral spheres, with stable ordering
    // within each candidate class.
    for (int pass = 0; pass < 2; ++pass) {
        selected = nullptr;
        for (const auto& object : objects) {
            if (object.className != "SpawnSphere") continue;
            const int objectTeam = authoredMissionMarker(object).teamId;
            const bool wanted = pass == 0 ? objectTeam == teamId : objectTeam == 0;
            if (wanted && better(&object, selected)) selected = &object;
        }
        if (selected) return selected;
    }
    for (const auto& object : objects) {
        if (object.className != "SpawnSphere") continue;
        if (better(&object, selected)) selected = &object;
    }
    return selected;
}
