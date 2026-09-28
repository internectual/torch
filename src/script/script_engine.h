#pragma once
#include <memory>
#include "script/dso_reader.h"
#include "core/console.h"
#include "core/math.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <stack>
#include <functional>
#include <cstdint>
#include <map>
#include <tuple>

class ScriptEngine;
class TorqueScript;

// isDemo() describes the executable build mode, not demo recording playback.
inline bool isDemoBuildMode(bool demoBuild, bool dedicated) {
    return demoBuild && !dedicated;
}

struct ScriptConnectionState {
    std::string serverAddress;
    uint16_t serverPort = 0;
    std::string clientName;
    int clientId = -1;
    std::string missionName;
    std::string missionType;
    uint32_t missionCrc = 0;
    bool online = false;
    bool observer = false;
};

struct ScriptObjectState {
    struct ThreadState {
        int sequence = -1;
        int state = 0;
        float timescale = 1.0f;
        float position = 0.0f;
        bool forward = true;
        bool atEnd = false;
        bool valid = false;
    };
    struct MountedImage {
        int datablockId = -1;
        int mountPoint = 0;
        int mountNodeObjectId = 0;
        bool loaded = false;
        bool firing = false;
    };
    int datablockId = 0;
    std::string className;
    std::string shapeName;
    std::string skinName;
    std::string profileName;
    std::string name;
    int type = 0;
    Point3F position{};
    Point3F rotation{};
    float rotationW = 1.0f;
    Point3F velocity{};
    float health = 100.0f;
    float maxHealth = 100.0f;
    float energy = 100.0f;
    int damageState = 0;
    float repairRate = 0.0f;
    int sensorGroup = -1;
    float headPitch = 0.0f;
    float headYaw = 0.0f;
    float barrelPitch = 0.0f;
    float barrelYaw = 0.0f;
    float shieldLevel = 0.0f;
    bool hasHeadAngles = false;
    bool hasTurretAim = false;
    bool hasShield = false;
    bool hasRotation = false;
    bool hasVelocity = false;
    bool hasHealth = false;
    bool hasMaxHealth = false;
    bool hasEnergy = false;
    bool hasDamageState = false;
    bool jetting = false;
    bool frozen = false;
    bool braking = false;
    bool hasVehicleState = false;
    bool cloaked = false;
    bool hasCloak = false;
    MountedImage mountedImages[8]{};
    ThreadState threads[4]{};
    int teamId = 0;
    int state = 0;
};

namespace ScriptStateParity {
inline float damageLevel(float health, float maxHealth) {
    if (!(maxHealth > 0.0f)) return 0.0f;
    const float value = 1.0f - health / maxHealth;
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

inline float energyPercent(float energy) {
    return energy < 0.0f ? 0.0f : energy > 100.0f ? 1.0f : energy / 100.0f;
}
}

struct ScriptLoadoutState {
    std::map<std::string, int> inventory;
    std::map<std::string, int> maxInventory;
    std::map<int, int> weaponAmmo;
    int currentWeapon = -1;
    int ammo = -1;
    int weaponCount = 0;
    int backpackIndex = -1;
    bool backpackActive = false;
    bool hasInventory = false;
    bool hasWeapons = false;
    bool hasBackpack = false;
};

struct VMValue {
    enum Type { None, Int, Float, String };
    Type type = None;
    union { int32_t i = 0; float f; };
    double dbl = 0; // for float table loading
    std::string str;

    VMValue() = default;
    VMValue(int32_t v) : type(Int), i(v) {}
    VMValue(float v) : type(Float), f(v), dbl(v) {}
    VMValue(double v) : type(Float), f((float)v), dbl(v) {}
    VMValue(const char* v) : type(String), str(v ? v : "") {}
    VMValue(const std::string& v) : type(String), str(v) {}

