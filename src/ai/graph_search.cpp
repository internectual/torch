// Graph nodes and edges (ai/graphBase.cc), transient nodes
// (graphTransient.cc), partitions (graphPartition.cc), the searchers
// (graphDijkstra.cc, graphSearchLOS.cc) and the node locator
// (graphLocate.cc).
#include "ai/graph.h"
#include "core/console.h"
#include "sim/nav_graph.h"
#include <cmath>
#include <cstdlib>

using namespace NavMath;

//-------------------------------------------------------------------------------------
//                                  GraphEdge

GraphEdge::GraphEdge() {
    mDest = -1;       // Destination node index
    mBorder = -1;     // Border segment on interiors
    mDist = 1.0f;     // 3D dist, though jet edges are adjusted a little so ratings work
    mJet = 0;         // Jetting connection
    mHopOver = 0;     // For jetting, amount to hop up and over
    mOnPath = 0;
    mTeam = 0;        // An edge only this team can pass - i.e. force fields.
    mJump = 0;        // On jetting edges - is it flat enough to jump?
    mLateral = 0;     // Crude lateral distance - needed to know if we can jet down.
    mDown = 0;        // Down side of a jet?
    mSteep = 0;       // Can't walk up
    mInverse = 0;
    setInverse(1.0f);
}

const char* GraphEdge::problems() const {
    if (mDist < 0) return "Dijkstra() forbids edge with negative distance";
    if (getInverse() < 0) return "Edge with negative scale";
    return nullptr;
}

// Given the jet ratings, can this connection be traversed? Down only needs
// the lateral, except for the partitioner's first pass (both).
bool GraphEdge::canJet(const float* ratings, bool both) const {
    const float rating = ratings[mJump];
    if (!mDown || both) return rating >= mDist;
    return rating > float(mLateral);
}

// The inverse speeds, kept in a few bits.
const float GraphEdge::csInvSpdTab[InvTabSz + 1] = {
    0.0f,    0.1f,    0.2f,    0.3f,    0.4f,
    0.6f,    0.8f,    1.0f,    1.2f,    1.4f,

    1.7f,    2.0f,    2.6f,    3.5f,    4.6f,
    5.7f,    6.8f,    8.0f,    9.2f,    10.4f,

    12.0f,   14.1f,   17.0f,   20.0f,   24.0f,
    30.0f,   37.0f,   45.0f,   54.0f,   64.0f,

    80.0f,   100.0f,  125.0f,  156.0f,  200.0f,
    400.0f,  800.0f,  1600.0f, 3200.0f, 6400.0f,

    2.56e4f, 1.02e5f, 4.1e5f,  1.6e6f,  6.5e6f,
    2.6e7f,  1.05e8f, 4.19e8f, 1.68e9f, 9.9e10f,

    1e11f,   1e12f,   1e13f,   1e14f,   1e15f,
    1e16f,   1e17f,   1e18f,   1e19f,   1e20f,
    1e21f,   1e22f,   1e23f,   1e24f,

    // Sentinal value (1e40 as the engine's F32: infinity)
    INFINITY,
};

// The 6-bit number by a binary search: the largest entry we're >= to.
void GraphEdge::setInverse(float is) {
    int32_t lo = 0;
    int32_t hi = InvTabSz;
    for (int32_t i = InvSpdBits; i > 0; i--) {
        const int32_t mid = (hi + lo) >> 1;
        if (is >= csInvSpdTab[mid])
            lo = mid;
        else
            hi = mid;
    }
    mInverse = (uint8_t)lo;
}

//-------------------------------------------------------------------------------------
//                         GraphNode Methods, Default Virtuals

// Derived classes supply the fetcher which takes a buffer; this patches to
// it with a ring of buffers.
const Point3F& GraphNode::location() const {
    static Point3F buff[8];
    static uint8_t ind = 0;
    return fetchLoc(buff[ind++ & 7]);
}

// Outdoor nodes use level (from -1); path randomization also uses it.
int32_t GraphNode::getLevel() const { return -2; }

void GraphNode::setIsland(int32_t num) {
    mFlags.set(GotIsland);
    mIsland = (int16_t)num;
}

// The ratio approximates travel speed on slopes:
//       xy / total           going down (XY is net path distance)
//       total / xy           going up
static float getBaseEdgeScale(const Point3F& src, Point3F dst) {
    const float totalDist = len(dst -= src);
    const bool goingUp = (dst.z > 0);
    dst.z = 0;
    const float xyDist = len(dst);
    float ratio = 1.0f;
    if (xyDist > 0.01f && totalDist > 0.01f) {
        if (goingUp) {
            ratio = (totalDist / xyDist);
            if (ratio < 1.2f) ratio = 1.0f;
        } else {
            ratio = (xyDist / totalDist);
        }
        // Between the square root and the straight value: the 3/4 power.
        ratio = std::sqrt(std::sqrt(ratio) * ratio);
    }
    return ratio;
}

float GraphNode::edgeScale(const GraphNode* dest) const {
    float scale = getBaseEdgeScale(location(), dest->location());
    if (submerged())
        scale *= gNavGraph->submergedScale();
    else if (shoreline())
        scale *= gNavGraph->shoreLineScale();
    if (dest->submerged())
        scale *= gNavGraph->submergedScale();
    else if (dest->shoreline())
        scale *= gNavGraph->shoreLineScale();
    // The searcher reserves REALLY large numbers to signal search failure.
    return std::min(scale, 1000000000.0f);
}

static GraphEdge sEdgeBuff[MaxOnDemandEdges];

bool GraphNode::neighbors(const GraphNode* other) const {
    GraphEdgeArray edgeList = other->getEdges(sEdgeBuff);
    const int32_t indexOfThis = getIndex();
    while (GraphEdge* edge = edgeList++)
        if (edge->mDest == indexOfThis) return true;
    return false;
}

GraphEdge* GraphNode::getEdgeTo(int32_t to) const {
    GraphEdgeArray edgeList = getEdges(sEdgeBuff);
    while (GraphEdge* edge = edgeList++)
        if (edge->mDest == to) return edge;
    return nullptr;
}

const GraphEdge* GraphNode::getEdgePtr() const { return nullptr; }
// Only called on nodes known to be terrain.
float GraphNode::terrHeight() const { return 1e17f; }
float GraphNode::radius() const { return 2.0f; }
float GraphNode::area() const { return 0.01f; }
// Thinness of the node.
float GraphNode::minDim() const { return 0.01f; }

