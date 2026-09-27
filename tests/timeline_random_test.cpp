#include "game/timeline_random.h"
#include <cassert>
#include <cmath>

int main() {
    // Reference values from t2-mapper's timelineRandom (stream/timelineRandom.ts).
    TimelineRandom random = timelineRandom({42, 1234, -512.25, 300.5, 87.125});
    assert(std::fabs(random.next() - 0.7079560917336494) < 1e-12);
    assert(std::fabs(random.next() - 0.8496609255671501) < 1e-12);
    assert(std::fabs(random.next() - 0.0881976333912462) < 1e-12);
    // randomIntInclusive: floor(random x (2v + 1)) - v.
    assert(random.intInclusive(160) == -32);
    assert(random.intInclusive(0) == 0);
    return 0;
}
