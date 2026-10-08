#include "game/debris_physics.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }

int main() {
    // randomDir stays within theta of the axis.
    {
        TimelineRandom random = timelineRandom({1.0, 2.0});
        for (int i = 0; i < 200; ++i) {
            const Point3F d = DebrisPhysics::randomDir({0, 1, 0}, 0.0f, 50.0f, 0.0f, 360.0f, random);
            const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            assert(near(length, 1.0f, 1e-4f));
            assert(d.y >= std::cos(50.0f * Math::PI / 180.0f) - 1e-4f);
        }
        const Point3F flat = DebrisPhysics::randomDir({0, 1, 0}, 90.0f, 90.0f, 0.0f, 0.0f, random);
        assert(near(flat.y, 0.0f, 1e-4f));
    }
    V12::DecodedDataBlock::DebrisData data;
    data.lifetimeMS = 3000;
    data.lifetimeVarianceMS = 1000;
    data.numBounces = 2;
    data.elasticity = 0.5f;
    data.friction = 0.0f;
    data.minSpin = 100.0f;
    data.maxSpin = 100.0f;
    // onAdd: lifetime + v * (4r - 3) lies in [lifetime - 3v, lifetime + v].
    {
        TimelineRandom random = timelineRandom({3.0});
        for (int i = 0; i < 100; ++i) {
            const auto b = DebrisPhysics::init(data, {0, 0, 0}, {0, 1, 0}, 0.2f, random);
            assert(b.lifetime >= 0.0f - 1e-4f && b.lifetime <= 4.0f + 1e-4f);
            assert(near(b.spinX, 100.0f, 1e-3f));
            assert(b.spinZ >= 10.0f - 1e-3f && b.spinZ <= 50.0f + 1e-3f);
        }
    }
    // A non-zero datablock velocity replaces the launch speed, keeping the
    // direction; useRadiusMass scales by baseRadius / radius.
    {
        auto fast = data;
        fast.velocity = 10.0f;
        fast.useRadiusMass = true;
        fast.baseRadius = 1.0f;
        TimelineRandom random = timelineRandom({4.0});
        const auto b = DebrisPhysics::init(fast, {0, 0, 0}, {0, 0, 3}, 2.0f, random);
        assert(near(b.vel.z, 10.0f, 1e-4f) && near(b.vel.x, 0.0f, 1e-6f));
        assert(near(b.radius, 2.0f, 1e-6f));
        assert(near(b.elasticity, 0.25f, 1e-6f));
        const auto small = DebrisPhysics::init(fast, {0, 0, 0}, {0, 0, 3}, 0.2f, random);
        assert(near(small.radius, 1.0f, 1e-6f) && near(small.elasticity, 0.5f, 1e-6f));
    }
    // Falls under gravity, bounces off a floor at y = 0 with elasticity, and
    // reports its last bounce.
    {
        TimelineRandom random = timelineRandom({5.0});
        auto b = DebrisPhysics::init(data, {0, 1, 0}, {0, -5, 0}, 0.2f, random);
        b.bounces = 2;
        const ProjectilePhysics::CastRay floor = [](const Point3F& a, const Point3F& c,
                                                    ProjectilePhysics::RayHit& hit) {
            if (a.y >= 0.0f && c.y < 0.0f) {
                hit.t = a.y / (a.y - c.y);
                hit.normal = {0, 1, 0};
                return true;
            }
            return false;
        };
        int bounced = 0;
        DebrisPhysics::StepResult last = DebrisPhysics::StepResult::None;
        for (int i = 0; i < 200 && last == DebrisPhysics::StepResult::None; ++i) {
            const float vy = b.vel.y;
            last = DebrisPhysics::step(b, data, -20.0f, floor);
            if (b.vel.y > 0.0f && vy < 0.0f) {
                ++bounced;
                assert(b.spinX < 100.0f);
            }
        }
        assert(last == DebrisPhysics::StepResult::MaxBounce);
        assert(bounced == 2);
        assert(b.pos.y > -0.5f);
    }
    // staticOnMaxBounce freezes the piece.
    {
        auto sticky = data;
        sticky.numBounces = 1;
        sticky.staticOnMaxBounce = true;
        TimelineRandom random = timelineRandom({6.0});
        auto b = DebrisPhysics::init(sticky, {0, 0.1f, 0}, {0, -5, 0}, 0.2f, random);
        const ProjectilePhysics::CastRay floor = [](const Point3F& a, const Point3F& c,
                                                    ProjectilePhysics::RayHit& hit) {
            if (a.y >= 0.0f && c.y < 0.0f) { hit.t = a.y / (a.y - c.y); hit.normal = {0, 1, 0}; return true; }
            return false;
        };
        assert(DebrisPhysics::step(b, sticky, -20.0f, floor) == DebrisPhysics::StepResult::MaxBounce);
        const Point3F at = b.pos;
        DebrisPhysics::step(b, sticky, -20.0f, floor);
        assert(b.stationary && at.y == b.pos.y && at.x == b.pos.x);
    }
    return 0;
}