const Point3F& GraphNode::getNormal() const {
    static const Point3F sStraightUp{0, 0, 1};
    return sStraightUp;
}

// How much a node contains a point: more negative is more inside.
NodeProximity GraphNode::containment(const Point3F& point) const {
    NodeProximity nodeProx;
    nodeProx.mLateral = len(point - location()) - radius();
    return nodeProx;
}

Point3F GraphNode::randomLoc() const { return location(); }

// Nodes with volumes (lets transients answer with the volume they're in).
int32_t GraphNode::volumeIndex() const { return getIndex(); }

// Set the avoidance if it isn't already (important overrides).
void GraphNode::setAvoid(uint32_t duration, bool isImportant) {
    const uint32_t curTime = navSimTimeMS();
    if (curTime > mAvoidUntil || isImportant) {
        mAvoidUntil = curTime + duration;
        mFlags.set(StuckAvoid, isImportant);
        // On avoidance, also mark the neighbors (a slight avoid).
        if (isImportant) {
            GraphEdgeArray edgeList = getEdges(sEdgeBuff);
            while (GraphEdge* edge = edgeList++) {
                GraphNode* node = gNavGraph->lookupNode(edge->mDest);
                node->mAvoidUntil = mAvoidUntil;
                node->mFlags.clear(StuckAvoid);
            }
        }
    }
}

//-------------------------------------------------------------------------------------

RegularNode::RegularNode() { mIndex = -1; }

GraphEdgeArray RegularNode::getEdges(GraphEdge*) const {
    return GraphEdgeArray((int32_t)mEdges.size(), const_cast<GraphEdge*>(mEdges.data()));
}

GraphEdge& RegularNode::pushEdge(GraphNode* node) {
    GraphEdge edge;
    edge.mDest = (int16_t)node->getIndex();
    mEdges.push_back(edge);
    return mEdges.back();
}

const Point3F& RegularNode::getNormal() const { return mNormal; }

//-------------------------------------------------------------------------------------

void GraphNodeList::setFlags(uint32_t bits) {
    for (GraphNode* node : *this)
        if (node) node->mFlags.set(bits);
}

void GraphNodeList::clearFlags(uint32_t bits) {
    for (GraphNode* node : *this)
        if (node) node->mFlags.clear(bits);
}

int32_t GraphNodeList::searchSphere(const SphereF& sphere, const GraphNodeList& listIn) {
    clear();
    const float radiusSquared = sphere.radius * sphere.radius;
    for (GraphNode* node : listIn)
        if (node)
            if (lenSquared(sphere.center - node->location()) < radiusSquared) push_back(node);
    return (int32_t)size();
}

GraphNode* GraphNodeList::closest(const Point3F& loc, bool) {
    GraphNode* bestNode = nullptr;
    float bestRadSquared = 1e12f, dSqrd;
    for (GraphNode* node : *this)
        if (node)
            if ((dSqrd = lenSquared(loc - node->location())) < bestRadSquared) {
                bestRadSquared = dSqrd;
                bestNode = node;
            }
    return bestNode;
}

bool GraphNodeList::addUnique(GraphNode* node) {
    for (GraphNode* n : *this)
        if (n == node) return false;
    push_back(node);
    return true;
}

//-------------------------------------------------------------------------------------
//                            Transient nodes (graphTransient.cc)

GraphEdge* GraphNode::pushTransientEdge(int32_t dest) {
    // Hook back to the supplied destination-
    GraphEdge edge;
    edge.mDest = (int16_t)dest;
    edge.mDist = len(mLoc - gNavGraph->lookupNode(dest)->location());
    mEdges.push_back(edge);
    return &mEdges.back();
}

void GraphNode::popTransientEdge() { mEdges.pop_back(); }

TransientNode::TransientNode(int32_t index) {
    mFlags.clear();
    mFlags.set(Transient);
    mIndex = (int16_t)index;
}

// The transient returns the edges it used in the last search, not its
// current best connections (kept separately).
GraphEdgeArray TransientNode::getEdges(GraphEdge*) const {
    return GraphEdgeArray((int32_t)mSearchEdges.size(), const_cast<GraphEdge*>(mSearchEdges.data()));
}

// The current best hooks.
GraphEdgeArray TransientNode::getHookedEdges() const {
    return GraphEdgeArray((int32_t)mEdges.size(), const_cast<GraphEdge*>(mEdges.data()));
}

// Lets the node traverser handle transients.
int32_t TransientNode::volumeIndex() const {
    if (mSaveClosest && mSaveClosest->indoor()) return mSaveClosest->volumeIndex();
    return -1;
}

// Remove the two hook nodes referenced by the index.
void NavigationGraph::destroyPairOfHooks(int32_t pairLookup) {
    for (int32_t i = 0; i < 2; i++) {
        delete mNodeList[(size_t)(pairLookup + i)];
        mNodeList[(size_t)(pairLookup + i)] = nullptr;
    }
}

int32_t NavigationGraph::createPairOfHooks() {
    int32_t findPair = mMaxTransients;
    while ((findPair -= 2) >= 0)
        if (!mNodeList[(size_t)(mTransientStart + findPair)]) break;
    if (findPair < 0) {
        // AssertISV: fatal in the shipping engine too.
        Console::instance().printf(LogLevel::Error, "Out of dynamic graph connections (bot max exceeded?)");
        std::abort();
    }
    findPair += mTransientStart;
    for (int32_t i = 0; i < 2; i++) mNodeList[(size_t)(findPair + i)] = new TransientNode(findPair + i);
    return findPair;
}

// Transients point INTO the graph; the graph hooks back only for a search.
// The search edges are the hooked ones (the last path edge may cross a
// border, reflected: they come in pairs).
int32_t NavigationGraph::hookTransient(TransientNode& transient) {
    int32_t retIsland = -7;
    const int32_t thisIndex = transient.getIndex();
    GraphEdgeArray edgeList = transient.getHookedEdges();

    transient.mSearchEdges.clear();
    transient.mSaveClosest = transient.mClosest;
    transient.mFirstHook = nullptr;

    while (GraphEdge* edge = edgeList++) {
        GraphNode* hookTo = lookupNode(edge->mDest);
        const int32_t hookIsland = hookTo->mIsland;
        if (retIsland == -7 || hookIsland == retIsland) retIsland = hookIsland;

        // The first hook represents the transient for partition reachability.
        if (!transient.mFirstHook && !hookTo->transient()) transient.mFirstHook = hookTo;

        if (GraphEdge* edgeBack = hookTo->pushTransientEdge(thisIndex)) {
            if (edge->isBorder()) edgeBack->mBorder = (int16_t)(edge->mBorder ^ 1);
            if (edge->isJetting()) edgeBack->setJetting();
            if (edge->getTeam()) edgeBack->setTeam(edge->getTeam());
        }
        transient.mSearchEdges.push_back(*edge);
    }
    // restore the regular list.
    transient.mEdges = transient.mSearchEdges;
    return (transient.mIsland = (int16_t)retIsland);
}

