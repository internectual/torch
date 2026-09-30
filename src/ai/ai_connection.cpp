#include "ai/ai_connection.h"
#include "sim/projectiles.h"
#include "ai/ai_step.h"
#include "ai/ai_task.h"
#include "sim/containers.h"
#include "sim/nav_graph.h"
#include "sim/player.h"
#include "sim/projectile_aim.h"
#include "sim/sim_state.h"
#include "sim/target_manager.h"
#include "sim/torque_math.h"
#include "sim/vehicle.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <strings.h>

using namespace SimContainer;

namespace {

constexpr float Pi = 3.14159265358979323846f;
constexpr float TwoPi = 2.0f * Pi;
constexpr float EqualConst = 0.000001f; // __EQUAL_CONST_F

int gAIDetectionOffset = 0;

float randF() { return Nav::gRandGen().randF(); }
int now() { return (int)simTimeMs(); }
bool isZeroF(float v) { return std::fabs(v) < EqualConst; }
bool isDead(PlayerObject* player) { return strcasecmp(player->stateName(), "dead") == 0; }
bool same(const Point3F& a, const Point3F& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
Point3F flat(const Point3F& p) { return {p.x, p.y, 0}; }
Point3F position(const SceneObject& o) { return {o.transform[3], o.transform[7], o.transform[11]}; }

// findCrossVector (ai/aiMath.cc): v1 x v2 normalized; false when degenerate.
bool findCrossVector(const Point3F& v1, const Point3F& v2, Point3F* result) {
    if (len(v1) < 0.001f || len(v2) < 0.001f) return false;
    *result = cross(v1, v2);
    if (len(*result) < 0.001f) return false;
    *result = normalize(*result);
    return true;
}

PlayerObject* connectionPlayer(GameConnection* connection) {
    return connection ? EngineObjects::get<PlayerObject>(connection->controlObject()) : nullptr;
}

Point3F eyePosition(const ShapeBase& shape) {
    const auto eye = shape.getEyeTransform();
    return {eye[3], eye[7], eye[11]};
}

const char* gTargetObjectMode[AIConnection::NumObjectModes] = {"Destroy", "Repair", "Laze", "Mortar", "Missile",
                                                               "MissileNoLock", "AttackMode1", "AttackMode2",
                                                               "AttackMode3", "AttackMode4"};

void callScript(const char* function, const std::vector<std::string>& args) {
    auto* ts = ScriptEngine::instance().ts();
    if (!ts) return;
    std::vector<VMValue> values;
    for (const auto& a : args) values.emplace_back(a);
    ts->callFunction(function, values);
}

std::string fstr(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%f", v);
    return buffer;
}

} // namespace

bool gAISystemEnabled() { return SimState::server().aiSystemEnabled; }

AIConnection::AIConnection() {
    isServer = true;
    local = false;
    mPlayerDetectionCounter = gAIDetectionOffset;
    if (++gAIDetectionOffset >= 3) gAIDetectionOffset = 0;
}

AIConnection::~AIConnection() {
    clearTasks();
    clearStep();
}

int AIConnection::id() const { return script ? ScriptEngine::instance().objectId(script) : 0; }

PlayerObject* AIConnection::controlPlayer() const { return EngineObjects::get<PlayerObject>(controlObject()); }

ScriptObject* AIConnection::projectileData() const {
    return mProjectileName.empty() ? nullptr : ScriptEngine::instance().findObject(mProjectileName.c_str());
}

void AIConnection::setMoveSpeed(float speed) { mMoveSpeed = speed <= 0.0f ? 0.0f : std::min(1.0f, speed); }

void AIConnection::setMoveMode(int mode, bool abortStuckCode) {
    if (mode < 0 || mode >= ModeCount) mode = 0;
    // A stuck bot finishes "unsticking" first (setMoveDestination aborts it).
    if (mMoveMode == ModeStuck && mode != ModeStop && !abortStuckCode) return;
    // make sure we're not moving if in the middle of a jet...
    mMoveModePending = mode;
    if (!mNavUsingJet) mMoveMode = mode;
}

void AIConnection::setMoveDestination(const Point3F& location) {
    if (!same(mMoveDestination, location) && mMoveMode == ModeStuck) {
        setMoveMode(ModeExpress, true);
        mStuckLocation = {0, 0, 0};
    }
    mMoveDestination = location;
}

bool AIConnection::setScriptAimLocation(const Point3F& location, int duration) {
    // can't set through scripts while aiming at an object or engage player
    if (mTargetPlayer || EngineObjects::get<ShapeBase>(mTargetObject)) return false;
    setAimLocation(location);
    mLookAtTargetTimeMS = std::max(mLookAtTargetTimeMS, now() + duration);
    return true;
}

bool AIConnection::scriptIsAiming() const { return now() < mLookAtTargetTimeMS; }

void AIConnection::setPilotPitchRange(float pitchUpMax, float pitchDownMax, float pitchIncMax) {
    mPitchUpMax = pitchUpMax;
    mPitchDownMax = pitchDownMax;
    mPitchIncMax = pitchIncMax;
}

void AIConnection::setPilotDestination(const Point3F& dest, float maxSpeed) {
    // must always aim where you're flying to
    mPilotDestination = dest;
    mPilotAimLocation = dest;
    mPilotSpeed = std::max(0.0f, std::min(1.0f, maxSpeed));
}

void AIConnection::setPilotAimLocation(const Point3F& aimLocation) {
    // if you're aiming, you can't move
    mPilotAimLocation = aimLocation;
    mPilotSpeed = 0;
}

void AIConnection::setWeaponInfo(const std::string& projectile, int minDist, int maxDist, int triggerCount,
                                 float energyRequired, float errorFactor) {
    mEngageMinDistance = minDist;
    mEngageMaxDistance = maxDist;
    mTriggerCounter = triggerCount;
    mScriptTriggerCounter = -1;
    mWeaponEnergy = energyRequired;
    mWeaponErrorFactor = errorFactor;
    mProjectileName.clear();
    if (!projectile.empty() && strcasecmp(projectile.c_str(), "NoAmmo") != 0) {
        if (ScriptEngine::instance().findObject(projectile.c_str()))
            mProjectileName = projectile;
        else
            Console::instance().printf(LogLevel::Info, "setWeaponInfo() failed - unable to find datablock: %s",
                                       projectile.c_str());
    }
}

void AIConnection::setEngageTarget(GameConnection* target) {
    const std::string key = target && target->script ? ScriptEngine::instance().objectKey(target->script) : "";
    // reset the engagement if we're aiming at someone new...
    if (target && key != mEngageTarget) {
        mTargetInRange = false;
        mTargetInSight = false;
        mEngageState = ChooseWeapon;
        mFiring = false;
        mTriggerCounter = 0;
        mScriptTriggerCounter = -1;
    }
    mEngageTarget = key;
    mTargetPlayer = connectionPlayer(target);
    // make sure we actually got a target
    if (!mTargetPlayer) mEngageTarget.clear();
}

int AIConnection::getEngageTarget() {
    auto* connection = mEngageTarget.empty() ? nullptr : EngineObjects::get<GameConnection>(mEngageTarget);
    PlayerObject* targetPlayer = connectionPlayer(connection);
    // no one, or the target is dead
    if (!targetPlayer || isDead(targetPlayer)) {
        mTargetPlayer = nullptr;
        mEngageTarget.clear();
        return -1;
    }
    return ScriptEngine::instance().objectId(connection->script);
}

void AIConnection::setVictim(GameConnection* victim, PlayerObject* corpse) {
    mVictim = victim && victim->script ? ScriptEngine::instance().objectKey(victim->script) : "";
    mCorpse = corpse && corpse->script ? ScriptEngine::instance().objectKey(corpse->script) : "";
    mVictimTime = now();
}

int AIConnection::getVictimCorpse() {
    auto* corpse = mCorpse.empty() ? nullptr : EngineObjects::get<PlayerObject>(mCorpse);
    return corpse ? ScriptEngine::instance().objectId(corpse->script) : -1;
}

void AIConnection::setTargetObject(ShapeBase* targetObject, float range, int objectMode) {
    const std::string key =
        targetObject && targetObject->script ? ScriptEngine::instance().objectKey(targetObject->script) : "";
    // reset the engagement if not aiming at a player and this is a new object
    if (!mTargetPlayer && (key.empty() || mTargetObject.empty() || key != mTargetObject)) {
        mTargetInRange = false;
        mTargetInSight = false;
        mEngageState = ChooseWeapon;
        mFiring = false;
        mTriggerCounter = 0;
        mScriptTriggerCounter = -1;
    }
    mTargetObject = key;
    mRangeToTarget = range;
    mObjectMode = objectMode;
}

int AIConnection::getTargetObject() {
    auto* object = mTargetObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(mTargetObject);
    return object ? ScriptEngine::instance().objectId(object->script) : -1;
}

void AIConnection::setPathDest(const Point3F* dest) {
    if (dest) mPathDest = *dest;
    mNewPath = true;
}

// Pass down jetting abilities to the path machinery. (Tribes' Player jet
// ability query was removed from the player class: all zero.)
void AIConnection::setPathCapabilities(PlayerObject* player) {
    JetManager::Ability ability;
    player->getJetAbility(ability.acc, ability.dur, ability.v0);
    mPath.setJetAbility(ability);
}

float AIConnection::getPathDistance(const Point3F& destination, const Point3F& source) {
    Point3F sourceLocation = source;
    if (same(sourceLocation, {-1, -1, -1})) {
        PlayerObject* myPlayer = controlPlayer();
        if (!myPlayer) return -1;
        setPathCapabilities(myPlayer);
        sourceLocation = position(*myPlayer);
        // make sure the client can actually get to the destination
        if (!mPath.canReachLoc(destination)) return -1;
    }
    // Vertical movement is exaggerated (the bots avoid it).
    const Point3F distVec = sub(sourceLocation, destination);
    return std::sqrt(distVec.x * distVec.x + distVec.y * distVec.y + 9 * distVec.z * distVec.z);
}

float AIConnection::getPathDistRemaining(float maxDist) {
    if (mPath.isPathCurrent()) return mPath.distRemaining(maxDist);
    return std::min(maxDist, len(sub(mLocation, mMoveDestination)));
}

Point3F AIConnection::getLOSLocation(const Point3F& targetPoint, float minDistance, float maxDistance,
                                     const Point3F& nearPoint) {
    PlayerObject* myPlayer = controlPlayer();
    if (!myPlayer) return targetPoint;
    const Point3F sourceLocation = position(*myPlayer);
    const Point3F nearLocation = same(nearPoint, {-1, -1, -1}) ? sourceLocation : nearPoint;
    if (minDistance < 0)
        return NavigationGraph::findLOSLocation(sourceLocation, targetPoint, 0, SphereF{nearLocation, 1e6f}, maxDistance);
    return NavigationGraph::findLOSLocation(sourceLocation, targetPoint, minDistance,
                                            SphereF{nearLocation, minDistance / 2.0f}, maxDistance);
}

Point3F AIConnection::getHideLocation(const Point3F& targetPoint, float range, const Point3F& nearPoint,
                                      float hideLength) {
    Point3F sourceLocation = nearPoint;
    if (same(sourceLocation, {-1, -1, -1})) {
        PlayerObject* myPlayer = controlPlayer();
        if (!myPlayer) return targetPoint;
        sourceLocation = position(*myPlayer);
    }
    if (hideLength <= 0) return NavigationGraph::hideOnSlope(sourceLocation, targetPoint, range, 20);
    return NavigationGraph::hideOnDistance(sourceLocation, targetPoint, range, hideLength);
}

void AIConnection::process(ShapeBase* ctrlObject) {
    if (!ctrlObject) return;
    // update the task queue: the highest weight is the current task
    AITask* highestWeightTask = nullptr;
    std::string highestKey;
    for (const auto& key : std::vector<std::string>(mTaskList)) {
        auto* task = EngineObjects::get<AITask>(key);
        if (!task) continue;
        task->calcWeight(this);
        if (!highestWeightTask || task->getWeight() > highestWeightTask->getWeight()) {
            highestWeightTask = task;
            highestKey = key;
        }
    }
    // Path needs team for avoiding threats
    mPath.setTeam(ServerTargets::connectionSensorGroup(*this));
    if (highestWeightTask && mCurrentTask != highestKey) {
        if (AITask* current = currentTask()) current->retire(this);
        highestWeightTask->assume(this);
        mCurrentTask = highestKey;
        mCurrentTaskTime = now();
    }
    if (AITask* current = currentTask()) current->monitor(this);

    if (auto* myPlayer = dynamic_cast<PlayerObject*>(ctrlObject)) {
        initProcessVars(myPlayer);
        if (AIStep* s = step()) s->process(this, myPlayer);
        if (mTurretMountedId <= 0) processEngagement(myPlayer);
        if (myPlayer->mount.empty()) processMovement(myPlayer);
        else processVehicleMovement(myPlayer);
    } else if (auto* myVehicle = dynamic_cast<VehicleObject*>(ctrlObject)) {
        processPilotVehicle(myVehicle);
    }
}

float AIConnection::get2DDot(const Point3F& vec1, const Point3F& vec2) {
    // No normalization of a zero length vector.
    const Point3F v1{vec1.x, vec1.y, 0}, v2{vec2.x, vec2.y, 0};
    const float len1 = len(v1), len2 = len(v2);
    if (len1 < EqualConst || len2 < EqualConst) return 1;
    return dot(mul(v1, 1 / len1), mul(v2, 1 / len2));
}

// The 2D heading from basePt to endPt, 0..2pi.
float AIConnection::get2DAngle(const Point3F& endPt, const Point3F& basePt) {
    const Point3F direction2D{endPt.x - basePt.x, endPt.y - basePt.y, 0};
    float angularDirection;
    if (!isZeroF(direction2D.y)) angularDirection = std::atan2(direction2D.x, direction2D.y);
    else if (direction2D.x < 0) angularDirection = -Pi / 2.0f;
    else angularDirection = Pi / 2.0f;
    if (angularDirection < 0) angularDirection += TwoPi;
    else if (angularDirection >= TwoPi) angularDirection -= TwoPi;
    return angularDirection;
}

Point3F AIConnection::dopeAimLocation(const Point3F& startLocation, const Point3F& aimLocation) {
    // the plane perpendicular to the direction we are firing
    Point3F horzOrth, vertOrth;
    if (!findCrossVector(sub(startLocation, aimLocation), {0, 0, 1}, &horzOrth)) return aimLocation;
    if (!findCrossVector(sub(startLocation, aimLocation), horzOrth, &vertOrth)) return aimLocation;
    const float radiusFactor = mSkillLevel == 1.0f ? 0.0f : 0.04f + (1.0f - mSkillLevel) * 0.2f;
    float radiusError = len(sub(startLocation, aimLocation)) * radiusFactor * mWeaponErrorFactor;
    ScriptObject* projectile = projectileData();
    if (projectile && !Fields::boolean(projectile, "isBallistic", false) && mTargStillTimeMS > 0 && radiusError > 0) {
        // begin honing in after 3 seconds, and over the next 3 seconds
        const int elapsedTime = now() - mTargStillTimeMS - 3000;
        if (elapsedTime > 0) radiusError = radiusError * (float(3000 - std::min(elapsedTime, 3000)) / 3000.0f);
    }
    const float horzError = randF() * radiusError * (randF() < 0.5f ? -1.0f : 1.0f);
    const float vertError = randF() * radiusError * (randF() < 0.5f ? -1.0f : 1.0f);
    return add(aimLocation, add(mul(horzOrth, horzError), mul(vertOrth, vertError)));
}

float AIConnection::getOutdoorRadius(const Point3F& location) {
    float freedomRadius;
    if (mPath.locationIsOutdoors(location, &freedomRadius)) return freedomRadius;
    return -1;
}

void AIConnection::initProcessVars(PlayerObject* player) {
    mLocation = position(*player);
    mVelocity = player->state.velocity;
    mVelocity2D = flat(mVelocity);
    mRotation = {0, 0, player->state.yaw};
    mHeadRotation = {player->state.headPitch, 0, player->state.headYaw};
    mEnergy = player->getEnergyValue();
    mDamage = player->getDamageValue();
    if (mEnergy < mEnergyReserve) mEnergyRecharge = true;
    else if (mEnergy > mEnergyReserve + mEnergyFloat) mEnergyRecharge = false;
    mEnergyAvailable = ((mEnergy > mEnergyReserve + mEnergyFloat) || (mEnergy > mEnergyReserve && !mEnergyRecharge)) &&
                       (mEnergy > mWeaponEnergy - mEnergyFloat);
    // the muzzle point (getMuzzlePointAI: the muzzle transform's position)
    float muzzle[3];
    player->getMuzzlePoint(0, muzzle);
    mMuzzlePosition = {muzzle[0], muzzle[1], muzzle[2]};
    mEyePosition = eyePosition(*player);
    // how far we have to go, and whether we're heading the right way
    mDistToNode2D = len(sub(flat(mNodeLocation), flat(mLocation)));
    mDotOffCourse = std::clamp(get2DDot(sub(mNodeLocation, mLocation), mVelocity2D), -1.0f, 1.0f);
    float dummy;
    const bool outdoors = mPath.locationIsOutdoors(mLocation, &dummy);
    mHeadingDownhill = false;
    if (len(mVelocity2D) >= player->dataFloat("maxForwardSpeed", 0) * 0.85f && outdoors && !mInWater &&
        mDotOffCourse > 0.85f) {
        // heading in approximately the right direction (+- 30 deg or so)
        Point3F myDirection2D = mVelocity2D;
        if (len(myDirection2D) < 0.001f) myDirection2D = {0, 1, 0};
        myDirection2D = normalize(myDirection2D);
        const uint32_t mask = TerrainObjectType | InteriorObjectType | WaterObjectType;
        RayInfo ray1Info, ray2Info;
        Point3F startPt = mLocation, endPt = mLocation;
        endPt.z -= 5.0f;
        // not within 5 m of the ground: we can't ski...
        if (!castRay(startPt, endPt, mask, ray1Info, {player->script})) {
            mHeadingDownhill = true;
        } else {
            // if we don't hit anything we're (on the edge of a cliff?) heading downhill
            startPt = add(startPt, myDirection2D);
            endPt = add(endPt, myDirection2D);
            if (!castRay(startPt, endPt, mask, ray2Info, {player->script})) mHeadingDownhill = true;
            else if (ray1Info.point.z > ray2Info.point.z) mHeadingDownhill = true;
        }
    }
    // the target object
    if (auto* object = mTargetObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(mTargetObject)) {
        mObjectLocation = position(*object);
        mDistToObject2D = len(sub(flat(mLocation), flat(mObjectLocation)));
    }
    // someone to shoot at
    auto* engage = mEngageTarget.empty() ? nullptr : EngineObjects::get<GameConnection>(mEngageTarget);
    mTargetPlayer = connectionPlayer(engage);
    if (!mTargetPlayer) mEngageTarget.clear();
    if (mTargetPlayer) {
        mTargLocation = position(*mTargetPlayer);
        mTargVelocity = mTargetPlayer->state.velocity;
        mTargVelocity2D = flat(mTargVelocity);
        mTargRotation = {0, 0, mTargetPlayer->state.yaw};
        mTargEnergy = mTargetPlayer->getEnergyValue();
        mTargDamage = mTargetPlayer->getDamageValue();
        // the target standing still
        if (len(mTargVelocity) > 4.0f) mTargStillTimeMS = 0;
        else if (mTargStillTimeMS == 0) mTargStillTimeMS = now();
        // previous locations, simulating a slower response time (up to 600 ms)
        if (now() - mTargPrevTimeMS > 300) {
            mTargPrevTimeMS = now();
            mTargPrevLocation[3] = mTargPrevLocation[2];
            mTargPrevLocation[2] = mTargPrevLocation[1];
            mTargPrevLocation[1] = mTargPrevLocation[0];
            mTargPrevLocation[0] = worldBoxCenter(mTargetPlayer->script);
        }
        if (isDead(mTargetPlayer)) {
            mTargetPlayer = nullptr;
            mEngageTarget.clear();
        } else {
            mDistToTarg2D = len(sub(flat(mTargLocation), flat(mLocation)));
        }
    }
    mOutdoors = getOutdoorRadius(mLocation) > 0;
    // in water (WaterBlock::isPointSubmergedSimple)
    mInWater = pointInWater(mLocation) || pointInWater(mMuzzlePosition);
    mTargInWater = mTargetPlayer && pointInWater(mTargLocation);
    mObjectInWater = !mTargetObject.empty() && EngineObjects::get<ShapeBase>(mTargetObject) && pointInWater(mObjectLocation);
    // within range of the engage target or the target object
    if (--mCheckTargetLOSCounter <= 0) {
        mCheckTargetLOSCounter = 10;
        mTargetInSight = false;
        auto* object = mTargetObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(mTargetObject);
        if (mTargetPlayer || object) {
            const float rangeDist = mTargetPlayer ? mDistToTarg2D : mDistToObject2D;
            const Point3F rangeLocation = worldBoxCenter(mTargetPlayer ? mTargetPlayer->script : object->script);
            RayInfo rayInfo;
            if (rangeDist <= mRangeToTarget &&
                !castRay(mMuzzlePosition, rangeLocation, TerrainObjectType | InteriorObjectType, rayInfo))
                mTargetInSight = true;
        }
    }
    if (!mTargetPlayer) mWeaponEnergy = 0.0f;
    if (auto* corpse = mCorpse.empty() ? nullptr : EngineObjects::get<PlayerObject>(mCorpse))
        mCorpseLocation = position(*corpse);
}

void AIConnection::updateDetectionTable(PlayerObject* player) {
    // a blinded bot updates nothing
    if (now() < mBlindedTimer) return;
    if (--mPlayerDetectionCounter > 0) return;
    ScriptObject* group = ScriptEngine::instance().findObject("ClientGroup");
    const int count = group ? group->internals["__childCount"].toInt() : 0;
    if (count <= 1) return;
    auto client = [&](int index) -> GameConnection* {
        auto it = group->internals.find("__child" + std::to_string(index));
        return it == group->internals.end() ? nullptr : EngineObjects::get<GameConnection>(it->second.toString());
    };
    if (++mPlayerDetectionIndex >= count) mPlayerDetectionIndex = 0;
    GameConnection* targClient = client(mPlayerDetectionIndex);
    // make sure it's not me...
    if (targClient == this) {
        if (++mPlayerDetectionIndex >= count) mPlayerDetectionIndex = 0;
        targClient = client(mPlayerDetectionIndex);
    }
    if (!targClient || !targClient->script) return;
    const int targClientId = ScriptEngine::instance().objectId(targClient->script);
    PlayerDetectionEntry* targEntry = nullptr;
    for (auto& e : mPlayerDetectionTable)
        if (e.playerId == targClientId) targEntry = &e;
    // (the table never shrinks)
    if (!targEntry) {
        mPlayerDetectionTable.insert(mPlayerDetectionTable.begin(), PlayerDetectionEntry{targClientId});
        targEntry = &mPlayerDetectionTable.front();
    }
    PlayerObject* targPlayer = connectionPlayer(targClient);
    if (!targPlayer) {
        if (targEntry->playerLOS) {
            targEntry->playerLOS = false;
            targEntry->playerLOSTime = now();
        }
        return;
    }
    const Point3F myEyePosition = eyePosition(*player), targEyePosition = eyePosition(*targPlayer);
    Point3F losVector = sub(targEyePosition, myEyePosition);
    const float distToTarg = len(losVector);
    bool clearLOSToTarg;
    if (mSkillLevel >= 1.0f) clearLOSToTarg = true;
    else if (distToTarg < 0.5f) clearLOSToTarg = true;
    else if (targPlayer->cloaked) clearLOSToTarg = false; // cloaked players can't be detected
    else if (distToTarg > 300.0f) clearLOSToTarg = false;
    else {
        // facing the right way?
        const Point3F facingVector = sub(mAimLocation, mLocation);
        const float visibleRange = 0.4f - (0.4f * mSkillLevel);
        if (len(facingVector) < 0.5f) {
            clearLOSToTarg = false;
        } else if (get2DDot(facingVector, losVector) < visibleRange && distToTarg > 1.5f) {
            clearLOSToTarg = false;
        } else {
            if (len(losVector) < 0.001f) losVector = {0, 1, 0};
            losVector = normalize(losVector);
            RayInfo rayInfo;
            clearLOSToTarg = !castRay(myEyePosition, add(myEyePosition, mul(losVector, std::min(300.0f, distToTarg))),
                                      TerrainObjectType | InteriorObjectType, rayInfo);
        }
    }
    if (clearLOSToTarg != targEntry->playerLOS) targEntry->playerLOSTime = now();
    targEntry->playerLOS = clearLOSToTarg;
    if (clearLOSToTarg) targEntry->playerLastPosition = worldBoxCenter(targPlayer->script);
    // the timeslice counter
    mPlayerDetectionCounter = std::max(3, 30 / (count - 1));
}

void AIConnection::setBlinded(int duration) {
    // can't blind the Kidney Bot!!!
    if (mSkillLevel >= 1.0f) return;
    mBlindedTimer = now() + duration;
    for (auto& e : mPlayerDetectionTable)
        if (e.playerLOS) {
            e.playerLOS = false;
            e.playerLOSTime = now();
        }
    // react...
    Point3F dangerLocation = mLocation;
    dangerLocation.x += -2.0f + (randF() * 4.0f);
    dangerLocation.y += -2.0f + (randF() * 4.0f);
    setEvadeLocation(dangerLocation);
    mEvadingCounter = 30;
}

void AIConnection::clientDetected(int targId) {
    if (id() == targId) return;
    ScriptObject* object = ScriptEngine::instance().findObject(std::to_string(targId).c_str());
    auto* targClient = object ? dynamic_cast<GameConnection*>(object->engine.get()) : nullptr;
    if (!targClient) return;
    PlayerDetectionEntry* targEntry = nullptr;
    for (auto& e : mPlayerDetectionTable)
        if (e.playerId == targId) targEntry = &e;
    if (!targEntry) {
        mPlayerDetectionTable.insert(mPlayerDetectionTable.begin(), PlayerDetectionEntry{targId});
        targEntry = &mPlayerDetectionTable.front();
    }
    PlayerObject* targPlayer = connectionPlayer(targClient);
    if (!targPlayer) return;
    if (!targEntry->playerLOS) targEntry->playerLOSTime = now();
    targEntry->playerLOS = true;
    targEntry->playerLastPosition = worldBoxCenter(targPlayer->script);
}

bool AIConnection::hasLOSToClient(int clientId, int& losTime, Point3F& lastLocation) {
    for (auto& e : mPlayerDetectionTable)
        if (e.playerId == clientId) {
            losTime = now() - e.playerLOSTime;
            lastLocation = e.playerLastPosition;
            return e.playerLOS;
        }
    losTime = now();
    lastLocation = {0, 0, 0};
    return false;
}

void AIConnection::scriptProcessEngagement() {
    if (!gScriptEngageSlicer().ready(mPackCheckCounter, 6)) return;
    std::string targetId = "-1", targetType = "none";
    auto* object = mTargetObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(mTargetObject);
    if (mTargetPlayer) {
        targetType = "player";
        targetId = std::to_string(ScriptEngine::instance().objectId(EngineObjects::get<GameConnection>(mEngageTarget)->script));
    } else if (object && mObjectMode == DestroyObject) {
        targetType = "object";
        targetId = std::to_string(ScriptEngine::instance().objectId(object->script));
    }
    std::string projectileId = "-1";
    if (ScriptObject* projectile = mEnemyProjectile.empty() ? nullptr : ScriptEngine::instance().findObject(mEnemyProjectile.c_str()))
        if (mEvadingCounter > 0 && mEvadingCounter < 45) projectileId = std::to_string(ScriptEngine::instance().objectId(projectile));
    callScript("AIProcessEngagement", {std::to_string(id()), targetId, targetType, projectileId});
}

void AIConnection::scriptChooseEngageWeapon(float distToTarg) {
    auto* engage = EngineObjects::get<GameConnection>(mEngageTarget);
    const char* environment = mInWater || mTargInWater ? "water" : (mOutdoors ? "outdoors" : "indoors");
    callScript("AIChooseEngageWeapon", {std::to_string(id()), std::to_string(ScriptEngine::instance().objectId(engage->script)),
                                        fstr(distToTarg), mNavUsingJet ? "false" : "true", environment});
}

void AIConnection::scriptChooseObjectWeapon(float distToTarg) {
    auto* object = EngineObjects::get<ShapeBase>(mTargetObject);
    const char* environment = mInWater || mTargInWater ? "water" : (mOutdoors ? "outdoors" : "indoors");
    callScript("AIChooseObjectWeapon", {std::to_string(id()), std::to_string(ScriptEngine::instance().objectId(object->script)),
                                        fstr(distToTarg), gTargetObjectMode[mObjectMode],
                                        mNavUsingJet ? "false" : "true", environment});
}

void AIConnection::setEvadeLocation(const Point3F& dangerLocation, int durationTicks) {
    Point3F dangerVector = sub(dangerLocation, mLocation);
    Point3F myVector = sub(mMoveLocation, mLocation);
    if (len(dangerVector) < 0.001f) dangerVector = {0, 1, 0};
    dangerVector = normalize(dangerVector);
    if (len(myVector) < 0.001f) myVector = {0, 1, 0};
    Point3F orthDanger = cross(dangerVector, {0, 0, 1});
    if (len(orthDanger) < 0.001f) orthDanger = {0, 1, 0};
    orthDanger = normalize(orthDanger);
    if (std::isnan(orthDanger.x) || std::isnan(orthDanger.y)) {
        dangerVector = sub(mTargLocation, mLocation);
        if (len(dangerVector) < 0.001f) dangerVector = {0, 1, 0};
        orthDanger = normalize(cross(normalize(dangerVector), {0, 0, 1}));
        if (std::isnan(orthDanger.x) || std::isnan(orthDanger.y)) orthDanger = {0, 1, 0};
    }
    // evade to the side nearer our heading (the shipped arithmetic: 256
    // as the full circle, and angleDiff2 only set in one branch)
    const float myAngle = get2DAngle(add(mLocation, mVelocity2D), mLocation);
    const float tempAngle1 = get2DAngle(mLocation, add(mTargLocation, orthDanger));
    float tempAngle2 = get2DAngle(mLocation, sub(mTargLocation, orthDanger));
    const float angleDiff1 = myAngle < tempAngle1 ? std::min(tempAngle1 - myAngle, 256 + myAngle - tempAngle1)
                                                  : std::min(myAngle - tempAngle1, 256 + tempAngle1 - myAngle);
    float angleDiff2 = 0;
    if (myAngle < tempAngle2) angleDiff2 = std::min(tempAngle2 - myAngle, 256 + myAngle - tempAngle2);
    else tempAngle2 = std::min(myAngle - tempAngle2, 256 + tempAngle2 - myAngle);
    mEvadeLocation = angleDiff1 < angleDiff2 ? add(mLocation, mul(orthDanger, 30.0f)) : sub(mLocation, mul(orthDanger, 30.0f));
    if (durationTicks > 0) mEvadingCounter = durationTicks;
}

void AIConnection::processEngagement(PlayerObject* player) {
    updateDetectionTable(player);
    scriptProcessEngagement();
    // a weapon change cancels the scripted sustained fire
    const int image = player->images[0].dataBlock ? player->images[0].dataBlock->id : -1;
    if (image != mMountedImageId) mScriptTriggerCounter = -1;
    mMountedImageId = image;
    if (mScriptTriggerCounter-- >= 0) pressFire(true);
    auto* object = mTargetObject.empty() ? nullptr : EngineObjects::get<ShapeBase>(mTargetObject);
    if (!mTargetPlayer && !object) return;
    pressFire(false);
    mLookAtTargetTimeMS = now() + 1500;
    auto* engage = EngineObjects::get<GameConnection>(mEngageTarget);
    const int engageId = engage && engage->script ? ScriptEngine::instance().objectId(engage->script) : -1;
    bool engagingPlayer;
    Point3F targetLocation;
    float distToTarg;
    if (mTargetPlayer) {
        engagingPlayer = true;
        if (now() < mBlindedTimer) {
            int dummyTime;
            hasLOSToClient(engageId, dummyTime, targetLocation);
        } else {
            int losTime;
            Point3F losLocation;
            const bool hasLOS = hasLOSToClient(engageId, losTime, losLocation);
            // lost sight recently: where we last saw them; else a location
            // from the past per skill (a slower response)
            if (!hasLOS && losTime < 2500) targetLocation = losLocation;
            else if (mSkillLevel >= 0.9f) targetLocation = mTargPrevLocation[0];
            else if (mSkillLevel >= 0.7f) targetLocation = mTargPrevLocation[1];
            else if (mSkillLevel >= 0.3f) targetLocation = mTargPrevLocation[2];
            else targetLocation = mTargPrevLocation[3];
        }
        distToTarg = len(sub(flat(targetLocation), flat(mLocation)));
    } else {
        engagingPlayer = false;
        targetLocation = object->getAIRepairPoint();
        if (same(targetLocation, {0, 0, 0})) targetLocation = worldBoxCenter(object->script);
        distToTarg = mDistToObject2D;
    }
    //detect the projectiles from the target
    mProjectileCounter--;
    if (engagingPlayer) {
        const std::string incoming = engage && engage->script ? Fields::string(engage->script, "projectile") : std::string();
        ScriptObject* projectileObject = incoming.empty() ? nullptr : ScriptEngine::instance().findObject(incoming.c_str());
        auto* projectile = projectileObject ? dynamic_cast<ProjectileObject*>(projectileObject->engine.get()) : nullptr;
        const std::string projectileKey = projectile ? ScriptEngine::instance().objectKey(projectileObject) : std::string();
        //see if it's a new threat, or time to re-evaluate the current one
        if (projectile && (projectileKey != mEnemyProjectile || mProjectileCounter <= 0)) {
            //reset the projectile counter
            mProjectileCounter = 15;
            float timeToImpact;
            ScriptObject* projData = ScriptEngine::instance().findObject(projectile->dataBlock().c_str());
            if (projData && projectile->calculateImpact(4.0f, mImpactLocation, timeToImpact)) {
                //see if the impact location is within range
                const Point3F predictMyLocation = add(mLocation, mul(mVelocity, timeToImpact));
                const float distToDanger = len(sub(predictMyLocation, mImpactLocation));
                if (distToDanger < std::max(Fields::f32(projData, "damageRadius", 0.0f), 2.0f)) {
                    setEvadeLocation(mImpactLocation);
                    mEnemyProjectile = projectileKey;
                    //set the evade counter - any value above 45 is simulated response time
                    const int responseTime = (int)(30 * (1.0f - mSkillLevel));
                    mEvadingCounter = 45 + responseTime;
                }
            }
        }
    }
    Point3F dummyPoint;
    if (mNavUsingJet && mJetting.shouldAimAt(dummyPoint)) return;
    if (engage) {
        int detectLOSTime;
        Point3F lastLOSLocation;
        if (!hasLOSToClient(engageId, detectLOSTime, lastLOSLocation) && detectLOSTime > mDetectHiddenPeriod) return;
    }
    ScriptObject* projectile = projectileData();
    const bool ballistic = projectile && Fields::boolean(projectile, "isBallistic", false);
    auto aimAtTarget = [&] {
        if (!mTargetInRange) setAimLocation(targetLocation);
        else setAimLocation({targetLocation.x, targetLocation.y, mAimLocation.z});
    };
    switch (mEngageState) {
    case ChooseWeapon: {
        aimAtTarget();
        mFiring = false;
        // a lower skill switches weapons less often
        if (--mChangeWeaponCounter > 0) {
            mEngageState = ReloadWeapon;
            mStateCounter = 45;
            mDelayCounter = (int)(20.0f * (1.0f - mSkillLevel));
        } else {
            mChangeWeaponCounter = (int)((1.0f - mSkillLevel) / 0.10f);
        }
        const std::string prevWeapon = mProjectileName;
        if (engagingPlayer) scriptChooseEngageWeapon(distToTarg);
        else scriptChooseObjectWeapon(distToTarg);
        if (distToTarg > mEngageMaxDistance) {
            mEngageState = OutOfRange;
            mStateCounter = 30;
        } else {
            mEngageState = ReloadWeapon;
            mStateCounter = 60;
            mDelayCounter = (int)(20.0f * (1.0f - mSkillLevel));
            if (strcasecmp(mProjectileName.c_str(), prevWeapon.c_str()) != 0) mDelayCounter *= 3;
        }
        break;
    }
    case OutOfRange:
        aimAtTarget();
        if (distToTarg <= mEngageMaxDistance || --mStateCounter <= 0) mEngageState = ChooseWeapon;
        break;
    case ReloadWeapon: {
        aimAtTarget();
        if (--mStateCounter <= 0 || (mWeaponEnergy > 0 && mNavUsingJet)) {
            mEngageState = ChooseWeapon;
            break;
        }
        const bool weaponReady = player->isImageReady(0);
        const bool hasEnergy = mEnergy >= mWeaponEnergy;
        const float skillVelocityFactor = 6.0f + (mSkillLevel * mSkillLevel * mSkillLevel * 300.0f);
        const bool mustSlowDown = len(mVelocity) > skillVelocityFactor;
        if (weaponReady && hasEnergy && !mustSlowDown) {
            mStateCounter = 15;
            if (--mDelayCounter <= 0) mEngageState = FindTargetPoint;
        }
        if (mFiring && mTriggerCounter > 0) {
            mTriggerCounter--;
            pressFire();
        }
        break;
    }
    case FindTargetPoint: {
        aimAtTarget();
        if (--mStateCounter <= 0 || (mWeaponEnergy > 0 && mNavUsingJet)) {
            mEngageState = ChooseWeapon;
            break;
        }
        //should we go for center mass, or splash damage, or are we shooting at a lazed target
        Point3F aimAtTargetPoint{0, 0, 0};
        mAimAtLazedTarget = false;
        //only shoot at lazed targets if we're using a ballistic weapon
        if (ballistic) {
            // Sim::getServerTargetSet: the beacons (GameBase::setBeacon from
            // the datablock's beacon flag) and their GameBase::getTarget.
            for (auto& [key, object] : ScriptEngine::instance().objects) {
                auto* target = object ? dynamic_cast<GameBase*>(object->engine.get()) : nullptr;
                if (!target || !target->dataBool("beacon", false)) continue;
                Point3F targetPoint;
                if (auto* laser = dynamic_cast<TargetProjectileObject*>(target)) {
                    if (!laser->truncated) continue;
                    targetPoint = laser->endPoint;
                } else {
                    if (target->targetId < 0) continue;
                    targetPoint = {target->transform[3], target->transform[7], target->transform[11]};
                }
                //see if the target is within 20m of what we're trying to shoot at...
                if (len(sub(targetPoint, targetLocation)) < 10.0f) {
                    aimAtTargetPoint = targetPoint;
                    mAimAtLazedTarget = true;
                }
            }
        }
        const bool splash = projectile && Fields::boolean(projectile, "hasDamageRadius", false) &&
                            Fields::f32(projectile, "damageRadius", 0) > 5.0f;
        if (!mAimAtLazedTarget) aimAtTargetPoint = engagingPlayer && splash ? mTargLocation : targetLocation;
        if (projectile) {
            Point3F aimVectorMin, aimVectorMax;
            float timeMin, timeMax;
            bool canShoot = false;
            if (engagingPlayer)
                canShoot = ProjectileAim::calculateAim(projectile, aimAtTargetPoint, mTargVelocity, mMuzzlePosition,
                                                       mVelocity, &aimVectorMin, &timeMin, &aimVectorMax, &timeMax);
            else if (mObjectMode != MissileVehicle ||
                     (object && player->lockedTargetId() == ScriptEngine::instance().objectId(object->script)))
                canShoot = ProjectileAim::calculateAim(projectile, aimAtTargetPoint, {0, 0, 0}, mMuzzlePosition,
                                                       mVelocity, &aimVectorMin, &timeMin, &aimVectorMax, &timeMax);
            if (canShoot) {
                Point3F aimLocation = add(mMuzzlePosition, mul(aimVectorMin, len(sub(aimAtTargetPoint, mMuzzlePosition))));
                const bool needToDope = len(mVelocity) > 4.0f || engagingPlayer || (ballistic && !mAimAtLazedTarget);
                if (needToDope) aimLocation = dopeAimLocation(mMuzzlePosition, aimLocation);
                setAimLocation(aimLocation);
                mEngageState = AimAtTarget;
                mStateCounter = 15;
                processEngagement(player);
            } else {
                mTargetInRange = false;
                if (mFiring && mTriggerCounter > 0) {
                    mTriggerCounter--;
                    pressFire();
                }
            }
        } else {
            mEngageState = ChooseWeapon;
            mTargetInRange = false;
        }
        break;
    }
    case AimAtTarget: {
        if (mWeaponEnergy > 0 && mNavUsingJet) {
            mEngageState = ChooseWeapon;
            break;
        }
        // lead our own motion by a frame
        if (engagingPlayer) setAimLocation(sub(mAimLocation, mul(mVelocity, 1.0f / 30.0f)));
        bool clearLOSToTarget = false;
        RayInfo rayInfo;
        const Point3F startPt = add(mMuzzlePosition, mul(mVelocity, 1.0f / 30.0f));
        uint32_t mask = TerrainObjectType | InteriorObjectType | PlayerObjectType | ForceFieldObjectType;
        if (!player->mount.empty()) mask |= VehicleObjectType;
        if (!castRay(startPt, mAimLocation, mask, rayInfo, {player->script})) clearLOSToTarget = true;
        else if (engagingPlayer && rayInfo.object && mTargetPlayer && rayInfo.object == mTargetPlayer->script)
            clearLOSToTarget = true;
        else if (!engagingPlayer && rayInfo.object && object && rayInfo.object == object->script)
            clearLOSToTarget = true;
        bool readyToFire = false;
        if (clearLOSToTarget || (ballistic && mSkillLevel >= 0.7f)) readyToFire = true;
        if (readyToFire) {
            mTargetInRange = true;
            mEngageState = FireWeapon;
        } else {
            mTargetInRange = false;
            mEngageState = FindTargetPoint;
        }
        if (mFiring && mTriggerCounter > 0) {
            mTriggerCounter--;
            pressFire();
        }
        break;
    }
    case FireWeapon:
        if (mWeaponEnergy > 0 && mNavUsingJet) {
            mEngageState = ChooseWeapon;
            break;
        }
        mTargetInRange = true;
        pressFire();
        mFiring = true;
        mTriggerCounter--;
        if (mTriggerCounter > 0) {
            mEngageState = FindTargetPoint;
            mStateCounter = 15;
        } else {
            mEngageState = ChooseWeapon;
            mStateCounter = 60;
        }
        break;
    }
}

Point3F AIConnection::correctHeading() {
    Point3F newLocation = mNodeLocation;
    if (len(mVelocity2D) > 4) {
        const float heading = std::atan2(mVelocity2D.x, mVelocity2D.y) - (Pi / 2.0f);
        if (mDotOffCourse <= 0) {
            // heading the wrong way: aim the opposite way
            const float newHeading = heading + Pi;
            newLocation.x = mLocation.x + mDistToNode2D * std::cos(newHeading);
            newLocation.y = mLocation.y - mDistToNode2D * std::sin(newHeading);
        } else {
            const float headingDiff = std::acos(mDotOffCourse);
            const float overCompFactor = mDotOffCourse > 0.85f ? 3.0f : 2.0f;
            const float angle1 = heading + headingDiff, angle2 = heading - headingDiff;
            const Point3F newVec1{std::cos(angle1), -std::sin(angle1), 0}, newVec2{std::cos(angle2), -std::sin(angle2), 0};
            const float angle1Dot = get2DDot(sub(mNodeLocation, mLocation), newVec1);
            const float angle2Dot = get2DDot(sub(mNodeLocation, mLocation), newVec2);
            const float newAngle = angle1Dot > angle2Dot ? heading + overCompFactor * headingDiff
                                                         : heading - overCompFactor * headingDiff;
            newLocation.x = mLocation.x + mDistToNode2D * std::cos(newAngle);
            newLocation.y = mLocation.y - mDistToNode2D * std::sin(newAngle);
        }
    }
    return newLocation;
}

Point3F AIConnection::avoidPlayers(PlayerObject* player, const Point3F& desiredDestination, bool destIsFinal) {
    const Point3F qmin = sub(mLocation, {2.0f, 2.0f, 0.5f}), qmax = add(mLocation, {2.0f, 2.0f, 2.5f});
    ShapeBase* closestObject = nullptr;
    float closestDist = 32767;
    Point3F closestLocation{0, 0, 0};
    const auto result = findObjects(qmin, qmax, ShapeBaseObjectType | StaticTSObjectType);
    if (result.size() > 1) {
        for (ScriptObject* neighbor : result) {
            auto* neighborObject = neighbor ? dynamic_cast<ShapeBase*>(neighbor->engine.get()) : nullptr;
            if (!neighborObject || neighborObject == player) continue;
            const uint32_t type = typeMask(neighbor);
            // only what the datablock says to avoid (and every TSStatic)
            if (!(type & StaticTSObjectType) && !neighborObject->dataBool("aiAvoidThis", false)) continue;
            if (type & CorpseObjectType) continue;
            Point3F tempVector = sub(worldBoxCenter(neighbor), worldBoxCenter(player->script));
            if (len(tempVector) > 1.0f) {
                tempVector = normalize(tempVector);
                RayInfo rayInfo;
                const Point3F startPt = worldBoxCenter(player->script);
                const bool losResult = castRay(startPt, add(startPt, mul(tempVector, 2.0f)), type, rayInfo, {player->script});
                if (!losResult || rayInfo.object != neighbor) continue;
            }
            const Point3F tempLocation = position(*neighborObject);
            const float tempDist = len(sub(tempLocation, mLocation));
            if (tempDist < closestDist) {
                closestDist = tempDist;
                closestObject = neighborObject;
                closestLocation = tempLocation;
            }
        }
    }
    if (!closestObject) {
        mAvoidingObject.clear();
        return desiredDestination;
    }
    // bumping into a player detects him
    if (auto* closestPlayer = dynamic_cast<PlayerObject*>(closestObject))
        if (auto* closestClient = EngineObjects::get<GameConnection>(closestPlayer->controllingClient))
            clientDetected(ScriptEngine::instance().objectId(closestClient->script));
    Point3F newLocation;
    float newHeading;
    const float distToDest2D = len(sub(flat(desiredDestination), flat(mLocation)));
    Point3F directionVector = flat(sub(desiredDestination, mLocation));
    Point3F neighborVector = flat(sub(closestLocation, mLocation));
    if (len(directionVector) < 0.001f) directionVector = {0, 1, 0};
    directionVector = normalize(directionVector);
    if (std::isnan(directionVector.x) || std::isnan(directionVector.y)) directionVector = {0, 1, 0};
    if (len(neighborVector) < 0.001f) neighborVector = {0, 1, 0};
    neighborVector = normalize(neighborVector);
    if (std::isnan(neighborVector.x) || std::isnan(neighborVector.y)) neighborVector = {0, 1, 0};
    const float neighborDot = get2DDot(neighborVector, directionVector);
    const std::string closestKey = ScriptEngine::instance().objectKey(closestObject->script);
    if (!destIsFinal && neighborDot < 0.5f) {
        // the neighbor isn't in our way
        mAvoidingObject.clear();
        return desiredDestination;
    }
    if (destIsFinal) {
        // stopped: step away from the neighbor
        const float neighborHeading = std::atan2(neighborVector.x, neighborVector.y) - (Pi / 2.0f);
        const float angle1 = neighborHeading + ((Pi / 2.0f) + (Pi / 4.0f));
        const float angle2 = neighborHeading - ((Pi / 2.0f) + (Pi / 4.0f));
        const Point3F newVec1{std::cos(angle1), -std::sin(angle1), 0}, newVec2{std::cos(angle2), -std::sin(angle2), 0};
        newHeading = get2DDot(neighborVector, newVec1) < get2DDot(neighborVector, newVec2) ? angle1 : angle2;
        newLocation.x = mLocation.x + std::max(6.0f, distToDest2D) * std::cos(newHeading);
        newLocation.y = mLocation.y - std::max(6.0f, distToDest2D) * std::sin(newHeading);
        newLocation.z = desiredDestination.z;
        mAvoidingObject.clear();
    } else if (mAvoidingObject != closestKey || !same(desiredDestination, mAvoidDestinationPoint)) {
        // a new obstacle: go around it on the side nearer our direction
        const float neighborHeading = std::atan2(neighborVector.x, neighborVector.y) - (Pi / 2.0f);
        const float angle1 = neighborHeading + (Pi / 2.0f), angle2 = neighborHeading - (Pi / 2.0f);
        const Point3F newVec1{std::cos(angle1), -std::sin(angle1), 0}, newVec2{std::cos(angle2), -std::sin(angle2), 0};
        newHeading = get2DDot(directionVector, newVec1) > get2DDot(directionVector, newVec2) ? angle1 : angle2;
        newLocation.x = mLocation.x + std::max(6.0f, distToDest2D) * std::cos(newHeading);
        newLocation.y = mLocation.y - std::max(6.0f, distToDest2D) * std::sin(newHeading);
        newLocation.z = desiredDestination.z;
        mAvoidingObject = closestKey;
        mAvoidSourcePoint = mLocation;
        mAvoidDestinationPoint = desiredDestination;
        mAvoidMovePoint = newLocation;
        mAvoidForcedPath = false;
    } else if (len(sub(mAvoidMovePoint, mLocation)) < 1.5f) {
        if (!mAvoidForcedPath) {
            mPath.forceSearch();
            mAvoidForcedPath = true;
        }
        newLocation = desiredDestination;
    } else {
        newLocation = mAvoidMovePoint;
    }
    return newLocation;
}

void AIConnection::processVehicleMovement(PlayerObject* player) {
    if (mNavUsingJet) {
        mNavUsingJet = false;
        mJetting.reset();
        mPath.forceSearch();
    }
    // mounted: look out the vehicle's front
    if (auto* vehicle = player->mount.empty() ? nullptr : EngineObjects::get<ShapeBase>(player->mount)) {
        const auto vehicleMat = vehicle->getMountTransform(player->mountNode);
        const float vehicleRot = std::atan2(vehicleMat[1], vehicleMat[5]) - (Pi / 2.0f);
        const Point3F aimVector{std::cos(vehicleRot), -std::sin(vehicleRot), 0};
        Point3F aimLocation = add(mLocation, mul(aimVector, 30.0f));
        aimLocation.z += 2.0f;
        setScriptAimLocation(aimLocation, 1000);
    }
    callScript("AIProcessVehicle", {std::to_string(id())});
}

void AIConnection::processPilotVehicle(VehicleObject*) {
    if (mNavUsingJet) {
        mNavUsingJet = false;
        mJetting.reset();
        mPath.forceSearch();
    }
    callScript("AIPilotVehicle", {std::to_string(id())});
}

void AIConnection::processMovement(PlayerObject* player) {
    setPathCapabilities(player);
    mEvadingCounter--;
    if (mEvadingCounter > 0 && mEvadingCounter < 45) {
        mJetting.reset();
        mNavUsingJet = false;
        mIsEvading = true;
        setMoveLocation({mEvadeLocation.x, mEvadeLocation.y, 0});
        setMoveSpeed(1.0f);
        if (player->canJump()) pressJump();
        else pressJet();
    } else {
        auto* ts = ScriptEngine::instance().ts();
        const int focusOnBot = ts ? ts->getGlobal("$AIFocusOnBot").toInt() : 0;
        if (focusOnBot) {
            if (focusOnBot != id()) return;
            const float e = mPath.jetWillNeedEnergy(12.0);
            if (e > 0.0) Console::instance().printf(LogLevel::Info, "%d wants %f energy", focusOnBot, e);
        }
        if (mIsEvading) {
            mIsEvading = false;
            mNavUsingJet = false;
            mPath.forceSearch();
        }
        mPath.setDestMounted(false);
        if (!mNavUsingJet) mMoveMode = mMoveModePending;
        switch (mMoveMode) {
        case ModeStop: {
            mPath.updateLocations(mLocation, mMoveDestination);
            mNodeLocation = mPath.getSeekLoc(mVelocity);
            mNavUsingJet = false;
            const Point3F moveLocation = avoidPlayers(player, mLocation, true);
            if (!same(moveLocation, mLocation)) {
                setMoveSpeed(0.6f);
                setMoveLocation(moveLocation);
            } else {
                setMoveSpeed(0.0f);
            }
            break;
        }
        case ModeGainHeight:
            mNavUsingJet = false;
            if (player->canJump()) pressJump();
            else pressJet();
            setMoveLocation(mLocation);
            break;
        case ModeMountVehicle:
            mPath.setDestMounted(true);
            if (!player->mount.empty()) {
                setMoveSpeed(0.0f);
                break;
            }
            [[fallthrough]];
        case ModeWalk:
        case ModeExpress: {
            mPath.updateLocations(mLocation, mMoveDestination);
            mNodeLocation = mPath.getSeekLoc(mVelocity);
            if (!same(mNodeLocation, mPrevNodeLocation)) {
                mPrevNodeLocation = mNodeLocation;
                mInitialLocation = mLocation;
            } else if (!mNavUsingJet) {
                // passed the node without reaching it: search again
                Point3F vec1 = sub(mInitialLocation, mNodeLocation), vec2 = sub(mLocation, mNodeLocation);
                vec2.z = vec1.z = 0;
                if (dot(vec1, vec1) > 1.0f && dot(vec2, vec2) > 1.0f && dot(vec1, vec2) < 0.0f) {
                    mPrevNodeLocation = {0, 0, 0};
                    mPath.forceSearch();
                }
            }
            if (mPath.userMustJet() && !mInWater) {
                mNavUsingJet = true;
                if (mJetting.status() == AIJetWorking) {
                    mJetting.process(this, player);
                    if (mJetting.status() != AIJetWorking) {
                        if (mJetting.status() == AIJetSuccess) {
                            mPath.informJetDone();
                        } else {
                            if (mMoveMode == ModeMountVehicle) pressJump();
                            mPath.forceSearch();
                        }
                        mJetting.reset();
                    } else if (mJetting.badTimeToSearch()) {
                        mPath.informJetBusy();
                    }
                } else {
                    mJetting.init(mNodeLocation, mPath.intoMount(), mPath.getJetInfo());
                }
            } else {
                if (mNavUsingJet) {
                    mPath.forceSearch();
                    mNavUsingJet = false;
                }
                // falling fast: jet to cushion it
                if (mVelocity.z < -20.0f) pressJet();
                const bool pathIsCurrent = mPath.isPathCurrent();
                float distToEnd = pathIsCurrent ? mPath.distRemaining(2 * mMoveTolerance) : 2 * mMoveTolerance;
                const float tolerance = distToEnd > mMoveTolerance ? 0.25f : mMoveTolerance;
                const float jetEnergy = mPath.jetWillNeedEnergy(30);
                Point3F moveLocation = mNodeLocation;
                if (mDistToNode2D > 0.25f) moveLocation = correctHeading();
                const float distToMove2D = len(sub(flat(mLocation), flat(moveLocation)));
                const float distToEnd2D = len(sub(flat(mLocation), flat(mMoveDestination)));
                distToEnd = std::max(distToEnd, distToEnd2D);
                if (mMoveMode != ModeMountVehicle || distToEnd > 3.0f)
                    moveLocation = avoidPlayers(player, moveLocation, false);
                setMoveLocation(moveLocation);
                // stuck: two seconds within a metre of the same place
                if (distToEnd > tolerance && distToEnd > 1.0f) {
                    const Point3F checkStuck2D = flat(sub(mLocation, mStuckLocation));
                    if (len(sub(mNodeLocation, mStuckDestination)) > 1.0f || len(checkStuck2D) > 1.0f) {
                        mStuckLocation = mLocation;
                        mStuckDestination = mNodeLocation;
                        mStuckTimer = now();
                    } else if (now() - mStuckTimer > 2000) {
                        setMoveMode(ModeStuck);
                        mStuckInitialized = false;
                        mStuckTryJump = true;
                        mStuckJumpInitialized = false;
                        return;
                    } else {
                        mPath.informProgress(mVelocity);
                    }
                }
                Point3F nextNodeLocation;
                bool isCollinearPath = false;
                if (mPath.getPathNodeLoc(1, nextNodeLocation) &&
                    get2DDot(sub(moveLocation, mLocation), sub(nextNodeLocation, moveLocation)) > 0.9f)
                    isCollinearPath = true;
                if (distToMove2D > 30.0f) {
                    setMoveSpeed(1.0f);
                    if (mHeadingDownhill) {
                        if (player->canJump() && mSkillLevel >= 0.25f) pressJump();
                    } else if (mEnergyAvailable && mEnergy > jetEnergy && mMoveMode != ModeWalk) {
                        if (player->canJump()) pressJump();
                        else pressJet();
                    }
                } else if (distToEnd > 10.0f && isCollinearPath) {
                    setMoveSpeed(1.0f);
                    if (mEnergyAvailable && mEnergy > jetEnergy && mMoveMode != ModeWalk) pressJet();
                } else if (distToMove2D > std::max(4.0f, tolerance)) {
                    setMoveSpeed(1.0f);
                } else if (distToMove2D > tolerance) {
                    setMoveSpeed(0.4f);
                } else if (distToEnd < tolerance) {
                    setMoveMode(ModeStop);
                }
            }
            break;
        }
        case ModeStuck:
            if (mStuckTryJump) {
                // first try jumping out
                if (!mStuckJumpInitialized) {
                    mStuckJumpInitialized = true;
                    mStuckJumpTimer = now();
                    setMoveSpeed(0.0f);
                    setMoveLocation(mLocation);
                } else if (now() - mStuckJumpTimer < 500) {
                    if (mEnergy < 0.25f) mStuckJumpTimer = now();
                } else if (now() - mStuckJumpTimer < 1000) {
                    pressJump();
                    pressJet();
                } else if (now() - mStuckJumpTimer < 2000) {
                    setMoveLocation(mStuckDestination);
                    setMoveSpeed(1.0f);
                    pressJet();
                } else {
                    mStuckTryJump = false;
                    if (len(sub(mLocation, mStuckLocation)) > 1.0f) {
                        mPath.forceSearch();
                        setMoveSpeed(1.0f);
                        setMoveMode(ModeExpress, true);
                        mStuckLocation = {0, 0, 0};
                        return;
                    }
                }
            } else {
                // then walk off at an angle to the way we were going
                if (!mStuckInitialized) {
                    mStuckInitialized = true;
                    Point3F newLocation = mLocation;
                    float foundLength = 32767;
                    const Point3F curHeading = sub(mNodeLocation, mLocation);
                    const float curAngle = std::atan2(curHeading.x, curHeading.y) - (Pi / 2.0f);
                    for (float i = 0.0f; i < 5.0f; i += 1.0f) {
                        const float testAngle = curAngle + ((i + 2.0f) * Pi / 4.0f);
                        const Point3F newVec{std::cos(testAngle), -std::sin(testAngle), 0.0f};
                        RayInfo rayInfo;
                        Point3F startPt = mLocation, endPt = add(mLocation, mul(newVec, 4.0f));
                        startPt.z += 0.3f;
                        endPt.z += 0.3f;
                        if (!castRay(startPt, endPt, TerrainObjectType | InteriorObjectType, rayInfo)) {
                            newLocation = endPt;
                            break;
                        }
                        if (len(sub(rayInfo.point, startPt)) < foundLength) {
                            foundLength = len(sub(rayInfo.point, startPt));
                            newLocation = endPt;
                        }
                    }
                    mStuckTimer = now();
                    setMoveLocation(newLocation);
                    setMoveSpeed(1.0f);
                }
                if (len(sub(mLocation, mMoveLocation)) < 1.0f || now() - mStuckTimer > 2000) {
                    mStuckLocation = {0, 0, 0};
                    mPath.informStuck(mStuckLocation, mStuckDestination);
                    setMoveMode(ModeExpress, true);
                }
            }
            break;
        }
    }
    // the aim: the jet's, else 10 m along the path (nothing to shoot at)
    Point3F jetAimLocation;
    if (mNavUsingJet && mJetting.shouldAimAt(jetAimLocation)) {
        setAimLocation(jetAimLocation);
    } else if (mMoveMode != ModeStop && mPath.isPathCurrent() && !mTargetPlayer &&
               !EngineObjects::get<ShapeBase>(mTargetObject) && now() > mLookAtTargetTimeMS) {
        const float distToEnd = mPath.distRemaining(10);
        Point3F aimPoint = distToEnd >= 10 ? mPath.getLocOnPath(10) : mNodeLocation;
        const Point3F directionVector = flat(sub(aimPoint, mLocation));
        const float directionLen = len(directionVector);
        if (directionLen < 0.001f) {
            aimPoint.x = mLocation.x + 30.0f * std::cos(mRotation.z);
            aimPoint.y = mLocation.y - 30.0f * std::sin(mRotation.z);
        } else {
            aimPoint.x = mLocation.x + 30.0f * (directionVector.x / directionLen);
            aimPoint.y = mLocation.y + 30.0f * (directionVector.y / directionLen);
        }
        aimPoint.z = mMuzzlePosition.z;
        setAimLocation(aimPoint);
    }
}

// Move::clamp's packed form, with the floats kept (an AI move is exact).
ClientMoveIn AIConnection::packMove() const {
    ClientMoveIn move;
    auto angle = [](float a) { return (int16_t)(uint16_t)((uint32_t)(int64_t)((a / TwoPi) * 0x10000) & 0xFFFF); };
    auto axis = [](float v) { return v < -1 ? 0 : v > 1 ? 32 : (int)((v + 1) * 16); };
    move.yaw = angle(mMoveYaw);
    move.pitch = angle(mMovePitch);
    move.x = axis(mMoveX);
    move.y = axis(mMoveY);
    move.z = axis(0);
    move.exact = true;
    move.fx = mMoveX;
    move.fy = mMoveY;
    move.fyaw = mMoveYaw;
    move.fpitch = mMovePitch;
    for (int i = 0; i < 6; ++i) move.trigger[i] = mTriggers[i];
    return move;
}

// The move's x/y from the move location, in the control object's frame
// (moveMatrix: the yaw to aim with).
static void moveToward(const Point3F& cur, const Point3F& target, float speed, float yaw, float& mx, float& my,
                       float threshold) {
    const float xDiff = target.x - cur.x, yDiff = target.y - cur.y;
    float x = 0, y = 0;
    if (std::fabs(xDiff) <= threshold) y = cur.y > target.y ? -speed : speed;
    else if (std::fabs(yDiff) <= threshold) x = cur.x > target.x ? -speed : speed;
    else if (std::fabs(xDiff) > std::fabs(yDiff)) {
        const float value = std::fabs(yDiff / xDiff) * speed;
        y = cur.y > target.y ? -value : value;
        x = cur.x > target.x ? -speed : speed;
    } else {
        const float value = std::fabs(xDiff / yDiff) * speed;
        x = cur.x > target.x ? -value : value;
        y = cur.y > target.y ? -speed : speed;
    }
    // world -> the rotated frame: MatrixF::set(EulerF(0, 0, yaw)) is rows
    // (cz sz; -sz cz), and the move goes through its transpose.
    const float cz = std::cos(yaw), sz = std::sin(yaw);
    mx = cz * x - sz * y;
    my = sz * x + cz * y;
}

// AIConnection::getMoveList.
ClientMoveIn AIConnection::getMove() {
    mMoveYaw = mMovePitch = mMoveX = mMoveY = 0;
    ClientMoveIn nullMove;
    if (!gAISystemEnabled()) {
        for (bool& t : mTriggers) t = false;
        return nullMove;
    }
    auto* ctrlObject = EngineObjects::get<ShapeBase>(controlObject());
    if (!ctrlObject) return nullMove;
    if (auto* myPlayer = dynamic_cast<PlayerObject*>(ctrlObject)) {
        // no script functions for the dead
        if (isDead(myPlayer)) return nullMove;
        process(myPlayer);
        // the mounted vehicle's rotation about z
        float vehicleRot = 0;
        if (auto* mount = myPlayer->mount.empty() ? nullptr : EngineObjects::get<ShapeBase>(myPlayer->mount)) {
            const auto vehicleMat = mount->getMountTransform(myPlayer->mountNode);
            vehicleRot = -std::atan2(-vehicleMat[1], vehicleMat[5]);
        }
        const Point3F curLocation = position(*myPlayer);
        float curYaw = myPlayer->state.yaw + vehicleRot;
        const float curPitch = myPlayer->state.headPitch;
        const float xDiff = mAimLocation.x - curLocation.x, yDiff = mAimLocation.y - curLocation.y;
        float moveYaw = curYaw;
        // first do Yaw
        if (!isZeroF(xDiff) || !isZeroF(yDiff)) {
            while (curYaw > TwoPi) curYaw -= TwoPi;
            while (curYaw < -TwoPi) curYaw += TwoPi;
            const float newYaw = std::atan2(xDiff, yDiff);
            float yawDiff = newYaw - curYaw;
            if (yawDiff < 0.0f) yawDiff += TwoPi;
            else if (yawDiff >= TwoPi) yawDiff -= TwoPi;
            if (yawDiff > Pi) yawDiff -= TwoPi;
            else if (yawDiff < -Pi) yawDiff += TwoPi;
            mMoveYaw = yawDiff;
            moveYaw = newYaw;
        }
        // next do pitch
        const float horzDist = std::hypot(mAimLocation.x - mEyePosition.x, mAimLocation.y - mEyePosition.y);
        if (!isZeroF(horzDist)) {
            const float vertDist = mAimLocation.z - mEyePosition.z;
            mMovePitch = std::atan2(horzDist, vertDist) - (Pi / 2.0f) - curPitch;
        }
        // finally, the move itself
        const float mxDiff = mMoveLocation.x - curLocation.x, myDiff = mMoveLocation.y - curLocation.y;
        if ((std::fabs(mxDiff) > 0 || std::fabs(myDiff) > 0) && !isZeroF(mMoveSpeed))
            moveToward(curLocation, mMoveLocation, mMoveSpeed, moveYaw, mMoveX, mMoveY, EqualConst);
    } else if (auto* myVehicle = dynamic_cast<VehicleObject*>(ctrlObject)) {
        process(myVehicle);
        const Point3F curLocation = position(*myVehicle);
        // extractRotation's z: atan2(-m[0][1], m[1][1])
        float curYaw = std::atan2(-myVehicle->transform[1], myVehicle->transform[5]);
        float xDiff = mPilotAimLocation.x - curLocation.x, yDiff = mPilotAimLocation.y - curLocation.y;
        float yawDiff = 0;
        const Point3F velocity = myVehicle->getVelocity();
        const Point3F velocity2D = flat(velocity);
        const float velocityDot = get2DDot(velocity2D, sub(mPilotDestination, curLocation));
        float moveYaw = curYaw;
        if (std::fabs(xDiff) > 5.0f || std::fabs(yDiff) > 5.0f) {
            while (curYaw > TwoPi) curYaw -= TwoPi;
            while (curYaw < -TwoPi) curYaw += TwoPi;
            const float newYaw = std::atan2(xDiff, yDiff);
            yawDiff = newYaw - curYaw;
            if (yawDiff < 0.0f) yawDiff += TwoPi;
            else if (yawDiff >= TwoPi) yawDiff -= TwoPi;
            if (yawDiff > Pi) yawDiff -= TwoPi;
            else if (yawDiff < -Pi) yawDiff += TwoPi;
            mMoveYaw = yawDiff;
            moveYaw = newYaw;
        }
        xDiff = mPilotDestination.x - curLocation.x;
        yDiff = mPilotDestination.y - curLocation.y;
        const float zDiff = mPilotDestination.z - curLocation.z;
        const float dist2D = std::hypot(yDiff, xDiff);
        const Point3F yAxis = normalize({myVehicle->transform[1], myVehicle->transform[5], myVehicle->transform[9]});
        const float curPitch = std::atan2(1.0f, yAxis.z) - (Pi / 2.0f);
        float newPitch = std::atan2(dist2D, zDiff) - (Pi / 2.0f);
        newPitch = std::max(mPitchUpMax, std::min(mPitchDownMax, newPitch));
        int pitchDir = 0;
        const float pitchDelta = curPitch - mPreviousPitch;
        mPreviousPitch = curPitch;
        if (mPilotSpeed > 0.0f && len(velocity2D) > 5.0f) {
            const float timeToDest = dist2D / len(velocity2D);
            const float vertDist = velocity.z * timeToDest;
            if (zDiff > 5.0f && (vertDist < zDiff - 2.0f || curPitch > 0.0f)) pitchDir = 1;
            else if (zDiff < -5.0f && (vertDist > zDiff + 2.0f || curPitch < 0.0f)) pitchDir = -1;
            if (pitchDir > 0 && (curPitch < mPitchUpMax || pitchDelta < -mPitchIncMax)) pitchDir = -1;
            else if (pitchDir < 0 && (curPitch > mPitchDownMax || pitchDelta > mPitchIncMax)) pitchDir = 1;
            if (pitchDir == 0) {
                if (curPitch < 0.0f && pitchDelta < 0) pitchDir = -1;
                else if (curPitch > 0.0f && pitchDelta > 0) pitchDir = 1;
            }
            if (pitchDir > 0) mMovePitch = mPitchUpMax;
            else if (pitchDir < 0) mMovePitch = mPitchDownMax;
        }
        float speed = 0.0f;
        // (the shipped test is mFabs(yawDiff < 0.8f): the comparison's 0/1)
        if ((std::fabs(xDiff) > 5.0f || std::fabs(yDiff) > 5.0f) && std::fabs((float)(yawDiff < 0.8f)))
            speed = 1.0f - (std::fabs(yawDiff) / Pi);
        if (speed > 0.0f && dist2D >= 5.0f && dist2D <= 20.0f) speed *= 0.3f * std::min(0.7f, dist2D / 20.0f);
        if (dist2D < 6.0f && len(velocity2D) > 5.0f && std::fabs(velocityDot) >= 0.78f)
            speed = std::max(-1.0f, -2.0f * speed);
        speed *= mPilotSpeed;
        if (len(velocity2D) < 8.0f && curPitch <= 0.05f && zDiff > 5.0f) mTriggers[JetTrigger] = true;
        mCurrentPitch = curPitch;
        mDesiredPitch = newPitch;
        mPitchIncrement = mMovePitch;
        if ((std::fabs(xDiff) > 5.0f || std::fabs(yDiff) > 5.0f) && !isZeroF(speed))
            moveToward(curLocation, mPilotDestination, speed, moveYaw, mMoveX, mMoveY, 5.0f);
    }
    const ClientMoveIn move = packMove();
    for (bool& t : mTriggers) t = false;
    return move;
}

void AIConnection::clearStep() {
    if (!mStep.empty()) ScriptEngine::instance().deleteScriptObject(mStep);
    mStep.clear();
}

void AIConnection::setStep(const std::string& stepKey) {
    clearStep();
    mStep = stepKey;
}

AIStep* AIConnection::step() const { return mStep.empty() ? nullptr : EngineObjects::get<AIStep>(mStep); }

const char* AIConnection::getStepStatus() {
    AIStep* s = step();
    if (!s) return "Finished";
    switch (s->getStatus()) {
    case AIStep::InProgress: return "InProgress";
    case AIStep::Failed: return "Failed";
    default: return "Finished";
    }
}

const char* AIConnection::getStepName() {
    AIStep* s = step();
    return !s || s->name.empty() ? "NONE" : s->name.c_str();
}

AITask* AIConnection::currentTask() const {
    return mCurrentTask.empty() ? nullptr : EngineObjects::get<AITask>(mCurrentTask);
}

void AIConnection::clearTasks() {
    std::vector<std::string> tasks;
    tasks.swap(mTaskList);
    for (const auto& key : tasks) ScriptEngine::instance().deleteScriptObject(key);
    mCurrentTask.clear();
}

void AIConnection::addTask(const std::string& task) {
    if (!task.empty()) mTaskList.push_back(task);
}

void AIConnection::removeTask(int taskId) {
    for (size_t i = 0; i < mTaskList.size(); ++i) {
        ScriptObject* object = ScriptEngine::instance().findObject(mTaskList[i].c_str());
        if (!object || ScriptEngine::instance().objectId(object) != taskId) continue;
        if (mCurrentTask == mTaskList[i]) {
            if (AITask* current = currentTask()) current->retire(this);
            mCurrentTask.clear();
        }
        const std::string key = mTaskList[i];
        mTaskList.erase(mTaskList.begin() + i);
        ScriptEngine::instance().deleteScriptObject(key);
        break;
    }
}

void AIConnection::listTasks() {
    for (const auto& key : mTaskList)
        if (auto* task = EngineObjects::get<AITask>(key); task && task->script)
            Console::instance().printf(LogLevel::Info, "%d: %s", ScriptEngine::instance().objectId(task->script),
                                       task->name.c_str());
}

void AIConnection::missionCycleCleanup() {
    setMoveMode(ModeStop);
    clearTasks();
    clearStep();
    mPath.forceSearch();
    mPath.missionCycleCleanup();
}
