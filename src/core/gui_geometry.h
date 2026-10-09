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

// GUI coordinates are logical units: the canvas is the window's logical
// size, scaled to the drawable on high-DPI displays.
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

// A logical-unit rect (top-left origin) as a glScissor box in drawable pixels
// (bottom-left origin) inside the viewport.
inline void guiLogicalToScissor(const GuiViewport& viewport, int drawableHeight,
                                float x, float y, float w, float h,
                                int& sx, int& sy, int& sw, int& sh) {
    sx = viewport.x + (int)std::lround(x * viewport.scale);
    sw = std::max(0, (int)std::lround(w * viewport.scale));
    sh = std::max(0, (int)std::lround(h * viewport.scale));
    const int top = viewport.y + (int)std::lround(y * viewport.scale);
    sy = drawableHeight - top - sh;
}
