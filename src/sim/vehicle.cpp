#include "sim/vehicle.h"
#include "sim/force_field.h"
#include "sim/containers.h"
#include "sim/player.h"
#include "sim/server_container.h"
#include "net/torque_bit_writer.h"
#include "render/dts_loader.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <strings.h>
#include <unordered_map>

using namespace SimContainer;
using TorqueMath::Matrix;
using TorqueMath::Quat;

namespace {

constexpr float TickSec = 0.032f;
constexpr float VehicleGravity = -20;  // sHoverVehicleGravity / sFlyingVehicleGravity
constexpr float CollisionTol = 0.07f;  // sCollisionTol: distance to maintain
constexpr float IntersectionTol = 0.01f;
constexpr float ContactTol = 0.5f;     // sContactTol: collision contact velocity
constexpr float SpringForce = 400;     // sF
constexpr float SpringDamping = 2;     // sD
// Deepest a hull vertex may be behind a surface and still touch it (deeper
// is the far side of a wall; crossing a face is caught as it happens).
constexpr float MaxPenetration = 0.1f;

// sCollisionMoveMask (hoverVehicle.cc, flyingVehicle.cc).
constexpr uint32_t CollisionMoveMask = TerrainObjectType | InteriorObjectType | WaterObjectType |
                                       PlayerObjectType | StaticShapeObjectType | VehicleObjectType |
                                       VehicleBlockerObjectType | ForceFieldObjectType | StaticTSObjectType;

Point3F mulV(const Matrix& m, const Point3F& v) {
    const float in[3] = {v.x, v.y, v.z};
    float out[3];
    TorqueMath::mulV(m, in, out);
    return {out[0], out[1], out[2]};
}

Point3F mulP(const Matrix& m, const Point3F& p) {
    const float in[3] = {p.x, p.y, p.z};
    float out[3];
    TorqueMath::mulP(m, in, out);
    return {out[0], out[1], out[2]};
}

Point3F column(const Matrix& m, int c) { return {m[c], m[4 + c], m[8 + c]}; }

Matrix transpose3(const Matrix& m) {
    Matrix t = m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t[r * 4 + c] = m[c * 4 + r];
    return t;
}

std::string vec3(const Point3F& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%f %f %f", v.x, v.y, v.z);
    return buffer;
}

std::string f32(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%f", v);
    return buffer;
}

TorqueBitWriter::Point wp(const Point3F& p) { return {p.x, p.y, p.z}; }

int intGlobal(const char* name) {
    auto* ts = ScriptEngine::instance().ts();
    return ts ? ts->getGlobal(name).toInt() : 0;
}

} // namespace

//----------------------------------------------------------------------------
// Rigid

Point3F Rigid::State::getVelocity(const Point3F& r) const { return add(cross(angVelocity, r), linVelocity); }

Matrix Rigid::State::getTransform() const {
    Matrix mat = TorqueMath::matrix(angPosition);
    mat[3] = linPosition.x; mat[7] = linPosition.y; mat[11] = linPosition.z;
    return mat;
}

void Rigid::State::setTransform(const Matrix& mat) {
    angPosition = TorqueMath::quat(mat);
    linPosition = {mat[3], mat[7], mat[11]};
}

void Rigid::integrate(State& t, float delta) const {
    t.linPosition = add(t.linPosition, mul(t.linVelocity, delta));
    t.linMomentum = add(t.linMomentum, mul(t.force, delta));
    t.linVelocity = mul(t.linMomentum, oneOverMass);
    // The rotation rate is the rigid's current angular velocity (state, not t).
    const float angle = len(state.angVelocity);
    if (angle != 0.0f) {
        Quat dq;
        float sinHalfAngle = std::sin(angle * delta * -0.5f);
        dq.w = std::cos(angle * delta * -0.5f);
        sinHalfAngle *= 1 / angle;
        dq.x = t.angVelocity.x * sinHalfAngle;
        dq.y = t.angVelocity.y * sinHalfAngle;
        dq.z = t.angVelocity.z * sinHalfAngle;
        t.angPosition = TorqueMath::normalize(TorqueMath::mul(t.angPosition, dq));
    }
    t.angMomentum = add(t.angMomentum, mul(t.torque, delta));
    // Move angular momentum into world space.
    const Matrix qmat = TorqueMath::matrix(t.angPosition);
    t.invWorldInertia = TorqueMath::mul(TorqueMath::mul(qmat, invObjectInertia), transpose3(qmat));
    t.angVelocity = mulV(t.invWorldInertia, t.angMomentum);
}

void Rigid::updateVelocity(State& t) const {
    t.linVelocity = mul(t.linMomentum, oneOverMass);
    t.angVelocity = mulV(t.invWorldInertia, t.angMomentum);
}

void Rigid::applyImpulse(State& t, const Point3F& r, const Point3F& impulse) {
    atRest = false;
    t.linMomentum = add(t.linMomentum, impulse);
    t.linVelocity = mul(t.linMomentum, oneOverMass);
    t.angMomentum = add(t.angMomentum, cross(r, impulse));
    t.angVelocity = mulV(t.invWorldInertia, t.angMomentum);
}

bool Rigid::resolveCollision(State& s, const Point3F& p, const Point3F& normal) {
    atRest = false;
    const Point3F r1 = sub(p, s.linPosition);
    const Point3F v1 = s.getVelocity(r1);
    const float n = -dot(v1, normal);
    if (n >= 0) {
        // Collision impulse
        const float d = getZeroImpulse(s, r1, normal);
        const float j = n * (1 + restitution) * d;
        Point3F impulse = mul(normal, j);
        // Friction impulse
        Point3F uv = add(v1, mul(normal, n));
        const float ul = len(uv);
        if (ul) {
            uv = mul(uv, -1 / ul);
            const float u = n * d * friction * getZeroImpulse(s, r1, uv);
            impulse = add(impulse, mul(uv, u));
        }
        applyImpulse(s, r1, impulse);
    }
    return true;
}

float Rigid::getZeroImpulse(const State& s, const Point3F& r, const Point3F& normal) const {
    const Point3F c = cross(mulV(s.invWorldInertia, cross(r, normal)), r);
    return 1 / (oneOverMass + dot(c, normal));
}

void Rigid::setObjectInertia() {
    invObjectInertia = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Matrix qmat = TorqueMath::matrix(state.angPosition);
    state.invWorldInertia = TorqueMath::mul(TorqueMath::mul(qmat, invObjectInertia), transpose3(qmat));
}

//----------------------------------------------------------------------------
// Vehicle

// Vehicle::onAdd (with ShapeBase::onNewDataBlock's mass).
void VehicleObject::readFields() {
    ShapeBase::readFields();
    disableMove = Fields::boolean(script, "disableMove", disableMove);
    mass = data("mass", 1.0f);
    oneOverMass = mass != 0 ? 1 / mass : 1;
    const std::string shapeFile = shapeFileOf(*this);
    shapeFileBounds(shapeFile, objMin, objMax);
    hull = shapeCollisionHull(shapeFile);
    rigid.state.setTransform(transform);
    rigid.mass = 1;
    rigid.oneOverMass = 1 / rigid.mass;
    rigid.setObjectInertia();
}

void VehicleObject::setPosition(const Point3F& pos, const Quat& rot) {
    Matrix mat = TorqueMath::matrix(rot);
    mat[3] = pos.x; mat[7] = pos.y; mat[11] = pos.z;
    transform = mat;
}

void VehicleObject::setTransform(const Matrix& mat) {
    rigid.state.setTransform(mat);
    transform = mat;
    setMaskBits(PositionMask);
}

void VehicleObject::applyImpulse(const Point3F& pos, const Point3F& impulse) {
    const float mc[3] = {0, 0, 0}; // VehicleData::massCenter (always zero)
    float massCenter[3];
    TorqueMath::mulP(transform, mc, massCenter);
    const Point3F r = sub(pos, {massCenter[0], massCenter[1], massCenter[2]});
    rigid.applyImpulse(rigid.state, r, mul(impulse, oneOverMass));
}

