#pragma once
// The run time of the Tribes 2 AI navigation graph (engine ai/graph*.cc):
// the NavigationGraph built from a mission's prebuilt terrains/<Mission>.nav
// (outdoor terrain grid nodes, indoor volume nodes, bridges and jetting
// edges), node location (GraphLocate, FindGraphNode, GraphSearchDist),
// transient nodes, Dijkstra / A* searches, LOS-table queries
// (GraphSearchLOS), threats, force field partitions, the JetManager, and
// NavigationPath, the per-bot path follower the AI code drives.
//
// Names and methods are the engine's so the AI port calls them as the
// engine does: gNavGraph, NavigationGraph::findLOSLocation / getChokePoints /
// fastDistance / hideOnSlope / hideOnDistance, gNavGraph->jetManager(),
// and NavigationPath's public API (graphPath.h). The Torque arithmetic on
// Point3F comes from NavMath (ai/graph_math.h).
//
// Not ported: graph generation (FloorPlan, GroundPlan, graphMake's table
// and bridge builders, graphBuildLOS, seeds, island culling, saving) and
// all rendering / debug drawing.
#include "ai/graph_data.h"
#include "sim/engine_object.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class NavigationGraph;
class NavigationPath;
class GraphSearch;
class TransientNode;
using GraphQIndex = int32_t;
using GraphThreatSet = uint64_t;

//-------------------------------------------------------------------------------------
//       Graph Edges (graphBase.cc)

class GraphEdge {
    enum { InvSpdBits = 6, InvTabSz = 1 << InvSpdBits };
    static const float csInvSpdTab[InvTabSz + 1];

    uint8_t mSteep : 1;
    uint8_t mInverse : InvSpdBits;
    uint8_t mOnPath : 1;
    uint8_t mJet : 1;
    uint8_t mDown : 1;
    uint8_t mJump : 1;
    uint8_t mTeam : 5;
    uint8_t mLateral;
    uint8_t mHopOver;

public:
    float mDist;
    int16_t mDest;
    int16_t mBorder;

    GraphEdge();
    bool empty() const { return mDest < 0; }
    bool isBorder() const { return mBorder >= 0; }
    bool isJetting() const { return mJet; }
    bool isDown() const { return mDown; }
    bool isJump() const { return mJump; }
    bool isSteep() const { return mSteep; }
    uint8_t getTeam() const { return mTeam; }
    uint8_t getLateral() const { return mLateral; }
    float getHop() const { return mapU8ToJetHop(mHopOver); }
    float getInverse() const { return csInvSpdTab[mInverse]; }
    bool hasHop() const { return mHopOver != 0; }
    void setJetting() { mJet = 1; }
    void setDown(bool b) { mDown = b; }
    void setJump(bool b) { mJump = b; }
    void setTeam(uint8_t team) { mTeam = team; }
    void setLateral(uint8_t amt) { mLateral = amt; }
    void setSteep(bool b) { mSteep = b; }
    void setImpossible() { setInverse(1e37f); }
    void setHop(float amt) { mHopOver = mapJetHopToU8(amt); }
    void setHop(uint8_t persistAmt) { mHopOver = persistAmt; }
    float getTime() const { return mDist * getInverse(); }
    void copyInverse(const GraphEdge* e) { mInverse = e->mInverse; }
    bool canJet(const float* ratings, bool both = false) const;
    void setInverse(float inv);
    const char* problems() const;
};

using GraphEdgeList = std::vector<GraphEdge>;
using GraphEdgePtrs = std::vector<GraphEdge*>;

class GraphEdgeArray {
    int32_t count = 0;
    GraphEdge* edges = nullptr;

public:
    GraphEdgeArray() = default;
    GraphEdgeArray(int32_t c, GraphEdge* e) : count(c), edges(e) {}
    GraphEdge* operator++(int) { return count ? (count--, edges++) : nullptr; }
    int32_t numEdges() const { return count; }
    void incCount() { count++; }
};

struct NodeProximity {
    float mLateral, mHeight, mAboveC;
    void makeBad() { mLateral = 1e13f; }
    void makeGood() { mAboveC = mLateral = -1e13f; }
    operator float&() { return mLateral; }
    operator float() const { return mLateral; }
    NodeProximity() : mLateral(1e13f), mHeight(0), mAboveC(0) {}
    bool inside() const { return mLateral <= 0 && mAboveC <= 0; }
    bool insideZ() const { return mAboveC <= 0; }
    bool possible() const;
};

//-------------------------------------------------------------------------------------
//                   Run Time Graph Nodes

constexpr int32_t GraphMaxOnPath = 31;

class GraphNode {
    friend class NavigationGraph;
    friend class GraphNodeList;

public:
    enum : uint32_t {
        NodeSpecific = 0xFF,
        Grid = 1u << 8,
        Outdoor = 1u << 9,
        Transient = 1u << 10,
        ExtendedGrid = 1u << 11,
        Indoor = 1u << 12,
        BelowPortal = 1u << 13,
        GotIsland = 1u << 14,
        Transient0 = 1u << 15,
        Transient1 = (Transient0 << 1),
        HaveTransient = (Transient0 | Transient1),
        PotentialSeed = 1u << 19,
        UsefulSeed = 1u << 20,
        Flat = 1u << 21,
        Algorithmic = 1u << 22,
        StuckAvoid = 1u << 23,
        Inventory = 1u << 24,
        Render0 = 1u << 25,
        Shadowed = 1u << 26,
        // Three contiguous bits for shoreline, which is wider for lava.
        LiquidPos = 27,
        LiquidZone = (0x7u << LiquidPos),
        Submerged = 1u << (LiquidPos + 0),
        ShoreLine = 1u << (LiquidPos + 1),
        LavaBuffer = 1u << (LiquidPos + 2),
    };

protected:
    BitSet32 mFlags;
    int16_t mIsland = -1;
    int16_t mIndex = -1;
    int8_t mOnPath = 0;
    int8_t mLevel = 0;
    uint32_t mAvoidUntil = 0;
    GraphThreatSet mThreats = 0;
    GraphEdgeList mEdges;
    Point3F mLoc{-1, -1, -1};

public:
    // graphTransient.cc
    GraphEdge* pushTransientEdge(int32_t dest);
    void popTransientEdge();

