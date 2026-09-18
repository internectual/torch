#include "audio/audio_system.h"
#include <cassert>
#include <vector>

int main() {
    using AudioSourcePolicy::Candidate;
    std::vector<Candidate> candidates = {
        {1, false, false, 10},
        {1, false, false, 4},
        {0, true, false, 1},
        {0, false, true, 2},
    };
    assert(AudioSourcePolicy::selectSteal(candidates, 2) == 1);
    assert(AudioSourcePolicy::selectSteal(candidates, 1) == -1);
    assert(AudioSourcePolicy::selectSteal(candidates, 0) == -1);

    candidates[1].priority = 0;
    assert(AudioSourcePolicy::selectSteal(candidates, 1) == 1);
    candidates[1].serial = 20;
    candidates[0].priority = 0;
    assert(AudioSourcePolicy::selectSteal(candidates, 1) == 0);

    uint32_t first = 7;
    uint32_t second = 7;
    for (int i = 0; i < 8; ++i)
        assert(AudioSourcePolicy::nextLoopGapMs(first, 10, 20) ==
               AudioSourcePolicy::nextLoopGapMs(second, 10, 20));
    assert(AudioSourcePolicy::nextLoopGapMs(first, 25, 25) == 25);
    assert(AudioSourcePolicy::nextLoopGapMs(first, -5, -1) == 0);

    SoundSource source;
    assert(source.priority == 0 && source.serial == 0);
    assert(source.environmentSend == 0);
    assert(source.offsetSeconds == 0.0f);
    assert(!source.persistent && !source.paused);
    assert(SoundSource::clampOffsetSeconds(-1.0f, 1000) == 0.0f);
    assert(SoundSource::clampOffsetSeconds(0.25f, 1000) == 0.25f);
    assert(SoundSource::clampOffsetSeconds(2.0f, 1000) == 1.0f);
    assert(SoundSource::clampOffsetSeconds(2.0f, 0) == 2.0f);
    return 0;
}
