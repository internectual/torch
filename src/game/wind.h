#pragma once

#include "core/math.h"
#include <cmath>

// Torque's particle and precipitation systems share one client wind vector.
inline Point3F torchWindVelocity{};

inline void setTorchWindVelocity(const Point3F& velocity) {
    // Mission/script wind is shared by precipitation and particles.  Reject
    // malformed components individually so one bad field cannot make all
    // weather positions non-finite until the next mission load.
    torchWindVelocity = {
        std::isfinite(velocity.x) ? velocity.x : 0.0f,
        std::isfinite(velocity.y) ? velocity.y : 0.0f,
        std::isfinite(velocity.z) ? velocity.z : 0.0f};
}

inline Point3F getTorchWindVelocity() {
    return torchWindVelocity;
}

inline Point3F torchWindVelocityToTorque() {
    return {torchWindVelocity.x, -torchWindVelocity.z, torchWindVelocity.y};
}

inline Point3F windAcceleration(float coefficient) {
    if (!std::isfinite(coefficient)) return {};
    const Point3F acceleration{torchWindVelocity.x * coefficient,
                               torchWindVelocity.y * coefficient,
                               torchWindVelocity.z * coefficient};
    return {std::isfinite(acceleration.x) ? acceleration.x : 0.0f,
            std::isfinite(acceleration.y) ? acceleration.y : 0.0f,
            std::isfinite(acceleration.z) ? acceleration.z : 0.0f};
}
