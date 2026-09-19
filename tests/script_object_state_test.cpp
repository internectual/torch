#include "script/script_engine.h"

#include <cassert>

int main() {
    ScriptObjectState empty;
    assert(empty.position.x == 0.0f && empty.position.y == 0.0f && empty.position.z == 0.0f);
    assert(empty.rotationW == 1.0f);
    assert(empty.sensorGroup == -1);
    assert(!empty.hasRotation && !empty.hasVelocity && !empty.hasHealth && !empty.hasEnergy);
    assert(empty.mountedImages[0].datablockId == -1);

    ScriptObjectState populated;
    populated.position = {1.0f, 2.0f, 3.0f};
    populated.rotation = {0.0f, 0.0f, 0.7071f};
    populated.rotationW = 0.7071f;
    populated.velocity = {-4.0f, 5.0f, 6.0f};
    populated.health = 42.0f;
    populated.maxHealth = 100.0f;
    populated.energy = 73.0f;
    populated.sensorGroup = 2;
    populated.hasRotation = populated.hasVelocity = populated.hasHealth = populated.hasEnergy = true;
    populated.mountedImages[1].datablockId = 17;
    populated.mountedImages[1].mountPoint = 3;
    populated.mountedImages[1].loaded = true;
    populated.mountedImages[1].firing = true;

    assert(populated.position.z == 3.0f && populated.velocity.x == -4.0f);
    assert(populated.health == 42.0f && populated.energy == 73.0f);
    assert(populated.sensorGroup == 2);
    assert(populated.mountedImages[1].datablockId == 17 &&
           populated.mountedImages[1].mountPoint == 3 &&
           populated.mountedImages[1].loaded && populated.mountedImages[1].firing);

    return 0;
}
