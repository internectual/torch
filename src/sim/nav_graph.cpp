#include "sim/nav_graph.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace Nav {

// ---------------------------------------------------------------------------
// math/mRandom.cc

static uint32_t msSeed = 1376312589;

uint32_t RandomLCG::generateSeed() {
    // "A very, VERY crude LCG but good enough to generate a nice range of seed values"
    msSeed = (msSeed * 0x015a4e35u) + 1;
    msSeed = (msSeed >> 16) & 0x7fff;
    return msSeed;
}

RandomLCG::RandomLCG() : mSeed((int32_t)generateSeed()) {}

static constexpr int32_t S32Max = 0x7fffffff;
static constexpr int32_t msQuotient = S32Max / 16807;
static constexpr int32_t msRemainder = S32Max % 16807;

uint32_t RandomLCG::randI() {
    if (mSeed <= msQuotient)
        mSeed = (mSeed * 16807) % S32Max;
    else {
        const int32_t highPart = mSeed / msQuotient;
        const int32_t lowPart = mSeed % msQuotient;
        const int32_t test = (16807 * lowPart) - (msRemainder * highPart);
        mSeed = test > 0 ? test : test + S32Max;
    }
    return (uint32_t)mSeed;
}

// i + randI() % (n - i + 1); the span is taken in 64 bits so a full-range
// request does not divide by the wrapped zero.
int32_t RandomLCG::randI(int32_t i, int32_t n) {
    const int64_t span = (int64_t)n - (int64_t)i + 1;
    if (span <= 0) return i;
    return (int32_t)((int64_t)i + (int64_t)(randI() % (uint64_t)span));
}

float RandomLCG::randF() {
    return float(randI()) * float(1.0 / 2147483647.0);
}

float RandomLCG::randF(float i, float n) {
    return i + (n - i) * randF();
}

RandomLCG& gRandGen() {
    static RandomLCG generator;
    return generator;
}

void setGlobalRandSeed(uint32_t seed) {
    gRandGen().setSeed((int32_t)seed);
}

// ---------------------------------------------------------------------------
// Hooks

static RayCastFn& rayCaster() {
    static RayCastFn cast;
    return cast;
}

void setRayCaster(RayCastFn cast) { rayCaster() = std::move(cast); }

bool castRay(const Point3& start, const Point3& end, uint32_t mask, RayHit& hit) {
    return rayCaster() && rayCaster()(start, end, mask, hit);
}

static FileReadFn& fileReader() {
    static FileReadFn read;
    return read;
}

void setFileReader(FileReadFn read) { fileReader() = std::move(read); }

static std::vector<uint8_t> readFile(const std::string& path) {
    if (fileReader()) return fileReader()(path);
    return Engine::instance().fs().read(path.c_str());
}

} // namespace Nav

using Nav::Point3;

namespace {

// ---------------------------------------------------------------------------
// core/stream.h reads over a whole file: little-endian, fails past the end.

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : data(bytes) {}
    bool bytes(void* out, size_t n) {
        if (n > data.size() - pos) { pos = data.size(); return false; }
        if (n) std::memcpy(out, data.data() + pos, n);
        pos += n;
        return true;
    }
    bool skip(size_t n) {
        if (n > data.size() - pos) { pos = data.size(); return false; }
        pos += n;
        return true;
    }
    template <class T> bool read(T* out) { return bytes(out, sizeof(T)); }
    // Stream::read(bool*): one byte.
    bool read(bool* out) {
        uint8_t value;
        if (!read(&value)) return false;
        *out = value != 0;
        return true;
    }
    bool point(Point3* p) { return read(&p->x) && read(&p->y) && read(&p->z); }
    // A vector count: no more elements than bytes left (guards garbage counts).
    bool count(int32_t* n, size_t elementSize) {
        if (!read(n)) return false;
        return *n >= 0 && (size_t)*n <= (data.size() - pos) / (elementSize ? elementSize : 1);
    }
    size_t remaining() const { return data.size() - pos; }

private:
    const std::vector<uint8_t>& data;
    size_t pos = 0;
};

// readVector1 / readVector2 of fixed-size elements this port does not keep.
bool skipVector(Reader& s, size_t elementSize) {
    int32_t n;
    return s.count(&n, elementSize) && s.skip((size_t)n * elementSize);
}

