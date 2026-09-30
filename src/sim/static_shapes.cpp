#include "sim/static_shapes.h"
#include "sim/torque_math.h"
#include "sim/game_connection.h"
#include "sim/nav_graph.h"
#include "sim/sim_state.h"
#include "sim/containers.h"
#include "sim/engine_classes.h"
#include "sim/force_field.h"
#include "sim/player.h"
#include "sim/projectile_aim.h"
#include "sim/target_manager.h"
#include "sim/vehicle.h"
#include "core/console.h"
#include "core/engine.h"
#include "game/player_prediction.h"
#include "render/dts_loader.h"
#include "ai/graph_math.h"
#include "script/torquescript.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <strings.h>
#include <unordered_map>
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <memory>

using namespace NavMath;

static bool controlledBy(const GameBase& object, GameConnection& connection) {
    return !object.controllingClient.empty() && connection.script &&
           object.controllingClient == ScriptEngine::instance().objectKey(connection.script);
}

uint32_t StaticShapeObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & PositionMask)) {
        writeAffineTransform(w);
        writeScale(w);
    }
    w.writeFlag(powered);
    return ret;
}

// StaticShape::processTick: a controlling move's triggers drive image
// slots 0 and 1 (the mount is followed in ShapeBase).
void StaticShapeObject::processMove(const ClientMoveIn* move) {
    ShapeBase::processMove(move);
    if (move && damageState == Enabled) {
        setImageTriggerState(0, move->trigger[0]);
        setImageTriggerState(1, move->trigger[1]);
    }
}

//----------------------------------------------------------------------------
// Turret

