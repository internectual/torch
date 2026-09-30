#include "ai/ai_step.h"
#include "ai/ai_connection.h"
#include "ai/ai_task.h"
#include "sim/containers.h"
#include "sim/nav_graph.h"
#include "sim/player.h"
#include "sim/projectile_aim.h"
#include "script/script_engine.h"
#include <algorithm>
#include <cmath>
#include <strings.h>

using namespace SimContainer;

namespace {

float randF() { return Nav::gRandGen().randF(); }

bool isDead(PlayerObject* player) { return strcasecmp(player->stateName(), "dead") == 0; }

PlayerObject* controlledPlayer(const std::string& client) {
    auto* connection = client.empty() ? nullptr : EngineObjects::get<GameConnection>(client);
    return connection ? EngineObjects::get<PlayerObject>(connection->controlObject()) : nullptr;
}

Point3F position(ShapeBase& shape) { return {shape.transform[3], shape.transform[7], shape.transform[11]}; }

// A look point near `lookLocation`, 10 m out with noise (the idle glances).
Point3F glance(AIConnection* client, const Point3F& lookLocation, float minLen) {
    Point3F tempVector = sub(lookLocation, client->mLocation);
    tempVector.z = 0;
    if (len(tempVector) < minLen) tempVector = minLen < 0.01f ? Point3F{0, 1, 0} : Point3F{1, 0, 0};
    tempVector = mul(normalize(tempVector), 10.0f);
    const float noise = 2.0f;
    tempVector.x += -noise + randF() * 2 * noise;
    tempVector.y += -noise + randF() * 2 * noise;
    tempVector.z = 1.5f + randF() * 2 * noise;
    return add(client->mLocation, tempVector);
}

} // namespace

void AIStep::process(AIConnection* client, PlayerObject* player) {
    if (mStatus != InProgress) return;
    if (!client || !player) {
        mStatus = Failed;
        return;
    }
    if (isDead(player)) mStatus = Failed;
}

//----------------------------------------------------------------------------

void AIStepEscort::process(AIConnection* client, PlayerObject* player) {
    AIStep::process(client, player);
    if (mStatus != InProgress) return;
    PlayerObject* targetPlayer = controlledPlayer(mClientToEscort);
    if (!targetPlayer) {
        mStatus = Failed;
        return;
    }
    if (!mInitialized) {
        mInitialized = true;
        client->setMoveMode(!targetPlayer->mount.empty() ? AIConnection::ModeMountVehicle : AIConnection::ModeExpress);
        client->setMoveTolerance(4.0f);
        client->setEnergyLevels(0.05f, 0.15f);
    }
    const Point3F targetLocation = position(*targetPlayer);
    const float distToTarget = std::max(client->getPathDistRemaining(20), len(sub(client->mLocation, targetLocation)));
    if (--mResetDestinationCounter <= 0) {
        mResetDestinationCounter = 5;
        client->setMoveDestination(targetLocation);
    }
    const int curTime = (int)simTimeMs();
    auto follow = [&] {
        client->setMoveMode(!targetPlayer->mount.empty() ? AIConnection::ModeMountVehicle : AIConnection::ModeExpress);
    };
    auto stop = [&] {
        if (client->getMoveMode() != AIConnection::ModeStop) {
            client->setMoveMode(AIConnection::ModeStop);
            mStoppedTime = curTime;
            mIdleStarted = false;
        }
    };
    const Point3F headLocation{targetLocation.x, targetLocation.y, targetLocation.z + 1.6f};
    if (distToTarget < 10.0f) {
        mProximityBuffer = true;
        stop();
    } else if (distToTarget < 16.0f) {
        if (mProximityBuffer) {
            stop();
        } else {
            if (client->getMoveMode() == AIConnection::ModeStop) client->setScriptAimLocation(headLocation, 500);
            follow();
        }
    } else {
        if (client->getMoveMode() == AIConnection::ModeStop) client->setScriptAimLocation(headLocation, 500);
        mProximityBuffer = false;
        follow();
    }
    // Stopped a while: look around the choke points (or at the target).
    if (client->getMoveMode() == AIConnection::ModeStop && curTime - mStoppedTime > 5000) {
        if (!mIdleStarted) {
            mIdleStarted = true;
            mChokePoints.clear();
            NavigationGraph::getChokePoints(client->mLocation, mChokePoints, 10, 65);
            mIdleNextTime = 0;
        }
        if (curTime > mIdleNextTime) {
            mIdleNextTime = curTime + 2000 + (int)(randF() * 3000);
            if (randF() < 0.06f) {
                player->setActionThread("pda", false, true);
            } else {
                Point3F lookLocation;
                if (mChokePoints.empty()) {
                    lookLocation = randF() < 0.3f ? targetLocation : client->mLocation;
                } else {
                    const int index = (int)(randF() * (mChokePoints.size() + 0.9));
                    lookLocation = index == (int)mChokePoints.size() ? targetLocation : mChokePoints[index];
                }
                if (!client->scriptIsAiming()) client->setScriptAimLocation(glance(client, lookLocation, 0.1f), 3000);
            }
        }
    }
}