    int32_t toInt() const;
    float toFloat() const;
    double toDouble() const;
    std::string toString() const;
    bool toBool() const;
};

class EngineObject;

struct ScriptObject {
    // SimObject id: datablocks from 3, dynamic objects from 2051 (Tribes 2:
    // 11-bit datablock ids, 3..2050). Assigned
    // on first use (ScriptEngine::objectId); an id is only ever handed out
    // after that, so every id lookup can be answered.
    int id = 0;
    std::string className;
    std::string name;
    std::unordered_map<std::string, VMValue> fields;
    std::unordered_map<std::string, VMValue> internals;
    std::vector<std::string> deleteNotifyListeners;
    // Engine-class state (src/sim), when the class is an engine class.
    std::shared_ptr<EngineObject> engine;
};

struct ScriptMissionObject {
    int id = 0;
    std::string className;
    std::string name;
    std::string parentName;
    std::unordered_map<std::string, VMValue> fields;
};

// Action-map binding store, shared by the TS bind/bindcmd natives and the
// ActionMap::save / getBinding natives (engine.cpp re-registers bind/bindcmd
// and must feed the same storage the save path reads).
struct ActionBinding {
    std::string cmdOn;
    std::string cmdOff;
    bool isCmd = false;
};
inline std::map<std::tuple<std::string, int, std::string>, ActionBinding>& actionBindingStore() {
    static std::map<std::tuple<std::string, int, std::string>, ActionBinding> s_binds;
    return s_binds;
}

class VirtualMachine {
public:
    using NativeFunc = std::function<VMValue(const std::vector<VMValue>&)>;

    VirtualMachine(ScriptEngine* engine);
    ~VirtualMachine();

    bool loadScript(const uint8_t* data, size_t size, const char* name = nullptr);
    bool loadScriptFile(const char* path);

    VMValue callFunction(const char* name, const std::vector<VMValue>& args = {});
    // A function compiled into a loaded DSO (natives excluded); false when
    // no active DSO defines it.
    bool callScriptFunction(const char* name, const std::vector<VMValue>& args, VMValue& result);
    VMValue callMethod(const char* objName, const char* method, const std::vector<VMValue>& args = {});

    void setVariable(const char* name, const VMValue& val);
    VMValue getVariable(const char* name);

    ScriptObject* getObject(const char* name);
    void addObject(ScriptObject* obj);

    void registerNativeFunction(const char* name, NativeFunc fn);
    bool unloadScript(const char* name);

    VMValue execute(DSOFile* dso, uint32_t startIp, const std::vector<VMValue>& args,
                    bool methodCall = false);
    const std::vector<DSOFile*>& loadedScripts() const;

private:
    struct Impl;
    Impl* impl;
};

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    static ScriptEngine& instance();
    static bool exists();

    bool init();
    void shutdown();

    void executeString(const char* script);
    void executeFile(const char* path);

    using NativeFunc = VirtualMachine::NativeFunc;
    void registerFunction(const char* name, NativeFunc fn);

    VirtualMachine* vm() { return vmInstance; }
    TorqueScript* ts() { return tsInstance; }