// GraphEdgeInfo: 2 x {U32 flags, S32 res, S32 dest}, 3 x Point3F.
constexpr size_t EdgeInfoSize = 2 * 12 + 3 * 12;
// GraphVolInfo: S32 planeCount, S32 planeIndex.  PlaneF: 4 x F32.
constexpr size_t VolInfoSize = 8, PlaneSize = 16;
// GraphBridgeData: U16 nodes[2], U8 jetClear, U8 howTo.
constexpr size_t BridgeDataSize = 6;
// LOSHashTable::Segment: U16 node, U16 seg, U8 mLOS[SegAlloc = 16].
constexpr size_t LOSSegmentSize = 4 + 16;
// TerrainGraphInfo::smVersion
constexpr int32_t TerrainInfoVersion = 0;

// BitVectorW::setDims + numBytes: the data bytes of a (numEntries, bitWidth) table.
size_t bitVectorBytes(uint32_t numEntries, uint32_t bitWidth) {
    if (numEntries > 0 && bitWidth <= 17)
        return (size_t)((((uint64_t)bitWidth * numEntries) + 32) >> 3);
    return 0;
}

bool readBitVector(Reader& s) {
    uint32_t numEntries, bitWidth;
    return s.read(&numEntries) && s.read(&bitWidth) && s.skip(bitVectorBytes(numEntries, bitWidth));
}

// SpawnLocations::read
bool readSpawnList(Reader& s, NavGraph& g) {
    int32_t n;
    if (!s.read(&g.spawnRes0) || !s.count(&n, 12)) return false;
    g.spawnList.assign((size_t)n, Point3{});
    for (auto& point : g.spawnList)
        if (!s.point(&point)) return false;
    if (!s.count(&n, 29)) return false;
    g.spheres.assign((size_t)n, NavGraph::SpawnSphere{});
    for (auto& sphere : g.spheres)
        if (!(s.read(&sphere.res0) && s.point(&sphere.center) && s.read(&sphere.radius) &&
              s.read(&sphere.inside) && s.read(&sphere.count) && s.read(&sphere.offset)))
            return false;
    return true;
}

// TerrainGraphInfo::read
bool readTerrainInfo(Reader& s, NavGraph& g) {
    g.haveTerrainGraph = false;
    int32_t version, nodeCount;
    if (!s.read(&version) || version != TerrainInfoVersion) return false;
    Point3 originWorld;
    int32_t grid[6];   // originGrid, gridDimensions, gridTopRight (Point2I each)
    bool ok = s.read(&nodeCount) && s.point(&originWorld);
    for (int i = 0; ok && i < 6; ++i) ok = s.read(&grid[i]);
    ok = ok && skipVector(s, 1)      // navigableFlags  Vector<U8>
            && skipVector(s, 1)      // neighborFlags   Vector<U8>
            && skipVector(s, 4)      // shadowHeights   Vector<F32>
            && skipVector(s, 2);     // roamRadii       Vector<U16>
    int32_t n;
    ok = ok && s.count(&n, 8);
    if (ok) {
        g.consolidated.assign((size_t)n, NavGraph::OutdoorNodeInfo{});
        for (auto& node : g.consolidated)
            if (!(ok = s.read(&node.level) && s.read(&node.flags) && s.read(&node.height) &&
                       s.read(&node.x) && s.read(&node.y)))
                break;
    }
    return g.haveTerrainGraph = ok;
}

// PathXRefTable::read (constructVector of PathXRefEntry).
bool readPathXRef(Reader& s) {
    int32_t n;
    if (!s.count(&n, 8)) return false;
    for (int32_t i = 0; i < n; ++i)
        if (!readBitVector(s)) return false;
    return true;
}

// LOSHashTable::read
bool readLOSHash(Reader& s) {
    int32_t numNodes, res0, res1;
    return s.read(&numNodes) && skipVector(s, LOSSegmentSize) && s.read(&res0) && s.read(&res1);
}

float dAtof(const std::string& text) { return (float)std::atof(text.c_str()); }
int32_t dAtoi(const std::string& text) { return (int32_t)std::atoi(text.c_str()); }
bool dAtob(const std::string& text) {
    return strcasecmp(text.c_str(), "true") == 0 || dAtof(text) != 0.0f;
}

