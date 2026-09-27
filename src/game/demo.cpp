#include <set>
#include "game/demo.h"
#include "net/v12_datablocks.h"
#include "net/v12_registry.h"
#include "net/v12_ghosts.h"
#include "core/console.h"
#include "core/config.h"
#include "core/timer.h"
#include "game/mission_discovery.h"
#include "game/observer_parity.h"
#include "game/ghost_parity.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <memory>
#include <sstream>
#include <zlib.h>

// Pending explosion events from projectile ghost parsers
std::vector<DemoParser::PendingExplosion> DemoParser::s_pendingExplosions;
float DemoParser::s_packetTime = 0.0f;

// Quaternion for a Torque yaw about +Z (MatrixF::set(EulerF(0, 0, yaw)),
// which turns forward +Y toward +X), in the Torque frame the renderer
// converts with torqueQuaternionToYUp.
static Vec4 torqueYawQuaternion(float yaw) {
    const float half = yaw * 0.5f;
    return {0, 0, -sinf(half), cosf(half)};
}

static std::string stripDemoColorCodes(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'c') {
            i += (i + 2 < value.size()) ? 2 : 1;
            continue;
        }
        result.push_back(value[i]);
    }
    return result;
}

void DemoParser::extractMissionInfo() {
    initialBlock.missionDisplayName.clear();
    initialBlock.missionTypeDisplayName.clear();
    initialBlock.gameClassName.clear();
    initialBlock.serverDisplayName.clear();
    initialBlock.modName.clear();
    initialBlock.recorderName.clear();
    initialBlock.recorderClientId = -1;
    initialBlock.recordingDate.clear();

    for (size_t i = 0; i < initialBlock.demoValues.size(); ++i) {
        std::vector<std::string> fields;
        std::stringstream row(initialBlock.demoValues[i]);
        std::string field;
        while (std::getline(row, field, '\t')) fields.push_back(field);
        if (fields.size() >= 3 && fields[1].size() >= 4 &&
            fields[1].compare(fields[1].size() - 4, 4, "Game") == 0)
            initialBlock.gameClassName = fields[1];

        if (initialBlock.demoValues[i] != "readplayerinfo" || i + 1 >= initialBlock.demoValues.size())
            continue;
        const std::string& value = initialBlock.demoValues[++i];
        fields.clear();
        std::stringstream info(value);
        while (std::getline(info, field, '\t')) fields.push_back(field);
        if (fields.empty()) continue;
        if (fields[0] == "1") {
            if (fields.size() > 1) initialBlock.recorderClientId = std::atoi(fields[1].c_str());
            if (fields.size() > 2) initialBlock.recorderName = stripDemoColorCodes(fields[2]);
        } else if (fields[0] == "2") {
            if (fields.size() > 1) initialBlock.serverDisplayName = fields[1];
            if (fields.size() > 3) initialBlock.recordingDate = fields[3];
            if (fields.size() > 4) initialBlock.missionDisplayName = fields[4];
        } else if (fields[0] == "3") {
            if (fields.size() > 1) initialBlock.modName = fields[1];
            if (fields.size() > 2) initialBlock.missionTypeDisplayName = fields[2];
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Huffman processor
// ═══════════════════════════════════════════════════════════════
static const int CSM_CHAR_FREQS[256] = {
    0,0,0,0,0,0,0,0,0,329,21,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    2809,68,0,27,0,58,3,62,4,7,0,0,15,65,554,3,
    394,404,189,117,30,51,27,15,34,32,80,1,142,3,142,39,
    0,144,125,44,122,275,70,135,61,127,8,12,113,246,122,36,
    185,1,149,309,335,12,11,14,54,151,0,0,2,0,0,211,
    0,2090,344,736,993,2872,701,605,646,1552,328,305,1240,735,1533,1713,
    562,3,1775,1149,1469,979,407,553,59,279,31,0,0,0,68,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

BitStream::HuffNode BitStream::s_huffNodes[512]{};
BitStream::HuffLeaf BitStream::s_huffLeaves[256]{};
int BitStream::s_huffNodeCount = 0;
bool BitStream::s_huffBuilt = false;

void BitStream::buildHuffmanTables() {
    if (s_huffBuilt) return;
    s_huffBuilt = true;
    s_huffNodeCount = 1;

    for (int i = 0; i < 256; i++) {
        int pop = CSM_CHAR_FREQS[i] + 1;
        if ((i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z'))
            pop += 1;
        s_huffLeaves[i] = {pop, i, 0, 0};
    }

    struct Wrap { int nodeIdx; int leafIdx; bool isNode; };
    Wrap wraps[512];
    int wrapCount = 256;
    for (int i = 0; i < 256; i++)
        wraps[i] = {-1, i, false};

    auto wrapPop = [&](const Wrap& w) {
        return w.isNode ? s_huffNodes[w.nodeIdx].pop : s_huffLeaves[w.leafIdx].pop;
    };

    while (wrapCount > 1) {
        int min1 = 0x7FFFFFFF, min2 = 0x7FFFFFFF;
        int idx1 = -1, idx2 = -1;
        for (int i = 0; i < wrapCount; i++) {
            int p = wrapPop(wraps[i]);
            if (p < min1) { min2 = min1; idx2 = idx1; min1 = p; idx1 = i; }
            else if (p < min2) { min2 = p; idx2 = i; }
        }
        HuffNode node;
        node.pop = wrapPop(wraps[idx1]) + wrapPop(wraps[idx2]);
        node.index0 = wraps[idx1].isNode ? wraps[idx1].nodeIdx : -(wraps[idx1].leafIdx + 1);
        node.index1 = wraps[idx2].isNode ? wraps[idx2].nodeIdx : -(wraps[idx2].leafIdx + 1);
        int nodeIdx = s_huffNodeCount++;
        s_huffNodes[nodeIdx] = node;

        int mergeIdx = idx1 < idx2 ? idx1 : idx2;
        int nukeIdx = idx1 > idx2 ? idx1 : idx2;
        wraps[mergeIdx] = {nodeIdx, -1, true};
        if (nukeIdx != wrapCount - 1)
            wraps[nukeIdx] = wraps[wrapCount - 1];
        wrapCount--;
    }

    s_huffNodes[0] = wraps[0].isNode ? s_huffNodes[wraps[0].nodeIdx] : HuffNode{0,0,0};

    struct StackFrame { int code, nodeIdx, depth; };
    StackFrame stack[256];
    int sp = 0;
    stack[sp++] = {0, 0, 0};
    while (sp > 0) {
        StackFrame f = stack[--sp];
        if (f.nodeIdx < 0) {
            HuffLeaf& leaf = s_huffLeaves[-(f.nodeIdx + 1)];
            leaf.code = f.code;
            leaf.numBits = f.depth;
        } else {
            HuffNode& n = s_huffNodes[f.nodeIdx];
            stack[sp++] = {f.code | (1 << f.depth), n.index1, f.depth + 1};
            stack[sp++] = {f.code, n.index0, f.depth + 1};
        }
    }
}

std::string BitStream::readHuffBuffer() {
    buildHuffmanTables();
    if (readFlag()) {
        int len = readInt(8);
        std::string r; r.reserve(len);
        for (int i = 0; i < len; i++) {
            int idx = 0;
            while (true) {
                if (idx >= 0) {
                    bool f = readFlag();
                    idx = f ? s_huffNodes[idx].index1 : s_huffNodes[idx].index0;
                } else {
                    r += (char)s_huffLeaves[-(idx + 1)].symbol;
                    break;
                }
            }
        }
        return r;
    } else {
        int len = readInt(8);
        std::string r; r.reserve(len);
        for (int i = 0; i < len; i++) r += (char)readU8();
        return r;
    }
}

// ═══════════════════════════════════════════════════════════════
// BitStream
// ═══════════════════════════════════════════════════════════════
BitStream::BitStream(const uint8_t* d, size_t sz, size_t bitOff)
    : data(d), dataLen(sz), bitNum((int)bitOff), maxReadBitNum((int)(sz * 8)), error(false) {}

bool BitStream::readFlag() {
    if (bitNum >= maxReadBitNum) { error = true; return false; }
    bool ret = (data[bitNum >> 3] & (1 << (bitNum & 7))) != 0;
    bitNum++;
    return ret;
}

int BitStream::readInt(int bitCount) {
    if (bitCount <= 0 || bitCount > 32) { error = true; return 0; }
    if (bitNum + bitCount > maxReadBitNum) { error = true; return 0; }
    int startByte = bitNum >> 3;
    int downShift = bitNum & 7;
    bitNum += bitCount;
    uint64_t val = 0;
    int bytesNeeded = (bitCount + downShift + 7) >> 3;
    for (int i = 0; i < bytesNeeded && (startByte + i) < (int)dataLen; i++)
        val |= (uint64_t)data[startByte + i] << (i * 8);
    val >>= downShift;
    if (bitCount < 32) val &= (1u << bitCount) - 1;
    return (int)(uint32_t)val;
}

int BitStream::readSignedInt(int bitCount) {
    if (bitCount < 1 || bitCount > 32) { error = true; return 0; }
    bool neg = readFlag();
    int mag = readInt(bitCount - 1);
    return neg ? -mag : mag;
}

float BitStream::readFloat(int bitCount) {
    if (bitCount < 1 || bitCount > 31) { error = true; return 0.0f; }
    return (float)readInt(bitCount) / (float)((1u << bitCount) - 1u);
}

float BitStream::readSignedFloat(int bitCount) {
    if (bitCount < 1 || bitCount > 31) { error = true; return 0.0f; }
    return ((float)readInt(bitCount) * 2.0f) / (float)((1u << bitCount) - 1u) - 1.0f;
}

int BitStream::readRangedU32(int rangeStart, int rangeEnd) {
    if (rangeEnd < rangeStart) { error = true; return rangeStart; }
    // getBinLog2(getNextPow2(size)) bits; a single-value range reads none.
    // The engine does not validate the value, so neither does the stream.
    const unsigned rangeSize = (unsigned)(rangeEnd - rangeStart) + 1u;
    int bits = 0;
    while (bits < 31 && (1u << bits) < rangeSize) bits++;
    const int value = (bits > 0 ? readInt(bits) : 0) + rangeStart;
    return std::min(value, rangeEnd);
}

uint8_t BitStream::readU8() { return (uint8_t)readInt(8); }
uint16_t BitStream::readU16() { return (uint16_t)readInt(16); }
uint32_t BitStream::readU32() { return (uint32_t)readInt(32); }
int32_t  BitStream::readS32() { return (int32_t)readU32(); }

    float BitStream::readF32() {
        if (bitNum + 32 > maxReadBitNum) { error = true; return 0; }
        int startByte = bitNum >> 3, downShift = bitNum & 7;
        bitNum += 32;
        uint8_t u8[4];
        if (downShift == 0) {
            u8[0] = data[startByte]; u8[1] = data[startByte+1];
            u8[2] = data[startByte+2]; u8[3] = data[startByte+3];
        } else {
            int upShift = 8 - downShift;
            for (int i = 0; i < 4; i++) {
                uint8_t cb = data[startByte + i];
                uint8_t nb = (startByte + i + 1 < (int)dataLen) ? data[startByte + i + 1] : 0;
                u8[i] = (uint8_t)(((cb >> downShift) | (nb << upShift)) & 0xff);
            }
        }
        float val; memcpy(&val, u8, 4); return val;
    }

bool BitStream::readBool() { return readU8() != 0; }
Vec3 BitStream::readPoint3F() { Vec3 v; v.x=readF32(); v.y=readF32(); v.z=readF32(); return v; }

Vec3 BitStream::readNormalVector(int bitCount) {
    float phi = readSignedFloat(bitCount + 1) * (float)M_PI;
    float theta = readSignedFloat(bitCount) * (float)(M_PI / 2.0);
    Vec3 v;
    v.x = sinf(phi) * cosf(theta);
    v.y = cosf(phi) * cosf(theta);
    v.z = sinf(theta);
    return v;
}

Vec3 BitStream::readCompressedPoint(const Vec3& cp, float scale) {
    if (!std::isfinite(scale) || scale <= 0.0f) { error = true; return cp; }
    int type = readInt(2);
    if (type == 3) return readPoint3F();
    static const int bitCounts[] = {16, 18, 20};
    int bits = bitCounts[type];
    Vec3 v;
    v.x = cp.x + (float)readSignedInt(bits) * scale;
    v.y = cp.y + (float)readSignedInt(bits) * scale;
    v.z = cp.z + (float)readSignedInt(bits) * scale;
    return v;
}

BitStream::AffineTransform BitStream::readAffineTransform(const Vec3& cp) {
    AffineTransform at;
    at.position = readCompressedPoint(cp);
    float qx=readF32(), qy=readF32(), qz=readF32();
    float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
    if (readFlag()) qw = -qw;
    at.rotation = {qx, qy, qz, qw};
    return at;
}

std::string BitStream::readString() {
    // BitStream::readString: with the string buffer enabled, the prefix flag
    // is always present, including for the first string after enabling it.
    if (stringBufferEnabled && readFlag()) {
        int offset = readInt(8);
        stringBuffer = stringBuffer.substr(0, offset) + readHuffBuffer();
    } else {
        stringBuffer = readHuffBuffer();
    }
    return stringBuffer;
}

std::string BitStream::readRawString() {
    // Raw string format: U8 length + raw bytes (used in v24834)
    int len = readInt(8);
    if (len < 0 || len > 256 || isError()) return "";
    std::string r;
    r.resize(len);
    for (int i = 0; i < len; i++)
        r[i] = (char)readU8();
    return r;
}

std::string BitStream::unpackNetString() {
    int code = readInt(2);
    switch (code) {
        case 0: return ""; // null
        case 1: return readString(); // Huffman
        case 2: { // tagged string ref
            int tag = readInt(10);
            return "\\x01" + std::to_string(tag);
        }
        case 3: { // integer
            bool neg = readFlag();
            int num;
            if (readFlag()) num = readInt(7);       // small
            else if (readFlag()) num = readInt(15); // medium
            else num = readInt(31);                 // large
            if (neg) num = -num;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", num);
            return buf;
        }
        default: return "";
    }
}

int* BitStream::readMatrixF(Vec3* outPos) {
    static float elements[16];
    for (int i = 0; i < 16; i++) elements[i] = readF32();
    // Torque MatrixF is row-major: the translation is column 3.
    if (outPos) { outPos->x = elements[3]; outPos->y = elements[7]; outPos->z = elements[11]; }
    return (int*)elements;
}

// ─── Read/discard a T2 Move from the bitstream ─────────────────
static void readMove(BitStream& bs) {
    if (bs.readFlag()) bs.readInt(16); // pyaw
    if (bs.readFlag()) bs.readInt(16); // ppitch
    if (bs.readFlag()) bs.readInt(16); // proll
    bs.readInt(6); bs.readInt(6); bs.readInt(6); // px, py, pz
    bs.readFlag(); // freeLook
    for (int i = 0; i < T2Demo::MaxTriggerKeys; i++) bs.readFlag(); // triggers
}

// ═══════════════════════════════════════════════════════════════
// GhostTracker
// ═══════════════════════════════════════════════════════════════
bool GhostTracker::hasGhost(int index) const { return ghosts.find(index) != ghosts.end(); }
const GhostEntry* GhostTracker::getGhost(int index) const {
    auto it = ghosts.find(index); return it != ghosts.end() ? &it->second : nullptr;
}
GhostEntry* GhostTracker::getMutableGhost(int index) {
    auto it = ghosts.find(index); return it != ghosts.end() ? &it->second : nullptr;
}
void GhostTracker::createGhost(int index, int classId, const std::string& cn) {
    const char* registeredName = (index >= 0 && classId >= 0)
        ? V12::ghostClassName((size_t)classId) : nullptr;
    if (index >= T2Demo::MaxGhostCount || index < 0 || classId < 0 || classId >= 128) {
        Console::instance().printf(LogLevel::Error,
            "Demo: rejecting invalid ghost index/class (%d/%d)", index, classId);
        return;
    }
    GhostEntry e;
    e.classId = classId;
    e.className = registeredName ? registeredName :
        (cn.empty() ? "Class" + std::to_string(classId) : cn);
    ghosts[index] = e;
}
void GhostTracker::deleteGhost(int index) { ghosts.erase(index); }
void GhostTracker::clear() { ghosts.clear(); }
int GhostTracker::size() const { return (int)ghosts.size(); }
std::vector<int> GhostTracker::getAllIndices() const {
    std::vector<int> r;
    r.reserve(ghosts.size());
    for (const auto& [k, v] : ghosts) r.push_back(k);
    // Ghost IDs are the native presentation order for observer targets and
    // player lists. Do not expose unordered_map iteration order to callers.
    std::sort(r.begin(), r.end());
    return r;
}

// ═══════════════════════════════════════════════════════════════
// DemoParser
// ═══════════════════════════════════════════════════════════════
DemoParser::DemoParser() {}
DemoParser::~DemoParser() {
    if (ownsBuffer && buf) free((void*)buf);
    if (decompressed) free(decompressed);
}

bool DemoParser::loadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(sz);
    if (!b) { fclose(f); return false; }
    size_t readBytes = fread(b, 1, sz, f); fclose(f);
    if ((long)readBytes != sz) { free(b); Console::instance().printf(LogLevel::Error, "Demo: short read %s", path); return false; }
    bool ok = loadData(b, (size_t)sz);
    free(b);
    return ok;
}

bool DemoParser::loadData(const uint8_t* data, size_t size) {
    uint8_t* b = (uint8_t*)malloc(size);
    if (!b) return false;
    memcpy(b, data, size);
    bool ok = load(b, size);
    if (ownsBuffer) free((void*)buf);
    buf = b; bufSize = size; ownsBuffer = true;
    return ok;
}

// ─── Header ──────────────────────────────────────────────────
void DemoParser::readHeader() {
    if (offset + 1 > bufSize) return;
    int strLen = buf[offset++];
    if (offset + strLen > bufSize) return;
    header.identString.assign((const char*)buf + offset, strLen);
    offset += strLen;
    auto r32 = [&]() -> uint32_t {
        if (offset + 4 > bufSize) return 0;
        uint32_t v = (uint32_t)buf[offset]|((uint32_t)buf[offset+1]<<8)|((uint32_t)buf[offset+2]<<16)|((uint32_t)buf[offset+3]<<24);
        offset += 4; return v;
    };
    header.protocolVersion = r32();
    header.demoLengthMs = r32();
    header.initialBlockSize = r32();
}

// ─── Initial Block Sub-Readers ───
QueuedMove DemoParser::readQueuedMove(BitStream& bs) {
    QueuedMove move{};
    move.pyaw = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    move.ppitch = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    move.proll = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    move.px = (uint32_t)bs.readInt(6);
    move.py = (uint32_t)bs.readInt(6);
    move.pz = (uint32_t)bs.readInt(6);
    move.freeLook = bs.readFlag();
    for (bool& trigger : move.trigger) trigger = bs.readFlag();
    return move;
}

std::vector<std::string> DemoParser::readDemoValues(BitStream& bs) {
    std::vector<std::string> values;
    while (bs.readFlag()) values.push_back(bs.readString());
    return values;
}

void DemoParser::readTaggedStrings(BitStream& bs) {
    for (int id = 0; id < T2Demo::TaggedStringCount && !bs.isError(); ++id)
        if (bs.readFlag())
            initialBlock.taggedStrings[id] = bs.readString();
}

void DemoParser::readComplexTargetManager(BitStream& bs) {
    bs.readU8(); bs.readU8(); bs.readU8(); bs.readU8();
    for (int group = 0; group < 32; group++)
        for (int tg = 0; tg < 32; tg++)
            if (bs.readFlag()) { bs.readU8(); bs.readU8(); bs.readU8(); bs.readU8(); }
    for (int i = 0; i < 512; i++) {
        if (!bs.readFlag()) continue;
        TargetEntry te;
        te.targetId = i;
        if (bs.readFlag()) te.sensorData = bs.readF32();
        if (bs.readFlag()) te.voiceMapData = bs.readF32();
        if (bs.readFlag()) te.name = bs.readString();
        if (bs.readFlag()) te.skin = bs.readString();
        if (bs.readFlag()) te.skinPref = bs.readString();
        if (bs.readFlag()) te.voice = bs.readString();
        if (bs.readFlag()) te.typeDescription = bs.readString();
        te.sensorGroup = bs.readInt(5);
        te.targetData = bs.readInt(9);
        if (i >= 32 && bs.readFlag()) te.dataBlockRef = bs.readInt(11);
        te.damageLevel = bs.readFloat(7);
        initialBlock.targetEntries.push_back(te);
        DemoTargetState& target = initialTargets_[i];
        target.name = te.name;
        target.skin = te.skin;
        target.type = te.typeDescription;
        target.sensorGroup = te.sensorGroup;
        target.renderFlags = te.targetData;
        target.hasRenderFlags = true;
    }
}

void DemoParser::readSimpleTargetManager(BitStream& bs) {
    bs.readU8(); bs.readU32(); bs.readU32(); bs.readU32(); bs.readU32();
}

void DemoParser::readConnectionProtocol(BitStream& bs) {
    for (int i = 0; i < 32; i++)
        initialBlock.connectionState.lastSeqRecvdAtSend[i] = bs.readU32();
    initialBlock.connectionState.lastSeqRecvd = bs.readU32();
    initialBlock.connectionState.highestAckedSeq = bs.readU32();
    initialBlock.connectionState.lastSendSeq = bs.readU32();
    initialBlock.connectionState.ackMask = bs.readU32();
    initialBlock.connectionState.connectSequence = bs.readU32();
    initialBlock.connectionState.lastRecvAckAck = bs.readU32();
    initialBlock.connectionState.connectionEstablished = bs.readBool();
}

std::vector<PathManagerEntry> DemoParser::readPathManager(BitStream& bs) {
    std::vector<PathManagerEntry> entries;
    int entryCount = (int)bs.readU32();
    for (int i = 0; i < entryCount; i++) {
        PathManagerEntry e;
        e.entryId = bs.readU32();
        int recCount = (int)bs.readU32();
        for (int j = 0; j < recCount; j++) {
            PathManagerRecord r;
            r.field0 = bs.readU32();
            r.field1 = bs.readU32();
            r.field2 = bs.readU32();
            r.auxField = bs.readU32();
            e.records.push_back(r);
        }
        entries.push_back(e);
    }
    return entries;
}

static bool readGhostClassData(BitStream& bs, int classId, bool isInitial,
                               const Vec3& cp, GhostEntry* entry);

void DemoParser::applyTarget(GhostEntry& ghost) const {
    auto it = targets_.find(ghost.targetId);
    if (it == targets_.end()) return;
    const DemoTargetState& target = it->second;
    if (!target.name.empty()) ghost.playerName = target.name;
    if (!target.skin.empty()) ghost.skinName = target.skin;
    if (!target.type.empty()) ghost.targetType = target.type;
    if (target.sensorGroup >= 0) ghost.sensorGroup = target.sensorGroup;
    if (target.hasRenderFlags) {
        ghost.targetRenderFlags = target.renderFlags;
        // Render bit 0x2 marks CTF flags; on a player's own target it marks
        // the carrier instead.
        ghost.isFlag = (target.renderFlags & 0x2) != 0 &&
            !ObserverParity::isPlayerClass(ghost.className);
        ghost.flagTeamId = ghost.isFlag ? target.sensorGroup : 0;
        if (ghost.isFlag) ghost.teamId = target.sensorGroup;
    }
}

void DemoParser::readEventStartBlock(BitStream& bs) {
    initialBlock.nextRecvEventSeq = bs.readU32();
    while (bs.readFlag() && !bs.isError()) {
        NetEventInfo ev{};
        ev.classId = bs.readInt(T2Demo::NetEventClassBitSize) + T2Demo::NetEventClassFirst;
        ev.guaranteed = true;
        ev.dataBitsStart = bs.getCurPos();
        const bool decoded = readEventPayload(bs, ev, Vec3{}, false);
        ev.dataBitsEnd = bs.getCurPos();
        initialBlock.initialEvents.push_back(ev);
        if (!decoded) {
            // Without a decoder the payload length is unknown; the rest of the
            // start block cannot be read reliably.
            Console::instance().printf(LogLevel::Error,
                "Demo: unsupported start-block event class=%d at bit=%d",
                ev.classId, ev.dataBitsStart);
            bs.fail();
            break;
        }
    }
}

bool DemoParser::readGhostStartBlock(BitStream& bs, bool useIBTracker) {
    if (bs.getRemainingBits() < 32) return false;
    initialBlock.ghostingSequence = bs.readU32();
    int created = 0;
    int lastGhostIndex = -1;
    int lastGhostClass = -1;
    while (bs.readFlag() && !bs.isError()) {
        GhostUpdate gu{};
        gu.index = bs.readInt(T2Demo::GhostIdBitSize);
        gu.classId = bs.readInt(T2Demo::NetObjectClassBitSize) + T2Demo::NetObjectClassFirst;
        lastGhostIndex = gu.index;
        lastGhostClass = gu.classId;
        std::string cn;
        if (const char* name = V12::ghostClassName((size_t)gu.classId)) cn = name;
        else
            cn = "Class" + std::to_string(gu.classId);
        GhostTracker& tracker = useIBTracker ? ibGhostTracker : ghostTracker;
        tracker.createGhost(gu.index, gu.classId, cn);
        GhostEntry* entry = tracker.getMutableGhost(gu.index);
        if (!entry) return false;
        gu.type = GhostUpdate::Create;
        gu.updateBitsStart = bs.getCurPos();
        if (!readGhostClassData(bs, gu.classId, true, Vec3{}, entry)) {
            Console::instance().printf(LogLevel::Error,
                "Demo: unsupported initial ghost class %d (%s)",
                gu.classId, cn.c_str());
            return false;
        }
        gu.updateBitsEnd = bs.getCurPos();
        initialBlock.initialGhosts.push_back(gu);
        created++;
    }
    if (bs.isError()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: ghost loop exhausted at bit %d after %d ghosts (last %d/%d)",
            bs.getCurPos(), created, lastGhostIndex, lastGhostClass);
        return false;
    }

    Console::instance().printf(LogLevel::Debug, "GhostStartBlock: %d ghosts created, seq=%u, ctrlIdx=%d",
        created, initialBlock.ghostingSequence, -1);
    return true;
}

static bool readInitialControlPacket(BitStream& bs, const GhostEntry& ghost,
                                     float& viewYaw, float& viewPitch, Vec3& compressionPoint) {
    if (ghost.classId != 4 && ghost.classId != 25) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial control packet parser missing for class %d (%s)",
            ghost.classId, ghost.className.c_str());
        return false;
    }

    bs.readF32(); // energy level
    bs.readF32(); // recharge rate
    if (ghost.classId == 4) {
        const Vec3 position = bs.readPoint3F();
        viewPitch = bs.readF32(); // rotation X
        viewYaw = bs.readF32(); // rotation Z
        const int mode = bs.readInt(3);
        if (mode == 3 || mode == 4) {
            bs.readF32();
            bs.readF32();
            bs.readF32();
            if (mode == 3) {
                bs.readFlag();
                bs.readInt(T2Demo::GhostIdBitSize);
            } else {
                bs.readCompressedPoint(compressionPoint);
            }
        }
        compressionPoint = position;
    } else {
        // Player::readPacketData after ShapeBase::readPacketData.
        bs.readInt(3); // action state
        if (bs.readFlag()) bs.readInt(7); // recover ticks
        if (bs.readFlag()) bs.readInt(7); // jump delay
        if (bs.readFlag()) {
            compressionPoint = bs.readPoint3F();
            bs.readPoint3F(); // velocity
            bs.readInt(4); // jump surface last contact
        }
        viewPitch = bs.readF32(); // head X
        bs.readF32(); // head Z
        viewYaw = bs.readF32(); // rotation Z

        if (bs.readFlag()) {
            const int pilotedIndex = bs.readInt(T2Demo::GhostIdBitSize);
            // A Player can recursively delegate to its piloted Vehicle. The
            // initial stream must consume that virtual readPacketData too.
            if (pilotedIndex >= 0) {
                bs.readF32(); // vehicle energy level
                bs.readF32(); // vehicle recharge rate
                bs.readF32(); // steering X
                bs.readF32(); // steering Y
                const Vec3 vehiclePosition = bs.readPoint3F();
                bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32(); // orientation
                bs.readPoint3F(); // linear momentum
                bs.readPoint3F(); // angular momentum
                bs.readFlag(); // disable move
                bs.readFlag(); // frozen
                compressionPoint = vehiclePosition;
            }
        }
        bs.readFlag(); // disable move
        bs.readFlag(); // pilot
    }
    return !bs.isError();
}

bool DemoParser::readDataBlocks(BitStream& bs) {
    int count = 0;
    while (bs.readFlag()) {
        if (++count > 4096) {
            Console::instance().printf(LogLevel::Error,
                "Demo: too many initial datablocks");
            return false;
        }

        // The outer flag is the event list entry. SimDataBlockEvent::pack
        // writes its own process flag before the object metadata.
        if (!bs.readFlag()) continue;

        DataBlockHeader header{};
        const uint32_t objectIndex = (uint32_t)bs.readInt(11);
        header.objectId = objectIndex;
        header.classId = (uint32_t)bs.readInt(7) + T2Demo::DataBlockClassFirst;
        header.index = (uint32_t)bs.readInt(11);
        header.total = (uint32_t)bs.readInt(12);
        header.dataBitsStart = bs.getCurPos();

        if (bs.isError()) return false;

        V12::DecodedDataBlock decoded;
        V12BitStream payload(bs.getBuffer(), bs.getBufferSize(),
                             (size_t)header.dataBitsStart);
        if (!V12::readDataBlockPayload(payload, header.classId, &decoded)) {
            const char* className = V12::dataBlockClassName(header.classId - T2Demo::DataBlockClassFirst);
            Console::instance().printf(LogLevel::Error,
                "Demo: unsupported or malformed initial datablock class %u%s",
                header.classId, className ? className : "");
            return false;
        }

        const size_t consumed = payload.position() - (size_t)header.dataBitsStart;
        if (consumed > (size_t)bs.getRemainingBits()) return false;
        bs.setCurPos(header.dataBitsStart + (int)consumed);
        initialBlock.dataBlockHeaders.push_back(header);
        ParsedDataBlock parsed;
        parsed.classId = header.classId;
        parsed.decoded = decoded;
        if (const char* name = V12::dataBlockClassName(header.classId - T2Demo::DataBlockClassFirst))
            parsed.className = name;
        parsed.objectId = header.objectId;
        if (!decoded.shapeFile.empty()) {
            parsed.data["shapeFile"] = decoded.shapeFile;
            initialBlock.datablockWeaponShapes[header.objectId] = decoded.shapeFile;
        }
        if (!decoded.debrisShape.empty()) parsed.data["debrisShape"] = decoded.debrisShape;
        if (!decoded.cloakTexture.empty()) parsed.data["cloakTexture"] = decoded.cloakTexture;
        initialBlock.dataBlocks[header.objectId] = std::move(parsed);
    }

    initialBlock.dataBlockCount = count;
    Console::instance().printf(LogLevel::Debug,
        "DataBlocks: %d decoded", initialBlock.dataBlockCount);
    return !bs.isError();
}

// Forward declaration
static bool readGhostClassData(BitStream& bs, int classId, bool isInitial, const Vec3& cp, GhostEntry* entry);

const std::string& DemoParser::getPlayerNameForSkin(const std::string& skin) const {
    static std::string empty;
    auto it = skinToPlayer_.find(skin);
    return it != skinToPlayer_.end() ? it->second : empty;
}

bool DemoParser::readInitialBlock(const uint8_t* data, size_t size, uint32_t protocolVersion) {
    BitStream bs(data, size);
    auto fail = [&](const char* stage) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial block parse failed at %s (bit %d/%d)",
            stage, bs.getCurPos(), bs.getMaxPos());
        return false;
    };
    // Tribes 2's build-25034 GameConnection reader starts with the tagged
    // string table and a separate datablock count. This is not the generic
    // Torque3D start-block order.
    readTaggedStrings(bs);
    const uint32_t expectedDataBlocks = bs.readU32();
    Console::instance().printf(LogLevel::Debug,
        "Demo: initial tagged strings=%zu expected datablocks=%u bit=%d",
        initialBlock.taggedStrings.size(), expectedDataBlocks, bs.getCurPos());
    if (expectedDataBlocks > 4096) return fail("datablock count");
    if (!readDataBlocks(bs)) return fail("datablocks");
    if ((uint32_t)initialBlock.dataBlockCount != expectedDataBlocks) {
        Console::instance().printf(LogLevel::Warn,
            "Demo: datablock count header=%u entries=%d",
            expectedDataBlocks, initialBlock.dataBlockCount);
    }

    initialBlock.firstPerson = bs.readU8() != 0;
    initialBlock.connectionFields.clear();
    for (int i = 0; i < 6; ++i) initialBlock.connectionFields.push_back(bs.readU32());
    initialBlock.stateArray.clear();
    for (int i = 0; i < 16; ++i) initialBlock.stateArray.push_back(bs.readU32());

    // A packed move is at least 3 + 18 + 1 + 6 = 28 bits.
    const uint32_t moveCount = bs.readU32();
    if ((uint64_t)moveCount * 28 > (uint64_t)bs.getRemainingBits())
        return fail("move count");
    initialBlock.queuedMoves.clear();
    for (uint32_t i = 0; i < moveCount; ++i)
        initialBlock.queuedMoves.push_back(readQueuedMove(bs));
    initialBlock.demoValues = readDemoValues(bs);
    extractMissionInfo();
    readComplexTargetManager(bs);
    if (bs.isError()) return fail("target manager");

    readConnectionProtocol(bs);
    initialBlock.roundTripTime = bs.readF32();
    initialBlock.packetLoss = bs.readF32();
    readPathManager(bs);
    initialBlock.notifyCount = bs.readU32();
    readEventStartBlock(bs);
    if (!readGhostStartBlock(bs, true) || bs.isError())
        return fail("events or ghosts");

    initialBlock.controlObjectGhostIndex = bs.readS32();
    if (initialBlock.controlObjectGhostIndex >= 0) {
        const GhostEntry* control = ibGhostTracker.getGhost(
            initialBlock.controlObjectGhostIndex);
        if (!control || !readInitialControlPacket(bs, *control, initialBlock.controlYaw,
                                                  initialBlock.controlPitch,
                                                  initialBlock.initialCompressionPoint))
            return fail("control object");
        initialBlock.hasControlRotation = true;
    }
    initialBlock.missionName = bs.readString();
    initialBlock.missionCRC = bs.readU32();
    readSimpleTargetManager(bs);
    readSimpleTargetManager(bs);
    return bs.isError() ? fail("mission tail") : true;
}

