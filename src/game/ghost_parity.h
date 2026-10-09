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
    // StaticShape and ScopeAlwaysShape ghosts carry their datablock shape
    // and draw (and collide) as ghosts; the world never holds them.
    for (const char* name : {"InteriorInstance",
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

inline bool isWheeledVehicleGhostClass(const std::string& className) {
    return ghostClassIs(className, "WheeledVehicle");
}


// Torque class lookup is case-insensitive. Keep rendering classification on
// the same rule so streamed/custom class casing cannot change presentation.
inline bool isVehicleGhostClass(const std::string& className) {
    return ghostClassIs(className, "FlyingVehicle") || ghostClassIs(className, "HoverVehicle") ||
           ghostClassIs(className, "WheeledVehicle");
}

inline bool isTurretGhostClass(const std::string& className) {
    return ghostClassIs(className, "Turret");
}
