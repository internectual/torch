#pragma once
// Trigger (game/trigger.cc), server side: the objects inside its
// polyhedron, onEnterTrigger / onLeaveTrigger / onTickTrigger.
#include "sim/game_base.h"
#include <string>
#include <vector>

class TriggerObject : public GameBase {
public:
    void readFields() override;
    void processTick() override;
    // Trigger::potentialEnterObject: enter when inside, once.
    void potentialEnterObject(const std::string& object);
    // The trigger's world bounding box.
    void worldBox(float lo[3], float hi[3]) const;
    std::vector<std::string> objects;

private:
    bool testObject(const std::string& object) const;
    float origin[3] = {0, 0, 0}, vecs[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    bool hasPolyhedron = false;
    int currTick = 0, lastThink = 0;
};

// Every trigger whose box overlaps [lo, hi] gets potentialEnterObject.
void triggersPotentialEnter(const std::string& object, const float lo[3], const float hi[3]);
void registerTriggerNatives(class TorqueScript& ts);
