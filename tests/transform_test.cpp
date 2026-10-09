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

    // Axis-angle input is normalized, while a zero/non-finite axis is the
    // deterministic identity instead of a scaled or NaN matrix.
    MatrixF zeroAxis;
    zeroAxis.setRotationAxis({0, 0, 0}, Math::PI * 0.5f);
    assertPoint(zeroAxis.transform({1, 2, 3}), {1, 2, 3});
    MatrixF scaledAxis;
    scaledAxis.setRotationAxis({0, 0, 2}, Math::PI * 0.5f);
    assertPoint(scaledAxis.transform({1, 0, 0}), {0, 1, 0});

    // ParticleEmissionDummy emits along Torque +Z, which is engine +Y.
    const MatrixF dummyRotation = Math::torqueRotationToYUp({0, 0, 1}, 0.0f);
    assertPoint(dummyRotation.transformNormal(Math::torquePointToYUp({0, 0, 1})),
                {0, 1, 0});

    // The native Camera uses local +Y as forward.
    assertPoint(Math::torqueCameraForwardToYUp({0, 0, 1}, 0.0f),
                {0, 0, -1});
    // Mission rotations are clockwise: a 90 degree yaw faces Torque +X.
    assertPoint(Math::torqueCameraForwardToYUp({0, 0, 1}, Math::PI * 0.5f),
                {1, 0, 0});
    // Matches the object/mapper-camera convention.
    assertPoint(Math::torqueRotationToYUp({0, 0, 1}, -Math::PI * 0.5f).transformNormal({0, 0, -1}),
                {1, 0, 0});

    // Player body yaw (MatrixF::set(EulerF(0, 0, yaw))) turns forward +Y
    // toward +X about Torque +Z; as a quaternion it stays upright in Y-up.
    {
        const float yaw = 1.0f;
        const QuatF bodyYaw{0, 0, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
        const MatrixF yUp = Math::torqueQuaternionToYUp(bodyYaw);
        assertPoint(yUp.transformNormal({0, 1, 0}), {0, 1, 0});
        assertPoint(yUp.transformNormal(Math::torquePointToYUp({0, 1, 0})),
                    Math::torquePointToYUp({std::sin(yaw), std::cos(yaw), 0}));
    }

    // A networked QuatF (QuatF::set of the object's matrix) turns like the
    // mission rotation it came from: AngAxisF(+Z, 90) faces Torque +X.
    {
        const float half = Math::PI * 0.25f;
        const QuatF networked{0, 0, std::sin(half), std::cos(half)};
        assertPoint(Math::torqueQuaternionMatrix(networked).transformNormal({0, 1, 0}), {1, 0, 0});
        assertPoint(Math::torqueQuaternionToYUp(networked).transformNormal(Math::torquePointToYUp({0, 1, 0})),
                    Math::torqueRotationToYUp({0, 0, 1}, -Math::PI * 0.5f).transformNormal(Math::torquePointToYUp({0, 1, 0})));
    }

    const QuatF source = {0.2f, -0.3f, 0.4f, 0.8f};
    const QuatF roundTrip = QuatF::fromMatrix(source.toMatrix());
    assert(close(roundTrip.x * roundTrip.x + roundTrip.y * roundTrip.y +
                 roundTrip.z * roundTrip.z + roundTrip.w * roundTrip.w, 1.0f));
    assertPoint(roundTrip.toMatrix().transform({1, 2, 3}),
                source.toMatrix().transform({1, 2, 3}));
    assertPoint(QuatF{0, 0, 0, 0}.toMatrix().transform({4, 5, 6}), {4, 5, 6});
    MatrixF malformed;
    malformed.m[0][0] = NAN;
    assertPoint(malformed.inverse().transform({4, 5, 6}), {4, 5, 6});
    MatrixF singular;
    singular.setScale({1, 0, 1});
    assertPoint(singular.inverse().transform({4, 5, 6}), {4, 5, 6});

    // A transient invalid observer/demo camera must not turn the view basis
    // into NaNs and make the whole frame disappear.
    MatrixF invalidView;
    invalidView.lookAt({NAN, 0, 0}, {0, 0, 0}, {0, 1, 0});
    assertPoint(invalidView.transform({4, 5, 6}), {4, 5, 6});
}
