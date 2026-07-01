#pragma once
#include "core/math.h"
#include "game/game.h"

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

    void resolveCollision(Player* player, Point3F& pos, float dt);

private:
    float gravity = -20.0f;
    float friction = 0.85f;
    float airFriction = 0.95f;
    float jetDrain = 25.0f;
    float jetRecharge = 15.0f;
};
