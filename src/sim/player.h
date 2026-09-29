#pragma once
// Player (game/player.cc), server side: Player::processTick through the
// PlayerPrediction port, the ghost update and the control object's packet
// data in the layout the Tribes 2 client reads.
#include "sim/shape_base.h"
#include "game/player_prediction.h"
#include <functional>

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
};
ServerCollision& serverCollision();

void registerPlayerNatives(class TorqueScript& ts);