void VehicleObject::setEnergyLevel(float level) {
    ShapeBase::setEnergyLevel(level);
    setMaskBits(EnergyMask);
}

void VehicleObject::setFrozenState(bool state) {
    frozen = state;
    setMaskBits(FrozenMask);
}

uint32_t VehicleObject::collisionMask() const { return CollisionMoveMask; }

// disableCollision: the vehicle and what is mounted on it.
std::vector<ScriptObject*> VehicleObject::collisionExempt() const {
    std::vector<ScriptObject*> exempt{script};
    for (const auto& key : mounted)
        if (ScriptObject* o = ScriptEngine::instance().findObject(key.c_str())) exempt.push_back(o);
    return exempt;
}

// APPROXIMATION of Convex::getCollisionInfo / findClosestStateBounded: the
// collision hull's vertices against the world triangles and the boxes of
// the shapes in the collision mask. A vertex within `tol` of a face it
// projects onto (and not deeper than MaxPenetration behind it) is a
// contact; a vertex that crossed a face since `from` intersects. The
// closest distance is negative while penetrating (so a step that backs out
// is taken), contacts carry it clamped at 0 as GJK reports an overlap.
float VehicleObject::collide(const Matrix& mat, float tol, std::vector<Contact>* contacts, const Matrix* from,
                             std::string* closestObject) {
    auto hullPoints = [&](const Matrix& m) {
        std::vector<Point3F> points;
        if (hull) {
            points.reserve(hull->size());
            for (const auto& p : *hull) points.push_back(mulP(m, {p.x * scale[0], p.y * scale[1], p.z * scale[2]}));
        } else {
            for (int c = 0; c < 8; ++c)
                points.push_back(mulP(m, {((c & 1) ? objMax[0] : objMin[0]) * scale[0],
                                          ((c & 2) ? objMax[1] : objMin[1]) * scale[1],
                                          ((c & 4) ? objMax[2] : objMin[2]) * scale[2]}));
        }
        return points;
    };
    const std::vector<Point3F> points = hullPoints(mat);
    const std::vector<Point3F> prev = from ? hullPoints(*from) : std::vector<Point3F>{};
    Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto* list : {&points, &prev})
        for (const auto& p : *list) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    const float pad = tol + MaxPenetration;
    lo = sub(lo, {pad, pad, pad});
    hi = add(hi, {pad, pad, pad});

    std::vector<PlayerPrediction::Triangle> tris;
    const uint32_t mask = collisionMask();
    if ((mask & (TerrainObjectType | InteriorObjectType)) && serverCollision().triangles)
        serverCollision().triangles(lo, hi, tris);
    if (mask & ForceFieldObjectType) ForceFields::gather(this, lo, hi, tris);
    // Shapes collide as their world boxes: a hull vertex within `tol` of a
    // box touches the face it is least deep behind (the closest feature).
    struct Box { Point3F min, max; std::string object; };
    std::vector<Box> boxes;
    const auto exempt = collisionExempt();
    for (ScriptObject* object : findObjects(lo, hi, mask & ~(TerrainObjectType | InteriorObjectType | WaterObjectType))) {
        if (std::find(exempt.begin(), exempt.end(), object) != exempt.end()) continue;
        auto* shape = dynamic_cast<ShapeBase*>(object->engine.get());
        Box box;
        float clo[3], chi[3];
        if (!shape) {
            // A VehicleBlocker's box convex.
            if (!(SimContainer::typeMask(object) & VehicleBlockerObjectType) ||
                !SimContainer::worldBox(object, box.min, box.max))
                continue;
        } else if (shape->hidden) {
            continue;
        } else if (!dynamic_cast<PlayerObject*>(shape) && Engine::instance().filesys) {
            if (!shape->collisionBox(clo, chi)) continue;
            box.min = {clo[0], clo[1], clo[2]};
            box.max = {chi[0], chi[1], chi[2]};
        } else if (!SimContainer::worldBox(object, box.min, box.max)) {
            continue;
        }
        box.object = ScriptEngine::instance().objectKey(object);
        boxes.push_back(box);
    }

    float closest = 1e7f;
    for (size_t i = 0; i < points.size(); ++i) {
        const Point3F& p = points[i];
        for (const auto& b : boxes) {
            const float gaps[6] = {b.min.x - p.x, p.x - b.max.x, b.min.y - p.y, p.y - b.max.y, b.min.z - p.z, p.z - b.max.z};
            static const Point3F normals[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            int face = 0;
            for (int k = 1; k < 6; ++k) if (gaps[k] > gaps[face]) face = k;
            const float d = gaps[face];
            if (d >= tol) continue;
            // Outside the box, the vertex must be over the face.
            bool over = true;
            for (int k = 0; k < 6 && over; ++k) if (k / 2 != face / 2 && gaps[k] > tol) over = false;
            if (!over) continue;
            const float value = std::max(d, -MaxPenetration);
            if (value < closest) {
                closest = value;
                if (closestObject) *closestObject = b.object;
            }
            if (contacts) contacts->push_back({p, normals[face], std::max(d, 0.0f), b.object});
        }
        for (const auto& t : tris) {
            auto over = [&](const Point3F& q) {
                const Point3F e0 = sub(t.b, t.a), e1 = sub(t.c, t.a), e2 = sub(q, t.a);
                const float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(e2, e0),
                            d21 = dot(e2, e1);
                const float denom = d00 * d11 - d01 * d01;
                if (std::fabs(denom) < 1e-12f) return false;
                const float v = (d11 * d20 - d01 * d21) / denom, w = (d00 * d21 - d01 * d20) / denom;
                return v >= -1e-4f && w >= -1e-4f && v + w <= 1 + 1e-4f;
            };
            const float d = dot(sub(p, t.a), t.n);
            if (from) {
                const float d0 = dot(sub(prev[i], t.a), t.n);
                if (d0 >= 0 && d < 0 && over(add(prev[i], mul(sub(p, prev[i]), d0 / (d0 - d))))) {
                    closest = -MaxPenetration;
                    if (closestObject) closestObject->clear();
                }
            }
            if (d >= tol || d <= -MaxPenetration || !over(sub(p, mul(t.n, d)))) continue;
            if (d < closest) {
                closest = d;
                if (closestObject) closestObject->clear();
            }
            if (contacts) contacts->push_back({p, t.n, std::max(d, 0.0f), {}});
        }
    }
    // The other way round: a shape's box corners inside the hull's bounds
    // (a player is smaller than a vehicle, so no hull vertex enters its
    // box). Each is a contact on the hull face it is least deep behind,
    // pushing the vehicle off the corner.
    Point3F llo{1e30f, 1e30f, 1e30f}, lhi{-1e30f, -1e30f, -1e30f};
    if (hull) {
        for (const auto& p : *hull) {
            const Point3F q{p.x * scale[0], p.y * scale[1], p.z * scale[2]};
            llo = {std::min(llo.x, q.x), std::min(llo.y, q.y), std::min(llo.z, q.z)};
            lhi = {std::max(lhi.x, q.x), std::max(lhi.y, q.y), std::max(lhi.z, q.z)};
        }
    } else {
        llo = {objMin[0] * scale[0], objMin[1] * scale[1], objMin[2] * scale[2]};
        lhi = {objMax[0] * scale[0], objMax[1] * scale[1], objMax[2] * scale[2]};
    }
    if (llo.x <= lhi.x)
        for (const auto& b : boxes)
            for (int c = 0; c < 8; ++c) {
                const Point3F w{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z};
                const float dx = w.x - mat[3], dy = w.y - mat[7], dz = w.z - mat[11];
                const Point3F l{mat[0] * dx + mat[4] * dy + mat[8] * dz, mat[1] * dx + mat[5] * dy + mat[9] * dz,
                                mat[2] * dx + mat[6] * dy + mat[10] * dz};
                const float gaps[6] = {llo.x - l.x, l.x - lhi.x, llo.y - l.y, l.y - lhi.y, llo.z - l.z, l.z - lhi.z};
                static const Point3F faceNormals[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
                int face = 0;
                for (int k = 1; k < 6; ++k) if (gaps[k] > gaps[face]) face = k;
                const float d = gaps[face];
                if (d >= tol) continue;
                bool over = true;
                for (int k = 0; k < 6 && over; ++k) if (k / 2 != face / 2 && gaps[k] > tol) over = false;
                if (!over) continue;
                const float value = std::max(d, -MaxPenetration);
                if (value < closest) {
                    closest = value;
                    if (closestObject) *closestObject = b.object;
                }
                if (contacts) {
                    const Point3F& n = faceNormals[face];
                    const Point3F nw{mat[0] * n.x + mat[1] * n.y + mat[2] * n.z, mat[4] * n.x + mat[5] * n.y + mat[6] * n.z,
                                     mat[8] * n.x + mat[9] * n.y + mat[10] * n.z};
                    contacts->push_back({w, mul(nw, -1.0f), std::max(d, 0.0f), b.object});
                }
            }
    return closest;
}

void VehicleObject::processMove(const ClientMoveIn* m) {
    const float energyBefore = energy;
    ShapeBase::processMove(m);
    if (!frozen) {
        updateMove(m);
        rigid.clearForces();
        updateForces();
        updatePos(TickSec);
        setPosition(rigid.state.linPosition, rigid.state.angPosition);
        setMaskBits(PositionMask);
        updateContainer();
    } else {
        setPosition(rigid.state.linPosition, rigid.state.angPosition);
    }
    if (energy != energyBefore) setMaskBits(EnergyMask);
}

void VehicleObject::updateMove(const ClientMoveIn* m) {
    if (m && m->exact) {
        move = {};
        move.x = m->fx; move.y = m->fy; move.z = m->fz;
        move.yaw = m->fyaw; move.pitch = m->fpitch; move.roll = m->froll;
        move.freeLook = m->freeLook;
        for (int i = 0; i < 6; ++i) move.trigger[i] = m->trigger[i];
    } else {
        move = m ? PlayerPrediction::unclampMove(m->x, m->y, m->z, (uint16_t)m->yaw, (uint16_t)m->pitch,
                                             (uint16_t)m->roll, m->freeLook, m->trigger)
             : PlayerPrediction::Move{};
    }
    packedMove = m ? *m : ClientMoveIn{};
    // Image Triggers
    if (damageState == Enabled) {
        setImageTriggerState(0, move.trigger[0]);
        setImageTriggerState(1, move.trigger[1]);
    }
    // Throttle
    if (!disableMove) throttle = move.y;
    // Steering
    const float maxSteer = data("maxSteeringAngle", dynamic_cast<FlyingVehicleObject*>(this) ? (float)M_PI : 0.785f);
    if (m) {
        steering[0] = std::clamp(steering[0] + move.yaw, -maxSteer, maxSteer);
        steering[1] = std::clamp(steering[1] + move.pitch, -maxSteer, maxSteer);
    } else {
        steering[0] = steering[1] = 0;
    }
    // Jetting
    if (move.trigger[3]) {
        if (!jetting && getEnergyLevel() >= data("minJetEnergy", 1.0f)) jetting = true;
        if (jetting) {
            float newEnergy = getEnergyLevel() - data("jetEnergyDrain", 0.8f);
            if (newEnergy < 0) {
                newEnergy = 0;
                jetting = false;
            }
            setEnergyLevel(newEnergy);
        }
    } else {
        jetting = false;
    }
    if (!inLiquid && waterCoverage != 0.0f) {
        callDataBlock("onEnterLiquid", {f32(waterCoverage), std::to_string(liquidType)});
        inLiquid = true;
        heat = 0.0f;
    } else if (inLiquid && waterCoverage == 0.0f) {
        callDataBlock("onLeaveLiquid", {std::to_string(liquidType)});
        inLiquid = false;
        heat = 1.0f;
    }
}

void VehicleObject::updatePos(float dt) { advanceToCollision(dt); }

// Vehicle::advanceToCollision: a displaceable shape (a player) the vehicle
// runs into is pushed out of the way before the vehicle collides with it.
bool VehicleObject::advanceToCollision(float time) {
    float ct = 0, dt = time;
    Rigid::State ns = rigid.state;
    Matrix mat = rigid.state.getTransform();
    std::string closestObject;
    float dist = collide(mat, CollisionTol, nullptr, nullptr, &closestObject);
    std::vector<Contact> info;
    const float mt = time / 2.0f;
    dt = mt;
    bool collided = false, displaced = false;
    bool success = true;
    const Point3F origVelocityStart = rigid.state.linVelocity;
    Point3F origVelocity = origVelocityStart;
    do {
        const float prevDist = dist;
        info.clear();
        if (dist < CollisionTol) {
            // Try to displace the object by the amount we're trying to move
            if (auto* player = closestObject.empty() ? nullptr : EngineObjects::get<PlayerObject>(closestObject)) {
                const float objMass = player->mass();
                Point3F objNewMom = mul(ns.linVelocity, objMass * 1.1f);
                Point3F objOldMom = player->getMomentum();
                Point3F objNewVel = mul(objNewMom, 1.0f / objMass);
                float mlo[3], mhi[3], tlo[3], thi[3];
                worldBox(mlo, mhi);
                player->worldBox(tlo, thi);
                const Point3F myCenter{(mlo[0] + mhi[0]) * 0.5f, (mlo[1] + mhi[1]) * 0.5f, (mlo[2] + mhi[2]) * 0.5f};
                const Point3F theirCenter{(tlo[0] + thi[0]) * 0.5f, (tlo[1] + thi[1]) * 0.5f, (tlo[2] + thi[2]) * 0.5f};
                if (dot(sub(myCenter, theirCenter), objNewMom) >= 0.0f || len(objNewVel) < 0.01f) {
                    objNewMom = mul(normalize(sub(theirCenter, myCenter)), 1.0f * objMass);
                    objNewVel = mul(objNewMom, 1.0f / objMass);
                }
                player->setMomentum(objNewMom);
                if (player->displaceObject(mul(objNewVel, 1.1f * mt))) {
                    // Determine the speed at which we will damage this object
                    const float speed = len(sub(mul(objOldMom, 1.0f / objMass), mul(objNewMom, 1.0f / objMass)));
                    if (std::none_of(struck.begin(), struck.end(), [&](const Struck& s) { return s.object == closestObject; }))
                        struck.push_back({closestObject, speed, true});
                    dist = 1e7f;
                    closestObject.clear();
                    displaced = true;
                    continue;
                }
            }
            collide(mat, CollisionTol * 1.25f, &info);
            collided |= resolveCollision(ns, info);
            resolveContacts(ns, info, dt);
            if (collided) {
                ns.force = {0, 0, 0};
                ns.torque = {0, 0, 0};
            }
        }
        rigid.integrate(ns, dt);
        const Matrix before = mat;
        mat = ns.getTransform();
        closestObject.clear();
        dist = collide(mat, CollisionTol, nullptr, &before, &closestObject);
        if (dist <= IntersectionTol && dist <= prevDist) {
            if ((dt *= 0.25f) < 0.0001f) {
                // Make sure we check the collision damage...
                collided = true;
                success = false;
                rigid.state.linVelocity = rigid.state.linMomentum = {0, 0, 0};
                rigid.state.angVelocity = rigid.state.angMomentum = {0, 0, 0};
                break;
            }
            dist = 1e7f;
            ns = rigid.state;
            mat = ns.getTransform();
            continue;
        }
        rigid.state = ns;
        ct += dt;
        if (dt < mt) dt *= 1.2f;
        if (dt > time - ct) dt = time - ct;
    } while (ct < time);

    if (collided || displaced) {
        const float collVel = len(sub(origVelocity, rigid.state.linVelocity));
        if (origVelocity.x != 0 || origVelocity.y != 0 || origVelocity.z != 0)
            origVelocity = normalize(origVelocity);
        else
            origVelocity = {0, 0, 1};
        if (collVel > data("minImpactSpeed", 25.0f)) {
            const Point3F vec = mul(origVelocity, collVel);
            callDataBlock("onImpact", {"0", vec3(vec), f32(len(vec))});
        }
        damageQueuedObjects(collVel);
        const Point3F up = column(rigid.state.getTransform(), 2);
        bool blowup = false;
        if (up.z < -0.25f) {
            blowup = true;
        } else {
            // VehicleData::preload's stuckTimerZ = mCos(stuckTimerAngle).
            const float stuckZ = std::cos(std::clamp(data("stuckTimerAngle", 180.0f), 0.0f, 180.0f));
            if (up.z <= stuckZ) ++stuckTimer;
            else stuckTimer = 0;
            if (stuckTimer >= std::max(1, (int)data("stuckTimerTicks", 1.0f))) blowup = true;
        }
        if (blowup)
            callDataBlock("damageObject", {"0", vec3(rigid.state.linPosition), "1000",
                                           std::to_string(intGlobal("$DamageType::Ground"))});
    } else {
        stuckTimer = 0;
    }
    return success;
}

void VehicleObject::damageQueuedObjects(float collisionVel) {
    const float threshold = data("collDamageThresholdVel", 20.0f);
    const float multiplier = data("collDamageMultiplier", 0.05f);
    const float damageVal = std::max(0.0f, (collisionVel - threshold) * multiplier);
    const std::string damageType = std::to_string(intGlobal("$DamageType::Impact"));
    const std::string self = handle();
    const std::string key = script ? ScriptEngine::instance().objectKey(script) : std::string();
    std::vector<Struck> list;
    list.swap(struck);
    for (const auto& s : list) {
        callDataBlock("onCollision", {EngineObjects::get<ShapeBase>(s.object) ? EngineObjects::get<ShapeBase>(s.object)->handle() : ""});
        if (!EngineObjects::get<ShapeBase>(key)) return;
        auto* other = EngineObjects::get<ShapeBase>(s.object);
        if (!other) continue;
        const std::string position = vec3(rigid.state.linPosition);
        if (!s.useData && damageVal != 0.0f)
            other->callDataBlock("damageObject", {self, position, f32(damageVal), damageType});
        else if (s.data > threshold)
            other->callDataBlock("damageObject", {self, position, f32((s.data - threshold) * multiplier), damageType});
        if ((other = EngineObjects::get<ShapeBase>(s.object)) && EngineObjects::get<ShapeBase>(key))
            other->callDataBlock("onCollision", {self});
        if (!EngineObjects::get<ShapeBase>(key)) return;
    }
}

bool VehicleObject::resolveCollision(Rigid::State& ns, const std::vector<Contact>& contacts) {
    // Apply impulses to resolve collision
    bool collided = false, colliding;
    do {
        colliding = false;
        for (const auto& c : contacts) {
            if (c.distance >= CollisionTol) continue;
            const Point3F v = ns.getVelocity(sub(c.point, ns.linPosition));
            if (dot(v, c.normal) < -ContactTol) {
                rigid.resolveCollision(ns, c.point, c.normal);
                colliding = collided = true;
                // Track collisions (shapes only: a VehicleBlocker is not one)
                if (!c.object.empty() && EngineObjects::get<ShapeBase>(c.object) &&
                    std::none_of(struck.begin(), struck.end(), [&](const Struck& s) { return s.object == c.object; }))
                    struck.push_back({c.object, 0, false});
            }
        }
    } while (colliding);
    return collided;
}

void VehicleObject::resolveContacts(Rigid::State& ns, const std::vector<Contact>& contacts, float dt) {
    // Apply impulse to resolve contacts
    Point3F p{0, 0, 0}, l{0, 0, 0};
    for (const auto& c : contacts) {
        if (c.distance >= CollisionTol) continue;
        const Point3F r = sub(c.point, ns.linPosition);
        const Point3F v = ns.getVelocity(r);
        const float vn = dot(v, c.normal);
        if (vn > -ContactTol) {
            // Penetration force (the zero impulse is the rigid's current state's)
            const float zi = rigid.getZeroImpulse(rigid.state, r, c.normal);
            const float d = (CollisionTol - c.distance) / CollisionTol;
            const float s = (d * d) * zi * SpringForce - vn * SpringDamping;
            Point3F f = mul(c.normal, s * dt);
            // Frictional force
            const Point3F uv = sub(v, mul(c.normal, vn));
            const float ul = len(uv);
            if (s > 0 && ul) f = sub(f, mul(uv, s * rigid.friction * dt / ul));
            p = add(p, f);
            l = add(l, cross(r, f));
        }
    }
    ns.linMomentum = add(ns.linMomentum, p);
    ns.angMomentum = add(ns.angMomentum, l);
    rigid.updateVelocity(ns);
}

// Vehicle::writePacketData.
bool VehicleObject::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = ShapeBase::writePacketData(connection, w);
    w.writeF32(steering[0]);
    w.writeF32(steering[1]);
    const auto& s = rigid.state;
    w.writePoint(wp(s.linPosition));
    w.writeF32(s.angPosition.x);
    w.writeF32(s.angPosition.y);
    w.writeF32(s.angPosition.z);
    w.writeF32(s.angPosition.w);
    w.writePoint(wp(s.linMomentum));
    w.writePoint(wp(s.angMomentum));
    w.writeFlag(disableMove);
    w.writeFlag(frozen);
    w.setCompressionPoint(wp(s.linPosition));
    return ret;
}

