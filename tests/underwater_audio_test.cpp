#include "audio/audio_system.h"
#include <cassert>

int main() {
    const float drySource = 1.0f;
    const float wetSource = drySource * AudioSystem::underwaterSourceGain();
    const float wetListener = AudioSystem::underwaterListenerGain();
    const float wetHighs = AudioSystem::underwaterHighFrequencyGain();

    assert(wetSource > 0.0f && wetSource < drySource);
    assert(wetListener > 0.0f && wetListener < 1.0f);
    assert(wetHighs > 0.0f && wetHighs < 1.0f);
    assert(AudioSystem::underwaterSourceGain() < 1.0f);
    assert(AudioSystem::underwaterListenerGain() < 1.0f);
    assert(AudioSystem::underwaterHighFrequencyGain() < 1.0f);
    return 0;
}
