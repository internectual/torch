#pragma once

#include "net/v12_bitstream.h"
#include "net/v12_datablocks.h"

#include <cstdint>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace V12 {

constexpr int EventClassBits = 6;
constexpr int EventClassFirst = 255;
constexpr int EventSequenceBits = 7;

class NetStringTable;

struct EventHeader {
    bool guaranteed = false;
    bool sequential = false;
    uint8_t classId = 0;
    uint8_t sequence = 0;
};

struct ServerEvent {
    bool guaranteed = false;
    uint8_t sequence = 0;
    uint8_t classId = 0;
    std::string message;
    bool hasGhostingMessage = false;
    uint32_t ghostSequence = 0;
    uint8_t ghostMessage = 0;
    uint16_t ghostCount = 0;
    bool hasDatablock = false;
    bool datablockProcess = false;
    uint16_t datablockObject = 0;
    uint8_t datablockClass = 0;
    uint16_t datablockIndex = 0;
    uint16_t datablockTotal = 0;
    std::string datablockClassName;
    DecodedDataBlock datablockData;
    bool hasGhostAlways = false;
    uint16_t ghostAlwaysIndex = 0;
    uint8_t ghostAlwaysClass = 0;
    bool ghostAlwaysHasData = false;
};

struct ServerGameState {
    uint32_t lastMoveAck = 0;
    float damageFlash = 0;
    float whiteOut = 0;
    bool controlPresent = false;
    bool controlDirty = false;
    uint16_t controlGhost = 0;
    V12Vec3 compressionPoint{};
    bool hasCompressionPoint = false;
    bool hasCameraFov = false;
    uint8_t cameraFov = 0;
};

// Event packets contain two lists: unguaranteed events first, then guaranteed
// events. The continuation bit for each list is consumed by the caller.
bool readEventHeader(V12BitStream& stream, bool guaranteedPhase, EventHeader& header);
void writeEventHeader(V12BitWriter& writer, bool guaranteedPhase,
                      const EventHeader& header, bool sequential = false);

class NetStringTable {
public:
    std::pair<uint16_t, bool> getOrAdd(const std::string& value);
    bool set(uint16_t id, std::string value);
    const std::string* get(uint16_t id) const;
    void clear() { values.clear(); }
    size_t size() const { return values.size(); }

private:
    std::unordered_map<uint16_t, std::string> values;
};

bool readServerEvents(V12BitStream& stream, NetStringTable& strings,
                      std::vector<ServerEvent>& events);
bool readServerPacketEvents(V12BitStream& stream, NetStringTable& strings,
                            std::vector<ServerEvent>& events,
                            ServerGameState* state = nullptr,
                            V12Vec3* compressionPoint = nullptr,
                            size_t* eventsEnd = nullptr);

} // namespace V12
