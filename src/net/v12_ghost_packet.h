#pragma once

#include "net/v12_bitstream.h"
#include "net/v12_ghosts.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace V12 {

struct GhostUpdate {
    enum class Operation { Create, Update, Delete } operation;
    uint16_t index = 0;
    uint16_t classId = 0;
    size_t dataBegin = 0;
    size_t dataEnd = 0;
    bool failed = false;
};

using GhostPayloadReader = std::function<bool(
    V12BitStream&, uint16_t index, uint16_t classId, bool initial)>;

// Decode the ghost section after game state and events. The callback owns the
// class-specific payload decoding; returning false stops safely at that ghost.
bool readGhostUpdates(V12BitStream& stream, GhostTracker& tracker,
                      std::vector<GhostUpdate>& updates,
                      const GhostPayloadReader& readPayload = {});

} // namespace V12
