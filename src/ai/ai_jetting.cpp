#include "ai/ai_jetting.h"
#include "ai/ai_connection.h"
#include "ai/graph.h"
#include "sim/containers.h"
#include "sim/player.h"
#include "sim/projectile_aim.h"
#include <algorithm>
#include <cmath>

using namespace NavMath;
using namespace SimContainer;

namespace {

constexpr float GravityConstant = 20.0f;
constexpr float AmountInFront = 0.37f;
constexpr float PullInPercent = 0.63f;
constexpr float HitPointThresh = 1.2f;
constexpr int JumpWaitCount = 3;

Point3F flat(Point3F p) {
    p.z = 0;
    return p;
}

// Run LOS to see if we can safely jump to our destination.
constexpr uint32_t scBonkMask = InteriorObjectType | StaticShapeObjectType | StaticObjectType | TerrainObjectType;

// We may still need to nudge a little bit to assure we have clearance.
constexpr uint32_t sLOSMask = InteriorObjectType | StaticShapeObjectType | StaticObjectType | TerrainObjectType;
constexpr float sClearDistance = 1.4f;

} // namespace

bool AIJetting::init(const Point3F& dest, bool intoMount, NavJetting* jetInfo) {
    mSeekDest = dest;
    mFirstTime = true;
    mWillBonk = false;
    mLaunchSpeed = 0.1f;
    mIntoMount = intoMount;
    mStatus = AIJetWorking;
    mJetInfo = jetInfo;
    return true;
}

// We don't worry about this if we're jetting up more than 2 meters.  The
// shipped check always answers false ("Disable temporarily...").
bool AIJetting::willBonk(Point3F src, Point3F dst) {
    if (dst.z - src.z < 2.0f) {
        const float dx = src.x - dst.x, dy = src.y - dst.y;
        if (dx * dx + dy * dy < LiberalBonkXY * LiberalBonkXY) {
            src.z = dst.z = std::max(src.z, dst.z) + 3.7f;
            Loser los(scBonkMask);
            if (!los.haveLOS(src, dst)) return false;
        }
    }
    return false;
}

void AIJetting::newState(int16_t state, int16_t counter) {
    mCounter = counter;
    mState = state;
}

// Distance from the landing "wall", (dist < 0) meaning we're that much beyond it.
float AIJetting::distFromWall(const Point3F& loc) {
    return mDot(flat(mWallPoint - loc), mWallNormal);
}

bool AIJetting::figureLandingDist(AIConnection* ai, float& dist) {
    const float A = -(GravityConstant * 0.5f);
    const float B = ai->mVelocity.z;
    const float C = ai->mLocation.z - mLandPoint.z;
    float solutions[2];
    const uint32_t N = ProjectileAim::solveQuadratic(A, B, C, solutions);
    if (N > 0) {
        // use the larger time solution (first will be negative, or coming up on soln)
        const float T = solutions[N - 1];
        dist = T * NavMath::len(ai->mVelocity2D);
        return true;
    }
    return false;
}

bool AIJetting::firstTimeStuff(AIConnection* ai, PlayerObject* player) {
    if (mFirstTime) {
        mFirstTime = false;
        mCanInterupt = true;
        mWillBonk = false;
        mLandPoint = mSeekDest;
        mWallNormal = flat(mSeekDest - ai->mLocation);
        if ((mTotal2D = NavMath::len(mWallNormal)) < GraphJetFailXY) {
            mStatus = AIJetFail;
            return false;
        }
        const float zDiff = mSeekDest.z - ai->mLocation.z;
        mSlope = zDiff / mTotal2D;
        // Zero slope -> full speed, steep slope -> zero speed.
        mLaunchSpeed = mapValueQuadratic(mSlope, 1.3f, 0.0f, 0.0f, 1.0f);
        // Wall normal points along our path in XY plane.
        mWallNormal *= 1.0f / mTotal2D;
        mUpChute = mJetInfo && mJetInfo->mChuteUp;
        // our dest will be a unit or so beyond for sake of aiming
        mWallPoint = mLandPoint;
        mSeekDest += mWallNormal * 1.1f;
        // Hop over walls-
        if (mJetInfo) mSeekDest.z += mJetInfo->mHopOver;
        newState(AssureClear);
    }
    mShouldAim = false;
    if (mIntoMount && !player->mount.empty()) {
        mStatus = AIJetSuccess;
        return false;
    }
    return true;
}

