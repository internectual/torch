#include "sim/shape_base.h"
#include "sim/datablock_pack.h"
#include "sim/engine_classes.h"
#include "sim/game_connection.h"
#include "sim/nav_graph.h"
#include "sim/net_string_table.h"
#include "sim/player.h"
#include "sim/torque_math.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <strings.h>

static const char* sDamageStateName[] = {"Enabled", "Disabled", "Destroyed"};

float ShapeBase::getEnergyValue() const {
    const float max = maxEnergy();
    return max > 0.0f ? energy / max : 0.0f;
}

void ShapeBase::setEnergyLevel(float level) {
    if (damageState == Enabled) energy = std::clamp(level, 0.0f, std::max(0.0f, maxEnergy()));
}

void ShapeBase::setDamageLevel(float level) {
    if (invincible() || level == damage) return;
    damage = std::clamp(level, 0.0f, maxDamage());
    callDataBlock("onDamage");
}

void ShapeBase::applyRepair(float amount) {
    if (amount > 0 && (repairReserve += amount) > damage) repairReserve = damage;
}

float ShapeBase::getDamageValue() const {
    const float max = maxDamage();
    return max > 0.0f ? damage / max : 0.0f;
}

const char* ShapeBase::damageStateName() const { return sDamageStateName[damageState]; }

void ShapeBase::setDamageState(DamageState state) {
    if (damageState == state) return;
    const char* callback = nullptr;
    const std::string lastState = damageStateName();
    switch (state) {
        case Destroyed:
            if (damageState == Enabled) setDamageState(Disabled);
            callback = "onDestroyed";
            break;
        case Disabled:
            if (damageState == Enabled) callback = "onDisabled";
            break;
        case Enabled:
            callback = "onEnabled";
            break;
    }
    damageState = state;
    if (damageState != Enabled) {
        repairReserve = 0;
        energy = 0;
    }
    if (callback) callDataBlock(callback, {lastState});
}

