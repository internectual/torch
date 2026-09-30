#pragma once
// AIConnection (ai/aiConnection.cc): a bot's GameConnection. Each server
// tick getMoveList runs its tasks (AITask), its step (AIStep), engagement
// and movement, then turns the aim and move locations into the Move its
// control object processes.
#include "sim/game_connection.h"
#include "ai/ai_jetting.h"
#include "ai/graph.h"
#include "core/math.h"
#include <memory>
#include <string>
#include <vector>

class PlayerObject;
class ShapeBase;
class VehicleObject;
class AIStep;
class AITask;

class AIConnection : public GameConnection {
public:
    enum ObjectModes {
        DestroyObject = 0, RepairObject, LazeObject, MortarObject, MissileVehicle, MissileNoLock,
        AttackMode1, AttackMode2, AttackMode3, AttackMode4, NumObjectModes
    };
    enum { ModeStop = 0, ModeWalk, ModeGainHeight, ModeExpress, ModeMountVehicle, ModeStuck, ModeCount };

    AIConnection();
    ~AIConnection() override;

    // AIConnection::getMoveList: the tick's move for the control object.
    ClientMoveIn getMove();
    // AIConnection::getMoveList: this tick's move for the control object.
    void getMoveList() override {
        if (moves.empty()) moves.push_back(getMove());
    }

    static float get2DDot(const Point3F& vec1, const Point3F& vec2);
    static float get2DAngle(const Point3F& endPt, const Point3F& basePt);
    float getOutdoorRadius(const Point3F& location);
    void setSkillLevel(float level) { mSkillLevel = std::max(0.0f, std::min(1.0f, level)); }
    float getSkillLevel() const { return mSkillLevel; }
    void setMoveSpeed(float speed);
    void setMoveMode(int mode, bool abortStuckCode = false);
    int getMoveMode() const { return mMoveMode; }
    void setMoveTolerance(float tolerance) { mMoveTolerance = std::max(0.1f, tolerance); }
    void setMoveLocation(const Point3F& location) { mMoveLocation = location; }
    void setMoveDestination(const Point3F& location);
    void setTurretMounted(int turretId) { mTurretMountedId = turretId; }
    void setAimLocation(const Point3F& location) { mAimLocation = location; }
    const Point3F& getAimLocation() const { return mAimLocation; }
    void setWeaponInfo(const std::string& projectile, int minDist, int maxDist, int triggerCount,
                       float energyRequired, float errorFactor = 1.0f);
    void setEnergyLevels(float eReserve, float eFloat = 0.10f) { mEnergyReserve = eReserve; mEnergyFloat = eFloat; }
    bool setScriptAimLocation(const Point3F& location, int duration);
    bool scriptIsAiming() const;
    void setPilotPitchRange(float pitchUpMax, float pitchDownMax, float pitchIncMax);
    void setPilotDestination(const Point3F& dest, float maxSpeed);
    void setPilotAimLocation(const Point3F& dest);
    void setEngageTarget(GameConnection* target);
    int getEngageTarget();
    void setTargetObject(ShapeBase* targetObject, float range = 35.0f, int objectMode = DestroyObject);
    int getTargetObject();
    bool targetInRange() const { return mTargetInRange || mTargetInSight; }
    bool targetInSight() const { return mTargetInSight; }
    void setVictim(GameConnection* victim, PlayerObject* corpse);
    int getVictimCorpse();
    int getVictimTime() const { return mVictimTime; }
    void setPathDest(const Point3F* dest = nullptr);
    float getPathDistance(const Point3F& destination, const Point3F& source);
    float getPathDistRemaining(float maxDist);
    Point3F getLOSLocation(const Point3F& targetPoint, float minDistance, float maxDistance, const Point3F& nearPoint);
    Point3F getHideLocation(const Point3F& targetPoint, float range, const Point3F& nearPoint, float hideLength);
    void setBlinded(int duration);
    void clientDetected(int targId);
    bool hasLOSToClient(int clientId, int& losTime, Point3F& lastLocation);
    void setDetectPeriod(int period) { mDetectHiddenPeriod = period; }
    int getDetectPeriod() const { return mDetectHiddenPeriod; }
    void setEvadeLocation(const Point3F& dangerLocation, int durationTicks = 0);
    void scriptSustainFire(int count) { mScriptTriggerCounter = count; }
    void pressFire(bool value = true) { mTriggers[FireTrigger] = value; }
    void pressJump() { mTriggers[JumpTrigger] = true; }
    void pressJet() { mTriggers[JetTrigger] = true; }
    void pressGrenade() { mTriggers[GrenadeTrigger] = true; }
    void pressMine() { mTriggers[MineTrigger] = true; }

