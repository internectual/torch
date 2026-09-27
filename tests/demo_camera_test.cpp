#include "game/demo.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    // Camera::processTick clamps pitch to 1.3962; players to 0.494 pi.
    float yaw = 0.0f, pitch = 0.0f;
    T2Demo::accumulateViewMove(yaw, pitch, 0.0f, 3.0f, T2Demo::CameraMaxViewPitch);
    assert(near(pitch, 1.3962f));
    pitch = 0.0f;
    T2Demo::accumulateViewMove(yaw, pitch, 0.0f, 3.0f);
    assert(near(pitch, 3.14159265f * 0.494f));

    // validateEyePoint: max - min when the ray is clear or grazes a surface,
    // else pulled in to the hit less CameraRadius / dot, within [0, max - min].
    assert(near(T2Demo::orbitEyeDistance(0.5f, 4.5f, false, 0.0f, 0.0f), 4.0f));
    assert(near(T2Demo::orbitEyeDistance(0.5f, 4.5f, true, 2.0f, 0.005f), 4.0f));
    assert(near(T2Demo::orbitEyeDistance(0.5f, 4.5f, true, 2.0f, 0.5f), 1.9f));
    assert(near(T2Demo::orbitEyeDistance(0.5f, 4.5f, true, 9.0f, 1.0f), 4.0f));
    assert(near(T2Demo::orbitEyeDistance(0.5f, 4.5f, true, 0.01f, 1.0f), 0.0f));
    return 0;
}
