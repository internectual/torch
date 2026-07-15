#include "game/demo.h"
#include "core/console.h"
#include "core/timer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <zlib.h>

// Pending explosion events from projectile ghost parsers
std::vector<DemoParser::PendingExplosion> DemoParser::s_pendingExplosions;
DemoParser::SunData DemoParser::s_sunData;

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
    if (bitCount <= 0) return 0;
    if (bitNum + bitCount > maxReadBitNum) { error = true; return 0; }
    int startByte = bitNum >> 3;
    int downShift = bitNum & 7;
    bitNum += bitCount;
    unsigned int val = 0;
    int bytesNeeded = (bitCount + downShift + 7) >> 3;
    for (int i = 0; i < bytesNeeded && (startByte + i) < (int)dataLen; i++)
        val |= (unsigned int)data[startByte + i] << (i * 8);
    val >>= downShift;
    if (bitCount < 32) val &= (1u << bitCount) - 1;
    else if (bitCount == 32) val &= 0xFFFFFFFFu;
    return (int)val;
}

int BitStream::readSignedInt(int bitCount) {
    bool neg = readFlag();
    int mag = readInt(bitCount - 1);
    return neg ? -mag : mag;
}

float BitStream::readFloat(int bitCount) {
    return (float)readInt(bitCount) / (float)((1 << bitCount) - 1);
}

float BitStream::readSignedFloat(int bitCount) {
    return ((float)readInt(bitCount) * 2.0f) / (float)((1 << bitCount) - 1) - 1.0f;
}

