#include "sim/projectile_aim.h"
#include "sim/engine_classes.h"
#include "sim/net_object.h"
#include "script/script_engine.h"
#include <cmath>
#include <utility>

namespace ProjectileAim {

namespace {

constexpr double EqnEpsilon = 1e-8;

Point3F sub(const Point3F& a, const Point3F& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point3F add(const Point3F& a, const Point3F& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Point3F mul(const Point3F& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(const Point3F& a) { return std::sqrt(dot(a, a)); }
bool zero(const Point3F& a) { return a.x == 0 && a.y == 0 && a.z == 0; }
// Point3F::normalize / normalizeSafe.
Point3F normalize(const Point3F& a) {
    const float l = len(a);
    return l > 0 ? mul(a, 1.0f / l) : a;
}
Point3F normalizeSafe(const Point3F& a) {
    const float l = len(a);
    return l > 1e-4f ? mul(a, 1.0f / l) : a;
}

bool fail(Point3F* vMin, float* tMin, Point3F* vMax, float* tMax) {
    *vMin = {0, 0, 1};
    *vMax = {0, 0, 1};
    *tMin = -1;
    *tMax = -1;
    return false;
}

// LinearProjectileData / EnergyProjectileData: a straight shot at `speed`,
// solved by the law of cosines.
bool straightAim(float speed, float velInheritFactor, float lifetimeMS, const Point3F& targetPos,
                 const Point3F& targetVel, const Point3F& sourcePos, const Point3F& sourceVel, Point3F* vMin,
                 float* tMin, Point3F* vMax, float* tMax) {
    const Point3F effTargetPos = sub(targetPos, sourcePos);
    const Point3F effTargetVel = sub(targetVel, mul(sourceVel, velInheritFactor));
    const Point3F normPos = zero(effTargetPos) ? effTargetPos : normalize(effTargetPos);
    const Point3F normVel = zero(effTargetVel) ? effTargetVel : normalize(effTargetVel);
    const float a = dot(effTargetVel, effTargetVel) - speed * speed;
    const float b = 2 * len(effTargetPos) * len(effTargetVel) * dot(normPos, normVel);
    const float c = dot(effTargetPos, effTargetPos);
    const float det = b * b - 4 * a * c;
    if (det < 0.0) return fail(vMin, tMin, vMax, tMax);
    const float sol1 = (-b + std::sqrt(det)) / (2 * a);
    const float sol2 = (-b - std::sqrt(det)) / (2 * a);
    const float t = sol2 > 0.0 ? sol2 : sol1;
    if (t < 0.0) return fail(vMin, tMin, vMax, tMax);
    const Point3F finalAnswer = normalize(add(mul(effTargetPos, 1.0f / (speed * t)), mul(effTargetVel, 1.0f / speed)));
    *vMin = *vMax = finalAnswer;
    *tMin = *tMax = t;
    return (t * 1000.0) <= lifetimeMS;
}

// GrenadeProjectileData: the ballistic quartic in the flight time.
bool grenadeAim(ScriptObject* d, const Point3F& targetPos, const Point3F& targetVel, const Point3F& sourcePos,
                const Point3F& sourceVel, Point3F* vMin, float* tMin, Point3F* vMax, float* tMax) {
    const float velInheritFactor = Fields::f32(d, "velInheritFactor", 1.0f);
    const float muzzleVelocity = Fields::f32(d, "muzzleVelocity", 75);
    const float lifetimeMS = Fields::f32(d, "lifetimeMS", 20000);
    const Point3F p = sub(targetPos, sourcePos);
    const Point3F v = sub(targetVel, mul(sourceVel, velInheritFactor));
    const float g = 9.81f * Fields::f32(d, "gravityMod", 1.0f);
    float x[4];
    const uint32_t numRealSolutions = solveQuartic(0.25f * (g * g), v.z * g,
                                                   p.z * g + dot(v, v) - muzzleVelocity * muzzleVelocity,
                                                   2 * dot(p, v), dot(p, p), x);
    if (numRealSolutions == 0) return fail(vMin, tMin, vMax, tMax);
    float minPos[2] = {1e8, 1e8};
    for (uint32_t i = 0; i < numRealSolutions; ++i)
        if (x[i] > 0.0) {
            if (x[i] < minPos[0]) {
                minPos[1] = minPos[0];
                minPos[0] = x[i];
            } else if (x[i] < minPos[1]) {
                minPos[1] = x[i];
            }
        }
    if (minPos[0] == 1e8f) return fail(vMin, tMin, vMax, tMax);
    if (minPos[1] == 1e8f) minPos[1] = minPos[0];
    auto aim = [&](float t) {
        Point3F out = add(targetPos, mul(targetVel, t));
        out = sub(out, mul(Point3F{0, 0, -g}, 0.5f * t * t));
        out = mul(sub(out, sourcePos), 1.0f / t);
        out = mul(sub(out, mul(sourceVel, velInheritFactor)), 1.0f / muzzleVelocity);
        return normalize(out);
    };
    *vMin = aim(minPos[0]);
    *vMax = aim(minPos[1]);
    *tMin = minPos[0];
    *tMax = minPos[1];
    return (*tMin * 1000.0) < lifetimeMS;
}

// The instant/beam weapons: straight at the target within a range.
bool rangedAim(float range, bool inclusive, float time, bool safe, const Point3F& targetPos,
               const Point3F& sourcePos, Point3F* vMin, float* tMin, Point3F* vMax, float* tMax) {
    const Point3F d = sub(targetPos, sourcePos);
    if (inclusive ? len(d) >= range : len(d) > range) return false;
    *vMin = *vMax = safe ? normalizeSafe(d) : normalize(d);
    *tMin = *tMax = time;
    return true;
}

} // namespace

namespace {
// The shipped solvers ran on the x87 with its default 53-bit precision, the
// F32 intermediates never rounded to single: in single precision a double
// root's resolvent term 2z - p lands past EqnEpsilon and loses real roots.
bool isZeroD(double v) { return v > -EqnEpsilon && v < EqnEpsilon; }
double cbrtD(double v) { return v < 0.0 ? -std::pow(-v, 1.0 / 3.0) : std::pow(v, 1.0 / 3.0); }
uint32_t solveLinearD(double a, double b, double* x) {
    if (isZeroD(a)) return 0;
    x[0] = -b / a;
    return 1;
}
void sortRootsD(double* x, uint32_t num) {
    for (int j = 0; j < (int)num - 1; ++j)
        for (int k = j + 1; k < (int)num; ++k)
            if (x[k] < x[j]) std::swap(x[k], x[j]);
}

uint32_t solveQuadraticD(double a, double b, double c, double* x) {
    // really linear?
    if (isZeroD(a)) return solveLinearD(b, c, x);
    const double desc = (b * b) - (4.0 * a * c);
    // The shipped roots are (b +- sqrt(desc)) / 2a: b's sign is not negated
    // (the quartic's two quadratics then trade their roots).
    if (isZeroD(desc)) {
        x[0] = b / (2.0 * a);
        return 1;
    }
    if (desc > 0.0) {
        const double sqrdesc = std::sqrt(desc), den = 2.0 * a;
        x[0] = (b + sqrdesc) / den;
        x[1] = (b - sqrdesc) / den;
        if (x[1] < x[0]) std::swap(x[0], x[1]);
        return 2;
    }
    return 0;
}

// Graphics Gems I, pp 738-742.
uint32_t solveCubicD(double a, double b, double c, double d, double* x) {
    if (isZeroD(a)) return solveQuadraticD(b, c, d, x);
    const double A = b / a, B = c / a, C = d / a;
    const double A2 = A * A, A3 = A2 * A;
    const double p = (1.0 / 3.0) * (((-1.0 / 3.0) * A2) + B);
    const double q = (1.0 / 2.0) * (((2.0 / 27.0) * A3) - ((1.0 / 3.0) * A * B) + C);
    const double p3 = p * p * p, q2 = q * q, D = q2 + p3;
    uint32_t num = 0;
    if (isZeroD(D)) {
        if (isZeroD(q)) {
            x[0] = 0.0;
            num = 1;
        } else {
            const double u = cbrtD(-q);
            x[0] = 2.0 * u;
            x[1] = -u;
            num = 2;
        }
    } else if (D < 0.0) {
        const double phi = (1.0 / 3.0) * std::acos(-q / std::sqrt(-p3));
        const double t = 2.0 * std::sqrt(-p);
        x[0] = t * std::cos(phi);
        x[1] = -t * std::cos(phi + (M_PI / 3.0));
        x[2] = -t * std::cos(phi - (M_PI / 3.0));
        num = 3;
    } else {
        const double sqrtD = std::sqrt(D);
        x[0] = cbrtD(sqrtD - q) - cbrtD(sqrtD + q);
        num = 1;
    }
    const double s = (1.0 / 3.0) * A;
    for (uint32_t i = 0; i < num; ++i) x[i] -= s;
    sortRootsD(x, num);
    return num;
}

uint32_t solveQuarticD(double a, double b, double c, double d, double e, double* x) {
    if (isZeroD(a)) return solveCubicD(b, c, d, e, x);
    const double A = b / a, B = c / a, C = d / a, D = e / a;
    const double A2 = A * A, A3 = A2 * A, A4 = A2 * A2;
    const double p = ((-3.0 / 8.0) * A2) + B;
    const double q = ((1.0 / 8.0) * A3) - ((1.0 / 2.0) * A * B) + C;
    const double r = ((-3.0 / 256.0) * A4) + ((1.0 / 16.0) * A2 * B) - ((1.0 / 4.0) * A * C) + D;
    uint32_t num = 0;
    if (isZeroD(r)) {
        // no absolute term: y(y^3 + py + q) = 0
        num = solveCubicD(1.0, 0.0, p, q, x);
        x[num++] = 0.0;
    } else {
        // solve the resolvent cubic
        solveCubicD(1.0, (-1.0 / 2.0) * p, -r, ((1.0 / 2.0) * r * p) - ((1.0 / 8.0) * q * q), x);
        const double z = x[0];
        // build 2 quadratic equations from the one solution
        double u = (z * z) - r, v = (2.0 * z) - p;
        if (isZeroD(u)) u = 0.0;
        else if (u > 0.0) u = std::sqrt(u);
        else return 0;
        if (isZeroD(v)) v = 0.0;
        else if (v > 0.0) v = std::sqrt(v);
        else return 0;
        num = solveQuadraticD(1.0, v, z - u, x);
        num += solveQuadraticD(1.0, -v, z + u, x + num);
    }
    const double s = (1.0 / 4.0) * A;
    for (uint32_t i = 0; i < num; ++i) x[i] -= s;
    sortRootsD(x, num);
    return num;
}

} // namespace

uint32_t solveQuadratic(float a, float b, float c, float* x) {
    double r[2];
    const uint32_t n = solveQuadraticD(a, b, c, r);
    for (uint32_t i = 0; i < n; ++i) x[i] = (float)r[i];
    return n;
}

uint32_t solveCubic(float a, float b, float c, float d, float* x) {
    double r[3];
    const uint32_t n = solveCubicD(a, b, c, d, r);
    for (uint32_t i = 0; i < n; ++i) x[i] = (float)r[i];
    return n;
}

uint32_t solveQuartic(float a, float b, float c, float d, float e, float* x) {
    double r[4];
    const uint32_t n = solveQuarticD(a, b, c, d, e, r);
    for (uint32_t i = 0; i < n; ++i) x[i] = (float)r[i];
    return n;
}

bool calculateAim(ScriptObject* data, const Point3F& targetPos, const Point3F& targetVel, const Point3F& sourcePos,
                  const Point3F& sourceVel, Point3F* vMin, float* tMin, Point3F* vMax, float* tMax) {
    if (!data) return false;
    const std::string& cls = data->className;
    auto is = [&](const char* base) { return EngineClasses::isA(cls, base); };
    if (is("EnergyProjectileData"))
        return straightAim(Fields::f32(data, "muzzleVelocity", 100.0f), Fields::f32(data, "velInheritFactor", 1.0f),
                           Fields::f32(data, "lifetimeMS", 3000), targetPos, targetVel, sourcePos, sourceVel, vMin,
                           tMin, vMax, tMax);
    if (is("GrenadeProjectileData"))
        return grenadeAim(data, targetPos, targetVel, sourcePos, sourceVel, vMin, tMin, vMax, tMax);
    if (is("LinearProjectileData"))
        return straightAim(Fields::f32(data, "dryVelocity", 5.0f), Fields::f32(data, "velInheritFactor", 1.0f),
                           Fields::f32(data, "lifetimeMS", 1000), targetPos, targetVel, sourcePos, sourceVel, vMin,
                           tMin, vMax, tMax);
    if (is("SeekerProjectileData")) {
        const float lifetime = Fields::f32(data, "lifetimeMS", 3000) / 1000.0f;
        const float maxVelocity = Fields::f32(data, "maxVelocity", 65.0f);
        if (len(add(sub(targetPos, sourcePos), mul(targetVel, lifetime))) > lifetime * maxVelocity)
            return fail(vMin, tMin, vMax, tMax);
        const Point3F d = sub(targetPos, sourcePos);
        *tMin = *tMax = len(d) / maxVelocity;
        *vMin = *vMax = normalize(d);
        return true;
    }
    if (is("SniperProjectileData") || is("TargetProjectileData")) {
        if (len(sub(targetPos, sourcePos)) > Fields::f32(data, "maxRifleRange", 1000))
            return fail(vMin, tMin, vMax, tMax);
        return rangedAim(1e30f, false, 0.0f, true, targetPos, sourcePos, vMin, tMin, vMax, tMax);
    }
    if (is("ELFProjectileData")) {
        if (len(sub(targetPos, sourcePos)) > Fields::f32(data, "beamRange", 10.0f))
            return fail(vMin, tMin, vMax, tMax);
        return rangedAim(1e30f, false, 0.0f, false, targetPos, sourcePos, vMin, tMin, vMax, tMax);
    }
    if (is("ShockLanceProjectileData"))
        return rangedAim(Fields::f32(data, "boltLength", 2.0f) + 2.0f, true, 0.001f, true, targetPos, sourcePos,
                         vMin, tMin, vMax, tMax);
    if (is("RepairProjectileData"))
        return rangedAim(Fields::f32(data, "beamRange", 10.0f), true, 0.001f, false, targetPos, sourcePos, vMin,
                         tMin, vMax, tMax);
    // ProjectileData::calculateAim: "essentially pure virtual".
    return false;
}

} // namespace ProjectileAim