void AIJetting::setAim(const Point3F& /*always mSeekDest now*/) { mShouldAim = true; }

bool AIJetting::shouldAimAt(Point3F& atWhere) {
    if (mShouldAim) atWhere = mSeekDest;
    return mShouldAim;
}

bool AIJetting::assureClear(AIConnection* ai, PlayerObject*) {
    // Get loc a little bit above the feet-
    Point3F botLoc{ai->mLocation.x, ai->mLocation.y, ai->mLocation.z + 0.2f};
    if (botLoc.z < mSeekDest.z - 2.0f) {
        // This shouldn't go on very long, just need a nudge if anything...
        if (++mCounter < 32) {
            Point3F vec = flat(botLoc - mSeekDest);
            const float length = NavMath::len(vec);
            if (length > 0.1f) {
                Loser loser(sLOSMask);
                Point3F clearLoc{botLoc.x, botLoc.y, mSeekDest.z};
                vec *= sClearDistance / length;
                clearLoc -= vec;
                if (!loser.haveLOS(botLoc, clearLoc)) {
                    // Seek away, our vec contains which way to go...
                    ai->setMoveLocation(botLoc += vec);
                    ai->setMoveSpeed(0.6f);
                    return false;
                }
            }
        }
    }
    mWillBonk = willBonk(ai->mLocation, mSeekDest);
    newState(AwaitEnergy);
    return false;
}

// Wait for amount of energy we think we need.
bool AIJetting::awaitEnergy(AIConnection* ai, PlayerObject* player) {
    ai->setMoveLocation(mSeekDest);
    if (lenSquared(ai->mVelocity) < 0.2f) {
        if (player->getEnergyValue() > 0.99f) {
            newState(PrepareToJump);
        } else {
            // The same methods the graph uses to decide that a given hop is
            // makeable with a certain energy ability configuration.
            float ratings[2];
            JetManager::Ability ability;
            ability.dur = player->getJetAbility(ability.acc, ability.dur, ability.v0);
            gNavGraph->jetManager().calcJetRatings(ratings, ability);
            const float jetD = gNavGraph->jetManager().jetDistance(ai->mLocation, mSeekDest);
            if (jetD < ratings[!mWillBonk && player->canJump()]) newState(PrepareToJump);
        }
    } else {
        ai->setMoveSpeed(0);
    }
    return false;
}

bool AIJetting::prepareToJump(AIConnection* ai, PlayerObject* player) {
    ai->setMoveLocation(mSeekDest);
    ai->setMoveSpeed(0);
    if (++mCounter >= JumpWaitCount) {
        // Can't check if can jump so often right now - so just do it once...
        bool jumpReady = mCounter == JumpWaitCount && (player->haveContact() || !player->mount.empty());
        // HACK to remedy problem with never finding a contact surface sometimes.
        if (!jumpReady && mCounter > JumpWaitCount * 8) jumpReady = lenSquared(ai->mVelocity) < 0.04f;
        if (jumpReady) {
            mJumpPoint = ai->mLocation;
            Point3F here = ai->mLocation;
            here.z += 100;
            ai->setMoveLocation(here);
            if (!mWillBonk || !player->mount.empty()) ai->pressJump();
            ai->pressJet();
            newState(InTheAir);
            mCanInterupt = false;
        }
    }
    return false;
}