bool ShapeBase::setDamageState(const std::string& name) {
    for (int i = 0; i < 3; ++i)
        if (strcasecmp(name.c_str(), sDamageStateName[i]) == 0) {
            setDamageState(DamageState(i));
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// ShapeBaseImageData (shapeImage.cc): onAdd's state[] from the persist fields.

namespace {

constexpr float TickSec = 0.032f; // TickMs / 1000
using Matrix = std::array<float, 16>;
const Matrix kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

std::string indexed(const char* base, int index) {
    return std::string(base) + "[" + std::to_string(index) + "]";
}

// The datablock object `name` names when it is a ShapeBaseImageData (the
// engine's Sim::findObject(argv, ShapeBaseImageData*)).
ScriptObject* imageDataBlockObject(const std::string& name) {
    if (name.empty()) return nullptr;
    ScriptObject* object = ScriptEngine::instance().findObject(name.c_str());
    if (!object || !object->internals.count("__datablockKey") ||
        !EngineClasses::isA(object->className, "ShapeBaseImageData"))
        return nullptr;
    return object;
}

int imageDataBlockId(const std::string& name) {
    ScriptObject* object = imageDataBlockObject(name);
    return object ? ScriptEngine::instance().objectId(object) : 0;
}

// MatrixF::set(EulerF(x, 0, 0)) and set(EulerF(0, 0, z)) (m_matF_set_euler).
Matrix rotX(float x) {
    const float c = std::cos(x), s = std::sin(x);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}
Matrix rotZ(float z) {
    const float c = std::cos(z), s = std::sin(z);
    return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// APPROXIMATION (no DTS on the server): the height of a Player's "eye" node
// above its origin. The engine reads it from the animated shape
// (mShapeInstance->mNodeTransforms[eyeNode]); Torch uses 90% of the
// PlayerData boxSize height (light armor 2.3 -> 2.07, close to the retail
// armors' eye nodes). Other ShapeBase classes: 0 (the engine's fallback to
// the object transform when the shape has no such node).
float boxHeight(const ShapeBase& shape) {
    ScriptObject* data = ScriptEngine::instance().findObject(shape.dataBlock().c_str());
    return Fields::point(data, "boxSize", {1, 1, 2})[2]; // PlayerData's default
}
float eyeHeight(const ShapeBase& shape) {
    return dynamic_cast<const PlayerObject*>(&shape) ? boxHeight(shape) * 0.9f : 0.0f;
}

} // namespace

int ShapeBaseImageData::lookupState(const std::string& name) const {
    if (name.empty()) return -1;
    for (int i = 0; i < MaxStates; ++i)
        if (!state[i].name.empty() && strcasecmp(name.c_str(), state[i].name.c_str()) == 0) return i;
    Console::instance().printf(LogLevel::Error, "ShapeBaseImageData:: Could not resolve state \"%s\" for image \"%s\"",
                               name.c_str(), std::to_string(id).c_str());
    return 0;
}

std::shared_ptr<const ShapeBaseImageData> ShapeBaseImageData::find(const std::string& name) {
    ScriptObject* object = imageDataBlockObject(name);
    if (!object) return nullptr;
    auto data = std::make_shared<ShapeBaseImageData>();
    data->id = ScriptEngine::instance().objectId(object);
    data->className = object->className;
    // The datablock packer's field conventions (console type parsing and
    // the engine constructor's defaults).
    TorqueBitWriter unused;
    const DataBlockPack::Context c(object, unused, nullptr);
    data->mountPoint = c.s32("mountPoint", 0);
    data->offsetTransform = DataBlockPack::imageOffsetTransform(c);
    data->mass = c.f32("mass", 0.0f);
    data->usesEnergy = c.boolean("usesEnergy", false);
    data->minEnergy = c.f32("minEnergy", 2.0f);
    data->accuFire = c.boolean("accuFire", false);
    data->isSeeker = c.boolean("isSeeker", false);
    for (int i = 0; i < MaxStates; ++i) data->state[i].name = c.str(indexed("stateName", i).c_str());
    for (int i = 0; i < MaxStates; ++i) {
        StateData& s = data->state[i];
        auto field = [i](const char* base) { return indexed(base, i); };
        auto lookup = [&](const char* base) { return data->lookupState(c.str(field(base).c_str())); };
        s.transition.loaded[0] = lookup("stateTransitionOnNotLoaded");
        s.transition.loaded[1] = lookup("stateTransitionOnLoaded");
        s.transition.ammo[0] = lookup("stateTransitionOnNoAmmo");
        s.transition.ammo[1] = lookup("stateTransitionOnAmmo");
        s.transition.target[0] = lookup("stateTransitionOnNoTarget");
        s.transition.target[1] = lookup("stateTransitionOnTarget");
        s.transition.wet[0] = lookup("stateTransitionOnNotWet");
        s.transition.wet[1] = lookup("stateTransitionOnWet");
        s.transition.trigger[0] = lookup("stateTransitionOnTriggerUp");
        s.transition.trigger[1] = lookup("stateTransitionOnTriggerDown");
        s.transition.timeout = lookup("stateTransitionOnTimeout");
        s.waitForTimeout = c.boolean(field("stateWaitForTimeout").c_str(), true);
        s.timeoutValue = c.f32(field("stateTimeoutValue").c_str(), 0.0f);
        s.fire = c.boolean(field("stateFire").c_str(), false);
        s.ejectShell = c.boolean(field("stateEjectShell").c_str(), false);
        s.energyDrain = c.f32(field("stateEnergyDrain").c_str(), 0.0f);
        s.allowImageChange = c.boolean(field("stateAllowImageChange").c_str(), true);
        s.scaleAnimation = true; // stateScaleAnimation is not a retail persist field
        s.direction = c.boolean(field("stateDirection").c_str(), true);
        s.loaded = (StateData::LoadedState)c.enumValue(field("stateLoadedFlag").c_str(), {"Ignore", "Loaded", "Empty"}, 0);
        s.spin = (StateData::SpinState)c.enumValue(field("stateSpinThread").c_str(),
                                                   {"Ignore", "Stop", "SpinUp", "SpinDown", "FullSpeed"}, 0);
        s.recoil = (StateData::RecoilState)c.enumValue(field("stateRecoil").c_str(),
                                                       {"NoRecoil", "LightRecoil", "MediumRecoil", "HeavyRecoil"}, 0);
        s.script = c.str(field("stateScript").c_str());
        s.ignoreLoadedForReady = c.boolean(field("stateIgnoreLoadedForReady").c_str(), false);
        // The first state marked as "fire" is the state entered on the
        // client when it receives a fire event.
        if (s.fire && data->fireState == -1) data->fireState = i;
    }
    return data;
}

// ---------------------------------------------------------------------------
// ShapeBase mounted images (shapeImage.cc), server side: isGhost() is false
// throughout, so the client-only branches (shape instances, animation, spin
// and flash threads, sounds, emitters, shell casings) are not here.

bool ShapeBase::mountImage(std::shared_ptr<const ShapeBaseImageData> imageData, uint32_t slot, bool loaded,
                           uint32_t skinTag) {
    MountedImage& image = images[slot];
    if (image.dataBlock && imageData && image.dataBlock->id == imageData->id && image.desiredTag == skinTag) {
        // Image already loaded
        image.hasNext = false;
        image.nextImage = nullptr;
        return true;
    }
    setImage(slot, std::move(imageData), skinTag, loaded);
    return true;
}

bool ShapeBase::unmountImage(uint32_t slot) {
    if (!images[slot].dataBlock) return false;
    setImage(slot, nullptr, 0, false);
    return true;
}

const ShapeBaseImageData* ShapeBase::getPendingImage(uint32_t slot) const {
    return images[slot].hasNext ? images[slot].nextImage.get() : nullptr;
}

bool ShapeBase::isImageFiring(uint32_t slot) const {
    const MountedImage& image = images[slot];
    return image.dataBlock && image.stateData().fire;
}

bool ShapeBase::isImageReady(uint32_t slot, int ns, uint32_t depth) const {
    // Will pressing the trigger lead to a fire state?
    const MountedImage& image = images[slot];
    if (depth++ > 5 || !image.dataBlock) return false;
    const ShapeBaseImageData::StateData& stateData = ns == -1 ? image.stateData() : image.dataBlock->state[ns];
    if (stateData.fire) return true;
    const auto& t = stateData.transition;
    if ((ns = t.loaded[stateData.ignoreLoadedForReady ? 1 : image.loaded]) != -1 && isImageReady(slot, ns, depth))
        return true;
    if ((ns = t.ammo[image.ammo]) != -1 && isImageReady(slot, ns, depth)) return true;
    if ((ns = t.target[image.target]) != -1 && isImageReady(slot, ns, depth)) return true;
    if ((ns = t.wet[image.wet]) != -1 && isImageReady(slot, ns, depth)) return true;
    if ((ns = t.trigger[1]) != -1 && isImageReady(slot, ns, depth)) return true;
    if ((ns = t.timeout) != -1 && isImageReady(slot, ns, depth)) return true;
    return false;
}

bool ShapeBase::isImageMounted(int imageId) const {
    return getMountSlot(imageId) != -1;
}

int ShapeBase::getMountSlot(int imageId) const {
    for (int i = 0; i < MaxMountedImages; ++i)
        if (images[i].dataBlock && images[i].dataBlock->id == imageId) return i;
    return -1;
}

uint32_t ShapeBase::getImageSkinTag(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].skinTag : 0;
}

const char* ShapeBase::getImageState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].stateData().name.c_str() : nullptr;
}

void ShapeBase::setImageAmmoState(uint32_t slot, bool ammo) {
    MountedImage& image = images[slot];
    if (image.dataBlock && !image.dataBlock->usesEnergy && image.ammo != ammo) {
        setMaskBits(ImageMaskN << slot);
        image.ammo = ammo;
    }
}