void NavigationGraph::unhookTransient(TransientNode& transient) {
    GraphEdgeArray edgeList = transient.getEdges(nullptr);
    while (GraphEdge* edge = edgeList++) lookupNode(edge->mDest)->popTransientEdge();
}

// Balanced calls for hooking in a pair of transients. Returns whether the
// two can find each other (or it's ambiguous), by islands and partitions.
bool NavigationGraph::pushTransientPair(TransientNode& srcNode, TransientNode& dstNode, uint32_t team,
                                        const JetManager::ID& jetCaps) {
    const int32_t island0 = hookTransient(dstNode);
    const int32_t island1 = hookTransient(srcNode);
    if (island0 == island1 && island0 >= 0) {
        const int32_t srcInd = srcNode.mFirstHook->getIndex();
        const int32_t dstInd = dstNode.mFirstHook->getIndex();
        // The armor partition never answers ambiguous; force fields can (the
        // search then tries, and adds to the partition list on failure).
        GraphPartition::Answer answer = mJetManager.reachable(jetCaps, srcInd, dstInd);
        if (answer == GraphPartition::CanReach) {
            answer = mForceFields.reachable(team, srcInd, dstInd);
            if (answer == GraphPartition::CanReach || answer == GraphPartition::Ambiguous) return true;
        }
    }
    return false;
}

void NavigationGraph::popTransientPair(TransientNode& srcNode, TransientNode& dstNode) {
    unhookTransient(srcNode);
    unhookTransient(dstNode);
}

bool NavigationGraph::canReachLoc(const FindGraphNode& src, const FindGraphNode& dst, uint32_t team,
                                  const JetManager::ID& jetCaps) {
    GraphNode* srcNode = src.closest();
    GraphNode* dstNode = dst.closest();
    if (srcNode && dstNode) {
        if (srcNode->island() == dstNode->island()) {
            const int32_t srcInd = srcNode->getIndex();
            const int32_t dstInd = dstNode->getIndex();
            GraphPartition::Answer ans = mJetManager.reachable(jetCaps, srcInd, dstInd);
            if (ans == GraphPartition::CanReach) {
                ans = mForceFields.reachable(team, srcInd, dstInd);
                if (ans == GraphPartition::CanReach || ans == GraphPartition::Ambiguous) return true;
            }
        }
    }
    return false;
}

// A path searcher's handle on a pair of transient nodes.
GraphHookRequest::~GraphHookRequest() {
    if (NavigationGraph::gotOneWeCanUse())
        if (gNavGraph->incarnation() == mIncarnation) gNavGraph->destroyPairOfHooks(mPairLookup);
}

// Called on mission cycle - basically a re-construct.
void GraphHookRequest::reset() {
    mIncarnation = -1;
    mPairLookup = -1;
}

TransientNode& GraphHookRequest::getHook(int32_t firstOrSecond) {
    if (mIncarnation != gNavGraph->incarnation()) {
        mPairLookup = gNavGraph->createPairOfHooks();
        mIncarnation = gNavGraph->incarnation();
    }
    return *static_cast<TransientNode*>(gNavGraph->lookupNode(mPairLookup + firstOrSecond));
}

bool GraphHookRequest::iGrowOld() const { return gNavGraph->incarnation() != mIncarnation; }

//-------------------------------------------------------------------------------------
//                            Partitions (graphPartition.cc)

// Builds the downhill partition from the regular one, if different.
void GraphPartition::setDownhill(const GraphPartition& downhill) {
    const uint32_t bytes = mPartition.getByteSize();
    if (std::memcmp(mPartition.getBits(), downhill.mPartition.getBits(), bytes)) {
        mCanJetDown = true;
        mDownhill.copy(downhill.mPartition);
    } else {
        mCanJetDown = false;
    }
}

// Whether the partition can answer if we can get from A to B.
GraphPartition::Answer GraphPartition::reachable(int32_t A, int32_t B) const {
    if (mPartition.test((uint32_t)A)) {
        if (mPartition.test((uint32_t)B) || (mCanJetDown && mDownhill.test((uint32_t)B))) return CanReach;
        return CannotReach;
    } else if (mPartition.test((uint32_t)B)) {
        return CannotReach;
    }
    return Ambiguous;
}

// After a search failed to reach its target: the new partition it found.
void PartitionList::pushPartition(const GraphPartition& partition) {
    push_back(GraphPartition());
    back().install(partition);
    back().setType(mType);
}

GraphPartition::Answer PartitionList::reachable(int32_t A, int32_t B) const {
    for (const GraphPartition& partition : *this) {
        const GraphPartition::Answer answer = partition.reachable(A, B);
        if (answer != GraphPartition::Ambiguous) return answer;
    }
    return GraphPartition::Ambiguous;
}

int32_t PartitionList::haveEntry(int32_t forNode) const {
    for (const GraphPartition& partition : *this)
        if (partition.test(forNode)) return true;
    return false;
}

//-------------------------------------------------------------------------------------
//                            Dijkstra (graphDijkstra.cc)

// Back-index entries for extracted and not-yet-queued nodes.
#define DefExtracted (1 << 30)
#define DefNotInQueue GraphQIndex(-1)
#define MaskExtracted (DefExtracted - 1)
#define NodeExtracted(x) (((x) & DefExtracted) && (x) > 0)
#define FlagExtracted(x) ((x) |= DefExtracted)
#define NotInQueue(x) ((x) == DefNotInQueue)

// The Q and Q indices are shared among all searchers.
GraphSearch::GraphSearch()
    : mQueue(gNavGraph->mSharedSearchLists.searchQ),
      mQIndices(gNavGraph->mSharedSearchLists.qIndices),
      mHeuristicsVec(gNavGraph->mSharedSearchLists.heuristics),
      mPartition(gNavGraph->mSharedSearchLists.partition),
      mHead(-1, -1.0f, -1.0f) {
    mHead.mTime = -1;
    mExtractNode = nullptr;
    mVisitOnExtract = true;
    mVisitOnRelax = true;
    mEarlyOut = true;
    mSearchDist = 0.0f;
    mTargetNode = -1;
    mSourceNode = -1;
    mIterations = 0;
    mRelaxCount = 0;
    mThreatSet = 0;
    mInformThreats = 0;
    mInformTeam = 0;
    mInformRatings = nullptr;
    mInProgress = false;
    mAStar = false;
    mTargetLoc = {0, 0, 0};
    initializeIndices();
    mHeuristicsPtr = nullptr;
    mRandomize = false;
}