// ─── Block stream ────────────────────────────────────────────
bool DemoParser::load(const uint8_t* buffer, size_t size) {
    if (decompressed) {
        free(decompressed);
        decompressed = nullptr;
    }
    header = {};
    initialBlock = {};
    ibGhostTracker.clear();
    ghostTracker.clear();
    playerInfo_.clear();
    initialTaggedStrings_.clear();
    initialPlayerInfo_.clear();
    skinToPlayer_.clear();
    targets_.clear();
    initialTargets_.clear();
    missionChanges_.clear();
    missionCrcChanges_.clear();
    eventLog_.clear();
    decompressedSize = 0;
    moveTicksBefore_.clear();
    packetsParsed = 0;
    parseFault_.clear();
    packetsDroppedAfterFault_ = 0;
    s_pendingExplosions.clear();
    buf = buffer; bufSize = size; offset = 0; ownsBuffer = false;
    decompressed = nullptr; decompressedSize = 0;

    readHeader();
    if (header.identString != "Tribes2 Recording") {
        Console::instance().printf(LogLevel::Error,
            "Demo: unsupported recording signature '%s'", header.identString.c_str());
        return false;
    }
    if (header.protocolVersion != T2Demo::ProtocolV25034) {
        Console::instance().printf(LogLevel::Error,
            "Demo: unsupported protocol 0x%08X (native parser supports 0x%08X)",
            (unsigned)header.protocolVersion,
            (unsigned)T2Demo::ProtocolV25034);
        return false;
    }

    {
        const char* ver = "unknown";
        if (header.protocolVersion == T2Demo::ProtocolV24834) ver = "24834";
        else if (header.protocolVersion == T2Demo::ProtocolV25034) ver = "25034";
        Console::instance().printf(LogLevel::Debug, "Demo: protocol 0x%08X (v%s), %u bytes initial block",
            (unsigned)header.protocolVersion, ver, (unsigned)header.initialBlockSize);
    }

    if (offset + header.initialBlockSize > bufSize) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial block exceeds file size");
        return false;
    }
    if (!readInitialBlock(buf + offset, header.initialBlockSize, header.protocolVersion)) {
        Console::instance().printf(LogLevel::Error,
            "Demo: invalid native initial block");
        return false;
    }
    ghostTracker.clear();
    for (int index : ibGhostTracker.getAllIndices()) {
        const GhostEntry* source = ibGhostTracker.getGhost(index);
        if (!source) continue;
        ghostTracker.createGhost(index, source->classId, source->className);
        if (GhostEntry* target = ghostTracker.getMutableGhost(index))
            *target = *source;
    }
    offset += header.initialBlockSize;

    playerInfo_.clear();
    for (const auto& target : initialBlock.targetEntries) {
        if (target.name.empty()) continue;
        DemoPlayerInfo player;
        player.name = target.name;
        player.skin = target.skin;
        player.teamId = target.sensorGroup;
        player.damage = target.damageLevel;
        player.clientId = target.targetId;
        playerInfo_.push_back(std::move(player));
    }
    // The recordings.cs PLAYERLIST demo value uses the same roster field
    // order as MapGenius: name, ..., client id, ..., ping, packet loss.
    if (initialBlock.demoValues.size() >= 2) {
        size_t valueIndex = 1; // MISC precedes PLAYERLIST.
        const int playerCount = std::max(0, atoi(initialBlock.demoValues[valueIndex++].c_str()));
        for (int i = 0; i < playerCount && valueIndex < initialBlock.demoValues.size(); ++i) {
            std::string field;
            std::vector<std::string> fields;
            std::stringstream row(initialBlock.demoValues[valueIndex++]);
            while (std::getline(row, field, '\t')) fields.push_back(field);
            if (fields.size() < 8) continue;
            const int clientId = atoi(fields[2].c_str());
            auto player = std::find_if(playerInfo_.begin(), playerInfo_.end(),
                [&](const DemoPlayerInfo& entry) { return entry.clientId == clientId; });
            if (player != playerInfo_.end()) {
                player->ping = atoi(fields[6].c_str());
                player->packetLoss = atoi(fields[7].c_str());
            }
        }
        // recordings.cs getState order after PLAYERLIST: RETICLE, BACKPACK,
        // WEAPON, INVENTORY (then SCORE, CLOCK, CHAT, GRAVITY).
        const auto& values = initialBlock.demoValues;
        auto next = [&]() -> std::vector<std::string> {
            std::vector<std::string> fields;
            if (valueIndex >= values.size()) return fields;
            std::string field;
            std::stringstream row(values[valueIndex++]);
            while (std::getline(row, field, '\t')) fields.push_back(field);
            return fields;
        };
        auto field = [](const std::vector<std::string>& fields, size_t i) {
            return i < fields.size() ? fields[i] : std::string();
        };
        initialWeaponsHud_ = {};
        initialBackpackHud_ = {};
        initialInventoryHud_ = {};
        initialAmmoHud_ = {};
        // RETICLE: bitmap, visible, centre, ammo visible, ammo value, ...
        const auto reticle = next();
        if (!field(reticle, 4).empty()) initialAmmoHud_.count = atoi(field(reticle, 4).c_str());
        // BACKPACK: icon bitmap, frame visible, text, text visible, pack.
        const auto backpack = next();
        initialBackpackHud_.bitmap = field(backpack, 0);
        initialBackpackHud_.active = atoi(field(backpack, 1).c_str()) != 0;
        initialBackpackHud_.text = field(backpack, 2);
        if (!field(backpack, 4).empty()) initialBackpackHud_.packIndex = atoi(field(backpack, 4).c_str());
        // WEAPON: visible, background, highlight, infinite, count, slots,
        // active; count rows of (name, bitmap); slot rows of (id, count).
        const auto weapons = next();
        if (weapons.size() >= 7) {
            initialWeaponsHud_.backgroundBitmap = weapons[1];
            initialWeaponsHud_.highlightBitmap = weapons[2];
            initialWeaponsHud_.infiniteAmmoBitmap = weapons[3];
            const int count = std::clamp(atoi(weapons[4].c_str()), 0, 64);
            const int slotCount = std::clamp(atoi(weapons[5].c_str()), 0, 64);
            initialWeaponsHud_.activeIndex = atoi(weapons[6].c_str());
            for (int i = 0; i < count; ++i) {
                const auto item = next();
                if (!field(item, 1).empty()) initialWeaponsHud_.bitmaps[i] = field(item, 1);
            }
            for (int i = 0; i < slotCount; ++i) {
                const auto slot = next();
                if (!field(slot, 0).empty())
                    initialWeaponsHud_.slots[atoi(slot[0].c_str())] = atoi(field(slot, 1).c_str());
            }
        }
        // INVENTORY: the same header; count rows of (bitmap); slot rows.
        const auto inventory = next();
        if (inventory.size() >= 7) {
            initialInventoryHud_.backgroundBitmap = inventory[1];
            const int count = std::clamp(atoi(inventory[4].c_str()), 0, 64);
            const int slotCount = std::clamp(atoi(inventory[5].c_str()), 0, 64);
            for (int i = 0; i < count; ++i) {
                const auto item = next();
                if (!field(item, 0).empty()) initialInventoryHud_.bitmaps[i] = field(item, 0);
            }
            for (int i = 0; i < slotCount; ++i) {
                const auto slot = next();
                if (!field(slot, 0).empty())
                    initialInventoryHud_.slots[atoi(slot[0].c_str())] = atoi(field(slot, 1).c_str());
            }
        }
    }
    initialTaggedStrings_ = initialBlock.taggedStrings;
    initialPlayerInfo_ = playerInfo_;
    resetHudState();
    // Start-block ghosts carry target ids; resolve them against the initial
    // TargetManager state.
    targets_ = initialTargets_;
    for (GhostTracker* tracker : {&ibGhostTracker, &ghostTracker}) {
        for (int index : tracker->getAllIndices())
            if (GhostEntry* ghost = tracker->getMutableGhost(index))
                if (ghost->targetId >= 0) applyTarget(*ghost);
    }

    // Decompress block stream (raw deflate)
    size_t compSize = bufSize - offset;
    if (compSize == 0) return false;
    constexpr size_t kMaxCompressedBlock = 128u * 1024u * 1024u;
    constexpr size_t kMaxDecompressedBlock = 512u * 1024u * 1024u;
    if (compSize > kMaxCompressedBlock) {
        Console::instance().printf(LogLevel::Error,
            "Demo: compressed block exceeds safety limit");
        return false;
    }

    z_stream strm{};
    strm.next_in = (Bytef*)(buf + offset);
    strm.avail_in = (uInt)compSize;
    int ret = inflateInit2(&strm, -15);
    if (ret != Z_OK) return false;

    if (compSize > kMaxDecompressedBlock / 4) return false;
    size_t outSize = std::max(compSize * 4, (size_t)262144);
    if (outSize > kMaxDecompressedBlock) outSize = kMaxDecompressedBlock;
    decompressed = (uint8_t*)malloc(outSize);
    strm.next_out = decompressed;
    strm.avail_out = (uInt)outSize;

    ret = inflate(&strm, Z_FINISH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        inflateEnd(&strm);
        memset(&strm, 0, sizeof(strm));
        strm.next_in = (Bytef*)(buf + offset);
        strm.avail_in = (uInt)compSize;
        ret = inflateInit2(&strm, 15 + 32);
        if (ret != Z_OK) { free(decompressed); decompressed = nullptr; return false; }
        strm.next_out = decompressed;
        strm.avail_out = (uInt)outSize;
        if (inflate(&strm, Z_FINISH) != Z_STREAM_END) {
            free(decompressed); decompressed = nullptr;
            inflateEnd(&strm); return false;
        }
    }
    decompressedSize = strm.total_out;
    inflateEnd(&strm);

    // Init packet parser state
    std::copy(std::begin(initialBlock.connectionState.lastSeqRecvdAtSend),
              std::end(initialBlock.connectionState.lastSeqRecvdAtSend),
              std::begin(lastSeqRecvdAtSend));
    lastSeqRecvd = initialBlock.connectionState.lastSeqRecvd;
    highestAckedSeq = initialBlock.connectionState.highestAckedSeq;
    lastSendSeq = initialBlock.connectionState.lastSendSeq;
    recvAckMask = initialBlock.connectionState.ackMask;
    connectSequence = initialBlock.connectionState.connectSequence;
    lastRecvAckAck = initialBlock.connectionState.lastRecvAckAck;
    connectionEstablished = initialBlock.connectionState.connectionEstablished;
    nextRecvEventSeq = initialBlock.nextRecvEventSeq;
    packetsParsed = 0;
    compressionPoint = initialBlock.initialCompressionPoint;
    blockStreamOffset = 0; blockCursor_ = 0; blockCount_ = -1;
    ghostResets_ = 0;

    scanMissionChanges();

    return true;
}

