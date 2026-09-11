#include "game/demo.h"
#include "net/v12_datablocks.h"
#include "net/v12_registry.h"
#include "core/console.h"
#include "core/config.h"
#include "core/timer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <memory>
#include <set>
#include <zlib.h>

// Pending explosion events from projectile ghost parsers
std::vector<DemoParser::PendingExplosion> DemoParser::s_pendingExplosions;
DemoParser::SunData DemoParser::s_sunData;
std::string DemoParser::s_pendingTerrainFile;

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
    int rangeSize = rangeEnd - rangeStart + 1;
    int bits = 1;
    while (bits < 31 && (1u << bits) < (unsigned)rangeSize) bits++;
    int value = readInt(bits) + rangeStart;
    if (value > rangeEnd) { error = true; return rangeEnd; }
    return value;
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
    // v24834 uses raw strings: U8 length + raw bytes (no Huffman, no string buffer)
    // v25034+ uses Huffman with string buffer compression
    // Detect format by checking if the first flag+length produces a reasonable string
    if (stringBufferEnabled && !stringBuffer.empty() && readFlag()) {
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
            return stringBufferEnabled && tag >= 0 && tag < 1024
                ? ("\\x01" + std::to_string(tag)) : ("\\x01" + std::to_string(tag));
        }
        case 3: { // integer
            bool neg = readFlag();
            int num;
            if (readFlag()) num = readInt(7);     // small (0-127)
            else if (readFlag()) num = readInt(14); // medium (0-16383)
            else num = readInt(29);                 // large (0-536870911)
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
    if (outPos) { outPos->x = elements[12]; outPos->y = elements[13]; outPos->z = elements[14]; }
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
    if (index >= T2Demo::MaxGhostCount || !registeredName) {
        Console::instance().printf(LogLevel::Error,
            "Demo: rejecting invalid ghost index/class (%d/%d)", index, classId);
        return;
    }
    GhostEntry e;
    e.classId = classId;
    e.className = registeredName;
    ghosts[index] = e;
}
void GhostTracker::deleteGhost(int index) { ghosts.erase(index); }
void GhostTracker::clear() { ghosts.clear(); }
int GhostTracker::size() const { return (int)ghosts.size(); }
std::vector<int> GhostTracker::getAllIndices() const {
    std::vector<int> r; for (auto& [k,v] : ghosts) r.push_back(k); return r;
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
    if (!f) { Console::instance().printf(LogLevel::Error, "Demo: cannot open %s", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(sz);
    if (!b) { fclose(f); return false; }
    size_t readBytes = fread(b, 1, sz, f); fclose(f);
    if ((long)readBytes != sz) { free(b); Console::instance().printf(LogLevel::Error, "Demo: short read %s", path); return false; }
    bool ok = load(b, sz);
    if (ownsBuffer) free((void*)buf);
    buf = b; bufSize = sz; ownsBuffer = true;
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

// ─── Initial Block Sub-Readers (unused for this demo, kept for future use) ───
ScoreEntry DemoParser::readScoreEntry(BitStream& bs) {
    ScoreEntry se{};
    se.clientId = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    se.teamId = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    se.score = bs.readFlag() ? (uint32_t)bs.readInt(16) : 0;
    se.field0 = (uint32_t)bs.readInt(6);
    se.field1 = (uint32_t)bs.readInt(6);
    se.field2 = (uint32_t)bs.readInt(6);
    se.isBot = bs.readFlag();
    for (int i = 0; i < 6; i++) se.triggerFlags[i] = bs.readFlag();
    return se;
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

void DemoParser::readEventStartBlock(BitStream& bs) {
    initialBlock.nextRecvEventSeq = bs.readU32();
    while (bs.readFlag() && !bs.isError()) {
        NetEventInfo ev{};
        ev.classId = bs.readInt(T2Demo::NetEventClassBitSize) + T2Demo::NetEventClassFirst;
        ev.guaranteed = true;
        ev.dataBitsStart = bs.getCurPos();
        ev.dataBitsEnd = bs.getCurPos();
        initialBlock.initialEvents.push_back(ev);
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

static bool readInitialControlPacket(BitStream& bs, const GhostEntry& ghost) {
    if (ghost.classId != 4 && ghost.classId != 25) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial control packet parser missing for class %d (%s)",
            ghost.classId, ghost.className.c_str());
        return false;
    }

    bs.readF32(); // energy level
    bs.readF32(); // recharge rate
    if (ghost.classId == 4) {
        bs.readPoint3F();
        bs.readF32(); // rotation X
        bs.readF32(); // rotation Z
        const int mode = bs.readInt(3);
        if (mode == 3 || mode == 4) {
            bs.readF32();
            bs.readF32();
            bs.readF32();
            if (mode == 3) {
                bs.readFlag();
                bs.readInt(T2Demo::GhostIdBitSize);
            } else {
                bs.readCompressedPoint({});
            }
        } else if (mode == 5) {
            bs.readInt(T2Demo::GhostIdBitSize);
        }
    } else {
        // Player::readPacketData after ShapeBase::readPacketData.
        bs.readInt(3); // action state
        if (bs.readFlag()) bs.readInt(7); // recover ticks
        if (bs.readFlag()) bs.readInt(7); // jump delay
        if (bs.readFlag()) {
            bs.readPoint3F(); // compression point
            bs.readPoint3F(); // velocity
            bs.readInt(4); // jump surface last contact
        }
        bs.readF32(); // head X
        bs.readF32(); // head Z
        bs.readF32(); // rotation Z

        if (bs.readFlag()) {
            const int pilotedIndex = bs.readInt(T2Demo::GhostIdBitSize);
            // A Player can recursively delegate to its piloted Vehicle. The
            // initial stream must consume that virtual readPacketData too.
            if (pilotedIndex >= 0) {
                bs.readF32(); // vehicle energy level
                bs.readF32(); // vehicle recharge rate
                bs.readF32(); // steering X
                bs.readF32(); // steering Y
                bs.readPoint3F(); // vehicle position
                bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32(); // orientation
                bs.readPoint3F(); // linear momentum
                bs.readPoint3F(); // angular momentum
                bs.readFlag(); // disable move
                bs.readFlag(); // frozen
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

bool DemoParser::readInitialBlock(const uint8_t* data, size_t size) {
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

    const uint32_t scoreCount = bs.readU32();
    if (scoreCount > 256) return fail("score count");
    initialBlock.scoreEntries.clear();
    for (uint32_t i = 0; i < scoreCount; ++i)
        initialBlock.scoreEntries.push_back(readScoreEntry(bs));
    initialBlock.demoValues = readDemoValues(bs);
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
        if (!control || !readInitialControlPacket(bs, *control))
            return fail("control object");
    }
    initialBlock.missionName = bs.readString();
    initialBlock.missionCRC = bs.readU32();
    readSimpleTargetManager(bs);
    readSimpleTargetManager(bs);
    return bs.isError() ? fail("mission tail") : true;
}

// ─── Block stream ────────────────────────────────────────────
bool DemoParser::load(const uint8_t* buffer, size_t size) {
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
    if (!readInitialBlock(buf + offset, header.initialBlockSize)) {
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
    for (const auto& score : initialBlock.scoreEntries) {
        auto player = std::find_if(playerInfo_.begin(), playerInfo_.end(),
            [&](const DemoPlayerInfo& entry) { return entry.clientId == (int)score.clientId; });
        if (player != playerInfo_.end()) {
            player->teamId = (int)score.teamId;
            player->score = (int)score.score;
        }
    }
    initialTaggedStrings_ = initialBlock.taggedStrings;
    initialPlayerInfo_ = playerInfo_;

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
    memset(lastSeqRecvdAtSend, 0, sizeof(lastSeqRecvdAtSend));
    lastSeqRecvd = 0; highestAckedSeq = 0; lastSendSeq = 0;
    recvAckMask = 0; connectSequence = 0; lastRecvAckAck = 0;
    connectionEstablished = false; nextRecvEventSeq = 0; packetsParsed = 0;
    compressionPoint = {0,0,0};
    blockStreamOffset = 0; blockCursor_ = 0; blockCount_ = -1;

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
    compressionPoint = {0,0,0};
    initialBlock.taggedStrings = initialTaggedStrings_;
    playerInfo_ = initialPlayerInfo_;
    currentMissionCrc_ = initialBlock.missionCRC;
    currentMission_ = missionChanges_.empty() ? "" : missionChanges_[0].second;
    nextChangeIdx_ = 0;
    eventLog_.clear();
    weaponsHud_ = {};
    backpackHud_ = {};
    inventoryHud_ = {};
    vehicleHud_ = {};
    ammoHud_ = {};
    ghostTracker.clear();
    for (int index : ibGhostTracker.getAllIndices()) {
        const GhostEntry* source = ibGhostTracker.getGhost(index);
        if (!source) continue;
        ghostTracker.createGhost(index, source->classId, source->className);
        if (GhostEntry* target = ghostTracker.getMutableGhost(index))
            *target = *source;
    }
}

void DemoParser::handleHudRemoteCommand(const std::string& funcName,
                                        const std::vector<std::string>& args) {
    std::string name = funcName;
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.rfind("clientcmd", 0) == 0) name.erase(0, 8);
    auto arg = [&](size_t index) -> std::string {
        return index < args.size() ? args[index] : std::string();
    };
    if (name == "setweaponshuditem" && args.size() >= 4) {
        const int slot = atoi(arg(1).c_str());
        if (atoi(arg(3).c_str()) != 0) weaponsHud_.slots[slot] = atoi(arg(2).c_str());
        else {
            weaponsHud_.slots.erase(slot);
            weaponsHud_.bitmaps.erase(slot);
        }
    } else if (name == "setweaponshudammo" && args.size() >= 3) {
        const int slot = atoi(arg(1).c_str());
        if (weaponsHud_.slots.find(slot) != weaponsHud_.slots.end())
            weaponsHud_.slots[slot] = atoi(arg(2).c_str());
    } else if (name == "setweaponshudbitmap" && args.size() >= 4) {
        weaponsHud_.bitmaps[atoi(arg(1).c_str())] = arg(3);
    } else if (name == "setweaponshudbackgroundbmp" && args.size() >= 2) {
        weaponsHud_.backgroundBitmap = arg(1);
    } else if (name == "setweaponshudhighlightbmp" && args.size() >= 2) {
        weaponsHud_.highlightBitmap = arg(1);
    } else if (name == "setweaponshudinfiniteammobmp" && args.size() >= 2) {
        weaponsHud_.infiniteAmmoBitmap = arg(1);
    } else if (name == "setweaponshudactive" && args.size() >= 2) {
        weaponsHud_.activeIndex = atoi(arg(1).c_str());
    } else if (name == "setammohudcount" && args.size() >= 2) {
        ammoHud_.count = atoi(arg(1).c_str());
    } else if (name == "setweaponshudclearall") {
        weaponsHud_ = {};
    } else if (name == "setinventoryhuditem" && args.size() >= 4) {
        const int slot = atoi(arg(1).c_str());
        if (atoi(arg(3).c_str()) != 0) inventoryHud_.slots[slot] = atoi(arg(2).c_str());
        else {
            inventoryHud_.slots.erase(slot);
            inventoryHud_.bitmaps.erase(slot);
        }
    } else if (name == "setinventoryhudamount" && args.size() >= 3) {
        const int slot = atoi(arg(1).c_str());
        if (inventoryHud_.slots.find(slot) != inventoryHud_.slots.end())
            inventoryHud_.slots[slot] = atoi(arg(2).c_str());
    } else if (name == "setinventoryhudbitmap" && args.size() >= 4) {
        inventoryHud_.bitmaps[atoi(arg(1).c_str())] = arg(3);
    } else if (name == "setinventoryhudbackgroundbmp" && args.size() >= 2) {
        inventoryHud_.backgroundBitmap = arg(1);
    } else if (name == "setinventoryhudclearall") {
        inventoryHud_ = {};
        backpackHud_ = {};
    } else if (name == "setbackpackhuditem" && args.size() >= 3) {
        backpackHud_.packIndex = atoi(arg(1).c_str());
        backpackHud_.active = atoi(arg(2).c_str()) != 0;
        backpackHud_.bitmap.clear();
        if (!backpackHud_.active) {
            backpackHud_.text.clear();
        }
    } else if (name == "setbackpackhudbitmap" && args.size() >= 2) {
        backpackHud_.bitmap = arg(1);
    } else if (name == "updatepacktext" && args.size() >= 2) {
        backpackHud_.text = arg(1);
    } else if (name == "setsatchelarmed" ||
               name == "setcloakiconon" || name == "setcloakiconoff" ||
               name == "setrepairpackiconon" || name == "setrepairpackiconoff" ||
               name == "setshieldiconon" || name == "setshieldiconoff" ||
               name == "setsenjamiconon" || name == "setsenjamiconoff") {
        // These stock commands replace backpackIcon directly in TorqueScript.
        backpackHud_.bitmap.clear();
    } else if (name == "setvweaponshudactive" && args.size() >= 2) {
        vehicleHud_.activeWeapon = atoi(arg(1).c_str());
        if (args.size() >= 3) vehicleHud_.vehicleType = arg(2);
    } else if (name == "setvweaponshudclearall") {
        vehicleHud_.activeWeapon = -1;
    } else if (name == "showvehiclegauges" && args.size() >= 3) {
        vehicleHud_.vehicleType = arg(1);
        vehicleHud_.node = atoi(arg(2).c_str());
        vehicleHud_.dashboardVisible = true;
    }
}

DemoParserSnapshot DemoParser::captureSnapshot() const {
    DemoParserSnapshot snapshot;
    snapshot.blockStreamOffset = (size_t)std::max(0, blockStreamOffset);
    snapshot.blockCount = blockCount_;
    snapshot.blockCursor = blockCursor_;
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
    snapshot.missionChanges = missionChanges_;
    snapshot.taggedStrings = initialBlock.taggedStrings;
    snapshot.currentMissionCrc = currentMissionCrc_;
    snapshot.currentMission = currentMission_;
    snapshot.nextChangeIdx = nextChangeIdx_;
    snapshot.eventLog = eventLog_;
    snapshot.playerInfo = playerInfo_;
    snapshot.skinToPlayer = skinToPlayer_;
    snapshot.weaponsHud = weaponsHud_;
    snapshot.backpackHud = backpackHud_;
    snapshot.inventoryHud = inventoryHud_;
    snapshot.vehicleHud = vehicleHud_;
    snapshot.ammoHud = ammoHud_;
    return snapshot;
}

bool DemoParser::restoreSnapshot(const DemoParserSnapshot& snapshot) {
    if (!decompressed || snapshot.blockStreamOffset > decompressedSize ||
        snapshot.blockCursor < 0 || snapshot.nextChangeIdx < 0 ||
        snapshot.nextChangeIdx > (int)snapshot.missionChanges.size())
        return false;
    blockStreamOffset = (int)snapshot.blockStreamOffset;
    blockCount_ = snapshot.blockCount;
    blockCursor_ = snapshot.blockCursor;
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
    missionChanges_ = snapshot.missionChanges;
    initialBlock.taggedStrings = snapshot.taggedStrings;
    currentMissionCrc_ = snapshot.currentMissionCrc;
    currentMission_ = snapshot.currentMission;
    nextChangeIdx_ = snapshot.nextChangeIdx;
    eventLog_ = snapshot.eventLog;
    playerInfo_ = snapshot.playerInfo;
    skinToPlayer_ = snapshot.skinToPlayer;
    weaponsHud_ = snapshot.weaponsHud;
    backpackHud_ = snapshot.backpackHud;
    inventoryHud_ = snapshot.inventoryHud;
    vehicleHud_ = snapshot.vehicleHud;
    ammoHud_ = snapshot.ammoHud;
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
    std::string name = missionPath;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Remove trailing whitespace
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    return name;
}

void DemoParser::scanMissionChanges() {
    missionChanges_.clear();
    if (!decompressed) return;
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
        for (int j = 0; j + 4 < size; j++) {
            if (d[j] == '.' && d[j+1] == 'm' && d[j+2] == 'i' && d[j+3] == 's') {
                int start = (int)j;
                while (start > 0 && d[start-1] != 0 &&
                       d[start-1] >= 0x20) start--;
                std::string name((const char*)d + start, j + 4 - start);
                bool valid = false;
                for (char c : name) if (c == '/') { valid = true; break; }
                if (valid) {
                    // Only add if different from the last recorded mission
                    if (missionChanges_.empty() || name != missionChanges_.back().second)
                        missionChanges_.push_back({bi, name});
                }
            }
        }
        blockStreamOffset += 2 + size;
    }
    // Restore state
    blockStreamOffset = savedOffset;
    blockCursor_ = savedCursor;
    nextChangeIdx_ = 0;
    // Fallback: use the initial block's mission name if the decompressed scan found nothing
    if (missionChanges_.empty() && !initialBlock.missionName.empty()) {
        std::string extracted = extractMapName(initialBlock.missionName);
        if (!extracted.empty())
            missionChanges_.push_back({0, extracted});
    }
    currentMission_ = missionChanges_.empty() ? "" : missionChanges_[0].second;
    currentMissionCrc_ = initialBlock.missionCRC;
}
void DemoParser::setCurrentBlock(int blockIndex) {
    while (nextChangeIdx_ < (int)missionChanges_.size() &&
           blockIndex >= missionChanges_[nextChangeIdx_].first) {
        currentMission_ = missionChanges_[nextChangeIdx_].second;
        nextChangeIdx_++;
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
    gs.lastMoveAck = bs.readU32();

    // damageFlash and whiteOut (optional 7-bit floats)
    if (bs.readFlag()) {
        if (bs.readFlag())
            gs.damageFlash = bs.readFloat(7);
        if (bs.readFlag())
            gs.whiteOut = bs.readFloat(7) * 1.5f;
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
            if (control && control->classId == 4) {
                gs.compressionPoint = {bs.readF32(), bs.readF32(), bs.readF32()};
                bs.readF32(); // rotation X
                bs.readF32(); // rotation Z
                const int mode = bs.readInt(3);
                if (mode == 3 || mode == 4) {
                    bs.readF32();
                    bs.readF32();
                    bs.readF32();
                    if (mode == 3) {
                        bs.readFlag();
                        bs.readInt(T2Demo::GhostIdBitSize);
                    } else {
                        bs.readCompressedPoint(gs.compressionPoint);
                    }
                } else if (mode == 5) {
                 bs.readInt(T2Demo::GhostIdBitSize);
                }
            } else if (control && control->classId == 25) {
                // Player::readPacketData: ShapeBase state, movement, view,
                // optional piloted object, and final movement flags.
                bs.readInt(3); // action state
                if (bs.readFlag()) bs.readInt(7); // recover ticks
                if (bs.readFlag()) bs.readInt(7); // jump delay
                if (bs.readFlag()) {
                    gs.compressionPoint = {bs.readF32(), bs.readF32(), bs.readF32()};
                    bs.readF32(); bs.readF32(); bs.readF32(); // velocity
                    bs.readInt(4); // jump surface contact
                }
                bs.readF32(); // head X
                bs.readF32(); // head Z
                bs.readF32(); // rotation Z
                if (bs.readFlag()) {
                    const int pilotedIndex = bs.readInt(T2Demo::GhostIdBitSize);
                    const GhostEntry* piloted = ghostTracker.getGhost(pilotedIndex);
                    if (piloted && (piloted->classId == 4)) {
                        bs.readF32(); bs.readF32();
                        const Vec3 pos{bs.readF32(), bs.readF32(), bs.readF32()};
                        gs.compressionPoint = pos;
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
        // Parse known event payloads
        if (ev.classId == T2Demo::NetEventClassFirst + 22) { // SimpleMessageEvent
            ev.message = bs.readString();
        } else if (ev.classId == T2Demo::NetEventClassFirst + 9) { // RemoteCommandEvent
            int argc = bs.readInt(5);
            for (int i = 0; i < argc; i++) {
                std::string arg = bs.unpackNetString();
                if (arg.size() > 2 && arg[0] == '\\' && arg[1] == 'x') {
                    int tag = atoi(arg.c_str() + 2);
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
                if (!bs.isError() && id >= 0 && id < 1024)
                    initialBlock.taggedStrings[id] = value;
            }
        } else if (ev.classId == T2Demo::NetEventClassFirst + 4) { // GhostingMessageEvent
            bs.readU32();
            bs.readInt(3);
            bs.readInt(11);
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
        } else if (ev.classId == T2Demo::NetEventClassFirst + 5) { // GravityEvent
            bs.readF32();
        } else if (ev.classId == T2Demo::NetEventClassFirst + 6) { // LightningStrikeEvent
            bs.readInt(11);
            bs.readFloat(10);
            bs.readFloat(10);
            if (bs.readFlag()) bs.readInt(11);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 12) { // SensorGroupColorEvent
            bs.readInt(5);
            const uint32_t updateMask = bs.readU32();
            for (int i = 0; i < 32; ++i) {
                if ((updateMask & (1u << i)) != 0) {
                    if (bs.readFlag()) {
                        bs.readU8(); bs.readU8(); bs.readU8(); bs.readU8();
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
            if (bs.readFlag()) ev.targetRenderFlags = bs.readInt(9);
            if (bs.readFlag()) ev.targetVoicePitch = bs.readFloat(7) * 1.5f + 0.5f;
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
        } else if (ev.classId == T2Demo::NetEventClassFirst + 25) { // TargetToEvent
            if (bs.readFlag()) bs.readInt(9);
            if (bs.readFlag()) {
                bs.readF32(); bs.readF32(); bs.readF32();
            }
            bs.readFlag();
        } else if (ev.classId == T2Demo::NetEventClassFirst + 23) { // TargetFreeEvent
            ev.hasTargetFree = true;
            ev.targetId = bs.readInt(9);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 13) { // SetMissionCRCEvent
            ev.hasMissionCrc = true;
            ev.missionCrc = bs.readU32();
            currentMissionCrc_ = ev.missionCrc;
        } else if (ev.classId == T2Demo::NetEventClassFirst + 12) { // SensorGroupColorEvent
            bs.readInt(4); bs.readU32();
        } else {
            // Unknown event: break to avoid stream corruption
            ev.dataBitsEnd = bs.getCurPos();
            outEvents.push_back(ev);
            break;
        }
        ev.dataBitsEnd = bs.getCurPos();
        outEvents.push_back(ev);
        more = bs.readFlag();
    }
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
    if (bs.readFlag() && bs.readFlag()) bs.readInt(9);
}

static void readShapeBaseData(BitStream& bs, bool isInitial, GhostEntry* entry = nullptr) {
    readGameBaseData(bs, isInitial, entry);
    if (!bs.readFlag()) return;
    // DamageMask
    if (bs.readFlag()) {
        float dmg = bs.readFloat(6);
        if (entry) entry->health = (1.0f - dmg) * 100.0f;
        bs.readInt(2); bs.readFlag(); bs.readNormalVector(8);
    }
    // SoundMask (4 slots: flag → playing flag → optional 11-bit profileId)
    if (bs.readFlag()) {
        for (int i = 0; i < 4; i++)
            if (bs.readFlag()) { bool playing = bs.readFlag(); if (playing) bs.readInt(11); }
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
                    entry->threads[i].sequence = sequence;
                    entry->threads[i].state = state;
                    entry->threads[i].forward = forward;
                    entry->threads[i].atEnd = atEnd;
                    entry->threads[i].valid = true;
                }
            }
    }
    // ImageMask (8 mounted image slots)
    if (bs.readFlag()) {
        for (int i = 0; i < 8; i++) {
            if (bs.readFlag()) {
                if (bs.readFlag()) {
                    int dbId = bs.readInt(11);
                    if (entry && i < 8) entry->mountedImages[i].datablockId = dbId;
                }
                if (bs.readFlag()) {
                    if (bs.readFlag()) bs.readInt(10); else {
                        std::string s = bs.readString();
                        if (entry && entry->skinName.empty() && !s.empty())
                            entry->skinName = s;
                    }
                }
                bool triggerDown = bs.readFlag();
                bool loaded = bs.readFlag();
                bs.readFlag(); // ammo
                bs.readFlag(); // wet
                bs.readFlag(); // target
                int fireCount = bs.readInt(3);
                if (entry && i < 8) {
                    entry->mountedImages[i].loaded = loaded;
                    entry->mountedImages[i].isFiring = (fireCount > 0);
                }
                if (isInitial) bs.readFlag();
            }
        }
    }
    // CloakMask + state-B + invincibility state.
    if (bs.readFlag()) {
        if (bs.readFlag()) {
            bool cloaked = bs.readFlag();
            bs.readFlag(); // controlled
            if (entry) entry->cloaked = cloaked;
            if (bs.readFlag()) { bs.readFlag(); bs.readF32(); }
            else bs.readFlag();
        }
        if (bs.readFlag()) {
            if (bs.readFlag()) {
                bs.readFlag(); // state-B mode
            } else {
                bs.readNormalVector(8);
                bs.readFloat(5);
            }
        }
        if (bs.readFlag()) {
            bs.readU32();
            bs.readU32();
        }
    }
    if (bs.readFlag()) {
        if (bs.readFlag()) {
            bs.readInt(10);
            bs.readInt(5);
        }
    }
}

static void readPlayerData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) bs.readInt(3); // ImpactMask
    if (bs.readFlag()) { // ActionMask - action animation
        bs.readInt(8); bs.readFlag(); bool atEnd = bs.readFlag(); bs.readFlag();
        if (!atEnd && bs.readFlag()) bs.readSignedFloat(6);
    }
    if (bs.readFlag()) bs.readInt(8); // ArmAction
    if (bs.readFlag()) return; // control object shortcut
    if (bs.readFlag()) { // MoveMask
        int actionState = bs.readInt(3); // actionState: 0=Stop, 1=Walk, 2=Run, 3=Sprint
        if (entry) entry->isMoving = (actionState > 0);
        if (bs.readFlag()) bs.readInt(7); // recoverState
        bs.readFlag(); bs.readFlag(); // move flags
        if (entry) entry->position = bs.readCompressedPoint(cp);
        else bs.readCompressedPoint(cp);
        if (bs.readFlag()) { bs.readInt(13); bs.readNormalVector(10); }
        float headX = bs.readSignedFloat(6); // head pitch
        float headZ = bs.readSignedFloat(6); // head yaw
    float bodyYaw = bs.readFloat(7) * (2.0f * 3.14159f); // rotationZ (0-1 maps to 0-2PI)
        if (entry) {
            // Always update body yaw rotation from MoveMask
            float half = bodyYaw * 0.5f;
            entry->rotation = {0, sinf(half), 0, cosf(half)};
            entry->hasRotation = true;
            entry->headPitch = headX;
            entry->headYaw = headZ;
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
    bs.readFlag(); // jetting
    if (bs.readFlag()) { // control shortcut
        return;
    }
    bs.readFloat(9); bs.readFloat(9); // steering
    readMove(bs);
    bs.readFlag(); // frozen
    if (bs.readFlag()) { // PositionMask
        if (entry) {
            entry->position = bs.readCompressedPoint(cp);
        } else bs.readCompressedPoint(cp);
        float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32(), qw = bs.readF32();
        if (entry) { entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true; }
        bs.readPoint3F(); // linMomentum
        bs.readPoint3F(); // angMomentum
    }
    if (bs.readFlag()) bs.readFloat(8); // EnergyMask
}

static void readFlyingVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    if (bs.readFlag()) return; // FlyingVehicle control shortcut
    bs.readFlag(); // createHeightOn
    bs.readInt(3); // thrustDirection
}

static void readHoverVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    bs.readInt(3); // thrustDirection
}

static void readWheeledVehicleData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readVehicleData(bs, isInitial, cp, entry);
    bs.readFlag(); // braking
    if (bs.readFlag()) {
        for (int i = 0; i < 6; ++i) {
            bs.readF32(); // wheel angular velocity
            bs.readF32(); // wheel suspension displacement
            bs.readF32(); // wheel lateral displacement
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
        bs.readFlag(); bs.readFlag(); bs.readFlag(); // rotate, isStatic, collideable
        if (bs.readFlag()) bs.readPoint3F(); // scale
    }
    if (bs.readFlag()) bs.readInt(10); // ThrowSrcMask
    if (bs.readFlag()) { bs.readFlag(); bs.readF32(); } // RotationMask (zSign, angle)
    if (bs.readFlag()) { // PositionMask
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        bool atRest = bs.readFlag();
        if (!atRest) bs.readPoint3F(); // velocity
        bs.readFlag(); // warp
    }
}

static void readCameraData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) return; // control object shortcut
    if (bs.readFlag()) { // camera update mask
        bs.readF32(); bs.readF32(); bs.readF32();
        bs.readF32(); bs.readF32();
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
    readGameBaseData(bs, isInitial);
    if (!bs.readFlag()) return; // non-full state
    if (entry) entry->position = bs.readCompressedPoint(cp);
    else bs.readCompressedPoint(cp);
    bs.readCompressedPoint(cp); // velocity
    if (bs.readFlag()) bs.readInt(10); // source
    if (bs.readFlag()) bs.readInt(10); // vehicleObject
}

static void readRepairProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry*) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag() && bs.readFlag()) {
        bs.readInt(11); // source object
        bs.readInt(3); // source slot
        bs.readInt(11); // repairing object
    }
}

static void readDebrisData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) {
        if (entry) entry->position = bs.readCompressedPoint(cp);
        else bs.readCompressedPoint(cp);
    }
    for (int i = 0; i < 3; i++) bs.readF32(); // velocity
    for (int i = 0; i < 4; i++) bs.readBool();
    for (int i = 0; i < 6; i++) bs.readF32();
    for (int i = 0; i < 2; i++) bs.readBool();
    for (int i = 0; i < 3; i++) bs.readF32();
    bs.readBool();
    bs.readString(); bs.readString();
    for (int i = 0; i < 3; i++) { if (bs.readFlag()) bs.readInt(11); }
}

static void readGrenadeData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) { // initial update
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        Vec3 vel = bs.readPoint3F(); // velocity
        if (entry && (vel.x != 0 || vel.y != 0 || vel.z != 0)) {
            // Orient projectile along velocity vector
            float len = sqrtf(vel.x*vel.x + vel.y*vel.y + vel.z*vel.z);
            if (len > 0.001f) {
                Vec3 dir = {vel.x/len, vel.y/len, vel.z/len};
                // Build rotation from forward (0,1,0) to direction
                float yaw = atan2f(dir.x, dir.y);
                float half = yaw * 0.5f;
                entry->rotation = {0, sinf(half), 0, cosf(half)};
                entry->hasRotation = true;
            }
        }
        bs.readRangedU32(0, 4095); // currTick
        bs.readFlag(); // quickSplash
        if (bs.readFlag()) {
            Vec3 expPos = bs.readPoint3F();
            bs.readPoint3F(); // normal
            DemoParser::s_pendingExplosions.push_back({expPos, 0.0f});
        }
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); } // source
        if (bs.readFlag()) bs.readRangedU32(0, 1024); // vehicleObject
    } else { // non-initial
        if (bs.readFlag()) { // BounceMask
            if (entry) entry->position = bs.readPoint3F();
            else bs.readPoint3F();
            bs.readPoint3F(); // velocity
        }
        if (bs.readFlag()) {
            Vec3 expPos = bs.readPoint3F();
            bs.readPoint3F(); // normal
            DemoParser::s_pendingExplosions.push_back({expPos, 0.0f});
        }
    }
}