bool ShapeBase::getImageAmmoState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].ammo : false;
}

void ShapeBase::setImageTargetState(uint32_t slot, bool target) {
    MountedImage& image = images[slot];
    if (image.dataBlock && image.target != target) {
        setMaskBits(ImageMaskN << slot);
        image.target = target;
    }
}

bool ShapeBase::getImageTargetState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].target : false;
}

void ShapeBase::setImageWetState(uint32_t slot, bool wet) {
    MountedImage& image = images[slot];
    if (image.dataBlock && image.wet != wet) {
        setMaskBits(ImageMaskN << slot);
        image.wet = wet;
    }
}

bool ShapeBase::getImageWetState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].wet : false;
}

void ShapeBase::setImageLoadedState(uint32_t slot, bool loaded) {
    MountedImage& image = images[slot];
    if (image.dataBlock && image.loaded != loaded) {
        setMaskBits(ImageMaskN << slot);
        image.loaded = loaded;
    }
}

bool ShapeBase::getImageLoadedState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].loaded : false;
}

// Con::executef(image.dataBlock, 3, function, scriptThis(), slot):
// %imageData.function(%obj, %slot).
void ShapeBase::scriptCallback(uint32_t slot, const std::string& function) {
    const MountedImage& image = images[slot];
    auto* ts = ScriptEngine::instance().ts();
    if (!image.dataBlock || !ts || function.empty()) return;
    ts->callObjectMethod(std::to_string(image.dataBlock->id), function,
                         {VMValue(handle()), VMValue(std::to_string(slot))});
}

void ShapeBase::setImage(uint32_t slot, std::shared_ptr<const ShapeBaseImageData> imageData, uint32_t skinTag,
                         bool loaded, bool ammo, bool triggerDown, bool target) {
    MountedImage& image = images[slot];
    const int currentId = image.dataBlock ? image.dataBlock->id : 0;
    const int newId = imageData ? imageData->id : 0;
    if (currentId == newId) {
        image.hasNext = false;
        image.nextImage = nullptr;
        if (image.skinTag != skinTag) {
            setMaskBits(ImageMaskN << slot);
            image.skinTag = skinTag;
        }
        return;
    }

    // Delay image changes until these states are through
    if (imageData && image.dataBlock && !image.stateData().allowImageChange) {
        image.hasNext = true;
        image.nextImage = std::move(imageData);
        image.nextTeam = skinTag;
        image.nextLoaded = loaded;
        return;
    }
    setMaskBits(ImageMaskN << slot);

    // Notify script unmount
    if (image.dataBlock) scriptCallback(slot, "onUnmount");

    // No new type, just unselecting the current item
    if (!imageData) {
        resetImageSlot(slot);
        return;
    }

    // Init new shape
    resetImageSlot(slot);
    image.dataBlock = std::move(imageData);
    image.state = 0;
    image.skinTag = skinTag;
    image.loaded = loaded;
    image.ammo = ammo;
    image.triggerDown = triggerDown;
    image.target = target;
    setImageState(slot, 0, true);
    // updateMass: not ported (Torch's player physics takes its mass from
    // the PlayerData alone).

    // Notify script mount
    scriptCallback(slot, "onMount");
}

void ShapeBase::resetImageSlot(uint32_t slot) {
    // Clear out current image. fireCount and wet are left alone, as in the
    // engine.
    MountedImage& image = images[slot];
    image.dataBlock = nullptr;
    image.hasNext = false;
    image.nextImage = nullptr;
    image.skinTag = 0;
    image.nextTeam = 0;
    image.state = 0;
    image.delayTime = 0;
    image.ammo = false;
    image.triggerDown = false;
    image.loaded = false;
}

bool ShapeBase::getImageTriggerState(uint32_t slot) const {
    return images[slot].dataBlock ? images[slot].triggerDown : false;
}

void ShapeBase::setImageTriggerState(uint32_t slot, bool trigger) {
    MountedImage& image = images[slot];
    if (!image.dataBlock) return;
    if (trigger) {
        if (!image.triggerDown) {
            image.triggerDown = true;
            setMaskBits(ImageMaskN << slot);
            updateImageState(slot, 0);
        }
    } else if (image.triggerDown) {
        image.triggerDown = false;
        setMaskBits(ImageMaskN << slot);
        updateImageState(slot, 0);
    }
}

uint32_t ShapeBase::getImageFireState(uint32_t slot) const {
    // If there is no fire state, then try state 0
    const MountedImage& image = images[slot];
    if (image.dataBlock && image.dataBlock->fireState != -1) return (uint32_t)image.dataBlock->fireState;
    return 0;
}

