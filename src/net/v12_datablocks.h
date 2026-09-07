#pragma once

#include "net/v12_bitstream.h"

#include <cstddef>
#include <string>

namespace V12 {

struct DecodedDataBlock {
    std::string shapeFile;
    std::string debrisShape;
    std::string cloakTexture;
};

// Consume one Tribes 2 build-25034 SimDataBlock payload. classId may be
// either the registry index or the wire class id (128 + registry index).
// When supplied, decoded native asset references are returned in `decoded`.
bool readDataBlockPayload(V12BitStream& stream, size_t classId,
                          DecodedDataBlock* decoded = nullptr);

} // namespace V12
