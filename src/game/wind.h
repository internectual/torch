#pragma once

#include "core/math.h"

// Torque's particle and precipitation systems share one client wind vector.
inline Point3F torchWindVelocity{};

inline void setTorchWindVelocity(const Point3F& velocity) {
    torchWindVelocity = velocity;
}

inline Point3F getTorchWindVelocity() {
    return torchWindVelocity;
}

inline Point3F torchWindVelocityToTorque() {
    return {torchWindVelocity.x, -torchWindVelocity.z, torchWindVelocity.y};
}

inline Point3F windAcceleration(float coefficient) {
    return {torchWindVelocity.x * coefficient,
            torchWindVelocity.y * coefficient,
            torchWindVelocity.z * coefficient};
}