namespace {

constexpr uint32_t TickMs = 32;
constexpr uint32_t csmActiveScanMask =
    SimContainer::TerrainObjectType | SimContainer::InteriorObjectType | SimContainer::VehicleObjectType;
constexpr float csmFullyDeactivated = 0.0f;
constexpr float csmFullyActivated = 1.0f;
constexpr float csmPhiNull = 0.0f;
constexpr float csmThetaNull = 90.0f;
constexpr uint32_t csmDefaultDeactivateDelay = 1000;
constexpr uint32_t csmDefaultThinkTime = 200;
constexpr float csmDefaultActivationSpeed = 1.0f;
constexpr float csmDefaultPhiSpeed = 180.0f;
constexpr float csmDefaultThetaSpeed = 45.0f;
constexpr float csmDefaultAttackRadius = 40.0f;
constexpr float sgBigClunkyMultiplicationFactor = 40.0f;
constexpr float RadToDeg = 180.0f / (float)M_PI, DegToRad = (float)M_PI / 180.0f;

enum PrimaryAxis { YAxis, RevYAxis, ZAxis, RevZAxis };

// TurretImageData as its onAdd clamps it (activation and think times to a
// whole number of ticks), with the retail fire tolerances.
struct TurretImage {
    int32_t activationMS = 1000, deactivateDelayMS = 1000, thinkTimeMS = 100;
    float degPerSecTheta = 45.0f, degPerSecPhi = 180.0f, attackRadius = 40.0f;
    bool dontFireInsideDamageRadius = false;
    float damageRadius = 0.0f;
    float yawVariance = 0.5f, pitchVariance = 0.5f;
};

const TurretImage& turretImage(const ShapeBaseImageData& image) {
    static std::unordered_map<int, TurretImage> cache;
    auto it = cache.find(image.id);
    if (it != cache.end()) return it->second;
    ScriptObject* o = ScriptEngine::instance().findObject(std::to_string(image.id).c_str());
    TurretImage t;
    auto delay = [&](const char* field, int32_t fallback) {
        int32_t v = Fields::s32(o, field, fallback);
        if (v < (int32_t)TickMs || v > 5000) v = v < (int32_t)TickMs ? (int32_t)TickMs : 5000;
        return v;
    };
    auto degPerSec = [&](const char* field, float fallback) {
        float v = Fields::f32(o, field, fallback);
        if (v < 1.0f || v > 1080.0f) v = v < 1.0f ? 1.0f : 1080.0f;
        return (float)(uint32_t)v;
    };
    t.activationMS = (delay("activationMS", 1000) >> 5) << 5;
    t.deactivateDelayMS = (delay("deactivateDelayMS", 1000) >> 5) << 5;
    t.thinkTimeMS = delay("thinkTimeMS", 100);
    t.degPerSecTheta = degPerSec("degPerSecTheta", 45.0f);
    t.degPerSecPhi = degPerSec("degPerSecPhi", 180.0f);
    t.attackRadius = Fields::f32(o, "attackRadius", 40.0f);
    if (t.attackRadius < 10.0f || t.attackRadius > 1000.0f) t.attackRadius = t.attackRadius < 10.0f ? 10.0f : 1000.0f;
    t.activationMS = (t.activationMS + TickMs - 1) & ~(TickMs - 1);
    t.thinkTimeMS = (t.thinkTimeMS + TickMs - 1) & ~(TickMs - 1);
    t.dontFireInsideDamageRadius = Fields::boolean(o, "dontFireInsideDamageRadius", false);
    t.damageRadius = Fields::f32(o, "damageRadius", 0.0f);
    t.yawVariance = Fields::f32(o, "yawVariance", 0.5f);
    t.pitchVariance = Fields::f32(o, "pitchVariance", 0.5f);
    return cache.emplace(image.id, t).first->second;
}

// The turret's shape with the TurretData::preload sequences ("activate",
// "elevate", "turn") and its mount<n> nodes.
struct TurretShape {
    DTSLoadResult dts;
    int activate = -1, elevate = -1, turn = -1;
    int mount[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

const TurretShape* turretShape(const std::string& shapeFile) {
    static std::unordered_map<std::string, std::unique_ptr<TurretShape>> cache;
    if (shapeFile.empty() || !Engine::instance().filesys) return nullptr;
    std::string key = shapeFile;
    for (char& ch : key) ch = (char)std::tolower((unsigned char)ch);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::unique_ptr<TurretShape> shape;
        const auto bytes = Engine::instance().fs().read(("shapes/" + shapeFile).c_str());
        if (!bytes.empty()) {
            shape = std::make_unique<TurretShape>();
            shape->dts = loadDTS(bytes.data(), bytes.size(), shapeFile.c_str());
            for (size_t a = 0; a < shape->dts.animations.size(); ++a) {
                const char* name = shape->dts.animations[a].name.c_str();
                if (strcasecmp(name, "activate") == 0) shape->activate = (int)a;
                else if (strcasecmp(name, "elevate") == 0) shape->elevate = (int)a;
                else if (strcasecmp(name, "turn") == 0) shape->turn = (int)a;
            }
            for (size_t n = 0; n < shape->dts.nodes.size(); ++n)
                for (int m = 0; m < 8; ++m)
                    if (strcasecmp(shape->dts.nodes[n].name.c_str(), ("mount" + std::to_string(m)).c_str()) == 0)
                        shape->mount[m] = (int)n;
        }
        it = cache.emplace(key, std::move(shape)).first;
    }
    return it->second.get();
}

// graphMath.cc findCrossVector: the normalized cross product of two
// non-colinear vectors.
bool findCrossVector(const Point3F& v1, const Point3F& v2, Point3F* result) {
    const float l1 = std::sqrt(v1.x * v1.x + v1.y * v1.y + v1.z * v1.z);
    const float l2 = std::sqrt(v2.x * v2.x + v2.y * v2.y + v2.z * v2.z);
    if (l1 == 0.0f || l2 == 0.0f) return false;
    const Point3F a{v1.x / l1, v1.y / l1, v1.z / l1}, b{v2.x / l2, v2.y / l2, v2.z / l2};
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot > 0.999f || dot < -0.999f) return false;
    Point3F c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    const float lc = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    if (lc > 0.0f) c = {c.x / lc, c.y / lc, c.z / lc};
    *result = c;
    return true;
}

Point3F boxCenterOf(const ShapeBase& shape) {
    float lo[3], hi[3];
    shape.worldBox(lo, hi);
    return {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
}

// ShapeBase::getVelocity of a target.
Point3F velocityOf(const ShapeBase& shape) {
    if (auto* player = dynamic_cast<const PlayerObject*>(&shape)) return player->state.velocity;
    if (auto* vehicle = dynamic_cast<const VehicleObject*>(&shape)) return vehicle->getVelocity();
    if (auto* item = dynamic_cast<const ItemObject*>(&shape)) return {item->velocity[0], item->velocity[1], item->velocity[2]};
    return {0, 0, 0};
}

ShapeBase* shapeById(const VMValue& value) {
    // Sim::findObject(dAtoi(argv[2])): an id, never a name.
    const int id = std::atoi(value.toString().c_str());
    ScriptObject* object = id > 0 ? ScriptEngine::instance().findObject(std::to_string(id).c_str()) : nullptr;
    return object ? dynamic_cast<ShapeBase*>(object->engine.get()) : nullptr;
}

} // namespace

ShapeBase* TurretObject::target() const {
    return currTarget.empty() ? nullptr : EngineObjects::get<ShapeBase>(currTarget);
}

ShapeBase* TurretObject::engineer() const {
    return currEngineer.empty() ? nullptr : EngineObjects::get<ShapeBase>(currEngineer);
}

float TurretObject::thetaMin() const {
    // TurretData::preload clamps thetaMin to [0, 90].
    const float v = dataFloat("thetaMin", 45.0f);
    return v < 0.0f || v > 90.0f ? (v < 0.0f ? 0.0f : 90.0f) : v;
}

float TurretObject::thetaMax() const {
    // ... and thetaMax to [90, 180].
    const float v = dataFloat("thetaMax", 135.0f);
    return v < 90.0f || v > 180.0f ? (v < 90.0f ? 90.0f : 180.0f) : v;
}

float TurretObject::thetaNull() const {
    const float v = dataFloat("thetaNull", -1.0f);
    return v != -1.0f ? v : csmThetaNull;
}

int TurretObject::primaryAxis() const {
    ScriptObject* data = ScriptEngine::instance().findObject(dataBlock().c_str());
    const std::string axis = Fields::string(data, "primaryAxis", "zaxis");
    static const char* const names[] = {"yaxis", "revyaxis", "zaxis", "revzaxis"};
    for (int i = 0; i < 4; ++i)
        if (strcasecmp(axis.c_str(), names[i]) == 0) return i;
    return ZAxis;
}

float TurretObject::phiSpeed() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? turretImage(*image).degPerSecPhi : csmDefaultPhiSpeed;
}

float TurretObject::thetaSpeed() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? turretImage(*image).degPerSecTheta : csmDefaultThetaSpeed;
}

float TurretObject::activationSpeed() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? 1000.0f / (float)turretImage(*image).activationMS : csmDefaultActivationSpeed;
}

uint32_t TurretObject::deactivateDelay() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? (uint32_t)turretImage(*image).deactivateDelayMS : csmDefaultDeactivateDelay;
}

uint32_t TurretObject::thinkTime() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? (uint32_t)turretImage(*image).thinkTimeMS : csmDefaultThinkTime;
}

float TurretObject::attackRadius() const {
    const ShapeBaseImageData* image = getMountedImage(0);
    return image ? turretImage(*image).attackRadius : csmDefaultAttackRadius;
}

float TurretObject::capacitorFraction() const {
    const float max = dataFloat("maxCapacitorEnergy", 0.0f);
    return max > 0.0f ? capacitorLevel / max : 0.0f;
}

void TurretObject::onAdded() {
    // If there's a barrel, load it.
    const std::string barrel = Fields::string(script, "initialBarrel", "");
    if (!barrel.empty())
        if (auto image = ShapeBaseImageData::find(barrel)) mountImage(image, 0, false, 0);
    lastThink = 0;
    animate();
}