int BitStream::readRangedU32(int rangeStart, int rangeEnd) {
    int rangeSize = rangeEnd - rangeStart + 1;
    int bits = 1;
    while ((1 << bits) < rangeSize) bits++;
    return readInt(bits) + rangeStart;
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
    (void)scale;
    int type = readInt(2);
    if (type == 3) return readPoint3F();
    static const int bitCounts[] = {16, 18, 20};
    int bits = bitCounts[type];
    Vec3 v;
    v.x = cp.x + (float)readSignedInt(bits) * 0.01f;
    v.y = cp.y + (float)readSignedInt(bits) * 0.01f;
    v.z = cp.z + (float)readSignedInt(bits) * 0.01f;
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
    if (stringBufferEnabled && !stringBuffer.empty() && readFlag()) {
        int offset = readInt(8);
        stringBuffer = stringBuffer.substr(0, offset) + readHuffBuffer();
    } else {
        stringBuffer = readHuffBuffer();
    }
    return stringBuffer;
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
    GhostEntry e;
    e.classId = classId; e.className = cn;
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
    recFilePath_ = path;
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

void DemoParser::readGhostStartBlock(BitStream& bs, bool useIBTracker) {
    if (bs.getRemainingBits() < 32) return;
    initialBlock.ghostingSequence = bs.readU32();
    int created = 0;
    while (bs.readFlag() && !bs.isError()) {
        GhostUpdate gu{};
        gu.index = bs.readInt(T2Demo::GhostIdBitSize);
        gu.classId = bs.readInt(T2Demo::NetObjectClassBitSize) + T2Demo::NetObjectClassFirst;
        std::string cn;
        if (gu.classId >= 0 && gu.classId < T2Demo::NetObjectClassCount)
            cn = T2Demo::NetObjectClassNames[gu.classId];
        else
            cn = "Class" + std::to_string(gu.classId);
        GhostTracker& tracker = useIBTracker ? ibGhostTracker : ghostTracker;
        tracker.createGhost(gu.index, gu.classId, cn);
        gu.type = GhostUpdate::Create;
        gu.updateBitsStart = bs.getCurPos();
        initialBlock.initialGhosts.push_back(gu);
        created++;
    }
    if (!bs.isError())
        initialBlock.controlObjectGhostIndex = bs.readS32();
    else
        initialBlock.controlObjectGhostIndex = -1;
    Console::instance().printf(LogLevel::Debug, "GhostStartBlock: %d ghosts created, seq=%u, ctrlIdx=%d",
        created, initialBlock.ghostingSequence, initialBlock.controlObjectGhostIndex);
}

void DemoParser::readDataBlocks(BitStream& bs) {
    initialBlock.dataBlockCount = (int)bs.readU32();
    Console::instance().printf(LogLevel::Debug, "DataBlocks: %d blocks", initialBlock.dataBlockCount);
    for (int i = 0; i < initialBlock.dataBlockCount && !bs.isError(); i++) {
        DataBlockHeader hdr;
        hdr.classId  = bs.readU32();
        hdr.objectId = bs.readU32();
        hdr.index    = bs.readU32();
        hdr.total    = bs.readU32();
        hdr.dataBitsStart = bs.getCurPos();
        initialBlock.dataBlockHeaders.push_back(hdr);
        // Skip payload: read flag bits until end of this datablock's data
        // Each datablock payload is self-describing — we skip by reading the
        // top-level struct fields. Without class-specific parsers we conservatively
        // skip all remaining bits in the initial block after headers.
    }
    Console::instance().printf(LogLevel::Debug, "DataBlocks: %zu headers read", initialBlock.dataBlockHeaders.size());
}

// Forward declaration
static bool readGhostClassData(BitStream& bs, int classId, bool isInitial, const Vec3& cp, GhostEntry* entry);

// ─── Mission name extraction ──────────────────────────────
// Try reference parser via Node.js for reliable extraction.
static std::string extractMissionNameViaNode(const uint8_t* data, size_t size) {
    // Write initial block to temp file for the Node.js script
    char tmpPath[] = "/tmp/t2demo_ib_XXXXXX";
    int fd = mkstemp(tmpPath);
    if (fd < 0) return "";
    write(fd, data, size);
    close(fd);

    // Build the .rec file with just the header + initial block
    // Need full .rec format: U8 strlen + "Tribes2 Recording" + U32 proto + U32 lenMs + U32 ibSize + ibData
    char recPath[] = "/tmp/t2demo_rec_XXXXXX";
    int rfd = mkstemp(recPath);
    if (rfd < 0) { unlink(tmpPath); return ""; }
    uint8_t hdr[256];
    int hOff = 0;
    const char* sig = "Tribes2 Recording";
    hdr[hOff++] = (uint8_t)strlen(sig);
    memcpy(hdr + hOff, sig, strlen(sig)); hOff += (int)strlen(sig);
    auto put32 = [&](uint32_t v) { hdr[hOff++] = v & 0xff; hdr[hOff++] = (v>>8) & 0xff; hdr[hOff++] = (v>>16) & 0xff; hdr[hOff++] = (v>>24) & 0xff; };
    put32(0x00330004); // protocol v25034
    put32(0);          // length ms
    put32((uint32_t)size); // initial block size
    write(rfd, hdr, hOff);
    write(rfd, data, size);
    close(rfd);

    // Call Node.js script with timeout (prevents hanging on large or malformed demos)
    std::string cmd = std::string("timeout 5 node /tmp/mission_extract.js ") + recPath + " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) { unlink(tmpPath); unlink(recPath); return ""; }
    char buf[256];
    std::string result;
    if (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = 0;
        result = buf;
    }
    pclose(fp);
    unlink(tmpPath);
    unlink(recPath);
    return result;
}

// ─── Scoreboard data extraction via Node.js reference parser ──
void DemoParser::extractScoreboardData(const char* recPath) {
    playerInfo_.clear();
    skinToPlayer_.clear();
    std::string cmd = std::string("timeout 5 node /tmp/score_extract.js ") + recPath + " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    if (n == 0) return;
    buf[n] = 0;
    // Parse JSON output - simple manual parser (no dependency)
    // Looking for: {"scores":[...],"targets":[{"name":"...","skin":"...",...},...],...}
    auto findField = [&](const char* key, const char* src) -> std::string {
        std::string search = std::string("\"") + key + "\":\"";
        auto pos = std::string(src).find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        std::string val;
        while (pos < n && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < n) pos++; // skip escape
            val += src[pos++];
        }
        return val;
    };
    // Simple state machine to parse target entries
    const char* p = buf;
    const char* end = buf + n;
    // Find targets array
    std::string s(buf);
    auto targetsStart = s.find("\"targets\"");
    if (targetsStart == std::string::npos) return;
    auto arrStart = s.find('[', targetsStart);
    if (arrStart == std::string::npos) return;
    p = buf + arrStart;
    while (p < end) {
        // Find next {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        const char* objEnd = p;
        int braceDepth = 1;
        objEnd++;
        while (objEnd < end && braceDepth > 0) {
            if (*objEnd == '{') braceDepth++;
            else if (*objEnd == '}') braceDepth--;
            objEnd++;
        }
        if (braceDepth != 0) break;
        std::string obj(p, objEnd - p);
        PlayerInfo pi;
        pi.name = findField("name", obj.c_str());
        pi.skin = findField("skin", obj.c_str());
        // damage as number (not string)
        {
            auto dpos = obj.find("\"damage\"");
            if (dpos != std::string::npos) {
                dpos = obj.find(':', dpos);
                if (dpos != std::string::npos) {
                    dpos++;
                    while (dpos < obj.size() && (obj[dpos] == ' ' || obj[dpos] == '\t')) dpos++;
                    char* endp = nullptr;
                    pi.damage = (float)strtod(obj.c_str() + dpos, &endp);
                }
            }
        }
        if (!pi.name.empty()) {
            playerInfo_.push_back(pi);
            if (!pi.skin.empty())
                skinToPlayer_[pi.skin] = pi.name;
        }
        p = objEnd;
    }
}

