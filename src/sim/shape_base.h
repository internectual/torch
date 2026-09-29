#pragma once
// ShapeBase damage, repair, energy and presentation state (shapeBase.cc),
// and its mounted images (shapeImage.cc): the ShapeBaseImageData state
// machine as the server runs it.
#include "sim/game_base.h"
#include <string>
#include <vector>
#include <map>
#include <array>
#include <memory>

struct ScriptObject;

namespace DataBlockPack {
class Context;
// ShapeBaseImageData::offsetTransform from the "offset" and "rotation"
// persist fields (row-major, translation in column 3).
std::array<float, 16> imageOffsetTransform(const Context& c);
} // namespace DataBlockPack

// ShapeBaseImageData (game/shapeBase.h) as ShapeBaseImageData::onAdd builds
// its state[] array from the datablock's persist fields. Only what the
// server's state machine and transforms read; the shape-derived values
// (sequences, nodes, mountTransform's shape part) are not available on the
// server without the DTS.
struct ShapeBaseImageData {
    enum Constants { MaxStates = 31 };
    struct StateData {
        enum LoadedState { IgnoreLoaded, Loaded, NotLoaded };
        enum SpinState { IgnoreSpin, NoSpin, SpinUp, SpinDown, FullSpin };
        enum RecoilState { NoRecoil, LightRecoil, MediumRecoil, HeavyRecoil };
        std::string name; // empty: no state (the engine's NULL name)
        struct Transition {
            int loaded[2] = {-1, -1};  // NotLoaded/Loaded
            int ammo[2] = {-1, -1};    // NoAmmo/Ammo
            int target[2] = {-1, -1};  // NoTarget/Target
            int trigger[2] = {-1, -1}; // TriggerUp/Down
            int wet[2] = {-1, -1};     // NotWet/Wet
            int timeout = -1;
        } transition;
        bool ignoreLoadedForReady = false;
        bool fire = false;
        bool ejectShell = false;
        bool allowImageChange = true;
        bool scaleAnimation = true;
        bool direction = true;
        bool waitForTimeout = true;
        float timeoutValue = 0.0f;
        float energyDrain = 0.0f;
        LoadedState loaded = IgnoreLoaded;
        SpinState spin = IgnoreSpin;
        RecoilState recoil = NoRecoil;
        std::string script; // stateScript: a method on the image datablock
    };

    int id = 0;               // the datablock's SimObject id
    std::string className;
    int mountPoint = 0;
    std::array<float, 16> offsetTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::string shapeFile; // the image shape, for its mountPoint/muzzlePoint nodes
    float mass = 0.0f;
    bool usesEnergy = false;
    float minEnergy = 2.0f;
    bool accuFire = false;
    bool isSeeker = false;
    int fireState = -1; // the first state marked stateFire
    StateData state[MaxStates];

    // The datablock `name` (a name or id) when it is a ShapeBaseImageData
    // (or subclass) datablock, else nullptr.
    static std::shared_ptr<const ShapeBaseImageData> find(const std::string& name);
    // ShapeBaseImageData::lookupState: -1 for no name, 0 when unresolved.
    int lookupState(const std::string& name) const;
};

class ShapeBase : public GameBase {
public:
    enum DamageState { Enabled, Disabled, Destroyed };
    enum Constants { MaxMountedImages = 8, MaxTriggerKeys = 6 };
    enum ShapeBaseMasks : uint32_t {
        DamageMask = GameBase::NextFreeMask,
        NoWarpMask = GameBase::NextFreeMask << 1,
        MountedMask = GameBase::NextFreeMask << 2,
        CloakMask = GameBase::NextFreeMask << 3,
        ShieldMask = GameBase::NextFreeMask << 4,
        InvincibleMask = GameBase::NextFreeMask << 5,
        SoundMaskN = GameBase::NextFreeMask << 6,
        ThreadMaskN = SoundMaskN << 4,
        ImageMaskN = ThreadMaskN << 4,
        NextFreeMask = ImageMaskN << 8,
        SoundMask = (SoundMaskN << 4) - SoundMaskN,
        ThreadMask = (ThreadMaskN << 4) - ThreadMaskN,
        ImageMask = (ImageMaskN << 8) - ImageMaskN,
    };

    // ShapeBase::MountedImage, server half (no shape instance, threads,
    // sounds or emitters: the server never creates them).
    struct MountedImage {
        std::shared_ptr<const ShapeBaseImageData> dataBlock; // null: empty slot
        int state = 0; // index into dataBlock->state
        // nextImage: pending while the current state disallows image
        // changes (the engine's InvalidImagePtr is hasNext == false).
        bool hasNext = false;
        std::shared_ptr<const ShapeBaseImageData> nextImage;
        uint32_t skinTag = 0;
        uint32_t desiredTag = 0; // client-side only; always 0 on the server
        bool loaded = false;
        uint32_t nextTeam = 0;
        bool nextLoaded = false;
        float delayTime = 0.0f;
        uint32_t fireCount = 0;
        bool triggerDown = false;
        bool ammo = false;
        bool target = false;
        bool wet = false;
        const ShapeBaseImageData::StateData& stateData() const { return dataBlock->state[state]; }
    };
    MountedImage images[MaxMountedImages];

