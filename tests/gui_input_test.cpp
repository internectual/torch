#include "core/gui_input.h"
#include "core/input_parity.h"
#include "core/joystick_input.h"
#include <limits>
#include "render/gui_renderer.h"

#include <cassert>
#include <cmath>

int main() {
    assert(GuiBitmapStatePolicy::horizontalStateCount("gui/shll_menuclose", 105, 29) == 3);
    assert(GuiBitmapStatePolicy::horizontalStateCount("gui/shll_soundbutton", 104, 27) == 4);
    const auto closeHoverSlice = GuiBitmapStatePolicy::horizontalSlice(105, 3, 1);
    assert(std::abs(closeHoverSlice.x - 35.5f) < 0.001f);
    assert(std::abs(closeHoverSlice.width - 34.0f) < 0.001f);

    assert(nextGuiFocus(0, 3, false) == 1);
    assert(nextGuiFocus(0, 3, true) == 2);
    assert(nextGuiFocus(2, 3, false) == 0);
    assert(nextGuiFocus(3, 3, false) == 0);

    assert(std::abs(guiSliderValueAt(-10, 10, 100, 0, 10, 0) - 0) < 0.001f);
    assert(std::abs(guiSliderValueAt(60, 10, 100, 0, 10, 5) - 6) < 0.001f);
    assert(std::abs(guiSliderValueAt(60, 10, 100, 10, 20, 2) - 15) < 0.001f);
    assert(std::abs(guiSliderValueAt(200, 10, 100, 0, 10, 0) - 10) < 0.001f);

    assert(guiScrollAfterWheel(0, 500, 100, -1) == 30);
    assert(guiScrollAfterWheel(490, 500, 100, 1) == 400);
    assert(guiScrollAfterWheel(0, 50, 100, -1) == 0);
    assert(guiScrollAfterWheel(120, 500, 100, 0) == 120);
    // Scrollbar page-track clicks use the side of the thumb: right/up
    // advances toward later content, while left/down moves toward the start.
    assert(guiScrollAfterPage(100, 500, 100, 1) == 180);
    assert(guiScrollAfterPage(100, 500, 100, -1) == 20);
    assert(guiScrollAfterPage(450, 500, 100, 1) == 400);
    assert(mouseWheelDirection(0.25f) == 1);
    assert(mouseWheelDirection(-0.25f) == -1);
    assert(mouseWheelDirection(0.0f) == 0);
    assert(mouseWheelDelta(3.0f) == 3);
    assert(mouseWheelDelta(-2.0f) == -2);
    assert(mouseWheelDelta(0.25f) == 1);
    assert(mouseWheelDelta(std::numeric_limits<float>::infinity()) == 0);
    assert(mouseWheelDelta(2147483648.0f) == std::numeric_limits<int>::max());
    assert(mouseWheelDelta(-2147483648.0f, true) == std::numeric_limits<int>::max());
    assert(mouseWheelDelta(-0.25f) == -1);
    assert(mouseWheelDelta(0.0f) == 0);
    assert(mouseWheelSteps(3) == 3);
    assert(mouseWheelSteps(-2) == 2);
    assert(mouseWheelSteps(0) == 0);
    assert(wheelAxisValue(1) == 1.0f);
    assert(wheelAxisValue(-2) == -2.0f);
    assert(wheelAxisValue(1, true) == -1.0f);
    assert(cycleIndexByWheel(4, 1, 63) == 5);
    assert(cycleIndexByWheel(0, -1, 63) == 62);
    assert(cycleIndexByWheel(62, 2, 63) == 1);
    assert(toggledActionState(true, false, false));
    assert(toggledActionState(false, true, true));
    assert(toggledActionState(false, false, true));
    assert(toggledActionState(true, true, true));
    assert(modifiedBindingDown(true, modifierShift, modifierShift));
    // Releasing Shift while the primary key remains held must emit cmdOff.
    assert(!modifiedBindingDown(true, modifierShift, 0));
    assert(modifiedBindingDown(true, 0, 0));
    assert(!modifiedBindingDown(true, 0, modifierShift));
    assert(modifiedBindingDown(true, modifierCtrl, modifierCtrl));
    assert(!modifiedBindingDown(true, 0, modifierCtrl));
    assert(bindingModifierMask("ctrl w") == modifierCtrl);
    assert(bindingModifierMask("shift numpad1") == modifierShift);
    assert(bindingModifierMask("ctrl shift k") == (modifierCtrl | modifierShift));

    assert(JoystickInput::axisIndex("xaxis") == 0);
    assert(JoystickInput::axisIndex("RYAXIS") == 4);
    assert(JoystickInput::axisIndex("slider1") == 7);
    assert(JoystickInput::axisIndex("button0") == -1);
    assert(JoystickInput::buttonIndex("button0") == 0);
    assert(JoystickInput::buttonIndex("BUTTON12") == 12);
    assert(JoystickInput::buttonIndex("buttonx") == -1);
    assert(std::abs(JoystickInput::normalizeAxis(-32768) + 1.0f) < 1e-6f);
    assert(std::abs(JoystickInput::normalizeAxis(32767) - 1.0f) < 1e-6f);
    assert(JoystickInput::applyAxisBinding(0.05f, false, true, -0.1f, 0.1f,
                                           false, 1.0f) == 0.0f);
    assert(std::abs(JoystickInput::applyAxisBinding(0.5f, true, true, -0.1f, 0.1f,
                                                    true, 0.5f) + 0.25f) < 1e-6f);
    assert(JoystickInput::povPressed(0x03, "upov"));
    assert(JoystickInput::povPressed(0x03, "uprightov"));
    assert(!JoystickInput::povPressed(0x01, "rpov"));

    GuiMouseCapture capture;
    capture.begin(1);
    assert(capture.active && capture.button == 1);
    capture.release();
    assert(!capture.active && capture.button == 0);

    GuiControl root, child, sibling;
    child.parent = &root;
    assert(root.owns(&root));
    assert(root.owns(&child));
    assert(!root.owns(&sibling));
    assert(!root.owns(nullptr));
    return 0;
}
