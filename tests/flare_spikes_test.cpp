#include "game/flare_spikes.h"
#include <cassert>
#include <cmath>

int main() {
    float seed = 0.0f;
    auto random = [&]() { seed = std::fmod(seed + 0.618034f, 1.0f); return seed; };
    const std::array<float, 3> sizes{0.2f, 0.4f, 0.6f};
    std::vector<FlareSpikes::Spike> spikes;
    for (int i = 0; i < 3; ++i) spikes.push_back(FlareSpikes::spawn(sizes, random));
    for (const auto& s : spikes) {
        const float len = std::sqrt(s.dir[0] * s.dir[0] + s.dir[1] * s.dir[1] + s.dir[2] * s.dir[2]);
        assert(std::fabs(len - 1.0f) < 1e-4f);
        assert(s.lifetimeSec >= 0.4f && s.lifetimeSec <= 0.995f);
        assert(s.growSec >= 0.15f && s.growSec <= 0.375f);
        assert(s.baseScale == 0.2f && s.tip0 >= 0.4f && s.tip1 >= 0.6f);
    }
    // Four fans x four triangles x three vertices per spike.
    const auto tris = FlareSpikes::triangles(spikes);
    assert(tris.size() == spikes.size() * 48);
    // A fresh spike starts dark and brightens while its tip grows.
    assert(tris[0].shade == 0.0f);
    FlareSpikes::advance(spikes, 0.1f, sizes, random);
    assert(FlareSpikes::triangles(spikes)[0].shade > 0.0f);
    // Past its lifetime a spike respawns.
    FlareSpikes::advance(spikes, 2.0f, sizes, random);
    for (const auto& s : spikes) assert(s.ageSec == 0.0f);
    return 0;
}