bool GraphSearch::initializeIndices() {
    if (NavigationGraph::gotOneWeCanUse()) {
        if (gNavGraph->numNodesAll() != (int32_t)mQIndices.size()) {
            const int32_t totalNodes = gNavGraph->numNodesAll();
            mHeuristicsVec.resize((size_t)totalNodes);
            mQIndices.resize((size_t)totalNodes);
            mPartition.setSize(totalNodes);
            // Largest search includes just two transients
            mQueue.reserve(gNavGraph->numNodes() + 2);
            doLargeReset();
            return true;
        }
    }
    return false;
}

void GraphSearch::doSmallReset() {
    mPartition.clear();
    for (int32_t i = mQueue.size() - 1; i >= 0; i--) {
        const int32_t whichNode = mQueue[(uint32_t)i].mIndex;
        mQIndices[(size_t)whichNode] = DefNotInQueue;
        mHeuristicsVec[(size_t)whichNode] = 0;
    }
}

void GraphSearch::doLargeReset() {
    std::fill(mQIndices.begin(), mQIndices.end(), DefNotInQueue);
    std::fill(mHeuristicsVec.begin(), mHeuristicsVec.end(), 0.0f);
    mPartition.clear();
}

void GraphSearch::resetIndices() {
    if (!initializeIndices()) {
        if (mQueue.size()) {    // Compare size of search queue to total node count.
            const int32_t ratio = (int32_t)mQIndices.size() / mQueue.size();
            if (ratio > 7) {
                doSmallReset();
                return;
            }
        }
        doLargeReset();
    }
}

// Mainly for access to accumulated userdata on the nodes-
SearchRef* GraphSearch::getSearchRef(int32_t nodeIndex) {
    const int32_t queueInd = mQIndices[(size_t)nodeIndex];
    return &mQueue[(uint32_t)(queueInd & MaskExtracted)];
}

// A straight lookup using a Q index.
SearchRef* GraphSearch::lookupSearchRef(int32_t queueIndex) { return &mQueue[(uint32_t)(queueIndex & MaskExtracted)]; }

SearchRef* GraphSearch::insertSearchRef(int32_t nodeIndex, float dist, float sort) {
    const int32_t vecIndex = mQueue.size();
    mQIndices[(size_t)nodeIndex] = vecIndex;
    mQueue.insert(SearchRef(nodeIndex, dist, sort));
    return &mQueue[(uint32_t)vecIndex];
}

// The edge scale when randomizing; turned off after a point to limit the
// larger search expansion.
int32_t GraphSearch::getAvoidFactor() {
    if (mIterations > 80) {
        mRandomize = false;
    } else {
        // Note unsigned compare important -
        const uint32_t timeDiff = (mExtractNode->avoidUntil() - mCurrentSimTime);
        if (timeDiff < GraphMaxNodeAvoidMS) return mExtractNode->stuckAvoid() ? 400 : 25;
    }
    return 0;
}

float GraphSearch::calcHeuristic(const GraphEdge* edge) {
    float dist = mHeuristicsPtr[edge->mDest];
    if (dist == 0) {
        const auto* destNode = static_cast<const RegularNode*>(gNavGraph->lookupNode(edge->mDest));
        dist = len(destNode->rawLoc() - mTargetLoc);
        mHeuristicsPtr[edge->mDest] = dist;
    }
    return dist;
}

// Time along the edge (special searches play with it).
float GraphSearch::getEdgeTime(const GraphEdge* edge) {
    float edgeTime = (edge->mDist * edge->getInverse());
    // Jetting capabilities: edges that can't be traversed (not transient
    // connections: partition predictions must work out true).
    if (mJetRatings && edge->isJetting() && edge->mDest < mTransientStart)
        if (!edge->canJet(mJetRatings)) return SearchFailureAssure;
    // Edges only one team can go through, namely (working) force fields-
    if (mTeam && edge->getTeam()) {
        if (edge->getTeam() != mTeam)
            return SearchFailureAssure;
        else
            edgeTime *= 1.3f;
    }
    return edgeTime;
}

bool GraphSearch::runDijkstra() {
    mCurrentSimTime = navSimTimeMS();

    while (SearchRef* head = mQueue.head()) {
        // COPY of head since the list can move; visitors use it too.
        mHead = *head;
        mQueue.removeHead();

        // This means search failed
        if (mHead.mTime > SearchFailureThresh) break;

        mPartition.set(mHead.mIndex);

        if (mTargetNode == mHead.mIndex) {
            mSearchDist = mHead.mDist;
            break;
        }

        mExtractNode = gNavGraph->lookupNode(mHead.mIndex);
        if (mVisitOnExtract) onQExtraction();

        // Avoidance of nodes for randomize or for stuckness.
        const int32_t avoidThisNode = (mRandomize ? getAvoidFactor() : 0);
        const bool nodeThreatened = (mThreatSet && (mExtractNode->threats() & mThreatSet));

        // Mark extracted (after onQExtraction()).
        const GraphQIndex headIndex = mQIndices[(size_t)mHead.mIndex];
        FlagExtracted(mQIndices[(size_t)mHead.mIndex]);

        // Relax all neighbors (or add to the queue in the first place)
        GraphEdgeArray edgeList = mExtractNode->getEdges(mEdgeBuffer);
        while (GraphEdge* edge = edgeList++) {
            const GraphQIndex queueInd = mQIndices[(size_t)edge->mDest];
            SearchRef* searchRef;
            if (!NodeExtracted(queueInd)) {
                const float newDist = mHead.mDist + edge->mDist;
                float edgeTime = getEdgeTime(edge);
                if (nodeThreatened) edgeTime *= 10;
                if (avoidThisNode) edgeTime += avoidThisNode;
                const float newTime = mHead.mTime + edgeTime;
                const float sortOnThis = (mAStar ? newTime + calcHeuristic(edge) : newTime);

                if (NotInQueue(queueInd)) {
                    searchRef = insertSearchRef(edge->mDest, newDist, sortOnThis);
                    searchRef->mPrev = (int16_t)headIndex;
                    searchRef->mTime = newTime;
                } else if (sortOnThis < (searchRef = &mQueue[(uint32_t)queueInd])->mSort) {
                    searchRef->mTime = newTime;
                    searchRef->mSort = sortOnThis;
                    searchRef->mDist = newDist;
                    searchRef->mPrev = (int16_t)headIndex;
                    mQueue.changeKey(queueInd);
                } else {
                    continue;   // didn't relax - must skip relax callback
                }
                mRelaxCount++;
                if (mVisitOnRelax) onRelaxEdge(edge);
            }
        }

        mIterations++;
        if (mEarlyOut && earlyOut()) return (mInProgress = true);
    }
    return (mInProgress = false);
}

