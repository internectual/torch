#pragma once

#include "render/renderer.h"
#include <algorithm>
#include <cmath>
#include <limits>

// Thread position is a sequence-local position plus elapsed local time. Keep
// the end marker authoritative, especially for reverse playback.
inline float dtsThreadTime(float position, float duration, float elapsed,
                           float timescale, bool atEnd, bool forward) {
    if (!std::isfinite(duration) || duration <= 0.0f) return 0.0f;
    if (atEnd) return forward ? duration : 0.0f;
    if (!std::isfinite(position)) position = 0.0f;
    if (!std::isfinite(elapsed)) elapsed = 0.0f;
    if (!std::isfinite(timescale)) timescale = 0.0f;
    const float time = std::clamp(position, 0.0f, 1.0f) * duration +
        elapsed * timescale;
    return std::clamp(time, 0.0f, duration);
}

struct DTSObjectSample {
    float vis = 1.0f;
    int32_t frameIndex = 0;
    int32_t matFrameIndex = 0;
};

inline DTSObjectSample sampleDTSObject(const std::vector<DTSShape::ObjectKeyframe>& keys,
                                       int32_t objectIndex, float time) {
    DTSObjectSample result;
    float sampledTime = -std::numeric_limits<float>::infinity();
    for (const auto& key : keys) {
        if (key.objectIndex != objectIndex || key.time > time ||
            key.time < sampledTime) continue;
        // Object keyframes normally arrive sorted, but imported DTS files and
        // generated shapes do not all preserve that order. Select the latest
        // key at or before the sample time instead of trusting vector order.
        sampledTime = key.time;
        result.vis = key.vis;
        result.frameIndex = key.frameIndex;
        result.matFrameIndex = key.matFrameIndex;
    }
    return result;
}
