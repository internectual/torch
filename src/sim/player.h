#pragma once
// Player (game/player.cc), server side: Player::processTick through the
// PlayerPrediction port, the ghost update and the control object's packet
// data in the layout the Tribes 2 client reads.
#include "sim/shape_base.h"
#include "game/player_prediction.h"
#include <functional>
#include <string>
#include <vector>

class PlayerObject : public ShapeBase {
public:
    enum PlayerMasks : uint32_t {
        ActionMask = ShapeBase::NextFreeMask,
        MoveMask = ShapeBase::NextFreeMask << 1,
        ImpactMask = ShapeBase::NextFreeMask << 2,
    };

    PlayerObject();
    const char* netClassName() const override { return "Player"; }
    void readFields() override;

    PlayerPrediction::State state;
    // mActionAnimation / mArmAnimation: indices into the PlayerData action
    // list (actionNames); -1 is none.
    int action = -1, armAction = -1;
    bool actionHold = false, actionFirstPerson = true;
    // PlayerData::preload's action list for this player's shape (names).
    const std::vector<std::string>& actionNames();
    bool setActionThread(const std::string& name, bool hold, bool firstPerson);
    bool setArmThread(const std::string& name);
    // Moves the player's transform (setTransform): position and yaw.
    void setTransform(const std::array<float, 16>& matrix);
    const char* stateName() const;
    // Player::updateDamageLevel: disabled (dead) at maxDamage.
    void updateDamageLevel() override;
    bool worldBox(float lo[3], float hi[3]) const override;
    void setVelocity(const Point3F& velocity);
    // Player::applyImpulse: players ignore the angular part.
    void applyImpulse(const Point3F& impulse);

    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;
    // Player::setControlObject: the object (a vehicle or turret) the player's
    // moves drive; empty or the player itself clears it.
    void setControlObject(const std::string& object);
    void onRemove() override;
    std::string controlObject;
    bool pilot = false;

    bool canJump() const;
    bool haveContact() const { return !state.contactTimer; }
    // Player::getJetAbility: jet acceleration per tick, the ticks a full
    // tank jets for, and the jump speed; returns the ticks the current
    // energy jets for (the bots' jet ratings, JetManager::calcJetRatings).
    float getJetAbility(float& thrust, float& duration, float& jumpSpeed) const;
    // Player::getMomentum / setMomentum / displaceObject: a vehicle pushes
    // the player out of its way (Vehicle::advanceToCollision).
    float mass() const { return dataFloat("mass", 1.0f); }
    Point3F getMomentum() const;
    void setMomentum(const Point3F& momentum);
    bool displaceObject(const Point3F& displacement);

private:
    const PlayerPrediction::Data* physics();
    void syncTransform();
    PlayerPrediction::Collision collision;
    std::string physicsBlock;
    PlayerPrediction::Data physicsData;
    bool physicsValid = false;
};

// The static world geometry and water the server collides with: set to
// ServerContainer (the server's mission objects) at registration.
struct PlayerContacts {
    static void queue(PlayerObject& player);
};

struct ServerCollision {
    PlayerPrediction::GatherTriangles triangles;
    PlayerPrediction::WaterLevel water;
    // The terrain and / or interior triangles with each one's object (the
    // container's rays report what they hit).
    std::function<void(const Point3F&, const Point3F&, bool terrain, bool interiors,
                       std::vector<PlayerPrediction::Triangle>&, std::vector<const ScriptObject*>*)> geometry;
};
ServerCollision& serverCollision();

void registerPlayerNatives(class TorqueScript& ts);
