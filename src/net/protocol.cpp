#include "net/protocol.h"
#include <cstring>

// ─── Torch protocol codecs ────────────────────────────────────────

uint16_t T2Protocol::calculateChecksum(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++)
        sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

bool T2Protocol::verifyChecksum(const uint8_t* data, size_t size) {
    if (size < 2) return false;
    uint16_t stored = 0;
    memcpy(&stored, data + size - 2, 2);
    return calculateChecksum(data, size - 2) == stored;
}

static void writeU32LE(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t readU32LE(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void writeF32LE(uint8_t* out, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    writeU32LE(out, bits);
}

static float readF32LE(const uint8_t* data) {
    const uint32_t bits = readU32LE(data);
    float value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// ─── Move Message ─────────────────────────────────────────────────

size_t T2Protocol::encodeMove(uint8_t* buf, size_t bufSize, const MoveMessage& msg) {
    if (bufSize < 34) return 0;
    uint32_t pos = 0;
    buf[pos++] = GDT_Move;
    writeF32LE(buf + pos, msg.posX); pos += 4;
    writeF32LE(buf + pos, msg.posY); pos += 4;
    writeF32LE(buf + pos, msg.posZ); pos += 4;
    writeF32LE(buf + pos, msg.rotZ); pos += 4;
    writeF32LE(buf + pos, msg.rotX); pos += 4;
    buf[pos++] = msg.flags;
    writeF32LE(buf + pos, msg.lookX); pos += 4;
    writeF32LE(buf + pos, msg.lookY); pos += 4;
    writeU32LE(buf + pos, msg.seq); pos += 4;
    return pos;
}

bool T2Protocol::decodeMove(const uint8_t* data, size_t size, MoveMessage& msg) {
    if (size < 34 || data[0] != GDT_Move) return false;
    uint32_t pos = 1;
    msg.posX = readF32LE(data + pos); pos += 4;
    msg.posY = readF32LE(data + pos); pos += 4;
    msg.posZ = readF32LE(data + pos); pos += 4;
    msg.rotZ = readF32LE(data + pos); pos += 4;
    msg.rotX = readF32LE(data + pos); pos += 4;
    msg.flags = data[pos++];
    msg.lookX = readF32LE(data + pos); pos += 4;
    msg.lookY = readF32LE(data + pos); pos += 4;
    msg.seq = readU32LE(data + pos); pos += 4;
    const float values[] = {msg.posX, msg.posY, msg.posZ, msg.rotZ, msg.rotX, msg.lookX, msg.lookY};
    for (float value : values)
        if (!std::isfinite(value) || std::fabs(value) > 10000000.0f) return false;
    return true;
}

// ─── Update Message ───────────────────────────────────────────────

bool T2Protocol::encodeUpdate(uint8_t* buf, size_t bufSize, const UpdateMessage& msg) {
    if (bufSize < 40) return false;
    uint32_t pos = 0;
    buf[pos++] = GDT_Update;
    writeF32LE(buf + pos, msg.posX); pos += 4;
    writeF32LE(buf + pos, msg.posY); pos += 4;
    writeF32LE(buf + pos, msg.posZ); pos += 4;
    writeF32LE(buf + pos, msg.rotZ); pos += 4;
    writeF32LE(buf + pos, msg.rotX); pos += 4;
    writeF32LE(buf + pos, msg.velX); pos += 4;
    writeF32LE(buf + pos, msg.velY); pos += 4;
    writeF32LE(buf + pos, msg.velZ); pos += 4;
    float hc = msg.health; if (hc < 0) hc = 0; else if (hc > 100) hc = 100;
    float ec = msg.energy; if (ec < 0) ec = 0; else if (ec > 100) ec = 100;
    buf[pos++] = (uint8_t)(hc * 2);
    buf[pos++] = (uint8_t)(ec * 2);
    buf[pos++] = msg.flags;
    writeU32LE(buf + pos, msg.lastMoveSeq); pos += 4;
    return true;
}

bool T2Protocol::decodeUpdate(const uint8_t* data, size_t size, UpdateMessage& msg) {
    if (size < 40 || data[0] != GDT_Update) return false;
    uint32_t pos = 1;
    msg.posX = readF32LE(data + pos); pos += 4;
    msg.posY = readF32LE(data + pos); pos += 4;
    msg.posZ = readF32LE(data + pos); pos += 4;
    msg.rotZ = readF32LE(data + pos); pos += 4;
    msg.rotX = readF32LE(data + pos); pos += 4;
    msg.velX = readF32LE(data + pos); pos += 4;
    msg.velY = readF32LE(data + pos); pos += 4;
    msg.velZ = readF32LE(data + pos); pos += 4;
    msg.health = data[pos++] / 2.0f;
    msg.energy = data[pos++] / 2.0f;
    msg.flags = data[pos++];
    msg.lastMoveSeq = readU32LE(data + pos); pos += 4;
    const float values[] = {msg.posX, msg.posY, msg.posZ, msg.rotZ, msg.rotX,
                            msg.velX, msg.velY, msg.velZ, msg.health, msg.energy};
    for (float value : values)
        if (!std::isfinite(value) || std::fabs(value) > 10000000.0f) return false;
    if (msg.health < 0 || msg.health > 100 || msg.energy < 0 || msg.energy > 100)
        return false;
    return true;
}

// ─── Datablock Messages ───────────────────────────────────────────

size_t T2Protocol::encodeDatablock(uint8_t* buf, size_t bufSize,
                                    const DatablockHeader& hdr,
                                    const uint8_t* payload, size_t payloadLen) {
    size_t needed = 1 + 4*4 + payloadLen;
    if (bufSize < needed) return 0;
    uint32_t pos = 0;
    buf[pos++] = GDT_Datablock;
    writeProtocolU32LE(buf + pos, hdr.classId); pos += 4;
    writeProtocolU32LE(buf + pos, hdr.objectId); pos += 4;
    writeProtocolU32LE(buf + pos, hdr.index); pos += 4;
    writeProtocolU32LE(buf + pos, hdr.total); pos += 4;
    if (payload && payloadLen > 0) {
        memcpy(buf + pos, payload, payloadLen); pos += payloadLen;
    }
    return pos;
}

// Codec functions moved to protocol.h as inline definitions so they can be
// fuzzed (and reused) without linking GameServer/Engine.

const T2Protocol::RSAKey& T2Protocol::getTribesNextPublicKey() {
    // TribesNext RSA-2048 public key for authentication
    // Source: TribesNext open-source project
    static RSAKey key;
    static bool initialized = false;
    if (!initialized) {
        memset(key.modulus, 0, 256);
        memset(key.exponent, 0, 4);
        key.exponent[0] = 0x01; key.exponent[1] = 0x00; key.exponent[2] = 0x01; // 65537
        // Modulus would be loaded from the TribesNext public key file at runtime
        initialized = true;
    }
    return key;
}