int DemoParser::getBlockCount() {
    if (blockCount_ >= 0) return blockCount_;
    if (!decompressed) { blockCount_ = 0; return 0; }
    int count = 0, off = 0;
    while (off + 2 <= (int)decompressedSize) {
        int sz = decompressed[off] | (decompressed[off+1] << 8);
        sz &= 0xfff;
        off += 2 + sz;
        if (off > (int)decompressedSize) break;
        count++;
    }
    blockCount_ = count;
    return count;
}

int DemoParser::getMoveBlockCount() const {
    if (!decompressed) return 0;
    int count = 0;
    int off = 0;
    while (off + 2 <= (int)decompressedSize) {
        const int header = decompressed[off] | (decompressed[off + 1] << 8);
        const int type = header >> 12;
        const int size = header & 0xfff;
        off += 2 + size;
        if (off > (int)decompressedSize) break;
        if (type == T2Demo::BlockTypeMove) count++;
    }
    return count;
}

const std::vector<int>& DemoParser::getMoveTicksBefore() {
    if (!moveTicksBefore_.empty() || !decompressed) return moveTicksBefore_;
    moveTicksBefore_.push_back(0);
    int off = 0;
    while (off + 2 <= (int)decompressedSize) {
        const int header = decompressed[off] | (decompressed[off + 1] << 8);
        off += 2 + (header & 0xfff);
        if (off > (int)decompressedSize) break;
        moveTicksBefore_.push_back(moveTicksBefore_.back() +
            ((header >> 12) == T2Demo::BlockTypeMove ? 1 : 0));
    }
    return moveTicksBefore_;
}

DemoBlock* DemoParser::nextBlock() {
    if (!decompressed || blockStreamOffset + 2 > (int)decompressedSize) return nullptr;
    int ts = decompressed[blockStreamOffset] | (decompressed[blockStreamOffset+1] << 8);
    int type = ts >> 12, size = ts & 0xfff;
    if (blockStreamOffset + 2 + size > (int)decompressedSize) return nullptr;

    DemoBlock* block = new DemoBlock;
    block->index = blockCursor_++;
    block->type = type;
    block->size = size;
    block->data.assign(decompressed + blockStreamOffset + 2,
                       decompressed + blockStreamOffset + 2 + size);
    blockStreamOffset += 2 + size;
    return block;
}

void DemoParser::reset() {
    blockStreamOffset = 0; blockCursor_ = 0; blockCount_ = -1;
    ghostResets_ = 0;
    compressionPoint = initialBlock.initialCompressionPoint;
    missionCrcChanges_.clear();
    initialBlock.taggedStrings = initialTaggedStrings_;
    playerInfo_ = initialPlayerInfo_;
    targets_ = initialTargets_;
    currentMissionCrc_ = initialBlock.missionCRC;
    currentMission_ = missionChanges_.empty() ? "" : missionChanges_[0].second;
    nextChangeIdx_ = 0;
    eventLog_.clear();
    resetHudState();
    s_pendingExplosions.clear();
    ghostTracker.clear();
    for (int index : ibGhostTracker.getAllIndices()) {
        const GhostEntry* source = ibGhostTracker.getGhost(index);
        if (!source) continue;
        ghostTracker.createGhost(index, source->classId, source->className);
        if (GhostEntry* target = ghostTracker.getMutableGhost(index))
            *target = *source;
    }
    std::copy(std::begin(initialBlock.connectionState.lastSeqRecvdAtSend),
              std::end(initialBlock.connectionState.lastSeqRecvdAtSend),
              std::begin(lastSeqRecvdAtSend));
    lastSeqRecvd = initialBlock.connectionState.lastSeqRecvd;
    highestAckedSeq = initialBlock.connectionState.highestAckedSeq;
    lastSendSeq = initialBlock.connectionState.lastSendSeq;
    recvAckMask = initialBlock.connectionState.ackMask;
    connectSequence = initialBlock.connectionState.connectSequence;
    lastRecvAckAck = initialBlock.connectionState.lastRecvAckAck;
    connectionEstablished = initialBlock.connectionState.connectionEstablished;
    nextRecvEventSeq = initialBlock.nextRecvEventSeq;
    packetsParsed = 0;
    parseFault_.clear();
    packetsDroppedAfterFault_ = 0;
}

void DemoParser::resetMissionState() {
    // The new mission's ghosts are the connection's (EndGhosting cleared the
    // old ones); only presentation state resets here.
    playerInfo_ = initialPlayerInfo_;
    resetHudState();
    s_pendingExplosions.clear();
}

void DemoParser::handleHudRemoteCommand(const std::string& funcName,
                                        const std::vector<std::string>& args) {
    std::string name = funcName;
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.rfind("clientcmd", 0) == 0) name.erase(0, 9);
    const bool includesFunction = !args.empty() && args[0] == funcName;
    auto arg = [&](size_t index) -> std::string {
        const size_t actual = index + (includesFunction ? 1 : 0);
        return actual < args.size() ? args[actual] : std::string();
    };
    const size_t count = args.size() - (includesFunction ? 1 : 0);
    auto has = [&](size_t required) { return count >= required; };
    auto number = [&](size_t index) { return atoi(arg(index).c_str()); };
    if (name == "setweaponshuditem" && has(3)) {
        const int slot = number(0);
        if (number(2) != 0) weaponsHud_.slots[slot] = number(1);
        else weaponsHud_.slots.erase(slot); // the item goes; its bitmap stays
    } else if (name == "setweaponshudammo" && has(2)) {
        const int slot = number(0);
        if (weaponsHud_.slots.find(slot) != weaponsHud_.slots.end())
            weaponsHud_.slots[slot] = number(1);
    } else if (name == "setweaponshudbitmap" && has(3)) {
        weaponsHud_.bitmaps[number(0)] = arg(2);
    } else if (name == "setweaponshudbackgroundbmp" && has(1)) {
        weaponsHud_.backgroundBitmap = arg(0);
    } else if (name == "setweaponshudhighlightbmp" && has(1)) {
        weaponsHud_.highlightBitmap = arg(0);
    } else if (name == "setweaponshudinfiniteammobmp" && has(1)) {
        weaponsHud_.infiniteAmmoBitmap = arg(0);
    } else if (name == "setweaponshudactive" && has(1)) {
        weaponsHud_.activeIndex = number(0);
    } else if (name == "setammohudcount" && has(1)) {
        ammoHud_.count = number(0);
    } else if (name == "setweaponshudclearall") {
        // HudWeapons::clearAll drops the carried items; the bitmaps were sent
        // once on connect and stay.
        weaponsHud_.slots.clear();
        weaponsHud_.activeIndex = -1;
    } else if (name == "setinventoryhuditem" && has(3)) {
        const int slot = number(0);
        if (number(2) != 0) inventoryHud_.slots[slot] = number(1);
        else inventoryHud_.slots.erase(slot);
    } else if (name == "setinventoryhudamount" && has(2)) {
        const int slot = number(0);
        if (inventoryHud_.slots.find(slot) != inventoryHud_.slots.end())
            inventoryHud_.slots[slot] = number(1);
    } else if (name == "setinventoryhudbitmap" && has(3)) {
        inventoryHud_.bitmaps[number(0)] = arg(2);
    } else if (name == "setinventoryhudbackgroundbmp" && has(1)) {
        inventoryHud_.backgroundBitmap = arg(0);
    } else if (name == "setinventoryhudclearall") {
        // clientCmdSetInventoryHudClearAll: clear the items and the backpack.
        inventoryHud_.slots.clear();
        backpackHud_ = {};
    } else if (name == "setbackpackhuditem" && has(2)) {
        backpackHud_.packIndex = number(0);
        backpackHud_.active = number(1) != 0;
        backpackHud_.bitmap.clear();
        if (!backpackHud_.active) {
            backpackHud_.text.clear();
        }
    } else if (name == "setbackpackhudbitmap" && has(1)) {
        backpackHud_.bitmap = arg(0);
    } else if (name == "updatepacktext" && has(1)) {
        backpackHud_.text = arg(0);
    } else if (name == "setsatchelarmed" ||
               name == "setcloakiconon" || name == "setcloakiconoff" ||
               name == "setrepairpackiconon" || name == "setrepairpackiconoff" ||
               name == "setshieldiconon" || name == "setshieldiconoff" ||
               name == "setsenjamiconon" || name == "setsenjamiconoff") {
        // These stock commands toggle the backpack icon directly in TorqueScript.
        backpackHud_.active = name == "setsatchelarmed" ||
            (name.size() >= 2 && name.substr(name.size() - 2) == "on");
        backpackHud_.bitmap.clear();
        if (!backpackHud_.active) backpackHud_.text.clear();
    } else if (name == "setvweaponshudactive" && has(1)) {
        vehicleHud_.activeWeapon = number(0);
        if (has(2)) vehicleHud_.vehicleType = arg(1);
    } else if (name == "setvweaponshudclearall") {
        vehicleHud_.activeWeapon = -1;
    } else if (name == "showvehiclegauges" && has(2)) {
        vehicleHud_.vehicleType = arg(0);
        vehicleHud_.node = number(1);
        vehicleHud_.dashboardVisible = true;
    } else if (name == "hidevehiclegauges") {
        // The stock HUD hides gauges independently of the active vehicle
        // weapon, so do not discard the vehicle identity or selected weapon.
        vehicleHud_.dashboardVisible = false;
    }
}

