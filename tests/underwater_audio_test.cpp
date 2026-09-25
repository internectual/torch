#include "audio/audio_system.h"
#include <cassert>
#include <limits>

int main() {
    const Point3F validListener{1.0f, 2.0f, 3.0f};
    assert(sanitizeListenerVector(validListener).z == 3.0f);
    const Point3F invalidListener{1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
    const Point3F sanitized = sanitizeListenerVector(invalidListener);
    assert(sanitized.x == 0.0f && sanitized.y == 0.0f && sanitized.z == 0.0f);

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
