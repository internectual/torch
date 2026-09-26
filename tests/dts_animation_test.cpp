#include "render/dts_animation.h"
#include "render/texture_frames.h"
#include "render/material_parity.h"
#include "game/animation_parity.h"
#include <cassert>
#include <cmath>
#include <limits>

static void testThreadTiming() {
    assert(dtsThreadTime(0.25f, 4.0f, 0.5f, 1.5f, false, true) == 1.75f);
    assert(dtsThreadTime(0.25f, 4.0f, 0.5f, -1.0f, false, false) == 0.5f);
    assert(dtsThreadTime(0.25f, 4.0f, 99.0f, -1.0f, true, false) == 0.0f);
    assert(dtsThreadTime(0.25f, 4.0f, 99.0f, 1.0f, true, true) == 4.0f);
}

static void testObjectSampling() {
    std::vector<DTSShape::ObjectKeyframe> keys = {
        {2, 0.0f, 0.0f, 1, 3}, {2, 1.0f, 1.0f, 4, 7},
    };
    const auto before = sampleDTSObject(keys, 2, 0.5f);
    assert(before.vis == 0.0f && before.frameIndex == 1 && before.matFrameIndex == 3);
    const auto after = sampleDTSObject(keys, 2, 1.0f);
    assert(after.vis == 1.0f && after.frameIndex == 4 && after.matFrameIndex == 7);
    assert(sampleDTSObject(keys, 9, 2.0f).vis == 1.0f);

    // Imported object sequences are not required to be sorted by timestamp.
    // Sampling must still use the most recent key, not the last stored key.
    const std::vector<DTSShape::ObjectKeyframe> unsorted = {
        {3, 1.5f, 0.0f, 6, 8}, {3, 0.5f, 1.0f, 2, 4},
    };
    const auto sampled = sampleDTSObject(unsorted, 3, 1.0f);
    assert(sampled.vis == 1.0f && sampled.frameIndex == 2 && sampled.matFrameIndex == 4);
}

static void testMountOrder() {
    MatrixF base, mount, point;
    base.setTranslation({10, 0, 0});
    mount.setTranslation({0, 2, 0});
    point.setTranslation({0, 0, 3});
    const Point3F p = (base * mount * point.inverse()).transform({0, 0, 3});
    assert(std::fabs(p.x - 10.0f) < 0.001f && std::fabs(p.y - 2.0f) < 0.001f);
    assert(std::fabs(p.z) < 0.001f);
}

static void testIFLParsingAndTiming() {
    const auto frames = parseTextureFrameSources(
        "  first.png 250\r\n"
        "; ignored.png 999\n"
        "second.png\t500 # trailing\n"
        "third.png 0 // default\n");
    assert(frames.size() == 3);
    // Counts are frames of 1/30 s; zero or missing is one frame.
    assert(frames[0].name == "first.png" && std::fabs(frames[0].duration - 250.0f / 30.0f) < 0.0001f);
    assert(frames[1].name == "second.png" && std::fabs(frames[1].duration - 500.0f / 30.0f) < 0.0001f);
    assert(frames[2].name == "third.png" && std::fabs(frames[2].duration - 1.0f / 30.0f) < 0.0001f);
    const auto tabbed = parseTextureFrameSources("mort000.png\t1\nmort001.png\n");
    assert(tabbed.size() == 2 && tabbed[0].name == "mort000.png" && tabbed[1].name == "mort001.png");
    assert(std::fabs(tabbed[1].duration - 1.0f / 30.0f) < 0.0001f);
    assert(textureFrameIndex({0.25f, 0.5f}, 2, 0.24f) == 0);
    assert(textureFrameIndex({0.25f, 0.5f}, 2, 0.25f) == 1);
    assert(textureFrameIndex({0.25f, 0.5f}, 2, 0.75f) == 0);
    assert(textureFrameIndex({}, 4, 2.0f) == 2);
    assert(textureFrameIndex({}, 4, 4.0f) == 0);
    assert(textureFrameIndex({0.0f, -1.0f}, 2, 1.1f) == 1);
    // Invalid clocks must not produce an undefined frame index during a
    // mission/demo transition.
    assert(textureFrameIndex({0.25f, 0.5f}, 2,
                             std::numeric_limits<float>::quiet_NaN()) == 0);
    assert(textureFrameIndex({}, 4, std::numeric_limits<float>::infinity()) == 0);
    // Shore textures use the same authored IFL timing as surface textures.
    assert(textureFrameIndex({0.1f, 0.2f}, 2, 0.1f) == 1);
}

static void testMaterialParity() {
    assert(materialAlphaTestThreshold(MatFlag_Translucent) == 0.5f);
    assert(materialAlphaTestThreshold(MatFlag_Additive) > 0.0f);
    assert(materialAlphaTestThreshold(MatFlag_None) == 0.0f);
    assert(std::abs(materialReflectionFactor(128) - 128.0f / 255.0f) < 0.0001f);
    assert(materialReflectionFactor(0) == 0.0f);
    assert(materialReflectionFactor(300) == 1.0f);
}

static void testInvalidAnimationClock() {
    assert(animationSampleTime(1.25f, 1.0f) == 0.25f);
    assert(animationSampleTime(1.25f, 1.0f, false) == 1.0f);
    assert(animationSampleTime(-0.25f, 1.0f, false) == 0.0f);
    assert(animationSampleTime(-0.25f, 1.0f) == 0.75f);
    assert(animationSampleTime(3.0f, 0.0f) == 0.0f);
    assert(animationSampleTime(3.0f, -1.0f) == 0.0f);
    assert(animationSampleTime(std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0.0f);
    assert(animationSampleTime(1.0f, std::numeric_limits<float>::infinity()) == 0.0f);
}

int main() {
    testThreadTiming();
    testObjectSampling();
    testMountOrder();
    testIFLParsingAndTiming();
    testMaterialParity();
    testInvalidAnimationClock();
    return 0;
}
