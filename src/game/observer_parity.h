#pragma once
#include <strings.h>

#include <string>
#include <cctype>
#include <cstdint>

namespace ObserverParity {
inline bool shouldCycleReplayTargets(bool demoPlaying, bool liveConnection) {
    return demoPlaying && !liveConnection;
}

inline bool shouldCycleLiveTargets(bool liveConnection, bool observerState,
                                   bool hasObserverTransport) {
    return liveConnection && observerState && hasObserverTransport;
}

inline bool isPositionReady(bool hasPosition) {
    return hasPosition;
}

// The live observer snapshot uses native ghost id zero as its no-target
// sentinel. Convert it before presentation, where -1 already means free
// camera and positive ids are actual ghosts.
inline int controlGhostIndex(uint16_t nativeGhost) {
    return nativeGhost == 0 ? -1 : static_cast<int>(nativeGhost);
}

// Ghost class names come from the engine's network class table: Player and
// its AIPlayer subclass; FlyingVehicle, HoverVehicle and WheeledVehicle.
inline bool isPlayerClass(const std::string& className) {
    return strcasecmp(className.c_str(), "Player") == 0 || strcasecmp(className.c_str(), "AIPlayer") == 0;
}

inline bool isVehicleClass(const std::string& className) {
    return strcasecmp(className.c_str(), "FlyingVehicle") == 0 ||
           strcasecmp(className.c_str(), "HoverVehicle") == 0 ||
           strcasecmp(className.c_str(), "WheeledVehicle") == 0;
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