    void clearStep();
    void setStep(const std::string& step);
    AIStep* step() const;
    const char* getStepStatus();
    const char* getStepName();
    void clearTasks();
    void addTask(const std::string& task);
    void removeTask(int id);
    void listTasks();
    AITask* currentTask() const;
    std::string currentTaskKey() const { return mCurrentTask; }
    int getTaskTime() const { return mCurrentTaskTime; }
    void missionCycleCleanup();
    int id() const;

    // The tick's view of the bot and its targets (initProcessVars).
    Point3F mLocation{0, 0, 0}, mVelocity{0, 0, 0}, mVelocity2D{0, 0, 0}, mRotation{0, 0, 0}, mHeadRotation{0, 0, 0};
    Point3F mMuzzlePosition{0, 0, 0}, mEyePosition{0, 0, 0};
    float mDamage = 0, mEnergy = 0, mEnergyReserve = 0, mEnergyFloat = 0.10f;
    bool mEnergyRecharge = false, mEnergyAvailable = false, mHeadingDownhill = false, mInWater = false,
         mOutdoors = false;
    float mDotOffCourse = 0;
    PlayerObject* mTargetPlayer = nullptr;
    Point3F mTargLocation{0, 0, 0}, mTargVelocity{0, 0, 0}, mTargVelocity2D{0, 0, 0}, mTargRotation{0, 0, 0};
    float mTargEnergy = 1.0f, mTargDamage = 0;
    bool mTargInWater = false;
    Point3F mObjectLocation{0, 0, 0};
    bool mObjectInWater = false;
    float mSkillLevel = 0.5f;
    int mTargStillTimeMS = 0;
    int mTargPrevTimeMS = 0;
    Point3F mTargPrevLocation[4]{};
    int mChangeWeaponCounter = 0;
    float mDistToTarg2D = -1.0f, mDistToNode2D = 0, mDistToObject2D = -1.0f;
    int mEngageMinDistance = 20, mEngageMaxDistance = 75;
    int mTriggerCounter = 0, mScriptTriggerCounter = -1;
    float mWeaponEnergy = 0.0f, mWeaponErrorFactor = 1.0f;
    bool mFiring = false;

private:
    enum EngageStates { ChooseWeapon, OutOfRange, ReloadWeapon, FindTargetPoint, AimAtTarget, FireWeapon, Finished };
    enum { FireTrigger = 0, JumpTrigger = 2, JetTrigger = 3, GrenadeTrigger = 4, MineTrigger = 5 };
    struct PlayerDetectionEntry {
        int playerId = 0;
        bool playerLOS = false;
        int playerLOSTime = 0;
        Point3F playerLastPosition{0, 0, 0};
    };

