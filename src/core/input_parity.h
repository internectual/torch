#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

// SDL mouse button values are 1-based.  Keep these separate from array
// bounds so gameplay code does not accidentally treat slot zero as left click.
constexpr int mouseLeftButton = 1;
constexpr int mouseMiddleButton = 2;
constexpr int mouseRightButton = 3;

// SDL3 reports high-resolution wheel motion as fractional ticks. Torque's
// input map consumes the direction of a wheel event, so precise motion must
// not disappear during integer conversion.
constexpr int mouseWheelDirection(float delta) {
    return delta > 0.0f ? 1 : (delta < 0.0f ? -1 : 0);
}

// SDL may coalesce multiple notches before the frame is processed. Preserve
// the count for actions such as Tribes 2 weapon cycling.
constexpr int mouseWheelSteps(int delta) {
    // Avoid signed overflow for a malformed/extreme caller value.
    if (delta == std::numeric_limits<int>::min())
        return 0;
    return delta < 0 ? -delta : delta;
}

// SDL can report several whole notches in one event, or a fractional
// high-resolution notch. Preserve whole-notch magnitude and make fractional
// motion usable by emitting one directional step.
constexpr int mouseWheelDelta(float delta, bool flipped = false) {
    // SDL can deliver malformed or very large accumulated deltas.  Avoid the
    // undefined float-to-int conversion path while retaining a usable wheel
    // direction for weapon cycling.
    if (!std::isfinite(delta)) return 0;
    constexpr float minIntMagnitude = 2147483648.0f;
    if (delta > 0.0f) {
        const int whole = delta >= minIntMagnitude
            ? std::numeric_limits<int>::max() : static_cast<int>(delta);
        const int result = whole > 0 ? whole : 1;
        return flipped ? -result : result;
    }
    if (delta < 0.0f) {
        const float magnitude = -delta;
        const int whole = magnitude >= minIntMagnitude
            ? std::numeric_limits<int>::max() : static_cast<int>(magnitude);
        const int result = whole > 0 ? -whole : -1;
        return flipped ? -result : result;
    }
    return 0;
}

// Wheel input is a frame-local signed delta, not a persistent position.
constexpr int cycleIndexByWheel(int current, int wheelDelta, int count) {
    if (count <= 0) return current;
    const long long wrapped = static_cast<long long>(current) + wheelDelta;
    const long long remainder = wrapped % count;
    return static_cast<int>(remainder < 0 ? remainder + count : remainder);
}

// SDL scancodes for the top-row weapon slots 1..9,0.
constexpr int weaponSlotForScancode(int scancode) {
    if (scancode >= 30 && scancode <= 38) return scancode - 30;
    return scancode == 39 ? 9 : -1;
}

constexpr bool observerCyclePressed(bool reload, bool altFire) {
    return reload || altFire;
}

// Actions such as reload are edge-triggered in the native input map.
constexpr bool buttonPressed(bool down, bool wasDown) {
    return down && !wasDown;
}

constexpr uint8_t modifierShift = 1u << 0;
constexpr uint8_t modifierCtrl = 1u << 1;
constexpr uint8_t modifierAlt = 1u << 2;

constexpr uint8_t bindingModifierMask(std::string_view binding) {
    uint8_t mask = 0;
    std::size_t begin = 0;
    while (begin < binding.size()) {
        const std::size_t end = binding.find(' ', begin);
        const auto token = binding.substr(begin,
            end == std::string_view::npos ? binding.size() - begin : end - begin);
        if (token == "shift") mask |= modifierShift;
        else if (token == "ctrl" || token == "control") mask |= modifierCtrl;
        else if (token == "alt") mask |= modifierAlt;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return mask;
}

// A binding fires only for its exact modifier chord. This prevents a plain
// key binding from also firing when a Ctrl/Shift/Alt variant is held.
constexpr bool modifiedBindingDown(bool keyDown, uint8_t requiredModifiers,
                                    uint8_t activeModifiers) {
    return keyDown && requiredModifiers == activeModifiers;
}

// Tribes 2's toggleZoom action latches on the press edge instead of treating
// the bound key or mouse button as a hold.
constexpr bool toggledActionState(bool down, bool wasDown, bool active) {
    return buttonPressed(down, wasDown) ? !active : active;
}

// Tribes 2 permits looking almost straight up or down. Keep this shared by
// local input and physics so camera aim does not change at the prediction
// boundary.
constexpr float cameraPitchLimit = 1.5f;

constexpr float clampCameraPitch(float pitch) {
    return pitch < -cameraPitchLimit ? -cameraPitchLimit :
        (pitch > cameraPitchLimit ? cameraPitchLimit : pitch);
}

// Free-camera yaw uses the same mouse convention as the player camera.
constexpr float freeCameraYaw(float yaw, float mouseDelta) {
    return yaw + mouseDelta;
}

// Free-camera input is a digital 3D direction. Normalize simultaneous
// forward/strafe/elevation inputs so diagonal movement keeps native speed.
constexpr float freeCameraMoveScale(bool forward, bool backward,
                                    bool left, bool right,
                                    bool up, bool down) {
    const int x = (right ? 1 : 0) - (left ? 1 : 0);
    const int y = (up ? 1 : 0) - (down ? 1 : 0);
    const int z = (forward ? 1 : 0) - (backward ? 1 : 0);
    const int lengthSquared = x * x + y * y + z * z;
    if (lengthSquared == 2) return 0.7071067812f;
    if (lengthSquared == 3) return 0.5773502692f;
    return 1.0f;
}