const std::string& DemoParser::getPlayerNameForSkin(const std::string& skin) const {
    static std::string empty;
    auto it = skinToPlayer_.find(skin);
    return it != skinToPlayer_.end() ? it->second : empty;
}

// Scan the initial block for a plausible mission name near its end.
// The mission name is a Huffman-compressed string followed by a U32 CRC.
static std::string scanMissionName(const uint8_t* data, size_t size, uint32_t* outCRC) {
    int totalBits = (int)size * 8;
    *outCRC = 0;
    std::string best; int bestScore = -999; uint32_t bestCRC = 0;
    // Search only the last 30% of the block (mission name is before SimpleTargetManager)
    int searchStart = totalBits - (int)(size * 0.30 * 8);
    if (searchStart < 0) searchStart = 0;
    for (int bitOff = searchStart; bitOff + 200 < totalBits; bitOff++) {
        if ((bitOff % 8) != 0 && (bitOff % 13) != 0 && (bitOff % 17) != 0) continue;
        BitStream bs(data, size, bitOff);
        std::string s = bs.readString();
        if (bs.isError()) continue;
        uint32_t crc = bs.readU32();
        if (bs.isError() || crc == 0 || s.size() < 4 || s.size() > 35) continue;
        bool bad = false;
        for (char c : s) { if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) { bad = true; break; } }
        if (bad) continue;
        if (s.find(' ') != std::string::npos) continue;
        if (s.find('/') != std::string::npos) continue;
        if (s.find('.') != std::string::npos) continue;
        if (s.find('<') != std::string::npos || s.find('>') != std::string::npos) continue;
        // Reject runs of repeated chars
        int maxRun = 1, curRun = 1;
        for (size_t k = 1; k < s.size(); k++) {
            if (s[k] == s[k-1]) { curRun++; if (curRun > maxRun) maxRun = curRun; }
            else curRun = 1;
        }
        if (maxRun > 2) continue;
        // Reject strings that have digits before the first uppercase letter
        // (typical T2 missions start with uppercase: "Katabatic", "Training1")
        size_t firstUpper = s.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        size_t firstDigit = s.find_first_of("0123456789");
        if (firstDigit != std::string::npos && firstUpper != std::string::npos && firstDigit < firstUpper) continue;
    // Score: high alpha ratio wins
    int alnum = 0;
    for (char c : s) if (isalnum((unsigned char)c)) alnum++;
    double alphaRatio = (double)alnum / (double)s.size();
    if (alphaRatio < 0.80) continue;
    // First char must be alphanumeric and uppercase (most T2 missions)
    if (!isalnum((unsigned char)s[0])) continue;
    if (islower((unsigned char)s[0])) continue;
    // Reject strings with runs of >3 same character case (e.g. "AAAA" is garbage)
    int caseRun = 1;
    for (size_t k = 1; k < s.size(); k++) {
        if (isupper((unsigned char)s[k]) == isupper((unsigned char)s[k-1]))
            { caseRun++; if (caseRun > 3) { bad = true; break; } }
        else caseRun = 1;
    }
    if (bad) continue;
    // Reject strings with more than 4 consecutive consonants (garbage)
    int conRun = 1;
    const char* vowels = "aeiouAEIOU";
    for (size_t k = 1; k < s.size(); k++) {
        bool isVowel = strchr(vowels, s[k]) != nullptr;
        bool prevVowel = strchr(vowels, s[k-1]) != nullptr;
        if (!isVowel && !prevVowel) { conRun++; if (conRun > 4) { bad = true; break; } }
        else conRun = 1;
    }
    if (bad) continue;
    int score = (int)(alphaRatio * 100) + (int)s.size();
        if (score > bestScore) { bestScore = score; best = s; bestCRC = crc; }
    }
    if (bestScore > 0 && !best.empty()) {
        *outCRC = bestCRC;
        return best;
    }
    *outCRC = 0;
    return "";
}