void GraphSearch::initSearch(GraphNode* S, GraphNode* D) {
    mIterations = 0;
    mRelaxCount = 0;
    mInProgress = true;
    mSearchDist = 0.0f;
    mTransientStart = gNavGraph->numNodes();
    mSourceNode = S->getIndex();

    // A* requires a target.
    if (D)
        mTargetNode = D->getIndex(), mTargetLoc = D->location();
    else
        mTargetNode = -1, mInformAStar = false;

    // These hold their value for only one search-
    mAStar = mInformAStar;       mInformAStar = false;
    mThreatSet = mInformThreats; mInformThreats = 0;
    mTeam = mInformTeam;         mInformTeam = 0;
    mJetRatings = mInformRatings; mInformRatings = nullptr;

    mHeuristicsPtr = mHeuristicsVec.data();

    resetIndices();

    // The source is the first element in the Q-
    mQueue.clear();
    insertSearchRef(S->getIndex(), 0, 0)->mTime = 0;
    mQueue.buildHeap();
}

// The node indices of the search that just happened, back from the target;
// whether the target was reached.
bool GraphSearch::getPathIndices(std::vector<int32_t>& indices, int32_t target) {
    indices.clear();
    if (target < 0) target = mTargetNode;
    if (mPartition.test(target)) {
        // Queue indices: 0 is the source (first in the Q), hence while (prev).
        int32_t prev = (mQIndices[(size_t)target] & MaskExtracted);
        if (prev < (int32_t)mQIndices.size()) {
            while (prev) {
                indices.push_back(mQueue[(uint32_t)prev].mIndex);
                prev = mQueue[(uint32_t)prev].mPrev;
            }
            indices.push_back(mSourceNode);
            reverseVec(indices);
            return true;
        }
    }
    return false;
}

// D can be NULL (visitation expansions).
int32_t GraphSearch::performSearch(GraphNode* S, GraphNode* D) {
    if (D && D->island() != S->island()) return -1;
    initSearch(S, D);
    runDijkstra();
    return mIterations;
}

// A search that can be interrupted: true while in progress.
bool GraphSearch::runSearch(GraphNode* S, GraphNode* D) {
    if (D && D->island() != S->island()) return false;
    if (!mInProgress) initSearch(S, D);
    return runDijkstra();
}

//-------------------------------------------------------------------------------------
//                            LOS searches (graphSearchLOS.cc)

// Does much of the work for the hiding (two types), LOS acquiring and choke
// point queries.
void GraphSearchLOS::onQExtraction() {
    GraphNode* extNode = extractedNode();
    mExtractLoc = extNode->location();
    mExtractInd = extNode->getIndex();
    setOnRelax(false);

    if (mLOSTable && !extNode->transient()) {
        if (mSeekingLOS) {
            if (!mNeedInside || lenSquared(mLOSLoc - mExtractLoc) < mFarDist) {
                // LOS if we're looking for it (the table has no same entry);
                // keep the best entry relative to the search sphere.
                if (mExtractInd != mLOSKey) {
                    if (mLOSTable->muzzleLOS(mLOSKey, mExtractInd)) {
                        if (mNearSphere) {
                            // In the band outside this sphere: take it, else
                            // keep looking but record the best found.
                            if (mOuterSphere.isContained(mExtractLoc)) {
                                setEarlyOut(true);
                                setTarget(mExtractInd);
                            } else {
                                const float dSqrd = lenSquared(mExtractLoc - mNearSphere->center);
                                if (dSqrd < mBestDistSqrd) {
                                    setTarget(mExtractInd);
                                    mBestDistSqrd = dSqrd;
                                }
                            }
                        } else {
                            // No sphere: the first LOS point found
                            setEarlyOut(true);
                            setTarget(mExtractInd);
                        }
                    }
                }
            }
        } else { // LOS Avoid (find hidden / choke point query)-
            if (mExtractInd != mLOSKey) {
                if (mAvoidingLOS) {
                    if (mLOSTable->hidden(mLOSKey, mExtractInd)) {
                        const float distHidden = getSearchRef(mExtractInd)->mUser.f;
                        if (mDistanceBased) {
                            if (distHidden > mMinHidden) {
                                setEarlyOut(true);
                                setTarget(mExtractInd);
                            } else {
                                // Propagate distance for hidden nodes that
                                // aren't the solution (in the relax virtual).
                                setOnRelax(true);
                                if (distHidden > mBestHidden) {
                                    mBestHidden = distHidden;
                                    setTarget(mExtractInd);
                                }
                            }
                        } else {
                            // Hide on slope: better than the minimum, or the
                            // best found. Small 2d lengths (indoors) are
                            // indeterminate.
                            Point3F V{mExtractLoc.x - mLOSLoc.x, mExtractLoc.y - mLOSLoc.y, 0};
                            const float L = len(V);
                            const float dot = (L > 0.001f ? mDot(extNode->getNormal(), V *= (1.0f / L)) : -1.0f);
                            if (dot > mMinSlopeDot) {
                                setEarlyOut(true);
                                setTarget(mExtractInd);
                            } else if (dot > mBestSlopeDot) {
                                mBestSlopeDot = dot;
                                setTarget(mExtractInd);
                            }
                        }
                    }
                } else { // Choke point query
                    if (distSoFar() < mMaxChokeDist) {
                        // First-time obstructed nodes with lots of obstructed
                        // descendants: descendants update the ancestor's best.
                        if (mLOSTable->hidden(mLOSKey, mExtractInd)) {
                            setOnRelax(true);
                            if (mHead.mPrev >= 0) {
                                mChokePoints.push_back(mExtractInd);
                            } else {
                                mExtractInd = ~mHead.mPrev;   // Ones compliment has its uses!
                                SearchRef* ancestor = getSearchRef(mExtractInd);
                                if (mHead.mUser.f > ancestor->mUser.f) ancestor->mUser.f = mHead.mUser.f;
                            }
                        }
                    } else {
                        setEarlyOut(true);
                    }
                }
            }
        }
    }
}