bool TurretObject::mountImage(std::shared_ptr<const ShapeBaseImageData> image, uint32_t slot, bool loaded,
                              uint32_t skinTag) {
    const bool ret = StaticShapeObject::mountImage(image, slot, loaded, skinTag);
    if (ret && image) {
        ScriptObject* block = ScriptEngine::instance().findObject(std::to_string(image->id).c_str());
        ScriptEngine::instance().setObjectField(script, "initialBarrel",
                                                VMValue(block && !block->name.empty() ? block->name : std::string()));
    }
    return true;
}

bool TurretObject::unmountImage(uint32_t slot) {
    if (StaticShapeObject::unmountImage(slot)) ScriptEngine::instance().setObjectField(script, "initialBarrel", VMValue(""));
    return true;
}

void TurretObject::setSkill(float skill) { skillLevel = skill <= 1 && skill > 0 ? skill : 1.0f; }

bool TurretObject::setTarget(ShapeBase* newTarget) {
    currTarget = newTarget ? newTarget->handle() : std::string();
    targetlessTime = 0;
    return newTarget != nullptr;
}

int TurretObject::getTargetId() const {
    ShapeBase* t = target();
    return t ? ScriptEngine::instance().objectId(t->script) : 0;
}

bool TurretObject::currTargetValid() const { return isValidTarget(target()); }

bool TurretObject::isValidTarget(ShapeBase* t) const {
    if (!t) return false;
    // First, let's see if this target is dead...
    if (t->getDamageValue() >= 1.0f) return false;
    // this and target need to have an associated 'target'
    if (t->targetId < 0 || targetId < 0) return false;
    // If we are loaded with seeking projectiles, then we need to check to
    // see if the target is hot enough to kill.
    const ShapeBaseImageData* image = getMountedImage(0);
    if (image && image->isSeeker && t->heat < image->minSeekHeat) return false;
    const Point3F centerBox = boxCenterOf(*t);
    const Point3F myPos{transform[3], transform[7], transform[11]};
    const float distToTarget = NavMath::len(myPos - centerBox);
    // make sure we have a fire condition before continuing
    if (image) {
        const TurretImage& data = turretImage(*image);
        // see if we have to be at least the damage radius from the target
        if (data.dontFireInsideDamageRadius && distToTarget <= data.damageRadius) return false;
        // see if we need to be a certain distance from the target
        if (image->targetingDist != 0.0f && distToTarget <= image->targetingDist) return false;
    }
    // Query sensor network: the target visible to the turret, and not
    // friendly to the target's sensor group.
    if (!ServerTargets::isTargetVisible(t->targetId, sensorGroupOf(*this)) ||
        ServerTargets::isTargetFriendly(targetId, sensorGroupOf(*t)))
        return false;
    // Let's see if the target is outside of our attack range...
    const auto muzzle = getMuzzleTransform(0);
    const Point3F mountPoint{muzzle[3], muzzle[7], muzzle[11]};
    if (NavMath::len(centerBox - mountPoint) > attackRadius()) return false;
    SimContainer::RayInfo info;
    return !SimContainer::castRay(mountPoint, centerBox, csmActiveScanMask, info, {script, t->script});
}

void TurretObject::selectTarget() {
    // Call out to script to set the target, if possible...
    callDataBlock("selectTarget");
}

void TurretObject::setAutoFire(bool status) {
    autoFire = status;
    if (!autoFire) setTarget(nullptr);
}

void TurretObject::checkReplace() {
    ShapeBase* e = engineer();
    if (e) callDataBlock("replaceCallback", {e->handle()});
    currEngineer.clear();
}

bool TurretObject::initiateReplace(ShapeBase* e) {
    if (!e) return false;
    currEngineer = e->handle();
    if (!getMountedImage(0) || state == Dormant) {
        checkReplace();
        Console::instance().printf(LogLevel::Error, "instant replace");
    } else {
        state = DeactivateForReplace;
        Console::instance().printf(LogLevel::Error, "active replace");
    }
    return true;
}

void TurretObject::updateState(bool playerControlled) {
    if (playerControlled) {
        currTarget.clear();
        if (state == Dormant) {
            state = Activating;
        } else if (state == Deactivating) {
            if (activationLevel == csmFullyActivated) {
                elevateThread = turnThread = true;
                state = Active;
            } else {
                state = Activating;
            }
        }
    } else if (getMountedImage(0)) {
        // Go to active state if we have a target, otherwise, wait for the
        // deactivate delay to expire, and deactivate...
        if (state == Active) {
            if (!target() && targetlessTime > deactivateDelay()) state = Deactivating;
        } else if (state == Deactivating) {
            if (target()) {
                if (activationLevel == csmFullyActivated) {
                    elevateThread = turnThread = true;
                    state = Active;
                } else {
                    state = Activating;
                }
            }
        } else if (state == Dormant) {
            elevateThread = turnThread = false;
            if (target()) state = Activating;
        }
    } else {
        // Go to inactive state
        currTarget.clear();
        if (state == Dormant || state == Deactivating) elevateThread = turnThread = false;
        else state = Deactivating;
    }
}

void TurretObject::aiThink() {
    lastThink = 0;
    // First, if we have a target, let's see if the target is still valid;
    // with none (or no longer valid), select one.
    if (!target() || !currTargetValid()) {
        currTarget.clear();
        selectTarget();
    }
    // Let's see if we still have a target: activate ourselves if necessary.
    if (target()) {
        if (state == Dormant) state = Activating;
        else if (state == Deactivating) state = activationLevel == csmFullyActivated ? Active : Activating;
    }
}

Point3F TurretObject::dopeAim(const Point3F& startLocation, const Point3F& aimLocation) {
    static Nav::RandomLCG rand((int32_t)(SimState::simTime() * 1000.0));
    // find the "horizontal" orthogonal vector
    Point3F horzOrth;
    if (!findCrossVector(startLocation - aimLocation, Point3F{0, 0, 1}, &horzOrth)) return aimLocation;
    // now find the "vertical" orthogonal vector
    Point3F vertOrth;
    if (!findCrossVector(startLocation - aimLocation, horzOrth, &vertOrth)) return aimLocation;
    // determine the radius factor
    const float radiusFactor = skillLevel == 1.0f ? 0.0f : 0.04f + (1.0f - skillLevel) * 0.2f;
    // calculate the radius error
    const float radiusError = NavMath::len(startLocation - aimLocation) * radiusFactor * 0.1f;
    float horzError = rand.randF() * radiusError;
    horzError *= rand.randF() < 0.5f ? -1.0f : 1.0f;
    float vertError = rand.randF() * radiusError;
    vertError *= rand.randF() < 0.5f ? -1.0f : 1.0f;
    return aimLocation + horzOrth * horzError + vertOrth * vertError;
}