static void readSniperProjectileData(BitStream& bs, bool isInitial, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) { // initial
        bs.readFloat(7); // energyPercentage
        Vec3 startPos = bs.readPoint3F();
        Vec3 endPos = bs.readPoint3F(); // endPos
        if (entry) entry->position = startPos;
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
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) bs.readRangedU32(0, 1024); // targetObject
    if (bs.readFlag()) { // initial update
        Vec3 start = bs.readPoint3F();
        Vec3 end = bs.readPoint3F(); // end
        if (entry) entry->position = start;
        bs.readFlag(); // hitObject
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); }
    }
}

static void readBombProjectileData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    // Inlined GameBase update
    if (bs.readFlag()) bs.readInt(11);
    if (bs.readFlag()) { if (bs.readFlag()) bs.readInt(9); }
    if (!bs.readFlag()) { // non-full
        if (bs.readFlag()) {
            if (entry) entry->position = bs.readPoint3F();
            else bs.readPoint3F();
            Vec3 vel = bs.readPoint3F(); // velocity
            if (entry && (vel.x != 0 || vel.y != 0 || vel.z != 0)) {
                float len = sqrtf(vel.x*vel.x + vel.y*vel.y + vel.z*vel.z);
                if (len > 0.001f) {
                    Vec3 dir = {vel.x/len, vel.y/len, vel.z/len};
                    float yaw = atan2f(dir.x, dir.y);
                    float half = yaw * 0.5f;
                    entry->rotation = {0, sinf(half), 0, cosf(half)};
                    entry->hasRotation = true;
                }
            }
        }
        if (!bs.readFlag()) return;
        bs.readPoint3F(); bs.readPoint3F(); return; // endPoint, endNormal
    }
    // full state
    if (entry) entry->position = bs.readPoint3F();
    else bs.readPoint3F();
    Vec3 vel2 = bs.readPoint3F(); // velocity
    if (entry && (vel2.x != 0 || vel2.y != 0 || vel2.z != 0)) {
        float len = sqrtf(vel2.x*vel2.x + vel2.y*vel2.y + vel2.z*vel2.z);
        if (len > 0.001f) {
            Vec3 dir = {vel2.x/len, vel2.y/len, vel2.z/len};
            float yaw = atan2f(dir.x, dir.y);
            float half = yaw * 0.5f;
            entry->rotation = {0, sinf(half), 0, cosf(half)};
            entry->hasRotation = true;
        }
    }
    bs.readInt(12); // currTick
    if (bs.readFlag()) {}
    if (bs.readFlag()) { bs.readPoint3F(); bs.readPoint3F(); }
    if (bs.readFlag()) { bs.readInt(11); bs.readInt(3); }
    if (bs.readFlag()) bs.readInt(11);
}