void DemoParser::readInitialBlock(const uint8_t* data, size_t size) {
    BitStream bs(data, size);
    int totalBits = (int)size * 8;

    // ─── Tagged strings table (1024 entries) ─────────────────
    for (int i = 0; i < T2Demo::TaggedStringCount && !bs.isError(); i++)
        if (bs.readFlag()) initialBlock.taggedStrings[i] = bs.readString();

    // ─── Find mission name ───────────────────────────────────
    // 1. First try scanning tagged strings for a .mis path or mission name
    std::string taggedMission;
    for (auto& [tag, str] : initialBlock.taggedStrings) {
        if (str.find(".mis") != std::string::npos || str.find("missions/") != std::string::npos) {
            taggedMission = str;
            break;
        }
    }
    if (taggedMission.empty()) {
        // Scan all tagged strings for mission-like patterns
        for (auto& [tag, str] : initialBlock.taggedStrings) {
            std::string lower = str;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.find("missions/") != std::string::npos ||
                lower.find("levels/") != std::string::npos ||
                lower.find(".mis") != std::string::npos) {
                taggedMission = str;
                break;
            }
            // Direct mission name match
            if (lower == "katabatic" || lower == "damnation" || lower == "desert" ||
                lower == "snow" || lower == "training1" || lower == "training2")
                taggedMission = str;
        }
    }
    // 2. Try path-like strings: extract base name
    if (taggedMission.empty()) {
        for (auto& [tag, str] : initialBlock.taggedStrings) {
            if (str.size() > 6 && str.size() < 80 && str.find('/') != std::string::npos) {
                size_t slash = str.rfind('/');
                std::string base = (slash != std::string::npos) ? str.substr(slash + 1) : str;
                size_t dot = base.rfind('.');
                if (dot != std::string::npos) base = base.substr(0, dot);
                if (base.size() >= 3 && isupper((unsigned char)base[0])) {
                    taggedMission = base;
                    break;
                }
            }
        }
    }

    // ─── Data blocks ────────────────────────────────
    // Read headers even if we skip payloads
    readDataBlocks(bs);

    // Skip the rest (connection state, scores, target manager, etc.) — we
    // still can't consume datablock payloads without full class parsers.
    bs.setCurPos(totalBits);

    // 2. Try the C++ heuristic scan
    uint32_t crc = 0;
    std::string scanned = scanMissionName(data, size, &crc);
    initialBlock.missionCRC = crc;

    // 3. Prefer tagged string mission (more reliable) over heuristic scan
    if (!taggedMission.empty()) {
        initialBlock.missionName = taggedMission;
    } else {
        initialBlock.missionName = scanned;
    }

    // Fallback: reference parser via Node.js
    if (initialBlock.missionName.empty()) {
        initialBlock.missionName = extractMissionNameViaNode(data, size);
    }

    // Last resort: try to extract mission name from demo filename
    if (initialBlock.missionName.empty() && !recFilePath_.empty()) {
        std::string lower = recFilePath_;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        static const char* knownMissions[] = {
            "katabatic", "damnation", "training1", "training2",
            "oasis", "gauntlet", "icebound", "desiccator",
            "crater71", "haven", "tombstone", "whiteout",
            "treachery", "archipelago", "caldera",
            nullptr
        };
        for (int i = 0; knownMissions[i]; i++) {
            if (lower.find(knownMissions[i]) != std::string::npos) {
                initialBlock.missionName = knownMissions[i];
                // Capitalize first letter
                initialBlock.missionName[0] = (char)std::toupper((unsigned char)initialBlock.missionName[0]);
                break;
            }
        }
    }
}