void TurretObject::aiUpdateActive(float& yaw, float& pitch, bool trigger[2]) {
    ShapeBase* t = target();
    if (!t) {
        // Wait for deactivate.
        trigger[0] = trigger[1] = false;
        return;
    }
    // Let's find the vector we need to follow to hit this target...
    const auto muzzle = getMuzzleTransform(0);
    const Point3F mountPoint{muzzle[3], muzzle[7], muzzle[11]};
    Point3F targetPoint = boxCenterOf(*t);
    // only used by bots - when a player mounts, skill should be set to 1.0 (from script)
    if (skillLevel < 1.0f) targetPoint = dopeAim(mountPoint, targetPoint);
    // Can we see the target?
    SimContainer::RayInfo info;
    if (SimContainer::castRay(mountPoint, targetPoint, csmActiveScanMask, info, {script, t->script})) {
        // Can't see the center of our target.  we're going to deactivate...
        targetlessTime = 0;
        currTarget.clear();
        return;
    }
    const ShapeBaseImageData* image = getMountedImage(0);
    ScriptObject* projectile = image ? ScriptEngine::instance().findDataBlock(image->projectile) : nullptr;
    Point3F dirMin, dirMax;
    float timeMin, timeMax;
    bool allowFire = projectile && ProjectileAim::calculateAim(projectile, targetPoint, velocityOf(*t), mountPoint,
                                                               {0, 0, 0}, &dirMin, &timeMin, &dirMax, &timeMax);
    // Can't hit it: point somewhere near it...
    if (!allowFire) dirMin = targetPoint - mountPoint;
    // Let's translate this into our own coordinate system...
    const auto& m = transform;
    Point3F dir{m[0] * dirMin.x + m[4] * dirMin.y + m[8] * dirMin.z,
                m[1] * dirMin.x + m[5] * dirMin.y + m[9] * dirMin.z,
                m[2] * dirMin.x + m[6] * dirMin.y + m[10] * dirMin.z};
    const float dirLen = NavMath::len(dir);
    if (dirLen > 0.0f) dir = dir * (1.0f / dirLen);
    // Ok, now we need to derive a phi/theta angle to point this way.
    float thetaLen, newPhi = 0.0f, newTheta = 0.0f;
    switch (primaryAxis()) {
    case ZAxis:
        thetaLen = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        newPhi = std::atan2(dir.x, dir.y) * RadToDeg;
        newTheta = 180.0f - (std::atan2(dir.z, thetaLen) * RadToDeg + 90.0f);
        break;
    case YAxis:
    case RevYAxis:
        thetaLen = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        newPhi = std::atan2(-dir.x, dir.z) * RadToDeg;
        newTheta = 180.0f - (std::atan2(dir.y, thetaLen) * RadToDeg + 90.0f);
        break;
    case RevZAxis:
        thetaLen = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        newPhi = 360.0f - std::atan2(dir.x, dir.y) * RadToDeg;
        newTheta = std::atan2(dir.z, thetaLen) * RadToDeg + 90.0f;
        break;
    }
    // Lets twiddle newPhi
    if (newPhi < 0.0f) newPhi += 360.0f;
    if (std::fabs((newPhi - 360.0f) - currPhi) < std::fabs(newPhi - currPhi)) newPhi -= 360.0f;
    else if (std::fabs((newPhi + 360.0f) - currPhi) < std::fabs(newPhi - currPhi)) newPhi += 360.0f;
    yaw = newPhi - currPhi;
    pitch = newTheta - currTheta;
    // Determine whether or not we fire: within the barrel's tolerances.
    const TurretImage* data = image ? &turretImage(*image) : nullptr;
    const bool fire = allowFire && data && std::fabs(yaw) < data->yawVariance && std::fabs(pitch) < data->pitchVariance;
    trigger[0] = trigger[1] = fire;
}

void TurretObject::getMuzzleVector(uint32_t slot, float vec[3]) const {
    if (!controllingClient.empty())
        if (auto* connection = EngineObjects::get<GameConnection>(controllingClient))
            if (!(connection->script && EngineClasses::isA(connection->script->className, "AIConnection")) &&
                connection->firstPerson && getCorrectedAim(getMuzzleTransform(slot), vec))
                return;
    const float theta = currTheta * DegToRad, phi = currPhi * DegToRad;
    const float len = std::sin(theta);
    float v[3] = {0, 0, 0};
    switch (primaryAxis()) {
    case ZAxis:
        v[0] = std::sin(phi) * len;
        v[1] = std::cos(phi) * len;
        v[2] = std::cos(theta);
        break;
    case YAxis:
        v[0] = -std::sin(phi) * len;
        v[1] = std::cos(theta);
        v[2] = std::cos(phi) * len;
        break;
    case RevZAxis:
        v[0] = std::sin((360.0f - currPhi) * DegToRad) * len;
        v[1] = std::cos((360.0f - currPhi) * DegToRad) * len;
        v[2] = std::cos((180.0f - currTheta) * DegToRad);
        break;
    case RevYAxis:
        v[0] = std::sin(phi) * len;
        v[1] = -std::cos(theta);
        v[2] = -std::cos(phi) * len;
        break;
    }
    TorqueMath::mulV(transform, v, vec);
}

void TurretObject::performActivateRamp() {
    setImageLoadedState(0, true);
    activationLevel += activationSpeed() * (TickMs / 1000.0f);
    if (activationLevel >= csmFullyActivated) {
        elevateThread = turnThread = true;
        activationLevel = csmFullyActivated;
        state = Active;
    }
}