void DemoParser::resetHudState() {
    weaponsHud_ = initialWeaponsHud_;
    backpackHud_ = initialBackpackHud_;
    inventoryHud_ = initialInventoryHud_;
    vehicleHud_ = {};
    ammoHud_ = initialAmmoHud_;
}

DemoParserSnapshot DemoParser::captureSnapshot() const {
    DemoParserSnapshot snapshot;
    snapshot.blockStreamOffset = (size_t)std::max(0, blockStreamOffset);
    snapshot.blockCount = blockCount_;
    snapshot.blockCursor = blockCursor_;
    snapshot.ghostResets = ghostResets_;
    snapshot.ghostTracker = ghostTracker;
    snapshot.compressionPoint = compressionPoint;
    std::copy(std::begin(lastSeqRecvdAtSend), std::end(lastSeqRecvdAtSend),
              std::begin(snapshot.lastSeqRecvdAtSend));
    snapshot.lastSeqRecvd = lastSeqRecvd;
    snapshot.highestAckedSeq = highestAckedSeq;
    snapshot.lastSendSeq = lastSendSeq;
    snapshot.recvAckMask = recvAckMask;
    snapshot.connectSequence = connectSequence;
    snapshot.lastRecvAckAck = lastRecvAckAck;
    snapshot.connectionEstablished = connectionEstablished;
    snapshot.nextRecvEventSeq = nextRecvEventSeq;
    snapshot.packetsParsed = packetsParsed;
    snapshot.parseFault = parseFault_;
    snapshot.packetsDroppedAfterFault = packetsDroppedAfterFault_;
    snapshot.missionChanges = missionChanges_;
    snapshot.missionCrcChanges = missionCrcChanges_;
    snapshot.taggedStrings = initialBlock.taggedStrings;
    snapshot.currentMissionCrc = currentMissionCrc_;
    snapshot.currentMission = currentMission_;
    snapshot.nextChangeIdx = nextChangeIdx_;
    snapshot.eventLog = eventLog_;
    snapshot.playerInfo = playerInfo_;
    snapshot.skinToPlayer = skinToPlayer_;
    snapshot.targets = targets_;
    snapshot.weaponsHud = weaponsHud_;
    snapshot.backpackHud = backpackHud_;
    snapshot.inventoryHud = inventoryHud_;
    snapshot.vehicleHud = vehicleHud_;
    snapshot.ammoHud = ammoHud_;
    snapshot.pendingExplosions = s_pendingExplosions;
    return snapshot;
}

bool DemoParser::restoreSnapshot(const DemoParserSnapshot& snapshot) {
    if (!decompressed || snapshot.blockStreamOffset > decompressedSize ||
        snapshot.blockCursor < 0 || snapshot.nextChangeIdx < 0 ||
        snapshot.nextChangeIdx > (int)snapshot.missionChanges.size() ||
        (snapshot.blockCount >= 0 && snapshot.blockCursor > snapshot.blockCount))
        return false;
    blockStreamOffset = (int)snapshot.blockStreamOffset;
    blockCount_ = snapshot.blockCount;
    blockCursor_ = snapshot.blockCursor;
    ghostResets_ = snapshot.ghostResets;
    ghostTracker = snapshot.ghostTracker;
    compressionPoint = snapshot.compressionPoint;
    std::copy(std::begin(snapshot.lastSeqRecvdAtSend), std::end(snapshot.lastSeqRecvdAtSend),
              std::begin(lastSeqRecvdAtSend));
    lastSeqRecvd = snapshot.lastSeqRecvd;
    highestAckedSeq = snapshot.highestAckedSeq;
    lastSendSeq = snapshot.lastSendSeq;
    recvAckMask = snapshot.recvAckMask;
    connectSequence = snapshot.connectSequence;
    lastRecvAckAck = snapshot.lastRecvAckAck;
    connectionEstablished = snapshot.connectionEstablished;
    nextRecvEventSeq = snapshot.nextRecvEventSeq;
    packetsParsed = snapshot.packetsParsed;
    parseFault_ = snapshot.parseFault;
    packetsDroppedAfterFault_ = snapshot.packetsDroppedAfterFault;
    missionChanges_ = snapshot.missionChanges;
    missionCrcChanges_ = snapshot.missionCrcChanges;
    initialBlock.taggedStrings = snapshot.taggedStrings;
    currentMissionCrc_ = snapshot.currentMissionCrc;
    currentMission_ = snapshot.currentMission;
    nextChangeIdx_ = snapshot.nextChangeIdx;
    eventLog_ = snapshot.eventLog;
    playerInfo_ = snapshot.playerInfo;
    skinToPlayer_ = snapshot.skinToPlayer;
    targets_ = snapshot.targets;
    weaponsHud_ = snapshot.weaponsHud;
    backpackHud_ = snapshot.backpackHud;
    inventoryHud_ = snapshot.inventoryHud;
    vehicleHud_ = snapshot.vehicleHud;
    ammoHud_ = snapshot.ammoHud;
    s_pendingExplosions = snapshot.pendingExplosions;
    return true;
}

int DemoParser::processBlocks(int count) {
    int proc = 0;
    for (int i = 0; i < count; i++) {
        DemoBlock* b = nextBlock();
        if (!b) break;
        delete b; proc++;
    }
    return proc;
}

bool DemoParser::seekToBlock(int blockIndex) {
    if (!decompressed || blockIndex < 0) return false;
    if (blockIndex < blockCursor_) reset();
    while (blockCursor_ < blockIndex) {
        std::unique_ptr<DemoBlock> block(nextBlock());
        if (!block) return false;
        if (block->type == T2Demo::BlockTypeSendPacket) {
            onSendPacketTrigger();
        } else if (block->type == T2Demo::BlockTypePacket) {
            parsePacket(block->data.data(), block->data.size(), block->index);
        }
    }
    setCurrentBlock(blockIndex);
    return true;
}

bool DemoParser::parseFull(std::vector<DemoBlock>& out) {
    out.clear();
    reset();
    while (auto* b = nextBlock()) { out.push_back(*b); delete b; }
    return !out.empty();
}

// ─── Mission Change Tracking ────────────────────────────────
// Scan all blocks for .mis paths and record which block index each appears at.
// This handles cross-map-load demos.
// Extract base map name from a mission path (e.g. "Missions/Katabatic.mis" -> "Katabatic")
static std::string extractMapName(const std::string& missionPath) {
    return missionLoadPath(missionPath);
}

static bool sameMissionName(const std::string& left, const std::string& right) {
    return missionLower(left) == missionLower(right);
}

static bool isMissionPath(const std::string& value) {
    std::string path = value;
    for (char& c : path) {
        if (c == '\\') c = '/';
        c = (char)std::tolower((unsigned char)c);
    }
    if (path.rfind("missions/", 0) != 0 &&
        path.rfind("base/missions/", 0) != 0)
        return false;
    if (!path.ends_with(".mis") && !path.ends_with(".mispk")) return false;
    for (char c : value) {
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
            return false;
    }
    return true;
}

void DemoParser::scanMissionChanges() {
    missionChanges_.clear();
    missionCrcChanges_.clear();
    if (!decompressed) return;
    if (!initialBlock.missionName.empty()) {
        const std::string initial = extractMapName(initialBlock.missionName);
        if (!initial.empty()) missionChanges_.push_back({0, initial});
    }
    // Save state and reset to scan from start
    int savedOffset = blockStreamOffset;
    int savedCursor = blockCursor_;
    blockStreamOffset = 0;
    blockCursor_ = 0;
    while (true) {
        if (blockStreamOffset + 2 > (int)decompressedSize) break;
        int ts = decompressed[blockStreamOffset] | (decompressed[blockStreamOffset+1] << 8);
        int size = ts & 0xfff;
        if (blockStreamOffset + 2 + size > (int)decompressedSize) break;
        int bi = blockCursor_++;
        const uint8_t* d = decompressed + blockStreamOffset + 2;
        for (int j = 0; j + 4 <= size; j++) {
            if (d[j] == '.' && (d[j+1] == 'm' || d[j+1] == 'M') &&
                (d[j+2] == 'i' || d[j+2] == 'I') &&
                (d[j+3] == 's' || d[j+3] == 'S')) {
                int start = (int)j;
                while (start > 0 && d[start-1] != 0 &&
                       d[start-1] >= 0x20) start--;
                std::string name((const char*)d + start, j + 4 - start);
                if (isMissionPath(name)) {
                    const std::string mapName = extractMapName(name);
                    if (missionChanges_.empty() ||
                        !sameMissionName(mapName, missionChanges_.back().second))
                        missionChanges_.push_back({bi, mapName});
                }
            }
        }
        blockStreamOffset += 2 + size;
    }
    // Restore state
    blockStreamOffset = savedOffset;
    blockCursor_ = savedCursor;
    nextChangeIdx_ = 0;
    currentMission_ = missionChanges_.empty() ? "" : missionChanges_[0].second;
    currentMissionCrc_ = initialBlock.missionCRC;
}
void DemoParser::setCurrentBlock(int blockIndex) {
    if (blockIndex < 0) return;
    // Recompute from the timeline so direct backward seeks cannot retain the
    // mission selected by a later block.
    nextChangeIdx_ = 0;
    currentMission_.clear();
    while (nextChangeIdx_ < (int)missionChanges_.size() &&
           blockIndex >= missionChanges_[nextChangeIdx_].first) {
        currentMission_ = missionChanges_[nextChangeIdx_].second;
        nextChangeIdx_++;
    }
    currentMissionCrc_ = initialBlock.missionCRC;
    for (const auto& change : missionCrcChanges_) {
        if (change.first > blockIndex) break;
        currentMissionCrc_ = change.second;
    }
}

// ─── Raw Move ────────────────────────────────────────────────
DemoMove DemoParser::readRawMove(const uint8_t* d, size_t sz) {
    DemoMove m{}; if (sz < 64) return m;
    auto r32 = [&](int o) { return (int32_t)(d[o]|(d[o+1]<<8)|(d[o+2]<<16)|(d[o+3]<<24)); };
    auto ru32 = [&](int o) { return (uint32_t)(d[o]|(d[o+1]<<8)|(d[o+2]<<16)|(d[o+3]<<24)); };
    auto rf32 = [&](int o) { uint32_t u=ru32(o); float v; memcpy(&v,&u,4); return v; };
    m.px=r32(0); m.py=r32(4); m.pz=r32(8);
    m.pyaw=ru32(12); m.ppitch=ru32(16); m.proll=ru32(20);
    m.x=rf32(24); m.y=rf32(28); m.z=rf32(32);
    m.yaw=rf32(36); m.pitch=rf32(40); m.roll=rf32(44);
    m.id=ru32(48); m.sendCount=ru32(52);
    m.freeLook=d[56]!=0;
    for (int i=0;i<6;i++) m.trigger[i]=d[57+i]!=0;
    return m;
}

InfoBlock DemoParser::readInfoBlock(const uint8_t* d, size_t sz) {
    InfoBlock ib{}; if (sz<8) return ib;
    auto ru32 = [&](int o) { return (uint32_t)(d[o]|(d[o+1]<<8)|(d[o+2]<<16)|(d[o+3]<<24)); };
    auto rf32 = [&](int o) { uint32_t u=ru32(o); float v; memcpy(&v,&u,4); return v; };
    ib.value1=ru32(0); ib.value2=rf32(4); return ib;
}

// ─── Packet Parsing ────────────────────────────────────────
DnetHeader DemoParser::readDnetHeader(BitStream& bs) {
    DnetHeader dh{};
    dh.gameFlag = bs.readFlag();
    dh.connectSeqBit = bs.readInt(1);
    dh.seqNumber = bs.readInt(9);
    dh.highestAck = bs.readInt(9);
    dh.packetType = bs.readInt(2);
    dh.ackByteCount = bs.readInt(3);
    dh.ackMask = 0;
    for (int i = 0; i < dh.ackByteCount; ++i)
        dh.ackMask |= (uint64_t)(uint32_t)bs.readInt(8) << (i * 8);
    return dh;
}

GameState DemoParser::readGameState(BitStream& bs) {
    GameState gs{};
    gs.compressionPoint = compressionPoint;
    gs.lastMoveAck = bs.readU32();

    // damageFlash and whiteOut (optional 7-bit floats)
    if (bs.readFlag()) {
        if (bs.readFlag()) {
            gs.damageFlash = bs.readFloat(7);
            gs.hasDamageFlash = true;
        }
        if (bs.readFlag()) {
            gs.whiteOut = bs.readFloat(7) * 1.5f;
            gs.hasWhiteOut = true;
        }
    }

    // selfLocked / selfHomed (gated by flag)
    if (bs.readFlag()) {
        gs.selfLocked = bs.readFlag();
        gs.selfHomed = bs.readFlag();
    }

    // seeker tracking (gated by flag)
    if (bs.readFlag()) {
        gs.seekerTracking = bs.readFlag();
        if (gs.seekerTracking) {
            gs.seekerTrackingPos.x = bs.readF32();
            gs.seekerTrackingPos.y = bs.readF32();
            gs.seekerTrackingPos.z = bs.readF32();
        }
        gs.seekerMode = bs.readRangedU32(0, 2);
        if (gs.seekerMode == 1) {
            // LockObject: ghost-index of locked target
            if (bs.readFlag())
                gs.seekerObjectGhostIndex = bs.readRangedU32(0, T2Demo::MaxGhostCount - 1);
        } else if (gs.seekerMode == 2) {
            // LockPosition: target position
            gs.targetPos.x = bs.readF32();
            gs.targetPos.y = bs.readF32();
            gs.targetPos.z = bs.readF32();
        }
    }

    // pinged / jammed
    gs.pinged = bs.readFlag();
    gs.jammed = bs.readFlag();

    // Control object section (gated)
    if (bs.readFlag()) {
        gs.controlObjectDirty = bs.readFlag();
        if (gs.controlObjectDirty) {
            // Full update: ghost index + readPacketData
            gs.controlObjectGhostIndex = bs.readInt(T2Demo::GhostIdBitSize);
            // ShapeBase::readPacketData reads: energy (f32) + rechargeRate (f32)
            gs.energy = bs.readF32();
            gs.rechargeRate = bs.readF32();
            const GhostEntry* control = ghostTracker.getGhost(gs.controlObjectGhostIndex);
            if (control && ghostClassIs(control->className, "Camera")) {
                gs.cameraPosition = {bs.readF32(), bs.readF32(), bs.readF32()};
                gs.cameraPitch = bs.readF32();
                gs.cameraYaw = bs.readF32();
                gs.hasCameraTransform = true;
                const int mode = bs.readRangedU32(0, 4);
                gs.cameraMode = mode;
                if (mode == 3 || mode == 4) {
                    gs.orbitMinDistance = bs.readF32();
                    gs.orbitMaxDistance = bs.readF32();
                    gs.orbitDistance = bs.readF32();
                    if (mode == 3) {
                        bs.readFlag();
                        gs.orbitObjectGhostIndex = bs.readInt(T2Demo::GhostIdBitSize);
                    } else {
                        // Relative to the previous compression point.
                        gs.orbitPoint = bs.readCompressedPoint(gs.compressionPoint);
                    }
                }
                gs.compressionPoint = gs.cameraPosition;
                gs.compressionPointUpdated = true;
            } else if (control && control->classId == 25) {
                // Player::readPacketData: ShapeBase state, movement, view,
                // optional piloted object, and final movement flags.
                bs.readInt(3); // action state
                if (bs.readFlag()) bs.readInt(7); // recover ticks
                if (bs.readFlag()) bs.readInt(7); // jump delay
                if (bs.readFlag()) {
                    gs.compressionPoint = {bs.readF32(), bs.readF32(), bs.readF32()};
                    gs.compressionPointUpdated = true;
                    bs.readF32(); bs.readF32(); bs.readF32(); // velocity
                    bs.readInt(4); // jump surface contact
                }
                gs.controlHeadX = bs.readF32();
                gs.controlHeadZ = bs.readF32();
                gs.controlRotZ = bs.readF32();
                gs.hasControlRotation = true;
                if (bs.readFlag()) {
                    const int pilotedIndex = bs.readInt(T2Demo::GhostIdBitSize);
                    const GhostEntry* piloted = ghostTracker.getGhost(pilotedIndex);
                    if (piloted && (piloted->classId == 4)) {
                        bs.readF32(); bs.readF32();
                        const Vec3 pos{bs.readF32(), bs.readF32(), bs.readF32()};
                        bs.readF32(); bs.readF32();
                        const int mode = bs.readInt(3);
                        if (mode == 3 || mode == 4) {
                            bs.readF32(); bs.readF32(); bs.readF32();
                            if (mode == 3) {
                                bs.readFlag(); bs.readInt(T2Demo::GhostIdBitSize);
                            } else {
                                bs.readCompressedPoint(gs.compressionPoint);
                            }
                        }
                        gs.compressionPoint = pos;
                        gs.compressionPointUpdated = true;
                    } else if (piloted && (piloted->classId == 10 ||
                                             piloted->classId == 14 ||
                                             piloted->classId == 52)) {
                        // Player::readPacketData delegates to the piloted
                        // vehicle's readPacketData. Missing this nested
                        // payload shifts the remainder of the packet.
                        bs.readF32(); bs.readF32(); // vehicle energy/recharge
                        bs.readF32(); bs.readF32(); // steering
                        const Vec3 vehiclePosition{bs.readF32(), bs.readF32(), bs.readF32()};
                        gs.compressionPoint = vehiclePosition;
                        gs.compressionPointUpdated = true;
                        bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32(); // orientation
                        bs.readPoint3F(); // linear momentum
                        bs.readPoint3F(); // angular momentum
                        bs.readFlag(); // disable move
                        bs.readFlag(); // frozen
                        if (piloted->classId == 52) {
                            bs.readFlag(); // braking
                            for (int i = 0; i < 18; ++i) bs.readF32();
                        }
                    }
                }
                bs.readFlag(); // disable move
                bs.readFlag(); // pilot
            }
        } else {
            // Compression point only
            gs.compressionPoint.x = bs.readF32();
            gs.compressionPoint.y = bs.readF32();
            gs.compressionPoint.z = bs.readF32();
            gs.compressionPointUpdated = true;
        }
    }

    // Target visibility: while(readFlag()) { readInt(4) index; readInt(32) mask; }
    while (bs.readFlag()) {
        int idx = bs.readInt(4);
        uint32_t mask = (uint32_t)bs.readInt(32);
        gs.targetVisibility.push_back({idx, (int)mask});
    }

    // Camera FOV (optional)
    if (bs.readFlag()) {
        gs.cameraFov = (float)bs.readInt(8);
    }

    return gs;
}