// Propagate hidden distance to neighbors that are also hidden.
void GraphSearchLOS::onRelaxEdge(const GraphEdge* edge) {
    if (edge->mDest != mLOSKey) {
        if (mLOSTable->hidden(mLOSKey, edge->mDest)) {
            SearchRef* sref = getSearchRef(edge->mDest);
            const float hiddenDistSoFar = mHead.mUser.f;
            sref->mUser.f = (hiddenDistSoFar + edge->mDist);
            // For the choke point query, mPrev propagates the ancestor-
            if (mChokePtQuery) sref->mPrev = (int16_t)~mExtractInd;
        }
    }
}

// The choke point query filters out edges going down that have LOS to the
// key point (it wants the ones that don't).
float GraphSearchLOS::getEdgeTime(const GraphEdge* edge) {
    if (mChokePtQuery && edge->isJetting() && edge->isDown())
        if (edge->mDest != mLOSKey) {
            if (!mLOSTable->hidden(mLOSKey, edge->mDest)) {
                return SearchFailureAssure;
            } else {
                // Short hops from edges before longer lateral hops downward.
                const float lateral = JetManager::invertLateralMod(float(edge->getLateral()));
                const float truncLat = std::max(lateral - 4.0f, 0.0f);
                return truncLat * truncLat;
            }
        }
    return Parent::getEdgeTime(edge);
}

void GraphSearchLOS::setNullDefaults() {
    setEarlyOut(false);
    mLOSTable = nullptr;
    mLOSLoc = {-1, -1, -1};
    mAvoidingLOS = mSeekingLOS = mNeedInside = mChokePtQuery = false;
    mDistanceBased = true;
    mMinSlopeDot = -1.0f;
    mBestSlopeDot = -1.0f;
    mFarDist = 1e22f;
    mThreatCount = 0;
    mMaxChokeDist = 500;
    mBestHidden = 0.0f;
    mMinHidden = 22.0f;
    mNearSphere = nullptr;
    setOnRelax(false);
}

Point3F GraphSearchLOS::findLOSLoc(const Point3F& from, const Point3F& wantToSee, float minDist,
                                   const SphereF& getCloseTo, float capDist) {
    setNullDefaults();
    if (GraphNode* losNode = gNavGraph->closestNode(wantToSee)) {
        if (GraphNode* sourceNode = gNavGraph->closestNode(from)) {
            // Configure the search for LOS seeking
            mLOSKey = losNode->getIndex();
            mLOSLoc = losNode->location();
            mLOSTable = gNavGraph->getLOSXref();
            mNeedInside = (mFarDist = capDist) < 1e6f;
            mFarDist *= mFarDist;
            (mOuterSphere = *(mNearSphere = &getCloseTo)).radius += (mNearWidth = 10);
            mBestDistSqrd = 1e13f;
            mSeekingLOS = true;

            gNavGraph->threats()->pushTempThreat(losNode, wantToSee, minDist);
            GraphSearch::performSearch(sourceNode, nullptr);
            gNavGraph->threats()->popTempThreat();

            if (getTarget() >= 0) {
                getPathIndices(gNavGraph->tempNodeBuff());
                return gNavGraph->lookupNode(getTarget())->location();
            }
        }
    }
    return from;
}

Point3F GraphSearchLOS::hidingPlace(const Point3F& from, const Point3F& avoid, float avoidRad,
                                    float basisForAvoidance, bool avoidOnSlope) {
    if (GraphNode* losNode = gNavGraph->closestNode(avoid)) {
        if (GraphNode* sourceNode = gNavGraph->closestNode(from)) {
            setNullDefaults();
            mLOSKey = losNode->getIndex();
            mLOSLoc = losNode->location();
            mLOSTable = gNavGraph->getLOSXref();
            mAvoidingLOS = true;

            // Avoid on distance (beyond the first hidden point), or on slope.
            if (avoidOnSlope) {
                mDistanceBased = false;
                mMinSlopeDot = std::cos(basisForAvoidance);
            } else {
                mDistanceBased = true;
                mMinHidden = basisForAvoidance;
            }

            // Run with the avoid center temporarily affected.
            gNavGraph->threats()->pushTempThreat(losNode, avoid, avoidRad);
            GraphSearch::performSearch(sourceNode, nullptr);
            gNavGraph->threats()->popTempThreat();

            if (getTarget() >= 0) {
                getPathIndices(gNavGraph->tempNodeBuff());      // (debug rendering)
                return gNavGraph->lookupNode(getTarget())->location();
            }
        }
    }
    return from;
}

// A choke point is just barely out of LOS from the source, with a long
// unbroken hidden string of nodes beyond (so not around poles or into
// alcoves). Uses SearchRef::mPrev, not needed since no path is found.
int32_t GraphSearchLOS::findChokePoints(GraphNode* S, std::vector<Point3F>& points, float minHideDist,
                                        float maxSearchDist) {
    setNullDefaults();
    points.clear();
    if ((mLOSTable = gNavGraph->getLOSXref()) != nullptr) {
        mLOSKey = S->getIndex();
        mLOSLoc = S->location();
        mMaxChokeDist = (maxSearchDist + minHideDist);
        mMinHidden = minHideDist;
        mChokePoints.clear();
        mChokePtQuery = true;

        GraphSearch::performSearch(S, nullptr);

        GraphNodeList downList;
        for (int32_t nodeInd : mChokePoints) {
            SearchRef* searchRef = getSearchRef(nodeInd);
            if (searchRef->mUser.f > minHideDist) {
                // Nodes down a jet connection are filtered, EXCEPT that the
                // tops of those are kept in downList in case few are found.
                const int32_t beforeIndex = lookupSearchRef(searchRef->mPrev)->mIndex;
                GraphNode* beforeNode = gNavGraph->lookupNode(beforeIndex);
                GraphEdge* edgeTo = beforeNode->getEdgeTo(nodeInd);
                if (edgeTo->isJetting() && edgeTo->isDown())
                    downList.addUnique(beforeNode);
                else
                    points.push_back(gNavGraph->lookupNode(nodeInd)->location());
            }
        }
        // Few points: use some of the locations just before a downward jet.
        while (!downList.empty() && points.size() < 4) {
            points.push_back(downList.back()->location());
            downList.pop_back();
        }
    }
    return (int32_t)points.size();
}

