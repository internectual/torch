#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include "fs/file_system.h"
#include "fs/vl2_archive.h"
#include "core/engine.h"

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
    int32_t szAll = *(const int32_t*)(data.data()+4);
    int32_t s16 = *(const int32_t*)(data.data()+8);
    int32_t s8 = *(const int32_t*)(data.data()+12);

    size_t sz32b = (size_t)s16 * 4;
    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    size_t size32 = sz32b / 4;

    // Find all positions where value matches expected guard N
    // Guards should be: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, ...
    // We look for consecutive values
    fprintf(stderr, "=== Looking for guard sequence in 32-bit buffer ===\n");
    
    // Find first occurrence of each guard value 0-20
    for (int g = 0; g <= 20; g++) {
        // Find the position of value g that could be guard g
        // Guards should appear in increasing position order
        // and the values should be consecutive
        size_t found = SIZE_MAX;
        for (size_t i = (g == 0 ? 0 : found); i < size32; i++) {
            if (buf32[i] == (uint32_t)g) {
                found = i;
                break;
            }
        }
        if (found != SIZE_MAX) {
            // Check if previous guard was found before this
            fprintf(stderr, "Guard %d at 32bit[%zu] = %u\n", g, found, buf32[found]);
        } else {
            fprintf(stderr, "Guard %d not found\n", g);
            break;
        }
    }

    // Also specifically look at values around expected positions
    fprintf(stderr, "\n=== Values at known positions ===\n");
    fprintf(stderr, "[18] = %u (guard 0 expected)\n", buf32[18]);
    fprintf(stderr, "[30] = %u (guard 1 expected)\n", buf32[30]);
    fprintf(stderr, "[186] = %u (guard 2 expected)\n", buf32[186]);
    fprintf(stderr, "[331] = %u (guard 3 expected)\n", buf32[331]);
    
    // Between guard 3 (331) and guard 4: 33 decals * ? S32s
    // Try to find guard 4
    fprintf(stderr, "\n=== Searching for guard 4 after pos 331 ===\n");
    for (size_t i = 332; i < size32 && i < 600; i++) {
        if (buf32[i] == 4) {
            int offset = i - 332;
            fprintf(stderr, "Found 4 at [%zu], offset from guard3+1 = %d, decals*per = %d if 33 decals\n", i, offset, offset/33);
        }
    }

    // After finding guard 4, search for guard 5
    fprintf(stderr, "\n=== Searching for guard 5 after pos 497 ===\n");
    for (size_t i = 498; i < size32 && i < 650; i++) {
        if (buf32[i] == 5) {
            fprintf(stderr, "Found 5 at [%zu]\n", i);
        }
    }

    fprintf(stderr, "\n=== Dump 495-560 ===\n");
    for (size_t i = 495; i < std::min(size32, (size_t)560); i++) {
        fprintf(stderr, "[%3zu]=%u ", i, buf32[i]);
        if ((i+1) % 10 == 0) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    // After guard 5, the subshapes should be 3 values (1 subshape * 3)
    // Then guard 6
    // Let's look for guard 6
    fprintf(stderr, "\n=== Searching for guard 6 after subshapes ===\n");
    // If guard 4 at [497] and guard 5 at [498]:
    // Subshape first set: [499-501] (3 values)
    // Guard 6 at [502]?
    // Or: subshape first set: [499-501], subshape second set: [502-504], guard 6 at [505]?
    for (size_t i = 499; i < std::min(size32, (size_t)560); i++) {
        if (buf32[i] == 6) {
            fprintf(stderr, "Found 6 at [%zu]\n", i);
        }
    }

    return 0;
}
