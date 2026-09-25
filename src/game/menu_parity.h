#pragma once

#include <string_view>

#include <cmath>

inline int mainMenuItemCount() {
    return 6;
}

// Demo builds use their configured master endpoint when one is present; an
// empty endpoint intentionally retains the stock LAN-discovery fallback.
inline bool serverBrowserUsesMaster(bool demoMode, std::string_view masterUrl) {
    return demoMode && !masterUrl.empty();
}

inline bool mainMenuShowsCredits(int selection) {
    return selection == 4;
}

inline bool mainMenuShowsQuit(int selection) {
    return selection == 5;
}

inline bool menuButtonPressed(bool down, bool& previousDown) {
    const bool pressed = down && !previousDown;
    previousDown = down;
    return pressed;
}

// Keep browser hit testing aligned with the displayed rows.
inline int serverBrowserRowAt(float x, float y, float left, float right,
                              float top, float rowHeight, int count) {
    if (count <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(rowHeight) ||
        rowHeight <= 0.0f || x < left || x > right || y < top)
        return -1;
    const int row = static_cast<int>((y - top) / rowHeight);
    return row >= 0 && row < count ? row : -1;
}