static void readLinearProjectileData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) { // InitialUpdateMask
        if (bs.readFlag()) { // hidden/already exploded
            Vec3 expPos = bs.readCompressedPoint(cp);
            bs.readNormalVector(14);
            bool hitWater = bs.readFlag();
            DemoParser::s_pendingExplosions.push_back({expPos, 0.0f});
        } else { // live projectile
            if (entry) entry->position = bs.readCompressedPoint(cp);
            else bs.readCompressedPoint(cp);
            Vec3 dir = bs.readNormalVector(14); // direction
            if (entry && (dir.x != 0 || dir.y != 0 || dir.z != 0)) {
                float yaw = atan2f(dir.x, dir.y);
                float half = yaw * 0.5f;
                entry->rotation = {0, sinf(half), 0, cosf(half)};
                entry->hasRotation = true;
            }
            bs.readRangedU32(0, 511); // currTick
            if (bs.readFlag()) {
                bs.readInt(10); bs.readRangedU32(0, 7); // source
                if (bs.readFlag()) { bs.readRangedU32(0, 255); bs.readNormalVector(7); }
            }
            if (bs.readFlag()) bs.readInt(10); // vehicleObject
        }
    } else { // non-initial: explosion
        Vec3 expPos = bs.readCompressedPoint(cp);
        bs.readNormalVector(14);
        bs.readFlag();
        DemoParser::s_pendingExplosions.push_back({expPos, 0.0f});
    }
}

