#pragma once

// SDL scancodes for the top-row weapon slots 1..9,0.
constexpr int weaponSlotForScancode(int scancode) {
    if (scancode >= 30 && scancode <= 38) return scancode - 30;
    return scancode == 39 ? 9 : -1;
}

constexpr bool observerCyclePressed(bool reload, bool altFire) {
    return reload || altFire;
}
