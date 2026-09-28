#pragma once
// TSShapeInstance trigger states: crossing a sequence trigger while a thread
// plays sets (on) or clears its state bits; reverse play flips triggers
// flagged InvertOnReverse. Positions are sequence phases; for cyclic
// sequences `from`/`to` may be unwrapped (whole loops).
#include "render/renderer.h"
#include <cmath>
#include <vector>

namespace DTSTriggers {

inline uint32_t advance(const std::vector<DTSShape::Animation::Trigger>& triggers, float from, float to,
                        bool cyclic, uint32_t state) {
    if (from == to || !std::isfinite(from) || !std::isfinite(to)) return state;
    auto activate = [&](float a, float b) {
        const bool forward = a <= b;
        const int n = (int)triggers.size();
        for (int k = 0; k < n; ++k) {
            const auto& trigger = triggers[forward ? k : n - 1 - k];
            if (trigger.position < std::min(a, b) || trigger.position >= std::max(a, b)) continue;
            bool on = (trigger.state & 0x80000000u) != 0;
            if (!forward && (trigger.state & 0x40000000u)) on = !on;
            const uint32_t mask = trigger.state & 0x1f;
            state = on ? (state | mask) : (state & ~mask);
        }
    };
    const int loops = cyclic ? (int)std::floor(to) - (int)std::floor(from) : 0;
    const float a = cyclic ? from - std::floor(from) : from;
    const float b = cyclic ? to - std::floor(to) : to;
    if (!loops) activate(a, b);
    else if (loops > 0) {
        activate(loops == 1 ? a : b, 1.0f);
        activate(0.0f, b);
    } else {
        activate(loops == -1 ? a : b, 0.0f);
        activate(1.0f, b);
    }
    return state;
}

} // namespace DTSTriggers