void DemoParser::readEvents(BitStream& bs, std::vector<NetEventInfo>& outEvents, const Vec3& compressionPoint) {
    bool guaranteedPhase = false;
    bool more = bs.readFlag();
    int previousGuaranteedSequence = -2;
    while (!bs.isError()) {
        if (!more) {
            if (guaranteedPhase) break;
            guaranteedPhase = true;
            more = bs.readFlag();
            if (!more || bs.isError()) break;
        }
        NetEventInfo ev{};
        ev.guaranteed = guaranteedPhase;
        if (guaranteedPhase) {
            if (bs.readFlag()) {
                ev.sequenceNumber = (previousGuaranteedSequence + 1) & 0x7f;
            } else {
                ev.sequenceNumber = bs.readInt(7);
            }
            previousGuaranteedSequence = ev.sequenceNumber;
        }
        int rawId = bs.readInt(T2Demo::NetEventClassBitSize);
        ev.classId = rawId + T2Demo::NetEventClassFirst;
        if (rawId >= 0 && rawId < T2Demo::NetEventClassCount) {
            if (const char* name = V12::eventClassName((size_t)rawId)) ev.eventName = name;
        } else {
            ev.eventName = "Event" + std::to_string(rawId);
        }
        ev.dataBitsStart = bs.getCurPos();
        if (!readEventPayload(bs, ev, compressionPoint, true)) {
            // Event payloads are not length-delimited. Do not consume a guessed
            // base payload or reinterpret its bits as the next event/ghost.
            Console::instance().printf(LogLevel::Error,
                "Demo: unsupported event payload class=%d name='%s' at bit=%d "
                "(previous class=%d bits=%d-%d); packet parsing stopped",
                ev.classId, ev.eventName.empty() ? "<unknown>" : ev.eventName.c_str(),
                ev.dataBitsStart,
                outEvents.empty() ? -1 : (int)outEvents.back().classId,
                outEvents.empty() ? -1 : outEvents.back().dataBitsStart,
                outEvents.empty() ? -1 : outEvents.back().dataBitsEnd);
            ev.dataBitsEnd = bs.getCurPos();
            outEvents.push_back(ev);
            bs.fail();
            break;
        }
        ev.dataBitsEnd = bs.getCurPos();
        outEvents.push_back(ev);
        more = bs.readFlag();
    }
}

// Decodes one event payload. applyEffects is false for demo start-block
// events, which are queued behind every real sequence and never dispatch.
bool DemoParser::readEventPayload(BitStream& bs, NetEventInfo& ev,
                                  const Vec3& compressionPoint, bool applyEffects) {
    if (ev.classId == T2Demo::NetEventClassFirst + 22) { // SimpleMessageEvent
        ev.message = bs.readString();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 9) { // RemoteCommandEvent
        int argc = bs.readInt(5);
        for (int i = 0; i < argc; i++) {
            std::string arg = bs.unpackNetString();
            // unpackNetString marks tagged strings as "\\x01<id>".
            if (arg.size() > 4 && arg.compare(0, 4, "\\x01") == 0) {
                int tag = atoi(arg.c_str() + 4);
                auto it = initialBlock.taggedStrings.find(tag);
                if (it != initialBlock.taggedStrings.end()) arg = it->second;
            }
            ev.arguments.push_back(arg);
            if (!ev.message.empty()) ev.message += ' ';
            ev.message += arg;
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 7) { // NetStringEvent
        const int id = bs.readInt(10);
        if (bs.readFlag()) {
            const std::string value = bs.readString();
            if (applyEffects && !bs.isError() && id >= 0 && id < 1024)
                initialBlock.taggedStrings[id] = value;
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 4) { // GhostingMessageEvent
        bs.readU32();
        const int message = bs.readInt(3);
        bs.readInt(11);
        // NetConnection::handleGhostMessage EndGhosting deletes the client's
        // ghosts (datablocks are connection state and stay). Events are read
        // before the packet's ghost section, as the engine applies them.
        constexpr int GhostMsgEndGhosting = 2;
        if (applyEffects && message == GhostMsgEndGhosting) {
            ghostTracker.clear();
            ++ghostResets_;
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 0) { // CRCChallengeEvent
        bs.readU32(); bs.readU32(); bs.readU32(); bs.readFlag();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 1) { // CRCChallengeResponseEvent
        bs.readU32(); bs.readU32(); bs.readU32();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 19) { // SimDataBlockEvent
        int objId  = bs.readInt(T2Demo::SimDBEventObjectIdBits); (void)objId;
        int clsId  = bs.readInt(T2Demo::SimDBEventClassIdBits); (void)clsId;
        int idx    = bs.readInt(T2Demo::SimDBEventIndexBits); (void)idx;
        int total_ = bs.readInt(T2Demo::SimDBEventTotalBits); (void)total_;
    } else if (ev.classId == T2Demo::NetEventClassFirst + 17 ||
               ev.classId == T2Demo::NetEventClassFirst + 18) {
        ev.audioProfileId = bs.readInt(11);
        ev.directAudioProfile = true;
        if (ev.classId == T2Demo::NetEventClassFirst + 18 && bs.readFlag()) {
            bs.readFloat(8); bs.readFloat(8); bs.readFloat(8);
            bs.readFlag(); // quaternion W sign
        }
        if (ev.classId == T2Demo::NetEventClassFirst + 18) {
            const Vec3 position = bs.readCompressedPoint(compressionPoint, 0.5f);
            ev.audioPosition = {position.x, position.y, position.z};
            ev.hasAudioPosition = true;
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 20) { // SimTargetAudioEvent
        ev.targetId = bs.readInt(9);
        bs.readInt(12); // file tag
        bs.readRangedU32(3, 1026); // audio description ID
        if (bs.readFlag()) {
            const Vec3 position = bs.readCompressedPoint(compressionPoint, 0.5f);
            ev.audioPosition = {position.x, position.y, position.z};
            ev.hasAudioPosition = true;
        }
        bs.readFlag(); // update sound
    } else if (ev.classId == T2Demo::NetEventClassFirst + 2) { // FogChallengeEvent
        // No payload.
    } else if (ev.classId == T2Demo::NetEventClassFirst + 5) { // GravityEvent
        bs.readF32();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 6) { // LightningStrikeEvent
        if (bs.readFlag()) {
            bs.readInt(11);   // source ghost
            bs.readFloat(10); // strike x
            bs.readFloat(10); // strike y
            if (bs.readFlag()) bs.readInt(11); // target ghost
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 12) { // SensorGroupColorEvent
        const int sensorGroup = bs.readInt(5);
        const uint32_t updateMask = bs.readU32();
        for (int i = 0; i < 32; ++i) {
            if ((updateMask & (1u << i)) != 0) {
                if (bs.readFlag()) {
                    const uint32_t color = bs.readU32();
                    if (applyEffects)
                        sensorGroupColors_[{sensorGroup, uint32_t(1) << i}] = color;
                }
            }
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 14) { // SetObjectActiveImageEvent
        bs.readRangedU32(0, 1023);
        bs.readRangedU32(0, 8);
    } else if (ev.classId == T2Demo::NetEventClassFirst + 15) { // SetSensorGroupEvent
        bs.readInt(5);
    } else if (ev.classId == T2Demo::NetEventClassFirst + 16) { // SetServerTargetEvent
        if (bs.readFlag()) bs.readInt(9);
        bs.readF32(); bs.readF32(); bs.readF32();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 24) { // TargetInfoEvent
        ev.hasTargetInfo = true;
        ev.targetId = bs.readInt(9);
        auto readTag = [&](std::string& value) {
            if (bs.readFlag()) {
                const int tag = bs.readFlag() ? bs.readInt(10) : 0x400;
                if (tag != 0x400) {
                    auto it = initialBlock.taggedStrings.find(tag);
                    if (it != initialBlock.taggedStrings.end()) value = it->second;
                }
            }
        };
        readTag(ev.targetName);
        readTag(ev.targetSkin);
        readTag(ev.targetSkinPreference);
        readTag(ev.targetVoice);
        readTag(ev.targetType);
        if (bs.readFlag()) ev.targetSensorGroup = bs.readInt(5);
        if (bs.readFlag()) ev.targetDataBlockId = bs.readFlag() ? bs.readInt(11) : -2;
        if (bs.readFlag()) {
            ev.targetRenderFlags = bs.readInt(9);
            ev.hasTargetRenderFlags = true;
        }
        if (bs.readFlag()) ev.targetVoicePitch = bs.readFloat(7) * 1.5f + 0.5f;
        if (!applyEffects) return true;
        if (!ev.targetName.empty()) {
            auto player = std::find_if(playerInfo_.begin(), playerInfo_.end(),
                [&](const DemoPlayerInfo& entry) {
                    return entry.name == ev.targetName;
                });
            if (player != playerInfo_.end()) {
                if (!ev.targetSkin.empty()) player->skin = ev.targetSkin;
            } else {
                DemoPlayerInfo added;
                added.name = ev.targetName;
                added.skin = ev.targetSkin;
                added.teamId = ev.targetSensorGroup;
                added.clientId = ev.targetId;
                playerInfo_.push_back(std::move(added));
            }
        }
        DemoTargetState& target = targets_[ev.targetId];
        if (!ev.targetName.empty()) target.name = ev.targetName;
        if (!ev.targetSkin.empty()) target.skin = ev.targetSkin;
        if (!ev.targetType.empty()) target.type = ev.targetType;
        if (ev.targetSensorGroup >= 0) target.sensorGroup = ev.targetSensorGroup;
        if (ev.hasTargetRenderFlags) {
            target.renderFlags = ev.targetRenderFlags;
            target.hasRenderFlags = true;
        }
        // Apply to every ghost that owns this target slot.
        for (int index : ghostTracker.getAllIndices()) {
            GhostEntry* ghost = ghostTracker.getMutableGhost(index);
            if (ghost && ghost->targetId == ev.targetId) applyTarget(*ghost);
        }
    } else if (ev.classId == T2Demo::NetEventClassFirst + 25) { // TargetToEvent
        if (bs.readFlag()) bs.readInt(9);
        if (bs.readFlag()) {
            bs.readF32(); bs.readF32(); bs.readF32();
        }
        bs.readFlag();
    } else if (ev.classId == T2Demo::NetEventClassFirst + 23) { // TargetFreeEvent
        ev.hasTargetFree = true;
        ev.targetId = bs.readInt(9);
        if (!applyEffects) return true;
        // Target ids are not ghost indices; ghosts are removed only by the
        // ghost section's delete records.
        targets_.erase(ev.targetId);
        playerInfo_.erase(std::remove_if(playerInfo_.begin(), playerInfo_.end(),
            [&](const DemoPlayerInfo& player) { return player.clientId == ev.targetId; }),
            playerInfo_.end());
    } else if (ev.classId == T2Demo::NetEventClassFirst + 13) { // SetMissionCRCEvent
        ev.hasMissionCrc = true;
        ev.missionCrc = bs.readU32();
        if (!applyEffects) return true;
        currentMissionCrc_ = ev.missionCrc;
        if (parsingBlockIndex_ >= 0 &&
            (missionCrcChanges_.empty() ||
             missionCrcChanges_.back().first != parsingBlockIndex_ ||
             missionCrcChanges_.back().second != ev.missionCrc))
            missionCrcChanges_.push_back({parsingBlockIndex_, ev.missionCrc});
    } else {
        return false;
    }
    return true;
}

// ─── Ghost update data readers ──────────────────────────────────
// Ported from T2 decompiled TypeScript parsers.
// Each reads the class-specific unpackUpdate data from the bitstream
// and advances the stream past all data for that ghost.

static void readGameBaseData(BitStream& bs, bool, GhostEntry* entry = nullptr) {
    if (bs.readFlag()) {
        int datablockId = bs.readInt(11);
        if (entry) {
            entry->datablockId = datablockId;
            entry->hasDatablock = true;
        }
    }
    if (bs.readFlag()) { // TargetMask
        const int targetId = bs.readFlag() ? bs.readInt(9) : -1;
        if (entry) entry->targetId = targetId;
    }
}

static void readShapeBaseData(BitStream& bs, bool isInitial, GhostEntry* entry = nullptr) {
    readGameBaseData(bs, isInitial, entry);
    if (!bs.readFlag()) return;
    // DamageMask
    if (bs.readFlag()) {
        float dmg = bs.readFloat(6);
        int damageState = bs.readInt(2);
        if (entry) {
            entry->health = (1.0f - dmg) * 100.0f;
            entry->damageState = damageState;
        }
        bs.readFlag(); bs.readNormalVector(8);
    }
    // SoundMask (4 slots: flag -> playing flag -> optional profileId)
    if (bs.readFlag()) {
        for (int i = 0; i < 4; i++)
            if (bs.readFlag()) {
                bool playing = bs.readFlag();
                int profile = playing ? bs.readInt(11) : -1;
                if (entry) entry->soundThreads[i] = {profile, playing, true};
            }
    }
    // ThreadMask: sequence/state plus compact direction/end flags.
    if (bs.readFlag()) {
        for (int i = 0; i < 4; i++)
            if (bs.readFlag()) {
                int sequence = bs.readInt(5);
                int state = bs.readInt(2);
                bool forward = bs.readFlag();
                bool atEnd = bs.readFlag();
                if (entry) {
                    const bool changed = !entry->threads[i].valid ||
                        entry->threads[i].sequence != sequence ||
                        entry->threads[i].state != state ||
                        entry->threads[i].forward != forward ||
                        entry->threads[i].atEnd != atEnd;
                    entry->threads[i].sequence = sequence;
                    entry->threads[i].state = state;
                    entry->threads[i].timescale = forward ? 1.0f : -1.0f;
                    entry->threads[i].position = atEnd ? (forward ? 1.0f : 0.0f) :
                        (changed ? 0.0f : entry->threads[i].position);
                    entry->threads[i].forward = forward;
                    entry->threads[i].atEnd = atEnd;
                    entry->threads[i].valid = true;
                    if (changed) entry->threadAnimTime = 0.0f;
                }
            }
    }
    // ImageMask (8 mounted image slots)
    if (bs.readFlag()) {
        for (int i = 0; i < 8; i++) {
            if (bs.readFlag()) {
                const bool wasFiring = entry && entry->mountedImages[i].isFiring;
                if (entry) {
                    // The state machine persists while the image datablock
                    // stays the same.
                    GhostEntry::MountedImage previous = std::move(entry->mountedImages[i]);
                    entry->mountedImages[i] = {};
                    entry->mountedImages[i].animation = std::move(previous.animation);
                    entry->mountedImages[i].animationDatablock = previous.animationDatablock;
                }
                if (bs.readFlag()) {
                    int dbId = bs.readInt(11);
                    if (entry && i < 8) entry->mountedImages[i].datablockId = dbId;
                }
                if (bs.readFlag()) {
                    if (bs.readFlag()) bs.readInt(10);
                    else {
                        std::string value = bs.readString();
                        if (entry && entry->skinName.empty() && !value.empty())
                            entry->skinName = value;
                    }
                }
                const bool triggerDown = bs.readFlag();
                const bool loaded = bs.readFlag();
                const bool ammo = bs.readFlag();
                const bool wet = bs.readFlag();
                const bool target = bs.readFlag();
                const int fireCount = bs.readInt(3);
                const bool firing = fireCount != 0;
                const bool extraFlag = isInitial ? bs.readFlag() : false;
                if (entry && i < 8) {
                    auto& image = entry->mountedImages[i];
                    image.triggerDown = triggerDown;
                    image.loaded = loaded;
                    image.ammo = ammo;
                    image.wet = wet;
                    image.target = target;
                    image.fireCount = fireCount;
                    image.forceFire = extraFlag;
                    image.isFiring = firing;
                    if (wasFiring != firing) entry->threadAnimTime = 0.0f;
                }
            }
        }
    }
    // CloakMask + state-B + invincibility state.
    if (bs.readFlag()) {
        if (bs.readFlag()) {
            bool cloaked = bs.readFlag();
            bs.readFlag(); // controlled
            if (entry) {
                entry->cloaked = cloaked;
                entry->hasCloak = true;
            }
            if (bs.readFlag()) {
                const bool fadeOut = bs.readFlag();
                const float fadeTime = bs.readF32();
                if (entry) {
                    entry->fading = true;
                    entry->fadeOut = fadeOut;
                    entry->fadeTime = fadeTime;
                    entry->fadeFresh = true;
                }
            } else {
                const bool visible = bs.readFlag();
                if (entry) {
                    entry->fading = false;
                    entry->fadeVal = visible ? 1.0f : 0.0f;
                }
            }
        }
        if (bs.readFlag()) {
            if (bs.readFlag()) {
                bs.readFlag(); // state-B mode
            } else {
                bs.readNormalVector(8);
                const float shield = bs.readFloat(5);
                if (entry) {
                    entry->shieldLevel = shield;
                    entry->hasShield = true;
                }
            }
        }
        if (bs.readFlag()) {
            bs.readU32();
            bs.readU32();
        }
    }
    if (bs.readFlag()) { // MountedMask
        if (bs.readFlag()) {
            const int mount = bs.readInt(10);
            const int node = bs.readInt(5);
            if (entry) {
                entry->mountObject = mount;
                // ShapeBase::mountObject clamps an invalid slot to zero.
                entry->mountNode = node >= 0 && node < 32 ? node : 0;
            }
        } else if (entry) {
            entry->mountObject = -1;
        }
    }
}

static void readPlayerData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) bs.readInt(3); // ImpactMask
    if (bs.readFlag()) { // ActionMask - action animation
        const int action = bs.readInt(8);
        const bool holdAtEnd = bs.readFlag();
        const bool atEnd = bs.readFlag();
        bs.readFlag(); // first person
        float position = 0.0f;
        if (!atEnd && bs.readFlag()) position = bs.readSignedFloat(6);
        if (entry) {
            entry->actionAnim = action;
            entry->actionHoldAtEnd = holdAtEnd;
            entry->actionAtEnd = atEnd;
            entry->actionAnimPos = std::clamp(position, 0.0f, 1.0f);
            entry->actionTime = DemoParser::s_packetTime;
        }
    }
    if (bs.readFlag()) { // ArmAction
        const int armAction = bs.readInt(8);
        if (entry) entry->armAction = armAction;
    }
    if (bs.readFlag()) return; // control object shortcut
    if (bs.readFlag()) { // MoveMask
        int actionState = bs.readInt(3); // actionState: 0=Stop, 1=Walk, 2=Run, 3=Sprint
        if (entry) entry->isMoving = (actionState > 0);
        if (bs.readFlag()) bs.readInt(7); // recoverState
        const bool falling = bs.readFlag();
        const bool jetting = bs.readFlag();
        if (entry) entry->position = bs.readCompressedPoint(cp);
        else bs.readCompressedPoint(cp);
        Vec3 velocity{};
        if (bs.readFlag()) {
            const float speed = bs.readInt(13) / 32.0f;
            const Vec3 dir = bs.readNormalVector(10);
            velocity = {dir.x * speed, dir.y * speed, dir.z * speed};
        }
        if (entry) {
            entry->falling = falling;
            entry->jetting = jetting;
            entry->torqueVelocity = velocity;
        }
        float headX = bs.readSignedFloat(6); // head pitch
        float headZ = bs.readSignedFloat(6); // head yaw
    float bodyYaw = bs.readFloat(7) * (2.0f * (float)M_PI); // rotationZ (0-1 maps to 0-2PI)
        if (entry) {
            // Always update body yaw rotation from MoveMask
            entry->rotation = torqueYawQuaternion(bodyYaw);
            entry->hasRotation = true;
            entry->headPitch = headX;
            entry->headYaw = headZ;
            entry->bodyYaw = bodyYaw;
        }
        readMove(bs);
        bs.readFlag(); // allowWarp
    }
    float en = bs.readFloat(5); // energy
    if (entry) entry->energy = en * 100.0f;
}

// ─── Vehicle ghost parsers ─────────────────────────────────────
static void readVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    const bool jetting = bs.readFlag();
    if (entry) entry->vehicleJetting = jetting;
    if (bs.readFlag()) { // control shortcut
        return;
    }
    const float packedSteering = bs.readFloat(9);
    bs.readFloat(9); // steering Y component
    if (entry) {
        entry->steeringYaw = V12::decodeVehicleSteering(packedSteering);
        entry->hasSteering = true;
    }
    readMove(bs);
    const bool frozen = bs.readFlag();
    if (entry) entry->frozen = frozen;
    if (bs.readFlag()) { // PositionMask
        if (entry) {
            entry->position = bs.readCompressedPoint(cp);
        } else bs.readCompressedPoint(cp);
        float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32(), qw = bs.readF32();
        if (entry) { entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true; }
        if (entry) {
            const Vec3 momentum = bs.readPoint3F(); // linMomentum
            // Vehicle datablock mass is not part of this ghost update. Use
            // the same native fallback as the reference parser until the
            // datablock mass can be resolved at render time.
            entry->velocity = {momentum.x / 200.0f, momentum.y / 200.0f,
                               momentum.z / 200.0f};
            entry->linearMomentum = momentum;
            entry->hasVelocity = true;
            entry->hasLinearMomentum = true;
        } else {
            bs.readPoint3F(); // linMomentum
        }
        bs.readPoint3F(); // angMomentum
    }
    if (bs.readFlag()) bs.readFloat(8); // EnergyMask
}

static void readFlyingVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    if (bs.readFlag()) return; // FlyingVehicle control shortcut
    bs.readFlag(); // createHeightOn
    const int thrust = bs.readInt(3);
    if (entry) entry->thrustDirection = thrust;
}

static void readHoverVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    const int thrust = bs.readInt(3);
    if (entry) entry->thrustDirection = thrust;
}

static void readWheeledVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    bs.readFlag(); // braking
    if (bs.readFlag()) {
        for (int i = 0; i < 6; ++i) {
            const float angularVelocity = bs.readF32();
            const float suspension = bs.readF32();
            const float lateral = bs.readF32();
            if (entry)
                entry->wheels[i] = {angularVelocity, suspension, lateral, true};
        }
    }
}

