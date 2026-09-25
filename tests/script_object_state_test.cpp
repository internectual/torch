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
    populated.mountedImages[1].mountNodeObjectId = 23;
    populated.threads[2].sequence = 4;
    populated.threads[2].state = 1;
    populated.threads[2].valid = true;

    assert(populated.position.z == 3.0f && populated.velocity.x == -4.0f);
    assert(populated.health == 42.0f && populated.energy == 73.0f);
    assert(populated.sensorGroup == 2);
    assert(populated.mountedImages[1].datablockId == 17 &&
           populated.mountedImages[1].mountPoint == 3 &&
           populated.mountedImages[1].loaded && populated.mountedImages[1].firing &&
           populated.mountedImages[1].mountNodeObjectId == 23);
    assert(populated.threads[2].sequence == 4 && populated.threads[2].state == 1 &&
           populated.threads[2].valid);

    assert(ScriptStateParity::damageLevel(100.0f, 100.0f) == 0.0f);
    assert(ScriptStateParity::damageLevel(25.0f, 100.0f) == 0.75f);
    assert(ScriptStateParity::damageLevel(0.0f, 0.0f) == 0.0f);
    assert(ScriptStateParity::energyPercent(-1.0f) == 0.0f);
    assert(ScriptStateParity::energyPercent(50.0f) == 0.5f);
    assert(ScriptStateParity::energyPercent(101.0f) == 1.0f);

    return 0;
}