//----------------------------------------------------------------------------

Point3F AIStepEngage::findStraifeLocation(AIConnection* client) {
    Point3F tempV = sub(client->mLocation, client->mTargLocation);
    if (len(tempV) < 0.001f) tempV = {0, 1, 0};
    tempV = normalize(tempV);
    Point3F newLocationVector = cross(tempV, {0, 0, 1});
    if (len(newLocationVector) < 0.001f) newLocationVector = {0, 1, 0};
    newLocationVector = normalize(newLocationVector);
    const float newLocationLength = len(newLocationVector);
    if (newLocationLength > 0.9f && newLocationLength < 1.1f) {
        const Point3F offset = mul(newLocationVector, (float)client->mEngageMinDistance);
        if (len(client->mVelocity2D) < 1.0f) return add(client->mTargLocation, offset);
        // straife the way we are already moving
        const float myAngle = AIConnection::get2DAngle(add(client->mLocation, client->mVelocity2D), client->mLocation);
        const float tempAngle1 = AIConnection::get2DAngle(client->mLocation, add(client->mTargLocation, newLocationVector));
        const float tempAngle2 = AIConnection::get2DAngle(client->mLocation, sub(client->mTargLocation, newLocationVector));
        return std::fabs(tempAngle1 - myAngle) < std::fabs(tempAngle2 - myAngle) ? add(client->mTargLocation, offset)
                                                                                 : sub(client->mTargLocation, offset);
    }
    return client->mTargLocation;
}