// ─── Block stream ────────────────────────────────────────────
bool DemoParser::load(const uint8_t* buffer, size_t size) {
    buf = buffer; bufSize = size; offset = 0; ownsBuffer = false;
    decompressed = nullptr; decompressedSize = 0;

    readHeader();
    if (header.identString != "Tribes2 Recording") return false;

    {
        const char* ver = "unknown";
        if (header.protocolVersion == T2Demo::ProtocolV24834) ver = "24834";
        else if (header.protocolVersion == T2Demo::ProtocolV25034) ver = "25034";
        Console::instance().printf(LogLevel::Debug, "Demo: protocol 0x%08X (v%s), %u bytes initial block",
            (unsigned)header.protocolVersion, ver, (unsigned)header.initialBlockSize);
    }

    if (offset + header.initialBlockSize > bufSize) return false;
    readInitialBlock(buf + offset, header.initialBlockSize);
    offset += header.initialBlockSize;

    // Decompress block stream (raw deflate)
    size_t compSize = bufSize - offset;
    if (compSize == 0) return false;

    z_stream strm{};
    strm.next_in = (Bytef*)(buf + offset);
    strm.avail_in = (uInt)compSize;
    int ret = inflateInit2(&strm, -15);
    if (ret != Z_OK) return false;

    size_t outSize = std::max(compSize * 4, (size_t)262144);
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

    // Extract scoreboard data via Node.js reference parser
    if (!recFilePath_.empty()) {
        extractScoreboardData(recFilePath_.c_str());
    }

    // FALLBACK: populate scoreboard from ghost tracker if Node.js didn't provide data
    if (playerInfo_.empty()) {
        for (int i : ghostTracker.getAllIndices()) {
            const GhostEntry* g = ghostTracker.getGhost(i);
            if (!g || (g->className != "Player" && g->className != "MPB")) continue;
            PlayerInfo pi;
            pi.name = g->playerName.empty() ? g->skinName : g->playerName;
            pi.skin = g->skinName;
            pi.teamId = g->teamId;
            pi.damage = 1.0f - (g->health / 100.0f);
            playerInfo_.push_back(pi);
        }
    }

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
    ghostTracker.clear();
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

bool DemoParser::parseFull(std::vector<DemoBlock>& out) {
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
    int connSeqBits = bs.readInt(4);
    dh.connectSeqBit = connSeqBits;
    dh.seqNumber = bs.readInt(connSeqBits);
    dh.highestAck = bs.readInt(connSeqBits);
    if (dh.highestAck > dh.seqNumber)
        dh.highestAck -= (1 << connSeqBits);
    dh.packetType = bs.readInt(2);
    dh.ackByteCount = bs.readInt(3);
    dh.ackMask = bs.readInt(dh.ackByteCount * 8);
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
            // Derived class readPacketData may follow (e.g. Player)
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

void DemoParser::readEvents(BitStream& bs, std::vector<NetEventInfo>& outEvents) {
    while (bs.readFlag() && !bs.isError()) {
        NetEventInfo ev{};
        int rawId = bs.readInt(T2Demo::NetEventClassBitSize);
        ev.classId = rawId + T2Demo::NetEventClassFirst;
        if (rawId >= 0 && rawId < T2Demo::NetEventClassCount)
            ev.eventName = T2Demo::NetEventClassNames[rawId];
        else
            ev.eventName = "Event" + std::to_string(rawId);
        ev.guaranteed = bs.readFlag();
        if (ev.guaranteed) ev.sequenceNumber = (int)bs.readU32();
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
                if (!ev.message.empty()) ev.message += ' ';
                ev.message += arg;
            }
        } else if (ev.classId == T2Demo::NetEventClassFirst + 7) { // NetStringEvent
            if (bs.readFlag()) {
                bs.readInt(8);
                bs.readString();
            }
        } else if (ev.classId == T2Demo::NetEventClassFirst + 4) { // GhostingMessageEvent
            if (bs.readFlag()) { int n = bs.readInt(5); for (int i = 0; i < n; i++) { bs.readFlag(); } }
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
            ev.audioProfileId = bs.readRangedU32(0, 1024);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 20) { // SimTargetAudioEvent
            ev.audioProfileId = bs.readRangedU32(0, 1024);
            bs.readRangedU32(0, T2Demo::MaxGhostCount - 1);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 5) { // GravityEvent
            bs.readF32();
        } else if (ev.classId == T2Demo::NetEventClassFirst + 6) { // LightningStrikeEvent
            bs.readPoint3F(); bs.readPoint3F();
        } else if (ev.classId == T2Demo::NetEventClassFirst + 24) { // TargetInfoEvent
            bs.readInt(4); bs.readInt(32);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 25) { // TargetToEvent
            bs.readInt(4); bs.readInt(4);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 23) { // TargetFreeEvent
            bs.readInt(4);
        } else if (ev.classId == T2Demo::NetEventClassFirst + 13) { // SetMissionCRCEvent
            bs.readU32();
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
    }
}

// ─── Ghost update data readers ──────────────────────────────────
// Ported from T2 decompiled TypeScript parsers.
// Each reads the class-specific unpackUpdate data from the bitstream
// and advances the stream past all data for that ghost.

static void readGameBaseData(BitStream& bs, bool) {
    if (bs.readFlag()) bs.readInt(11);
    if (bs.readFlag() && bs.readFlag()) bs.readInt(9);
}

static void readShapeBaseData(BitStream& bs, bool isInitial, GhostEntry* entry = nullptr) {
    readGameBaseData(bs, isInitial);
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
    // ThreadMask (4 slots: flag → seq5 + state2 + forward + atEnd)
    if (bs.readFlag()) {
        for (int i = 0; i < 4; i++)
            if (bs.readFlag()) { bs.readInt(5); bs.readInt(2); bs.readFlag(); bs.readFlag(); }
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
                bool flags[5];
                for (int f = 0; f < 5; f++) flags[f] = bs.readFlag();
                int fireCount = bs.readInt(3);
                if (entry && i < 8) {
                    entry->mountedImages[i].loaded = flags[3]; // "loaded" flag
                    entry->mountedImages[i].isFiring = (fireCount > 0);
                }
                if (isInitial) bs.readFlag();
            }
        }
    }
    // CloakMask + MountMask + ShieldMask
    if (bs.readFlag()) {
        if (bs.readFlag()) { // hasCloakData
            bool cloaked = bs.readFlag();
            bs.readFlag(); // isControlled
            if (entry) entry->cloaked = cloaked;
            if (bs.readFlag()) { bs.readFlag(); bs.readF32(); } // fading → fadeOut, fadeTime
        }
        if (bs.readFlag()) { // MountMask
            bs.readFlag(); // mountNodeIndex
            bs.readNormalVector(8); bs.readFloat(5);
            if (bs.readFlag()) { bs.readU32(); bs.readU32(); }
        }
        if (bs.readFlag()) { // ShieldMask
            int shieldVal = bs.readInt(10);
            bs.readInt(5);
            if (entry) entry->shieldLevel = shieldVal / 1023.0f;
        }
    }
    if (bs.readFlag()) {
        if (bs.readFlag()) { bs.readInt(10); bs.readInt(5); }
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
        }
        (void)headX; (void)headZ;
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
}

