#pragma once
// ShapeBase damage, repair, energy and presentation state (shapeBase.cc).
#include "sim/game_base.h"

class ShapeBase : public GameBase {
public:
    enum DamageState { Enabled, Disabled, Destroyed };
    enum ShapeBaseMasks : uint32_t {
        DamageMask = GameBase::NextFreeMask,
        NoWarpMask = GameBase::NextFreeMask << 1,
        MountedMask = GameBase::NextFreeMask << 2,
        CloakMask = GameBase::NextFreeMask << 3,
        ShieldMask = GameBase::NextFreeMask << 4,
        InvincibleMask = GameBase::NextFreeMask << 5,
        SoundMaskN = GameBase::NextFreeMask << 6,
        ThreadMaskN = SoundMaskN << 4,
        ImageMaskN = ThreadMaskN << 4,
        NextFreeMask = ImageMaskN << 8,
        SoundMask = (SoundMaskN << 4) - SoundMaskN,
        ThreadMask = (ThreadMaskN << 4) - ThreadMaskN,
        ImageMask = (ImageMaskN << 8) - ImageMaskN,
    };

    float damage = 0.0f;
    float repairRate = 0.0f;
    float repairReserve = 0.0f;
    float rechargeRate = 0.0f;
    float energy = 0.0f;
    DamageState damageState = Enabled;
    float damageFlash = 0.0f;
    float whiteOut = 0.0f;
    float invincibleTime = 0.0f, invincibleSpeed = 0.0f;
    bool cloaked = false;
    bool passiveJammed = false;
    float heat = 0.0f;
    bool hidden = false;
    float cameraFov = 90.0f;
    bool trigger[6]{}; // mTrigger: the last move's trigger states

    float maxDamage() const { return dataFloat("maxDamage", 1.0f); }
    float maxEnergy() const { return dataFloat("maxEnergy", 0.0f); }
    bool invincible() const { return dataBool("isInvincible", false); }

    float getEnergyLevel() const { return energy; }
    float getEnergyValue() const;
    void setEnergyLevel(float level);
    void setDamageLevel(float level);
    void applyDamage(float amount) { if (amount > 0) setDamageLevel(damage + amount); }
    void applyRepair(float amount);
    float getDamageValue() const;
    void setDamageState(DamageState state);
    bool setDamageState(const std::string& name);
    const char* damageStateName() const;

    void processMove(const ClientMoveIn* move) override;
    void processShapeTick(); // energy and repair
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;
};

void registerShapeBaseNatives(class TorqueScript& ts);