void AIStepEngage::process(AIConnection* client, PlayerObject* player) {
    AIStep::process(client, player);
    if (mStatus != InProgress) return;
    if (!mInitialized) {
        mInitialized = true;
        client->setMoveTolerance(2.0f);
        client->setEngageTarget(mTarget.empty() ? nullptr : EngineObjects::get<GameConnection>(mTarget));
        client->setEnergyLevels(0.3f, 0.2f);
        return;
    }
    if (!client->mTargetPlayer) {
        mStatus = Finished;
        client->setMoveMode(AIConnection::ModeStop);
        return;
    }
    const bool targIsOutdoors = client->getOutdoorRadius(client->mTargLocation) > 0;
    const bool playerIsOutdoors = client->getOutdoorRadius(client->mLocation) > 0;
    int losTime;
    Point3F losLocation;
    const bool hasLOS = client->hasLOSToClient(client->getEngageTarget(), losTime, losLocation);
    if (targIsOutdoors) {
        bool timeToStraife = false;
        mStraifeCounter--;
        if (client->getMoveMode() == AIConnection::ModeGainHeight) {
            if (client->mEnergy < 0.2f || client->mLocation.z - client->mTargLocation.z > 15.0f ||
                client->mEnergy < client->mWeaponEnergy - client->mEnergyReserve)
                timeToStraife = true;
        } else {
            if (playerIsOutdoors && mPauseCounter <= 0 &&
                (client->mEnergy > 0.5f || client->mEnergy - client->mTargEnergy > 0.3f) &&
                client->mEnergy > client->mWeaponEnergy + client->mEnergyFloat && client->mDistToTarg2D < 45.0f &&
                len(client->mVelocity2D) > 5.0f && client->mLocation.z - client->mTargLocation.z < 15.0f &&
                client->mSkillLevel >= 0.3f) {
                client->setMoveMode(AIConnection::ModeGainHeight);
            } else {
                Point3F straifeVector = sub(client->mLocation, mStraifeLocation);
                straifeVector.z = 0;
                if (len(straifeVector) < std::min(8, client->mEngageMinDistance)) {
                    client->setMoveMode(AIConnection::ModeStop);
                    if (!mPausing && (hasLOS || losTime < 5000)) {
                        const float skillAdjust =
                            (1.0f - client->mSkillLevel) * (1.0f - client->mSkillLevel) * (1.0f - client->mSkillLevel);
                        mPauseCounter = (int)(randF() * 400.0f * skillAdjust);
                        mPausing = true;
                    }
                    mPauseCounter--;
                }
                if (mStraifeCounter <= 0 || mPauseCounter <= 0) timeToStraife = true;
            }
        }
        if (timeToStraife) {
            const float skillAdjust = (1.0f - client->mSkillLevel) * (1.0f - client->mSkillLevel);
            mStraifeCounter = 70 + (int)(400.0f * skillAdjust);
            mPauseCounter = 32767;
            mPausing = false;
            mStraifeLocation = findStraifeLocation(client);
            client->setMoveMode(AIConnection::ModeExpress);
            client->setMoveDestination(mStraifeLocation);
        }
    } else {
        if (losLocation.x == 0 && losLocation.y == 0 && losLocation.z == 0) losLocation = client->mTargLocation;
        if (playerIsOutdoors) losLocation = client->mTargLocation;
        if (--mCheckLOSCounter <= 0) {
            mCheckLOSCounter = 10;
            RayInfo rayInfo;
            mClearLOSToTarget = !castRay(worldBoxCenter(player->script), client->mTargLocation,
                                         TerrainObjectType | InteriorObjectType | WaterObjectType | ForceFieldObjectType,
                                         rayInfo, {player->script});
        }
        if (!mClearLOSToTarget && !mSearching && len(sub(client->mLocation, losLocation)) < 3.0f &&
            client->getMoveMode() == AIConnection::ModeStop) {
            mSearching = true;
            mSearchInitialized = false;
        } else if (mSearching) {
            if (mClearLOSToTarget) {
                mSearching = false;
            } else if (!mSearchInitialized) {
                mSearchInitialized = true;
                mChokeLocation = losLocation;
                mChokeIndex = -1;
                NavigationGraph::getChokePoints(losLocation, mChokePoints, 10, 65);
                mSearchTimer = (int)simTimeMs() + 3000;
            } else {
                if (len(sub(client->mLocation, mChokeLocation)) < 3.0f &&
                    client->getMoveMode() == AIConnection::ModeStop && (int)simTimeMs() > mSearchTimer) {
                    if (mChokeIndex >= 0) mChokePoints.erase(mChokePoints.begin() + mChokeIndex);
                    if (mChokePoints.empty()) {
                        mStatus = Failed;
                        return;
                    }
                    mChokeIndex = (int)(randF() * (mChokePoints.size() - 0.1f));
                    mChokeLocation = mChokePoints[mChokeIndex];
                    mSearchTimer = (int)simTimeMs() + (int)(randF() * 1000) + 2000;
                    client->setMoveMode(AIConnection::ModeExpress);
                }
                client->setMoveDestination(mChokeLocation);
            }
        } else {
            client->setMoveDestination(losLocation);
            if (mClearLOSToTarget && client->mDistToTarg2D < 15.0f) client->setMoveMode(AIConnection::ModeStop);
            else client->setMoveMode(AIConnection::ModeExpress);
        }
    }
}

//----------------------------------------------------------------------------

