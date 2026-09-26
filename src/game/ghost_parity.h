#pragma once

#include <cctype>
#include <string>

inline bool ghostClassIs(const std::string& className, const char* expected) {
    const std::string wanted(expected);
    if (className.size() != wanted.size()) return false;
    for (size_t i = 0; i < className.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(className[i])) !=
            std::tolower(static_cast<unsigned char>(wanted[i])))
            return false;
    }
    return true;
}

inline bool isWorldLevelGhostClass(const std::string& className) {
    for (const char* name : {"InteriorInstance", "StaticShape", "ScopeAlwaysShape",
                             "TSStatic", "TerrainBlock", "Sky", "Sun", "Lightning",
                             "WaterBlock", "MissionArea", "ForceFieldBare",
                             // SceneObjects without a shape; World draws or
                             // plays the mission copies.
                             "AudioEmitter", "Precipitation", "VehicleBlocker"}) {
        if (ghostClassIs(className, name)) return true;
    }
    return false;
}

inline bool isProjectileGhostClass(const std::string& className) {
    std::string normalized = className;
    for (char& c : normalized) c = (char)std::tolower(static_cast<unsigned char>(c));
    return normalized.find("projectile") != std::string::npos ||
           ghostClassIs(className, "EnergyBolt") ||
           ghostClassIs(className, "LinearFlare") ||
           ghostClassIs(className, "Splash");
}

inline bool ghostClassContains(const std::string& className, const char* fragment);

inline bool isWheeledVehicleGhostClass(const std::string& className) {
    // Tribes 2 streams concrete vehicle class names (for example MPB and
    // Wildcat), not their WheeledVehicle base class. Wheel animation must
    // use the same subclass classification as vehicle rendering.
    return ghostClassIs(className, "WheeledVehicle") ||
           ghostClassIs(className, "MPB") ||
           ghostClassIs(className, "Wildcat") ||
           ghostClassContains(className, "wheeledvehicle");
}

inline bool ghostClassContains(const std::string& className, const char* fragment) {
    std::string folded = className;
    std::string wanted(fragment);
    for (char& c : folded) c = (char)std::tolower(static_cast<unsigned char>(c));
    for (char& c : wanted) c = (char)std::tolower(static_cast<unsigned char>(c));
    return folded.find(wanted) != std::string::npos;
}

// Torque class lookup is case-insensitive. Keep rendering classification on
// the same rule so streamed/custom class casing cannot change presentation.
inline bool isVehicleGhostClass(const std::string& className) {
    return ghostClassContains(className, "vehicle") ||
           ghostClassIs(className, "Shrike") ||
           ghostClassIs(className, "Turbograv") ||
           ghostClassIs(className, "Shield") ||
           ghostClassIs(className, "Wildcat");
}

inline bool isTurretGhostClass(const std::string& className) {
    return ghostClassIs(className, "Turret") || ghostClassIs(className, "Sentry");
}