//-------------------------------------------------------------------------------------
//                            Locating (graphLocate.cc)

#define WaitAfterNearbyFound 11
#define WaitAfterNothingFound 16

bool NodeProximity::possible() const { return mLateral < 4.0f; }

// APPROXIMATION: dQsort is replaced by std::sort (candidates with equal
// proximity may come out in a different order).
void ProximateList::sort() {
    std::sort(begin(), end(), [](const ProximateNode& a, const ProximateNode& b) {
        return a.mProximity.mLateral < b.mProximity.mLateral;
    });
}

// "Dist" searcher is a misnomer: a depth searcher, only for the locator.
GraphSearchDist::GraphSearchDist() {
    mBestMatch = nullptr;
    mPoint = {0, 0, 0};
    mMaxSearchDist = 100;
    setOnDepth(false);
}

// For the locator, search out to a certain DEPTH (a few connections removed).
float GraphSearchDist::getEdgeTime(const GraphEdge* edge) {
    if (mSearchOnDepth) return 1.001f;
    return edge->mDist;
}

void GraphSearchDist::onQExtraction() {
    GraphNode* extractNode = extractedNode();
    if (extractNode->indoor()) {
        NodeProximity metric = extractNode->containment(mPoint);
        if (metric.inside()) {
            mBestMatch = extractNode;
            mBestMetric = metric;
            mProximate.clear();
            setEarlyOut(true);
        } else {
            // Track any really close matches, disregarding the high ones.
            if (metric.possible())
                if (metric < mCurThreshold) {
                    mProximate.push_back(ProximateNode(metric, extractNode));
                    if (metric < mBestMetric) {
                        mBestMetric = metric;
                        mCurThreshold = std::max(float(metric), 1.0f);
                    }
                }
        }
        const float thusFar = (mSearchOnDepth ? timeSoFar() : distSoFar());
        if (thusFar > mMaxSearchDist) setEarlyOut(true);
    }
}

// The best indoor node to connect to, and how good (lower is better).
NodeProximity GraphSearchDist::findBestMatch(GraphNode* current, const Point3F& loc) {
    mBestMatch = nullptr;
    mBestMetric.makeBad();
    mProximate.clear();
    if (!current) current = gNavGraph->closestNode(loc);
    if (current) {
        setEarlyOut(false);
        mPoint = loc;
        mCurThreshold = 1e13f;
        if (current->indoor())
            setDistCap(3.3f);
        else
            setDistCap(2.2f);
        setOnDepth(true);
        performSearch(current);
        setOnDepth(false);
    }
    return mBestMetric;
}

GraphLocate::GraphLocate() {
    reset();
    mMounted = false;
}

void GraphLocate::reset() {
    mLocation = {1e13f, 1e13f, 1e13f};
    mLoc2D = mLocation;
    mTraveled = 1e13f;
    mClosest = nullptr;
    mCounter = 0;
    mTerrain = false;
    mUpInAir = false;
}

void GraphLocate::forceCheck() {
    mCounter = 0;
    mTraveled = 1e13f;
}

namespace {

constexpr uint32_t sLocateMask = 1u << 3;          // InteriorObjectType
constexpr uint32_t sLocateTerrain = 1u << 2;       // TerrainObjectType

bool haveLOS(Point3F src, Point3F dst, bool indoor) {
    NavRayInfo coll;
    src.z += 0.13f;
    dst.z += 0.13f;
    // Terrain matters in some proximity checks to indoor nodes.
    const uint32_t mask = indoor ? (sLocateMask | sLocateTerrain) : sLocateMask;
    return !navCastRay(src, dst, mask, coll);
}

// A slight raise of the lower point, up to step height (Masada bunker
// generators, top of the steps, fail LOS).
bool checkLOS(const Point3F& src, const Point3F& dst, bool indoor) {
    if (!haveLOS(src, dst, indoor)) {
        Point3F high, low;
        if (src.z < dst.z)
            low = src, high = dst;
        else
            low = dst, high = src;
        low.z += std::min(0.7f, high.z - low.z);
        return haveLOS(low, high, indoor);
    }
    return true;
}

// A proximity that sorts first.
NodeProximity bestProximity() {
    NodeProximity p;
    p.makeGood();
    return p;
}

} // namespace

bool GraphLocate::canHookTo(const GraphLocate& other, bool& mustJet) const {
    bool canHook = false;
    if (mClosest && other.mClosest) {
        if (onTerrain() && other.onTerrain())
            canHook = (mClosest == other.mClosest || mClosest->neighbors(other.mClosest));
        else
            canHook = (mClosest == other.mClosest);
    }
    if (canHook) mustJet = (mUpInAir || other.mUpInAir);
    return canHook;
}

void GraphLocate::setMounted(bool b) {
    if (mMounted != b) {
        mMounted = b;
        mClosest = nullptr;
        forceCheck();
    }
}

// A 2D check (established Ok in 3D, no jet connections): heavies are far
// off the edge in Z on slopes.
bool GraphLocate::checkRegularEdge(Point3F fromLoc, GraphEdge* edge) {
    GraphNode* destNode = gNavGraph->lookupNode(edge->mDest);
    Point3F destLoc = destNode->location();
    destLoc.z = fromLoc.z = 0.0f;
    LineSegment edgeSeg(fromLoc, destLoc);
    //==> For now the walking width is hard coded.
    return edgeSeg.distance(mLoc2D) <= 0.8f;
}

ProximateNode* GraphLocate::examineCandidates(ProximateList& proximate) {
    ProximateNode* bestIndoor = nullptr;
    ProximateNode* bestOutdoor = nullptr;
    mConnections.clear();
    if (!proximate.empty()) {
        proximate.sort();
        for (ProximateNode& i : proximate) {
            const bool isIndoor = i.mNode->indoor();
            if (checkLOS(i.mNode->location(), mLocation, isIndoor)) {
                pushEdge(i.mNode);
                if (bestIndoor == nullptr) {
                    if (isIndoor)
                        bestIndoor = &i;
                    else
                        bestOutdoor = &i;
                }
            }
            if (mConnections.size() > 3) break;
        }
    }
    // A final jetting connection, only for the best match.
    if (mMounted && !mConnections.empty()) {
        mConnections[0].mBorder = -1;
        mConnections[0].setJetting();
    }
    return bestIndoor ? bestIndoor : bestOutdoor;
}