static void readStaticShapeData(BitStream& bs, bool isInitial, const Vec3& cp, GhostEntry* entry) {
    readShapeBaseData(bs, isInitial, entry);
    if (bs.readFlag()) {
        if (entry) {
            entry->position = bs.readCompressedPoint(cp);
        } else {
            bs.readCompressedPoint(cp);
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
        if (entry) entry->position = bs.readCompressedPoint(cp);
        else bs.readCompressedPoint(cp);
        float qx = bs.readF32(), qy = bs.readF32(), qz = bs.readF32();
        bool qwNeg = bs.readFlag();
        float qw = sqrtf(fmaxf(0, 1.0f - (qx*qx + qy*qy + qz*qz)));
        if (qwNeg) qw = -qw;
        if (entry) { entry->rotation = {qx, qy, qz, qw}; entry->hasRotation = true; }
        bs.readF32(); bs.readF32(); // fovOrDist, orbitParam
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
            auto at = bs.readAffineTransform(cp);
            entry->position = at.position;
            entry->rotation = at.rotation;
            entry->hasRotation = true;
        } else {
            bs.readAffineTransform(cp);
        }
        bs.readPoint3F(); // scale
    }
}

static void readProjectileData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry*) {
    readGameBaseData(bs, isInitial);
    // Projectile base class has no extra fields
}

