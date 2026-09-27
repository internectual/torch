#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>

// The client's cosmetic RNG isn't recorded; seed it from the event's stable
// identity, tick and Torque position (t2-mapper timelineRandom) so the same
// event looks the same on every replay and seek.
struct TimelineRandom {
    uint32_t state = 0;
    double next() {
        state = state * 1664525u + 1013904223u;
        return (double)state / 4294967296.0;
    }
    float operator()() { return (float)next(); }
    // Engine randI() % (2 v + 1) - v.
    int intInclusive(int variance) {
        if (variance <= 0) return 0;
        return (int)std::floor(next() * (2 * variance + 1)) - variance;
    }
};
inline TimelineRandom timelineRandom(std::initializer_list<double> values) {
    uint32_t state = 2166136261u;
    for (double value : values)
        state = (state ^ (uint32_t)(int64_t)std::trunc(value * 1000.0)) * 16777619u;
    return {state};
}
