#include "core/math.h"
#include "core/console.h"
#include "fs/file_system.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>

// Minimal .mis file parser
// Extracts object definitions and their properties

struct MisProp {
    std::string name;
    std::string value;
};

struct MisObject {
    std::string className;
    std::string objName;
    std::vector<MisProp> props;
    std::vector<MisObject> children;
};

static std::string trim(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) start++;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r')) end--;
    return s.substr(start, end - start);
}

static MisObject parseMisObject(const std::string& input, size_t& pos) {
    MisObject obj;
    // Skip whitespace
    while (pos < input.size() && input[pos] <= ' ') pos++;

    // Expect "new"
    if (pos + 4 > input.size() || input.substr(pos, 3) != "new") return obj;
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
        if (pos + 4 <= input.size() && input.substr(pos, 3) == "new") {
            MisObject child = parseMisObject(input, pos);
            if (!child.className.empty()) {
                obj.children.push_back(std::move(child));
            }
            continue;
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
    size_t pos = 0;

    // Remove // comments
    std::string clean;
    for (size_t i = 0; i < content.size(); i++) {
        if (i + 1 < content.size() && content[i] == '/' && content[i+1] == '/') {
            while (i < content.size() && content[i] != '\n') i++;
            continue;
        }
        clean += content[i];
    }

    // Parse objects
    while (pos < clean.size()) {
        // Look for "new"
        size_t found = clean.find("new ", pos);
        if (found == std::string::npos) break;
        pos = found;

        MisObject obj = parseMisObject(clean, pos);
        if (!obj.className.empty()) {
            objects.push_back(std::move(obj));
        }
    }

    return objects;
}

// Find a property value by name (case-insensitive)
static std::string getProp(const std::vector<MisProp>& props, const std::string& name) {
    for (auto& p : props) {
        if (p.name == name) return p.value;
    }
    return "";
}

// Find first object of a given class
static MisObject* findObject(std::vector<MisObject>& objects, const std::string& className) {
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