// Neighbors onto the list (we're inside the node): all except certain
// straight line edges; careful with jet, border and walk edges.
void GraphLocate::getNeighbors(GraphNode* ofNode, bool outdoor, bool makeJet) {
    static GraphEdge edgeBuffer[MaxOnDemandEdges];
    GraphEdgeArray edgeList = ofNode->getEdges(edgeBuffer);
    const int32_t outdoorNumber = gNavGraph->numOutdoor();
    const Point3F fromLoc = ofNode->location();

    if (outdoor) {
        while (GraphEdge* edge = edgeList++) {
            if (edge->mDest < outdoorNumber) {
                GraphEdge connection = *edge;
                if (!connection.isSteep()) {
                    if (makeJet || connection.isJetting()) connection.setJetting();
                    mConnections.push_back(connection);
                }
            } else {
                // Near the edge? (terrain nodes have no 'border' edges out)
                if (!edge->isJetting() && checkRegularEdge(fromLoc, edge)) mConnections.push_back(*edge);
            }
        }
    } else {
        // From an indoor node we're inside.
        while (GraphEdge* edge = edgeList++) {
            if (edge->mDest >= outdoorNumber) {  // (indoor dest)
                if (makeJet) {
                    // Inside the neighbor's top and bottom planes: a jet connection.
                    if (gNavGraph->verticallyInside(edge->mDest, mLocation)) {
                        GraphNode* destNode = gNavGraph->lookupNode(edge->mDest);
                        if (gNavGraph->possibleToJet(mLocation, destNode->location())) {
                            GraphEdge jetEdge;
                            jetEdge.mDest = edge->mDest;
                            jetEdge.setJetting();
                            jetEdge.mDist = edge->mDist;
                            jetEdge.copyInverse(edge);
                            mConnections.push_back(jetEdge);
                        }
                    }
                } else {
                    if (!edge->isJetting())
                        if (edge->isBorder() || checkRegularEdge(fromLoc, edge)) mConnections.push_back(*edge);
                }
            } else {  // Neighbor is outdoor-
                if (makeJet) {
                    // Above the terrain, but maybe under the top of a shaded node.
                    GraphNode* destNode = gNavGraph->lookupNode(edge->mDest);
                    const Point3F destLoc = destNode->location();
                    const float topZ = (destLoc.z + destNode->terrHeight());
                    if (mLocation.z < topZ) {
                        if (gNavGraph->possibleToJet(mLocation, destLoc)) {
                            mConnections.push_back(*edge);
                            mConnections.back().setJetting();
                        }
                    }
                } else {
                    if (!edge->isJetting() && checkRegularEdge(fromLoc, edge)) mConnections.push_back(*edge);
                }
            }
        }
    }
}

void GraphLocate::pushEdge(GraphNode* to, float* getCosine) {
    GraphEdge edge;
    edge.mDest = (int16_t)to->getIndex();
    Point3F toLoc = to->location();
    edge.mDist = len(toLoc -= mLocation);
    // Steepness detection (LH 11/12/00).
    if (getCosine) {
        if (edge.mDist < 0.01f) {
            *getCosine = 1.0f;
        } else {
            toLoc.z = 0;
            *getCosine = (len(toLoc) / edge.mDist);
        }
    }
    mConnections.push_back(edge);
}

// We're inside this one: hook to it and all its neighbors.
void GraphLocate::beLikeNode(GraphNode* ourNode) {
    mConnections.clear();
    bool tryJet = false;
    const bool outdoor = !ourNode->indoor();
    if (mMounted) {
        if (outdoor) {
            float height;
            if (gNavGraph->terrainHeight(mLocation, &height)) tryJet = (mLocation.z - height) > 2.0f;
        } else {
            // Inside: just the height above the floor.
            if (gNavGraph->heightAboveFloor(ourNode->getIndex(), mLocation) > 2.0f) tryJet = true;
        }
    }
    getNeighbors(mClosest = ourNode, outdoor, tryJet);

    float cosAngle = 1.0f;
    pushEdge(mClosest, outdoor ? &cosAngle : nullptr);
    if (tryJet || cosAngle < gNavGlobs.mPrettySteepDot) mConnections.back().setJetting();
    mTraveled = 0.0f;
    mCounter = 0;
    mUpInAir = tryJet;
}

void GraphLocate::update(const Point3F& newLoc) {
    const bool searchIsReady = ((--mCounter < 0) && (mTraveled > 0));
    const float moveDist = len(mLocation - newLoc);
    mTraveled += moveDist;

    // Big jumps - redo the search.
    if (moveDist > 5.0f) mClosest = nullptr;

    if ((moveDist > 0.1f) || searchIsReady) {
        mLocation = newLoc;
        mLoc2D = {mLocation.x, mLocation.y, 0.0f};
        mUpInAir = false;

        if (GraphNode* onTerr = gNavGraph->findTerrainNode(mLocation)) {
            mTerrain = true;
            mMetric.makeBad();
            beLikeNode(onTerr);
        } else {
            mTerrain = false;
            NodeProximity newMetric;
            // A good match: search as soon as we leave the node. Moving
            // inside a node we were near remakes the connections.
            if (mClosest && (newMetric = mClosest->containment(mLocation)).inside()) {
                if (!mMetric.inside()) {
                    beLikeNode(mClosest);
                } else if (mCounter < 0) {
                    // Re-evaluate 'straight-line' edge connections every so often.
                    if (mCounter < -4) beLikeNode(mClosest);
                } else {
                    mCounter = 0;
                }
                mMetric = newMetric;
            } else if (searchIsReady) {
                GraphSearchDist* searcher = gNavGraph->getDistSearcher();
                mMetric = searcher->findBestMatch(mClosest, mLocation);
                if (searcher->bestMatch()) {
                    beLikeNode(searcher->bestMatch());
                } else {
                    // The proximate list.
                    ProximateList& nearby = searcher->getProximate();
                    if (GraphNode* terr = gNavGraph->nearbyTerrainNode(mLocation))
                        nearby.push_back(ProximateNode(bestProximity(), terr));

                    if (ProximateNode* bestBet = examineCandidates(nearby)) {
                        mClosest = bestBet->mNode;
                        mCounter = WaitAfterNearbyFound;
                        mTraveled = std::max(float(bestBet->mProximity), 0.0f);
                    } else {
                        // Nothing found: the travel distance isn't reset, which
                        // widens the search gradually.
                        mClosest = nullptr;
                        mCounter = WaitAfterNothingFound;
                    }
                    mCounter += (int32_t)(Nav::gRandGen().randI() & 3);     // Little bit of stagger
                }
            }
        }
    }
}
