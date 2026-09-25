#pragma once
#include "core/math.h"
#include "game/game.h"
#include <cmath>

// Camera and interaction traces are presentation-only, but they still feed
// terrain and interior collision. Reject an incomplete view ray before its
// normalization can turn the trace and impact effects into NaNs.
inline bool rayCastInputUsable(const Point3F& direction, float maxDistance) {
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(maxDistance) ||
        maxDistance <= 0.0f)
        return false;
    const float length = std::sqrt(direction.x * direction.x +
                                   direction.y * direction.y +
                                   direction.z * direction.z);
    return std::isfinite(length) && length > 1.0e-4f;
}

class Physics {
public:
    Physics();
    ~Physics();

    void update(Player* player, float dt, const Game::InputMove& input);

    // Collision
    struct RayCastResult {
        bool hit = false;
        Point3F point;
        Point3F normal;
        float distance = 0;
    };

    RayCastResult rayCast(const Point3F& origin, const Point3F& dir, float maxDist);

    // Gravity
    void setGravity(float g) { gravity = g; }
    float getGravity() const { return gravity; }

    void resolveCollision(Player* player, Point3F& pos, Point3F& velocity, float dt);

private:
    float gravity = -20.0f;
    float friction = 0.85f;
    float airFriction = 0.95f;
    float jetDrain = 25.0f;
    float jetRecharge = 15.0f;
};
