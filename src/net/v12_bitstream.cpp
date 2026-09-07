#include "net/v12_bitstream.h"

#include <limits>
#include <cmath>
#include <cstring>
#include <array>
#include <algorithm>
#include <mutex>
#include <vector>
#include <functional>
#include <string>

namespace {
struct HuffNode { int zero = 0; int one = 0; };
struct HuffTree {
    std::vector<HuffNode> nodes;
    std::array<uint32_t, 256> codes{};
    std::array<uint8_t, 256> codeBits{};
};

const HuffTree& huffmanTree() {
    static std::once_flag once;
    static HuffTree tree;
    std::call_once(once, [] {
        static constexpr int frequencies[] = {
            0,0,0,0,0,0,0,0,0,329,21,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            2809,68,0,27,0,58,3,62,4,7,0,0,15,65,554,3,
            394,404,189,117,30,51,27,15,34,32,80,1,142,3,142,39,
            0,144,125,44,122,275,70,135,61,127,8,12,113,246,122,36,
            185,1,149,309,335,12,11,14,54,151,0,0,2,0,0,211,
            0,2090,344,736,993,2872,701,605,646,1552,328,305,1240,735,1533,1713,
            562,3,1775,1149,1469,979,407,553,59,279,31,0,0,0,68,0
        };
        struct Wrap { int nodeIdx; int leafIdx; bool isNode; };
        std::array<Wrap, 512> wraps{};
        int wrapCount = 256;
        for (int i = 0; i < 256; ++i) {
            const int freq = i < (int)(sizeof(frequencies) / sizeof(frequencies[0])) ? frequencies[i] : 0;
            const bool alnum = (i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z');
            wraps[i] = {-1, i, false};
            tree.codes[i] = (uint32_t)(freq + (alnum ? 2 : 1));
        }
        tree.nodes.push_back({});
        // Keep the population separate from the wire code table while
        // constructing the tree; the table is populated with codes below.
        std::array<int, 512> populations{};
        for (int i = 0; i < 256; ++i) {
            const int freq = i < (int)(sizeof(frequencies) / sizeof(frequencies[0])) ? frequencies[i] : 0;
            const bool alnum = (i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z');
            populations[i] = freq + (alnum ? 2 : 1);
        }
        while (wrapCount > 1) {
            int min1 = 0x7fffffff, min2 = 0x7fffffff;
            int idx1 = -1, idx2 = -1;
            for (int i = 0; i < wrapCount; ++i) {
                const int pop = populations[i];
                if (pop < min1) { min2 = min1; idx2 = idx1; min1 = pop; idx1 = i; }
                else if (pop < min2) { min2 = pop; idx2 = i; }
            }
            HuffNode node;
            node.zero = wraps[idx1].isNode ? wraps[idx1].nodeIdx : -(wraps[idx1].leafIdx + 1);
            node.one = wraps[idx2].isNode ? wraps[idx2].nodeIdx : -(wraps[idx2].leafIdx + 1);
            const int index = (int)tree.nodes.size();
            tree.nodes.push_back(node);
            const int mergeIndex = std::min(idx1, idx2);
            const int nukeIndex = std::max(idx1, idx2);
            wraps[mergeIndex] = {index, -1, true};
            populations[mergeIndex] = min1 + min2;
            if (nukeIndex != wrapCount - 1) {
                wraps[nukeIndex] = wraps[wrapCount - 1];
                populations[nukeIndex] = populations[wrapCount - 1];
            }
            --wrapCount;
        }
        tree.nodes[0] = tree.nodes[wraps[0].nodeIdx];
        std::function<void(int, uint32_t, uint8_t)> assignCodes =
            [&](int node, uint32_t code, uint8_t bits) {
                if (node < 0) {
                    int symbol = -node - 1;
                    tree.codes[symbol] = code;
                    tree.codeBits[symbol] = bits;
                    return;
                }
                assignCodes(tree.nodes[node].zero, code, bits + 1);
                assignCodes(tree.nodes[node].one, code | (1u << bits), bits + 1);
            };
        assignCodes(0, 0, 0);
    });
    return tree;
}
}

V12BitStream::V12BitStream(const uint8_t* input, size_t size, size_t bitOffset)
    : bytes(input), bitPosition(bitOffset), bitSize(size > SIZE_MAX / 8 ? SIZE_MAX : size * 8) {
    if (!bytes || bitPosition > bitSize) error = true;
}

bool V12BitStream::require(size_t bits) {
    if (error || bits > bitSize - bitPosition) {
        error = true;
        return false;
    }
    return true;
}

bool V12BitStream::readFlag() {
    if (!require(1)) return false;
    const bool value = (bytes[bitPosition >> 3] & (uint8_t)(1u << (bitPosition & 7))) != 0;
    ++bitPosition;
    return value;
}

uint32_t V12BitStream::readUnsigned(int bits) {
    if (bits < 1 || bits > 32 || !require((size_t)bits)) return 0;
    uint32_t value = 0;
    for (int i = 0; i < bits; ++i) {
        if (bytes[bitPosition >> 3] & (uint8_t)(1u << (bitPosition & 7))) value |= 1u << i;
        ++bitPosition;
    }
    return value;
}

int32_t V12BitStream::readSigned(int bits) {
    if (bits < 1 || bits > 32) { error = true; return 0; }
    const uint32_t raw = readUnsigned(bits);
    if (error) return 0;
    if (bits == 32) return (int32_t)raw;
    const uint32_t sign = 1u << (bits - 1);
    const uint32_t magnitude = raw & (sign - 1);
    return raw & sign ? -((int32_t)magnitude) : (int32_t)magnitude;
}

int32_t V12BitStream::readSignedInt(int bits) {
    if (bits < 1 || bits > 32) { error = true; return 0; }
    const bool negative = readFlag();
    const uint32_t magnitude = bits == 1 ? 0 : readUnsigned(bits - 1);
    return negative ? -((int32_t)magnitude) : (int32_t)magnitude;
}

uint32_t V12BitStream::readRange(uint32_t first, uint32_t last) {
    if (last < first) { error = true; return 0; }
    const uint64_t count = (uint64_t)last - first + 1;
    int bits = 0;
    while (((uint64_t)1 << bits) < count) ++bits;
    if (bits == 0) return first;
    const uint32_t value = readUnsigned(bits);
    if (error || (uint64_t)value >= count) { error = true; return 0; }
    return first + value;
}

float V12BitStream::readFloat(int bits) {
    if (bits < 1 || bits > 31) { error = true; return 0.0f; }
    const uint32_t value = readUnsigned(bits);
    return (float)value / (float)((1u << bits) - 1u);
}

float V12BitStream::readSignedFloat(int bits) {
    if (bits < 1 || bits > 31) { error = true; return 0.0f; }
    return readFloat(bits) * 2.0f - 1.0f;
}

float V12BitStream::readF32() {
    const uint32_t raw = readUnsigned(32);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

V12Vec3 V12BitStream::readPoint3F() {
    return {readF32(), readF32(), readF32()};
}

V12Vec3 V12BitStream::readNormalVector(int bits) {
    constexpr float Pi = 3.14159265358979323846f;
    const float phi = readSignedFloat(bits + 1) * Pi;
    const float theta = readSignedFloat(bits) * (Pi * 0.5f);
    return {std::sin(phi) * std::cos(theta),
            std::cos(phi) * std::cos(theta), std::sin(theta)};
}

V12Vec3 V12BitStream::readCompressedPoint(const V12Vec3& compressionPoint,
                                          float scale) {
    if (!std::isfinite(scale) || scale <= 0.0f) {
        error = true;
        return compressionPoint;
    }
    const uint32_t type = readUnsigned(2);
    if (error) return compressionPoint;
    if (type == 3) return readPoint3F();
    static constexpr int bitCounts[] = {16, 18, 20};
    const int bits = bitCounts[type];
    auto readSignedMagnitude = [this, bits]() -> int32_t {
        const bool negative = readFlag();
        const uint32_t magnitude = readUnsigned(bits - 1);
        return negative ? -(int32_t)magnitude : (int32_t)magnitude;
    };
    return {compressionPoint.x + (float)readSignedMagnitude() * scale,
            compressionPoint.y + (float)readSignedMagnitude() * scale,
            compressionPoint.z + (float)readSignedMagnitude() * scale};
}

V12AffineTransform V12BitStream::readAffineTransform(const V12Vec3& compressionPoint) {
    V12AffineTransform result;
    result.position = readCompressedPoint(compressionPoint);
    result.x = readF32();
    result.y = readF32();
    result.z = readF32();
    result.w = std::sqrt(std::max(0.0f, 1.0f -
        (result.x * result.x + result.y * result.y + result.z * result.z)));
    if (readFlag()) result.w = -result.w;
    return result;
}

std::string V12BitStream::readRawString(size_t maxLength) {
    const uint32_t length = readUnsigned(8);
    if (error || length > maxLength || !require((size_t)length * 8)) {
        error = true;
        return {};
    }
    std::string result;
    result.reserve(length);
    for (uint32_t i = 0; i < length; ++i) result.push_back((char)readUnsigned(8));
    return result;
}

std::string V12BitStream::readHuffmanString(size_t maxLength) {
    const bool compressed = readFlag();
    const uint32_t length = readUnsigned(8);
    if (error || length > maxLength) { error = true; return {}; }
    if (!compressed) {
        if (!require((size_t)length * 8)) return {};
        std::string result;
        result.reserve(length);
        for (uint32_t i = 0; i < length; ++i) result.push_back((char)readUnsigned(8));
        return result;
    }

    const auto& tree = huffmanTree();
    std::string result;
    result.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        int node = 0;
        while (node >= 0) {
            const bool one = readFlag();
            if (error) return {};
            node = one ? tree.nodes[node].one : tree.nodes[node].zero;
        }
        result.push_back((char)(-node - 1));
    }
    return result;
}

std::string V12BitStream::readString(size_t maxLength) {
    if (!stringBufferEnabled) return readHuffmanString(maxLength);
    if (readFlag()) {
        const uint32_t offset = readUnsigned(8);
        const std::string suffix = readHuffmanString(maxLength);
        if (error || offset > stringBuffer.size() ||
            offset + suffix.size() > maxLength) {
            error = true;
            return {};
        }
        stringBuffer = stringBuffer.substr(0, offset) + suffix;
    } else {
        stringBuffer = readHuffmanString(maxLength);
    }
    return stringBuffer;
}

void V12BitStream::setStringBuffer(bool enabled) {
    stringBufferEnabled = enabled;
    stringBuffer.clear();
}

std::string V12BitStream::unpackNetString() {
    switch (readUnsigned(2)) {
    case 0:
        return {};
    case 1:
        return readString();
    case 2: {
        const uint32_t tag = readUnsigned(10);
        return std::string("\x01") + std::to_string(tag);
    }
    case 3: {
        const bool negative = readFlag();
        int bits = readFlag() ? 7 : (readFlag() ? 15 : 31);
        const int32_t value = (int32_t)readUnsigned(bits);
        return std::to_string(negative ? -value : value);
    }
    default:
        return {};
    }
}

void V12BitWriter::writeFlag(bool value) {
    if ((bitPosition & 7) == 0) bytes.push_back(0);
    if (value) bytes.back() |= (uint8_t)(1u << (bitPosition & 7));
    ++bitPosition;
}

void V12BitWriter::writeUnsigned(uint32_t value, int bits) {
    if (bits < 1 || bits > 32) return;
    for (int i = 0; i < bits; ++i) writeFlag((value & (1u << i)) != 0);
}

bool V12BitWriter::writeHuffmanString(const std::string& value) {
    if (value.size() > 255) return false;
    const auto& tree = huffmanTree();
    uint64_t compressedBits = 0;
    for (unsigned char c : value) compressedBits += tree.codeBits[c];
    const bool compressed = compressedBits < value.size() * 8;
    writeFlag(compressed);
    writeUnsigned((uint32_t)value.size(), 8);
    if (compressed) {
        for (unsigned char c : value)
            writeUnsigned(tree.codes[c], tree.codeBits[c]);
    } else {
        for (unsigned char c : value) writeUnsigned(c, 8);
    }
    return true;
}

bool V12BitWriter::writeNetString(const std::string& value) {
    writeUnsigned(1, 2); // normal Huffman string
    return writeHuffmanString(value);
}

void V12BitWriter::alignToByte() {
    while (bitPosition & 7) writeFlag(false);
}

void V12BitWriter::writeBits(const uint8_t* source, size_t bits) {
    if (!source) return;
    for (size_t i = 0; i < bits; ++i)
        writeFlag((source[i >> 3] & (uint8_t)(1u << (i & 7))) != 0);
}
