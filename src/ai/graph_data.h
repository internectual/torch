#pragma once
// The persisted data of the Tribes 2 AI navigation graph (engine
// ai/graphData.h / graphData.cc, graphDefines.h): what a terrains/*.nav or
// *.spn stream holds, read little-endian as core/stream.h does.
//
// Ported for the run time: indoor edges / nodes / volumes, the terrain grid
// info with its consolidated outdoor nodes, bridges, the spawn list and the
// LOS hash table. The path cross-reference table (PathXRefTable) is read
// past: the engine dropped it (mValidPathTable is always false). Not ported:
// writing (graph generation) and the old LOSXRefTable format (only graphs
// before RevisedLOSToHash carry it; the shipped graphs are all version 8).
#include "ai/graph_math.h"
#include <cstdint>
#include <vector>

// graphDefines.h
enum GridOffsets {
    GridBottomLeft,
    GridBottom, GridLeft,
    GridBottomRight, GridTopLeft,
    GridRight, GridTop,
    GridTopRight,
    NumGridOffsets
};

enum NavGraphDefinitions {
    GraphNodeClear = 0,              // Navigable flags on the terrain grid information.
    GraphNodeShadowed,
    GraphNodeObstructed,
    GraphNodeSubmerged,
    GroundNodeSteep = (1 << 3),      // Next to unwalkable slope.

    GraphThreatLimit = 64,
    GraphMaxTeams = 32,
    AbsMaxBotCount = 64,
};

// 2.5 minutes
constexpr uint32_t GraphMaxNodeAvoidMS = 150000;
// Pull-in used to avoid rounding errors in indoor node volume obstruction checks.
constexpr float NodeVolumeShrink = 0.014f;
// Jet connections the bridge builder assures, and the smaller threshold the
// jet code uses for not attempting.
constexpr float GraphJetBridgeXY = 1.7f;
constexpr float GraphJetFailXY = 1.3f;
constexpr float MaxGraphNodeVolRad = 25.0f;

enum NavGraphEnum {
    NavGraphNotANode = 0,
    NavGraphOutdoorNode,
    NavGraphIndoorNode,
    NavGraphSubmergedNode,
    MaxOnDemandEdges = 13,     // for nodes that create edges on demand (ie. grid nodes)
};

// Repository for all graph dimension numbers.
struct NavGraphGlobals {
    // Terrain:
    float mSquareWidth;
    float mInverseWidth;
    float mSquareRadius;
    int32_t mSquareShift;
    Point3F mHalfSquare;

    void setTerrNumbers(int32_t shift) {
        mSquareShift = shift;
        mSquareWidth = float(1 << shift);
        mInverseWidth = 1.0f / mSquareWidth;
        mSquareRadius = float(1 << (shift - 1));
        mHalfSquare = {mSquareRadius - 0.01f, mSquareRadius - 0.01f, 0.0f};
    }

    // Walk / jet / jump configuration numbers
    float mWalkableDot;
    float mWalkableAngle;
    float mJumpAdd;
    float mTallestPlayer;
    float mStepHeight;
    float mPrettySteepAngle;
    float mPrettySteepDot;

    void setWalkData(float angle, float stepHeight) {
        mWalkableAngle = angle * 3.14159265358979323846f / 180.0f;
        mWalkableDot = std::cos(mWalkableAngle);
        mJumpAdd = 2.0f;
        mTallestPlayer = 2.3f;
        mStepHeight = stepHeight;
        // Angle slightly less than max used to decide to make a transient
        // connection jettable between two points outdoors (graphLocate).
        mPrettySteepAngle = mWalkableAngle * (6.0f / 7.0f);
        mPrettySteepDot = std::cos(mPrettySteepAngle);
    }

    NavGraphGlobals() {
        setTerrNumbers(3);            // Default shift value
        setWalkData(70, 0.75f);       // Walk angle (min of all maxes), step height
    }
};
extern NavGraphGlobals gNavGlobs;