static void readStaticShapeData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) {
        if (entry) {
            entry->position = bs.readPoint3F();
        } else {
            bs.readPoint3F();
        }
        float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32();
        bool qwNeg = bs.readFlag();
        float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
        if (qwNeg) qw = -qw;
        if (entry) { entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true; }
        bs.readPoint3F(); // scale
    }
    bs.readFlag(); // powered
}

static void readBeaconObjectData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readStaticShapeData(bs, isInitial, cp, entry);
    if (bs.readFlag()) bs.readInt(2); // beacon type
}

static void readItemData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) { // InitialUpdateMask
        bs.readFlag(); // rotate
        const bool isStatic = bs.readFlag();
        if (entry) entry->itemStatic = isStatic;
        bs.readFlag(); // collideable
        if (bs.readFlag()) bs.readPoint3F(); // scale
    }
    if (bs.readFlag()) bs.readInt(10); // ThrowSrcMask
    if (bs.readFlag()) { bs.readFlag(); bs.readF32(); } // RotationMask (zSign, angle)
    if (bs.readFlag()) { // PositionMask
        const Vec3 position = bs.readPoint3F();
        const bool atRest = bs.readFlag();
        const Vec3 velocity = atRest ? Vec3{} : bs.readPoint3F();
        const bool warp = bs.readFlag();
        if (entry) {
            entry->position = position;
            entry->itemAtRest = atRest;
            entry->itemVelocity = velocity;
            entry->itemWarp = warp;
            ++entry->itemPositionUpdates;
        }
    }
}

static void readCameraData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) return; // control object shortcut
    if (bs.readFlag()) { // camera update mask
        const Vec3 position{bs.readF32(), bs.readF32(), bs.readF32()};
        const float pitch = bs.readF32();
        const float yaw = bs.readF32();
        if (entry) {
            entry->position = position;
            entry->cameraEuler = {pitch, 0.0f, yaw};
            entry->hasCameraEuler = true;
        }
    }
}

static void readMarkerData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    float qx = 0, qy = 0, qz = 0;
    Vec3 pos;
    if (entry) {
        pos = entry->position = bs.readCompressedPoint(cp);
        qx = bs.readF32(); qy = bs.readF32(); qz = bs.readF32();
        bool qwNeg = bs.readFlag();
        float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
        if (qwNeg) qw = -qw;
        entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true;
    } else {
        bs.readCompressedPoint(cp);
        bs.readF32(); bs.readF32(); bs.readF32(); bs.readFlag();
    }
}

static void readMissionMarkerData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) {
        if (entry) {
            entry->position = bs.readPoint3F();
            const float qx = bs.readF32();
            const float qy = bs.readF32();
            const float qz = bs.readF32();
            float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
            if (bs.readFlag()) qw = -qw;
            entry->rotation = {qx, qy, qz, qw};
            entry->hasRotation = true;
        } else {
            bs.readPoint3F();
            bs.readF32(); bs.readF32(); bs.readF32(); bs.readFlag();
        }
        bs.readPoint3F(); // scale
    }
}

static void readSpawnSphereData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readMissionMarkerData(bs, isInitial, cp, entry);
    if (bs.readFlag()) {
        bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32();
    }
}

static void readWayPointData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readMissionMarkerData(bs, isInitial, cp, entry);
    if (bs.readFlag()) bs.readString();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readFlag();
}

static void readProjectileData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (!bs.readFlag()) return; // non-full state
    if (entry) entry->position = bs.readCompressedPoint(cp);
    else bs.readCompressedPoint(cp);
    Vec3 velocity = bs.readCompressedPoint(cp); // velocity
    if (entry) {
        entry->velocity = velocity;
        entry->hasVelocity = true;
        const float length = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
        if (length > 0.001f) {
            entry->rotation = torqueYawQuaternion(atan2f(velocity.x, velocity.y));
            entry->hasRotation = true;
        }
    }
    if (bs.readFlag()) bs.readInt(10); // source
    if (bs.readFlag()) bs.readInt(10); // vehicleObject
}

static void readELFProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag() && bs.readFlag()) {
        const int source = bs.readInt(11);
        const int slot = bs.readInt(3); // source image slot
        const int target = bs.readInt(11);
        if (entry) {
            entry->linkSourceGhost = source;
            entry->linkSourceSlot = slot;
            entry->linkTargetGhost = target;
        }
    }
}

static void readRepairProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag() && bs.readFlag()) {
        const int source = bs.readInt(11); // source object
        const int slot = bs.readInt(3); // source slot
        const int target = bs.readInt(11); // repairing object
        if (entry) {
            entry->linkSourceGhost = source;
            entry->linkSourceSlot = slot;
            entry->linkTargetGhost = target;
        }
    }
}

static void readDebrisData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) {
        if (entry) entry->position = bs.readCompressedPoint(cp);
        else bs.readCompressedPoint(cp);
    }
    Vec3 velocity{bs.readF32(), bs.readF32(), bs.readF32()}; // velocity
    if (entry) {
        entry->velocity = velocity;
        entry->hasVelocity = true;
    }
    for (int i = 0; i < 4; i++) bs.readBool();
    for (int i = 0; i < 6; i++) bs.readF32();
    for (int i = 0; i < 2; i++) bs.readBool();
    for (int i = 0; i < 3; i++) bs.readF32();
    bs.readBool();
    bs.readString(); bs.readString();
    for (int i = 0; i < 3; i++) { if (bs.readFlag()) bs.readInt(11); }
}

static void readGrenadeData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) { // initial update
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        Vec3 vel = bs.readPoint3F(); // velocity
        if (entry) { entry->velocity = vel; entry->hasVelocity = true; }
        const uint32_t currTick = bs.readRangedU32(0, 4095);
        if (entry) {
            // The client flies the shell from this state; grenades stay
            // world-aligned (GrenadeProjectile::processTick).
            entry->ballisticSentPos = entry->position;
            entry->ballisticSentVel = vel;
            entry->ballisticCurrTick = (int)currTick;
            entry->hasBallistic = entry->ballisticFresh = true;
            entry->ballisticTime = 0.0f; // a new flight (the ghost index may be reused)
            entry->ballisticStopped = false;
            entry->rotation = {0, 0, 0, 1};
            entry->hasRotation = true;
        }
        bs.readFlag(); // quickSplash
        if (bs.readFlag()) {
            Vec3 expPos = bs.readPoint3F();
            Vec3 normal = bs.readPoint3F();
            DemoParser::s_pendingExplosions.push_back({expPos, normal, 0.0f,
                entry ? entry->datablockId : -1});
            if (entry) entry->exploded = true;
        }
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); } // source
        if (bs.readFlag()) bs.readRangedU32(0, 1024); // vehicleObject
    } else { // non-initial
        if (bs.readFlag()) { // BounceMask: the server's corrected state
            if (entry) entry->position = bs.readPoint3F();
            else bs.readPoint3F();
            const Vec3 vel = bs.readPoint3F();
            if (entry) {
                entry->velocity = vel;
                entry->ballisticSentPos = entry->position;
                entry->ballisticSentVel = vel;
                entry->ballisticFresh = true;
            }
        }
        if (bs.readFlag()) {
            Vec3 expPos = bs.readPoint3F();
            Vec3 normal = bs.readPoint3F();
            DemoParser::s_pendingExplosions.push_back({expPos, normal, 0.0f,
                entry ? entry->datablockId : -1});
            if (entry) entry->exploded = true;
        }
    }
}

static void readSniperProjectileData(BitStream& bs, bool isInitial, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) { // initial
        bs.readFloat(7); // energyPercentage
        Vec3 startPos = bs.readPoint3F();
        Vec3 endPos = bs.readPoint3F(); // endPos
        if (entry) {
            entry->position = startPos;
            entry->beamStart = startPos;
            entry->beamEnd = endPos;
            entry->hasBeam = true;
        }
        bs.readFlag(); bs.readFlag(); // truncated, hitWater
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); bs.readFlag(); }
    } else { // swing update
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); bs.readFlag(); }
        else {
            Vec3 pos = bs.readPoint3F();
            if (entry) entry->position = pos;
        }
        bs.readPoint3F(); // endPos
        bs.readFlag(); // truncated
    }
}

static void readShockLanceProjectileData(BitStream& bs, bool isInitial, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) { // targetObject
        const int target = (int)bs.readRangedU32(0, 1024);
        if (entry) entry->linkTargetGhost = target;
    }
    if (bs.readFlag()) { // initial update
        Vec3 start = bs.readPoint3F();
        Vec3 end = bs.readPoint3F(); // end
        const bool hitObject = bs.readFlag();
        if (entry) {
            entry->position = start;
            entry->beamStart = start;
            entry->beamEnd = end;
            entry->hasBeam = true;
            entry->beamHit = hitObject;
            entry->shockFresh = true;
        }
        if (bs.readFlag()) {
            const int source = (int)bs.readRangedU32(0, 1024);
            const int slot = (int)bs.readRangedU32(0, 7);
            if (entry) { entry->linkSourceGhost = source; entry->linkSourceSlot = slot; }
        }
    }
}

static void readBombProjectileData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    // BombProjectile::unpackUpdate (FUN_00636050, a GrenadeProjectile): the
    // client flies the bomb from each transmitted state; bombs stay
    // world-aligned.
    if (bs.readFlag()) bs.readInt(11);
    if (bs.readFlag()) { if (bs.readFlag()) bs.readInt(9); }
    auto setFlight = [&](const Vec3& pos, const Vec3& vel, bool restart) {
        if (!entry) return;
        entry->position = pos;
        entry->velocity = vel;
        entry->hasVelocity = true;
        entry->ballisticSentPos = pos;
        entry->ballisticSentVel = vel;
        entry->hasBallistic = entry->ballisticFresh = true;
        entry->rotation = {0, 0, 0, 1};
        entry->hasRotation = true;
        if (restart) { entry->ballisticTime = 0.0f; entry->ballisticStopped = false; }
    };
    auto explode = [&](const Vec3& point, const Vec3& normal) {
        DemoParser::s_pendingExplosions.push_back({point, normal, 0.0f, entry ? entry->datablockId : -1});
        if (entry) entry->exploded = true;
    };
    if (!bs.readFlag()) { // non-full
        if (bs.readFlag()) {
            const Vec3 pos = bs.readPoint3F();
            const Vec3 vel = bs.readPoint3F();
            setFlight(pos, vel, false);
        }
        if (!bs.readFlag()) return;
        const Vec3 point = bs.readPoint3F();
        const Vec3 normal = bs.readPoint3F();
        explode(point, normal);
        return;
    }
    // full state
    const Vec3 pos = bs.readPoint3F();
    const Vec3 vel = bs.readPoint3F();
    const int currTick = bs.readInt(12);
    setFlight(pos, vel, true);
    if (entry) entry->ballisticCurrTick = currTick;
    bs.readFlag(); // reset
    if (bs.readFlag()) {
        const Vec3 point = bs.readPoint3F();
        const Vec3 normal = bs.readPoint3F();
        explode(point, normal);
    }
    if (bs.readFlag()) { bs.readInt(11); bs.readInt(3); } // source object, slot
    if (bs.readFlag()) bs.readInt(11); // vehicle object
}

static void readLinearProjectileData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) { // InitialUpdateMask
        if (bs.readFlag()) { // hidden/already exploded
            Vec3 expPos = bs.readCompressedPoint(cp);
            Vec3 normal = bs.readNormalVector(14);
            bool hitWater = bs.readFlag();
            DemoParser::s_pendingExplosions.push_back({expPos, normal, 0.0f,
                entry ? entry->datablockId : -1});
            if (entry) entry->exploded = true;
        } else { // live projectile
            if (entry) entry->position = bs.readCompressedPoint(cp);
            else bs.readCompressedPoint(cp);
             Vec3 dir = bs.readNormalVector(14); // direction
            if (entry && (dir.x != 0 || dir.y != 0 || dir.z != 0)) {
                float yaw = atan2f(dir.x, dir.y);
                entry->rotation = torqueYawQuaternion(yaw);
                entry->hasRotation = true;
            }
            const uint32_t currTick = bs.readRangedU32(0, 511);
            float excessVel = 0.0f;
            Vec3 excessDir{0, 0, 0};
            if (bs.readFlag()) {
                bs.readInt(10); bs.readRangedU32(0, 7); // source
                if (bs.readFlag()) {
                    excessVel = (float)bs.readRangedU32(0, 255);
                    excessDir = bs.readNormalVector(7);
                }
            }
            // LinearProjectile ghosts carry the initial state; the client
            // flies the segment (createSegments) from here.
            if (entry) {
                entry->linearStart = entry->position;
                entry->linearDir = dir;
                entry->linearExcess = {excessDir.x * excessVel, excessDir.y * excessVel,
                                       excessDir.z * excessVel};
                entry->linearCurrTick = (int)currTick;
                entry->hasLinearFlight = true;
                entry->linearSegmentValid = false;
            }
            if (bs.readFlag()) bs.readInt(10); // vehicleObject
        }
    } else { // non-initial: explosion
        Vec3 expPos = bs.readCompressedPoint(cp);
        Vec3 normal = bs.readNormalVector(14);
        bs.readFlag();
        DemoParser::s_pendingExplosions.push_back({expPos, normal, 0.0f,
            entry ? entry->datablockId : -1});
        if (entry) entry->exploded = true;
    }
}

