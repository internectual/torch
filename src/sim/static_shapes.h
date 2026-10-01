#pragma once
// StaticShape (game/staticShape.cc), Turret (game/turret.cc) and Item
// (game/item.cc) ghosts in the layout the Tribes 2 client reads.
#include "sim/shape_base.h"

class StaticShapeObject : public ShapeBase {
public:
    enum StaticShapeMasks : uint32_t { PositionMask = ShapeBase::NextFreeMask, NextFreeMask = ShapeBase::NextFreeMask << 1 };
    explicit StaticShapeObject(const char* netClass = "StaticShape", bool always = false) : netClass(netClass) {
        ghostable = true;
        scopeAlways = always;
    }
    const char* netClassName() const override { return netClass; }
    bool powered = false; // mPowered
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    // StaticShape::processTick: a move's triggers drive image slots 0 and 1.
    void processMove(const ClientMoveIn* move) override;

private:
    const char* netClass;
};

// game/turret.cc as the shipped server runs it (V12 plus the retail
// capacitor, the yawVariance/pitchVariance fire tolerances and the
// dontFireInsideDamageRadius test against damageRadius): the barrel's
// activation, aim and fire states, the script's target selection, and the
// barrel swap.
class TurretObject : public StaticShapeObject {
public:
    enum TurretMasks : uint32_t {
        MountedUpdateMask = StaticShapeObject::NextFreeMask,
        BogoMask = MountedUpdateMask << 1,
        NextFreeMask = BogoMask << 2,
    };
    enum States { Dormant, Activating, Deactivating, Active, DeactivateForReplace };
    TurretObject() : StaticShapeObject("Turret") {}
    // Turret::onAdd, after the script's onAdd: mounts initialBarrel in slot 0.
    void onAdded() override;
    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;
    // mCurrBarrel is the initialBarrel persist field: it names the barrel
    // mounted now.
    bool mountImage(std::shared_ptr<const ShapeBaseImageData> image, uint32_t slot, bool loaded,
                    uint32_t skinTag) override;
    bool unmountImage(uint32_t slot) override;
    // The mount node as the activate/elevate/turn threads last posed it.
    std::array<float, 16> getMountTransform(uint32_t mountPoint) const override;
    void getMuzzleVector(uint32_t slot, float vec[3]) const override;

    // Console methods.
    void setSkill(float skill);
    bool setTarget(ShapeBase* target);
    int getTargetId() const;
    bool isValidTarget(ShapeBase* target) const;
    bool initiateReplace(ShapeBase* engineer);
    void setAutoFire(bool status);
    // The capacitor as a fraction of the datablock's maxCapacitorEnergy.
    float capacitorFraction() const;

    int state = Dormant;          // mCurrState
    float activationLevel = 0.0f; // 0 deactivated .. 1 activated
    float currPhi = 0.0f;         // degrees, null 0
    float currTheta = 90.0f;      // degrees, null 90
    float skillLevel = 1.0f;
    std::string currTarget, currEngineer; // SimObjectPtr<ShapeBase> (object keys)
    uint32_t targetlessTime = 0, lastThink = 0;
    bool autoFire = true;
    float capacitorLevel = 0.0f, capacitorRechargeRate = 0.0f;

private:
    ShapeBase* target() const;
    ShapeBase* engineer() const;
    bool currTargetValid() const;
    bool isFrozen() const { return damageState != Enabled; }
    float thetaMin() const;
    float thetaMax() const;
    float thetaNull() const;
    int primaryAxis() const;
    float phiSpeed() const;
    float thetaSpeed() const;
    float activationSpeed() const;
    uint32_t deactivateDelay() const;
    uint32_t thinkTime() const;
    float attackRadius() const;
    void updateState(bool playerControlled);
    void performActivateRamp();
    void performDeactivateRamp();
    void selectTarget();
    void checkReplace();
    void aiThink();
    void aiUpdateActive(float& yaw, float& pitch, bool trigger[2]);
    Point3F dopeAim(const Point3F& startLocation, const Point3F& aimLocation);
    // The shape instance's threads (mActivateThread, mElevateThread,
    // mTurnThread) and the mount nodes as animate() last posed them.
    bool activateThread = false, elevateThread = false, turnThread = false;
    struct Pose {
        bool valid = false, activate = false, elevate = false, turn = false;
        float activatePos = 0, elevatePos = 0, turnPos = 0;
    } pose;
    bool mountPosed[8]{};
    std::array<float, 16> mountPose[8]{};
    void animate();
};