void ShapeBase::setImageState(uint32_t slot, uint32_t newState, bool force) {
    MountedImage& image = images[slot];
    if (!image.dataBlock || newState >= (uint32_t)ShapeBaseImageData::MaxStates) return;
    // Scripts called below may remount or unmount the slot; the state data
    // stays with this datablock.
    const std::shared_ptr<const ShapeBaseImageData> imageData = image.dataBlock;

    // If going back into the same state, just reset the timer and invoke
    // the script callback
    if (!force && image.state == (int)newState) {
        image.delayTime = image.stateData().timeoutValue;
        if (!image.stateData().script.empty()) scriptCallback(slot, image.stateData().script);
        return;
    }

    image.state = (int)newState;
    const ShapeBaseImageData::StateData& stateData = imageData->state[newState];

    // Mount pending images
    if (image.hasNext && stateData.allowImageChange) {
        setImage(slot, image.nextImage, image.nextTeam, image.nextLoaded);
        return;
    }

    // Check for immediate transitions
    int ns;
    if ((ns = stateData.transition.loaded[image.loaded]) != -1) { setImageState(slot, ns); return; }
    if ((ns = stateData.transition.ammo[image.ammo]) != -1) { setImageState(slot, ns); return; }
    if ((ns = stateData.transition.target[image.target]) != -1) { setImageState(slot, ns); return; }
    if ((ns = stateData.transition.wet[image.wet]) != -1) { setImageState(slot, ns); return; }
    if ((ns = stateData.transition.trigger[image.triggerDown]) != -1) { setImageState(slot, ns); return; }

    // Initialize the new state...
    image.delayTime = stateData.timeoutValue;
    if (stateData.loaded != ShapeBaseImageData::StateData::IgnoreLoaded)
        image.loaded = stateData.loaded == ShapeBaseImageData::StateData::Loaded;
    if ((int)newState == imageData->fireState) {
        setMaskBits(ImageMaskN << slot);
        image.fireCount = (image.fireCount + 1) & 0x7;
    }
    // Recoil (onImageRecoil) only drives the Player's recoil animation
    // thread; sounds, sequences and emitters play on the client. The spin
    // thread's timeout scaling applies only where a spin thread exists,
    // which is on the client.

    // Script callback on server
    if (!stateData.script.empty()) scriptCallback(slot, stateData.script);

    // If there is a zero timeout, and a timeout transition, then go ahead
    // and transition immediately.
    if (!image.delayTime && (ns = stateData.transition.timeout) != -1) setImageState(slot, ns);
}

void ShapeBase::updateImageState(uint32_t slot, float dt) {
    MountedImage& image = images[slot];
    if (!image.dataBlock) return;
    const std::shared_ptr<const ShapeBaseImageData> imageData = image.dataBlock;
    const ShapeBaseImageData::StateData& stateData = image.stateData();
    image.delayTime -= dt;

    // Energy management
    if (imageData->usesEnergy) {
        const float newEnergy = std::max(0.0f, getEnergyLevel() - stateData.energyDrain * dt);
        setEnergyLevel(newEnergy);
        const bool ammo = newEnergy > imageData->minEnergy;
        if (ammo != image.ammo) {
            setMaskBits(ImageMaskN << slot);
            image.ammo = ammo;
        }
    }

    // Check for transitions. On some states we must wait for the full
    // timeout value before moving on.
    int ns;
    if (image.delayTime <= 0 || !stateData.waitForTimeout) {
        if ((ns = stateData.transition.loaded[image.loaded]) != -1) { setImageState(slot, ns); return; }
        if ((ns = stateData.transition.ammo[image.ammo]) != -1) { setImageState(slot, ns); return; }
        if ((ns = stateData.transition.target[image.target]) != -1) { setImageState(slot, ns); return; }
        if ((ns = stateData.transition.wet[image.wet]) != -1) { setImageState(slot, ns); return; }
        if ((ns = stateData.transition.trigger[image.triggerDown]) != -1) { setImageState(slot, ns); return; }
        if (image.delayTime <= 0 && (ns = stateData.transition.timeout) != -1) { setImageState(slot, ns); return; }
    }
    // The spin thread's time scale: client only.
}

// ---------------------------------------------------------------------------
// Image transforms. The engine composes DTS node transforms the server does
// not have (Torch loads no shapes on the server):
//   mount:  mObjToWorld * mShapeInstance->mNodeTransforms[mountPointNode[n]]
//   image:  mount * (offsetTransform * inverse(image shape "mountPoint" node))
//   muzzle: image * image shapeInstance->mNodeTransforms[muzzleNode]
//   eye:    mObjToWorld * mNodeTransforms[eyeNode]
// Each APPROXIMATION below says what stands in for the missing node.

// APPROXIMATION: the mount node is taken at the object's origin, raised to
// eyeHeight() for a Player (so weapon muzzles sit near the eye, not the
// feet); the engine's fallback when the shape has no mount node is the
// object transform itself.
std::array<float, 16> ShapeBase::getMountTransform(uint32_t mountPoint) const {
    (void)mountPoint;
    Matrix m = transform;
    const float h = eyeHeight(*this);
    m[3] += m[2] * h;
    m[7] += m[6] * h;
    m[11] += m[10] * h;
    return m;
}

// APPROXIMATION: the image's mountTransform is its offsetTransform (from the
// "offset"/"rotation" fields, exact) without the inverse of the image
// shape's "mountPoint" node.
std::array<float, 16> ShapeBase::getImageTransform(uint32_t slot) const {
    const MountedImage& image = images[slot];
    if (!image.dataBlock) return transform;
    return TorqueMath::mul(getMountTransform((uint32_t)image.dataBlock->mountPoint), image.dataBlock->offsetTransform);
}

// ShapeBase::getMuzzleTransform, with Player::getMuzzleTransform's
// orientation for players.
// APPROXIMATION: the image's "muzzlePoint" node is taken at the image's
// origin (the engine's own result when the image shape has no muzzle node).
// Player: the engine first pulls the muzzle back to where a ray from the
// image origin to the "retractionPoint" node hits a wall; with neither node
// known both points coincide, so that step is skipped. The orientation is
// exact: the player's transform pitched by its head pitch (Player's
// standard-animation branch; Torch's server player is always in one).
std::array<float, 16> ShapeBase::getMuzzleTransform(uint32_t slot) const {
    const Matrix nmat = getImageTransform(slot);
    if (!images[slot].dataBlock) return transform;
    if (auto* player = dynamic_cast<const PlayerObject*>(this)) {
        Matrix mat = TorqueMath::mul(transform, rotX(player->state.headPitch));
        mat[3] = nmat[3];
        mat[7] = nmat[7];
        mat[11] = nmat[11];
        return mat;
    }
    return nmat;
}