static void readSeekerProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    // SeekerProjectile::unpackUpdate. The client coasts the missile on the
    // transmitted velocity between updates.
    auto setFlight = [&](const Vec3& pos, const Vec3& vel) {
        if (!entry) return;
        entry->position = pos;
        entry->velocity = vel;
        entry->hasVelocity = true;
        entry->ballisticSentPos = pos;
        entry->ballisticSentVel = vel;
        entry->hasBallistic = entry->ballisticFresh = entry->ballisticCoast = true;
    };
    auto readTarget = [&]() {
        if (bs.readFlag()) {
            if (!bs.readFlag()) bs.readPoint3F(); // target direction
            else bs.readInt(11);                  // target ghost
        }
    };
    const bool fullState = bs.readFlag();
    if (!fullState) {
        if (bs.readFlag()) { // explode(position, normal)
            const Vec3 expPos = bs.readPoint3F();
            const Vec3 normal = bs.readPoint3F();
            DemoParser::s_pendingExplosions.push_back({expPos, normal, 0.0f,
                entry ? entry->datablockId : -1});
            if (entry) entry->exploded = true;
            return;
        }
        const Vec3 pos = bs.readPoint3F();
        const Vec3 vel = bs.readPoint3F();
        setFlight(pos, vel);
        readTarget();
        return;
    }
    const Vec3 pos = bs.readPoint3F();
    const Vec3 vel = bs.readPoint3F();
    bs.readPoint3F(); // orientation
    setFlight(pos, vel);
    if (entry) { entry->ballisticTime = 0.0f; entry->ballisticStopped = false; }
    if (bs.readFlag()) { bs.readInt(11); bs.readInt(3); } // source object, slot
    readTarget();
    bs.readFlag(); // timeout reset
}

// ─── Scene ghosts ────────────────────────────────────────────
// SceneObject ghosts (terrain, interiors, statics, sky, sun, water, mission
// area) carry what the client builds its world from. Their initial update is
// kept in mission-file form so the world loader reads it like a .mis object.
static std::string sceneFloats(std::initializer_list<float> values) {
    std::string out;
    char buf[32];
    for (float value : values) {
        snprintf(buf, sizeof(buf), "%.9g", value);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

static void setSceneProp(GhostEntry* entry, const char* name, std::string value) {
    if (!entry) return;
    for (auto& prop : entry->sceneProps)
        if (prop.first == name) { prop.second = std::move(value); return; }
    entry->sceneProps.emplace_back(name, std::move(value));
}

// A row-major MatrixF as the mission's position and rotation (AngAxisF of
// QuatF::set(MatrixF), degrees) plus scale.
static void setSceneTransform(GhostEntry* entry, const float* m, const Vec3& scale) {
    if (!entry) return;
    auto at = [&](int r, int c) { return m[r * 4 + c]; };
    float q[4]; // x y z w
    const float trace = at(0, 0) + at(1, 1) + at(2, 2);
    if (trace > 0.0f) {
        float t = std::sqrt(trace + 1.0f);
        q[3] = t * 0.5f;
        t = 0.5f / t;
        q[0] = (at(1, 2) - at(2, 1)) * t;
        q[1] = (at(2, 0) - at(0, 2)) * t;
        q[2] = (at(0, 1) - at(1, 0)) * t;
    } else {
        int i = 0;
        if (at(1, 1) > at(0, 0)) i = 1;
        if (at(2, 2) > at(i, i)) i = 2;
        const int j = (i + 1) % 3, k = (j + 1) % 3;
        float t = std::sqrt((at(i, i) - (at(j, j) + at(k, k))) + 1.0f);
        q[i] = t * 0.5f;
        t = 0.5f / t;
        q[j] = (at(i, j) + at(j, i)) * t;
        q[k] = (at(i, k) + at(k, i)) * t;
        q[3] = (at(j, k) - at(k, j)) * t;
    }
    const float w = std::clamp(q[3], -1.0f, 1.0f);
    const float angle = std::acos(w) * 2.0f;
    const float sinHalf = std::sqrt(std::max(0.0f, 1.0f - w * w));
    Vec3 axis{1.0f, 0.0f, 0.0f};
    if (sinHalf != 0.0f) axis = {q[0] / sinHalf, q[1] / sinHalf, q[2] / sinHalf};
    setSceneProp(entry, "position", sceneFloats({at(0, 3), at(1, 3), at(2, 3)}));
    setSceneProp(entry, "rotation", sceneFloats({axis.x, axis.y, axis.z, angle * 180.0f / 3.14159265358979f}));
    setSceneProp(entry, "scale", sceneFloats({scale.x, scale.y, scale.z}));
}

static void readSkyData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    // Sky::unpackUpdate (retail layout, as the reference parser decodes it).
    if (bs.readFlag()) {
        setSceneProp(entry, "materialList", bs.readString());
        const float fr = bs.readF32(), fg = bs.readF32(), fb = bs.readF32();
        setSceneProp(entry, "fogColor", sceneFloats({fr, fg, fb, 1.0f}));
        const uint32_t fogCount = bs.readU32();
        if (fogCount > 64) { bs.skipBits(bs.getRemainingBits()); return; }
        setSceneProp(entry, "useSkyTextures", bs.readBool() ? "1" : "0");
        setSceneProp(entry, "renderBottomTexture", bs.readBool() ? "1" : "0");
        const float sr = bs.readF32(), sg = bs.readF32(), sb = bs.readF32();
        setSceneProp(entry, "SkySolidColor", sceneFloats({sr, sg, sb, 1.0f}));
        setSceneProp(entry, "windEffectPrecipitation", bs.readBool() ? "1" : "0");
        for (uint32_t i = 0; i < fogCount; ++i) {
            float v[6];
            for (float& value : v) value = bs.readF32();
            if (i < 3) {
                const std::string n = std::to_string(i + 1);
                setSceneProp(entry, ("fogVolume" + n).c_str(), sceneFloats({v[0], v[1], v[2]}));
                setSceneProp(entry, ("fogVolumeColor" + n).c_str(), sceneFloats({v[3], v[4], v[5], 1.0f}));
            }
        }
        for (int i = 0; i < 3; ++i) {
            const std::string texture = bs.readString();
            const float height = bs.readF32(), speed = bs.readF32();
            setSceneProp(entry, ("cloudText" + std::to_string(i + 1)).c_str(), texture);
            setSceneProp(entry, ("cloudHeightPer[" + std::to_string(i) + "]").c_str(), sceneFloats({height}));
            setSceneProp(entry, ("cloudSpeed" + std::to_string(i + 1)).c_str(), sceneFloats({speed}));
        }
        const Vec3 wind = bs.readPoint3F();
        setSceneProp(entry, "windVelocity", sceneFloats({wind.x, wind.y, wind.z}));
        bs.readF32(); // current storm
        if (bs.readFlag()) for (int j = 0; j < 5; ++j) bs.readF32();
    }
    if (bs.readFlag()) bs.readBool();
    if (bs.readFlag()) bs.readBool();
    if (bs.readFlag()) {
        const float visible = bs.readF32(), fog = bs.readF32();
        setSceneProp(entry, "visibleDistance", sceneFloats({visible}));
        setSceneProp(entry, "fogDistance", sceneFloats({fog}));
    }
    if (bs.readFlag()) { bs.readF32(); bs.readF32(); }
    if (bs.readFlag()) for (int j = 0; j < 3; ++j) bs.readF32();
    if (bs.readFlag()) for (int j = 0; j < 4; ++j) bs.readF32();
    if (bs.readFlag()) {
        const Vec3 wind = bs.readPoint3F();
        setSceneProp(entry, "windVelocity", sceneFloats({wind.x, wind.y, wind.z}));
    }
}

static void readSunData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    if (bs.readFlag()) {
        for (int i = 0; i < 5; ++i) {
            const std::string texture = bs.readString();
            setSceneProp(entry, ("texture[" + std::to_string(i) + "]").c_str(), texture);
        }
    }
    if (bs.readFlag()) {
        float v[19];
        for (float& value : v) value = bs.readF32();
        setSceneProp(entry, "direction", sceneFloats({v[0], v[1], v[2]}));
        setSceneProp(entry, "color", sceneFloats({v[3], v[4], v[5], v[6]}));
        setSceneProp(entry, "ambient", sceneFloats({v[7], v[8], v[9], v[10]}));
    }
}

static void readLightningData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial, entry);
    if (bs.readFlag()) {
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        bs.readPoint3F(); // scale
        bs.readF32(); // strike width
        bs.readF32(); // chance to hit target
        bs.readF32(); // strike radius
        bs.readF32(); // bolt start radius
        bs.readF32(); bs.readF32(); bs.readF32(); // color
        bs.readF32(); bs.readF32(); bs.readF32(); // fade color
        bs.readInt(8); // use fog (serialized as a byte)
        bs.readF32(); // strikes per minute
    }
}

static void readMissionAreaData(BitStream& bs, GhostEntry* entry) {
    if (bs.readFlag()) {
        const int32_t x = bs.readS32(), y = bs.readS32(), w = bs.readS32(), h = bs.readS32();
        setSceneProp(entry, "area", std::to_string(x) + " " + std::to_string(y) + " " +
                                    std::to_string(w) + " " + std::to_string(h));
        setSceneProp(entry, "flightCeiling", sceneFloats({bs.readF32()}));
        setSceneProp(entry, "flightCeilingRange", sceneFloats({bs.readF32()}));
    }
}

static void readPhysicalZoneData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    if (!bs.readFlag()) {
        bs.readFlag(); // active
        return;
    }
    bs.readMatrixF();
    bs.readPoint3F(); // scale
    const uint32_t pointCount = bs.readU32();
    if (pointCount > 4096) { bs.readFlag(); return; }
    for (uint32_t i = 0; i < pointCount; ++i) bs.readPoint3F();
    const uint32_t planeCount = bs.readU32();
    if (planeCount > 4096) { bs.readFlag(); return; }
    for (uint32_t i = 0; i < planeCount; ++i)
        for (int j = 0; j < 4; ++j) bs.readF32();
    const uint32_t edgeCount = bs.readU32();
    if (edgeCount > 4096) { bs.readFlag(); return; }
    for (uint32_t i = 0; i < edgeCount; ++i)
        for (int j = 0; j < 4; ++j) bs.readU32();
    bs.readF32(); // velocity modifier
    bs.readF32(); // gravity modifier
    bs.readPoint3F(); // applied force
    bs.readFlag(); // active
}

static void readForceFieldBareData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    const int payloadStart = bs.getCurPos();
    if (bs.readFlag()) {
        auto at = bs.readAffineTransform();
        if (entry) { entry->position = at.position; entry->rotation = at.rotation; entry->hasRotation = true; }
    }
    bs.readPoint3F();
    // The build-25034 recordings carry a fixed 316-bit ForceFieldBare
    // envelope despite variable-width affine position encodings. Consume the
    // remaining class-owned bits after decoding the common fields.
    const int consumed = bs.getCurPos() - payloadStart;
    for (int remaining = 316 - consumed; remaining > 0; remaining -= std::min(remaining, 32))
        bs.readInt(std::min(remaining, 32));
}

static void readTSStaticData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    // TSStatic::unpackUpdate: transform, scale and shape name, always.
    float m[16];
    const float* read = (const float*)bs.readMatrixF();
    std::copy(read, read + 16, m);
    const Vec3 scale = bs.readPoint3F();
    const std::string shapeName = bs.readString();
    setSceneTransform(entry, m, scale);
    setSceneProp(entry, "shapeName", shapeName);
}

static void readAudioEmitterData(BitStream& bs, GhostEntry* entry) {
    // AudioEmitter::unpackUpdate; the mission-file field names.
    bs.readFlag(); // initial update
    if (bs.readFlag()) {
        const Vec3 position = bs.readPoint3F();
        bs.readF32(); bs.readF32(); bs.readF32(); bs.readFlag(); // rotation
        setSceneProp(entry, "position", sceneFloats({position.x, position.y, position.z}));
        if (entry) entry->position = position;
    }
    if (bs.readFlag()) setSceneProp(entry, "audioProfileId", bs.readFlag() ? std::to_string(bs.readInt(11)) : "0");
    if (bs.readFlag()) setSceneProp(entry, "audioDescriptionId", bs.readFlag() ? std::to_string(bs.readInt(11)) : "0");
    if (bs.readFlag()) setSceneProp(entry, "fileName", bs.readString());
    if (bs.readFlag()) setSceneProp(entry, "useProfileDescription", bs.readFlag() ? "1" : "0");
    if (bs.readFlag()) setSceneProp(entry, "volume", sceneFloats({bs.readF32()}));
    if (bs.readFlag()) setSceneProp(entry, "isLooping", bs.readFlag() ? "1" : "0");
    if (bs.readFlag()) setSceneProp(entry, "is3D", bs.readFlag() ? "1" : "0");
    if (bs.readFlag()) setSceneProp(entry, "minDistance", sceneFloats({bs.readF32()}));
    if (bs.readFlag()) setSceneProp(entry, "maxDistance", sceneFloats({bs.readF32()}));
    if (bs.readFlag()) bs.readS32(); // cone inside angle
    if (bs.readFlag()) bs.readS32(); // cone outside angle
    if (bs.readFlag()) bs.readF32(); // cone outside volume
    if (bs.readFlag()) bs.readPoint3F(); // cone vector
    if (bs.readFlag()) setSceneProp(entry, "loopCount", std::to_string(bs.readS32()));
    if (bs.readFlag()) setSceneProp(entry, "minLoopGap", std::to_string(bs.readS32()));
    if (bs.readFlag()) setSceneProp(entry, "maxLoopGap", std::to_string(bs.readS32()));
    if (bs.readFlag()) bs.readS32(); // audio type
    if (bs.readFlag()) bs.readFlag(); // outside ambient
}

static void readTerrainBlockData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    auto readRuns = [&]() {
        const uint32_t count = bs.readU32();
        if (count > (uint32_t)(bs.getRemainingBits() / 32)) { bs.skipBits(bs.getRemainingBits()); return std::string(); }
        std::string runs;
        for (uint32_t i = 0; i < count; ++i) {
            if (!runs.empty()) runs += ' ';
            runs += std::to_string(bs.readU32());
        }
        return runs;
    };
    if (bs.readFlag()) { // InitMask
        bs.readU32(); // CRC
        setSceneProp(entry, "terrainFile", bs.readString());
        setSceneProp(entry, "detailTexture", bs.readString());
        const uint32_t squareSize = bs.readU32();
        setSceneProp(entry, "squareSize", std::to_string(squareSize));
        setSceneProp(entry, "emptySquares", readRuns());
        // TerrainBlock::onAdd centres the block: -squareSize * BlockSize / 2.
        const float corner = -(float)squareSize * 128.0f;
        setSceneProp(entry, "position", sceneFloats({corner, corner, 0.0f}));
    } else if (bs.readFlag()) { // EmptyMask
        setSceneProp(entry, "emptySquares", readRuns());
    }
}

static void readWaterBlockData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    // WaterBlock::unpackUpdate: everything, always.
    const Vec3 position = bs.readPoint3F();
    const float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32();
    float qw = sqrtf(fmaxf(0, 1.0f - (qx * qx + qy * qy + qz * qz)));
    if (bs.readFlag()) qw = -qw;
    const Vec3 scale = bs.readPoint3F();
    const std::string surface = bs.readString(), envMap = bs.readString();
    const std::string submerge0 = bs.readString(), submerge1 = bs.readString();
    const int32_t liquidType = bs.readS32();
    const float density = bs.readF32(), viscosity = bs.readF32();
    const float waveMagnitude = bs.readF32(), surfaceOpacity = bs.readF32();
    const float envMapIntensity = bs.readF32();
    const bool removeWetEdges = bs.readU8() != 0;
    if (bs.readFlag()) bs.readInt(11); // audio environment
    if (entry) {
        entry->position = position;
        entry->rotation = {qx, qy, qz, qw};
        entry->hasRotation = true;
        // The quaternion as a MatrixF (m_quatF_set_matF, row-major).
        const float xx = qx * qx, yy = qy * qy, zz = qz * qz, xy = qx * qy, xz = qx * qz,
                    yz = qy * qz, wx = qw * qx, wy = qw * qy, wz = qw * qz;
        const float m[16] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), position.x,
                             2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), position.y,
                             2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), position.z,
                             0, 0, 0, 1};
        setSceneTransform(entry, m, scale);
        setSceneProp(entry, "surfaceTexture", surface);
        setSceneProp(entry, "envMapTexture", envMap);
        setSceneProp(entry, "submergeTexture[0]", submerge0);
        setSceneProp(entry, "submergeTexture[1]", submerge1);
        setSceneProp(entry, "liquidType", std::to_string(liquidType));
        setSceneProp(entry, "density", sceneFloats({density}));
        setSceneProp(entry, "viscosity", sceneFloats({viscosity}));
        setSceneProp(entry, "waveMagnitude", sceneFloats({waveMagnitude}));
        setSceneProp(entry, "surfaceOpacity", sceneFloats({surfaceOpacity}));
        setSceneProp(entry, "envMapIntensity", sceneFloats({envMapIntensity}));
        setSceneProp(entry, "removeWetEdges", removeWetEdges ? "1" : "0");
    }
}

static void readVehicleBlockerData(BitStream& bs) {
    bs.readMatrixF();
    bs.readPoint3F();
    bs.readPoint3F();
}

static void readInteriorData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    auto readTransform = [&]() {
        float m[16];
        Vec3 position;
        const float* read = (const float*)bs.readMatrixF(&position);
        std::copy(read, read + 16, m);
        const Vec3 scale = bs.readPoint3F();
        if (entry) entry->position = position;
        setSceneTransform(entry, m, scale);
    };
    if (bs.readFlag()) { // InitMask - full initial state
        bs.readU32(); // CRC
        setSceneProp(entry, "interiorFile", bs.readString());
        setSceneProp(entry, "showTerrainInside", bs.readFlag() ? "1" : "0");
        readTransform();
        bs.readFlag(); // alarm state
        setSceneProp(entry, "skinBase", bs.readString());
        if (bs.readFlag()) bs.readInt(11);
        if (bs.readFlag()) bs.readInt(11);
    } else { // normal update
        if (bs.readFlag()) readTransform();
        bs.readFlag();
        if (bs.readFlag()) setSceneProp(entry, "skinBase", bs.readString());
        if (bs.readFlag()) {
            if (bs.readFlag()) bs.readInt(11);
            if (bs.readFlag()) bs.readInt(11);
        }
    }
}

