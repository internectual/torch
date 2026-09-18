#pragma once

#include <cstddef>

namespace DeathRespawn {
constexpr float RespawnDelay = 3.0f;

inline bool crossesIntoDeath(float healthBefore, float damage) {
    return healthBefore > 0.0f && healthBefore - damage <= 0.0f;
}

inline bool respawnDue(float now, float respawnAt) {
    return respawnAt > 0.0f && now >= respawnAt;
}

// Stable round-robin selection avoids making gameplay tests depend on rand().
inline std::size_t nextSpawn(std::size_t cursor, std::size_t count) {
    return count == 0 ? 0 : cursor % count;
}
}