    // Stock scripts use isDemo() for the executable's retail/demo build path;
    // playback state is tracked separately by isDemoPlaying().
    void setDemoStateProvider(std::function<bool()> provider) {
        demoStateProvider = std::move(provider);
    }
    bool isDemoPlaying() const {
        return demoStateProvider ? demoStateProvider() : false;
    }
    void setDemoModeProvider(std::function<bool()> provider) {
        demoModeProvider = std::move(provider);
    }
    bool isDemoMode() const {
        return demoModeProvider ? demoModeProvider() : false;
    }
    void setServerStateProvider(std::function<bool()> provider) {
        serverStateProvider = std::move(provider);
    }
    void setClientStateProvider(std::function<bool()> provider) {
        clientStateProvider = std::move(provider);
    }
    bool isServer() const {
        return serverStateProvider ? serverStateProvider() : false;
    }
    bool isClient() const {
        return clientStateProvider ? clientStateProvider() : false;
    }
    void setConnectionStateProvider(std::function<ScriptConnectionState()> provider) {
        connectionStateProvider = std::move(provider);
    }
    ScriptConnectionState connectionState() const {
        return connectionStateProvider ? connectionStateProvider() : ScriptConnectionState{};
    }
    void setControlObjectProvider(std::function<int()> provider) {
        controlObjectProvider = std::move(provider);
    }
    void setCameraObjectProvider(std::function<int()> provider) {
        cameraObjectProvider = std::move(provider);
    }
    void setObjectStateProvider(std::function<bool(int, ScriptObjectState&)> provider) {
        objectStateProvider = std::move(provider);
    }
    void setLoadoutStateProvider(std::function<ScriptLoadoutState()> provider) {
        loadoutStateProvider = std::move(provider);
    }
    using PlayerMutation = std::function<bool(int, float)>;
    using WeaponMutation = std::function<bool(int, int, int)>;
    using InventoryMutation = std::function<bool(int, const std::string&, int)>;
    using ObjectMutation = std::function<bool(int, int)>;
    using ImageMutation = std::function<bool(int, int, int)>;
    using VelocityMutation = std::function<bool(int, const Point3F&)>;
    using ThreadMutation = std::function<bool(int, int, int, const std::string&)>;
    void setHealthMutationProvider(PlayerMutation provider) {
        healthMutationProvider = std::move(provider);
    }
    void setEnergyMutationProvider(PlayerMutation provider) {
        energyMutationProvider = std::move(provider);
    }
    void setRepairRateMutationProvider(PlayerMutation provider) {
        repairRateMutationProvider = std::move(provider);
    }
    void setWeaponAmmoMutationProvider(WeaponMutation provider) {
        weaponAmmoMutationProvider = std::move(provider);
    }
    void setInventoryMutationProvider(InventoryMutation provider) {
        inventoryMutationProvider = std::move(provider);
    }
    void setCurrentWeaponMutationProvider(std::function<bool(int, int)> provider) {
        currentWeaponMutationProvider = std::move(provider);
    }
    void setTeamMutationProvider(ObjectMutation provider) {
        teamMutationProvider = std::move(provider);
    }
    void setMountedImageMutationProvider(ImageMutation provider) {
        mountedImageMutationProvider = std::move(provider);
    }
    void setVelocityMutationProvider(VelocityMutation provider) {
        velocityMutationProvider = std::move(provider);
    }
    void setThreadMutationProvider(ThreadMutation provider) {
        threadMutationProvider = std::move(provider);
    }
    void setControlObjectMutationProvider(ObjectMutation provider) {
        controlObjectMutationProvider = std::move(provider);
    }
    bool mutateHealth(int objectId, float health) const {
        return healthMutationProvider && healthMutationProvider(objectId, health);
    }
    bool mutateEnergy(int objectId, float energy) const {
        return energyMutationProvider && energyMutationProvider(objectId, energy);
    }
    bool mutateRepairRate(int objectId, float rate) const {
        return repairRateMutationProvider && repairRateMutationProvider(objectId, rate);
    }
    bool mutateWeaponAmmo(int objectId, int slot, int ammo) const {
        return weaponAmmoMutationProvider && weaponAmmoMutationProvider(objectId, slot, ammo);
    }
    bool mutateInventory(int objectId, const std::string& item, int amount) const {
        return inventoryMutationProvider && inventoryMutationProvider(objectId, item, amount);
    }
    bool mutateCurrentWeapon(int objectId, int slot) const {
        return currentWeaponMutationProvider && currentWeaponMutationProvider(objectId, slot);
    }
    bool mutateTeam(int objectId, int team) const {
        return teamMutationProvider && teamMutationProvider(objectId, team);
    }
    bool mutateMountedImage(int objectId, int slot, int datablock) const {
        return mountedImageMutationProvider && mountedImageMutationProvider(objectId, slot, datablock);
    }
    bool mutateVelocity(int objectId, const Point3F& velocity) const {
        return velocityMutationProvider && velocityMutationProvider(objectId, velocity);
    }
    bool mutateThread(int objectId, int slot, int operation, const std::string& value = {}) const {
        return threadMutationProvider && threadMutationProvider(objectId, slot, operation, value);
    }
    bool mutateControlObject(int connectionId, int objectId) const {
        return controlObjectMutationProvider && controlObjectMutationProvider(connectionId, objectId);
    }
    ScriptLoadoutState loadoutState() const {
        return loadoutStateProvider ? loadoutStateProvider() : ScriptLoadoutState{};
    }
    int controlObjectId() const {
        return controlObjectProvider ? controlObjectProvider() : -1;
    }
    int cameraObjectId() const {
        return cameraObjectProvider ? cameraObjectProvider() : -1;
    }
    bool objectState(int id, ScriptObjectState& state) const {
        return objectStateProvider && id > 0 && objectStateProvider(id, state);
    }

    ScriptObject* findObject(const char* name);