// ShapeBase::getEyeTransform / Player::getEyeTransform.
// APPROXIMATION: the eye node's position is (0, 0, eyeHeight()); a Player's
// rotation is exact (head yaw, then head pitch). Other classes: the object
// transform (the engine's result when the shape has no "eye" node).
std::array<float, 16> ShapeBase::getEyeTransform() const {
    if (auto* player = dynamic_cast<const PlayerObject*>(this)) {
        Matrix pmat = TorqueMath::mul(rotZ(player->state.headYaw), rotX(player->state.headPitch));
        pmat[3] = 0;
        pmat[7] = 0;
        pmat[11] = eyeHeight(*this);
        return TorqueMath::mul(transform, pmat);
    }
    return transform;
}

// ShapeBase::getCorrectedAim: aim the muzzle at what is 500 m straight ahead
// of the eye. APPROXIMATION: the ray is cast against the server's world
// geometry (terrain, interiors) only; objects are not in Torch's server
// container yet.
bool ShapeBase::getCorrectedAim(const std::array<float, 16>& muzzleMat, float result[3]) const {
    const float pullInD = 6.0f, maxAdjD = 500.0f;
    const Matrix eyeMat = getEyeTransform();
    const float eyePos[3] = {eyeMat[3], eyeMat[7], eyeMat[11]};
    const float ahead[3] = {0, maxAdjD, 0};
    float aheadVec[3];
    TorqueMath::mulV(eyeMat, ahead, aheadVec);
    const float aheadPoint[3] = {eyePos[0] + aheadVec[0], eyePos[1] + aheadVec[1], eyePos[2] + aheadVec[2]};
    const float muzzlePos[3] = {muzzleMat[3], muzzleMat[7], muzzleMat[11]};
    float collidePoint[3] = {aheadPoint[0], aheadPoint[1], aheadPoint[2]};
    Nav::RayHit hit;
    if (Nav::castRay({eyePos[0], eyePos[1], eyePos[2]}, {aheadPoint[0], aheadPoint[1], aheadPoint[2]}, 0xFFFFFFFFu, hit)) {
        collidePoint[0] = hit.point.x;
        collidePoint[1] = hit.point.y;
        collidePoint[2] = hit.point.z;
    }
    float collideVector[3] = {collidePoint[0] - eyePos[0], collidePoint[1] - eyePos[1], collidePoint[2] - eyePos[2]};
    // For close collision we want to NOT aim at ground since we're bending
    // the ray here as it is. But we don't want to pop, so adjust continuously.
    float lenSq = collideVector[0] * collideVector[0] + collideVector[1] * collideVector[1] +
                  collideVector[2] * collideVector[2];
    if (lenSq < pullInD * pullInD && lenSq > 0.04f) {
        const float scale = pullInD / std::sqrt(lenSq);
        for (int i = 0; i < 3; ++i) {
            collideVector[i] *= scale;
            collidePoint[i] = eyePos[i] + collideVector[i];
        }
    }
    float muzzleToCollide[3] = {collidePoint[0] - muzzlePos[0], collidePoint[1] - muzzlePos[1],
                                collidePoint[2] - muzzlePos[2]};
    lenSq = muzzleToCollide[0] * muzzleToCollide[0] + muzzleToCollide[1] * muzzleToCollide[1] +
            muzzleToCollide[2] * muzzleToCollide[2];
    if (lenSq > 0.04f) {
        const float inv = 1.0f / std::sqrt(lenSq);
        for (int i = 0; i < 3; ++i) result[i] = muzzleToCollide[i] * inv;
        return true;
    }
    return false;
}

void ShapeBase::getMuzzleVector(uint32_t slot, float vec[3]) const {
    const Matrix mat = getMuzzleTransform(slot);
    // A first-person human client aims at what its eye sees.
    if (!controllingClient.empty())
        if (auto* connection = EngineObjects::get<GameConnection>(controllingClient))
            if (!(connection->script && EngineClasses::isA(connection->script->className, "AIConnection")) &&
                connection->firstPerson && getCorrectedAim(mat, vec))
                return;
    const float forward[3] = {0, 1, 0};
    TorqueMath::mulV(mat, forward, vec);
}

void ShapeBase::getMuzzlePoint(uint32_t slot, float pos[3]) const {
    const Matrix mat = getMuzzleTransform(slot);
    pos[0] = mat[3];
    pos[1] = mat[7];
    pos[2] = mat[11];
}

// ShapeBase::processTick, server side: energy and repair, the wet and
// seeker target states, the images, then the onTrigger callbacks; for a
// Player, Player::updateMove's image triggers.
void ShapeBase::processMove(const ClientMoveIn* move) {
    processShapeTick();
    auto* player = dynamic_cast<PlayerObject*>(this);
    if (player) {
        // mWaterCoverage (updateContainer, from the last tick's position):
        // the fraction of the box under the water surface.
        const auto& water = serverCollision().water;
        const float surface = water ? water(transform[3], transform[7]) : std::numeric_limits<float>::quiet_NaN();
        const float height = boxHeight(*this);
        waterCoverage = std::isfinite(surface) && height > 0
            ? std::clamp((surface - transform[11]) / height, 0.0f, 1.0f) : 0.0f;
    }
    // update wet state
    setImageWetState(0, waterCoverage > 0.4f); // more than 40 percent covered
    if (waterCoverage < 0.4f && images[0].dataBlock && images[0].dataBlock->isSeeker && move) {
        // thinkAboutLocking is not ported (no server container of heat
        // sources): there are never potential targets, so the seeker is
        // neither tracking nor locked.
        setImageWetState(0, false);
        setImageTargetState(0, false);
    }

    // Advance images
    for (uint32_t i = 0; i < MaxMountedImages; ++i)
        if (images[i].dataBlock) updateImageState(i, TickSec);

    // Script on trigger state changes: %data.onTrigger(%obj, %trigger, %state).
    if (move)
        for (int i = 0; i < MaxTriggerKeys; ++i)
            if (move->trigger[i] != trigger[i]) {
                trigger[i] = move->trigger[i];
                callDataBlock("onTrigger", {std::to_string(i), trigger[i] ? "1" : "0"});
            }

    // Player::updateMove (NullMove when there is no move): the fire and
    // alternate triggers drive image slots 0 and 1.
    if (player && damageState == Enabled) {
        setImageTriggerState(0, move && move->trigger[0]);
        setImageTriggerState(1, move && move->trigger[1]);
    }
}

