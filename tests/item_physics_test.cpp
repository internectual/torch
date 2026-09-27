#include "game/item_physics.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// Flat ground at y = 0 for the cast.
static bool ground(const Point3F& a, const Point3F& b, ProjectilePhysics::RayHit& hit) {
    if (a.y >= 0.0f && b.y < 0.0f) {
        hit.t = a.y / (a.y - b.y);
        hit.point = {a.x + (b.x - a.x) * hit.t, 0.0f, a.z + (b.z - a.z) * hit.t};
        hit.normal = {0, 1, 0};
        return true;
    }
    return false;
}

int main() {
    ItemPhysics::Params params;
    params.friction = 0.6f;
    params.elasticity = 0.2f;
    // Free fall: gravity -20 x gravityMod per 32 ms tick.
    Point3F pos{0, 10, 0}, vel{0, 0, 0};
    assert(!ItemPhysics::step(pos, vel, params, ground));
    assert(near(vel.y, -0.64f));
    assert(near(pos.y, 10.0f - 0.64f * 0.032f));
    params.gravityMod = 0.5f;
    vel = {0, 0, 0};
    ItemPhysics::step(pos, vel, params, ground);
    assert(near(vel.y, -0.32f));
    params.gravityMod = 1.0f;

    // maxVelocity pulls back 10% of the excess over 1.05 x max.
    params.maxVelocity = 10.0f;
    pos = {0, 100, 0};
    vel = {20, 0.64f, 0};
    ItemPhysics::step(pos, vel, params, ground);
    assert(near(vel.x, 20.0f - 20.0f * (1.0f - 10.0f / 20.0f) * 0.1f, 1e-3f));
    params.maxVelocity = -1.0f;

    // Dropped onto the ground it bounces less each time and comes to rest.
    pos = {0, 1, 0};
    vel = {2, 0, 0};
    bool rest = false;
    for (int i = 0; i < 400 && !rest; ++i) rest = ItemPhysics::step(pos, vel, params, ground);
    assert(rest);
    assert(vel.x == 0.0f && vel.y == 0.0f && vel.z == 0.0f);
    assert(pos.y >= 0.0f && pos.y < 0.05f);

    // Sticky items stop at the first contact.
    params.sticky = true;
    pos = {0, 0.01f, 0};
    vel = {0, -5, 0};
    assert(ItemPhysics::step(pos, vel, params, ground));
    return 0;
}