// Vehicle::packUpdate.
uint32_t VehicleObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    w.writeFlag(jetting);
    const std::string conn = connection.script ? ScriptEngine::instance().objectKey(connection.script) : std::string();
    // The rest is the control object's packet data for its own client.
    if (w.writeFlag(!controllingClient.empty() && controllingClient == conn && !(mask & InitialUpdateMask)))
        return ret;
    const float maxSteer = data("maxSteeringAngle", dynamic_cast<FlyingVehicleObject*>(this) ? (float)M_PI : 0.785f);
    w.writeFloat((steering[0] + maxSteer) / (2 * maxSteer), 9);
    w.writeFloat((steering[1] + maxSteer) / (2 * maxSteer), 9);
    // mDelta.move.pack
    const ClientMoveIn& m = packedMove;
    if (w.writeFlag(m.yaw != 0)) w.writeInt((uint16_t)m.yaw, 16);
    if (w.writeFlag(m.pitch != 0)) w.writeInt((uint16_t)m.pitch, 16);
    if (w.writeFlag(m.roll != 0)) w.writeInt((uint16_t)m.roll, 16);
    w.writeInt(m.x, 6);
    w.writeInt(m.y, 6);
    w.writeInt(m.z, 6);
    w.writeFlag(m.freeLook);
    for (bool t : m.trigger) w.writeFlag(t);
    w.writeFlag(frozen);
    if (w.writeFlag(mask & PositionMask)) {
        const auto& s = rigid.state;
        w.writeCompressedPoint(wp(s.linPosition));
        w.writeF32(s.angPosition.x);
        w.writeF32(s.angPosition.y);
        w.writeF32(s.angPosition.z);
        w.writeF32(s.angPosition.w);
        w.writePoint(wp(s.linMomentum));
        w.writePoint(wp(s.angMomentum));
    }
    // The energy goes to a mounted player's client whose control object is
    // not this vehicle.
    bool found = false;
    if (mask & EnergyMask)
        for (const auto& key : mounted) {
            auto* rider = EngineObjects::get<PlayerObject>(key);
            if (!rider) continue;
            if (!rider->controllingClient.empty() && rider->controllingClient == conn) {
                auto* client = EngineObjects::get<GameConnection>(conn);
                const std::string self = script ? ScriptEngine::instance().objectKey(script) : std::string();
                found = client && client->controlObject() != self;
                break;
            }
        }
    if (w.writeFlag(found)) w.writeFloat(std::clamp(getEnergyValue(), 0.0f, 1.0f), 8);
    return ret;
}

