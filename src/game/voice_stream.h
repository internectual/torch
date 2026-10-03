#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace VoiceStream {

constexpr size_t MaxCaptureSamples = 5 * 8000;

inline size_t acceptedCaptureSamples(size_t captured, size_t available) {
    return captured >= MaxCaptureSamples ? 0 :
           std::min(available, MaxCaptureSamples - captured);
}

// Sequence zero starts a talk burst, except when the 7-bit sequence wrapped
// after a long burst. Allow a small packet-loss window before the wrap.
inline bool startsNewBurst(bool playbackExists, uint8_t sequence, int lastSequence,
                           bool previousBurstFinished = false) {
    return !playbackExists || (sequence == 0 &&
           (previousBurstFinished || lastSequence < 120));
}

} // namespace VoiceStream
