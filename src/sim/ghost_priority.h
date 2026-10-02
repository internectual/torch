#pragma once

#include "core/math.h"
#include <algorithm>
#include <cmath>

namespace GhostPriority {

struct Scope {
    Point3F position{};
    Point3F forward{0, 1, 0};
    float visibleDistance = 1.0f;
    float cosFov = 0.7071f;
};

struct Interest {
    bool player = false;
    bool projectile = false;
    bool item = false;
    bool ownedProjectile = false;
};

inline float score(const Point3F& center, const Point3F& velocity, const Scope& scope,
                   const Interest& interest, int updateSkips, bool gameBase = true) {
    if (!gameBase) return updateSkips * 0.1f;

    Point3F direction{center.x - scope.position.x, center.y - scope.position.y,
                       center.z - scope.position.z};
    float distance = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (distance == 0.0f) distance = 0.001f;
    direction.x /= distance;
    direction.y /= distance;
    direction.z /= distance;

    const float visible = std::max(0.0f, scope.visibleDistance);
    const float distanceWeight = distance < visible ? 1.0f - distance / visible : 0.0f;
    const float facing = direction.x * scope.forward.x + direction.y * scope.forward.y + direction.z * scope.forward.z;
    const bool inFov = facing > scope.cosFov;
    float velocityWeight = 0.0f;
    if (inFov && visible > 0.0f) {
        const float cx = scope.forward.y * velocity.z - scope.forward.z * velocity.y;
        const float cy = scope.forward.z * velocity.x - scope.forward.x * velocity.z;
        const float cz = scope.forward.x * velocity.y - scope.forward.y * velocity.x;
        velocityWeight = std::min(1.0f, std::sqrt(cx * cx + cy * cy + cz * cz) / visible);
    }

    float interestWeight = 0.0f;
    if (interest.player) {
        interestWeight = 0.75f;
    } else if (interest.projectile) {
        interestWeight = 0.30f;
        const float toward = -(direction.x * velocity.x + direction.y * velocity.y + direction.z * velocity.z);
        if (toward > 0.0f) interestWeight += 0.20f * toward;
    } else if (interest.item) {
        interestWeight = 0.25f;
    }

    return (inFov ? 1.0f : 0.0f) + distanceWeight * 0.4f + velocityWeight * 0.4f +
           (updateSkips * 0.5f) * 0.2f + interestWeight * 0.2f +
           (interest.ownedProjectile ? 0.2f : 0.0f);
}

} // namespace GhostPriority
