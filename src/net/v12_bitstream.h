#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Rendering-independent reader for Torque/V12 bit-packed network data.
struct V12Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct V12AffineTransform {
    V12Vec3 position{};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

class V12BitStream {
public:
    V12BitStream(const uint8_t* data, size_t size, size_t bitOffset = 0);

    bool readFlag();
    bool readBool() { return readUnsigned(8) != 0; }
    uint32_t readUnsigned(int bits);
    int32_t readSigned(int bits);
    int32_t readSignedInt(int bits);
    uint32_t readRange(uint32_t first, uint32_t last);
    float readFloat(int bits);
    float readSignedFloat(int bits);
    float readF32();
    V12Vec3 readPoint3F();
    V12Vec3 readNormalVector(int bits);
    V12Vec3 readCompressedPoint(const V12Vec3& compressionPoint,
                                float scale = 0.01f);
    V12AffineTransform readAffineTransform(const V12Vec3& compressionPoint = {});
    uint8_t readU8() { return (uint8_t)readUnsigned(8); }
    uint16_t readU16() { return (uint16_t)readUnsigned(16); }
    uint32_t readU32() { return readUnsigned(32); }
    std::string readRawString(size_t maxLength = 4096);
    std::string readHuffmanString(size_t maxLength = 255);
    std::string readString(size_t maxLength = 255);
    std::string unpackNetString();
    void setStringBuffer(bool enabled);

    size_t position() const { return bitPosition; }
    size_t sizeBits() const { return bitSize; }
    size_t remainingBits() const { return bitPosition <= bitSize ? bitSize - bitPosition : 0; }
    bool failed() const { return error; }

private:
    const uint8_t* bytes;
    size_t bitPosition;
    size_t bitSize;
    bool error = false;
    bool stringBufferEnabled = false;
    std::string stringBuffer;
    bool require(size_t bits);
};

class V12BitWriter {
public:
    void writeFlag(bool value);
    void writeUnsigned(uint32_t value, int bits);
    bool writeHuffmanString(const std::string& value);
    bool writeNetString(const std::string& value);
    void alignToByte();
    size_t sizeBits() const { return bitPosition; }
    void writeBits(const uint8_t* source, size_t bits);
    const std::vector<uint8_t>& data() const { return bytes; }

private:
    std::vector<uint8_t> bytes;
    size_t bitPosition = 0;
};