void TurretObject::performDeactivateRamp() {
    bool rampDownActive = true;
    // Check the phi angle.  This rotates, so spin down the shorter way.
    if (currPhi != 0.0f) {
        rampDownActive = false;
        const float speed = phiSpeed() * (TickMs / 1000.0f);
        if (currPhi > 180.0f) currPhi += speed;
        else currPhi -= speed;
        if (currPhi >= 360.0f || currPhi <= 0.0f) currPhi = 0.0f;
    }
    // Check the theta angle.  This is much easier
    if (currTheta != csmThetaNull) {
        rampDownActive = false;
        const float speed = thetaSpeed() * (TickMs / 1000.0f);
        const float dir = (csmThetaNull - currTheta) / std::fabs(csmThetaNull - currTheta);
        currTheta += dir * speed;
        if ((dir < 0.0f && currTheta <= csmThetaNull) || (dir > 0.0f && currTheta >= csmThetaNull))
            currTheta = csmThetaNull;
    }
    if (rampDownActive) {
        elevateThread = turnThread = false;
        setImageLoadedState(0, false);
        activationLevel -= activationSpeed() * (TickMs / 1000.0f);
        if (activationLevel <= csmFullyDeactivated) {
            activationLevel = csmFullyDeactivated;
            state = Dormant;
        }
    }
}

// Turret::processTick, server side (the retail build also recharges the
// capacitor there).
void TurretObject::processMove(const ClientMoveIn* move) {
    StaticShapeObject::processMove(move);
    if (hidden || dataBool("neverUpdateControl", false)) return;
    const bool playerControlled = move != nullptr;
    // The move, or the null move the AI fills in.
    float moveYaw = 0.0f, movePitch = 0.0f;
    bool trigger[2] = {false, false};
    if (move) {
        if (move->exact) {
            moveYaw = move->fyaw;
            movePitch = move->fpitch;
        } else {
            const auto m = PlayerPrediction::unclampMove(move->x, move->y, move->z, (uint16_t)move->yaw,
                                                         (uint16_t)move->pitch, (uint16_t)move->roll,
                                                         move->freeLook, move->trigger);
            moveYaw = m.yaw;
            movePitch = m.pitch;
        }
        trigger[0] = move->trigger[0];
        trigger[1] = move->trigger[1];
    }
    if (!mount.empty()) setMaskBits(MountedUpdateMask);
    const float maxCapacitor = dataFloat("maxCapacitorEnergy", 0.0f);
    if (capacitorLevel < maxCapacitor) {
        capacitorLevel += capacitorRechargeRate;
        if (capacitorLevel > maxCapacitor) capacitorLevel = maxCapacitor;
        if (capacitorLevel < 0.0f) capacitorLevel = 0.0f;
    }
    if (!isFrozen()) {
        updateState(playerControlled);
        // We might have to give the ai a chance to think before we allow it
        // to update...
        if (!playerControlled && autoFire && getMountedImage(0)) {
            lastThink += TickMs;
            if (lastThink >= thinkTime()) aiThink();
        }
        if (state == Dormant) {
            // Just make sure...
            activationLevel = 0.0f;
            currPhi = csmPhiNull;
            currTheta = thetaNull();
        } else if (state == Activating) {
            performActivateRamp();
            setMaskBits(BogoMask);
        } else if (state == Deactivating) {
            performDeactivateRamp();
            setMaskBits(BogoMask);
        } else if (state == Active) {
            // If we're not player controlled, the ai needs to think about
            // this move...
            float phiMove, thetaMove;
            if (!playerControlled) {
                aiUpdateActive(moveYaw, movePitch, trigger);
                phiMove = std::fabs(moveYaw);
                thetaMove = std::fabs(movePitch);
                if (!target()) targetlessTime += TickMs;
            } else {
                phiMove = std::fabs(moveYaw * sgBigClunkyMultiplicationFactor);
                thetaMove = std::fabs(movePitch * sgBigClunkyMultiplicationFactor);
            }
            const float phiPerTick = phiSpeed() * (TickMs / 1000.0f);
            const float thetaPerTick = thetaSpeed() * (TickMs / 1000.0f);
            if (phiMove > phiPerTick) phiMove = phiPerTick;
            if (moveYaw < 0.0f) phiMove *= -1.0f;
            if (thetaMove > thetaPerTick) thetaMove = thetaPerTick;
            if (movePitch < 0.0f) thetaMove *= -1.0f;
            currPhi += phiMove;
            currTheta += thetaMove;
            // Clamp phi to [0, 360)
            while (currPhi >= 360.0f) currPhi -= 360.0f;
            while (currPhi < 0.0f) currPhi += 360.0f;
            // Clamp theta
            if (currTheta > thetaMax()) currTheta = thetaMax();
            else if (currTheta < thetaMin()) currTheta = thetaMin();
            setMaskBits(BogoMask);
        } else if (state == DeactivateForReplace) {
            if (engineer()) {
                performDeactivateRamp();
                // We just went dormant.  Let's see if we can replace the
                // barrel here...
                if (state == Dormant) checkReplace();
            } else {
                state = Deactivating;
            }
        }
        const bool active = state == Active;
        setImageTriggerState(0, active && trigger[0]);
        setImageTriggerState(1, active && trigger[1]);
    } else {
        // If we're frozen, we do nothing but clear our current target (if any)
        currTarget.clear();
        currEngineer.clear();
    }
    updateContainer();
    heat = waterCoverage > 0.0f ? 0.0f : std::clamp(dataFloat("heat", 1.0f), 0.0f, 1.0f);
    // Set current positional state...
    activateThread = activationLevel != 0.0f;
    animate();
}