void conPrintf(const char* fmt, const char* a = nullptr, int b = 0) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), fmt, a ? a : "", b);
    Console::instance().printf(LogLevel::Info, "%s", buffer);
}

NavGraph* gNavGraph = nullptr;

// graph.cc getGraphName
std::string graphName(const std::string& mission, bool spawn) {
    return std::string("terrains/") + mission + (spawn ? ".spn" : ".nav");
}

} // namespace

// ---------------------------------------------------------------------------
// NavigationGraph

NavGraph::NavGraph() { gNavGraph = this; }

NavGraph::~NavGraph() {
    if (gNavGraph == this) gNavGraph = nullptr;
}

NavGraph* NavGraph::current() { return gNavGraph; }

void NavGraph::resetSpawnList() {
    spawnList.clear();
    spheres.clear();
    spawnRes0 = 0;
}

bool NavGraph::load(const std::vector<uint8_t>& bytes, bool isSpawn) {
    Reader s(bytes);
    // Version and four reserved S32s.
    int32_t res;
    if (!s.read(&version)) return false;
    if (!(s.read(&res) && s.read(&res) && s.read(&res) && s.read(&res))) return false;

    // The reduced spawn list. Before load it signals the graph is only a spawn
    // list; afterwards a non-empty list is the condition.
    if (version >= BetterSpawnMode) {
        const bool ok = readSpawnList(s, *this);
        if (isSpawn) return ok;
        resetSpawnList();
        if (!ok) return false;
    }

    // Indoor edges, nodes and volumes (volumes are one-to-one with nodes).
    int32_t n;
    if (!s.count(&n, EdgeInfoSize)) return false;
    edgeInfoCount = n;
    if (!s.skip((size_t)n * EdgeInfoSize)) return false;
    if (!s.count(&n, 20)) return false;   // U32 flags, U16, S16, Point3F
    indoorNodeInfo.assign((size_t)n, IndoorNodeInfo{});
    for (auto& node : indoorNodeInfo)
        if (!(s.read(&node.flags) && s.read(&node.unused) && s.read(&node.antecedent) && s.point(&node.pos)))
            return false;
    if (!(skipVector(s, VolInfoSize) && skipVector(s, PlaneSize))) return false;

    // Outside
    if (!readTerrainInfo(s, *this)) return false;

    // Bridges (AssertISV in the engine for older graphs: "Graph needs rebuilt for this mission")
    if (version < TrimmedBridges) return false;
    if (!s.count(&n, BridgeDataSize)) return false;
    bridgeCount = n;
    if (!s.skip((size_t)n * BridgeDataSize)) return false;

    // Tables
    if (version >= AddedPathTable && !readPathXRef(s)) return false;
    if (version >= AddedLOSTable) {
        if (version >= RevisedLOSToHash) {
            if (!readLOSHash(s)) return false;
        } else if (!readBitVector(s)) {
            return false;
        }
    }
    return true;
}

void NavGraph::makeGraph() {
    // makeRunTimeNodes: one outdoor node per consolidated entry (given a
    // terrain block), one indoor node per IndoorNodeInfo.
    numOutdoor = (int32_t)consolidated.size();
    numIndoor = (int32_t)indoorNodeInfo.size();
    Console::instance().printf(LogLevel::Info, "MakeGraph: %d indoor, %d outdoor", numIndoor, numOutdoor);
}

bool NavGraph::loadNavGraph(const std::vector<uint8_t>& bytes) {
    if (!load(bytes, false)) return false;
    makeGraph();
    return true;
}

bool NavGraph::loadGraph(const std::string& missionName) {
    bool ok = false;
    const std::string fileName = graphName(missionName, isSpawnGraph);
    const std::vector<uint8_t> bytes = Nav::readFile(fileName);
    if (!bytes.empty()) {
        if (!load(bytes, isSpawnGraph))
            conPrintf("loadGraph: Failed to load '%s'", fileName.c_str());
        else
            ok = true;

        if (ok && !isSpawnGraph) {
            // NAV graph needs the spawn data.
            const std::string spawnName = graphName(missionName, true);
            const std::vector<uint8_t> spawnBytes = Nav::readFile(spawnName);
            if (!spawnBytes.empty()) {
                if (!load(spawnBytes, true))
                    conPrintf("Error loading spawn file into NAV graph");
            } else {
                conPrintf("loadGraph: NAV Ok, but SPN open failed (%s)", spawnName.c_str());
            }
        }
    } else {
        conPrintf("loadGraph: Couldn't open '%s' for read", fileName.c_str());
    }
    return ok;
}