    float damage = 0.0f;
    float repairRate = 0.0f;
    float repairReserve = 0.0f;
    float rechargeRate = 0.0f;
    float energy = 0.0f;
    DamageState damageState = Enabled;
    float damageFlash = 0.0f;
    float whiteOut = 0.0f;
    float invincibleTime = 0.0f, invincibleSpeed = 0.0f;
    bool cloaked = false;
    bool passiveJammed = false;
    float heat = 0.0f;
    bool hidden = false;
    float cameraFov = 90.0f;
    bool trigger[MaxTriggerKeys]{}; // mTrigger: the last move's trigger states
    float waterCoverage = 0.0f;     // mWaterCoverage (0..1 of the object's box)

    float maxDamage() const { return dataFloat("maxDamage", 1.0f); }
    float maxEnergy() const { return dataFloat("maxEnergy", 0.0f); }
    bool invincible() const { return dataBool("isInvincible", false); }

    float getEnergyLevel() const { return energy; }
    float getEnergyValue() const;
    void setEnergyLevel(float level);
    void setDamageLevel(float level);
    // Server: the damage level's effect on the damage state (Player).
    virtual void updateDamageLevel() {}
    // mWorldBox: the shape's DTS bounds through the transform.
    virtual bool worldBox(float lo[3], float hi[3]) const;
    // queueCollision / notifyCollision: %data.onCollision(%obj, %col) on both
    // sides, at most once per CollisionTimeoutValue (250 ms) per pair.
    void queueCollision(const std::string& other);
    void notifyCollision();
    std::map<std::string, uint64_t> collisionTimeouts;
    std::vector<std::string> collisionsQueued;
    // mMount: what this object is mounted on (handle), its node, and the
    // objects mounted on this one (mMount.list order: newest first).
    std::string mount;
    int mountNode = 0;
    std::vector<std::string> mounted;
    void mountObject(ShapeBase& object, int node);
    void unmountObject(ShapeBase& object);
    void unmount();
    // A mounted object follows its mount's mount point.
    void followMount();
    bool fading = false, fadeOut = false;
    float fadeTime = 0, fadeDelay = 0, fadeElapsedTime = 0, fadeVal = 1.0f;
    void startFade(float time, float delay, bool out);
    void applyDamage(float amount) { if (amount > 0) setDamageLevel(damage + amount); }
    void applyRepair(float amount);
    float getDamageValue() const;
    void setDamageState(DamageState state);
    bool setDamageState(const std::string& name);
    const char* damageStateName() const;

    // Mounted images (shapeImage.cc).
    bool mountImage(std::shared_ptr<const ShapeBaseImageData> image, uint32_t slot, bool loaded, uint32_t skinTag);
    bool unmountImage(uint32_t slot);
    const ShapeBaseImageData* getMountedImage(uint32_t slot) const { return images[slot].dataBlock.get(); }
    const ShapeBaseImageData* getPendingImage(uint32_t slot) const;
    bool isImageFiring(uint32_t slot) const;
    bool isImageReady(uint32_t slot, int ns = -1, uint32_t depth = 0) const;
    bool isImageMounted(int imageId) const;
    int getMountSlot(int imageId) const;
    uint32_t getImageSkinTag(uint32_t slot) const;
    const char* getImageState(uint32_t slot) const;
    void setImageTriggerState(uint32_t slot, bool trigger);
    bool getImageTriggerState(uint32_t slot) const;
    void setImageAmmoState(uint32_t slot, bool ammo);
    bool getImageAmmoState(uint32_t slot) const;
    void setImageTargetState(uint32_t slot, bool target);
    bool getImageTargetState(uint32_t slot) const;
    void setImageWetState(uint32_t slot, bool wet);
    bool getImageWetState(uint32_t slot) const;
    void setImageLoadedState(uint32_t slot, bool loaded);
    bool getImageLoadedState(uint32_t slot) const;
    void setImageState(uint32_t slot, uint32_t state, bool force = false);
    void updateImageState(uint32_t slot, float dt);
    uint32_t getImageFireState(uint32_t slot) const;

    // Transforms (row-major, translation in column 3). See shape_base.cpp
    // for where these approximate the engine's DTS node transforms.
    std::array<float, 16> getMountTransform(uint32_t mountPoint) const;
    std::array<float, 16> getImageTransform(uint32_t slot) const;
    std::array<float, 16> getMuzzleTransform(uint32_t slot) const;
    std::array<float, 16> getEyeTransform() const;
    void getMuzzleVector(uint32_t slot, float vec[3]) const;
    void getMuzzlePoint(uint32_t slot, float pos[3]) const;

    void processMove(const ClientMoveIn* move) override;
    void processShapeTick(); // energy and repair
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;

private:
    void setImage(uint32_t slot, std::shared_ptr<const ShapeBaseImageData> image, uint32_t skinTag, bool loaded,
                  bool ammo = false, bool triggerDown = false, bool target = false);
    void resetImageSlot(uint32_t slot);
    void scriptCallback(uint32_t slot, const std::string& function);
    bool getCorrectedAim(const std::array<float, 16>& muzzle, float result[3]) const;
};

void registerShapeBaseNatives(class TorqueScript& ts);
