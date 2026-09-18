#pragma once

#include <algorithm>
#include <cmath>

struct GuiViewport {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
    float scale = 1.0f;
    float logicalWidth = 1.0f;
    float logicalHeight = 1.0f;
};

// GUI coordinates are authored in logical units. Gameplay uses the stock
// 640x480 canvas; shell screens use the current window's logical size.
inline GuiViewport guiViewport(int drawableWidth, int drawableHeight,
                               float logicalWidth, float logicalHeight) {
    const int dw = std::max(1, drawableWidth);
    const int dh = std::max(1, drawableHeight);
    const float lw = std::max(1.0f, logicalWidth);
    const float lh = std::max(1.0f, logicalHeight);
    const float scale = std::min(dw / lw, dh / lh);
    const int vw = std::max(1, (int)std::lround(lw * scale));
    const int vh = std::max(1, (int)std::lround(lh * scale));
    return {(dw - vw) / 2, (dh - vh) / 2, vw, vh, scale, lw, lh};
}

inline bool guiPhysicalToLogical(const GuiViewport& viewport,
                                 float physicalX, float physicalY,
                                 float& logicalX, float& logicalY) {
    if (viewport.scale <= 0.0f || physicalX < viewport.x || physicalY < viewport.y ||
        physicalX >= viewport.x + viewport.width || physicalY >= viewport.y + viewport.height)
        return false;
    logicalX = (physicalX - viewport.x) / viewport.scale;
    logicalY = (physicalY - viewport.y) / viewport.scale;
    return logicalX >= 0.0f && logicalX < viewport.logicalWidth &&
           logicalY >= 0.0f && logicalY < viewport.logicalHeight;
}
