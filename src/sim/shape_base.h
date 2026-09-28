#pragma once
// ShapeBase damage, repair, energy and presentation state (shapeBase.cc).
#include "sim/game_base.h"

class ShapeBase : public GameBase {
public:
    enum DamageState { Enabled, Disabled, Destroyed };

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

    void processTick() override;
};

void registerShapeBaseNatives(class TorqueScript& ts);
