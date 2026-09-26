#include "game/player_animation.h"
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

using namespace PlayerAnimation;

static void testActionTable() {
    // Embedded sequences first, then constructor imports in declaration
    // order. Table actions take the first match; the rest follow in order.
    const std::vector<std::string> names{
        "ambient", "root", "run", "back", "side", "look", "fall", "jet",
        "land", "jump", "death1", "death2", "Run"};
    const std::vector<int> table = buildActionTable(names);
    assert(table.size() == names.size());
    assert(table[Root] == 1 && table[Run] == 2 && table[Back] == 3 && table[Side] == 4);
    assert(table[Fall] == 6 && table[Jet] == 7 && table[Jump] == 9 && table[Land] == 8);
    // Non-table sequences from index 8, in shape order.
    assert(table[8] == 0);  // ambient
    assert(table[9] == 5);  // look
    assert(table[10] == 10 && table[11] == 11);
    assert(table[12] == 12); // duplicate "Run" is not a table action again

    const std::vector<int> missing = buildActionTable({"death1"});
    assert(missing.size() == 9 && missing[Root] == -1 && missing[8] == 0);
}

static void testPickMoveAnimation() {
    const float yaw = 0.0f; // forward = +Y
    assert(pickMoveAnimation(0, 5, yaw, 0, false, false).action == Run);
    assert(pickMoveAnimation(0, -5, yaw, 0, false, false).action == Back);
    assert(pickMoveAnimation(-5, 0, yaw, 0, false, false).action == Side);
    const MoveAnimation right = pickMoveAnimation(5, 0, yaw, 0, false, false);
    assert(right.action == Side && right.timeScale == -1.0f);
    assert(pickMoveAnimation(0.05f, 0.05f, yaw, 0, false, false).action == Root);
    // Rotated body: facing +X (yaw = 90 degrees) runs forward along +X.
    const float east = 3.14159265f / 2.0f;
    assert(pickMoveAnimation(5, 0, east, 0, false, false).action == Run);
    // Falling overrides; airborne picks jet or root regardless of velocity.
    assert(pickMoveAnimation(0, 5, yaw, 0, true, false).action == Fall);
    assert(pickMoveAnimation(0, 5, yaw, AirborneContactTicks, false, true).action == Jet);
    assert(pickMoveAnimation(0, 5, yaw, AirborneContactTicks, false, false).action == Root);
    assert(pickMoveAnimation(0, 5, yaw, AirborneContactTicks - 1, false, true).action == Run);
}

static void testContactAndActionPosition() {
    assert(hasRunContact(10.02f, 10.0f, 1.0f, 70.0f));
    assert(!hasRunContact(10.2f, 10.0f, 1.0f, 70.0f));
    assert(hasRunContact(9.5f, 10.0f, 1.0f, 70.0f)); // penetrating the floor
    assert(!hasRunContact(10.0f, 10.0f, 0.2f, 70.0f)); // too steep
    assert(!hasRunContact(10.0f, -1e9f, 1.0f, 70.0f));

    assert(sampleActionPosition(0.25f, false, 1.0f, 2.0f, 4.0f) == 0.5f);
    assert(sampleActionPosition(0.25f, true, 1.0f, 2.0f, 4.0f) == 1.0f);
    assert(sampleActionPosition(0.9f, false, 1.0f, 5.0f, 1.0f) == 1.0f);
    assert(sampleActionPosition(0.3f, false, 1.0f, 0.0f, 2.0f) == 0.3f);
}

int main() {
    testActionTable();
    testPickMoveAnimation();
    testContactAndActionPosition();
    return 0;
}