    virtual ~GraphNode() = default;

    void set(uint32_t bits) { mFlags.set(bits); }
    void clear(uint32_t bits) { mFlags.clear(bits); }
    bool test(uint32_t bits) const { return mFlags.test(bits); }
    bool grid() const { return test(Grid); }
    bool outdoor() const { return test(Outdoor); }
    bool indoor() const { return test(Indoor); }
    bool belowPortal() const { return test(BelowPortal); }
    bool shoreline() const { return test(ShoreLine); }
    bool shadowed() const { return test(Shadowed); }
    bool submerged() const { return test(Submerged); }
    bool stuckAvoid() const { return test(StuckAvoid); }
    bool transient() const { return test(Transient); }
    bool gotIsland() const { return test(GotIsland); }
    bool inventory() const { return test(Inventory); }
    bool algorithmic() const { return test(Algorithmic); }
    bool flat() const { return test(Flat); }
    bool canHaveBorders() const { return test(Indoor | Transient); }
    bool liquidZone() const { return test(LiquidZone); }
    bool potentialSeed() const { return test(PotentialSeed); }
    bool usefulSeed() const { return test(UsefulSeed); }
    int32_t island() const { return mIsland; }
    uint32_t onPath() const { return (uint32_t)mOnPath; }
    uint32_t avoidUntil() const { return mAvoidUntil; }
    void setOnPath() { mOnPath = GraphMaxOnPath; }
    void decOnPath() { if (mOnPath) mOnPath--; }
    GraphThreatSet& threats() { return mThreats; }

    virtual const Point3F& fetchLoc(Point3F& buff) const = 0;
    virtual GraphEdgeArray getEdges(GraphEdge* buff) const = 0;
    virtual int32_t getIndex() const = 0;

    virtual const Point3F& location() const;
    virtual const Point3F& getNormal() const;
    virtual const GraphEdge* getEdgePtr() const;
    virtual int32_t getLevel() const;
    virtual int32_t volumeIndex() const;
    virtual float edgeScale(const GraphNode* to) const;
    virtual NodeProximity containment(const Point3F& loc) const;
    virtual Point3F randomLoc() const;
    virtual float terrHeight() const;
    virtual float radius() const;
    virtual float minDim() const;
    virtual float area() const;

    void setIsland(int32_t islandNum);
    void setAvoid(uint32_t duration, bool important = false);
    bool neighbors(const GraphNode*) const;
    GraphEdge* getEdgeTo(int32_t to) const;
    GraphEdge* getEdgeTo(const GraphNode* n) const { return getEdgeTo(n->getIndex()); }
};

class GraphNodeList : public std::vector<GraphNode*> {
public:
    void setFlags(uint32_t bitset);
    void clearFlags(uint32_t bitset);
    int32_t searchSphere(const SphereF& sphere, const GraphNodeList& listIn);
    GraphNode* closest(const Point3F& loc, bool los = false);
    bool addUnique(GraphNode* node);
};

using GraphBSPTree = AxisAlignedBSP<GraphNode>;

class RegularNode : public GraphNode {
    friend class GraphSearch;

protected:
    Point3F mNormal{0, 0, 1};

public:
    RegularNode();
    GraphEdge& pushEdge(GraphNode* node);
    GraphEdgeArray getEdges(GraphEdge*) const override;
    const Point3F& getNormal() const override;

    void transientReserve() { mEdges.reserve(mEdges.size() + 2); }
    const Point3F& location() const override { return mLoc; }
    const Point3F& fetchLoc(Point3F&) const override { return mLoc; }
    const GraphEdge* getEdgePtr() const override { return mEdges.data(); }
    GraphEdgeArray getEdges() const { return getEdges(nullptr); }
    int32_t getIndex() const override { return mIndex; }
    float radius() const override { return 1.0f; }
    // GraphSearch::calcHeuristic reads the location directly.
    const Point3F& rawLoc() const { return mLoc; }
};

// Interior border crossing (graphIndoors.cc).
struct GraphBoundary {
    Point3F seg[2];
    Point3F normal;
    Point3F seekPt{0, 0, 0};
    float distIn = 0;

    explicit GraphBoundary(const GraphEdgeInfo&);
    void setItUp(int32_t N, const NodeInfoList&, const GraphVolumeList&);
    Point3F midpoint() const;
};
using GraphBoundaries = std::vector<GraphBoundary>;

class InteriorNode : public RegularNode {
    using Parent = RegularNode;
    float mMinDim, mArea;

public:
    InteriorNode();
    void init(const IndoorNodeInfo& data, int32_t index, const Point3F& normal);
    void setDims(float minDim, float area) { mMinDim = minDim; mArea = area; }
    float minDim() const override { return mMinDim; }
    float area() const override { return mArea; }
    GraphEdge& pushOneWay(const GraphEdgeInfo::OneWay&);
    NodeProximity containment(const Point3F& loc) const override;
};