// ─── Ghost data dispatch ──────────────────────────────────────
// Dispatch by class name string (from tagged strings).
// Also checks default class ID indices as fallback for unnamed classes.
// Returns true if the class was known and data was read, false if unknown.
static bool readGhostClassData(BitStream& bs, int classId, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    std::string cn = entry ? entry->className : "";
    bool known = true;
    if (ObserverParity::isPlayerClass(cn)) readPlayerData(bs, isInitial, cp, entry);
    else if (cn == "Vehicle") readVehicleData(bs, isInitial, cp, entry);
    else if (cn == "FlyingVehicle" || cn == "Shrike") readFlyingVehicleData(bs, isInitial, cp, entry);
    else if (cn == "HoverVehicle" || cn == "Turbograv") readHoverVehicleData(bs, isInitial, cp, entry);
    else if (cn == "WheeledVehicle") readWheeledVehicleData(bs, isInitial, cp, entry);
    else if (cn == "StaticShape" || cn == "ScopeAlwaysShape"
        || cn == "Generator" || cn == "Sensor"
        || cn == "Vehicle Pad" || cn == "Teleport Station"
        || cn == "Deployed" || cn == "transfer pad" || cn == "VehicleDrop")
        readStaticShapeData(bs, isInitial, cp, entry);
    else if (cn == "Turret" || cn == "Sentry") {
        readStaticShapeData(bs, isInitial, cp, entry);
        if (bs.readFlag()) bs.readFloat(8); // CapacitorEnergy
        if (bs.readFlag()) return true; // control shortcut
        if (bs.readFlag()) {
            // phi (around the primary axis), then theta, then activation.
            const float phi = bs.readFloat(10);
            const float theta = bs.readFloat(10);
            const float activation = bs.readFloat(8);
            if (entry) {
                entry->turretPhi = phi;
                entry->turretTheta = theta;
                entry->turretActivation = activation;
                entry->hasTurretAim = true;
                entry->barrelYaw = phi * 6.28318530717958647692f;
                entry->barrelPitch = theta * 3.14159265358979323846f;
            }
        }
    }
    else if (cn == "Item" || cn == "mine") readItemData(bs, isInitial, cp, entry);
    else if (cn == "BeaconObject") readBeaconObjectData(bs, isInitial, cp, entry);
    else if (cn == "Camera") readCameraData(bs, isInitial, cp, entry);
    else if (cn == "Marker") readMarkerData(bs, false, cp, entry);
    else if (cn == "MissionMarker") readMissionMarkerData(bs, isInitial, cp, entry);
    else if (cn == "MissionArea") readMissionAreaData(bs, entry);
    else if (cn == "Splash") {
        readGameBaseData(bs, isInitial, entry);
        if (bs.readFlag()) {
            if (entry) entry->position = bs.readPoint3F();
            else bs.readPoint3F();
        }
    }
    else if (cn == "Shockwave") {
        readGameBaseData(bs, isInitial, entry);
        if (bs.readFlag()) {
            if (entry) entry->position = bs.readPoint3F();
            else bs.readPoint3F();
            bs.readPoint3F();
        }
    }
    else if (cn == "ParticleEmissionDummy") {
        readGameBaseData(bs, isInitial, entry);
        float m[16];
        Vec3 position;
        const float* read = (const float*)bs.readMatrixF(&position);
        std::copy(read, read + 16, m);
        const Vec3 scale = bs.readPoint3F();
        if (entry) entry->position = position;
        setSceneTransform(entry, m, scale);
        // The emitter's up axis (the transform's z column).
        setSceneProp(entry, "emitterAxis", sceneFloats({m[2], m[6], m[10]}));
        if (bs.readFlag()) setSceneProp(entry, "emitterId", std::to_string(bs.readInt(11)));
    }
    else if (cn == "Trigger") {
        bs.readU32();
    }
    else if (cn == "PhysicalZone") readPhysicalZoneData(bs, isInitial, cp, entry);
    else if (cn == "WayPoint") readWayPointData(bs, isInitial, cp, entry);
    else if (cn == "SpawnSphere") readSpawnSphereData(bs, isInitial, cp, entry);
    else if (cn == "Debris") readDebrisData(bs, isInitial, cp, entry);
    else if (cn == "Projectile")
        readProjectileData(bs, isInitial, cp, entry);
    else if (cn == "ELFProjectile") readELFProjectileData(bs, isInitial, cp, entry);
    else if (cn == "EnergyProjectile" || cn == "FlareProjectile")
        readGrenadeData(bs, isInitial, cp, entry);
    else if (cn == "RepairProjectile") readRepairProjectileData(bs, isInitial, cp, entry);
    else if (cn == "BombProjectile") readBombProjectileData(bs, isInitial, cp, entry);
    else if (cn == "GrenadeProjectile") readGrenadeData(bs, isInitial, cp, entry);
    else if (cn == "LinearProjectile" || cn == "TracerProjectile" || cn == "LinearFlareProjectile")
        readLinearProjectileData(bs, isInitial, cp, entry);
    else if (cn == "SeekerProjectile") readSeekerProjectileData(bs, isInitial, cp, entry);
    else if (cn == "SniperProjectile") readSniperProjectileData(bs, isInitial, entry);
    else if (cn == "ShockLanceProjectile") readShockLanceProjectileData(bs, isInitial, entry);
    else if (cn == "Sky") readSkyData(bs, isInitial, cp, entry);
    else if (cn == "Sun") readSunData(bs, isInitial, cp, entry);
    else if (cn == "Lightning") readLightningData(bs, isInitial, cp, entry);
    else if (cn == "Precipitation") {
        readGameBaseData(bs, isInitial, entry);
        if (bs.readFlag()) {
            bs.readF32();
            const int colorCount = bs.readS32();
            if (colorCount < 0 || colorCount > 3) {
                Console::instance().printf(LogLevel::Error,
                    "Demo: invalid precipitation color count %d", colorCount);
                return false;
            }
            for (int i = 0; i < colorCount; ++i) {
                bs.readU8(); bs.readU8(); bs.readU8(); bs.readU8();
            }
            bs.readF32(); bs.readF32(); bs.readF32();
            bs.readS32(); bs.readF32();
            if (bs.readFlag()) { bs.readF32(); bs.readF32(); bs.readF32(); }
        }
        if (bs.readFlag()) bs.readBool();
        if (bs.readFlag()) { bs.readF32(); bs.readF32(); }
        if (bs.readFlag()) bs.readF32();
    }
    else if (cn == "StationFXPersonal" || cn == "StationFXVehicle") {
        readGameBaseData(bs, isInitial, entry);
        // InitialUpdateMask: optional station object reference.
        if (bs.readFlag() && bs.readFlag()) bs.readRangedU32(0, 1024);
    }
    else if (cn == "TargetProjectile") {
        readGameBaseData(bs, isInitial, entry);
        auto readSource = [&] {
            bs.readRangedU32(0, 1024); // source object
            bs.readRangedU32(0, 7);    // source image slot
            bs.readFlag();             // client owned
        };
        if (bs.readFlag()) { // InitialUpdateMask
            bs.readPoint3F(); // start
            bs.readPoint3F(); // end
            bs.readFlag();    // truncated
            if (bs.readFlag()) readSource();
        } else {
            if (bs.readFlag()) readSource();
            else bs.readPoint3F();
            bs.readPoint3F();
            bs.readFlag();
        }
    }
    else if (cn == "TSStatic") readTSStaticData(bs, isInitial, cp, entry);
    else if (cn == "AudioEmitter") readAudioEmitterData(bs, entry);
    else if (cn == "VehicleBlocker") readVehicleBlockerData(bs);
    else if (cn == "AIObjective") {
        readShapeBaseData(bs, isInitial, entry);
        if (bs.readFlag()) {
            if (entry) {
                entry->position = bs.readPoint3F();
                const float qx = bs.readF32();
                const float qy = bs.readF32();
                const float qz = bs.readF32();
                float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
                if (bs.readFlag()) qw = -qw;
                entry->rotation = {qx, qy, qz, qw};
                entry->hasRotation = true;
            } else {
                bs.readPoint3F();
                bs.readF32(); bs.readF32(); bs.readF32(); bs.readFlag();
            }
            bs.readPoint3F();
        }
        bs.readFlag();
    }
    else if (cn == "TerrainBlock") readTerrainBlockData(bs, isInitial, cp, entry);
    else if (cn == "WaterBlock") readWaterBlockData(bs, isInitial, cp, entry);
    else if (cn == "ForceFieldBare") readForceFieldBareData(bs, isInitial, cp, entry);
    else if (cn == "InteriorInstance") readInteriorData(bs, isInitial, cp, entry);
    else if (cn == "ShapeBase") readShapeBaseData(bs, isInitial, entry);
    else if (cn == "GameBase") readGameBaseData(bs, isInitial, entry);
    else known = false;

    if (!known) {
        Console::instance().printf(LogLevel::Warn,
            "Demo: unsupported ghost payload class=%d name='%s' initial=%d bit=%d; "
            "payload left unconsumed",
            classId, cn.empty() ? "<unknown>" : cn.c_str(), isInitial ? 1 : 0,
            bs.getCurPos());
        return false;
    }
    // A decoded ghost payload establishes a position even when that position
    // is the valid world origin. Do not make renderers infer validity from
    // the coordinate value.
    if (entry) entry->hasPosition = true;
    return true;
}

void DemoParser::readGhosts(BitStream& bs, std::vector<GhostUpdate>& outGhosts, int seqNumber, const Vec3* compressionPoint) {
    const Vec3 cp = compressionPoint ? *compressionPoint : Vec3{};
    if (!bs.readFlag()) return;
    int idSize = bs.readInt(3) + 3;
    if (bs.isError() || idSize > T2Demo::GhostIdBitSize) {
        Console::instance().printf(LogLevel::Error,
            "Demo: malformed ghost section seq=%d at bit=%d: invalid id width=%d",
            seqNumber, bs.getCurPos(), idSize);
        return;
    }
    int maxGhosts = 1024;
    while (bs.readFlag() && !bs.isError() && (int)outGhosts.size() < maxGhosts) {
        GhostUpdate gu{};
        gu.index = bs.readInt(idSize);
        if (bs.isError()) break;
        if (bs.readFlag()) {
            gu.type = GhostUpdate::Delete;
            ghostTracker.deleteGhost(gu.index);
            continue;
        }
        bool isNew = !ghostTracker.hasGhost(gu.index);
        if (isNew) {
            gu.type = GhostUpdate::Create;
            gu.classId = bs.readInt(T2Demo::NetObjectClassBitSize) + T2Demo::NetObjectClassFirst;
            if (!V12::ghostClassName((size_t)gu.classId))
                Console::instance().printf(LogLevel::Error,
                    "Demo: first unknown packet ghost seq=%d index=%d class=%d bit=%d",
                    seqNumber, gu.index, gu.classId, bs.getCurPos());
            std::string cn;
            if (const char* name = V12::ghostClassName((size_t)gu.classId)) cn = name;
            else
                cn = "Class" + std::to_string(gu.classId);
            ghostTracker.createGhost(gu.index, gu.classId, cn);
        } else {
            gu.type = GhostUpdate::Update;
            auto* existing = ghostTracker.getGhost(gu.index);
            if (existing) gu.classId = existing->classId;
        }
        gu.updateBitsStart = bs.savePos();
        GhostEntry* entry = ghostTracker.getMutableGhost(gu.index);
        bool known = readGhostClassData(bs, gu.classId, isNew, cp, entry);
        if (!known) {
            if (isNew) ghostTracker.deleteGhost(gu.index);
            Console::instance().printf(LogLevel::Error,
                "Demo: stopped ghost section block=%d seq=%d index=%d class=%d at bit=%d "
                "(previous index=%d class=%d bits=%d-%d); unknown payload boundary",
                parsingBlockIndex_, seqNumber, gu.index, gu.classId, gu.updateBitsStart,
                outGhosts.empty() ? -1 : outGhosts.back().index,
                outGhosts.empty() ? -1 : outGhosts.back().classId,
                outGhosts.empty() ? -1 : outGhosts.back().updateBitsStart,
                outGhosts.empty() ? -1 : outGhosts.back().updateBitsEnd);
            // Ghost payloads are not length-delimited; the stream is desynced.
            bs.fail();
            break;
        }
        gu.updateBitsEnd = bs.getCurPos();
        if (entry && entry->targetId >= 0) applyTarget(*entry);
        outGhosts.push_back(gu);
    }
    if (bs.isError()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: malformed ghost section seq=%d at bit=%d/%d after %zu updates",
            seqNumber, bs.getCurPos(), bs.getMaxPos(), outGhosts.size());
    } else if ((int)outGhosts.size() >= maxGhosts) {
        Console::instance().printf(LogLevel::Error,
            "Demo: ghost section seq=%d exceeded %d updates at bit=%d",
            seqNumber, maxGhosts, bs.getCurPos());
    }
}

bool DemoParser::applyProtocolHeader(const DnetHeader& dnet, bool& dispatchData) {
    if (dnet.connectSeqBit != (int)(connectSequence & 1))
        return false;
    if (dnet.ackByteCount > 4 || dnet.packetType > 2)
        return false;

    uint32_t seq = (uint32_t)dnet.seqNumber | (lastSeqRecvd & 0xfffffe00u);
    if (seq < lastSeqRecvd) seq += 0x200;
    if (lastSeqRecvd + 0x1f < seq)
        return false;

    uint32_t highestAck = (uint32_t)dnet.highestAck |
        (highestAckedSeq & 0xfffffe00u);
    if (highestAck < highestAckedSeq) highestAck += 0x200;
    if (lastSendSeq < highestAck)
        return false;

    uint32_t shift = (seq - lastSeqRecvd) & 0x1f;
    recvAckMask <<= shift;
    if (dnet.packetType == 0) recvAckMask |= 1;
    for (uint32_t ackSeq = highestAckedSeq + 1; ackSeq <= highestAck; ++ackSeq) {
        if (dnet.ackMask & (1u << ((highestAck - ackSeq) & 0x1f))) {
            lastRecvAckAck = lastSeqRecvdAtSend[ackSeq & 0x1f];
            connectionEstablished = true;
        }
    }
    if (seq - lastRecvAckAck > 0x20) lastRecvAckAck = seq - 0x20;
    highestAckedSeq = highestAck;
    dispatchData = lastSeqRecvd != seq && dnet.packetType == 0;
    lastSeqRecvd = seq;
    packetsParsed++;
    return true;
}

void DemoParser::recordParseFault(const char* stage, int blockIndex) {
    if (!parseFault_.empty()) return;
    parseFault_ = std::string(stage) + " section failed at block " + std::to_string(blockIndex);
    Console::instance().printf(LogLevel::Error,
        "Demo: parse fault (%s); later packets are dropped", parseFault_.c_str());
}

PacketData DemoParser::parsePacket(const uint8_t* data, size_t size, int blockIndex) {
    parsingBlockIndex_ = blockIndex;
    s_packetTime = blockIndex >= 0 ? T2Demo::playbackBlockTime(blockIndex, getMoveTicksBefore()) : 0.0f;
    PacketData pd{};
    BitStream bs(data, size);
    pd.dnetHeader = readDnetHeader(bs);
    if (bs.isError()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: malformed packet header at bit=%d/%d (block=%d)",
            bs.getCurPos(), bs.getMaxPos(), blockIndex);
        return pd;
    }
    if (!parseFault_.empty()) {
        // The engine has disconnected by now; a desynced stream is never
        // reinterpreted as later events or ghosts.
        ++packetsDroppedAfterFault_;
        return pd;
    }
    bool dispatchData = false;
    if (!applyProtocolHeader(pd.dnetHeader, dispatchData)) {
        Console::instance().printf(LogLevel::Error,
            "Demo: rejected packet header seq=%d ack=%d type=%d at bit=%d (block=%d)",
            pd.dnetHeader.seqNumber, pd.dnetHeader.highestAck,
            pd.dnetHeader.packetType, bs.getCurPos(), blockIndex);
        return pd;
    }
    if (!dispatchData) return pd;
    if (bs.readFlag()) { bs.readInt(10); bs.readInt(10); }
    if (bs.readFlag()) { bs.readInt(10); bs.readInt(10); }
    bs.setStringBufferEnabled(true);
    pd.gameState = readGameState(bs);
    compressionPoint = pd.gameState.compressionPoint;
    if (bs.isError()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: malformed packet seq=%d at game state bit=%d/%d (block=%d)",
            pd.dnetHeader.seqNumber, bs.getCurPos(), bs.getMaxPos(), blockIndex);
        bs.setStringBufferEnabled(false);
        recordParseFault("gameState", blockIndex);
        return pd;
    }
    readEvents(bs, pd.events, pd.gameState.compressionPoint);
    if (bs.isError()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: malformed or unsupported event section seq=%d at bit=%d/%d (block=%d)",
            pd.dnetHeader.seqNumber, bs.getCurPos(), bs.getMaxPos(), blockIndex);
        bs.setStringBufferEnabled(false);
        recordParseFault("event", blockIndex);
        return pd;
    }
    readGhosts(bs, pd.ghosts, pd.dnetHeader.seqNumber, &pd.gameState.compressionPoint);
    bs.setStringBufferEnabled(false);
    if (bs.isError()) recordParseFault("ghost", blockIndex);
    if (blockIndex >= 0) {
        const double blockTime = T2Demo::playbackBlockTime(blockIndex, getMoveTicksBefore());
        for (const auto& event : pd.events) {
            if (event.message.empty()) continue;
            DemoTimedEvent timeline;
            timeline.time = blockTime;
            timeline.text = event.message;
            timeline.type = event.classId == T2Demo::NetEventClassFirst + 22 ? 0 :
                event.classId == T2Demo::NetEventClassFirst + 9 ?
                    (!event.arguments.empty() && event.arguments[0] == "ChatMessage" ? 0 : 1) : 2;
            eventLog_.push_back(std::move(timeline));
        }
        if (eventLog_.size() > 200)
            eventLog_.erase(eventLog_.begin(), eventLog_.begin() +
                            (eventLog_.size() - 200));
    }
    return pd;
}

void DemoParser::onSendPacketTrigger() {
    ++lastSendSeq;
    lastSeqRecvdAtSend[lastSendSeq & 0x1f] = lastSeqRecvd;
}