bool NavGraph::loadForMission(const std::string& missionName, bool spawnGraph) {
    isSpawnGraph = spawnGraph;
    if (!loadGraph(missionName)) return false;
    if (!isSpawnGraph) makeGraph();
    return true;
}

void NavGraph::onAdd() {
    auto* ts = ScriptEngine::exists() ? ScriptEngine::instance().ts() : nullptr;
    auto global = [ts](const char* name) -> std::string {
        return ts ? ts->getGlobal(name).toString() : std::string();
    };
    const std::string mission = global("$CurrentMission");

    bool forceNavLoad = dAtob(global("$GraphForceLoad"));
    // Need to insure presence of NAV due to how code works below...
    if (forceNavLoad && Nav::readFile(graphName(mission, false)).empty())
        forceNavLoad = false;
    if (dAtob(global("$OFFLINE_NAV_BUILD")))
        forceNavLoad = true;

    // Outside single player, no bots means just the spawn graph; single
    // player needs the NAV.
    bool spawnGraph = false;
    if (!forceNavLoad && strcasecmp(global("$CurrentMissionType").c_str(), "SinglePlayer") != 0)
        spawnGraph = dAtoi(global("$HostGameBotCount")) == 0;
    loadForMission(mission, spawnGraph);
}

// ---------------------------------------------------------------------------
// graphSpawn.cc queries

int32_t NavGraph::randNode(const Point3& P, float R, bool indoor, bool outdoor) {
    // A valid spawn list (a SPN file, or the SPN loaded into a NAV graph) is
    // what is used; the radius and outdoor flag play no part there.
    if (!spawnList.empty()) {
        const uint32_t rnd = Nav::gRandGen().randI();
        // SpawnLocations::getRandom: the closest sphere of the wanted kind.
        const SpawnSphere* closest = nullptr;
        float minDist = 1e22f;
        for (const SpawnSphere& S : spheres) {
            if (S.inside != indoor || S.count <= 0) continue;
            const float dx = S.center.x - P.x, dy = S.center.y - P.y, dz = S.center.z - P.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < minDist) {
                closest = &S;
                minDist = d;
            }
        }
        if (!closest) return -1;
        const uint32_t count = (uint32_t)closest->count;
        return closest->offset + (int32_t)((rnd & 0x7FFFFF) % count);
    }

    // Node-based selection over the run-time graph (terrain grid nodes and the
    // indoor BSP, then a random pick among those in the sphere) needs
    // makeGraph, which this port does not build.
    (void)R; (void)outdoor;
    if (gotOneWeCanUse()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Console::instance().printf(LogLevel::Warn,
                "NavigationGraph::randNode: node graph queries without spawn data are not implemented");
        }
    }
    return -1;
}

const Point3* NavGraph::getSpawnLoc(int32_t index) const {
    // Without spawn data the engine returns node locations, which this port
    // does not have.
    if (!spawnList.empty() && index >= 0 && index < (int32_t)spawnList.size())
        return &spawnList[(size_t)index];
    return nullptr;
}

const Point3* NavGraph::getRandSpawnLoc(int32_t index) const {
    // Without spawn data the engine returns node->randomLoc(); not ported.
    return getSpawnLoc(index);
}

void NavGraph::adjustSpawnLoc(Point3& point) {
    // Go up slightly and cast down to ground (Loser(-1).hitBelow(point, 20)).
    point.z = (float)(point.z + 1.3);
    const Point3 below{point.x, point.y, point.z - 20.0f};
    Nav::RayHit hit;
    if (Nav::castRay(point, below, 0xFFFFFFFFu, hit)) {
        point = hit.point;
        const float sideways = std::sqrt(hit.normal.x * hit.normal.x + hit.normal.y * hit.normal.y);
        point.z = (float)(point.z + sideways / 2.0);
        point.z = (float)(point.z + 0.2);   // some shapes have problems... cf. ThinIce
    }
}

bool NavGraph::randNodeLoc(int32_t index, Point3& out) const {
    const Point3* pt = getRandSpawnLoc(index);
    if (!pt) return false;
    out = *pt;
    adjustSpawnLoc(out);
    return true;
}