class OutdoorNode : public RegularNode {
    friend class NavigationGraph;

protected:
    float mHeight = 0;

public:
    OutdoorNode();
    int32_t getLevel() const override;
    Point3F randomLoc() const override;
    float terrHeight() const override { return mHeight; }
    float radius() const override { return float(1 << mLevel) * gNavGlobs.mSquareRadius; }
    float minDim() const override { return float(1 << mLevel) * gNavGlobs.mSquareWidth; }
};

// graphTransient.cc
class TransientNode : public RegularNode {
    friend class NavigationGraph;
    GraphEdgeList mSearchEdges;
    int32_t mGroundStart = -1;
    const GraphNode* mClosest = nullptr;
    const GraphNode* mSaveClosest = nullptr;
    const GraphNode* mFirstHook = nullptr;

public:
    explicit TransientNode(int32_t index);
    GraphEdgeArray getEdges(GraphEdge*) const override;
    GraphEdgeArray getHookedEdges() const;
    void setLoc(const Point3F& loc) { mLoc = loc; }
    void setEdges(const GraphEdgeList& edges) { mEdges = edges; }
    void setClosest(const GraphNode* closest) { mClosest = closest; }
    int32_t volumeIndex() const override;
};

class GraphHookRequest {
    TransientNode& getHook(int32_t N);
    int32_t mIncarnation = -1;
    int32_t mPairLookup = -1;

public:
    GraphHookRequest() = default;
    ~GraphHookRequest();
    GraphHookRequest(const GraphHookRequest&) = delete;
    GraphHookRequest& operator=(const GraphHookRequest&) = delete;
    TransientNode& getHook1() { return getHook(0); }
    TransientNode& getHook2() { return getHook(1); }
    bool iGrowOld() const;
    void reset();
};

//-------------------------------------------------------------------------------------
//       Partitions (graphPartition.cc)

class GraphPartition {
public:
    enum Types { Armor, ForceField };
    enum Answer { CannotReach, CanReach, Ambiguous };

protected:
    Types mType = ForceField;
    bool mCanJetDown = false;
    BitVector mPartition;
    BitVector mDownhill;

public:
    void install(const GraphPartition& p) { mPartition.copy(p.mPartition); }
    void setDownhill(const GraphPartition& downhill);
    Answer reachable(int32_t from, int32_t to) const;
    void setSize(int32_t N) { mPartition.setSize((uint32_t)N); }
    void setType(Types t) { mType = t; }
    bool test(int32_t i) const { return mPartition.test((uint32_t)i); }
    void set(int32_t i) { mPartition.set((uint32_t)i); }
    void clear() { mPartition.clear(); }
};

class PartitionList : public std::vector<GraphPartition> {
protected:
    GraphPartition::Types mType = GraphPartition::ForceField;

public:
    void pushPartition(const GraphPartition& partition);
    void setType(GraphPartition::Types t) { mType = t; }
    GraphPartition::Answer reachable(int32_t from, int32_t to) const;
    int32_t haveEntry(int32_t forNode) const;
};

//-------------------------------------------------------------------------------------
//       Locating nodes (graphLocate.h)

struct ProximateNode {
    GraphNode* mNode;
    NodeProximity mProximity;
    ProximateNode(const NodeProximity& p, GraphNode* n) : mNode(n), mProximity(p) {}
};

class ProximateList : public std::vector<ProximateNode> {
public:
    void sort();
};

class GraphLocate : public GraphNodeList {
protected:
    Point3F mLocation;
    Point3F mLoc2D;
    float mTraveled;
    int32_t mCounter;
    bool mUpInAir;
    bool mMounted;
    GraphNode* mClosest;
    bool mTerrain;
    NodeProximity mMetric;
    GraphEdgeList mConnections;

    void getNeighbors(GraphNode* of, bool outdoor, bool makeJet = false);
    bool checkRegularEdge(Point3F from, GraphEdge* to);
    ProximateNode* examineCandidates(ProximateList& proximate);
    void beLikeNode(GraphNode* ourNode);
    void pushEdge(GraphNode* to, float* getCos = nullptr);

public:
    GraphLocate();
    void update(const Point3F& loc);
    bool canHookTo(const GraphLocate&, bool& jet) const;
    void reset();
    void setMounted(bool b);
    void forceCheck();

    const GraphEdgeList& getEdges() const { return mConnections; }
    GraphNode* bestMatch() const { return mClosest; }
    NodeProximity bestMetric() const { return mMetric; }
    bool onTerrain() const { return mTerrain; }
    bool isMounted() const { return mMounted; }
    void cleanup() { reset(); mMounted = false; }
};

// graphFind.cc: remembers loc-to-node results, also in a hash table on the
// graph.
class FindGraphNode {
    GraphNode* mClosest = nullptr;
    Point3F mPoint{-3.14159e14f, 3.1e13f, -2.22e22f};
    uint32_t calcHash(const Point3F& point);

public:
    enum { HashTableSize = 307 };
    FindGraphNode();
    explicit FindGraphNode(const Point3F& pt, GraphNode* hint = nullptr);
    void setPoint(const Point3F& pt, GraphNode* hint = nullptr);
    GraphNode* closest() const { return mClosest; }
    void init();
};

//-------------------------------------------------------------------------------------
//       Searches (graphSearches.h)

constexpr float SearchFailureThresh = 1e17f;
constexpr float SearchFailureAssure = 1e19f;

struct SearchRef {
    SearchRef(int32_t x, float d, float t) : mIndex((int16_t)x), mPrev(-1), mDist(d), mSort(t) {}
    union UserData {
        float f;
        uint32_t u;
        int32_t s;
        UserData() { f = 0; }
    };
    bool operator<(const SearchRef& sr2) const { return mSort > sr2.mSort; }
    int16_t mIndex;
    int16_t mPrev;
    float mDist;
    UserData mUser;
    float mTime = 0;
    float mSort;
};

