#pragma once
// Trigger (game/trigger.cc), server side: the objects inside its
// polyhedron, onEnterTrigger / onLeaveTrigger / onTickTrigger.
#include "sim/game_base.h"
#include "game/player_prediction.h"
#include <string>
#include <vector>

class TriggerObject : public GameBase {
public:
    void readFields() override;
    void processTick() override;
    void onDeleteNotify(ScriptObject* object) override;
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

// PhysicalZone (game/physicalZone.cc): a polyhedron that scales the
// velocity of a player meeting its faces (velocityMod); ghosted (scope
// always) with its polyhedron, gravityMod, appliedForce and active state.
class PhysicalZoneObject : public SceneObject {
public:
    enum Masks : uint32_t { InitialUpdateMask = 1u << 0, ActiveMask = 1u << 1 };
    PhysicalZoneObject() { scopeAlways = true; }
    const char* netClassName() const override { return "PhysicalZone"; }
    void readFields() override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    void activate();
    void deactivate();
    bool active = true;
    bool hasPolyhedron = false;
    bool overlapsBox(const float lo[3], const float hi[3]) const;
    float points[8][3] = {};   // Polyhedron::pointList (object space)
    float planes[6][4] = {};   // planeList: normal, d
    float velocityMod() const;
    // The polyhedron's faces in world space, outward.
    void faces(std::vector<PlayerPrediction::Triangle>& out) const;
};

namespace PhysicalZones {
struct Effects {
    float gravityMod = 1.0f;
    Point3F appliedForce{};
};
Effects effects(const float lo[3], const float hi[3]);
// The active zones whose world box overlaps [min, max].
void gather(const Point3F& min, const Point3F& max, std::vector<PlayerPrediction::Zone>& out);
} // namespace PhysicalZones
