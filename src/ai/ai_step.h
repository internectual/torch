#pragma once
// AIStep (ai/aiStep.cc, ai/aiNavStep.cc): what a bot is doing right now,
// processed each tick before its engagement and movement.
#include "sim/engine_object.h"
#include "ai/ai_jetting.h"
#include "core/math.h"
#include <string>
#include <vector>

class AIConnection;
class PlayerObject;
struct ScriptObject;

class AIStep : public EngineObject {
public:
    enum StepResult { InProgress = 0, Failed, Finished };
    int getStatus() const { return mStatus; }
    virtual void process(AIConnection* ai, PlayerObject* player);
    std::string name; // the name the step registered under (getStepName)

protected:
    int mStatus = InProgress;
};

class AIStepEscort : public AIStep {
public:
    explicit AIStepEscort(const std::string& clientToEscort = {}) : mClientToEscort(clientToEscort) {}
    void process(AIConnection* ai, PlayerObject* player) override;

private:
    bool mInitialized = false, mProximityBuffer = false;
    int mResetDestinationCounter = 0;
    std::string mClientToEscort;
    int mStoppedTime = 0;
    std::vector<Point3F> mChokePoints;
    bool mIdleStarted = false;
    int mIdleNextTime = 0;
};

class AIStepEngage : public AIStep {
public:
    explicit AIStepEngage(const std::string& target = {}) : mTarget(target) {}
    void process(AIConnection* ai, PlayerObject* player) override;
    Point3F findStraifeLocation(AIConnection* client);

private:
    bool mInitialized = false;
    std::string mTarget;
    int mStraifeCounter = 0, mPauseCounter = 0, mCheckLOSCounter = 0;
    bool mClearLOSToTarget = false, mPausing = false;
    Point3F mStraifeLocation{0, 0, 0};
    bool mSearching = false, mSearchInitialized = false;
    Point3F mChokeLocation{0, 0, 0};
    std::vector<Point3F> mChokePoints;
    int mChokeIndex = -1, mSearchTimer = 0;
};

class AIStepRangeObject : public AIStep {
public:
    AIStepRangeObject(const std::string& targetObject, const std::string& projectile, float minDist, float maxDist,
                      const Point3F* fromLocation);
    void process(AIConnection* ai, PlayerObject* player) override;

private:
    bool mInitialized = false;
    std::string mTargetObject;
    ScriptObject* mProjectile = nullptr;
    uint32_t mLOSMask = 0;
    float mMinDistance = 0, mMaxDistance = 1000;
    Point3F mTargetPoint{0, 0, 0}, mGraphDestination{0, 0, 0}, mFromLocation{-1, -1, -1}, mPrevLocation{0, 0, 0};
    int mCheckLOSCounter = 0;
};

class AIStepIdlePatrol : public AIStep {
public:
    explicit AIStepIdlePatrol(const Point3F* idleLocation = nullptr);
    void process(AIConnection* ai, PlayerObject* player) override;

private:
    enum IdleStates { MoveToLocation, LookAround };
    bool mInitialized = false;
    Point3F mIdleLocation{0, 0, 0}, mMoveLocation{0, 0, 0};
    std::vector<Point3F> mChokePoints;
    int mChokeIndex = 0;
    float mOutdoorRadius = 0;
    int mIdleState = MoveToLocation, mIdleNextTime = 0, mIdleEndTime = 0;
    bool mStateInit = false, mHeadingHome = false;
};

class AIStepJet : public AIStep {
public:
    explicit AIStepJet(const Point3F& dest) { mJetting.init(dest); }
    void process(AIConnection* ai, PlayerObject* player) override;

private:
    AIJetting mJetting;
};