using QIndexList = std::vector<GraphQIndex>;
using GraphQueue = BinHeap<SearchRef>;

class GraphSearch {
    GraphQueue& mQueue;
    QIndexList& mQIndices;
    std::vector<float>& mHeuristicsVec;
    GraphPartition& mPartition;
    // ^^^^ Shared lists ^^^^

    Point3F mTargetLoc;
    float* mHeuristicsPtr;
    int32_t mTargetNode, mSourceNode;
    float mSearchDist;
    bool mInProgress;
    int32_t mIterations;
    int32_t mRelaxCount;
    int32_t mTransientStart = 0;
    uint32_t mCurrentSimTime = 0;
    uint32_t mTeam = 0, mInformTeam;
    const float* mInformRatings;
    const float* mJetRatings = nullptr;
    GraphThreatSet mThreatSet, mInformThreats;
    GraphEdge mEdgeBuffer[MaxOnDemandEdges];
    bool mVisitOnExtract, mVisitOnRelax;
    bool mEarlyOut, mAStar, mInformAStar = false, mRandomize;

    bool initializeIndices();
    void doSmallReset();
    void doLargeReset();
    void resetIndices();
    SearchRef* insertSearchRef(int32_t idx, float dist, float time);
    int32_t getAvoidFactor();
    float calcHeuristic(const GraphEdge* to);
    void initSearch(GraphNode* S, GraphNode* D = nullptr);
    bool runDijkstra();

protected:
    GraphNode* mExtractNode;
    SearchRef mHead;
    void setDone() { mInProgress = false; }
    float distSoFar() const { return mHead.mDist; }
    float timeSoFar() const { return mHead.mTime; }
    GraphNode* extractedNode() const { return mExtractNode; }
    SearchRef* getSearchRef(int32_t nodeInd);
    SearchRef* lookupSearchRef(int32_t qInd);
    virtual void onQExtraction() { mVisitOnExtract = false; }
    virtual void onRelaxEdge(const GraphEdge*) { mVisitOnRelax = false; }
    virtual bool earlyOut() { return (mEarlyOut = false); }
    virtual float getEdgeTime(const GraphEdge* e);

public:
    GraphSearch();
    virtual ~GraphSearch() = default;
    int32_t performSearch(GraphNode* S, GraphNode* D = nullptr);
    bool getPathIndices(std::vector<int32_t>& nodeIndexList, int32_t target = -1);
    bool runSearch(GraphNode* S, GraphNode* D = nullptr);
    void setThreats(GraphThreatSet T) { mInformThreats = T; }
    void setRatings(const float* r) { mInformRatings = r; }
    void setTeam(uint32_t team) { mInformTeam = team; }
    const GraphPartition& getPartition() const { return mPartition; }
    bool inProgress() const { return mInProgress; }
    float searchDist() const { return mSearchDist; }
    int32_t getTarget() const { return mTargetNode; }
    int32_t relaxCount() const { return mRelaxCount; }
    void setRandomize(bool b) { mRandomize = b; }
    void setTarget(int32_t T) { mTargetNode = T; }
    void setEarlyOut(bool b) { mEarlyOut = b; }
    void setOnRelax(bool b) { mVisitOnRelax = b; }
    void setAStar(bool b = true) { mInformAStar = b; }
    class Globals {
        friend class GraphSearch;
        GraphQueue searchQ;
        QIndexList qIndices;
        std::vector<float> heuristics;
        GraphPartition partition;
    };
};

struct SearchThreat;

class GraphSearchLOS : public GraphSearch {    // graphSearchLOS.cc
    using Parent = GraphSearch;
    const LOSTable* mLOSTable = nullptr;
    const SphereF* mNearSphere = nullptr;
    int32_t mThreatCount = 0;
    int32_t mLOSKey = -1, mExtractInd = -1;
    Point3F mExtractLoc{0, 0, 0}, mLOSLoc{-1, -1, -1};
    SphereF mOuterSphere;
    std::vector<int32_t> mChokePoints;
    float mNearWidth = 0, mFarDist = 1e22f, mMaxChokeDist = 500;
    float mBestDistSqrd = 1e13f, mBestHidden = 0, mMinHidden = 22;
    float mMinSlopeDot = -1, mBestSlopeDot = -1;
    bool mSeekingLOS = false, mAvoidingLOS = false;
    bool mDistanceBased = true;
    bool mChokePtQuery = false;
    bool mNeedInside = false;
    void setNullDefaults();

protected:
    void onQExtraction() override;
    void onRelaxEdge(const GraphEdge*) override;
    float getEdgeTime(const GraphEdge* e) override;
    bool earlyOut() override { return true; }

public:
    int32_t findChokePoints(GraphNode* S, std::vector<Point3F>& pts, float hideD, float maxD);
    Point3F hidingPlace(const Point3F& from, const Point3F& avoid, float avoidRad, float avoidBasis, bool avoidOnSlope);
    Point3F findLOSLoc(const Point3F& from, const Point3F& wantToSee, float minDist, const SphereF& getCloseTo,
                       float capDist);
};

class GraphSearchDist : public GraphSearch {   // graphLocate.cc
    GraphNode* mBestMatch;
    NodeProximity mBestMetric;
    Point3F mPoint;
    ProximateList mProximate;
    float mCurThreshold = 1e13f;
    float mMaxSearchDist;
    bool mSearchOnDepth;

protected:
    float getEdgeTime(const GraphEdge* edge) override;
    void onQExtraction() override;

public:
    GraphSearchDist();
    NodeProximity findBestMatch(GraphNode* cur, const Point3F& loc);
    GraphNode* bestMatch() const { return mBestMatch; }
    void setOnDepth(bool b) { mSearchOnDepth = b; }
    bool earlyOut() override { return true; }
    ProximateList& getProximate() { return mProximate; }
    void setDistCap(float d) { mMaxSearchDist = d; }
};