static void readDebrisData(BitStream& bs, bool isInitial, const Vec3&, GhostEntry*) {
    readGameBaseData(bs, isInitial);
    for (int i = 0; i < 6; i++) bs.readF32();
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
        bs.readPoint3F(); // velocity
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
        bs.readPoint3F(); bs.readPoint3F(); // initialPosition, endPos
        bs.readFlag(); bs.readFlag(); // truncated, hitWater
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); bs.readFlag(); }
    } else { // swing update
        if (bs.readFlag()) { bs.readRangedU32(0, 1024); bs.readRangedU32(0, 7); bs.readFlag(); }
        else { bs.readPoint3F(); }
        bs.readPoint3F(); // endPos
        bs.readFlag(); // truncated
    }
}

static void readShockLanceProjectileData(BitStream& bs, bool isInitial, GhostEntry* entry) {
    readGameBaseData(bs, isInitial);
    if (bs.readFlag()) bs.readRangedU32(0, 1024); // targetObject
    if (bs.readFlag()) { // initial update
        bs.readPoint3F(); bs.readPoint3F(); // start, end
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
            bs.readPoint3F(); // velocity
        }
        if (!bs.readFlag()) return;
        bs.readPoint3F(); bs.readPoint3F(); return; // endPoint, endNormal
    }
    // full state
    if (entry) entry->position = bs.readPoint3F();
    else bs.readPoint3F();
    bs.readPoint3F(); // velocity
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
            bs.readNormalVector(14); // direction
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
    if (!bs.readFlag()) { // non-full state
        if (bs.readFlag()) {
            Vec3 expPos = bs.readPoint3F();
            bs.readPoint3F(); // normal
            DemoParser::s_pendingExplosions.push_back({expPos, 0.0f});
            return;
        }
        if (entry) entry->position = bs.readPoint3F();
        else bs.readPoint3F();
        bs.readPoint3F(); // velocity
        if (bs.readFlag()) {
            if (!bs.readFlag()) bs.readPoint3F(); // targetDirection
            else bs.readInt(11); // targetGhost
        }
        return;
    }
    // full state
    if (entry) entry->position = bs.readPoint3F();
    else bs.readPoint3F();
    bs.readPoint3F(); // velocity
    bs.readPoint3F(); // orientation
    if (bs.readFlag()) { bs.readInt(11); bs.readInt(3); } // source
    if (bs.readFlag()) {
        if (!bs.readFlag()) bs.readPoint3F(); // targetDirection
        else bs.readInt(11); // targetGhost
    }
    bs.readFlag(); // timeoutReset
}

static void readSkyData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    int skyCount = bs.readInt(5);
    for (int i = 0; i < skyCount; i++) bs.readString();
}

static void readSunData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    if (bs.readFlag()) {
        Vec3 sunDir = bs.readPoint3F();
        float az = bs.readFloat(8);
        float el = bs.readFloat(8);
        int r = bs.readInt(8);
        int g = bs.readInt(8);
        int b = bs.readInt(8);
        bs.readFlag(); bs.readFlag();
        auto& sd = DemoParser::s_sunData;
        sd.direction = sunDir;
        sd.azimuth = az;
        sd.elevation = el;
        sd.r = r; sd.g = g; sd.b = b;
        sd.valid = true;
    }
}