static void readSeekerProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    const bool fullState = bs.readFlag();
    if (!fullState) {
        if (bs.readFlag()) {
            bs.readPoint3F();
            bs.readPoint3F();
            return;
        }
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        bs.readPoint3F();
        if (bs.readFlag()) {
            if (!bs.readFlag()) bs.readPoint3F();
            else bs.readInt(11);
        }
        return;
    }
    if (entry) entry->position = bs.readPoint3F();
    else bs.readPoint3F();
    bs.readPoint3F();
    bs.readPoint3F();
    if (bs.readFlag()) { bs.readInt(11); bs.readInt(3); }
    if (bs.readFlag()) {
        if (!bs.readFlag()) bs.readPoint3F();
        else bs.readInt(11);
    }
    bs.readFlag();
}

static void readSkyData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    if (bs.readFlag()) {
        bs.readString();
        bs.readF32(); bs.readF32(); bs.readF32();
        const uint32_t fogCount = bs.readU32();
        if (fogCount > 64) { bs.skipBits(bs.getRemainingBits()); return; }
        bs.readBool(); bs.readBool();
        bs.readF32(); bs.readF32(); bs.readF32();
        bs.readBool();
        for (uint32_t i = 0; i < fogCount; ++i) {
            for (int j = 0; j < 6; ++j) bs.readF32();
        }
        for (int i = 0; i < 3; ++i) {
            bs.readString(); bs.readF32(); bs.readF32();
        }
        bs.readPoint3F(); bs.readF32();
        if (bs.readFlag()) for (int j = 0; j < 5; ++j) bs.readF32();
    }
    if (bs.readFlag()) bs.readBool();
    if (bs.readFlag()) bs.readBool();
    if (bs.readFlag()) { bs.readF32(); bs.readF32(); }
    if (bs.readFlag()) { bs.readF32(); bs.readF32(); }
    if (bs.readFlag()) for (int j = 0; j < 3; ++j) bs.readF32();
    if (bs.readFlag()) for (int j = 0; j < 4; ++j) bs.readF32();
    if (bs.readFlag()) bs.readPoint3F();
}