//-------------------------------------------------------------------------------------
//       Threats (graphThreats.h). The threat is a ShapeBase, held by its
//       handle (the engine's SimObjectPtr: gone once the object is).

struct SearchThreat : public SphereF {
    SearchThreat() = default;
    SearchThreat(const std::string& threat, float R, int32_t team);
    void updateNodes();
    bool checkEnableChange();
    class ShapeBase* threatObject() const;
    std::string mThreat;
    GraphNodeList mEffects;
    bool mActive = false;
    int16_t mTeam = 0;
    int16_t mSlot = 0;
};

class SearchThreats {
public:
    enum { MaxThreats = (sizeof(GraphThreatSet) << 3) };

protected:
    void informOtherTeams(int32_t onTeam, int32_t thisThreat, bool isOn);
    int32_t getThreatPtrs(const SearchThreat* L[MaxThreats], GraphThreatSet S) const;

    SearchThreat mThreats[MaxThreats];
    GraphThreatSet mTeamCaresAbout[GraphMaxTeams];
    int32_t mCheck, mCount;
    uint32_t mSaveTimeMS;

public:
    SearchThreats();
    void pushTempThreat(GraphNode* node, const Point3F& loc, float rad);
    void popTempThreat();
    bool checkOne();
    bool add(const SearchThreat&);
    GraphThreatSet getThreatSet(int32_t forTeam) const;
    bool sanction(const Point3F& from, const Point3F& to, int32_t team) const;
    void monitorThreats();
    int32_t numThreats() const { return mCount; }
};

//-------------------------------------------------------------------------------------
//       Force fields (graphForceField.h). A field is held by its handle.

class MonitorForceFields {
public:
    struct ForceField {
        void updateEdges();
        std::string mFF;
        GraphEdgePtrs mEffects;
        bool mActive = false;
        uint32_t mTeam = 0;
    };

protected:
    std::vector<ForceField> mFFList;
    int32_t mCount = 0;
    uint32_t mSaveTimeMS = 0;
    PartitionList mTeamPartitions[GraphMaxTeams];

public:
    MonitorForceFields();
    GraphPartition::Answer reachable(uint32_t team, int32_t from, int32_t to);
    void clearPartitions(uint32_t allBut);
    void informSearchFailed(GraphSearch* searcher, uint32_t team);
    int32_t count() const { return mCount; }
    void atMissionStart();
    void monitor();
};

//-------------------------------------------------------------------------------------
//       Jetting (graphJetting.h)

class JetManager {
public:
    class ID {
        friend class JetManager;
        int32_t id = -1;
        int32_t incarnation = -1;

    public:
        ID() = default;
        ~ID();
        ID(const ID&) = delete;
        ID& operator=(const ID&) = delete;
        void reset();
    };
    struct Ability {
        float acc = -111, dur = -111, v0 = -111;
        bool operator==(const Ability&) const;
    };
    friend class ID;

    static float modifiedLateral(float xyDist);
    static float modifiedDistance(float xyDist, float zDist);
    static float invertLateralMod(float lateralField);

protected:
    struct JetPartition : public PartitionList {
        float ratings[2];
        Ability ability;
        int32_t users;
        bool used;
        uint32_t time;
        JetPartition() { doConstruct(); }
        void doConstruct();
    };

    std::vector<uint16_t> mFloodInds[2];
    std::unique_ptr<GraphPartition> mArmorPart;
    JetPartition mPartitions[AbsMaxBotCount];

    int32_t floodPartition(uint32_t seed, bool needBoth, float ratings[2]);
    void decRefCount(const ID& id);

public:
    int32_t partitionOneArmor(JetPartition& list);
    GraphPartition::Answer reachable(const ID& jetCaps, int32_t from, int32_t to);
    const float* getRatings(const ID& jetCaps);
    float jetDistance(const Point3F& from, const Point3F& to) const;
    float calcJetScale(const GraphNode* from, const GraphNode* to) const;
    void initEdge(GraphEdge& edge, const GraphNode* src, const GraphNode* dst);
    void replaceEdge(GraphBridgeData& replaceInfo);
    float estimateEnergy(const ID& jetCaps, const GraphEdge* edge);
    float calcJetRating(float thrust, float duration);
    void calcJetRatings(float* ratings, const Ability& ability);
    bool update(ID& id, const Ability& ability);
    void clear();
};

//-------------------------------------------------------------------------------------
//       NavigationPath (graphPath.h)

struct NavJetting {
    NavJetting() { init(); }
    void init() { mHopOver = 0.0f; mChuteUp = mChuteDown = false; }
    float mHopOver;
    bool mChuteUp;
    bool mChuteDown;
};

class NavigationPath {
public:
    using VisitState = std::vector<BitSet32>;
    struct State {
        enum { Visited = (1 << 0), Outbound = (1 << 1), Skipped = (1 << 2) };
        void constructThis();
        std::vector<int32_t> path;
        VisitState visit;
        GraphEdgePtrs edges;
        Point3F seekLoc;
        Point3F hereLoc;
        Point3F destLoc;
        bool thisEdgeJetting;
        bool nextEdgeJetting;
        bool nextEdgePrecise;
        int32_t curSeekNode;
    };

private:
    enum HowToRedo { OnDist, OnPercent };
    void constructThis();
    bool checkPathUpdate(TransientNode& here, TransientNode& there);
    bool updateTransients(TransientNode& src, TransientNode& dst, bool redo);
    void saveEndpoints(TransientNode* from, TransientNode* to);
    void computePath(TransientNode& from, TransientNode& to);
    float estimateEnergy(const GraphEdge* edge);
    void checkWallAvoid();
    void randomization();
    void markRenderPath();
    void afterCompute();
    bool canAdvance();
    void setEdgeConstraints();
    void advanceByOne();

