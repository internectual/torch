#include "audio/audio_system.h"
#include <cassert>

int main() {
    assert(AudioSystem::clampEnvironmentValue(-200000.0f) == -100000.0f);
    assert(AudioSystem::clampEnvironmentValue(200000.0f) == 100000.0f);
    assert(AudioSystem::clampEnvironmentValue(1.25f) == 1.25f);

    AudioEnvironmentState state;
    state.enabled = true;
    assert(state.enabled);
    assert(state.setFloat("decaytime", 4.0f));
    assert(state.floats.at("decaytime") == 4.0f);
    assert(state.setFloat("bounded", 200000.0f));
    assert(state.floats.at("bounded") == 100000.0f);
    assert(!state.setFloat("bad", __builtin_nanf("")));
    assert(!state.setInt("", 1));
    assert(state.enabled);
    state.clear();
    assert(state.floats.empty() && state.ints.empty());
    // Clearing a mission must also detach its environment, not just discard
    // the values that would be applied on the next update.
    assert(!state.enabled);
    assert(AudioSystem::selectWetEffect(false, 4, 9) == 4);
    assert(AudioSystem::selectWetEffect(true, 4, 9) == 9);
    assert(AudioSystem::selectWetEffect(true, 4, 0) == 4);
    assert(AudioSystem::occlusionGain(-1.0f) == 1.0f);
    assert(AudioSystem::occlusionGain(0.5f) == 0.625f);
    assert(AudioSystem::occlusionGain(2.0f) == 0.25f);
    SoundSource source;
    assert(source.volume == 1.0f && source.pitch == 1.0f && !source.looping);
    assert(source.referenceDistance == 1.0f && source.maxDistance == 100.0f);
    assert(source.relative && !source.positional);
    assert(source.velocity.x == 0.0f && source.velocity.y == 0.0f && source.velocity.z == 0.0f);
    assert(source.auxiliarySend == 0 && source.auxiliarySendIndex == 0 && source.auxiliaryFilter == 0);
    assert(SoundSource::clampRolloff(-1.0f) == 0.0f);
    assert(SoundSource::clampRolloff(1.5f) == 1.5f);
    assert(SoundSource::clampPitch(0.0f) == 0.01f);
    assert(SoundSource::clampPitch(1.5f) == 1.5f);
    return 0;
}