// setOrientationThreads and mShapeInstance->animate(): the activate thread
// at the activation level, elevate at theta / 180 and turn at phi / 360.
void TurretObject::animate() {
    Pose p;
    p.valid = true;
    p.activate = activateThread;
    p.activatePos = activationLevel;
    float theta = currTheta, phi = currPhi;
    if (theta < thetaMin() || theta > thetaMax()) theta = theta < thetaMin() ? thetaMin() : thetaMax();
    while (phi >= 360.0f) phi -= 360.0f;
    while (phi < 0.0f) phi += 360.0f;
    p.elevate = elevateThread;
    p.elevatePos = theta / 180.0f;
    p.turn = turnThread;
    p.turnPos = phi / 360.0f;
    if (pose.valid && p.activate == pose.activate && p.elevate == pose.elevate && p.turn == pose.turn &&
        p.activatePos == pose.activatePos && p.elevatePos == pose.elevatePos && p.turnPos == pose.turnPos)
        return;
    pose = p;
    for (bool& posed : mountPosed) posed = false;
    const TurretShape* shape = turretShape(shapeFileOf(*this));
    if (!shape) return;
    std::vector<std::pair<int, float>> threads;
    if (p.activate && shape->activate != -1) threads.push_back({shape->activate, p.activatePos});
    if (p.elevate && shape->elevate != -1) threads.push_back({shape->elevate, p.elevatePos});
    if (p.turn && shape->turn != -1) threads.push_back({shape->turn, p.turnPos});
    const std::vector<MatrixF> nodes = dtsThreadsPose(shape->dts, threads);
    // The loader's frame has height in y and forward in z.
    static const int axis[4] = {0, 2, 1, 3};
    for (int i = 0; i < 8; ++i) {
        const int n = shape->mount[i];
        if (n < 0 || n >= (int)nodes.size()) continue;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) mountPose[i][r * 4 + c] = nodes[n].m[axis[r]][axis[c]];
        mountPosed[i] = true;
    }
}

std::array<float, 16> TurretObject::getMountTransform(uint32_t mountPoint) const {
    if (mountPoint < 8 && mountPosed[mountPoint]) return TorqueMath::mul(transform, mountPose[mountPoint]);
    return StaticShapeObject::getMountTransform(mountPoint);
}

// Retail Turret::packUpdate: the capacitor while mounted, the control
// shortcut, then phi, theta and activation.
uint32_t TurretObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = StaticShapeObject::packUpdate(connection, mask, w);
    if (w.writeFlag(!mount.empty())) w.writeFloat(capacitorFraction(), 8);
    // The rest of the data is part of the control object packet update.
    // If we're controlled by this client, we don't need to send it.
    if (w.writeFlag(controlledBy(*this, connection) && !(mask & InitialUpdateMask))) return ret;
    if (w.writeFlag(mask & BogoMask)) {
        const float compressedPhi = currPhi / 360.0f;
        const float compressedTheta = (currTheta - thetaMin()) / (thetaMax() - thetaMin());
        w.writeFloat(compressedPhi, 10);
        w.writeFloat(compressedTheta, 10);
        w.writeFloat(activationLevel, 8);
    }
    return ret;
}

bool TurretObject::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = StaticShapeObject::writePacketData(connection, w);
    // Dump entire state
    w.writeRangedU32((uint32_t)state, Dormant, DeactivateForReplace);
    w.writeF32(activationLevel);
    w.writeF32(currPhi);
    w.writeF32(currTheta);
    return ret;
}

void ItemObject::readFields() {
    ShapeBase::readFields();
    rotate = Fields::boolean(script, "rotate", false);
    isStatic = Fields::boolean(script, "static", false);
    collideable = Fields::boolean(script, "collideable", false);
}

uint32_t ItemObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = ShapeBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        w.writeFlag(rotate);
        w.writeFlag(isStatic);
        w.writeFlag(collideable);
        if (w.writeFlag(scale[0] != 1 || scale[1] != 1 || scale[2] != 1)) writeScale(w);
    }
    w.writeFlag(false); // no thrower collision object
    if (w.writeFlag((mask & RotationMask) && !rotate)) {
        // Assumes rotation about the z axis.
        const TorqueMath::AngAxis aa = TorqueMath::angAxis(TorqueMath::quat(transform));
        w.writeFlag(aa.z < 0);
        w.writeF32(aa.angle);
    }
    if (w.writeFlag(mask & PositionMask)) {
        w.writePoint({transform[3], transform[7], transform[11]});
        if (!w.writeFlag(atRest)) w.writePoint({velocity[0], velocity[1], velocity[2]});
        w.writeFlag(!(mask & NoWarpMask));
    }
    return ret;
}

void ItemObject::setVelocity(const float v[3]) {
    for (int i = 0; i < 3; ++i) velocity[i] = v[i];
    setMaskBits(PositionMask);
    atRest = false;
    atRestCounter = 0;
}

// Item::processTick, server side.
void ItemObject::processMove(const ClientMoveIn* move) {
    ShapeBase::processMove(move);
    if (!collisionObject.empty() && --collisionTimeout <= 0) collisionObject.clear();
    const bool sticky = dataBool("sticky", false);
    if (atRest && !isStatic && !sticky && ++atRestCounter > 64) {
        atRest = false;
        atRestCounter = 0;
    }
    if (!isStatic && !atRest && !hidden) {
        updateVelocity(0.032f);
        updatePos(0.032f);
    }
}

void ItemObject::updateVelocity(float dt) {
    const float gravityMod = dataFloat("gravityMod", 1.0f);
    velocity[2] += SimState::server().gravity * gravityMod * dt;
    const float maxVelocity = dataFloat("maxVelocity", -1.0f);
    const float len = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    if (maxVelocity > 0 && len > maxVelocity * 1.05f) {
        const float k = (1.0f - maxVelocity / len) * 0.1f;
        for (float& v : velocity) v -= v * k;
    }
    // Container buoyancy & drag
    velocity[2] -= buoyancy * (SimState::server().gravity * gravityMod * this->gravityMod) * dt;
    for (float& v : velocity) v -= v * drag * dt;
}

