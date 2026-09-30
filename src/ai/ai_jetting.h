#pragma once
// AIJetting (ai/aiNavJetting.cc): a bot's hop along a jetting edge of the
// navigation graph: nudge clear, wait for energy, jump, jet, land and walk
// onto the landing point.
#include "core/math.h"
#include <cstdint>

class AIConnection;
class PlayerObject;
struct NavJetting;

enum AIJetStatus { AIJetInactive, AIJetWorking, AIJetFail, AIJetSuccess };

class AIJetting {
public:
    AIJetting() { reset(); }
    AIJetStatus status() const { return mStatus; }
    void reset() { mStatus = AIJetInactive; }

    bool init(const Point3F& dest, bool intoMount = false, NavJetting* jetInfo = nullptr);
    bool process(AIConnection* ai, PlayerObject* player);
    bool shouldAimAt(Point3F& atWhere);
    bool badTimeToSearch() const { return true; } // {return !mCanInterupt;}

protected:
    enum JettingStates { AssureClear, AwaitEnergy, PrepareToJump, InTheAir, SlowToLand, WalkToPoint };

    bool mFirstTime = true;
    bool mUpChute = false;
    bool mIntoMount = false;
    bool mShouldAim = false;
    bool mCanInterupt = true;
    bool mWillBonk = false;
    Point3F mSeekDest{0, 0, 0};
    Point3F mLandPoint{0, 0, 0};
    Point3F mWallNormal{0, 0, 0};
    Point3F mJumpPoint{0, 0, 0};
    Point3F mWallPoint{0, 0, 0};
    Point3F mTopOfChute{0, 0, 0};
    float mTotal2D = 0;
    float mLaunchSpeed = 0.1f;
    float mSlope = 0;
    int32_t mState = AssureClear;
    int32_t mCounter = 0;
    AIJetStatus mStatus = AIJetInactive;
    NavJetting* mJetInfo = nullptr;

    void newState(int16_t state, int16_t counter = 0);
    float distFromWall(const Point3F& loc);
    void setAim(const Point3F& at);
    bool willBonk(Point3F src, Point3F dst);
    bool figureLandingDist(AIConnection* ai, float& dist);
    bool firstTimeStuff(AIConnection* ai, PlayerObject* player);

    bool assureClear(AIConnection* ai, PlayerObject* player);
    bool awaitEnergy(AIConnection* ai, PlayerObject* player);
    bool prepareToJump(AIConnection* ai, PlayerObject* player);
    bool inTheAir(AIConnection* ai, PlayerObject* player);
    bool slowToLand(AIConnection* ai, PlayerObject* player);
    bool walkToPoint(AIConnection* ai, PlayerObject* player);
};