void ShapeBase::processShapeTick() {
    // Energy management
    if (damageState == Enabled && !dataBool("inheritEnergyFromMount", false))
        energy = std::clamp(energy + rechargeRate, 0.0f, std::max(0.0f, maxEnergy()));
    // Repair management
    if (!invincible()) {
        const float store = damage;
        damage = std::clamp(damage - repairRate, 0.0f, maxDamage());
        if (repairReserve > damage) repairReserve = damage;
        if (repairReserve > 0.0f) {
            const float rate = std::min(dataFloat("repairRate", 0.0033f), repairReserve);
            damage -= rate;
            repairReserve -= rate;
        }
        if (store != damage) callDataBlock("onDamage");
    }
}

// Retail ShapeBase::packUpdate, in the order the client reads it: damage,
// sounds, threads, images, cloak/shield/invincibility, mount.
uint32_t ShapeBase::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    if (mask & InitialUpdateMask) {
        // No sound plays and no script thread runs yet; mask off images
        // that aren't mounted.
        mask &= ~(SoundMask | ThreadMask);
        for (uint32_t i = 0; i < MaxMountedImages; ++i)
            if (!images[i].dataBlock) mask &= ~(ImageMaskN << i);
    }
    if (!w.writeFlag(mask & (DamageMask | SoundMask | ThreadMask | ImageMask | CloakMask | MountedMask |
                             InvincibleMask | ShieldMask)))
        return ret;
    if (w.writeFlag(mask & DamageMask)) {
        const float max = maxDamage();
        w.writeFloat(std::clamp(max > 0.0f ? damage / max : 0.0f, 0.0f, 1.0f), 6);
        w.writeInt((int32_t)damageState, 2);
        w.writeFlag(false); // blowApart
        w.writeNormalVector({0, 0, 1}, 8); // damageDir
    }
    if (w.writeFlag(mask & SoundMask))
        for (int i = 0; i < 4; ++i) w.writeFlag(false);
    if (w.writeFlag(mask & ThreadMask))
        for (int i = 0; i < 4; ++i) w.writeFlag(false);
    if (w.writeFlag(mask & ImageMask))
        for (uint32_t i = 0; i < MaxMountedImages; ++i) {
            if (!w.writeFlag(mask & (ImageMaskN << i))) continue;
            const MountedImage& image = images[i];
            if (w.writeFlag(image.dataBlock != nullptr))
                w.writeInt(image.dataBlock->id - (int)DataBlockPack::ObjectIdFirst, 11);
            // NetConnection::packNetStringHandleU: a valid handle, then its
            // id when the client has it, else the string. The connection's
            // sent-string state is private to it, so the text goes along;
            // the client reads both forms.
            if (w.writeFlag(image.skinTag != 0)) {
                w.writeFlag(false);
                const std::string* text = NetStrings::lookup(image.skinTag);
                w.writeString(text ? *text : std::string());
            }
            // The retail order (V12 writes wet, ammo, loaded, target, trigger).
            w.writeFlag(image.triggerDown);
            w.writeFlag(image.loaded);
            w.writeFlag(image.ammo);
            w.writeFlag(image.wet);
            w.writeFlag(image.target);
            w.writeInt((int32_t)image.fireCount, 3);
            if (mask & InitialUpdateMask) w.writeFlag(isImageFiring(i));
        }
    if (w.writeFlag(mask & (ShieldMask | CloakMask | InvincibleMask))) {
        if (w.writeFlag(mask & CloakMask)) {
            w.writeFlag(cloaked);
            w.writeFlag(!controllingClient.empty());
            w.writeFlag(false); // not fading
            w.writeFlag(true);  // mFadeVal == 1
        }
        if (w.writeFlag(mask & ShieldMask)) {
            w.writeFlag(false);
            w.writeNormalVector({0, 0, 1}, 8);
            w.writeFloat(getEnergyValue(), 5);
        }
        if (w.writeFlag(mask & InvincibleMask)) {
            w.writeF32(invincibleTime);
            w.writeF32(invincibleSpeed);
        }
    }
    // Not mounted: an unmount unless this is the initial update.
    if (w.writeFlag((mask & MountedMask) && !(mask & InitialUpdateMask))) w.writeFlag(false);
    return ret;
}

bool ShapeBase::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    const bool ret = GameBase::writePacketData(connection, w);
    w.writeF32(getEnergyLevel());
    w.writeF32(rechargeRate);
    return ret;
}

