#include "render/renderer.h"

void TerrainBlock::setEmptySquareRuns(const std::vector<uint32_t>& runs) {
    emptySquares.assign((size_t)size * size, 0);
    // Runs are packed as x | (y << 8) | (count << 16).
    for (uint32_t packed : runs) {
        const uint32_t x = packed & 0xffu;
        const uint32_t y = (packed >> 8) & 0xffu;
        const uint32_t count = packed >> 16;
        if (y >= (uint32_t)size) continue;
        for (uint32_t i = 0; i < count; ++i)
            emptySquares[y * (uint32_t)size + ((x + i) % (uint32_t)size)] = 1;
    }
}

bool TerrainBlock::isEmptySquare(float wx, float wz) const {
    if (emptySquares.empty() || size < 2 || squareSize <= 0.0f) return false;
    const int x = (int)std::floor((wx - worldOffset.x) / squareSize);
    const int z = (int)std::floor((worldOffset.z - wz) / squareSize);
    if (x < 0 || z < 0 || x >= size || z >= size) return false;
    return emptySquares[(size_t)z * size + x] != 0;
}