// Item::updatePos. The engine casts from the box top centre (start) to the
// box bottom centre (end) for contact, then sweeps the box against the
// working set; APPROXIMATION: the sweep is a ray through the box centre.
void ItemObject::updatePos(float dt) {
    float pos[3] = {transform[3], transform[7], transform[11]};
    float lo[3], hi[3];
    worldBox(lo, hi);
    const float top = hi[2] - pos[2], bottom = lo[2] - pos[2], mid = (top + bottom) * 0.5f;
    const float cx = (lo[0] + hi[0]) * 0.5f - pos[0], cy = (lo[1] + hi[1]) * 0.5f - pos[1];
    const float friction = dataFloat("friction", 0.0f), elasticity = dataFloat("elasticity", 0.0f);
    const bool sticky = dataBool("sticky", false);
    bool contact = false, stickyNotify = false;
    auto respond = [&](const Nav::RayHit& hit) {
        float bd = -(velocity[0] * hit.normal.x + velocity[1] * hit.normal.y + velocity[2] * hit.normal.z);
        if (bd < 0) return false;
        if (sticky) {
            velocity[0] = velocity[1] = velocity[2] = 0;
            atRest = true;
            atRestCounter = 0;
            stickyNotify = true;
            stickyPos[0] = hit.point.x; stickyPos[1] = hit.point.y; stickyPos[2] = hit.point.z;
            stickyNormal[0] = hit.normal.x; stickyNormal[1] = hit.normal.y; stickyNormal[2] = hit.normal.z;
            return true;
        }
        const float n[3] = {hit.normal.x, hit.normal.y, hit.normal.z};
        float fv[3] = {velocity[0] + n[0] * bd, velocity[1] + n[1] * bd, velocity[2] + n[2] * bd};
        const float fvl = std::sqrt(fv[0] * fv[0] + fv[1] * fv[1] + fv[2] * fv[2]);
        if (fvl > 0) {
            const float ff = bd * friction;
            if (ff < fvl) for (float& f : fv) f *= ff / fvl;
        }
        bd *= 1 + elasticity;
        for (int i = 0; i < 3; ++i) velocity[i] += n[i] * (bd + 0.002f) - fv[i];
        contact = true;
        return false;
    };
    Nav::RayHit hit;
    const float end0[3] = {pos[0] + velocity[0] * dt, pos[1] + velocity[1] * dt, pos[2] + velocity[2] * dt};
    const bool stuck = Nav::castRay({pos[0] + cx, pos[1] + cy, pos[2] + top},
                                    {end0[0] + cx, end0[1] + cy, end0[2] + bottom}, 0xFFFFFFFFu, hit) &&
                       respond(hit);
    if (!stuck) {
        float time = dt;
        int count = 0;
        for (; count < 3; ++count) {
            const float end[3] = {pos[0] + velocity[0] * time, pos[1] + velocity[1] * time, pos[2] + velocity[2] * time};
            if (!Nav::castRay({pos[0] + cx, pos[1] + cy, pos[2] + mid}, {end[0] + cx, end[1] + cy, end[2] + mid},
                              0xFFFFFFFFu, hit)) {
                for (int i = 0; i < 3; ++i) pos[i] = end[i];
                break;
            }
            // To the collision point, less a margin.
            const float dx = hit.point.x - (pos[0] + cx), dy = hit.point.y - (pos[1] + cy), dz = hit.point.z - (pos[2] + mid);
            const float travel = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]) * time;
            const float t = travel > 0 ? std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) / travel - 0.01f) : 0.0f;
            for (int i = 0; i < 3; ++i) pos[i] += velocity[i] * time * t;
            time -= time * t;
            if (respond(hit)) break;
        }
        if (count == 3) velocity[0] = velocity[1] = velocity[2] = 0;
    }
    transform[3] = pos[0];
    transform[7] = pos[1];
    transform[11] = pos[2];
    updateContainer();
    if (contact) {
        const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
        if (speed < 0.15f) {
            velocity[0] = velocity[1] = velocity[2] = 0;
            atRest = true;
            atRestCounter = 0;
        }
        // Only static geometry is hit here: the client hears of the final
        // rest position.
        if (atRest) setMaskBits(PositionMask);
    }
    notifyCollision();
    if (stickyNotify) callDataBlock("onStickyCollision");
}

uint32_t BeaconObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = StaticShapeObject::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & BeaconMask)) w.writeInt(beaconType, 2);
    return ret;
}