AIStepRangeObject::AIStepRangeObject(const std::string& targetObject, const std::string& projectile, float minDist,
                                     float maxDist, const Point3F* fromLocation)
    : mTargetObject(targetObject) {
    // The shipped constructor tests *minDist / *maxDist for nonzero.
    const float minDistance = minDist ? minDist : 0, maxDistance = maxDist ? maxDist : 1000;
    mMinDistance = std::max(0.0f, std::min(minDistance, maxDistance));
    mMaxDistance = std::max(1.0f, std::max(minDistance, maxDistance));
    if (fromLocation) mFromLocation = *fromLocation;
    mLOSMask = TerrainObjectType | InteriorObjectType | PlayerObjectType | ForceFieldObjectType;
    if (EngineObjects::get<PlayerObject>(mTargetObject))
        mLOSMask = TerrainObjectType | InteriorObjectType | (1u << 13) /*StaticShape*/ | ForceFieldObjectType;
    if (!projectile.empty() && strcasecmp(projectile.c_str(), "NoAmmo") != 0)
        mProjectile = ScriptEngine::instance().findObject(projectile.c_str());
    if (!EngineObjects::get<ShapeBase>(mTargetObject) || !mProjectile) mStatus = Failed;
}

void AIStepRangeObject::process(AIConnection* client, PlayerObject* player) {
    if (!client || !player || isDead(player)) {
        mStatus = Failed;
        return;
    }
    auto* target = EngineObjects::get<ShapeBase>(mTargetObject);
    if (!target || !mProjectile) {
        mStatus = Failed;
        return;
    }
    if (!mInitialized) {
        mInitialized = true;
        client->setMoveTolerance(0.25f);
        client->setEngageTarget(nullptr);
        mTargetPoint = worldBoxCenter(target->script);
        const Point3F nearPoint = mFromLocation.x != -1 || mFromLocation.y != -1 || mFromLocation.z != -1
            ? mFromLocation : client->mLocation;
        mGraphDestination = NavigationGraph::findLOSLocation(client->mLocation, mTargetPoint, mMinDistance,
                                                             SphereF{nearPoint, mMinDistance / 2.0f}, mMaxDistance);
        // Too close a range: no graph point will do, head straight there.
        if (mMaxDistance <= 10.0f && len(sub(mTargetPoint, mGraphDestination)) > 10.0f)
            mGraphDestination = mTargetPoint;
        client->setMoveDestination(mGraphDestination);
        client->setMoveMode(AIConnection::ModeExpress);
        mStatus = InProgress;
    }
    if (--mCheckLOSCounter > 0) return;
    mCheckLOSCounter = 6;
    auto approach = [&] {
        if (len(sub(client->mLocation, mGraphDestination)) < 8.0f && client->getMoveMode() == AIConnection::ModeStop) {
            mMaxDistance = std::max(mMaxDistance - 10.0f, mMinDistance);
            mInitialized = false;
        } else {
            client->setMoveDestination(mGraphDestination);
            client->setMoveMode(AIConnection::ModeExpress);
        }
        mStatus = InProgress;
    };
    const float directionDot = AIConnection::get2DDot(sub(mGraphDestination, client->mLocation),
                                                      sub(mTargetPoint, client->mLocation));
    const float distToTarg = len(sub(mTargetPoint, client->mLocation));
    if (((distToTarg <= mMaxDistance || directionDot <= 0.0f) && (distToTarg > mMinDistance || directionDot > 0.0f)) ||
        len(sub(client->mLocation, mGraphDestination)) < 8.0f) {
        Point3F aimVectorMin, aimVectorMax;
        float timeMin, timeMax;
        bool targetInRange = ProjectileAim::calculateAim(mProjectile, mTargetPoint, {0, 0, 0}, client->mMuzzlePosition,
                                                         client->mVelocity, &aimVectorMin, &timeMin, &aimVectorMax,
                                                         &timeMax);
        if (targetInRange) {
            const bool playerIsOutdoors = client->getOutdoorRadius(client->mLocation) > 0;
            bool clearLOSToTarget = false;
            if (Fields::boolean(mProjectile, "isBallistic", false) && playerIsOutdoors) {
                clearLOSToTarget = true;
            } else {
                RayInfo rayInfo;
                if (!castRay(client->mMuzzlePosition, mTargetPoint, mLOSMask, rayInfo, {player->script}))
                    clearLOSToTarget = true;
                else if (rayInfo.object == target->script)
                    clearLOSToTarget = true;
            }
            targetInRange = clearLOSToTarget;
        }
        if (targetInRange) {
            client->setMoveMode(AIConnection::ModeStop);
            // (the shipped code compares the location it just stored)
            mPrevLocation = client->mLocation;
            mStatus = Finished;
        } else {
            approach();
        }
    } else {
        approach();
    }
}