namespace {
constexpr int CastShift = 6;
constexpr int NumCasts = 1 << CastShift;
constexpr int Periphery = NumCasts / 4 + 1;
constexpr double NavPi = 3.14159265358979323846;
constexpr double AngleInc = (NavPi * 2.0) / double(float(NumCasts));
constexpr float CapDistance = 60.0f;

// A circle of NumCasts LOS casts from 1m up: squared distances and angles.
float lookAround(Point3 P, float dists[NumCasts], float qAngles[NumCasts]) {
    float minDist = CapDistance * CapDistance;
    P.z += 1.0f;
    for (int i = 0; i < NumCasts; ++i) {
        float ang = (float)(float(i) * AngleInc);
        qAngles[i] = ang;
        ang = (float)(ang + NavPi / 2.0);
        Point3 D{std::cos(ang) * CapDistance, std::sin(ang) * CapDistance, 0.0f};
        D.x += P.x; D.y += P.y; D.z += P.z;
        Nav::RayHit coll;
        if (Nav::castRay(P, D, 0xFFFFFFFFu, coll)) {
            const float dx = coll.point.x - P.x, dy = coll.point.y - P.y, dz = coll.point.z - P.z;
            dists[i] = dx * dx + dy * dy + dz * dz;
            minDist = std::min(dists[i], minDist);
        } else {
            dists[i] = CapDistance * CapDistance;
        }
    }
    return minDist;
}
} // namespace

float NavGraph::whereToLook(Point3 P) {
    float dists[NumCasts];
    float qAngles[NumCasts];
    lookAround(P, dists, qAngles);

    // Maximize (forward dist squared) - (smaller side distance squared); the
    // side angle is a little over 90 degrees to angle away from walls.
    int bestIndex = 0;
    float bestMetric = -1e13f, sideDist, metric;
    const uint32_t wrap = NumCasts - 1;
    for (uint32_t j = 0; j < (uint32_t)NumCasts; ++j) {
        sideDist = std::min(dists[(j - Periphery) & wrap], dists[(j + Periphery) & wrap]);
        if ((metric = dists[j] - sideDist) > bestMetric) {
            bestMetric = metric;
            bestIndex = (int)j;
        }
    }
    return qAngles[bestIndex];
}

void NavGraph::printSpawnInfo() const {
    Console::instance().printf(LogLevel::Info, "--- %d Spheres were generated", (int)spheres.size());
    for (size_t i = 0; i < spheres.size(); ++i) {
        const SpawnSphere& S = spheres[i];
        Console::instance().printf(LogLevel::Info, "%d : %s at center (%f, %f, %f) - generated %d",
            (int)i + 1, S.inside ? "Inside" : "Outside", S.center.x, S.center.y, S.center.z, S.count);
    }
}

// ---------------------------------------------------------------------------
// Console functions (graph.cc consoleInit)