//----------------------------------------------------------------------------
// HoverVehicle

void HoverVehicleObject::updateMove(const ClientMoveIn* m) {
    VehicleObject::updateMove(m);
    forwardThrust = throttle > 0.0f ? throttle : 0.0f;
    reverseThrust = throttle < 0.0f ? -throttle : 0.0f;
    leftThrust = move.x < 0.0f ? -move.x : 0.0f;
    rightThrust = move.x > 0.0f ? move.x : 0.0f;
    thrustDirection = !move.y ? ThrustDown : move.y > 0 ? ThrustForward : ThrustBackward;
}

void HoverVehicleObject::updateForces() {
    const Point3F gravForce{0, 0, VehicleGravity * gravityMod};
    const Matrix currTransform = rigid.state.getTransform();
    const float mainThrust = data("mainThrustForce", 0), reverseForce = data("reverseThrustForce", 0),
                strafeForce = data("strafeThrustForce", 0);
    Point3F thrustForce = mulV(currTransform, {(rightThrust - leftThrust) * strafeForce,
                                               forwardThrust * mainThrust - reverseThrust * reverseForce, 0});
    if (jetting) thrustForce = mul(thrustForce, data("turboFactor", 1.0f));
    Point3F torque{0, 0, 0}, force{0, 0, 0};

    // HoverVehicle::getBaseStabilizerLength (HoverVehicleData::preload's
    // maxThrustSpeed = (mainThrustForce + strafeThrustForce) / dragForce).
    const float dragForce = std::max(data("dragForce", 0), 0.01f);
    const float maxThrustSpeed = (mainThrust + strafeForce) / dragForce;
    const float stabLenMin = data("stabLenMin", 0.5f), stabLenMax = data("stabLenMax", 2.0f);
    const float velRatio = (maxThrustSpeed - std::min(len(rigid.state.linVelocity), maxThrustSpeed)) / maxThrustSpeed;
    const float baseStabLen = stabLenMin + (stabLenMax - stabLenMin) * (1.0f - velRatio);
    const Point3F stabExtend = mulV(currTransform, {0, 0, -baseStabLen});

    // The stabilizers at the box's front and back (min.x twice, as shipped).
    const Point3F osPoints[2] = {{(objMin[0] + objMin[0]) * 0.5f, objMax[1], (objMin[2] + objMax[2]) * 0.5f},
                                 {(objMin[0] + objMin[0]) * 0.5f, objMin[1], (objMin[2] + objMax[2]) * 0.5f}};
    Point3F wsPoints[2];
    for (int i = 0; i < 2; ++i) wsPoints[i] = mulP(currTransform, osPoints[i]);

    floating = true;
    bool reallyFloating = true;
    float compression[2] = {0, 0}, normalMod[2] = {0, 0};
    bool normalSet[2] = {false, false};
    Point3F normal[2];
    for (int j = 0; j < 2; ++j) {
        RayInfo rinfo;
        if (castRay(wsPoints[j], add(wsPoints[j], mul(stabExtend, 2.0f)),
                    TerrainObjectType | InteriorObjectType | WaterObjectType, rinfo)) {
            reallyFloating = false;
            // The stab is in contact with the ground.
            if (rinfo.t <= 0.5f) compression[j] = (1.0f - rinfo.t * 2.0f) * baseStabLen;
            normalSet[j] = true;
            normalMod[j] = rinfo.t < 0.5f ? 1.0f : 1.0f - (rinfo.t - 0.5f) * 2.0f;
            normal[j] = rinfo.normal;
        }
        // Check the waterblock directly
        if (pointInWater(wsPoints[j])) compression[j] = baseStabLen;
    }
    const float springK = data("stabSpringConstant", 30), dampK = data("stabDampingConstant", 10);
    for (int j = 0; j < 2; ++j) {
        if (compression[j] == 0.0f) continue;
        floating = false;
        // Spring force and damping
        const Point3F up = normalize(mul(stabExtend, -1));
        const Point3F springForce = mul(up, compression[j] * springK);
        const Point3F springDamping = mul(up, -std::min(dot(up, rigid.state.linVelocity), 0.7f) * dampK);
        force = add(force, add(springForce, springDamping));
    }
    // Gravity
    force = add(force, reallyFloating ? mul(gravForce, data("floatingGravMag", 1)) : gravForce);
    // Braking
    const float vellen = len(rigid.state.linVelocity);
    if (throttle == 0.0f && leftThrust == 0.0f && rightThrust == 0.0f && vellen != 0.0f &&
        vellen < data("brakingActivationSpeed", 0))
        force = add(force, mul(normalize(rigid.state.linVelocity), -data("brakingForce", 0)));
    // Gyro Drag
    torque = mul(rigid.state.angMomentum, -data("gyroDrag", 10));
    // Move to proper normal
    const float normalForce = data("normalForce", 30);
    const Point3F sn = column(currTransform, 2);
    if (normalSet[0] || normalSet[1]) {
        if (normalSet[0] && normalSet[1]) {
            const float d = dot(normal[0], normal[1]);
            if (d > 0.999f) {
                // Just pick the first normal. They're too close to call
                if (dot(sub(sn, normal[0]), sub(sn, normal[0])) > 0.00001f)
                    torque = add(torque, mul(cross(sn, normal[0]), normalForce * normalMod[0]));
            } else {
                const Point3F rotAxis = normalize(cross(normal[0], normal[1]));
                const float angle = std::acos(d) * (normalMod[0] / (normalMod[0] + normalMod[1]));
                const Matrix tempMat = TorqueMath::matrix(TorqueMath::quat(TorqueMath::AngAxis{rotAxis.x, rotAxis.y, rotAxis.z, angle}));
                const Point3F newNormal = mulV(tempMat, normal[1]);
                if (dot(sub(sn, newNormal), sub(sn, newNormal)) > 0.00001f)
                    torque = add(torque, mul(cross(sn, newNormal), normalForce * ((normalMod[0] + normalMod[1]) * 0.5f)));
            }
        } else {
            const Point3F useNormal = normalSet[0] ? normal[0] : normal[1];
            const float useMod = normalSet[0] ? normalMod[0] : normalMod[1];
            if (dot(sub(sn, useNormal), sub(sn, useNormal)) > 0.00001f)
                torque = add(torque, mul(cross(sn, useNormal), normalForce * useMod));
        }
    } else if (dot(sub(sn, {0, 0, 1}), sub(sn, {0, 0, 1})) > 0.00001f) {
        torque = add(torque, mul(cross(sn, {0, 0, 1}), data("restorativeForce", 10)));
    }
    const Point3F x = column(currTransform, 0), y = column(currTransform, 1), z = column(currTransform, 2);
    torque = sub(torque, mul(normalize(cross(x, y)), steering[0] * data("steeringForce", 25)));
    torque = sub(torque, mul(normalize(cross(x, z)), steering[0] * data("rollForce", 2.5f)));
    torque = sub(torque, mul(normalize(cross(y, z)), steering[1] * data("pitchForce", 2.5f)));
    // Apply drag
    Point3F vDrag = rigid.state.linVelocity;
    const float vertFactor = std::clamp(data("vertFactor", 0.25f), 0.0f, 1.0f);
    vDrag = floating ? Point3F{vDrag.x * 0.25f, vDrag.y * 0.25f, vDrag.z * vertFactor}
                     : Point3F{vDrag.x, vDrag.y, vDrag.z * vertFactor};
    force = sub(force, mul(vDrag, dragForce));
    force = add(force, floating ? mul(thrustForce, std::clamp(data("floatingThrustFactor", 0.15f), 0.0f, 1.0f))
                                : thrustForce);
    // Physical zone force, container buoyancy & drag
    force = add(force, appliedForce);
    force = add(force, {0, 0, -buoyancy * VehicleGravity * gravityMod});
    force = sub(force, mul(rigid.state.linVelocity, drag));
    torque = sub(torque, mul(rigid.state.angMomentum, drag));
    rigid.state.force = force;
    rigid.state.torque = torque;
}

