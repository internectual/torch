#include "game/projectile_audio.h"
#include <cassert>

int main() {
    assert(ProjectileAudio::fireProfile(false, 4, 8) == 4);
    assert(ProjectileAudio::fireProfile(true, 4, 8) == 8);
    assert(ProjectileAudio::fireProfile(true, 4, 0) == 4);
    assert(ProjectileAudio::explosionProfile(true, 10, 11) == 11);
    assert(ProjectileAudio::attenuation(1.0f, 2.0f, 20.0f) == 1.0f);
    assert(ProjectileAudio::attenuation(4.0f, 2.0f, 20.0f) > 0.49f);
    assert(ProjectileAudio::attenuation(20.0f, 2.0f, 20.0f) == 0.0f);
    return 0;
}
