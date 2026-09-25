#pragma once

#include <cmath>
#include <algorithm>

// Damage updates must remain ordinary finite ShapeBase values.  Invalid
// script/network input is ignored rather than poisoning health state.
inline bool isValidDamageAmount(float amount) {
    return std::isfinite(amount);
}

// ShapeBase damage flashes are normalized to the damage received this tick.
inline float damageFlashForAmount(float amount, float maxHealth = 100.0f) {
    if (!isValidDamageAmount(amount) || !std::isfinite(maxHealth) ||
        amount <= 0.0f || maxHealth <= 0.0f)
        return 0.0f;
    return std::clamp(amount / maxHealth, 0.0f, 1.0f);
}

// Torque's damageLevel is damage as a fraction of the object's authored
// health cap, not a percentage against a universal 100-point player.
inline float damageLevelForHealth(float health, float maxHealth) {
    if (!std::isfinite(health) || !std::isfinite(maxHealth) || maxHealth <= 0.0f)
        return 0.0f;
    return std::clamp(1.0f - health / maxHealth, 0.0f, 1.0f);
}

// RepairTool projectiles encode healing as negative damage.  Keep the signed
// operation bounded by the ShapeBase health cap so client and server agree.
inline float applySignedDamage(float health, float amount,
                               float maxHealth = 100.0f) {
    if (!std::isfinite(health) || !std::isfinite(amount) ||
        !std::isfinite(maxHealth) || maxHealth <= 0.0f)
        return health;
    return std::clamp(health - amount, 0.0f, maxHealth);
}

// Armor can absorb at most the armor resource that remains.  Without the
// cap, a nearly depleted suit still reduces a large hit by the full armor
// percentage and drives the armor value negative.
inline float applyArmorDamage(float& health, float& armor, float amount) {
    if (!isValidDamageAmount(amount) || amount <= 0.0f) return 0.0f;
    const float availableArmor = std::max(0.0f, armor);
    const float absorbed = std::min(availableArmor, amount * 0.6f);
    armor = availableArmor - absorbed;
    // Armor can be depleted by a single hit. Keep ShapeBase health at its
    // terminal value so callers cannot remain in a negative, non-dead state.
    health = std::max(0.0f, health - (amount - absorbed));
    return absorbed;
}

// ShapeBase repairRate is health restored per second.  Repair is a regular
// tick effect, but must never revive a dead object or exceed its datablock max.
inline float applyRepairRate(float health, float repairRate, float dt,
                             float maxHealth = 100.0f) {
    if (!std::isfinite(health) || !std::isfinite(repairRate) ||
        !std::isfinite(dt) || !std::isfinite(maxHealth) || health <= 0.0f ||
        repairRate <= 0.0f || dt <= 0.0f || maxHealth <= 0.0f)
        return health;
    return std::clamp(health + repairRate * dt, 0.0f, maxHealth);
}
