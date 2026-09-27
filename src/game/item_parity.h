#pragma once

#include "core/math.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

enum class ItemKind { None, Health, Energy, Ammo };

constexpr float defaultItemPickupAmount(ItemKind kind) {
    return kind == ItemKind::Ammo ? 10.0f : 25.0f;
}

constexpr float defaultItemRespawnDelay() {
    return 20.0f;
}

inline float itemPickupAmount(ItemKind kind, float authoredAmount) {
    // Datablock amounts are script data. Native items fall back to their
    // stock amount when a custom value is missing or malformed.
    return std::isfinite(authoredAmount) && authoredAmount > 0.0f
        ? authoredAmount : defaultItemPickupAmount(kind);
}

inline float itemRespawnDelay(float authoredDelay) {
    return std::isfinite(authoredDelay) && authoredDelay > 0.0f
        ? authoredDelay : defaultItemRespawnDelay();
}

// A consumed item must not remain invisible when a script or ghost update
// supplies a malformed countdown. Native item state treats that timer as
// expired and makes the pickup available again.
inline bool itemRespawnReady(float respawnTimer, float dt) {
    // An already-expired timer is ready even on a paused tick.  Requiring a
    // positive dt here leaves items permanently hidden when the final timer
    // update lands on an otherwise zero-delta frame.
    return !std::isfinite(respawnTimer) || respawnTimer <= 0.0f ||
           (std::isfinite(dt) && dt > 0.0f && respawnTimer <= dt);
}

// ShapeBase item triggers include their boundary.  Keeping the comparison
// inclusive prevents a pickup at exactly the trigger radius from being left
// floating in the world.
inline bool itemWithinPickupRange(float distance, float radius = 2.0f) {
    return std::isfinite(distance) && std::isfinite(radius) &&
           radius >= 0.0f && distance <= radius;
}

// Hidden mission Items are not part of the native pickup trigger either.
inline bool itemCanBeCollected(bool enabled, bool active, bool missionVisible) {
    return enabled && active && missionVisible;
}

// Script toggles must not bypass a consumed item's respawn countdown. An item
// that was active when disabled may still reactivate immediately.
inline bool itemActiveAfterEnable(bool wasActive, float respawnTimer) {
    return wasActive || !std::isfinite(respawnTimer) || respawnTimer <= 0.0f;
}

// Mission item positions use Torque's Z-up frame; gameplay uses Torch's Y-up frame.
inline Point3F itemWorldPosition(const Point3F& authoredPosition) {
    return Math::torquePointToYUp(authoredPosition);
}

inline ItemKind classifyItemKind(std::string name) {
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.find("health") != std::string::npos || name.find("repair") != std::string::npos)
        return ItemKind::Health;
    if (name.find("energy") != std::string::npos)
        return ItemKind::Energy;
    if (name.find("ammo") != std::string::npos)
        return ItemKind::Ammo;
    return ItemKind::None;
}

// Torque resolves datablock/object names without regard to case. Mission item
// properties must therefore use the same lookup semantics as the script VM.
inline bool itemDatablockNameEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

inline float applyItemAmount(ItemKind kind, float value, float amount, float maximum) {
    // Malformed mission fields must not poison player state. Torque rejects
    // non-finite pickup amounts rather than propagating NaN through health,
    // energy, or ammo arithmetic.
    if (!std::isfinite(value) || !std::isfinite(amount) ||
        !std::isfinite(maximum)) return value;
    if (kind == ItemKind::Ammo) {
        // Tribes 2 uses negative ammo counts for weapons that do not consume
        // inventory ammo.  Pickups must not convert that sentinel to a finite
        // reserve.
        if (value < 0.0f) return value;
        const float result = value + std::max(0.0f, amount);
        return maximum > 0.0f ? std::min(result, maximum) : result;
    }
    return std::clamp(value + std::max(0.0f, amount), 0.0f, maximum);
}

inline bool itemPickupWouldApply(ItemKind kind, float value, float amount, float maximum) {
    if (kind == ItemKind::None || !std::isfinite(value) ||
        !std::isfinite(amount) || !std::isfinite(maximum) || amount <= 0.0f)
        return false;
    if (kind == ItemKind::Ammo && value < 0.0f) return false;
    if (kind == ItemKind::Ammo && maximum <= 0.0f) return true;
    return value < maximum;
}
