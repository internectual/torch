#pragma once

#include <cctype>
#include <algorithm>
#include <string>

inline std::string missionRulesLower(std::string value) {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    return value;
}

// Torque resolves mission class names case-insensitively. Keep runtime checks
// from making authored objects depend on the spelling used by the .mis file.
inline bool missionClassIs(const std::string& className, const char* expected) {
    return expected && missionRulesLower(className) == missionRulesLower(expected);
}

// SimObject names are resolved case-insensitively by the Torque console.
// Keep script calls to named mission objects consistent with class lookup.
inline bool missionObjectNameIs(const std::string& authoredName,
                                const std::string& requestedName) {
    return authoredName.size() == requestedName.size() &&
           missionRulesLower(authoredName) == missionRulesLower(requestedName);
}

// InteriorInstance is a renderable SceneObject and supports the same hidden
// state as StaticShape. Mission scripts use this for doors and dynamic map
// geometry, so it must not be rejected by the setHidden bridge.
inline bool missionObjectCanBeHidden(const std::string& className) {
    return missionClassIs(className, "InteriorInstance") ||
           missionClassIs(className, "StaticShape") ||
           missionClassIs(className, "TSStatic") ||
           missionClassIs(className, "Turret") ||
           missionClassIs(className, "Item") ||
           missionClassIs(className, "ForceFieldBare") ||
           missionClassIs(className, "Vehicle") ||
           missionRulesLower(className).ends_with("vehicle");
}
