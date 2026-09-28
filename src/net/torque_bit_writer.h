#pragma once
// core/bitStream.cc write side (plus Tribes 2's compressed points), on top
// of V12BitWriter. The demo BitStream reader is the other half.
#include "net/v12_bitstream.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

class TorqueBitWriter {
public:
    struct Point { float x = 0, y = 0, z = 0; };

    V12BitWriter& raw() { return writer; }
    const std::vector<uint8_t>& data() const { return writer.data(); }
    size_t bitPosition() const { return writer.sizeBits(); }

    bool writeFlag(bool value) { writer.writeFlag(value); return value; }
    void writeInt(int32_t value, int bits) { if (bits > 0) writer.writeUnsigned((uint32_t)value, bits); }
    void writeU32(uint32_t value) { writer.writeUnsigned(value, 32); }
    void writeSignedInt(int32_t value, int bits) {
        if (writeFlag(value < 0)) writeInt(-value, bits - 1);
        else writeInt(value, bits - 1);
    }
    void writeRangedU32(uint32_t value, uint32_t start, uint32_t end) {
        const uint32_t size = end - start + 1;
        uint32_t pow2 = 1;
        while (pow2 < size) pow2 <<= 1;
        int bits = 0;
        while ((1u << bits) < pow2) ++bits;
        writeInt((int32_t)(value - start), bits);
    }
    void writeFloat(float f, int bits) { writeInt((int32_t)(f * ((1 << bits) - 1)), bits); }
    void writeSignedFloat(float f, int bits) { writeInt((int32_t)(((f + 1) * 0.5f) * ((1 << bits) - 1)), bits); }
    void writeF32(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        writeU32(bits);
    }
    void writePoint(const Point& p) { writeF32(p.x); writeF32(p.y); writeF32(p.z); }
    void writeNormalVector(const Point& v, int bits) {
        const float phi = std::atan2(v.x, v.y) / (float)M_PI;
        const float theta = std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) / (float)(M_PI / 2.0);
        writeSignedFloat(phi, bits + 1);
        writeSignedFloat(theta, bits);
    }
    // Tribes 2 BitStream::writeCompressedPoint: a 16/18/20-bit signed offset
    // from the compression point in `scale` steps, else the raw point.
    void writeCompressedPoint(const Point& p, float scale = 0.01f) {
        const Point d{p.x - compression.x, p.y - compression.y, p.z - compression.z};
        const float invScale = 1.0f / scale;
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * invScale;
        int type = dist < (1 << 15) ? 0 : dist < (1 << 17) ? 1 : dist < (1 << 19) ? 2 : 3;
        writeInt(type, 2);
        if (type == 3) { writePoint(p); return; }
        static const int bitCounts[] = {16, 18, 20};
        writeSignedInt((int32_t)(d.x * invScale), bitCounts[type]);
        writeSignedInt((int32_t)(d.y * invScale), bitCounts[type]);
        writeSignedInt((int32_t)(d.z * invScale), bitCounts[type]);
    }
    void setCompressionPoint(const Point& p) { compression = p; }
    void clearCompression() { compression = {}; }
    const Point& compressionPoint() const { return compression; }

    // BitStream::setStringBuffer: strings share a prefix with the last one.
    void setStringBuffer(bool enabled) { stringBufferEnabled = enabled; stringBuffer.clear(); }
    // A Huffman string with no string-buffer prefix (the reader's readHuffmanString).
    void writeHuffmanString(const std::string& value) { writer.writeHuffmanString(value.substr(0, 255)); }
    void writeString(const std::string& value, size_t maxLen = 255) {
        std::string s = value.substr(0, maxLen);
        if (stringBufferEnabled) {
            size_t j = 0;
            while (j < s.size() && j < stringBuffer.size() && stringBuffer[j] == s[j]) ++j;
            stringBuffer = s;
            if (writeFlag(j > 2)) {
                writeInt((int32_t)j, 8);
                writer.writeHuffmanString(s.substr(j));
                return;
            }
        }
        writer.writeHuffmanString(s);
    }

private:
    V12BitWriter writer;
    Point compression;
    bool stringBufferEnabled = false;
    std::string stringBuffer;
};