    void process(ShapeBase* ctrlObject);
    void initProcessVars(PlayerObject* player);
    void updateDetectionTable(PlayerObject* player);
    void scriptProcessEngagement();
    void scriptChooseEngageWeapon(float distToTarg);
    void scriptChooseObjectWeapon(float distToTarg);
    void processEngagement(PlayerObject* player);
    Point3F dopeAimLocation(const Point3F& startLocation, const Point3F& aimLocation);
    Point3F correctHeading();
    Point3F avoidPlayers(PlayerObject* player, const Point3F& desiredDestination, bool destIsFinal);
    void processVehicleMovement(PlayerObject* player);
    void processPilotVehicle(VehicleObject* vehicle);
    void processMovement(PlayerObject* player);
    void setPathCapabilities(PlayerObject* player);
    PlayerObject* controlPlayer() const;
    ScriptObject* projectileData() const;

    ClientMoveIn packMove() const;
    // Move (float form) built by getMoveList.
    float mMoveYaw = 0, mMovePitch = 0, mMoveX = 0, mMoveY = 0;
    float mMoveSpeed = 0.0f;
    int mMoveMode = ModeStop, mMoveModePending = ModeStop;
    float mMoveTolerance = 0.25f;
    Point3F mMoveDestination{0, 0, 0}, mNodeLocation{0, 0, 0}, mMoveLocation{0, 0, 0}, mEvadeLocation{0, 0, 0};
    Point3F mAimLocation{0, 0, 0};
    int mLookAtTargetTimeMS = 0;
    Point3F mPrevNodeLocation{0, 0, 0}, mInitialLocation{0, 0, 0};
    std::string mEngageTarget; // the GameConnection's key
    int mEngageState = ChooseWeapon, mStateCounter = 0, mDelayCounter = 0, mPackCheckCounter = 0;
    bool mAimAtLazedTarget = false;
    int mTurretMountedId = 0;
    Point3F mPilotDestination{0, 0, 0}, mPilotAimLocation{0, 0, 0};
    float mPilotSpeed = 1.0f, mPitchUpMax = -0.25f, mPitchDownMax = 0.1f, mPitchIncMax = 0.05f;
    float mCurrentPitch = 0, mPreviousPitch = 0, mDesiredPitch = 0, mPitchIncrement = 0;
    std::string mProjectileName;
    int mProjectileCounter = 0, mEvadingCounter = 0;
    bool mIsEvading = false;
    std::string mVictim, mCorpse;
    Point3F mCorpseLocation{0, 0, 0};
    int mVictimTime = 0;
    std::string mTargetObject; // the ShapeBase's key
    int mObjectMode = DestroyObject;
    float mRangeToTarget = 30;
    int mCheckTargetLOSCounter = 0;
    bool mTargetInRange = false, mTargetInSight = false;
    NavigationPath mPath;
    AIJetting mJetting;
    Point3F mPathDest{0, 0, 0};
    bool mNewPath = false, mNavUsingJet = false;
    const std::string* mMountedImage = nullptr;
    int mMountedImageId = -1;
    Point3F mStuckLocation{0, 0, 0}, mStuckDestination{0, 0, 0};
    bool mStuckInitialized = false;
    int mStuckTimer = 0;
    bool mStuckTryJump = false, mStuckJumpInitialized = false;
    int mStuckJumpTimer = 0;
    std::string mAvoidingObject;
    Point3F mAvoidSourcePoint{0, 0, 0}, mAvoidDestinationPoint{0, 0, 0}, mAvoidMovePoint{0, 0, 0};
    bool mAvoidForcedPath = false;
    bool mTriggers[6]{};
    std::string mStep;                // the AIStep's key
    std::vector<std::string> mTaskList; // AITask keys
    std::string mCurrentTask;
    int mCurrentTaskTime = 0;
    std::vector<PlayerDetectionEntry> mPlayerDetectionTable;
    int mPlayerDetectionIndex = 0, mPlayerDetectionCounter = 0;
    int mDetectHiddenPeriod = 6000, mBlindedTimer = 0;
};

// aiConnect, AIConnection:: methods, AITask methods, AISystemEnabled, the
// AI slicers; and each tick, every bot's move into its connection's queue.
void registerAINatives(class TorqueScript& ts);