void registerNavGraphNatives(TorqueScript& ts) {
    EngineObjects::registerClass("NavigationGraph", [] {
        auto graph = std::make_shared<NavGraph>();
        graph->onAdd();
        return graph;
    });
    using Args = std::vector<VMValue>;
    auto graphOf = [](const Args& args) -> NavGraph* {
        return args.empty() ? nullptr : EngineObjects::get<NavGraph>(args[0].toString());
    };
    // Con::addCommand argument limits: argc counts the name (and the object
    // for methods), so argc = args.size() + 1 either way.
    auto argcOk = [](const Args& args, int minArgs, int maxArgs, const char* usage) {
        const int argc = (int)args.size() + 1;
        if (argc >= minArgs && argc <= maxArgs) return true;
        Console::instance().printf(LogLevel::Warn, "usage: %s", usage);
        return false;
    };
    auto vec3 = [](const std::string& text) {
        Point3 p;
        std::sscanf(text.c_str(), "%f %f %f", &p.x, &p.y, &p.z);
        return p;
    };
    auto format3 = [](const Point3& p) {
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f", p.x, p.y, p.z);
        return std::string(buffer);
    };
    static const char bogusLocation[] = "0 0 500";

    ts.registerNative("NavigationGraph::randNode", [graphOf, argcOk, vec3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 6, "navGraph.randNode(pt, rad, indoor, outdoor);")) return VMValue("");
        NavGraph* graph = graphOf(args);
        if (!graph) return VMValue(-1);
        const int argc = (int)args.size() + 1;
        if (argc >= 4) {
            const Point3 pt = vec3(args[1].toString());
            const float radius = (float)dAtoi(args[2].toString());
            const bool inside = argc > 4 ? dAtob(args[3].toString()) : false;
            const bool outside = argc > 5 ? dAtob(args[4].toString()) : false;
            return VMValue(graph->randNode(pt, radius, inside, outside));
        }
        Console::instance().printf(LogLevel::Info, "Given a point, radius, and flags for indoor or outdoor");
        Console::instance().printf(LogLevel::Info, "inclusion, %s returns a node index (-1 if not found).", "randNode");
        Console::instance().printf(LogLevel::Info, "Note use navGraph.nodeLoc() to get location from index");
        return VMValue(-1);
    });

    ts.registerNative("NavigationGraph::nodeLoc", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.nodeLoc(nodeIndex);")) return VMValue("");
        NavGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t index = dAtoi(args[1].toString());
            if (const Point3* pt = graph->getSpawnLoc(index)) return VMValue(format3(*pt));
            Console::instance().printf(LogLevel::Info, "Invalid index (%d) passed to nodeLoc()", index);
        } else {
            Console::instance().printf(LogLevel::Info, "Gets the location of the specified node or spawn index");
        }
        return VMValue(bogusLocation);
    });

    ts.registerNative("NavigationGraph::randNodeLoc", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.randNodeLoc(nodeIndex);")) return VMValue("");
        NavGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t index = dAtoi(args[1].toString());
            Point3 point;
            if (graph->randNodeLoc(index, point)) return VMValue(format3(point));
            Console::instance().printf(LogLevel::Info, "Invalid index (%d) passed to %s()", index, "randNodeLoc");
        } else {
            Console::instance().printf(LogLevel::Info, "Finds a random location within the space of the given node");
        }
        return VMValue(bogusLocation);
    });

    ts.registerNative("NavigationGraph::numNodes", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.numNodes();")) return VMValue("");
        NavGraph* graph = graphOf(args);
        return VMValue(graph ? graph->numNodes() : 0);
    });

    ts.registerNative("NavigationGraph::numSpawns", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.numSpawns()")) return VMValue("");
        NavGraph* graph = graphOf(args);
        return VMValue(graph ? graph->numSpawns() : 0);
    });

    ts.registerNative("NavigationGraph::getSpawn", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.getSpawn(which)")) return VMValue("");
        NavGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t which = dAtoi(args[1].toString());
            if (which >= 0 && which < graph->numSpawns()) {
                Point3 point = graph->spawnList[(size_t)which];
                NavGraph::adjustSpawnLoc(point);
                return VMValue(format3(point));
            }
            Console::instance().printf(LogLevel::Info, "Spawn index %d out of range!", which);
        }
        return VMValue("");
    });

    ts.registerNative("NavigationGraph::spawnInfo", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.spawnInfo();")) return VMValue("");
        if (NavGraph* graph = graphOf(args)) graph->printSpawnInfo();
        return VMValue("");
    });

    ts.registerNative("NavigationGraph::loadGraph", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.loadGraph();")) return VMValue("");
        NavGraph* graph = graphOf(args);
        if (!graph) return VMValue(0);
        auto* script = ScriptEngine::instance().ts();
        const std::string mission = script ? script->getGlobal("$CurrentMission").toString() : std::string();
        return VMValue(graph->loadGraph(mission) ? 1 : 0);
    });

    ts.registerNative("WhereToLook", [argcOk, vec3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "WhereToLook(playerLoc);")) return VMValue("");
        const Point3 point = vec3(args[0].toString());
        // We get a Z rotation back; the string is for setTransform().
        const float angle = -NavGraph::whereToLook(point);
        char buffer[120];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f 0 0 1 %f", point.x, point.y, point.z, angle);
        return VMValue(buffer);
    });

    ts.registerNative("navGraphExists", [argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 1, 1, "navGraphExists();")) return VMValue("");
        const NavGraph* graph = NavGraph::current();
        return VMValue(graph && (graph->gotOneWeCanUse() || graph->hasSpawnLocs()) ? 1 : 0);
    });
}
