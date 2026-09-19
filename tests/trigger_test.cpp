#include "game/trigger.h"
#include <cassert>
#include <unordered_set>

int main() {
    const auto cube = triggerBox({1, 2, 3});
    assert(cube.contains({0, 0, 0}));
    assert(cube.contains({1, 2, 3}));
    assert(!cube.contains({1.01f, 0, 0}));
    assert(!cube.contains({0, 2.01f, 0}));

    const auto tetra = triggerFromVertices({{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}});
    assert(tetra.planes.size() == 4);
    assert(tetra.contains({0.1f, 0.1f, 0.1f}));
    assert(!tetra.contains({1, 1, 1}));
    assert(!tetra.contains({-0.01f, 0.1f, 0.1f}));

    const auto values = triggerNumbers("0 0 0; 2.5 -1 4");
    assert(values.size() == 6 && values[3] == 2.5f && values[4] == -1.0f);

    std::unordered_set<std::string> previous{"zombie", "Player"};
    const auto first = triggerTransitions(previous, {"Player", "Bot2"});
    assert((first.entered == std::vector<std::string>{"Bot2"}));
    assert((first.left == std::vector<std::string>{"zombie"}));
    const auto stable = triggerTransitions(previous, {"Player", "Bot2"});
    assert(stable.entered.empty() && stable.left.empty());
    return 0;
}