bool AIJetting::inTheAir(AIConnection* ai, PlayerObject* player) {
    bool advanceState = false;
    if (player->haveContact()) {
        if (++mCounter >= 2) {
            mStatus = AIJetFail;
            return true;
        }
    } else {
        mCounter = 0;
    }
    ai->setMoveLocation(mSeekDest);
    if (mUpChute) {
        // Basically jet until bonk as long as we're going vertically
        if (lenSquared(ai->mVelocity2D) > 0.04f || ai->mVelocity.z < -0.1f) {
            mUpChute = false;
        } else {
            ai->setMoveSpeed(0.0f);
            ai->pressJet();
        }
    }
    if (!mUpChute) {
        if (ai->mLocation.z > mSeekDest.z) {
            // Must monitor our Z velocity-
            ai->setMoveSpeed(ai->mVelocity.z < 0 ? 0.0f : 1.0f);
            ai->pressJet();
        } else {
            ai->setMoveSpeed(0.0f);
            const float zSpeed = ai->mVelocity.z;
            const float howHigh = zSpeed * (zSpeed / GravityConstant);
            if (zSpeed < 0.01f || ai->mLocation.z + howHigh < mSeekDest.z + 1.2f) ai->pressJet();
        }
    }
    // get 2D distance to wall:
    const float wallD = distFromWall(ai->mLocation);
    if (wallD < mTotal2D * 0.5f) setAim(mSeekDest);
    if (wallD < 0.1f) advanceState = true;
    if (!advanceState) {
        float landD;
        if (figureLandingDist(ai, landD))
            if (landD > wallD - AmountInFront) advanceState = true;
    }
    if (advanceState) newState(SlowToLand);
    return false;
}

bool AIJetting::slowToLand(AIConnection* ai, PlayerObject* player) {
    if (player->haveContact()) {
        // First clause meant to handle case where we didn't get off the ledge-
        if (ai->mLocation.z - mLandPoint.z > 1.3f && distFromWall(ai->mLocation) > 1.0f) {
            newState(PrepareToJump, JumpWaitCount - 1);
        } else if (!within(ai->mLocation, mLandPoint, 4.0f)) {
            mStatus = AIJetFail;
            return true;
        } else {
            mCanInterupt = true;
            newState(WalkToPoint);
        }
    } else {
        const float speed2 = len2D(ai->mVelocity.x, ai->mVelocity.y);
        setAim(mSeekDest);
        // Use velocity checks to finish it up-
        const float zvel = std::fabs(ai->mVelocity.z);
        if (speed2 < 0.1f && zvel < 0.1f) return true;
        float landD;
        const float wallD = distFromWall(ai->mLocation);
        const bool beyond = figureLandingDist(ai, landD) && landD > wallD - AmountInFront;
        const bool pullIn = speed2 * PullInPercent > wallD && speed2 > 0.7f;
        if (beyond && pullIn) { // try to slow down
            ai->setMoveLocation(mJumpPoint);
            ai->setMoveSpeed(1.0f);
            ai->pressJet();
        } else {
            ai->setMoveLocation(ai->mLocation);
            ai->setMoveSpeed(0.0f);
            if (!beyond) ai->pressJet();
        }
    }
    return false;
}

bool AIJetting::walkToPoint(AIConnection* ai, PlayerObject*) {
    ai->setMoveLocation(mLandPoint);
    ai->setMoveTolerance(HitPointThresh * 0.6f);
    ai->setMoveSpeed(0.3f);
    if (within_2D(ai->mLocation, mLandPoint, HitPointThresh) || ++mCounter > 10) {
        // wound up under or over the point...
        mStatus = std::fabs(ai->mLocation.z - mLandPoint.z) > HitPointThresh ? AIJetFail : AIJetSuccess;
        ai->setMoveSpeed(0.0f);
        return true;
    }
    return false;
}

// Returns true when done.
bool AIJetting::process(AIConnection* ai, PlayerObject* player) {
    if (!firstTimeStuff(ai, player)) return true;
    switch (mState) {
    case AssureClear: return assureClear(ai, player);
    case AwaitEnergy: return awaitEnergy(ai, player);
    case PrepareToJump: return prepareToJump(ai, player);
    case InTheAir: return inTheAir(ai, player);
    case SlowToLand: return slowToLand(ai, player);
    case WalkToPoint: return walkToPoint(ai, player);
    }
    return true;
}
