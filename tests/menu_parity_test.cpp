#include "game/menu_parity.h"
#include "game/hud_parity.h"

#include <cassert>

int main() {
    assert(mainMenuItemCount() == 6);
    assert(mainMenuShowsCredits(4));
    assert(!mainMenuShowsCredits(5));
    assert(mainMenuShowsQuit(5));
    assert(!mainMenuShowsQuit(4));
    bool previousMouse = false;
    assert(menuButtonPressed(true, previousMouse));
    assert(!menuButtonPressed(true, previousMouse));
    assert(!menuButtonPressed(false, previousMouse));
    previousMouse = false;
    assert(menuButtonPressed(true, previousMouse));
    assert(serverBrowserRowAt(20, 80, 20, 780, 80, 30, 3) == 0);
    assert(serverBrowserRowAt(100, 109.9f, 20, 780, 80, 30, 3) == 0);
    assert(serverBrowserRowAt(100, 110, 20, 780, 80, 30, 3) == 1);
    assert(serverBrowserRowAt(780, 139, 20, 780, 80, 30, 3) == 1);
    assert(serverBrowserRowAt(19.9f, 100, 20, 780, 80, 30, 3) == -1);
    assert(serverBrowserRowAt(100, 170, 20, 780, 80, 30, 3) == -1);
    assert(serverBrowserUsesMaster(true, "https://master.example/list"));
    assert(!serverBrowserUsesMaster(true, ""));
    assert(!serverBrowserUsesMaster(false, "https://master.example/list"));
    assert(HudParity::scoreboardPlayerBefore(1, 10, 4, "Bravo",
                                             2, 100, 1, "Alpha"));
    assert(HudParity::scoreboardPlayerBefore(1, 100, 4, "Bravo",
                                             1, 10, 1, "Alpha"));
    assert(HudParity::scoreboardPlayerBefore(1, 10, 2, "Zulu",
                                             1, 10, 3, "Alpha"));
    assert(!HudParity::scoreboardPlayerBefore(1, 10, 3, "Alpha",
                                               1, 10, 2, "Zulu"));
    // Live observer telemetry is stored in maps, but the native scoreboard
    // still ranks rows by team and score rather than map iteration order.
    assert(HudParity::scoreboardPlayerBefore(1, 20, 7, "Bravo",
                                               2, 1, 1, "Alpha"));
    return 0;
}