static void readSunData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    if (bs.readFlag()) for (int i = 0; i < 5; ++i) bs.readString();
    if (bs.readFlag()) {
        auto& sd = DemoParser::s_sunData;
        sd.direction = {bs.readF32(), bs.readF32(), bs.readF32()};
        sd.r = (int)bs.readF32(); sd.g = (int)bs.readF32(); sd.b = (int)bs.readF32();
        for (int i = 0; i < 13; ++i) bs.readF32();
        sd.valid = true;
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

static void readMissionAreaData(BitStream& bs) {
    if (bs.readFlag()) {
        bs.readS32(); bs.readS32(); bs.readS32(); bs.readS32();
        bs.readF32(); bs.readF32();
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

static void readTSStaticData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    bs.readMatrixF(); bs.readPoint3F(); bs.readString();
}

static void readAudioEmitterData(BitStream& bs) {
    bs.readFlag();
    if (bs.readFlag()) {
        bs.readPoint3F();
        bs.readF32(); bs.readF32(); bs.readF32();
        bs.readFlag();
    }
    if (bs.readFlag() && bs.readFlag()) bs.readInt(11);
    if (bs.readFlag() && bs.readFlag()) bs.readInt(11);
    if (bs.readFlag()) bs.readString();
    if (bs.readFlag()) bs.readFlag();
    if (bs.readFlag()) bs.readF32();
    if (bs.readFlag()) bs.readFlag();
    if (bs.readFlag()) bs.readFlag();
    if (bs.readFlag()) bs.readF32();
    if (bs.readFlag()) bs.readF32();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readF32();
    if (bs.readFlag()) bs.readPoint3F();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readS32();
    if (bs.readFlag()) bs.readFlag();
}

static void readTerrainBlockData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry*) {
    if (bs.readFlag()) { // init
        bs.readU32(); // CRC
        std::string terrFile = bs.readString(); // terrain file name
        std::string detailTex = bs.readString(); // detail texture name
        bs.readU32(); // squareSize
        // Read empty square RLE
        uint32_t emptySize = bs.readU32();
        for (uint32_t i = 0; i < emptySize; i++) bs.readU32();
        // Store terrain file name for later loading
        if (!terrFile.empty()) {
            DemoParser::s_pendingTerrainFile = terrFile;
        }
    } else {
        // Normal update: empty square RLE
        uint32_t emptySize = bs.readU32();
        for (uint32_t i = 0; i < emptySize; i++) bs.readU32();
    }
}

static void readWaterBlockData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
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
        bs.readPoint3F(); bs.readF32(); bs.readF32(); bs.readF32(); bs.readFlag();
    }
    bs.readPoint3F();
    bs.readString(); bs.readString(); bs.readString(); bs.readString();
    bs.readS32();
    bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32(); bs.readF32();
    bs.readU8();
    if (bs.readFlag()) bs.readInt(11);
}

