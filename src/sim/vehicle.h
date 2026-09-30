#pragma once
// Vehicle (game/vehicle.cc), HoverVehicle (game/hoverVehicle.cc) and
// FlyingVehicle (game/flyingVehicle.cc), server side: the Rigid body, the
// move, forces and collision each tick, and the ghost/control-object data
// in the layout the Tribes 2 client reads.
#include "sim/shape_base.h"
#include "sim/torque_math.h"
#include "sim/game_connection.h"
#include "game/player_prediction.h"
#include <string>
#include <vector>

// game/rigid.cc.
struct Rigid {
    struct State {
        Point3F force{0, 0, 0}, torque{0, 0, 0};
        Point3F linVelocity{0, 0, 0}, linPosition{0, 0, 0}, linMomentum{0, 0, 0};
        Point3F angVelocity{0, 0, 0}, angMomentum{0, 0, 0};
        TorqueMath::Quat angPosition;
        TorqueMath::Matrix invWorldInertia{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Point3F getVelocity(const Point3F& r) const;
        TorqueMath::Matrix getTransform() const;
        void setTransform(const TorqueMath::Matrix& mat);
    };
    State state;
    TorqueMath::Matrix invObjectInertia{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float oneOverMass = 1, mass = 1, restitution = 0.3f, friction = 0.5f;
    bool atRest = false;

    void clearForces() { state.force = {0, 0, 0}; state.torque = {0, 0, 0}; }
    void updateVelocity(State& t) const;
    void integrate(State& t, float delta) const;
    void applyImpulse(State& t, const Point3F& r, const Point3F& impulse);
    bool resolveCollision(State& t, const Point3F& p, const Point3F& normal);
    float getZeroImpulse(const State& t, const Point3F& r, const Point3F& normal) const;
    // setObjectInertia: the box moment is not applied (the Tribes 2 engine
    // keeps the identity inverse inertia), only the world inertia follows.
    void setObjectInertia();
};

class VehicleObject : public ShapeBase {
public:
    enum VehicleMasks : uint32_t {
        PositionMask = ShapeBase::NextFreeMask,
        FrozenMask = ShapeBase::NextFreeMask << 1,
        NextFreeMask = ShapeBase::NextFreeMask << 2,
        EnergyMask = ShapeBase::NextFreeMask << 3,
    };
    enum ThrustDirection { ThrustForward, ThrustBackward, ThrustDown, NumThrustBits = 3 };

    explicit VehicleObject(const char* netClass) : netClass(netClass) { ghostable = true; }
    const char* netClassName() const override { return netClass; }
    void readFields() override;
    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;

    void setTransform(const TorqueMath::Matrix& mat);
    void applyImpulse(const Point3F& pos, const Point3F& impulse);
    Point3F getVelocity() const { return rigid.state.linVelocity; }
    void setEnergyLevel(float level);
    void setFrozenState(bool frozen);

    bool disableMove = false;

protected:
    struct Contact {
        Point3F point, normal;
        float distance;
        std::string object; // the ShapeBase hit, empty for world geometry
    };
    virtual void updateMove(const ClientMoveIn* move);
    virtual void updateForces() {}
    virtual uint32_t collisionMask() const;
    // The closest distance of the collision hull at `mat` to anything
    // within `tol` (1e7 when nothing is; negative while penetrating), and
    // the contacts within `tol`; the hull crossing a surface since `from`
    // reads as the deepest penetration.
    float collide(const TorqueMath::Matrix& mat, float tol, std::vector<Contact>* contacts,
                  const TorqueMath::Matrix* from = nullptr);
    std::vector<ScriptObject*> collisionExempt() const;

    Rigid rigid;
    PlayerPrediction::Move move;  // the unclamped move (mDelta.move)
    ClientMoveIn packedMove;      // as it was sent, for Move::pack
    float steering[2] = {0, 0};
    float throttle = 0;
    bool jetting = false, frozen = false, inLiquid = false;
    int stuckTimer = 0;
    float mass = 1, oneOverMass = 1;
    // VehicleData's drag and density defaults.
    float defaultDrag() const override { return 0.7f; }
    float defaultDensity() const override { return 4.0f; }
    float objMin[3] = {-1, -1, 0}, objMax[3] = {1, 1, 2}; // mObjBox

    float data(const char* field, float fallback) const { return dataFloat(field, fallback); }

private:
    void updatePos(float dt);
    bool advanceToCollision(float time);
    bool resolveCollision(Rigid::State& ns, const std::vector<Contact>& contacts);
    void resolveContacts(Rigid::State& ns, const std::vector<Contact>& contacts, float dt);
    void damageQueuedObjects(float collisionVel);
    void setPosition(const Point3F& pos, const TorqueMath::Quat& rot);
    const char* netClass;
    // CollisionTimeout: the objects struck this tick, with the speed at
    // which a displaced object was pushed (useData).
    struct Struck { std::string object; float data; bool useData; };
    std::vector<Struck> struck;
    const std::vector<Point3F>* hull = nullptr;
};

class HoverVehicleObject : public VehicleObject {
public:
    HoverVehicleObject() : VehicleObject("HoverVehicle") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;

protected:
    void updateMove(const ClientMoveIn* move) override;
    void updateForces() override;

private:
    float forwardThrust = 0, reverseThrust = 0, leftThrust = 0, rightThrust = 0;
    bool floating = false;
    ThrustDirection thrustDirection = ThrustForward;
};

class FlyingVehicleObject : public VehicleObject {
public:
    FlyingVehicleObject() : VehicleObject("FlyingVehicle") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    void useCreateHeight(bool on);

protected:
    void updateMove(const ClientMoveIn* move) override;
    void updateForces() override;

private:
    float getHeight();
    Point3F thrust{0, 0, 0};
    float ceilingFactor = 1;
    bool createHeightOn = false;
    ThrustDirection thrustDirection = ThrustForward;
};

class WheeledVehicleObject : public VehicleObject {
public:
    enum { MaxWheels = 8 };
    WheeledVehicleObject() : VehicleObject("WheeledVehicle") {}
    void readFields() override;
    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;

    // WheeledVehicleData::Wheel, from the shape's ground%d nodes and their
    // spring%d / turn%d sequences (WheeledVehicleData::preload).
    struct WheelData {
        enum Steering { None, Forward, Backward } steering = None;
        int opposite = -1;
        Point3F safePos, pos, spring;
    };
    struct Wheel {
        float extension = 1, center = 1, k = 0, s = 0;
        float avel = 0, Dy = 0, Dx = 0, torqueScale = 0;
        bool contact = false;
        Point3F surfacePos{0, 0, 0}, surfaceNormal{0, 0, 1};
    };

protected:
    void updateMove(const ClientMoveIn* move) override;
    void updateForces() override;

private:
    void updateWheels();
    const std::vector<WheelData>* wheelData = nullptr;
    Wheel wheels[MaxWheels];
    bool braking = false, wheelContact = false;
};

void registerVehicleNatives(class TorqueScript& ts);
