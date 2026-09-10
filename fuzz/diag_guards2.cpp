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
    size_t sz16b = (size_t)(s8 - s16) * 4;
    size_t sz8b = (size_t)(szAll - s8) * 4;

    const uint32_t* buf32 = (const uint32_t*)(data.data() + 16);
    const uint16_t* buf16 = (const uint16_t*)(data.data() + 16 + sz32b);
    const uint8_t* buf8 = (const uint8_t*)(data.data() + 16 + sz32b + sz16b);

    size_t size32 = sz32b / 4;
    size_t size16 = sz16b / 2;
    size_t size8 = sz8b;

    fprintf(stderr, "DTS v%u: size32=%zu size16=%zu size8=%zu\n", ver, size32, size16, size8);

    // Search for guard sequences: positions where buf32[i]=N, buf16[j]=N, buf8[k]=N
    // for consecutive N values (0, 1, 2, ...)
    // We need to find a sequence of positions p32, p16, p8 where:
    // buf32[p32+i] = i, buf16[p16+i] = i, buf8[p8+i] = i for i = 0,1,2,...

    fprintf(stderr, "\n=== Scanning 32-bit buffer for small values ===\n");
    for (size_t i = 0; i < size32 && i < 500; i++) {
        if (buf32[i] <= 20) {
            fprintf(stderr, "32bit[%3zu] = %u\n", i, buf32[i]);
        }
    }

    fprintf(stderr, "\n=== Scanning 16-bit buffer for small values ===\n");
    for (size_t i = 0; i < size16 && i < 50; i++) {
        if (buf16[i] <= 20) {
            fprintf(stderr, "16bit[%3zu] = %u\n", i, buf16[i]);
        }
    }

    fprintf(stderr, "\n=== Scanning 8-bit buffer for small values ===\n");
    for (size_t i = 0; i < size8 && i < 50; i++) {
        if (buf8[i] <= 20) {
            fprintf(stderr, "8bit[%3zu] = %u\n", i, buf8[i]);
        }
    }

    // Try to find the guard sequence by looking for consecutive integers
    // in the 16-bit buffer (guards are stored as S16 in 16-bit buffer)
    fprintf(stderr, "\n=== Looking for guard sequence in 16-bit buffer ===\n");
    int lastGuard = -1;
    for (size_t i = 0; i < size16; i++) {
        int16_t val = (int16_t)buf16[i];
        if (val >= 0 && val <= 30 && (int)val == lastGuard + 1) {
            // Could be a guard sequence
            if (lastGuard >= -1) {
                // Check if next few values are consecutive
                if (i + 1 < size16 && (int16_t)buf16[i+1] == val + 1) {
                    fprintf(stderr, "Possible guard %d at 16bit[%zu], %d at [%zu]\n", val, i, val+1, i+1);
                }
            }
            lastGuard = val;
        } else if (val >= 0 && val <= 30 && (int)val == lastGuard) {
            // Same value, could be duplicate guard
        } else if (val < 0 || val > 30) {
            lastGuard = -1;
        }
    }

    // Find guard positions by looking for the pattern: N at consecutive positions
    // in the 16-bit buffer, with the 8-bit buffer having the same pattern
    fprintf(stderr, "\n=== Scanning for guard pattern in all buffers ===\n");
    // Guards are sequential: 0, 1, 2, 3, ...
    // Look for position p16 where buf16[p16] = N and buf16[p16+1] = N+1
    // But guards are not adjacent - there's data between them
    // Instead, look for sequences where the 16-bit and 8-bit values match

    // Let me try: find all positions in 16-bit buffer where value == 0
    // (guard 0), then check if the value is 1 at some later position (guard 1)
    // The distance tells us the size of the data section

    // Actually, let me just look at the 8-bit buffer since that's where
    // Point3F data (read via readPoint3F from buf32... wait, Point3F is from buf32)
    // No, readPoint3F reads from buf32.

    // Let me search 16-bit buffer for consecutive guard values 0,1,2,...
    int guard_pos16[20] = {-1};
    for (size_t i = 0; i < size16 && i < 10000; i++) {
        int16_t val = (int16_t)buf16[i];
        if (val >= 0 && val < 20) {
            // Check if this is a guard by seeing if it also appears in 8-bit buffer at the right offset
            // For now, record all candidate positions
            if (guard_pos16[val] == -1 || i < guard_pos16[val]) {
                guard_pos16[val] = i;
            }
        }
    }

    for (int g = 0; g < 15; g++) {
        if (guard_pos16[g] >= 0) {
            fprintf(stderr, "Guard %d first candidate at 16bit[%d]\n", g, guard_pos16[g]);
        }
    }

    return 0;
}