uint32_t HoverVehicleObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = VehicleObject::packUpdate(connection, mask, w);
    w.writeInt(thrustDirection, NumThrustBits);
    return ret;
}

//----------------------------------------------------------------------------
// FlyingVehicle

void FlyingVehicleObject::updateMove(const ClientMoveIn* m) {
    VehicleObject::updateMove(m);
    if (!m) steering[0] = steering[1] = 0;
    if (len(rigid.state.linVelocity) < data("maxAutoSpeed", 0)) {
        const float damping = data("autoInputDamping", 1);
        steering[0] *= damping;
        steering[1] *= damping;
    }
    // The mission area's flight ceiling: thrust fades above it, and is 0 at
    // ceiling + range.
    ceilingFactor = 1.0f;
    ScriptObject* area = nullptr;
    for (auto& [name, object] : ScriptEngine::instance().objects)
        if (object && strcasecmp(object->className.c_str(), "MissionArea") == 0 &&
            strcasecmp(object->name.c_str(), "MissionArea") == 0) { area = object; break; }
    if (area) {
        const float flightCeiling = Fields::f32(area, "flightCeiling", 2000);
        const float ceilingRange = Fields::f32(area, "flightCeilingRange", 50);
        if (rigid.state.linPosition.z > flightCeiling) {
            if (ceilingRange == 0)
                ceilingFactor = 0;
            else
                ceilingFactor = std::max(0.0f, 1.0f - (rigid.state.linPosition.z - flightCeiling) /
                                                          (flightCeiling + ceilingRange));
        }
    }
    thrust.x = move.x;
    thrust.y = move.y;
    thrustDirection = thrust.y != 0.0f ? (thrust.y > 0 ? ThrustForward : ThrustBackward) : ThrustDown;
    if (ceilingFactor != 1.0f) jetting = false;
}

