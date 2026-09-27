#pragma once
// ShapeBase thread timing on the client (ShapeBase::unpackUpdate, as ported
// by t2-mapper stream/shapeThreads.ts). A thread keeps its position when an
// update names the same sequence, unless the update Stops it (position 0) or
// Plays it at its end (snap to the end in its direction). Only a playing
// thread that is not at its end advances, forward or backward at 1x, looping
// for cyclic sequences and clamping otherwise.
#include <algorithm>
#include <cmath>

namespace ShapeThreads {

enum State { Play = 0, Stop = 1, Pause = 2, Destroy = 3 };

struct Update {
    int sequence = -1;
    int state = Play;
    bool forward = true;
    bool atEnd = false;
};

struct Clock {
    bool valid = false;
    Update current;
    float anchorTime = 0.0f;   // when the current update took effect
    float anchorPos = 0.0f;    // thread time (seconds) at anchorTime
};

inline float advance(float time, float elapsed, const Update& thread, float duration, bool cyclic) {
    if (thread.state != Play || thread.atEnd) return time;
    time += std::max(0.0f, elapsed) * (thread.forward ? 1.0f : -1.0f);
    if (cyclic) {
        time = std::fmod(time, duration);
        if (time < 0.0f) time += duration;
        return time;
    }
    return std::clamp(time, 0.0f, duration);
}

// The thread's time in seconds at `now`, taking `incoming` as its latest
// networked state.
inline float timeAt(Clock& clock, const Update& incoming, float now, float duration, bool cyclic) {
    if (!(duration > 0.0f)) return 0.0f;
    const bool same = clock.valid && clock.current.sequence == incoming.sequence &&
                      clock.current.state == incoming.state &&
                      clock.current.forward == incoming.forward &&
                      clock.current.atEnd == incoming.atEnd;
    if (!same) {
        const bool preserves = clock.valid && clock.current.sequence == incoming.sequence &&
                               incoming.state != Stop && !(incoming.state == Play && incoming.atEnd);
        float time = preserves
            ? advance(clock.anchorPos, now - clock.anchorTime, clock.current, duration, cyclic) : 0.0f;
        if (incoming.state == Stop) time = 0.0f;
        else if (incoming.state == Play && incoming.atEnd) time = incoming.forward ? duration : 0.0f;
        clock.valid = true;
        clock.current = incoming;
        clock.anchorTime = now;
        clock.anchorPos = time;
    }
    return advance(clock.anchorPos, now - clock.anchorTime, clock.current, duration, cyclic);
}

} // namespace ShapeThreads
