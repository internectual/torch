#pragma once

#include <cstddef>
#include <cmath>

namespace DeathRespawn {
constexpr float RespawnDelay = 3.0f;
constexpr float NeverHitTime = -10.0f;

inline bool crossesIntoDeath(float healthBefore, float damage) {
    return healthBefore > 0.0f && healthBefore - damage <= 0.0f;
}

// Direct health changes such as fall damage still produce one death statistic
// when they cross the living boundary.
inline bool crossesHealthBoundary(float healthBefore, float healthAfter) {
    return std::isfinite(healthBefore) && std::isfinite(healthAfter) &&
           healthBefore > 0.0f && healthAfter <= 0.0f;
}

inline bool respawnDue(float now, float respawnAt) {
    return respawnAt > 0.0f && now >= respawnAt;
}

// Stable round-robin selection avoids making gameplay tests depend on rand().
inline std::size_t nextSpawn(std::size_t cursor, std::size_t count) {
    return count == 0 ? 0 : cursor % count;
}

// A new AI life must not inherit the previous life\'s reaction state. Keeping
// the hit timestamp makes the bot strafe/flee immediately after spawning.
inline void resetBotMotion(float& patrolOffset, float& moveYaw, float& animTime,
                           float& lastHitTime) {
    patrolOffset = 0.0f;
    moveYaw = 0.0f;
    animTime = 0.0f;
    lastHitTime = NeverHitTime;
}
}