void registerShapeBaseNatives(TorqueScript& ts) {
    EngineObjects::registerClass("ShapeBase", [] { return std::make_shared<ShapeBase>(); });
    using Args = std::vector<VMValue>;
    auto self = [](const Args& args) -> ShapeBase* {
        return args.empty() ? nullptr : EngineObjects::get<ShapeBase>(args[0].toString());
    };
    auto arg = [](const Args& args, size_t i) { return i < args.size() ? args[i] : VMValue(""); };
    auto method = [&ts](const char* name, std::function<VMValue(ShapeBase&, const Args&)> body) {
        ts.registerNative(std::string("ShapeBase::") + name, [name, body](const Args& args) -> VMValue {
            ShapeBase* shape = args.empty() ? nullptr : EngineObjects::get<ShapeBase>(args[0].toString());
            if (!shape) {
                Console::instance().printf(LogLevel::Warn, "ShapeBase::%s: '%s' is not a ShapeBase",
                                           name, args.empty() ? "" : args[0].toString().c_str());
                return VMValue("");
            }
            return body(*shape, args);
        });
    };
    (void)self;
    // ShapeBase::getControllingClient: the connection's id, 0 when none.
    method("getControllingClient", [](ShapeBase& s, const Args&) {
        ScriptObject* client = s.controllingClient.empty() ? nullptr
            : ScriptEngine::instance().findObject(s.controllingClient.c_str());
        return VMValue(client ? ScriptEngine::instance().objectId(client) : 0);
    });
    method("setEnergyLevel", [arg](ShapeBase& s, const Args& a) { s.setEnergyLevel(arg(a, 1).toFloat()); return VMValue(""); });
    method("getEnergyLevel", [](ShapeBase& s, const Args&) { return VMValue(s.getEnergyLevel()); });
    method("getEnergyPercent", [](ShapeBase& s, const Args&) { return VMValue(s.getEnergyValue()); });
    method("setDamageLevel", [arg](ShapeBase& s, const Args& a) { s.setDamageLevel(arg(a, 1).toFloat()); return VMValue(""); });
    method("getDamageLevel", [](ShapeBase& s, const Args&) { return VMValue(s.damage); });
    method("getDamagePercent", [](ShapeBase& s, const Args&) { return VMValue(s.getDamageValue()); });
    method("setDamageState", [arg](ShapeBase& s, const Args& a) { return VMValue(s.setDamageState(arg(a, 1).toString()) ? 1 : 0); });
    method("getDamageState", [](ShapeBase& s, const Args&) { return VMValue(s.damageStateName()); });
    method("isDestroyed", [](ShapeBase& s, const Args&) { return VMValue(s.damageState == ShapeBase::Destroyed ? 1 : 0); });
    method("isDisabled", [](ShapeBase& s, const Args&) { return VMValue(s.damageState != ShapeBase::Enabled ? 1 : 0); });
    method("isEnabled", [](ShapeBase& s, const Args&) { return VMValue(s.damageState == ShapeBase::Enabled ? 1 : 0); });
    method("applyDamage", [arg](ShapeBase& s, const Args& a) { s.applyDamage(arg(a, 1).toFloat()); return VMValue(""); });
    method("applyRepair", [arg](ShapeBase& s, const Args& a) { s.applyRepair(arg(a, 1).toFloat()); return VMValue(""); });
    method("setRepairRate", [arg](ShapeBase& s, const Args& a) { s.repairRate = std::max(0.0f, arg(a, 1).toFloat()); return VMValue(""); });
    method("getRepairRate", [](ShapeBase& s, const Args&) { return VMValue(s.repairRate); });
    method("setRechargeRate", [arg](ShapeBase& s, const Args& a) { s.rechargeRate = arg(a, 1).toFloat(); return VMValue(""); });
    method("getRechargeRate", [](ShapeBase& s, const Args&) { return VMValue(s.rechargeRate); });
    method("setDamageFlash", [arg](ShapeBase& s, const Args& a) { s.damageFlash = std::clamp(arg(a, 1).toFloat(), 0.0f, 1.0f); return VMValue(""); });
    method("getDamageFlash", [](ShapeBase& s, const Args&) { return VMValue(s.damageFlash); });
    method("setWhiteOut", [arg](ShapeBase& s, const Args& a) { s.whiteOut = std::clamp(arg(a, 1).toFloat(), 0.0f, 1.5f); return VMValue(""); });
    method("getWhiteOut", [](ShapeBase& s, const Args&) { return VMValue(s.whiteOut); });
    method("setInvincibleMode", [arg](ShapeBase& s, const Args& a) {
        s.invincibleTime = arg(a, 1).toFloat();
        s.invincibleSpeed = arg(a, 2).toFloat();
        return VMValue("");
    });
    method("setCloaked", [arg](ShapeBase& s, const Args& a) { s.cloaked = arg(a, 1).toBool(); return VMValue(""); });
    method("isCloaked", [](ShapeBase& s, const Args&) { return VMValue(s.cloaked ? 1 : 0); });
    method("setPassiveJammed", [arg](ShapeBase& s, const Args& a) { s.passiveJammed = arg(a, 1).toBool(); return VMValue(""); });
    method("isPassiveJammed", [](ShapeBase& s, const Args&) { return VMValue(s.passiveJammed ? 1 : 0); });
    method("setHeat", [arg](ShapeBase& s, const Args& a) {
        const float heat = arg(a, 1).toFloat();
        if (heat < 0.0f || heat > 1.0f) {
            Console::instance().printf(LogLevel::Error, "Heat must be in the range [0, 1]");
            return VMValue("");
        }
        s.heat = heat;
        return VMValue("");
    });
    method("getHeat", [](ShapeBase& s, const Args&) { return VMValue(s.heat); });
    method("hide", [arg](ShapeBase& s, const Args& a) { s.hidden = arg(a, 1).toBool(); return VMValue(""); });
    method("isHidden", [](ShapeBase& s, const Args&) { return VMValue(s.hidden ? 1 : 0); });
    method("getCameraFov", [](ShapeBase& s, const Args&) { return VMValue(s.cameraFov); });
    method("setCameraFov", [arg](ShapeBase& s, const Args& a) {
        s.cameraFov = std::clamp(arg(a, 1).toFloat(), s.dataFloat("cameraMinFov", 5.0f), s.dataFloat("cameraMaxFov", 120.0f));
        return VMValue("");
    });

    // Mounted images (shapeBase.cc cMountImage ... cGetEyeTransform).
    auto slotOf = [arg](const Args& a, size_t i, int& slot) {
        slot = arg(a, i).toInt();
        return slot >= 0 && slot < ShapeBase::MaxMountedImages;
    };
    auto triple = [](const float v[3]) {
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", v[0], v[1], v[2]);
        return VMValue(buffer);
    };
    // obj.mountImage(DataBlock, slot, [loaded=true], [skinTag]). As in the
    // engine, `loaded` is read only when exactly three arguments are given
    // (argc == 5) and the skin tag only as a tagged string.
    method("mountImage", [arg](ShapeBase& s, const Args& a) {
        if (auto image = ShapeBaseImageData::find(arg(a, 1).toString())) {
            const uint32_t slot = (uint32_t)arg(a, 2).toInt();
            const size_t argc = a.size() + 1;
            const bool loaded = argc == 5 ? arg(a, 3).toBool() : true;
            uint32_t team = 0;
            if (argc == 6) {
                const std::string tag = arg(a, 4).toString();
                if (NetStrings::isTag(tag)) team = NetStrings::tagId(tag);
            }
            if (slot < (uint32_t)ShapeBase::MaxMountedImages) s.mountImage(std::move(image), slot, loaded, team);
        }
        return VMValue(0);
    });
    method("unmountImage", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.unmountImage((uint32_t)slot) ? 1 : 0);
    });
    method("getMountedImage", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        const ShapeBaseImageData* data = slotOf(a, 1, slot) ? s.getMountedImage((uint32_t)slot) : nullptr;
        return VMValue(data ? data->id : 0);
    });
    method("getPendingImage", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        const ShapeBaseImageData* data = slotOf(a, 1, slot) ? s.getPendingImage((uint32_t)slot) : nullptr;
        return VMValue(data ? data->id : 0);
    });
    method("isImageFiring", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.isImageFiring((uint32_t)slot) ? 1 : 0);
    });
    method("isImageMounted", [arg](ShapeBase& s, const Args& a) {
        const int id = imageDataBlockId(arg(a, 1).toString());
        return VMValue(id && s.isImageMounted(id) ? 1 : 0);
    });
    method("getMountSlot", [arg](ShapeBase& s, const Args& a) {
        const int id = imageDataBlockId(arg(a, 1).toString());
        return VMValue(id ? s.getMountSlot(id) : -1);
    });
    method("getImageSkinTag", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) ? (int32_t)s.getImageSkinTag((uint32_t)slot) : -1);
    });
    method("getImageState", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue("Error");
        const char* state = s.getImageState((uint32_t)slot);
        return VMValue(state ? state : "");
    });
    method("getImageTrigger", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.getImageTriggerState((uint32_t)slot) ? 1 : 0);
    });
    method("setImageTrigger", [slotOf, arg](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue(0);
        s.setImageTriggerState((uint32_t)slot, arg(a, 2).toBool());
        return VMValue(s.getImageTriggerState((uint32_t)slot) ? 1 : 0);
    });
    method("getImageAmmo", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.getImageAmmoState((uint32_t)slot) ? 1 : 0);
    });
    method("setImageAmmo", [slotOf, arg](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue(0);
        const bool ammo = arg(a, 2).toBool();
        s.setImageAmmoState((uint32_t)slot, ammo);
        return VMValue(ammo ? 1 : 0);
    });
    method("getImageTarget", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.getImageTargetState((uint32_t)slot) ? 1 : 0);
    });
    method("setImageTarget", [slotOf, arg](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue(0);
        const bool target = arg(a, 2).toBool();
        s.setImageTargetState((uint32_t)slot, target);
        return VMValue(target ? 1 : 0);
    });
    method("getImageLoaded", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        return VMValue(slotOf(a, 1, slot) && s.getImageLoadedState((uint32_t)slot) ? 1 : 0);
    });
    method("setImageLoaded", [slotOf, arg](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue(0);
        const bool loaded = arg(a, 2).toBool();
        s.setImageLoadedState((uint32_t)slot, loaded);
        return VMValue(loaded ? 1 : 0);
    });
    method("getMuzzleVector", [slotOf, triple](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue("0 1 0");
        float v[3];
        s.getMuzzleVector((uint32_t)slot, v);
        return triple(v);
    });
    method("getMuzzlePoint", [slotOf, triple](ShapeBase& s, const Args& a) {
        int slot;
        if (!slotOf(a, 1, slot)) return VMValue("0 0 0");
        float p[3];
        s.getMuzzlePoint((uint32_t)slot, p);
        return triple(p);
    });
    // getSlotTransform(slot): the mount point's transform (identity for a
    // slot out of range).
    method("getSlotTransform", [slotOf](ShapeBase& s, const Args& a) {
        int slot;
        const std::array<float, 16> xf = slotOf(a, 1, slot) ? s.getMountTransform((uint32_t)slot) : kIdentity;
        return VMValue(TorqueMath::format(xf));
    });
    method("getEyeVector", [triple](ShapeBase& s, const Args&) {
        const float forward[3] = {0, 1, 0};
        float v[3];
        TorqueMath::mulV(s.getEyeTransform(), forward, v);
        return triple(v);
    });
    method("getEyeTransform", [](ShapeBase& s, const Args&) { return VMValue(TorqueMath::format(s.getEyeTransform())); });

    // The script engine's mission-object mountImage/unmountImage stand-ins
    // are registered per class ("Player::mountImage", ...) and would shadow
    // ShapeBase's along the namespace chain; engine ShapeBase objects take
    // the image system, anything else keeps the stand-in.
    for (const char* name : {"mountImage", "unmountImage"}) {
        std::string suffix = std::string("::") + name, own = std::string("ShapeBase") + suffix;
        for (auto& c : suffix) c = (char)std::tolower((unsigned char)c);
        for (auto& c : own) c = (char)std::tolower((unsigned char)c);
        const auto natives = ts.getNatives();
        const auto mine = natives.at(own);
        for (const auto& [key, previous] : natives) {
            if (key == own || key.size() <= suffix.size() ||
                key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            ts.registerNative(key, [mine, previous](const Args& args) -> VMValue {
                if (!args.empty() && EngineObjects::get<ShapeBase>(args[0].toString())) return mine(args);
                return previous(args);
            });
        }
    }
}
