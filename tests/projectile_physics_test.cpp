#include "game/projectile_physics.h"
#include <cassert>
#include <cmath>

using namespace ProjectilePhysics;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

int main() {
    // Mirror bounce off the floor with no friction or damping.
    Point3F v = bounceVelocity({3, -4, 0}, {0, 1, 0}, 0.0f, 1.0f);
    assert(near(v.x, 3) && near(v.y, 4));
    // Friction removes tangential speed; elasticity scales the result.
    v = bounceVelocity({3, -4, 0}, {0, 1, 0}, 0.5f, 0.5f);
    assert(near(v.x, 0.75f) && near(v.y, 2.0f));

    // A floor at y = 0.
    const CastRay floor = [](const Point3F& a, const Point3F& b, RayHit& hit) {
        if (a.y < 0.0f || b.y >= 0.0f) return false;
        hit.t = a.y / (a.y - b.y);
        hit.point = {a.x + (b.x - a.x) * hit.t, 0.0f, a.z + (b.z - a.z) * hit.t};
        hit.normal = {0, 1, 0};
        return true;
    };
    // Free flight gains gravity each tick.
    Point3F pos{0, 10, 0}, vel{1, 0, 0};
    assert(stepBallistic(pos, vel, -20.0f, 1.0f, 0.0f, false, floor));
    assert(near(vel.y, -0.64f) && near(pos.x, 0.032f) && pos.y < 10.0f);
    // Unarmed: bounces up off the floor.
    pos = {0, 0.01f, 0}; vel = {0, -10, 0};
    assert(stepBallistic(pos, vel, -20.0f, 0.5f, 0.0f, false, floor));
    assert(vel.y > 0.0f && pos.y > 0.0f);
    // Armed: stops at the contact.
    pos = {0, 0.01f, 0}; vel = {0, -10, 0};
    assert(!stepBallistic(pos, vel, -20.0f, 0.5f, 0.0f, true, floor));
    assert(near(pos.y, 0.0f) && vel.y == 0.0f);
    return 0;
}
