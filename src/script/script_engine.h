#pragma once
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
    struct MountedImage {
        int datablockId = -1;
        int mountPoint = 0;
        bool loaded = false;
        bool firing = false;
    };
    int datablockId = 0;
    std::string className;
    std::string shapeName;
    std::string name;
    Point3F position{};
    Point3F rotation{};
    float rotationW = 1.0f;
    Point3F velocity{};
    float health = 100.0f;
    float maxHealth = 100.0f;
    float energy = 100.0f;
    float repairRate = 0.0f;
    int sensorGroup = -1;
    bool hasRotation = false;
    bool hasVelocity = false;
    bool hasHealth = false;
    bool hasMaxHealth = false;
    bool hasEnergy = false;
    MountedImage mountedImages[8]{};
    int teamId = 0;
    int state = 0;
};

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

struct ScriptObject {
    std::string className;
    std::string name;
    std::unordered_map<std::string, VMValue> fields;
    std::unordered_map<std::string, VMValue> internals;
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
    VMValue callMethod(const char* objName, const char* method, const std::vector<VMValue>& args = {});

    void setVariable(const char* name, const VMValue& val);
    VMValue getVariable(const char* name);

    ScriptObject* getObject(const char* name);
    void addObject(ScriptObject* obj);

    void registerNativeFunction(const char* name, NativeFunc fn);

    VMValue execute(DSOFile* dso, uint32_t startIp, const std::vector<VMValue>& args);
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

    // Stock scripts use isDemo() to select replay-only UI and input paths.
    // Keep the source of that state injectable so the native remains testable.
    void setDemoStateProvider(std::function<bool()> provider) {
        demoStateProvider = std::move(provider);
    }
    bool isDemoPlaying() const {
        return demoStateProvider ? demoStateProvider() : false;
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

    // Global object registry
    std::unordered_map<std::string, ScriptObject*> objects;

private:
    static ScriptEngine* instance_;
    VirtualMachine* vmInstance{};
    TorqueScript* tsInstance{};
    Console* con{};
    std::function<bool()> demoStateProvider;
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
    ObjectMutation controlObjectMutationProvider;
};

// Prefs-export gate: boot-time default-seeding code paths may call
// export("$pref::*","prefs/ClientPrefs.cs") BEFORE saved prefs have been
// loaded, which would truncate them. The engine flips this after loading.
void allowClientPrefsExport(bool allowed);
bool clientPrefsExportAllowed();
