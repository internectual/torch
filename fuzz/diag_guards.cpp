#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

int main() {
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2.open((basePath + "/shapes.vl2").c_str());
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read("shapes/bioderm_medium.dts");
    if (data.empty()) { fprintf(stderr, "Failed to read\n"); return 1; }

    uint16_t ver = *(const uint16_t*)data.data();
    int32_t s16 = *(const int32_t*)(data.data()+8);
    int32_t s8 = *(const int32_t*)(data.data()+12);
    size_t sz32b = (size_t)s16 * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    size_t size32 = sz32b / 4;

    // Read header counts
    size_t p32 = 0;
    auto r32 = [&]() -> uint32_t { return buf32[p32++]; };
    auto r32s = [&]() -> int32_t { return (int32_t)buf32[p32++]; };

    int32_t numNodes = r32s();
    int32_t numObjects = r32s();
    int32_t numDecals = r32s();
    int32_t numSubShapes = r32s();
    int32_t numIFLs = r32s();
    int32_t numNodeRot = (ver >= 22) ? r32s() : 0;
    int32_t numNodeTrans = (ver >= 22) ? r32s() : 0;
    int32_t numNodeUScale = (ver >= 22) ? r32s() : 0;
    int32_t numNodeAScale = (ver >= 22) ? r32s() : 0;
    int32_t numNodeArbScale = (ver >= 22) ? r32s() : 0;
    if (ver < 22) {
        int32_t combined = r32s() - numNodes;
        if (combined < 0) combined = 0;
        numNodeRot = numNodeTrans = combined;
    }
    int32_t numGroundFrames = (ver > 23) ? r32s() : 0;
    int32_t numObjStates = r32s();
    int32_t numDecalStates = r32s();
    int32_t numTriggers = r32s();
    int32_t numDetails = r32s();
    int32_t numMeshes = r32s();
    int32_t numSkins = (ver < 23) ? r32s() : 0;
    int32_t numNames = r32s();
    r32s(); r32s(); // smallestVisSize, smallestVisDL

    fprintf(stderr, "Header: ver=%u numNodes=%d numObjects=%d numDecals=%d numSubShapes=%d numIFLs=%d\n",
            ver, numNodes, numObjects, numDecals, numSubShapes, numIFLs);
    fprintf(stderr, "numNodeRot=%d numNodeTrans=%d numUScale=%d numAScale=%d numArbScale=%d\n",
            numNodeRot, numNodeTrans, numNodeUScale, numNodeAScale, numNodeArbScale);
    fprintf(stderr, "numGroundFrames=%d numObjStates=%d numDecalStates=%d numTriggers=%d\n",
            numGroundFrames, numObjStates, numDecalStates, numTriggers);
    fprintf(stderr, "numDetails=%d numMeshes=%d numSkins=%d numNames=%d\n",
            numDetails, numMeshes, numSkins, numNames);

    fprintf(stderr, "pos32 after header: %zu (expected 17)\n", p32);

    // Find all guard positions in the 32-bit buffer
    // Guards are 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, ...
    // Let's scan and find where guard N appears at position p32
    fprintf(stderr, "\n=== Scanning for guard markers in 32-bit buffer ===\n");
    for (size_t i = 0; i < size32; i++) {
        if (buf32[i] <= 20 && buf32[i] == i) {
            // This might be a guard (value == position relative to start)
        }
    }

    // Actually, guards are at specific positions. Let me look for the pattern:
    // guard 0 = 0 at some position
    // guard 1 = 1 at the next position after the data before it
    // etc.
    // But I don't know the exact positions. Let me try to find runs of
    // consecutive integers.

    // Actually, let me just dump the 32-bit buffer around where we expect things to be.
    fprintf(stderr, "\n=== 32-bit buffer dump (first 500 values) ===\n");
    for (size_t i = 0; i < std::min(size32, (size_t)500); i++) {
        fprintf(stderr, "[%3zu]=%7u ", i, buf32[i]);
        if ((i+1) % 8 == 0) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    // Also check: does the 32-bit buffer have guard 6 at position p32?
    // Based on the code: after header (17) + guard0 (1) + bounds (11) + guard1 (1) + 
    // nodes (155) + guard2 (1) + objects (144) + guard3 (1) + decals(?) + guard4 (1) +
    // ifls(?) + guard5 (1) + subshapes(?) + guard6 (1)
    // = 17 + 1 + 11 + 1 + 155 + 1 + 144 + 1 + ? + 1 + ? + 1 + ? + 1 = ?
    
    size_t expected_pos = 17 + 1 + 11 + 1 + 155 + 1 + 144 + 1;
    fprintf(stderr, "\nExpected pos after guard3: %zu\n", expected_pos);
    fprintf(stderr, "Expected pos after decals (33*5=165): %zu\n", expected_pos + 165 + 1);
    fprintf(stderr, "Expected pos after decals (33*4=132): %zu\n", expected_pos + 132 + 1);
    fprintf(stderr, "Expected pos after decals (33*3=99): %zu\n", expected_pos + 99 + 1);
    
    // Let's scan for value 6 (guard 6) at various positions
    fprintf(stderr, "\n=== Looking for value 6 (guard 6) ===\n");
    for (size_t i = expected_pos; i < std::min(expected_pos + 100, size32); i++) {
        if (buf32[i] == 6) {
            fprintf(stderr, "Found 6 at pos %zu (offset from expected: %d)\n", i, (int)(i - expected_pos));
        }
    }

    // Let's also scan for value 7 (guard 7)
    fprintf(stderr, "\n=== Looking for value 0 (guard 0) after pos 0 ===\n");
    for (size_t i = 0; i < std::min(size32, (size_t)200); i++) {
        if (buf32[i] == 0) {
            fprintf(stderr, "Found 0 at pos %zu\n", i);
        }
    }

    return 0;
}
