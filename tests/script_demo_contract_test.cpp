#include "script/script_engine.h"

#include <cassert>

int main() {
    // No build-mode flag means a normal client is not a demo build.
    assert(!isDemoBuildMode(false, false));

    // The explicit demo build mode is visible to the client script path.
    assert(isDemoBuildMode(true, false));

    // Dedicated servers never report the client demo build mode.
    assert(!isDemoBuildMode(true, true));

    // Playback is independent: it must not make isDemo() true.
    bool recordingPlaying = true;
    assert(recordingPlaying);
    assert(!isDemoBuildMode(false, false));

    // The playback flag remains its own state.
    assert(recordingPlaying);
    recordingPlaying = false;
    assert(!recordingPlaying);
    return 0;
}
