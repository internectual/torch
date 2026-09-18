#include "game/wind.h"
#include <cassert>
#include <cmath>

static bool close(float a, float b) { return std::fabs(a - b) < 0.00001f; }

int main() {
    setTorchWindVelocity({3.0f, -2.0f, 4.0f});
    const Point3F acceleration = windAcceleration(0.5f);
    assert(close(acceleration.x, 1.5f));
    assert(close(acceleration.y, -1.0f));
    assert(close(acceleration.z, 2.0f));
    setTorchWindVelocity({});
    const Point3F zero = windAcceleration(-1.0f);
    assert(close(zero.x, 0.0f) && close(zero.y, 0.0f) && close(zero.z, 0.0f));
    return 0;
}