static void readVehicleBlockerData(BitStream& bs) {
    bs.readMatrixF();
    bs.readPoint3F();
    bs.readPoint3F();
}

static void readInteriorData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    if (bs.readFlag()) { // InitMask - full initial state
        bs.readU32(); bs.readString(); bs.readFlag();
        Vec3 matPos;
        bs.readMatrixF(&matPos);
        if (entry) { entry->position = matPos; }
        bs.readPoint3F();
        bs.readFlag(); bs.readString();
        if (bs.readFlag()) bs.readInt(11);
        if (bs.readFlag()) bs.readInt(11);
    } else { // normal update
        if (bs.readFlag()) {
            Vec3 matPos;
            bs.readMatrixF(&matPos);
            if (entry) { entry->position = matPos; }
            bs.readPoint3F();
        }
        bs.readFlag();
        if (bs.readFlag()) bs.readString();
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

    // If no tagged name, try default name by index
    if (cn.empty() || cn.rfind("Class", 0) == 0) {
        static const char* defaultNames[] = {
            "GameBase", "ShapeBase", "Player", "Vehicle", "FlyingVehicle",
            "HoverVehicle", "Item", "StaticShape", "ScopeAlwaysShape", "Marker",
            "SimpleNetObject", "BeaconObject", "MissionMarker", "Debris",
            "Projectile", "BombProjectile", "GrenadeProjectile", "SeekerProjectile",
            "Turret", "InteriorInstance", "Camera", "LinearProjectile",
            "ELFProjectile", "RepairProjectile", "TargetProjectile", "WayPoint",
            "SpawnSphere", "ForceFieldBare", "TSStatic", "TerrainBlock",
            "Sun", "Sky", "Lightning", "WaterBlock", "MissionArea",
            "Splash", "Shockwave", "FireballAtmosphere", "VehicleBlocker",
            "ParticleEmissionDummy", "Precipitation", "WheeledVehicle",
            "Trigger", "PhysicalZone", "AudioEmitter", "StationFXPersonal",
            "AIObjective", "SniperProjectile", "ShockLanceProjectile"
        };
        int idx = classId - T2Demo::NetObjectClassFirst;
        if (idx >= 0 && idx < (int)(sizeof(defaultNames)/sizeof(defaultNames[0])))
            cn = defaultNames[idx];
    }

    // Log unknown class first time
    static std::set<int> s_logged;
    bool known = true;
    if (cn == "Player" || cn == "MPB") readPlayerData(bs, isInitial, cp, entry);
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
            float barrelPitch = bs.readFloat(10);
            float barrelYaw = bs.readFloat(10);
            bs.readFloat(8);
            if (entry) {
                entry->barrelPitch = barrelPitch;
                entry->barrelYaw = barrelYaw;
            }
        }
    }
    else if (cn == "Item" || cn == "mine") readItemData(bs, isInitial, cp, entry);
    else if (cn == "BeaconObject") readBeaconObjectData(bs, isInitial, cp, entry);
    else if (cn == "Camera") readCameraData(bs, isInitial, cp, entry);
    else if (cn == "Marker") readMarkerData(bs, false, cp, entry);
    else if (cn == "MissionMarker") readMissionMarkerData(bs, isInitial, cp, entry);
    else if (cn == "MissionArea") readMissionAreaData(bs);
    else if (cn == "PhysicalZone") readPhysicalZoneData(bs, isInitial, cp, entry);
    else if (cn == "WayPoint") readWayPointData(bs, isInitial, cp, entry);
    else if (cn == "SpawnSphere") readSpawnSphereData(bs, isInitial, cp, entry);
    else if (cn == "Debris") readDebrisData(bs, isInitial, cp, entry);
    else if (cn == "Projectile")
        readProjectileData(bs, isInitial, cp, entry);
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
    else if (cn == "TSStatic") readTSStaticData(bs, isInitial, cp, entry);
    else if (cn == "AudioEmitter") readAudioEmitterData(bs);
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
    else if (cn == "GameBase") readGameBaseData(bs, isInitial);
    else known = false;

    if (!known) {
        if (s_logged.find(classId) == s_logged.end()) {
            s_logged.insert(classId);
            Console::instance().printf(LogLevel::Debug, "Ghost class %d: %s", classId, cn.c_str());
        }
        // Unknown class: read GameBase base data. Don't skip remaining bits.
        readGameBaseData(bs, isInitial);
        return false;
    }
    return true;
}

