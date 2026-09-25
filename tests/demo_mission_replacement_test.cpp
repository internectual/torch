#include "game/demo.h"

#include <cassert>

int main() {
    T2Demo::MissionReplacementState state{"initial", {}};
    assert(!state.defer("initial"));
    assert(state.pendingMission.empty());

    assert(state.defer("later"));
    assert(state.loadedMission == "initial");
    assert(state.pendingMission == "later");
    assert(!state.commit("other"));
    assert(state.pendingMission == "later");

    assert(state.defer("later"));
    assert(state.commit("later"));
    assert(state.loadedMission == "later");
    assert(state.pendingMission.empty());

    assert(state.defer("newer"));
    assert(state.defer("latest"));
    assert(state.pendingMission == "latest");
    assert(state.commit("latest"));
    assert(!state.commit("unrelated"));
    assert(state.loadedMission == "latest");

    T2Demo::MissionReplacementState fresh{"initial", {}};
    assert(!fresh.commit("later"));
    assert(fresh.loadedMission == "initial");
    assert(fresh.defer("Later"));
    assert(fresh.defer("later"));
    assert(fresh.pendingMission == "later");
    assert(fresh.commit("later"));
    return 0;
}
