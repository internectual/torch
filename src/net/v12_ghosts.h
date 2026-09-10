#pragma once

#include "net/v12_bitstream.h"
#include "net/v12_registry.h"

#include <cstdint>
#include <map>

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
        bool atEnd = false;
        bool valid = false;
    };
    uint16_t datablockId = 0;
    bool hasDatablock = false;
    V12Vec3 position{};
    V12Vec3 rotation{};
    float rotationW = 1.0f;
    float health = 100.0f;
    float energy = 100.0f;
    bool hasHealth = false;
    bool hasEnergy = false;
    float headPitch = 0.0f;
    float headYaw = 0.0f;
    bool hasPosition = false;
    bool hasHeadAngles = false;
    bool hasRotation = false;
    bool moving = false;
    bool hasMovement = false;
    ThreadState threads[4]{};
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
                      PlayerGhostState* playerState = nullptr);

// Native update payloads are sparse; fold them onto the last full player state.
PlayerGhostState mergePlayerGhostState(const PlayerGhostState& base,
                                       const PlayerGhostState& update);

} // namespace V12
