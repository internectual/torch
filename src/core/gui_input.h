#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// GL-free input rules shared by the GUI implementation and deterministic tests.
inline std::size_t nextGuiFocus(std::size_t current, std::size_t count, bool backwards) {
    if (count == 0) return 0;
    if (current >= count) return backwards ? count - 1 : 0;
    return backwards ? (current == 0 ? count - 1 : current - 1)
                     : (current + 1) % count;
}

inline float guiSliderValueAt(float x, float start, float width,
                              float minimum, float maximum, int ticks) {
    if (width <= 0.0f) return minimum;
    const float normalized = std::clamp((x - start) / width, 0.0f, 1.0f);
    float value = minimum + (maximum - minimum) * normalized;
    if (ticks > 0 && maximum > minimum) {
        const float step = (maximum - minimum) / ticks;
        value = minimum + std::round((value - minimum) / step) * step;
    }
    return std::clamp(value, minimum, maximum);
}

inline float guiScrollAfterWheel(float scroll, float content, float viewport,
                                 int wheelDelta, float step = 30.0f) {
    const float maximum = std::max(content - viewport, 0.0f);
    return std::clamp(scroll + (wheelDelta < 0 ? step : -step), 0.0f, maximum);
}

struct GuiMouseCapture {
    int button = 0;
    bool active = false;

    void begin(int capturedButton) { button = capturedButton; active = true; }
    void release() { button = 0; active = false; }
};
