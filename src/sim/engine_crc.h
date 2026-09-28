#pragma once
// core/crc.cc calculateCRC: reflected 0xedb88320 table, no final xor.
// ResManager::getCrc starts from 0xffffffff.
#include <cstddef>
#include <cstdint>

namespace EngineCrc {

inline uint32_t calculate(const void* buffer, size_t length, uint32_t crc = 0xffffffffu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int j = 0; j < 8; ++j) value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        ready = true;
    }
    const unsigned char* bytes = static_cast<const unsigned char*>(buffer);
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

} // namespace EngineCrc