    void objectAdded(ScriptObject* object);
    bool isSimSet(ScriptObject* object);
    bool isSimGroup(ScriptObject* object);
    // SimSet::addObject (a SimGroup takes the object from its old group).
    bool addToSet(ScriptObject* set, ScriptObject* object);
    bool removeFromSet(ScriptObject* set, ScriptObject* object);
    // The id of the object's dataBlock field target, or "" (GameBase).
    std::string objectDataBlock(ScriptObject* object);
    bool setObjectField(ScriptObject* object, const std::string& field, const VMValue& value);
    bool addDeleteNotify(ScriptObject* listener, ScriptObject* target);
    bool clearDeleteNotify(ScriptObject* listener, ScriptObject* target);
    bool deleteScriptObject(const std::string& name);

    void setMissionObjects(std::vector<ScriptMissionObject> objects, bool worldBacked = false);
    bool missionObjectsWorldBacked() const { return missionObjectsWorldBacked_; }
    void clearMissionObjects();
    void cancelMissionEvents();
    void dispatchMissionObjectRemovalCallbacks();
    const std::vector<ScriptMissionObject>& missionObjects() const { return missionObjects_; }
    std::vector<std::string> missionDeletionOrder(const std::string& name) const;
    void removeMissionObjects(const std::vector<std::string>& names);

    // Global object registry, keyed by objectKey (the name, or the id for an
    // anonymous object).
    std::unordered_map<std::string, ScriptObject*> objects;
    std::unordered_map<int, ScriptObject*> objectsById;
    // The object's SimObject id, assigned from the dynamic range on first use.
    int objectId(ScriptObject* object);
    // Datablocks take ids from their own range (3..2050).
    void assignDatablockId(ScriptObject* object);
    // SimDataBlock::assignId/onAdd: a datablock id, the next modified key,
    // and membership of DataBlockGroup (creation order).
    void registerDataBlock(ScriptObject* object);
    // An object the engine creates itself (registerObject + assignName).
    ScriptObject* createEngineObject(const std::string& className, const std::string& name);
    // deleteDataBlocks(): every datablock, last first; ids restart at 3.
    void deleteDataBlocks();
    // The engine's named groups (RootGroup children): ClientGroup, DataBlockGroup.
    void ensureEngineGroups();
    int allocateObjectId(bool datablock = false);
    // Forget a deleted object's id.
    void forgetObject(ScriptObject* object);
    // Registry key: the name, or the id for an anonymous object.
    std::string objectKey(ScriptObject* object);
    // A handle (name or numeric id) as the registry key it refers to; other
    // strings pass through unchanged.
    std::string canonicalName(const std::string& handle);
    // The object's script namespaces in dispatch order: its name, the
    // ScriptObject class / superClass fields, then its C++ class.
    std::vector<std::string> objectNamespaces(ScriptObject* object);

private:
    // SimObject id ranges (DataBlockObjectIdFirst, DynamicObjectIdFirst).
    int nextDatablockObjectId_ = 3;
    int nextDataBlockModifiedKey_ = 0;
    int nextDynamicObjectId_ = 2051;
    static ScriptEngine* instance_;
    VirtualMachine* vmInstance{};
    TorqueScript* tsInstance{};
    Console* con{};
    std::function<bool()> demoStateProvider;
    std::function<bool()> demoModeProvider;
    std::function<bool()> serverStateProvider;
    std::function<bool()> clientStateProvider;
    std::function<ScriptConnectionState()> connectionStateProvider;
    std::function<int()> controlObjectProvider;
    std::function<int()> cameraObjectProvider;
    std::function<bool(int, ScriptObjectState&)> objectStateProvider;
    std::function<ScriptLoadoutState()> loadoutStateProvider;
    PlayerMutation healthMutationProvider;
    PlayerMutation energyMutationProvider;
    PlayerMutation repairRateMutationProvider;
    WeaponMutation weaponAmmoMutationProvider;
    InventoryMutation inventoryMutationProvider;
    std::function<bool(int, int)> currentWeaponMutationProvider;
    ObjectMutation teamMutationProvider;
    ImageMutation mountedImageMutationProvider;
    VelocityMutation velocityMutationProvider;
    ThreadMutation threadMutationProvider;
    ObjectMutation controlObjectMutationProvider;
    std::vector<ScriptMissionObject> missionObjects_;
    bool missionObjectsWorldBacked_ = false;
};

// Prefs-export gate: boot-time default-seeding code paths may call
// export("$pref::*","prefs/ClientPrefs.cs") BEFORE saved prefs have been
// loaded, which would truncate them. The engine flips this after loading.
void allowClientPrefsExport(bool allowed);
bool clientPrefsExportAllowed();