// FlyingVehicle::getHeight: the hover height's distance to the ground below.
float FlyingVehicleObject::getHeight() {
    const float height = createHeightOn ? data("createHoverHeight", 2) : data("hoverHeight", 2);
    const float r = 10 + height;
    const Point3F sp{transform[3], transform[7], transform[11]};
    RayInfo collision;
    if (!castRay(sp, {sp.x, sp.y, sp.z - r}, 0xFFFFFFFFu, collision, collisionExempt())) collision.t = 1;
    return (r * collision.t - height) / 10;
}

void FlyingVehicleObject::updateForces() {
    const Matrix currPosMat = rigid.state.getTransform();
    const Point3F xv = column(currPosMat, 0), yv = column(currPosMat, 1), zv = column(currPosMat, 2);
    const float speed = len(rigid.state.linVelocity);
    Point3F force{0, 0, VehicleGravity * mass * gravityMod};
    Point3F torque{0, 0, 0};
    // Drag at any speed
    force = sub(force, mul(rigid.state.linVelocity, data("minDrag", 0)));
    torque = sub(torque, mul(rigid.state.angMomentum, data("rotationalDrag", 0)));
    // Auto-stop at low speeds
    const float maxAutoSpeed = data("maxAutoSpeed", 0);
    if (speed < maxAutoSpeed) {
        const float autoScale = 1 - speed / maxAutoSpeed;
        // Gyroscope
        torque = sub(torque, mul(xv, data("autoAngularForce", 0) * autoScale * yv.z));
        // Maneuvering jets
        const float sf = data("autoLinearForce", 0) * autoScale;
        force = sub(force, mul(yv, sf * dot(yv, rigid.state.linVelocity)));
        force = sub(force, mul(xv, sf * dot(xv, rigid.state.linVelocity)));
    }
    // Hovering Jet
    const float jetForce = data("jetForce", 500);
    float vf = -VehicleGravity * mass * gravityMod;
    const float h = getHeight();
    if (h <= 1) {
        if (h > 0) vf -= vf * h * 0.1f;
        else vf += jetForce * -h;
    }
    force = add(force, mul(zv, vf));
    // Damping "surfaces"
    force = sub(force, mul(xv, speed * dot(xv, rigid.state.linVelocity) * data("horizontalSurfaceForce", 0)));
    force = sub(force, mul(zv, speed * dot(zv, rigid.state.linVelocity) * data("verticalSurfaceForce", 0)));
    // Turbo Jet
    if (jetting) {
        if (thrustDirection == ThrustForward)
            force = add(force, mul(yv, jetForce * ceilingFactor));
        else if (thrustDirection == ThrustBackward)
            force = sub(force, mul(yv, jetForce * ceilingFactor));
        else
            force = add(force, mul(zv, jetForce * data("vertThrustMultiple", 1) * ceilingFactor));
    }
    // Maneuvering jets
    const float maneuvering = data("maneuveringForce", 0);
    force = add(force, mul(yv, thrust.y * maneuvering * ceilingFactor));
    force = add(force, mul(xv, thrust.x * maneuvering * ceilingFactor));
    // Steering
    const float maxSteer = data("maxSteeringAngle", (float)M_PI);
    float sx = steering[0] / maxSteer, sy = steering[1] / maxSteer;
    sx *= std::fabs(sx);
    sy *= std::fabs(sy);
    const float steeringForce = data("steeringForce", 1);
    torque = sub(torque, mul(xv, sy * steeringForce));
    torque = sub(torque, mul(zv, sx * steeringForce));
    // Roll
    torque = add(torque, mul(yv, sx * data("steeringRollForce", 1)));
    float ar = data("autoAngularForce", 0) * xv.z;
    ar -= data("rollForce", 1) * dot(xv, rigid.state.linVelocity);
    torque = add(torque, mul(yv, ar));
    force = add(force, appliedForce);
    force = sub(force, {0, 0, buoyancy * VehicleGravity * gravityMod});
    force = sub(force, mul(rigid.state.linVelocity, drag));
    rigid.state.force = mul(force, oneOverMass);
    rigid.state.torque = mul(torque, oneOverMass);
}

void FlyingVehicleObject::useCreateHeight(bool on) {
    createHeightOn = on;
    setMaskBits(NextFreeMask); // HoverHeight
}

uint32_t FlyingVehicleObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = VehicleObject::packUpdate(connection, mask, w);
    const std::string conn = connection.script ? ScriptEngine::instance().objectKey(connection.script) : std::string();
    // The rest is part of the control object packet update.
    if (w.writeFlag(!controllingClient.empty() && controllingClient == conn && !(mask & InitialUpdateMask)))
        return ret;
    w.writeFlag(createHeightOn);
    w.writeInt(thrustDirection, NumThrustBits);
    return ret;
}

//----------------------------------------------------------------------------
// WheeledVehicle

namespace {
// WheeledVehicleData::preload's wheels: ground%d's position with spring%d
// at 0 (spring) and 1 (pos), mirrored across x for a wheel opposite
// another (mirrorWheel).
const std::vector<WheeledVehicleObject::WheelData>* shapeWheels(const std::string& shapeFile, float tireRadius) {
    static std::unordered_map<std::string, std::vector<WheeledVehicleObject::WheelData>> cache;
    if (shapeFile.empty() || !Engine::instance().filesys) return nullptr;
    std::string key = shapeFile;
    for (char& ch : key) ch = (char)std::tolower((unsigned char)ch);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::vector<WheeledVehicleObject::WheelData> wheels;
        const auto bytes = Engine::instance().fs().read(("shapes/" + shapeFile).c_str());
        if (!bytes.empty()) {
            const DTSLoadResult shape = loadDTS(bytes.data(), bytes.size(), shapeFile.c_str());
            auto find = [&](const std::string& name, bool node) {
                if (node) {
                    for (size_t n = 0; n < shape.nodes.size(); ++n)
                        if (strcasecmp(shape.nodes[n].name.c_str(), name.c_str()) == 0) return (int)n;
                } else {
                    for (size_t a = 0; a < shape.animations.size(); ++a)
                        if (strcasecmp(shape.animations[a].name.c_str(), name.c_str()) == 0) return (int)a;
                }
                return -1;
            };
            // The loader's frame has height in y and forward in z.
            auto nodePos = [](const MatrixF& m) { return Point3F{m.m[0][3], m.m[2][3], m.m[1][3]}; };
            for (int i = 0; i < WheeledVehicleObject::MaxWheels; ++i) {
                const int node = find("ground" + std::to_string(i), true);
                const int seq = find("spring" + std::to_string(i), false);
                if (node == -1 || seq == -1) continue;
                WheeledVehicleObject::WheelData w;
                w.spring = nodePos(dtsSequencePose(shape, seq, 0)[node]);
                w.pos = nodePos(dtsSequencePose(shape, seq, 1)[node]);
                w.safePos = {w.pos.x, w.pos.y, w.pos.z + tireRadius};
                bool mirrored = false;
                for (size_t o = 0; o < wheels.size() && !mirrored; ++o)
                    if (std::fabs(wheels[o].pos.y - w.pos.y) < 0.5f) {
                        w.pos = {-wheels[o].pos.x, wheels[o].pos.y, wheels[o].pos.z};
                        w.spring = wheels[o].spring;
                        w.opposite = (int)o;
                        wheels[o].opposite = (int)wheels.size();
                        mirrored = true;
                    }
                if (!mirrored) {
                    w.spring.x = w.spring.y = 0;
                    w.spring.z -= w.pos.z;
                }
                w.steering = find("turn" + std::to_string(i), false) != -1 ? WheeledVehicleObject::WheelData::Forward
                                                                            : WheeledVehicleObject::WheelData::None;
                wheels.push_back(w);
            }
        }
        it = cache.emplace(key, std::move(wheels)).first;
    }
    return &it->second;
}

