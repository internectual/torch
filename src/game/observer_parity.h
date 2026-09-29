#pragma once

#include <string>
#include <cctype>
#include <cstdint>

namespace ObserverParity {
inline bool isPositionReady(bool hasPosition) {
    return hasPosition;
}

// The live observer snapshot uses native ghost id zero as its no-target
// sentinel. Convert it before presentation, where -1 already means free
// camera and positive ids are actual ghosts.
inline int controlGhostIndex(uint16_t nativeGhost) {
    return nativeGhost == 0 ? -1 : static_cast<int>(nativeGhost);
}

inline bool isPlayerClass(const std::string& className) {
    std::string normalized = className;
    for (char& c : normalized)
        c = (char)std::tolower((unsigned char)c);
    // AIPlayer is the stock bot subclass of Player.  It is still a valid
    // observer target; filtering it out leaves bot-heavy missions with no
    // targets even though their ghosts are present.
    if (normalized == "player" || normalized == "aiplayer" || normalized == "mpb")
        return true;
    // Mission mods commonly derive their observer-visible actors from Player
    // without registering every class name in the client. Torque class names
    // conventionally retain the base suffix, so keep those subclasses in the
    // same target set as the stock classes.
    return (normalized.size() > 6 &&
            normalized.compare(normalized.size() - 6, 6, "player") == 0) ||
           (normalized.size() > 3 &&
            normalized.compare(normalized.size() - 3, 3, "mpb") == 0);
}

inline bool isVehicleClass(const std::string& className) {
    std::string normalized = className;
    for (char& c : normalized)
        c = (char)std::tolower((unsigned char)c);
    if (normalized == "flyingvehicle" || normalized == "hovervehicle" ||
        normalized == "wheeledvehicle" || normalized == "vehicle")
        return true;
    if (normalized.size() > 7 &&
        normalized.compare(normalized.size() - 7, 7, "vehicle") == 0)
        return true;
    return false;
}

inline bool isPlayerTarget(const std::string& className, int damageState,
                           bool sensorVisible = true) {
    return sensorVisible && isPlayerClass(className) &&
           // ShapeBase damage state 1 is damaged/disabled but still has a
           // live body to observe. State 2 is the destroyed/wreck state; the
           // renderer uses the same boundary for destroyed animations.
           damageState >= 0 && damageState < 2;
}

// Observer cycling and the target-finder use the same sensor visibility
// filter.  A target that is hidden by the listener's sensor mask must not be
// reachable through the keyboard cycle even when its ghost is present.
inline bool isVisiblePlayerTarget(const std::string& className, int damageState,
                                  bool sensorVisible) {
    return isPlayerTarget(className, damageState, sensorVisible);
}

inline bool isSpectatableTarget(const std::string& className, int damageState,
                                bool sensorVisible = true) {
    return isVisiblePlayerTarget(className, damageState, sensorVisible) ||
           (sensorVisible && isVehicleClass(className) && damageState >= 0 &&
            damageState < 2);
}

inline bool isReadySpectatableTarget(const std::string& className, int damageState,
                                     bool hasPosition, bool sensorVisible = true) {
    return isPositionReady(hasPosition) &&
           isSpectatableTarget(className, damageState, sensorVisible);
}
} // namespace ObserverParity
