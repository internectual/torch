#pragma once

#include <cstdint>

// ShapeBase::unpackUpdate's SoundMask restarts a slot's sound on every write
// (updateAudioState), even when the profile is unchanged. Each write takes a
// revision from one process-wide counter: snapshot restores bring back older
// revisions, which must never look newer than a write made after the seek.
inline uint32_t nextSoundThreadRevision() {
    static uint32_t revision = 0;
    return ++revision;
}
