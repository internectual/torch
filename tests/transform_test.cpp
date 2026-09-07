#include "core/math.h"

#include <cassert>
#include <cmath>

static bool close(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

static void assertPoint(const Point3F& actual, const Point3F& expected) {
    assert(close(actual.x, expected.x));
    assert(close(actual.y, expected.y));
    assert(close(actual.z, expected.z));
}

int main() {
    const MatrixF basis = Math::czUpToYUp();
    assertPoint(basis.transform({1, 2, 3}), {1, 3, -2});
    assertPoint(Math::torquePointToYUp({1, 2, 3}), {1, 3, -2});
    const MatrixF convertedScale = Math::torqueScaleToYUp({2, 3, 4});
    assertPoint(convertedScale.transform({1, 1, 1}), {2, 4, 3});

    // A Torque-frame rotation about its up axis must become a rotation about
    // the engine's negative Z axis after the basis change.
    const MatrixF converted = Math::torqueRotationToYUp(
        {0, 0, 1}, Math::PI * 0.5f);
    assertPoint(converted.transform({1, 0, 0}), {0, 0, -1});

    // Conjugation must preserve the identity rotation for every axis.
    const MatrixF identity = Math::torqueRotationToYUp({1, 2, 3}, 0.0f);
    assertPoint(identity.transform({4, 5, 6}), {4, 5, 6});
}
