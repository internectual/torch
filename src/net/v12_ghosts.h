#pragma once

#include "net/v12_bitstream.h"
#include "net/v12_registry.h"

#include <cstdint>
#include <map>
#include <vector>

namespace V12 {

struct GhostEntry {
    uint16_t index = 0;
    uint16_t classId = 0;
    const char* className = nullptr;
};

struct PlayerGhostState {
    struct ThreadState {
        int sequence = -1;
        int state = 0;
        float timescale = 1.0f;
        float position = 0.0f;
        bool forward = true; // Compatibility view derived from timescale.
        bool atEnd = false;
        bool valid = false;
    };
    struct SoundThreadState {
        int profileId = -1;
        bool playing = false;
        bool valid = false;
    };
    struct MountedImage {
        int datablockId = -1;
        bool loaded = false;
        bool firing = false;
        bool valid = false;
    };
    struct WheelState {
        float angularVelocity = 0.0f;
        float suspension = 0.0f;
        float lateral = 0.0f;
        bool valid = false;
    };
    uint16_t datablockId = 0;
    bool hasDatablock = false;
    V12Vec3 position{};
    V12Vec3 rotation{};
    float rotationW = 1.0f;
    float health = 100.0f;
    float maxHealth = 100.0f;
    int damageState = 0;
    float energy = 100.0f;
    bool hasHealth = false;
    bool hasMaxHealth = false;
    bool hasDamageState = false;
    bool hasEnergy = false;
    int kills = 0;
    int deaths = 0;
    int score = 0;
    int team = 0;
    bool hasStats = false;
    bool jetting = false;
    bool hasJetting = false;
    bool controlObject = false;
    bool hasControlObject = false;
    bool frozen = false;
    bool hasFrozen = false;
    bool braking = false;
    bool hasBraking = false;
    bool hasVehicleState = false;
    float headPitch = 0.0f;
    float headYaw = 0.0f;
    bool hasPosition = false;
    bool hasHeadAngles = false;
    bool hasRotation = false;
    bool moving = false;
    bool hasMovement = false;
    V12Vec3 velocity{};
    bool hasVelocity = false;
    V12Vec3 beamStart{};
    V12Vec3 beamEnd{};
    bool hasBeam = false;
    float barrelPitch = 0.0f;
    float barrelYaw = 0.0f;
    bool hasTurretAim = false;
    ThreadState threads[4]{};
    SoundThreadState soundThreads[4]{};
    MountedImage mountedImages[8]{};
    WheelState wheels[6]{};
    bool cloaked = false;
    bool hasCloak = false;
    float shieldLevel = 0.0f;
    bool hasShield = false;
};

struct ProjectileImpact {
    V12Vec3 position{};
    V12Vec3 normal{0, 1, 0};
    uint16_t datablockId = 0;
    bool hasDatablock = false;
};

class GhostTracker {
public:
    bool create(uint16_t index, uint16_t classId);
    bool update(uint16_t index);
    bool erase(uint16_t index);
    void clear() { ghosts.clear(); }

    const GhostEntry* get(uint16_t index) const;
    size_t size() const { return ghosts.size(); }

private:
    std::map<uint16_t, GhostEntry> ghosts;
};

bool readPlayerGhostPayload(V12BitStream& stream, bool initial,
                            const V12Vec3& compressionPoint,
                            PlayerGhostState* state = nullptr);

bool readItemGhostPayload(V12BitStream& stream, bool initial,
                          const V12Vec3& compressionPoint,
                          PlayerGhostState* state = nullptr);

// Reads the verified Tribes 2 build-25034 payload for the supported class.
// Unsupported classes return false without consuming payload bits.
bool readGhostPayload(V12BitStream& stream, uint16_t classId, bool initial,
                      const V12Vec3& compressionPoint,
                      PlayerGhostState* playerState = nullptr,
                      std::vector<ProjectileImpact>* impacts = nullptr);

// Native update payloads are sparse; fold them onto the last full player state.
PlayerGhostState mergePlayerGhostState(const PlayerGhostState& base,
                                       const PlayerGhostState& update);

} // namespace V12
