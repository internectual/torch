#include "game/trigger.h"
#include <cassert>
#include <unordered_set>

int main() {
    assert(triggerActorIsActive(true));
    assert(!triggerActorIsActive(false));

    const auto cube = triggerBox({1, 2, 3});
    assert(cube.contains({0, 0, 0}));
    assert(cube.contains({1, 2, 3}));
    assert(!cube.contains({1.01f, 0, 0}));
    assert(!cube.contains({0, 2.01f, 0}));

    const auto fallback = triggerUnitBox();
    assert(fallback.contains({0.5f, 0, 0}));
    assert(!fallback.contains({0.51f, 0, 0}));

    const auto tetra = triggerFromVertices({{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}});
    assert(tetra.planes.size() == 4);
    assert(tetra.contains({0.1f, 0.1f, 0.1f}));
    assert(!tetra.contains({1, 1, 1}));
    assert(!tetra.contains({-0.01f, 0.1f, 0.1f}));

    // A rotated PhysicalZone must follow its authored orientation.  The
    // world-space point is outside the unrotated box but inside after a 90
    // degree rotation around the T2 Z axis.
    const Point3F center{0, 0, 0};
    assert(orientedBoxContains({0, 0, 1.5f}, center, {0, 0, 1}, 90.0f,
                               {3, 1, 1}));
    assert(!orientedBoxContains({0, 0, 1.5f}, center, {0, 0, 1}, 0.0f,
                                {3, 1, 1}));

    // PhysicalZone polyhedra can be smaller than the object's transform box;
    // containment must use the authored volume rather than scale alone.
    const auto zone = triggerFromVertices({{-0.25f, -0.25f, -0.25f},
                                           {0.25f, -0.25f, -0.25f},
                                           {-0.25f, 0.25f, -0.25f},
                                           {-0.25f, -0.25f, 0.25f}});
    assert(transformedTriggerContains(zone, {-2.0f, -2.0f, 2.0f}, center,
                                      {10, 10, 10}, {0, 0, 0}, 0.0f));
    assert(!transformedTriggerContains(zone, {2.0f, 0.0f, 0.0f}, center,
                                       {10, 10, 10}, {0, 0, 0}, 0.0f));

    // Applied force follows the same authored rotation as the zone volume.
    const Point3F force = physicalZoneForceToYUp({1, 0, 0}, {0, 0, 1}, 90.0f);
    assert(force.x > -0.01f && force.x < 0.01f);
    assert(force.y > -0.01f && force.y < 0.01f);
    assert(force.z > 0.99f && force.z < 1.01f);

    // Non-uniform authored scale is applied in Torque axes before the frame
    // conversion, rather than being swapped with the converted axes.
    const Point3F scaled = triggerLocalPointToYUp({1, 2, 3}, {4, 5, 6});
    assert(scaled.x == 4.0f && scaled.y == 18.0f && scaled.z == -10.0f);
    const Point3F network = triggerNetworkPointToYUp({4, 7, -2});
    assert(network.x == 4.0f && network.y == -2.0f && network.z == -7.0f);

    const auto values = triggerNumbers("0 0 0; 2.5 -1 4");
    assert(values.size() == 6 && values[3] == 2.5f && values[4] == -1.0f);
    // Invalid authored coordinates must not create a hull that contains every
    // actor: NaN makes all ordinary comparisons false.
    const auto invalidValues = triggerNumbers("0 0 0 nan 1 2");
    assert(invalidValues.size() == 5);
    TriggerPolyhedron invalidPlane;
    invalidPlane.planes.push_back({NAN, 0, 0, 0});
    assert(!invalidPlane.contains({0, 0, 0}));

    std::unordered_set<std::string> previous{"zombie", "Player"};
    const auto first = triggerTransitions(previous, {"Player", "Bot2"});
    assert((first.entered == std::vector<std::string>{"Bot2"}));
    assert((first.left == std::vector<std::string>{"zombie"}));
    previous = {"Player", "Bot2"};
    const auto stable = triggerTransitions(previous, {"Player", "Bot2"});
    assert(stable.entered.empty() && stable.left.empty());

    triggerVolumeChanged(previous);
    assert(previous.empty());
    const auto reevaluated = triggerTransitions(previous, {"Player"});
    assert((reevaluated.entered == std::vector<std::string>{"Player"}));
    return 0;
}