    int32_t checkOutsideAdvance();
    GraphNode* getNode(int32_t idx);
    Point3F getLoc(int32_t index);

    State mState;
    Point3F mHereLoc{0, 0, 0};
    Point3F mDestLoc{0, 0, 0};
    Point3F mAdjustSeek;
    HowToRedo mRedoMode;
    bool mForceSearch;
    bool mAreIndoors;
    bool mPathIndoors;
    bool mAwaitingSearch;
    bool mBusyJetting;
    float mPctThreshSqrd;
    float mRedoDistSqrd;
    float mRedoPercent = 0.15f;
    float mSearchDist;
    uint32_t mTeam;
    Point3F mSaveDest;
    int32_t mRepathCounter, mTimeSlice;
    int32_t mStopCounter;
    int32_t mSaveSeekNode;
    bool mUserStuck;
    bool mSearchWasValid;
    GraphEdge* mCurEdge;
    GraphLocate mLocateHere;
    GraphLocate mLocateDest;
    GraphHookRequest mHook;
    Point3F mSavedEndpoints[2];
    GraphEdge mSaveLastEdge;
    JetManager::ID mJetCaps;
    NavJetting mNavJetting;
    FindGraphNode mFindHere;
    double mCastAng, mCastZ;
    Point3F mCastAverage;
    const GraphEdge* mEstimatedEdge;
    float mEstimatedEnergy;

public:
    NavigationPath();

    bool updateLocations(const Point3F& src, const Point3F& dst);
    float checkOpenTerrain(const Point3F& from, Point3F& to);
    bool locationIsOutdoors(const Point3F& loc, float* roamDist = nullptr);
    bool weAreIndoors() const;
    bool userMustJet() const;
    bool intoMount() const;
    void informJetDone();
    void setJetAbility(const JetManager::Ability& ability);
    void informStuck(const Point3F& stuckLoc, const Point3F& stuckDest);
    void informProgress(const Point3F& vel);
    float jetWillNeedEnergy(float maxDist);
    Point3F getLocOnPath(float dist);
    float distRemaining(float maxCare = 1e9f);
    bool getPathNodeLoc(int32_t ahead, Point3F& loc);
    const Point3F& getSeekLoc(const Point3F& vel);
    bool canReachLoc(const Point3F& dst);
    void missionCycleCleanup();
    float visitRadius();
    void forceSearch();

    NavJetting* getJetInfo() { return &mNavJetting; }
    float searchDist() const { return mSearchDist; }
    bool isPathCurrent() const { return !mAwaitingSearch; }
    void getLastPath(std::vector<int32_t>& L) { L = mState.path; }
    void setTeam(uint32_t whichTeam) { mTeam = whichTeam; }
    void setRedoDist(float D = 0.8f) { mRedoDistSqrd = D * D; mRedoMode = OnDist; }
    void setRedoPercent(float P = 0.15f) { mRedoPercent = P; mRedoMode = OnPercent; }
    void setDestMounted(bool b) { mLocateDest.setMounted(b); }
    void setSourceMounted(bool b) { mLocateHere.setMounted(b); }
    void informJetBusy() { mBusyJetting = true; }
};

//-------------------------------------------------------------------------------------
//       NavigationGraph

class NavigationGraph : public EngineObject {
    friend class NavigationPath;

public:
    // graph.cc version history; only ChangedToFloorPlan on is loadable.
    enum Version {
        TerrainOnly, HandLaidInterior, AddedBridges, AddedPathTable, AddedLOSTable,
        ChangedToFloorPlan, TrimmedBridges, RevisedLOSToHash, BetterSpawnMode
    };
    static constexpr int32_t sVersion = BetterSpawnMode;

    static int32_t sTotalEdgeCount;
    static uint32_t sLoadMemUsed;

    static bool gotOneWeCanUse();
    static bool hasSpawnLocs();
    static void warning(const char* msg);
    static float fastDistance(const Point3F& p1, const Point3F& p2);
    static Point3F hideOnSlope(const Point3F& from, const Point3F& avoid, float rad, float deg);
    static Point3F hideOnDistance(const Point3F& from, const Point3F& avoid, float rad, float hideLen = 10.0f);
    static Point3F findLOSLocation(const Point3F& from, const Point3F& wantToSee, float minAway,
                                   const SphereF& getCloseTo, float capDist = 1e11f);
    static int32_t getChokePoints(const Point3F& srcPoint, std::vector<Point3F>& points, float minHideDist,
                                  float stopSearchDist = 1e9f);
    static float whereToLook(Point3F P);
    // Go up slightly and cast down to ground (cf. graph.cc adjustSpawnLoc).
    static void adjustSpawnLoc(Point3F& point);

protected:
    // .NAV or .SPN file data:
    SpawnLocations mSpawnList;
    EdgeInfoList mEdgeInfoList;
    NodeInfoList mNodeInfoList;
    GraphVolumeList mNodeVolumes;
    TerrainGraphInfo mTerrainInfo;
    BridgeDataList mBridgeList;
    LOSHashTable mLOSHashTable;