// sClientCollisionMask & ~PlayerObjectType (wheeledVehicle.cc).
constexpr uint32_t WheelCollisionMask = TerrainObjectType | InteriorObjectType | StaticShapeObjectType |
                                        VehicleObjectType | VehicleBlockerObjectType | ForceFieldObjectType |
                                        StaticTSObjectType;
constexpr float WheeledGravity = -20;     // sWheeledVehicleGravity
constexpr float BreakZeroEpsilon = 0.02f; // sBreakZeroEpsilon (m/s)
} // namespace

// WheeledVehicle::onNewDataBlock: the springs at their datablock rate.
void WheeledVehicleObject::readFields() {
    VehicleObject::readFields();
    wheelData = shapeWheels(shapeFileOf(*this), data("tireRadius", 0.6f));
    for (auto& w : wheels) {
        w = Wheel{};
        w.k = data("springForce", 0.6f);
        w.s = data("springDamping", 0);
    }
}

void WheeledVehicleObject::processMove(const ClientMoveIn* m) {
    VehicleObject::processMove(m);
    if (waterCoverage > 0.5f) {
        char pos[96];
        std::snprintf(pos, sizeof(pos), "%f %f %f", rigid.state.linPosition.x, rigid.state.linPosition.y,
                      rigid.state.linPosition.z);
        callDataBlock("damageObject", {"0", pos, "1000", std::to_string(intGlobal("$DamageType::Water"))});
    }
}

void WheeledVehicleObject::updateMove(const ClientMoveIn* m) {
    VehicleObject::updateMove(m);
    const size_t count = wheelData ? wheelData->size() : 0;
    // Breaking
    float wvel = 0;
    for (size_t i = 0; i < count; ++i) wvel += wheels[i].avel;
    if (std::fabs(wvel * data("tireRadius", 0.6f)) < BreakZeroEpsilon) wvel = 0;
    if (braking) {
        // No throttle, or throttle the way the wheels turn: no longer breaking.
        if (!wvel || !throttle || (throttle > 0 && wvel > 0) || (throttle < 0 && wvel < 0)) braking = false;
    } else if ((throttle > 0 && wvel < 0) || (throttle < 0 && wvel > 0)) {
        braking = true;
    }
    updateWheels();
    if (wheelContact) steering[1] = 0;
}

// WheeledVehicle::updateWheels. APPROXIMATION: the tire box's sweep along
// the spring is a ray through the bottom of the tire.
void WheeledVehicleObject::updateWheels() {
    wheelContact = false;
    const Matrix currMatrix = rigid.state.getTransform();
    const auto exempt = collisionExempt();
    for (size_t i = 0; wheelData && i < wheelData->size(); ++i) {
        const WheelData& wd = (*wheelData)[i];
        Wheel& w = wheels[i];
        w.extension = 1;
        const Point3F sp = mulP(currMatrix, wd.pos), vec = mulV(currMatrix, wd.spring);
        const Point3F hp = sub(sp, mul(vec, w.extension)), ep = add(sp, mul(vec, w.extension));
        RayInfo info;
        if (castRay(hp, ep, WheelCollisionMask, info, exempt)) {
            w.extension = info.t > 0.5f ? w.extension * (info.t - 0.5f) * 2.0f : 0;
            w.contact = true;
            w.surfacePos = add(sp, mul(vec, w.extension));
            w.surfaceNormal = info.normal;
            wheelContact = true;
        } else {
            // Make sure that we haven't sunk into the ground...
            if (castRay(mulP(currMatrix, wd.safePos), sp, WheelCollisionMask, info, exempt)) {
                w.extension = 0;
                w.surfaceNormal = info.normal;
                w.surfacePos = info.point;
                w.contact = true;
                wheelContact = true;
            } else {
                w.contact = false;
            }
        }
    }
}

