#pragma once

#include <algorithm>
#include <cmath>

// Animation clocks are presentation state. Invalid or reverse frame deltas
// must not poison the clock or make a model play backwards.
inline float animationDelta(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return 0.0f;
    return dt;
}

// Invalid authored durations must not turn a presentation clock into NaN.
// Non-looping sequences hold their final frame instead of wrapping back to
// their first frame when the presentation clock outlives the sequence.
inline float animationSampleTime(float time, float duration, bool looping = true) {
    if (!std::isfinite(time) || !std::isfinite(duration) || duration <= 0.0f)
        return 0.0f;
    if (!looping) return std::clamp(time, 0.0f, duration);
    float wrapped = std::fmod(time, duration);
    if (!std::isfinite(wrapped)) return 0.0f;
    // fmod keeps the sign of time. Reverse demo playback must continue from
    // the end of the loop rather than pinning the model to its first frame.
    if (wrapped < 0.0f) wrapped += duration;
    return wrapped;
}