    // Run time node / edge lists:
    GraphNodeList mNodeList;
    GraphNodeList mNodeGrid;
    GraphNodeList mNonTransient;
    GraphNodeList mIndoorPtrs;
    std::vector<InteriorNode> mIndoorNodes;
    std::vector<OutdoorNode> mOutdoorNodes;
    GraphNodeList mIslandPtrs;
    std::vector<int32_t> mIslandSizes;
    GraphBSPTree mIndoorTree;
    LOSTable* mLOSTable = nullptr;

    int32_t mVersion = -1;
    int32_t mLargestIsland = -1;
    int32_t mNumOutdoor = 0;
    int32_t mNumIndoor = 0;
    int32_t mTransientStart = -1;
    static int32_t mIncarnation;
    const int32_t mMaxTransients;
    bool mValidLOSTable = false;
    bool mValidPathTable = false;
    bool mHaveVolumes = false;
    bool mPushedBridges = false;
    bool mIsSpawnGraph = false;
    bool mDeadlyLiquid = false;
    float mSubmergedScale = 1.0f;
    float mShoreLineScale = 1.0f;
    std::unique_ptr<GraphSearch> mMainSearcher;
    std::unique_ptr<GraphSearchLOS> mLOSSearcher;
    std::unique_ptr<GraphSearchDist> mDistSearcher;
    GraphBoundaries mBoundaries;
    // GroundPlan::getTerrainObj() when the graph was added.
    bool mHaveTerrainBlock = false;
    GraphEdge mEdgeBuffer[MaxOnDemandEdges];
    GraphEdgePtrs mVisibleEdges;
    SearchThreats mThreats;
    MonitorForceFields mForceFields;
    JetManager mJetManager;

    std::vector<int32_t> mTempNodeBuf;
    GraphNodeList mUtilityNodeList1;
    GraphNodeList mUtilityNodeList2;
    GraphVolume mTempVolumeBuf;

public:
    FindGraphNode mFoundNodes[FindGraphNode::HashTableSize];

protected:
    int32_t markIslands();
    int32_t doFinalFixups();
    void clearLoadData();
    bool initInteriorNodes(const EdgeInfoList&, const NodeInfoList&, int32_t);
    int32_t getNodesInArea(GraphNodeList& listOut, GridArea gridArea);
    void makeRunTimeNodes();
    int32_t setupOutdoorNodes(const GridArea& area, const Consolidated& cons, GraphNodeList& grid,
                              GraphNodeList& list);
    float distancef(const Point3F& p1, const Point3F& p2);
    void chokePoints(const Point3F& S, std::vector<Point3F>& pts, float H, float M);
    int32_t crossingSegs(const GraphNode*, const GraphEdge*, LineSegment* segs);
    void newIncarnation();
    int32_t hookTransient(TransientNode&);
    void unhookTransient(TransientNode&);
    void resetSpawnList() { mSpawnList.reset(); }

public:
    NavigationGraph();
    ~NavigationGraph() override;
    NavigationGraph(const NavigationGraph&) = delete;
    NavigationGraph& operator=(const NavigationGraph&) = delete;

    // NavigationGraph::onAdd: reads $GraphForceLoad, $OFFLINE_NAV_BUILD,
    // $CurrentMissionType, $HostGameBotCount and $CurrentMission, loads, and
    // for a NAV graph makes it and pushes its bridges.
    void onAdd();
    // onAdd without the script engine's globals.
    bool loadForMission(const std::string& missionName, bool spawnGraph);

    bool inNodeVolume(const GraphNode* node, const Point3F& point);
    NodeProximity getContainment(int32_t indoorNodeIndex, const Point3F& point);
    bool verticallyInside(int32_t indoorIndex, const Point3F& point);
    bool closestPointOnVol(GraphNode*, const Point3F& pt, Point3F& soln) const;
    bool possibleToJet(const Point3F& from, const Point3F& to, uint32_t armor = 0);
    float heightAboveFloor(int32_t indoorIndex, const Point3F& point);
    int32_t indoorIndex(const GraphNode* node);
    PlaneF getFloorPlane(GraphNode* node);

    bool useVolumeTraverse(const GraphNode* from, const GraphEdge* to);
    bool volumeTraverse(const GraphNode* from, const GraphEdge* to, NavigationPath::State&);
    int32_t checkIndoorSkip(NavigationPath::State& state);
    bool installThreat(const std::string& threat, int32_t team, float rad);

    // Three types of search machines get used-
    GraphSearch* getMainSearcher();
    GraphSearchLOS* getLOSSearcher();
    GraphSearchDist* getDistSearcher();

    // Transient management
    void destroyPairOfHooks(int32_t idx);
    int32_t createPairOfHooks();
    bool pushTransientPair(TransientNode& src, TransientNode& dst, uint32_t team, const JetManager::ID& jetCaps);
    bool canReachLoc(const FindGraphNode& src, const FindGraphNode& dst, uint32_t team,
                     const JetManager::ID& jetCaps);
    void popTransientPair(TransientNode& src, TransientNode& dst);

    // Loading and the run-time graph
    bool load(const std::vector<uint8_t>& bytes, bool isSpawn = false);
    bool loadGraph();
    bool loadGraph(const std::string& missionName);
    bool makeGraph();
    const char* pushBridges();
    int32_t randNode(const Point3F& P, float R, bool in, bool out);
    void detectForceFields() { mForceFields.atMissionStart(); }
    const Point3F* getRandSpawnLoc(int32_t nodeIndex);
    const Point3F* getSpawnLoc(int32_t nodeIndex);
    int32_t findJumpableNodes();
    void expandShoreline(uint32_t wave = 0);