// core/stream.h reads over a whole file: little-endian, failing past the end.
class GraphStream {
public:
    explicit GraphStream(const std::vector<uint8_t>& bytes) : data(bytes) {}
    bool bytes(void* out, size_t n) {
        if (n > data.size() - pos) { pos = data.size(); return false; }
        if (n) std::memcpy(out, data.data() + pos, n);
        pos += n;
        return true;
    }
    template <class T> bool read(T* out) { return bytes(out, sizeof(T)); }
    bool read(bool* out) {
        uint8_t value;
        if (!read(&value)) return false;
        *out = value != 0;
        return true;
    }
    bool read(Point3F* p) { return read(&p->x) && read(&p->y) && read(&p->z); }
    bool read(Point2I* p) { return read(&p->x) && read(&p->y); }
    bool read(PlaneF* p) { return read(&p->x) && read(&p->y) && read(&p->z) && read(&p->d); }
    // A vector count, bounded by the bytes left (guards garbage counts).
    bool count(int32_t* n, size_t minElementSize) {
        if (!read(n)) return false;
        return *n >= 0 && (size_t)*n <= (data.size() - pos) / (minElementSize ? minElementSize : 1);
    }
    bool skip(size_t n) {
        if (n > data.size() - pos) { pos = data.size(); return false; }
        pos += n;
        return true;
    }

private:
    const std::vector<uint8_t>& data;
    size_t pos = 0;
};

// readVector1: count, then T::read on each element.
template <class T> bool readVector1(GraphStream& s, std::vector<T>& vec, size_t minElementSize) {
    int32_t num;
    if (!s.count(&num, minElementSize)) return false;
    vec.assign((size_t)num, T{});
    bool ok = true;
    for (int32_t i = 0; i < num && ok; i++) ok = vec[(size_t)i].read(s);
    return ok;
}
// readVector2 / mathReadVector: count, then stream.read(&element).
template <class T> bool readVector2(GraphStream& s, std::vector<T>& vec) {
    int32_t num;
    if (!s.count(&num, sizeof(T))) return false;
    vec.assign((size_t)num, T{});
    bool ok = true;
    for (int32_t i = 0; i < num && ok; i++) ok = s.read(&vec[(size_t)i]);
    return ok;
}

//-------------------------------------------------------------------------------------

struct GraphEdgeInfo {
    enum { Jetting = 1u << 0, Algorithmic = 1u << 16, Inventory = 1u << 18 };
    struct OneWay {
        BitSet32 flags;
        int32_t res = -1;
        int32_t dest = -1;
        bool isJetting() const { return flags.test(Jetting); }
    } to[2];
    // The segment between the two nodes; the normal points from the 1st to the 2nd.
    Point3F segPoints[2] = {{0, 0, 1}, {0, 0, 1}};
    Point3F segNormal{0, 0, 1};
    bool read(GraphStream& s);
};

struct IndoorNodeInfo {
    enum {
        Algorithmic = 1u << 1,
        WallNode = 1u << 2,
        Inventory = 1u << 4,
        BelowPortal = 1u << 5,
        Seed = 1u << 6
    };
    BitSet32 flags;
    uint16_t unused = 0;
    int16_t antecedent = -1;
    Point3F pos{-1, -1, -1};
    bool isAlgorithmic() const { return flags.test(Algorithmic); }
    bool isInventory() const { return flags.test(Inventory); }
    bool isBelowPortal() const { return flags.test(BelowPortal); }
    bool isSeed() const { return flags.test(Seed); }
    bool read(GraphStream& s);
};

// Persisted terrain (consolidated) node.
struct OutdoorNodeInfo {
    uint8_t flags = 0;
    int8_t level = -1;
    uint16_t height = 0;
    int16_t x = -1, y = -1;
    Point2I getPoint() const { return {x, y}; }
    int8_t getLevel() const { return level; }
    bool read(GraphStream& s);
};

using NodeInfoList = std::vector<IndoorNodeInfo>;
using EdgeInfoList = std::vector<GraphEdgeInfo>;
using Consolidated = std::vector<OutdoorNodeInfo>;