void WheeledVehicleObject::updateForces() {
    const float dt = TickSec;
    const Matrix currMatrix = rigid.state.getTransform();
    const size_t count = wheelData ? wheelData->size() : 0;
    const float tireRadius = data("tireRadius", 0.6f);
    const float oneOverSprungMass = 1 / (mass * 0.8f);
    const float maxAvel = data("maxWheelSpeed", 40) / tireRadius;
    const float aMomentum = count ? mass / count : mass;
    rigid.state.force = {0, 0, 0};
    rigid.state.torque = {0, 0, 0};
    // Drag
    rigid.state.force = sub(rigid.state.force, mul(rigid.state.linVelocity, data("minDrag", 0) * oneOverMass));
    rigid.state.torque = sub(rigid.state.torque, mul(rigid.state.angMomentum, data("antiRockForce", 0) * oneOverMass));
    // Body & Steering Vectors
    const Point3F bx = column(currMatrix, 0), by = column(currMatrix, 1), bz = column(currMatrix, 2);
    const Point3F worldY = by;
    const float quadraticSteering = -(steering[0] * std::fabs(steering[0]));
    const float sinSteering = std::sin(quadraticSteering), cosSteering = std::cos(quadraticSteering);
    // Center of mass in world space (the origin: massCenter is zero)
    const Point3F massCenter = mulP(currMatrix, {0, 0, 0});
    Point3F wheelForce[MaxWheels];
    // Vertical load for friction ("a hack", TimG)
    uint32_t contactCount = 0;
    for (size_t j = 0; j < count; ++j) if (wheels[j].contact) ++contactCount;
    const float verticalLoad = contactCount ? data("staticLoadScale", 1) * (mass * -WheeledGravity) / contactCount : 0;
    // Engine and break torque
    float engineTorque, breakTorque, maxBreakVel;
    if (braking) {
        breakTorque = data("breakTorque", 1) * std::fabs(throttle);
        maxBreakVel = (breakTorque / aMomentum) * dt;
        engineTorque = 0;
    } else if (throttle) {
        engineTorque = data("engineTorque", 1) * throttle;
        maxBreakVel = breakTorque = 0;
        // Double the engineTorque to help out the jets
        if (throttle > 0 && jetting) engineTorque += data("engineTorque", 1);
    } else {
        // Engine break.
        breakTorque = data("engineTorque", 1);
        maxBreakVel = (breakTorque / aMomentum) * dt;
        engineTorque = 0;
    }
    Point3F force{0, 0, WheeledGravity};
    // Jet Force
    if (jetting) force = add(force, mul(worldY, data("jetForce", 500) * oneOverMass));
    const float friction = data("tireFriction", 0.3f);
    const float lateralForce = data("tireLateralForce", 1000), lateralDamping = data("tireLateralDamping", 100),
                lateralRelaxation = data("tireLateralRelaxation", 1);
    const float longForce = data("tireLongitudinalForce", 1000), longDamping = data("tireLongitudinalDamping", 100),
                longRelaxation = data("tireLogitudinalRelaxation", 1); // the engine's field name, misspelled
    for (size_t j = 0; j < count; ++j) {
        Wheel& wheel = wheels[j];
        const WheelData& wheelData_ = (*wheelData)[j];
        Point3F& forceVector = wheelForce[j];
        forceVector = {0, 0, 0};
        if (wheel.contact) {
            const Point3F pos = mulP(currMatrix, wheelData_.pos);
            const Point3F localVel = rigid.state.getVelocity(sub(pos, massCenter));
            // Spring force and damping along the body's z at the contact
            const float spring = wheel.k * (wheel.center - wheel.extension);
            const float damping = std::max(0.0f, wheel.s * -dot(bz, localVel));
            // Anti-sway force based on difference in suspension extension
            float antiSway = 0;
            if (wheelData_.opposite != -1) {
                const Wheel& opposite = wheels[wheelData_.opposite];
                if (opposite.contact) antiSway = (opposite.extension - wheel.extension) * data("antiSwayForce", 1);
                if (antiSway < 0) antiSway = 0;
            }
            forceVector = add(forceVector, mul(bz, (spring + damping + antiSway) * oneOverSprungMass));
            // Tire direction vectors perpendicular to surface normal
            Point3F wheelXVec = bx;
            if (wheelData_.steering == WheelData::Forward)
                wheelXVec = add(mul(bx, cosSteering), mul(by, sinSteering));
            else if (wheelData_.steering == WheelData::Backward)
                wheelXVec = sub(mul(bx, cosSteering), mul(by, sinSteering));
            const Point3F tireY = normalize(cross(wheel.surfaceNormal, wheelXVec));
            const Point3F tireX = normalize(cross(tireY, wheel.surfaceNormal));
            // Velocity of wheel at surface contact
            const Point3F wheelVelocity = rigid.state.getVelocity(sub(wheel.surfacePos, massCenter));
            const float xVelocity = dot(tireX, wheelVelocity), yVelocity = dot(tireY, wheelVelocity);
            // Longitudinal deformation force
            const float ddy = (wheel.avel * tireRadius) - yVelocity - longRelaxation * std::fabs(wheel.avel) * wheel.Dy;
            wheel.Dy += ddy * dt;
            float Fy = longForce * wheel.Dy + longDamping * ddy;
            // Lateral deformation force
            const float ddx = xVelocity - lateralRelaxation * std::fabs(wheel.avel) * wheel.Dx;
            wheel.Dx += ddx * dt;
            float Fx = -(lateralForce * wheel.Dx + lateralDamping * ddx);
            // Reduce forces based on friction limit
            const float Fz = verticalLoad;
            const float sN = wheel.surfaceNormal.z;
            if (sN > 0) {
                const float muS = Fz * friction * sN, muS2 = muS * muS;
                const float Fn = (Fz * Fz) * muS2, Ff = (Fx * Fx + Fy * Fy) * muS2;
                if (Ff > Fn) {
                    const float K = std::sqrt(Fn / Ff);
                    Fy *= K;
                    Fx *= K;
                    wheel.Dy *= K;
                    wheel.Dx *= K;
                }
            } else {
                Fy = Fx = 0;
            }
            // Apply forces to wheel ground contact point
            forceVector = add(forceVector, add(mul(tireX, Fx * oneOverMass), mul(tireY, Fy * oneOverMass)));
            // Wheel angular acceleration from engine torque and tire deformation force
            wheel.torqueScale = std::fabs(wheel.avel) > maxAvel ? 0 : 1 - std::fabs(wheel.avel) / maxAvel;
            wheel.avel += (((wheel.torqueScale * engineTorque) - Fy * tireRadius) / aMomentum) * dt;
            // Wheel angular acceleration from break torque
            if (maxBreakVel > std::fabs(wheel.avel)) wheel.avel = 0;
            else wheel.avel += wheel.avel > 0 ? -maxBreakVel : maxBreakVel;
        } else {
            wheel.torqueScale = 0;
            wheel.Dy += (-longRelaxation * std::fabs(wheel.avel) * wheel.Dy) * dt;
            wheel.Dx += (-lateralRelaxation * std::fabs(wheel.avel) * wheel.Dx) * dt;
        }
    }
    // Sum up the torques and forces
    Point3F torque{0, 0, 0};
    for (size_t j = 0; j < count; ++j) {
        const Point3F r = sub(mulP(currMatrix, (*wheelData)[j].pos), massCenter);
        torque = add(torque, cross(r, wheelForce[j]));
        force = add(force, wheelForce[j]);
    }
    // Container buoyancy & drag
    force = add(force, {0, 0, -buoyancy * WheeledGravity});
    force = sub(force, mul(rigid.state.linVelocity, drag));
    torque = sub(torque, mul(rigid.state.angMomentum, drag));
    rigid.state.force = add(rigid.state.force, force);
    rigid.state.torque = add(rigid.state.torque, torque);
}

bool WheeledVehicleObject::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = VehicleObject::writePacketData(connection, w);
    w.writeFlag(braking);
    for (size_t i = 0; wheelData && i < wheelData->size(); ++i) {
        w.writeF32(wheels[i].avel);
        w.writeF32(wheels[i].Dy);
        w.writeF32(wheels[i].Dx);
    }
    return ret;
}

uint32_t WheeledVehicleObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = VehicleObject::packUpdate(connection, mask, w);
    // The rest is part of the control object's packet data for its client.
    const std::string self = script ? ScriptEngine::instance().objectKey(script) : std::string();
    if (connection.controlObject() == self && !(mask & InitialUpdateMask)) return ret;
    w.writeFlag(braking);
    if (w.writeFlag(mask & PositionMask))
        for (size_t i = 0; wheelData && i < wheelData->size(); ++i) {
            w.writeF32(wheels[i].avel);
            w.writeF32(wheels[i].Dy);
            w.writeF32(wheels[i].Dx);
        }
    return ret;
}

//----------------------------------------------------------------------------

void registerVehicleNatives(TorqueScript& ts) {
    EngineObjects::registerClass("HoverVehicle", [] { return std::make_shared<HoverVehicleObject>(); });
    EngineObjects::registerClass("FlyingVehicle", [] { return std::make_shared<FlyingVehicleObject>(); });
    EngineObjects::registerClass("WheeledVehicle", [] { return std::make_shared<WheeledVehicleObject>(); });
    using Args = std::vector<VMValue>;
    auto vehicle = [](const Args& args) -> VehicleObject* {
        return args.empty() ? nullptr : EngineObjects::get<VehicleObject>(args[0].toString());
    };
    ts.registerNative("Vehicle::setFrozenState", [vehicle](const Args& args) -> VMValue {
        if (auto* v = vehicle(args); v && args.size() > 1) v->setFrozenState(args[1].toBool());
        return VMValue("");
    });
    ts.registerNative("Vehicle::setTransform", [vehicle](const Args& args) -> VMValue {
        if (auto* v = vehicle(args); v && args.size() > 1) v->setTransform(TorqueMath::parse(args[1].toString()));
        return VMValue("");
    });
    ts.registerNative("Vehicle::getVelocity", [vehicle](const Args& args) -> VMValue {
        auto* v = vehicle(args);
        return VMValue(v ? vec3(v->getVelocity()) : std::string("0 0 0"));
    });
    // ShapeBase::applyImpulse(pos, vec) on a vehicle's rigid body.
    ts.registerNative("Vehicle::applyImpulse", [vehicle](const Args& args) -> VMValue {
        auto* v = vehicle(args);
        if (!v || args.size() < 3) return VMValue(0);
        Point3F pos{0, 0, 0}, vec{0, 0, 0};
        std::sscanf(args[1].toString().c_str(), "%f %f %f", &pos.x, &pos.y, &pos.z);
        std::sscanf(args[2].toString().c_str(), "%f %f %f", &vec.x, &vec.y, &vec.z);
        v->applyImpulse(pos, vec);
        return VMValue(1);
    });
    ts.registerNative("FlyingVehicle::useCreateHeight", [](const Args& args) -> VMValue {
        auto* v = args.empty() ? nullptr : EngineObjects::get<FlyingVehicleObject>(args[0].toString());
        if (v && args.size() > 1) v->useCreateHeight(args[1].toBool());
        return VMValue("");
    });
}
