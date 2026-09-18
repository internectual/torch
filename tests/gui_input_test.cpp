#include "core/gui_input.h"

#include <cassert>
#include <cmath>

int main() {
    assert(nextGuiFocus(0, 3, false) == 1);
    assert(nextGuiFocus(0, 3, true) == 2);
    assert(nextGuiFocus(2, 3, false) == 0);
    assert(nextGuiFocus(3, 3, false) == 0);

    assert(std::abs(guiSliderValueAt(-10, 10, 100, 0, 10, 0) - 0) < 0.001f);
    assert(std::abs(guiSliderValueAt(60, 10, 100, 0, 10, 5) - 6) < 0.001f);
    assert(std::abs(guiSliderValueAt(200, 10, 100, 0, 10, 0) - 10) < 0.001f);

    assert(guiScrollAfterWheel(0, 500, 100, -1) == 30);
    assert(guiScrollAfterWheel(490, 500, 100, 1) == 400);
    assert(guiScrollAfterWheel(0, 50, 100, -1) == 0);

    GuiMouseCapture capture;
    capture.begin(1);
    assert(capture.active && capture.button == 1);
    capture.release();
    assert(!capture.active && capture.button == 0);
    return 0;
}