//----------------------------------------------------------------------------

AIStepIdlePatrol::AIStepIdlePatrol(const Point3F* idleLocation) {
    if (idleLocation) mIdleLocation = *idleLocation;
}

void AIStepIdlePatrol::process(AIConnection* client, PlayerObject* player) {
    AIStep::process(client, player);
    if (mStatus != InProgress) return;
    if (!mInitialized) {
        mInitialized = true;
        if (mIdleLocation.x == 0 && mIdleLocation.y == 0 && mIdleLocation.z == 0) mIdleLocation = client->mLocation;
        NavigationGraph::getChokePoints(mIdleLocation, mChokePoints, 25, 65);
        mOutdoorRadius = client->getOutdoorRadius(mIdleLocation);
        mIdleState = MoveToLocation;
        mStateInit = false;
        mMoveLocation = mIdleLocation;
        mHeadingHome = true;
        return;
    }
    switch (mIdleState) {
    case MoveToLocation:
        if (!mStateInit) {
            mStateInit = true;
            client->setMoveDestination(mMoveLocation);
            client->setMoveMode(AIConnection::ModeExpress);
            client->setMoveTolerance(4.0f);
        } else if (client->getPathDistRemaining(20.0f) < 8.0f) {
            client->setMoveMode(AIConnection::ModeStop);
            mIdleState = LookAround;
            mStateInit = false;
        }
        break;
    case LookAround: {
        const int curTime = (int)simTimeMs();
        if (!mStateInit) {
            mStateInit = true;
            mIdleNextTime = curTime + 2000 + (int)(randF() * 3000);
            mIdleEndTime = curTime + 8000 + (int)(randF() * 8000);
        }
        if (curTime > mIdleEndTime) {
            if (mHeadingHome) {
                mHeadingHome = false;
                if (mChokePoints.empty()) {
                    mMoveLocation = mIdleLocation;
                    if (mOutdoorRadius > 0.0f) {
                        const float noise = std::min(mOutdoorRadius, 30.0f);
                        mMoveLocation.x += -noise / 2 + randF() * noise;
                        mMoveLocation.y += -noise / 2 + randF() * noise;
                    }
                } else {
                    mChokeIndex = (int)(randF() * (mChokePoints.size() - 0.1f));
                    mMoveLocation = mChokePoints[mChokeIndex];
                }
            } else if (randF() < 0.4f || mChokePoints.size() <= 1) {
                mHeadingHome = true;
                mMoveLocation = mIdleLocation;
            } else {
                mChokeIndex = (int)(randF() * (mChokePoints.size() - 0.1f));
                mMoveLocation = mChokePoints[mChokeIndex];
            }
            mIdleState = MoveToLocation;
            mStateInit = false;
        } else if (curTime > mIdleNextTime) {
            mIdleNextTime = curTime + 2000 + (int)(randF() * 3000);
            if (randF() < 0.06f) {
                player->setActionThread("pda", false, true);
            } else {
                Point3F lookLocation;
                if (mChokePoints.empty()) {
                    lookLocation = mIdleLocation;
                } else {
                    const int index = (int)(randF() * (mChokePoints.size() + 0.9));
                    lookLocation = index == (int)mChokePoints.size() ? mIdleLocation : mChokePoints[index];
                }
                if (!client->scriptIsAiming()) client->setScriptAimLocation(glance(client, lookLocation, 0.001f), 3000);
            }
        }
        break;
    }
    }
}

//----------------------------------------------------------------------------

void AIStepJet::process(AIConnection* ai, PlayerObject* player) {
    AIStep::process(ai, player);
    if (mStatus != InProgress) return;
    ai->setMoveMode(AIConnection::ModeWalk);
    if (mJetting.process(ai, player)) mStatus = mJetting.status() == AIJetSuccess ? Finished : Failed;
}