void registerStaticShapeNatives(TorqueScript& ts) {
    EngineObjects::registerClass("StaticShape", [] { return std::make_shared<StaticShapeObject>(); });
    EngineObjects::registerClass("ScopeAlwaysShape",
                                 [] { return std::make_shared<StaticShapeObject>("ScopeAlwaysShape", true); });
    EngineObjects::registerClass("Turret", [] { return std::make_shared<TurretObject>(); });
    auto turret = [](const std::vector<VMValue>& args) -> TurretObject* {
        return args.empty() ? nullptr : EngineObjects::get<TurretObject>(args[0].toString());
    };
    auto arg = [](const std::vector<VMValue>& args, size_t i) { return i < args.size() ? args[i] : VMValue(""); };
    ts.registerNative("Turret::setSkill", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        if (auto* t = turret(args)) t->setSkill(arg(args, 1).toFloat());
        return VMValue("");
    });
    ts.registerNative("Turret::setTargetObject", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        auto* t = turret(args);
        return VMValue(t && t->setTarget(shapeById(arg(args, 1))) ? 1 : 0);
    });
    ts.registerNative("Turret::clearTarget", [turret](const std::vector<VMValue>& args) -> VMValue {
        if (auto* t = turret(args)) t->setTarget(nullptr);
        return VMValue("");
    });
    ts.registerNative("Turret::getTargetObject", [turret](const std::vector<VMValue>& args) -> VMValue {
        auto* t = turret(args);
        return VMValue(t ? t->getTargetId() : 0);
    });
    ts.registerNative("Turret::isValidTarget", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        auto* t = turret(args);
        ShapeBase* target = shapeById(arg(args, 1));
        return VMValue(t && target && t->isValidTarget(target) ? 1 : 0);
    });
    ts.registerNative("Turret::initiateBarrelSwap", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        auto* t = turret(args);
        ShapeBase* engineer = shapeById(arg(args, 1));
        return VMValue(t && engineer && t->initiateReplace(engineer) ? 1 : 0);
    });
    ts.registerNative("Turret::setAutoFire", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        const std::string value = arg(args, 1).toString();
        if (auto* t = turret(args)) t->setAutoFire(strcasecmp(value.c_str(), "true") == 0 || std::atoi(value.c_str()) > 0);
        return VMValue("");
    });
    // The retail capacitor: plain field accessors; getCapacitorLevel is an
    // integer command (the level truncated).
    ts.registerNative("Turret::setCapacitorRechargeRate", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        if (auto* t = turret(args)) t->capacitorRechargeRate = arg(args, 1).toFloat();
        return VMValue("");
    });
    ts.registerNative("Turret::setCapacitorLevel", [turret, arg](const std::vector<VMValue>& args) -> VMValue {
        if (auto* t = turret(args)) t->capacitorLevel = arg(args, 1).toFloat();
        return VMValue("");
    });
    ts.registerNative("Turret::getCapacitorLevel", [turret](const std::vector<VMValue>& args) -> VMValue {
        auto* t = turret(args);
        return VMValue(t ? (int)t->capacitorLevel : 0);
    });
    EngineObjects::registerClass("BeaconObject", [] { return std::make_shared<BeaconObject>(); });
    static const char* const beaconTypes[] = {"enemy", "friend", "vehicle"};
    ts.registerNative("BeaconObject::setBeaconType", [](const std::vector<VMValue>& args) -> VMValue {
        auto* beacon = args.empty() ? nullptr : EngineObjects::get<BeaconObject>(args[0].toString());
        const std::string type = args.size() > 1 ? args[1].toString() : std::string();
        for (int i = 0; i < 3; ++i)
            if (strcasecmp(type.c_str(), beaconTypes[i]) == 0) {
                if (beacon) {
                    beacon->beaconType = i;
                    beacon->setMaskBits(BeaconObject::BeaconMask);
                }
                return VMValue("");
            }
        Console::instance().printf(LogLevel::Error, "BeaconObject::cGetBeaconType: invalid beacon type [%s]", type.c_str());
        return VMValue("");
    });
    ts.registerNative("BeaconObject::getBeaconType", [](const std::vector<VMValue>& args) -> VMValue {
        auto* beacon = args.empty() ? nullptr : EngineObjects::get<BeaconObject>(args[0].toString());
        return VMValue(beacon ? beaconTypes[std::clamp(beacon->beaconType, 0, 2)] : "");
    });
    EngineObjects::registerClass("Item", [] { return std::make_shared<ItemObject>(); });
    using Args = std::vector<VMValue>;
    auto item = [](const Args& args) -> ItemObject* {
        return args.empty() ? nullptr : EngineObjects::get<ItemObject>(args[0].toString());
    };
    auto vec = [](const VMValue& v, float out[3]) {
        out[0] = out[1] = out[2] = 0;
        std::sscanf(v.toString().c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
    };
    auto fmt = [](const float v[3]) {
        char b[100];
        std::snprintf(b, sizeof(b), "%g %g %g", v[0], v[1], v[2]);
        return VMValue(b);
    };
    // Item::applyImpulse: items ignore angular velocity; the new velocity
    // replaces the old (impulse / mass).
    ts.registerNative("Item::applyImpulse", [item, vec](const Args& args) -> VMValue {
        auto* i = item(args);
        if (!i || args.size() < 3) return VMValue("");
        float v[3];
        vec(args[2], v);
        const float mass = i->dataFloat("mass", 1.0f);
        if (mass > 0) for (float& c : v) c /= mass;
        i->setVelocity(v);
        return VMValue("");
    });
    ts.registerNative("Item::setVelocity", [item, vec](const Args& args) -> VMValue {
        auto* i = item(args);
        if (!i || args.size() < 2) return VMValue(0);
        float v[3];
        vec(args[1], v);
        i->setVelocity(v);
        return VMValue(1);
    });
    ts.registerNative("Item::getVelocity", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float zero[3] = {0, 0, 0};
        return fmt(i ? i->velocity : zero);
    });
    ts.registerNative("Item::isStatic", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        return VMValue(i && i->isStatic ? 1 : 0);
    });
    ts.registerNative("Item::isRotating", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        return VMValue(i && i->rotate ? 1 : 0);
    });
    ts.registerNative("Item::setCollisionTimeout", [item](const Args& args) -> VMValue {
        auto* i = item(args);
        ScriptObject* obj = args.size() > 1 ? ScriptEngine::instance().findObject(args[1].toString().c_str()) : nullptr;
        if (!i || !obj || !EngineObjects::get<ShapeBase>(args[1].toString())) return VMValue(0);
        i->collisionObject = std::to_string(ScriptEngine::instance().objectId(obj));
        i->collisionTimeout = 15;
        i->setMaskBits(ItemObject::ThrowSrcMask);
        return VMValue(1);
    });
    ts.registerNative("Item::getLastStickyPos", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float zero[3] = {0, 0, 0};
        return fmt(i ? i->stickyPos : zero);
    });
    ts.registerNative("Item::getLastStickyNormal", [item, fmt](const Args& args) -> VMValue {
        auto* i = item(args);
        const float up[3] = {0, 0, 1};
        return fmt(i ? i->stickyNormal : up);
    });
    ts.registerNative("StaticShape::setPoweredState", [](const Args& args) -> VMValue {
        if (auto* s = args.empty() ? nullptr : EngineObjects::get<StaticShapeObject>(args[0].toString()))
            if (args.size() > 1) s->powered = args[1].toBool();
        return VMValue("");
    });
    ts.registerNative("StaticShape::getPoweredState", [](const Args& args) -> VMValue {
        auto* s = args.empty() ? nullptr : EngineObjects::get<StaticShapeObject>(args[0].toString());
        return VMValue(s && s->powered ? 1 : 0);
    });
}