    // Node and edge finding
    const GraphEdgePtrs& getBlockedEdges(const std::string& forceField);
    const GraphNodeList& getVisibleNodes(GraphNode* S, const Point3F& loc, float rad);
    const GraphNodeList& getVisibleNodes(const Point3F& loc, float rad);
    const GraphVolume& fetchVolume(const GraphNode* N, bool above);
    int32_t getNodesInBox(Box3F worldBox, GraphNodeList& listOut, bool justIndoor = false);
    GraphNode* findTerrainNode(const Point3F& loc);
    GraphNode* nearbyTerrainNode(const Point3F& loc);
    GraphNode* closestNode(const Point3F& loc, float* containment = nullptr);
    bool haveMuzzleLOS(int32_t nodeInd1, int32_t nodeInd2);
    bool terrainHeight(Point3F pos, float* height, Point3F* normal = nullptr);
    float getRoamRadius(const Point3F& loc);
    void worldToGrid(const Point3F& wPos, Point2I& gPos);
    Point3F gridToWorld(const Point2I& gPos);
    GridArea getGridRectangle(const Point3F& atPos, int32_t gridRad);
    GridArea getWorldRect();

    // One-liners
    int32_t incarnation() const { return mIncarnation; }
    GraphNode* lookupNode(int32_t X) const { return mNodeList[(size_t)X]; }
    int32_t numIslands() const { return (int32_t)mIslandPtrs.size(); }
    int32_t numOutdoor() const { return mNumOutdoor; }
    int32_t numIndoor() const { return mNumIndoor; }
    int32_t numNodesAll() const { return (int32_t)mNodeList.size(); }
    int32_t numNodes() const { return (int32_t)mNonTransient.size(); }
    bool validLOSXref() const { return mValidLOSTable; }
    bool validPathXRef() const { return mValidPathTable; }
    bool haveVolumes() const { return mHaveVolumes; }
    bool haveForceFields() const { return mForceFields.count() != 0; }
    float submergedScale() const { return mSubmergedScale; }
    float shoreLineScale() const { return mShoreLineScale; }
    bool deadlyLiquids() const { return mDeadlyLiquid; }
    bool haveTerrain() const { return mHaveTerrainBlock; }
    int32_t numBridges() const { return mBridgeList.numPositiveBridges(); }
    int32_t largestIsland() const { return mLargestIsland; }
    int32_t numSpawns() const { return (int32_t)mSpawnList.size(); }
    Point3F getSpawn(int32_t i) const { return mSpawnList[(size_t)i]; }
    void printSpawnInfo() const { mSpawnList.printInfo(); }
    void setGenMode(bool spawn) { mIsSpawnGraph = spawn; }
    bool isSpawnGraph() const { return mIsSpawnGraph; }
    int32_t version() const { return mVersion; }
    void monitorForceFields() { mForceFields.monitor(); }
    std::vector<int32_t>& tempNodeBuff() { return mTempNodeBuf; }
    const LOSTable* getLOSXref() const { return mValidLOSTable ? mLOSTable : nullptr; }
    const GraphBoundary& getBoundary(int32_t i) { return mBoundaries[(size_t)i]; }
    GraphThreatSet getThreatSet(int32_t T) const { return mThreats.getThreatSet(T); }
    void newPartition(GraphSearch* S, uint32_t T) { mForceFields.informSearchFailed(S, T); }
    SearchThreats* threats() { return &mThreats; }
    JetManager& jetManager() { return mJetManager; }
    const TerrainGraphInfo& terrainInfo() const { return mTerrainInfo; }

    // The data shared (for memory) among searchers.
    GraphSearch::Globals mSharedSearchLists;
};

// The graph the global queries use: the one most recently constructed.
extern NavigationGraph* gNavGraph;

// Sim::getCurrentTime in milliseconds.
uint32_t navSimTimeMS();

// gServerContainer.castRay for the graph code: the first hit from a to b of
// anything in `mask` (engine object type bits, as SimContainer::TypeMasks).
// World geometry is split as the engine's masks ask: terrain for
// TerrainObjectType, interiors for InteriorObjectType (StaticObjectType
// takes both), closed force fields for ForceFieldObjectType.
struct NavRayInfo {
    float t = 1.0f;
    Point3F point{0, 0, 0};
    Point3F normal{0, 0, 1};
    uint32_t objectType = 0;   // the hit object's type mask (mColl.object->getTypeMask())
};
bool navCastRay(const Point3F& a, const Point3F& b, uint32_t mask, NavRayInfo& info);

// graphLOS.h: LOS casts with a type mask (the checks the graph builder and
// the AI jetting code use).
struct Loser {
    static uint32_t mCasts;
    const bool mCheckingFF;
    const uint32_t mMask;
    NavRayInfo mColl;
    bool mHitForceField = false;

    explicit Loser(uint32_t mask, bool checkFF = false) : mCheckingFF(checkFF), mMask(mask) {}
    bool haveLOS(const Point3F& src, const Point3F& dst);
    bool fannedLOS1(const Point3F& S, const Point3F& D, const Point3F& inc, int32_t N);
    bool fannedLOS2(const Point3F& S, const Point3F& D, const Point3F& inc, int32_t N);
    bool walkOverBumps(Point3F S, Point3F D, float inc = 0.2f);
    bool fannedBumpWalk(const Point3F& S, const Point3F& D, Point3F inc, int32_t N);
    bool findHopLine(Point3F S, Point3F D, float maxUp, float& freeHt);
    bool walkOverGaps(Point3F S, Point3F D, float allowedGap);
    bool hitBelow(Point3F& dropPoint, float castDown);
    float heightUp(const Point3F& from, float max);
};
