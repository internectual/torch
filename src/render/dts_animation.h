#pragma once

#include "render/renderer.h"
#include <algorithm>

// Thread position is a sequence-local position plus elapsed local time. Keep
// the end marker authoritative, especially for reverse playback.
inline float dtsThreadTime(float position, float duration, float elapsed,
                           float timescale, bool atEnd, bool forward) {
    if (atEnd) return forward ? duration : 0.0f;
    return position * duration + elapsed * timescale;
}

struct DTSObjectSample {
    float vis = 1.0f;
    int32_t frameIndex = 0;
    int32_t matFrameIndex = 0;
};

inline DTSObjectSample sampleDTSObject(const std::vector<DTSShape::ObjectKeyframe>& keys,
                                       int32_t objectIndex, float time) {
    DTSObjectSample result;
    for (const auto& key : keys) {
        if (key.objectIndex != objectIndex || key.time > time) continue;
        result.vis = key.vis;
        result.frameIndex = key.frameIndex;
        result.matFrameIndex = key.matFrameIndex;
    }
    return result;
}