//-------------------------------------------------------------------------------------

struct SpawnLocations : std::vector<Point3F> {
    struct Sphere {
        SphereF mSphere;
        bool mInside = false;
        int32_t mCount = 0;
        int32_t mOffset = 0;
        int32_t mRes0 = 0;
        bool read(GraphStream& s);
    };
    int32_t mRes0 = 0;
    std::vector<Sphere> mSpheres;

    int32_t getRandom(const SphereF& sphere, bool inside, uint32_t rnd);
    void printInfo() const;
    void reset();
    bool read(GraphStream& s);
};

//-------------------------------------------------------------------------------------

inline uint8_t mapJetHopToU8(float hop) { return uint8_t(std::floor((hop + 0.124) * 8.0)); }
inline float mapU8ToJetHop(uint8_t amt) { return float(amt) * 0.125f; }

struct GraphBridgeData {
    enum How { CanWalk = 1, MustJet = 2, Replacement = 4, Unreachable = 8 };
    uint16_t nodes[2] = {0xffff, 0xffff};
    uint8_t jetClear = 0;
    uint8_t howTo = 0;
    bool read(GraphStream& s);
    bool isWalkable() const { return howTo & CanWalk; }
    bool isReplacement() const { return howTo & Replacement; }
    bool isUnreachable() const { return howTo & Unreachable; }
    bool mustJet() const { return !isWalkable(); }
};

class BridgeDataList : public std::vector<GraphBridgeData> {
    int32_t mReplaced[2] = {0, 0};
    int32_t mSaveTotal = 0;

public:
    int32_t replaced() const { return mReplaced[0] + mReplaced[1]; }
    // Non-replacement bridges: how many edges they add (memory tracking).
    int32_t numPositiveBridges() const { return (mSaveTotal << 1) - replaced(); }
    bool read(GraphStream& s);
};

//-------------------------------------------------------------------------------------
// Indoor node volumes (graphVolume.cc): each node's wall planes, then the
// floor, then the ceiling.

struct GraphVolume {
    PlaneF mFloor;
    PlaneF mCeiling;
    const PlaneF* mPlanes = nullptr;
    int32_t mCount = 0;
    std::vector<Point3F> mCorners;
};

struct GraphVolInfo {
    int32_t mPlaneCount = 0;
    int32_t mPlaneIndex = 0;
    bool read(GraphStream& s) { return s.read(&mPlaneCount) && s.read(&mPlaneIndex); }
};

class GraphVolumeList : public std::vector<GraphVolInfo> {
    bool intersectWalls(int32_t i, const PlaneF& with, std::vector<Point3F>& list) const;

public:
    std::vector<PlaneF> mPlanes;

    const PlaneF* planeArray(int32_t i) const { return &mPlanes[(size_t)(*this)[(size_t)i].mPlaneIndex]; }
    int32_t planeCount(int32_t i) const { return (*this)[(size_t)i].mPlaneCount; }
    PlaneF floorPlane(int32_t i) const { return planeArray(i)[planeCount(i) - 2]; }
    PlaneF abovePlane(int32_t i) const { return planeArray(i)[planeCount(i) - 1]; }
    float getMinExt(int32_t i, std::vector<Point3F>& ptBuffer) const;
    bool closestPoint(int32_t ind, const Point3F& point, Point3F& soln) const;
    int32_t getCorners(int32_t i, std::vector<Point3F>& pts, bool noTop = false) const;
    bool read(GraphStream& s);
};

//-------------------------------------------------------------------------------------

class LOSTable {
public:
    enum TwoBitCodes { Hidden, MinorLOS, MuzzleLOS, FullLOS };
    virtual ~LOSTable() = default;
    virtual uint32_t value(int32_t ind1, int32_t ind2) const = 0;
    virtual bool valid(int32_t numNodes) const = 0;
    virtual void clear() = 0;
    bool hidden(int32_t i1, int32_t i2) const { return value(i1, i2) == Hidden; }
    bool fullLOS(int32_t i1, int32_t i2) const { return value(i1, i2) == FullLOS; }
    bool muzzleLOS(int32_t i1, int32_t i2) const { return value(i1, i2) >= MuzzleLOS; }
};