// The shipped BeaconObject: a StaticShape with a beacon type (enemy,
// friend, vehicle) the clients receive.
class BeaconObject : public StaticShapeObject {
public:
    enum BeaconMasks : uint32_t { BeaconMask = StaticShapeObject::NextFreeMask };
    BeaconObject() : StaticShapeObject("BeaconObject") {}
    int beaconType = 0;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
};

// game/missionMarker.cc: markers ghost (scope always) their transform and
// scale; outside the editor they stay out of the scene container.
class MissionMarkerObject : public ShapeBase {
public:
    enum MarkerMasks : uint32_t { PositionMask = ShapeBase::NextFreeMask, NextFreeMask = PositionMask << 1 };
    explicit MissionMarkerObject(const char* netClass = "MissionMarker") : netClass(netClass) {
        ghostable = true;
        scopeAlways = true;
    }
    const char* netClassName() const override { return netClass; }
    bool inContainer() const override { return false; }
    // MissionMarker::onAdd fails without a datablock: such a marker never
    // reaches the clients.
    void readFields() override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;

private:
    const char* netClass;
};

// AIObjective (ai/aiObjective.cc): a marker with an empty sphere update.
class AIObjectiveObject : public MissionMarkerObject {
public:
    enum AIObjectiveMasks : uint32_t { UpdateSphereMask = MissionMarkerObject::NextFreeMask };
    AIObjectiveObject() : MissionMarkerObject("AIObjective") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        const uint32_t ret = MissionMarkerObject::packUpdate(connection, mask, w);
        w.writeFlag(mask & UpdateSphereMask);
        return ret;
    }
};

// WayPoint: its name, team and hidden state for the clients' HUD.
class WayPointObject : public MissionMarkerObject {
public:
    enum WayPointMasks : uint32_t {
        UpdateNameMask = MissionMarkerObject::NextFreeMask,
        UpdateTeamMask = UpdateNameMask << 1,
        UpdateHiddenMask = UpdateTeamMask << 1,
    };
    WayPointObject() : MissionMarkerObject("WayPoint") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
};

// SpawnSphere: its radius and weights.
class SpawnSphereObject : public MissionMarkerObject {
public:
    enum SpawnSphereMasks : uint32_t { UpdateSphereMask = MissionMarkerObject::NextFreeMask };
    SpawnSphereObject() : MissionMarkerObject("SpawnSphere") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
};

class ItemObject : public ShapeBase {
public:
    enum ItemMasks : uint32_t {
        HiddenMask = ShapeBase::NextFreeMask,
        ThrowSrcMask = ShapeBase::NextFreeMask << 1,
        PositionMask = ShapeBase::NextFreeMask << 2,
        RotationMask = ShapeBase::NextFreeMask << 3,
    };
    ItemObject() { ghostable = true; }
    const char* netClassName() const override { return "Item"; }
    void readFields() override;
    bool rotate = false, isStatic = false, collideable = false, atRest = true;
    float velocity[3] = {0, 0, 0};
    int atRestCounter = 0;
    // setCollisionTimeout: the thrower, not collided with for 15 ticks.
    std::string collisionObject;
    int collisionTimeout = 0;
    float stickyPos[3] = {0, 0, 0}, stickyNormal[3] = {0, 0, 1};
    void setVelocity(const float v[3]);
    void processMove(const ClientMoveIn* move) override;

    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;

private:
    void updateVelocity(float dt);
    void updatePos(float dt);
};

void registerStaticShapeNatives(class TorqueScript& ts);

// The client's beacon labels by beacon type (enemy/target, friend/marker,
// vehicle): setBeaconNames, "Target Beacon" / "Marker Beacon" / "Bomb
// Target" until the server sends its own.
const std::string& beaconName(int type);
