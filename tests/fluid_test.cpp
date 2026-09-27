#include "game/water_parity.h"
#include <cassert>
#include <vector>

int main() {
    // TWL_Damnation: position 128 -168, scale 352 288 -> fluid space 1152 856.
    FluidInfo info = fluidSetInfo(128 + 1024.0f, -168 + 1024.0f, 352.0f, 288.0f);
    assert(info.squareX0 == 144 && info.squareY0 == 107);
    assert(info.highRes && info.step4 == 32.0f);
    assert(info.squaresX == 44 && info.squaresY == 36);
    assert(info.blocksX == 11 && info.blocksY == 9);

    // Large fluids: 8-square blocks, at most 256 squares.
    info = fluidSetInfo(0.0f, 0.0f, 4096.0f, 1030.0f);
    assert(!info.highRes && info.step4 == 64.0f);
    assert(info.squaresX == 256 && info.squaresY == 136);
    assert(info.blocksX == 32 && info.blocksY == 17);
    // The min corner clamps to the rep.
    assert(fluidSetInfo(-100.0f, 99999.0f, 8, 8).squareX0 == 0);
    assert(fluidSetInfo(-100.0f, 99999.0f, 8, 8).squareY0 == 2040);

    // A basin: terrain at 10 everywhere except a pit at height 0 in the
    // middle of an 8 x 8-square fluid (high res, 2 x 2 blocks).
    std::vector<float> heights(256 * 256, 10.0f);
    info = fluidSetInfo(0.0f, 0.0f, 64.0f, 64.0f);
    assert(info.blocksX == 2 && info.blocksY == 2);
    auto accept = fluidAcceptMask(info, 5.0f, 0.0f, false, heights.data());
    for (auto a : accept) assert(!a); // all underground
    heights[3 * 256 + 3] = 0.0f;      // one wet point in block (0, 0)
    accept = fluidAcceptMask(info, 5.0f, 0.0f, false, heights.data());
    assert(accept[0] && !accept[1] && !accept[2] && !accept[3]);
    // A wet point on the fluid's border is dried by removeWetEdges; the
    // enclosed one stays.
    heights[0 * 256 + 7] = 0.0f;
    accept = fluidAcceptMask(info, 5.0f, 0.0f, false, heights.data());
    assert(accept[1]);
    accept = fluidAcceptMask(info, 5.0f, 0.0f, true, heights.data());
    assert(accept[0] && !accept[1]);
    // The level includes half the wave amplitude.
    heights.assign(256 * 256, 10.0f);
    heights[3 * 256 + 3] = 5.5f;
    assert(!fluidAcceptMask(info, 5.0f, 0.0f, false, heights.data())[0]);
    assert(fluidAcceptMask(info, 5.0f, 2.0f, false, heights.data())[0]);
    // No terrain: every block.
    accept = fluidAcceptMask(info, 5.0f, 0.0f, true, nullptr);
    for (auto a : accept) assert(a);

    assert(fluidRepIndex(100.0f) == 0);
    assert(fluidRepIndex(2048.0f) == 1);
    assert(fluidRepIndex(-1.0f) == -1);
    assert(fluidRepIndex(-2048.0f) == -2);
    return 0;
}
