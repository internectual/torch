#include "core/gui_geometry.h"

#include <cassert>
#include <cmath>

int main() {
    const GuiViewport wide = guiViewport(1920, 1080, 640, 480);
    assert(wide.x == 240 && wide.y == 0 && wide.width == 1440 && wide.height == 1080);
    float x = 0, y = 0;
    assert(guiPhysicalToLogical(wide, 240, 0, x, y) && x == 0 && y == 0);
    assert(guiPhysicalToLogical(wide, 1680 - 1, 1079, x, y));
    assert(x > 639.0f && y > 479.0f - 1.0f);
    assert(!guiPhysicalToLogical(wide, 100, 100, x, y));

    const GuiViewport dpi = guiViewport(2560, 1440, 1280, 720);
    assert(std::abs(dpi.scale - 2.0f) < 0.001f);
    assert(guiPhysicalToLogical(dpi, 1280, 720, x, y));
    assert(std::abs(x - 640.0f) < 0.001f && std::abs(y - 360.0f) < 0.001f);

    const GuiViewport invalid = guiViewport(0, -1, 0, -1);
    assert(invalid.width == 1 && invalid.height == 1 && invalid.scale > 0.0f);
    return 0;
}