// The node-to-node LOS table as stored (RevisedLOSToHash on): segments of 64
// destination nodes (2 bits each) per source node, found through a hash.
class LOSHashTable : public LOSTable {
public:
    enum { SegShift = 6, SegSize = (1 << SegShift), SegMask = (SegSize - 1), SegAlloc = (1 << (SegShift - 2)) };

protected:
    struct Key {
        uint16_t mNode = 0, mSeg = 0;
        uint32_t compare() const { return uint32_t(mNode) | (uint32_t(mSeg) << 16); }
    };
    struct Segment {
        uint8_t mLOS[SegAlloc] = {};
        Key mKey;
        uint32_t value(uint32_t i) const { return 3 & (mLOS[i >> 2] >> ((i & 3) << 1)); }
        bool read(GraphStream& s);
    };
    using IndexType = uint16_t;

    uint32_t mTabSz = 0;
    std::vector<IndexType> mTable;
    std::vector<Segment> mSegments;
    uint32_t mNumNodes = 0;
    uint32_t mRes0 = 0, mRes1 = 0;

    uint32_t calcHash(Key key) const;
    uint32_t calcTabSize();
    void sortByHashVal();
    uint32_t makeTheTable();

public:
    uint32_t value(int32_t ind1, int32_t ind2) const override;
    bool valid(int32_t numNodes) const override { return uint32_t(numNodes) == mNumNodes; }
    bool read(GraphStream& s);
    void clear() override;
};

//-------------------------------------------------------------------------------------

struct TerrainGraphInfo {
    static const Point2I gridOffs[8];
    static constexpr int32_t smVersion = 0;

    // false until data is set or loaded:
    bool haveGraph = false;

    // persisted data:
    int32_t nodeCount = -1;
    int32_t numShadows = -1;
    Point3F originWorld{-1, -1, -1};
    Point2I originGrid{-1, -1};
    Point2I gridDimensions{-1, -1};
    Point2I gridTopRight{-1, -1};
    std::vector<uint8_t> navigableFlags;
    std::vector<uint8_t> neighborFlags;
    std::vector<float> shadowHeights;
    std::vector<uint16_t> roamRadii;
    Consolidated consolidated;

    // Computed from the above at load:
    LineSegment boundarySegs[4];
    int32_t indOffs[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    GridArea gridArea;

    Point2I indexToPos(int32_t index) const;
    Point3F* indexToLoc(Point3F& locOut, int32_t index) const;
    bool posToLoc(Point3F& locOut, const Point2I& p) const;
    int32_t posToIndex(Point2I pos) const;
    int32_t locToIndex(Point3F loc) const;
    int32_t locToIndexAndSphere(SphereF& s, const Point3F& L) const;
    bool obstructed(const Point3F& L) const;
    uint8_t squareType(int32_t n) const { return navigableFlags[(size_t)n] & 7; }
    bool obstructed(int32_t n) const { return squareType(n) == GraphNodeObstructed; }
    bool shadowed(int32_t n) const { return squareType(n) == GraphNodeShadowed; }
    bool submerged(int32_t n) const { return squareType(n) == GraphNodeSubmerged; }
    bool haveConsData() const { return !consolidated.empty(); }
    float shadowHeight(int32_t n) const { return shadowHeights[(size_t)n]; }
    bool inGraphArea(const Point3F& loc) const { return locToIndex(loc) >= 0; }
    bool steep(int32_t n) const { return navigableFlags[(size_t)n] & GroundNodeSteep; }

    Point3F whereToInbound(const Point3F& loc);
    float checkOpenTerrain(const Point3F& from, Point3F& to) const;

    bool read(GraphStream& s);
    void setSideSegs();
    // After data is loaded, the last needed calcs.
    void doFinalDataSetup();
};

// terrain/terrData.h: 11.5 fixed point.
inline float fixedToFloat(uint16_t val) { return float(val) * 0.03125f; }