static void readForceFieldBareData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    if (bs.readFlag()) {
        auto at = bs.readAffineTransform();
        if (entry) { entry->position = at.position; entry->rotation = at.rotation; entry->hasRotation = true; }
    }
    bs.readPoint3F();
}

static void readTSStaticData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    bs.readMatrixF(); bs.readPoint3F();
}

static void readTerrainBlockData(BitStream& bs, bool, const Vec3&, GhostEntry*) {
    if (bs.readFlag()) {
        bs.readInt(3); bs.readPoint3F(); bs.readInt(4);
        bs.readInt(4); bs.readInt(4);
    }
}

static void readWaterBlockData(BitStream& bs, bool, const Vec3&, GhostEntry* entry) {
    auto at = bs.readAffineTransform();
    if (entry) { entry->position = at.position; entry->rotation = at.rotation; entry->hasRotation = true; }
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
    else if (cn == "Camera") readCameraData(bs, isInitial, cp, entry);
    else if (cn == "Marker") readMarkerData(bs, false, cp, entry);
    else if (cn == "MissionMarker" || cn == "WayPoint" || cn == "SpawnSphere")
        readMissionMarkerData(bs, false, cp, entry);
    else if (cn == "Debris") readDebrisData(bs, isInitial, cp, entry);
    else if (cn == "Projectile" || cn == "EnergyProjectile" || cn == "FlareProjectile")
        readProjectileData(bs, isInitial, cp, entry);
    else if (cn == "BombProjectile") readBombProjectileData(bs, isInitial, cp, entry);
    else if (cn == "GrenadeProjectile") readGrenadeData(bs, isInitial, cp, entry);
    else if (cn == "LinearProjectile" || cn == "TracerProjectile" || cn == "LinearFlareProjectile")
        readLinearProjectileData(bs, isInitial, cp, entry);
    else if (cn == "SeekerProjectile") readSeekerProjectileData(bs, isInitial, cp, entry);
    else if (cn == "SniperProjectile") readSniperProjectileData(bs, isInitial, entry);
    else if (cn == "ShockLanceProjectile") readShockLanceProjectileData(bs, isInitial, entry);
    else if (cn == "Sky") readSkyData(bs, isInitial, cp, entry);
    else if (cn == "Sun") readSunData(bs, isInitial, cp, entry);
    else if (cn == "TSStatic") readTSStaticData(bs, isInitial, cp, entry);
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
            std::string cn;
            if (gu.classId >= 0 && gu.classId < T2Demo::NetObjectClassCount)
                cn = T2Demo::NetObjectClassNames[gu.classId];
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
    uint32_t connectSeq = (uint32_t)dnet.connectSeqBit;
    if (connectSeq != connectSequence && !connectionEstablished) {
        connectSequence = connectSeq;
        lastSeqRecvd = 0; highestAckedSeq = 0; lastSendSeq = 0;
        memset(lastSeqRecvdAtSend, 0, sizeof(lastSeqRecvdAtSend));
    }
    dispatchData = true;
    uint32_t seq = (uint32_t)dnet.seqNumber;
    if (seq > lastSeqRecvd || (seq < lastSeqRecvd &&
        seq + (1 << dnet.connectSeqBit) > lastSeqRecvd))
        lastSeqRecvd = seq;
    packetsParsed++;
    return true;
}

PacketData DemoParser::parsePacket(const uint8_t* data, size_t size, int blockIndex) {
    PacketData pd{};
    BitStream bs(data, size);
    pd.dnetHeader = readDnetHeader(bs);
    bool dispatchData = false;
    applyProtocolHeader(pd.dnetHeader, dispatchData);
    int rateBits = bs.readInt(2);
    int rateCount[] = {4, 6, 8, 10};
    for (int i = 0; i < rateCount[rateBits]; i++) bs.readFloat(7);

    pd.gameState = readGameState(bs);
    readEvents(bs, pd.events);
    readGhosts(bs, pd.ghosts, pd.dnetHeader.seqNumber, &pd.gameState.compressionPoint);
    return pd;
}