void DemoParser::readGhosts(BitStream& bs, std::vector<GhostUpdate>& outGhosts, int seqNumber, const Vec3* compressionPoint) {
    const Vec3 cp = compressionPoint ? *compressionPoint : Vec3{};
    if (!bs.readFlag()) return;
    int idSize = bs.readInt(3) + 3;
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
            readGameBaseData(bs, isNew);
            if (bs.readFlag()) {
                if (bs.readFlag()) { bs.readFloat(6); bs.readInt(2); bs.readFlag(); bs.readNormalVector(8); }
                if (bs.readFlag()) for (int i = 0; i < 4; i++) if (bs.readFlag()) { bool p = bs.readFlag(); if (p) bs.readInt(11); }
                if (bs.readFlag()) for (int i = 0; i < 4; i++) if (bs.readFlag()) { bs.readInt(5); bs.readInt(2); bs.readFlag(); bs.readFlag(); }
                if (bs.readFlag()) for (int i = 0; i < 8; i++) if (bs.readFlag()) {
                    if (bs.readFlag()) bs.readInt(11);
                    if (bs.readFlag()) { if (bs.readFlag()) bs.readInt(10); else bs.readString(); }
                    bs.readFlag(); bs.readFlag(); bs.readFlag(); bs.readFlag(); bs.readFlag();
                    bs.readInt(3);
                    if (isNew) bs.readFlag();
                }
                if (bs.readFlag()) {
                    if (bs.readFlag()) { bs.readFlag(); bs.readFlag(); if (bs.readFlag()) { bs.readFlag(); bs.readF32(); } }
                    if (bs.readFlag()) { bs.readFlag(); bs.readNormalVector(8); bs.readFloat(5); if (bs.readFlag()) { bs.readU32(); bs.readU32(); } }
                    if (bs.readFlag()) { bs.readInt(10); bs.readInt(5); }
                }
            }
            if (bs.readFlag()) {
                if (entry) entry->position = bs.readCompressedPoint(cp);
                else bs.readCompressedPoint(cp);
                float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32();
                bool qwNeg = bs.readFlag();
                float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
                if (qwNeg) qw = -qw;
                if (entry) { entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true; }
            }
        }
        gu.updateBitsEnd = bs.getCurPos();
        outGhosts.push_back(gu);
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

PacketData DemoParser::parsePacket(const uint8_t* data, size_t size, int blockIndex) {
    PacketData pd{};
    BitStream bs(data, size);
    pd.dnetHeader = readDnetHeader(bs);
    bool dispatchData = false;
    if (!applyProtocolHeader(pd.dnetHeader, dispatchData) || !dispatchData)
        return pd;
    if (bs.readFlag()) { bs.readInt(10); bs.readInt(10); }
    if (bs.readFlag()) { bs.readInt(10); bs.readInt(10); }
    bs.setStringBufferEnabled(true);
    pd.gameState = readGameState(bs);
    readEvents(bs, pd.events, pd.gameState.compressionPoint);
    readGhosts(bs, pd.ghosts, pd.dnetHeader.seqNumber, &pd.gameState.compressionPoint);
    bs.setStringBufferEnabled(false);
    return pd;
}

void DemoParser::onSendPacketTrigger() {
    ++lastSendSeq;
    lastSeqRecvdAtSend[lastSendSeq & 0x1f] = lastSeqRecvd;
}
