// The NavigationGraph run time: loading (ai/graph.cc), the run-time node
// graph (graphOutdoors.cc, graphIndoors.cc, graphIsland.cc, graphMake.cc
// doFinalFixups / makeGraph / findJumpableNodes, graphBridge.cc
// pushBridges), volume queries (graphVolume.cc), node finding
// (graphFind.cc), the static queries (graphQueries.cc) and the spawn
// queries (graphSpawn.cc).
#include "ai/graph.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/containers.h"
#include "sim/force_field.h"
#include "sim/nav_graph.h"
#include "sim/server_container.h"
#include "sim/sim_state.h"
#include <cmath>
#include <cstdio>
#include <strings.h>

using namespace NavMath;

NavigationGraph* gNavGraph = nullptr;
int32_t NavigationGraph::mIncarnation = 0;
int32_t NavigationGraph::sTotalEdgeCount = 0;
uint32_t NavigationGraph::sLoadMemUsed = 0;

namespace {

// game/objectTypes.h
constexpr uint32_t StaticObjectType = 1u << 0;
constexpr uint32_t TerrainObjectType = 1u << 2;
constexpr uint32_t InteriorObjectType = 1u << 3;
constexpr uint32_t ForceFieldObjectType = 1u << 8;
constexpr uint32_t StaticShapeObjectType = 1u << 13;

void conPrintf(const char* fmt, const char* a = "") {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), fmt, a);
    Console::instance().printf(LogLevel::Info, "%s", buffer);
}

std::string getGlobal(const char* name) {
    auto* ts = ScriptEngine::exists() ? ScriptEngine::instance().ts() : nullptr;
    return ts ? ts->getGlobal(name).toString() : std::string();
}
bool dAtob(const std::string& text) {
    return strcasecmp(text.c_str(), "true") == 0 || std::atof(text.c_str()) != 0.0;
}

// graph.cc getGraphName
std::string graphName(const std::string& mission, bool spawn) {
    return std::string("terrains/") + mission + (spawn ? ".spn" : ".nav");
}

// PathXRefTable::read: constructVector of PathXRefEntry (BitVectorW). The
// engine dropped the table (mValidPathTable is always false), so it is read
// past.
bool skipPathXRef(GraphStream& s) {
    int32_t n;
    if (!s.count(&n, 8)) return false;
    for (int32_t i = 0; i < n; i++) {
        uint32_t numEntries, bitWidth;
        if (!(s.read(&numEntries) && s.read(&bitWidth))) return false;
        // BitVectorW::setDims + numBytes
        const size_t bytes =
            (numEntries > 0 && bitWidth <= 17) ? (size_t)((((uint64_t)bitWidth * numEntries) + 32) >> 3) : 0;
        if (!s.skip(bytes)) return false;
    }
    return true;
}

} // namespace

uint32_t navSimTimeMS() { return (uint32_t)std::llround(SimState::simTime() * 1000.0); }

//-------------------------------------------------------------------------------------
// gServerContainer.castRay

bool navCastRay(const Point3F& a, const Point3F& b, uint32_t mask, NavRayInfo& info) {
    float best = 2.0f;
    Point3F normal{0, 0, 1};
    uint32_t hitType = 0;
    const Point3F lo{std::min(a.x, b.x) - 0.1f, std::min(a.y, b.y) - 0.1f, std::min(a.z, b.z) - 0.1f};
    const Point3F hi{std::max(a.x, b.x) + 0.1f, std::max(a.y, b.y) + 0.1f, std::max(a.z, b.z) + 0.1f};
    const Point3F d = b - a;
    // Moller-Trumbore over one kind of geometry.
    auto castTriangles = [&](const std::vector<PlayerPrediction::Triangle>& tris, uint32_t type) {
        for (const auto& t : tris) {
            const Point3F e1 = t.b - t.a, e2 = t.c - t.a;
            const Point3F p = mCross(d, e2);
            const float det = mDot(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            const Point3F sv = a - t.a;
            const float u = mDot(sv, p) / det;
            if (u < 0 || u > 1) continue;
            const Point3F q = mCross(sv, e1);
            const float v = mDot(d, q) / det;
            if (v < 0 || u + v > 1) continue;
            const float tt = mDot(e2, q) / det;
            if (tt < 0 || tt > 1 || tt >= best) continue;
            best = tt;
            normal = t.n;
            hitType = type;
        }
    };
    std::vector<PlayerPrediction::Triangle> tris;
    if (mask & (TerrainObjectType | StaticObjectType)) {
        ServerContainer::gatherGeometry(lo, hi, true, false, tris);
        castTriangles(tris, TerrainObjectType | StaticObjectType);
    }
    if (mask & (InteriorObjectType | StaticObjectType)) {
        tris.clear();
        ServerContainer::gatherGeometry(lo, hi, false, true, tris);
        castTriangles(tris, InteriorObjectType | StaticObjectType);
    }
    if (mask & ForceFieldObjectType) {
        // ForceFieldBare::castRay: a field blocks unless it is open.
        tris.clear();
        ForceFields::gather(nullptr, lo, hi, tris);
        castTriangles(tris, ForceFieldObjectType);
    }
    // Objects (and water): skip the container's object walk when nothing in
    // the mission has a type the mask asks for (the union of the types is
    // kept per sim millisecond and object count).
    const uint32_t objects = mask & ~(StaticObjectType | TerrainObjectType | InteriorObjectType | ForceFieldObjectType);
    if (objects && ScriptEngine::exists()) {
        static uint32_t sTypes = 0, sTime = ~0u;
        static size_t sCount = ~size_t(0);
        const auto& all = ScriptEngine::instance().objects;
        if (sTime != navSimTimeMS() || sCount != all.size()) {
            sTime = navSimTimeMS();
            sCount = all.size();
            sTypes = SimContainer::WaterObjectType;   // the container's water surface
            for (const auto& [key, object] : all)
                if (object && object->engine) sTypes |= SimContainer::typeMask(object);
        }
        SimContainer::RayInfo hit;
        if ((sTypes & objects) && SimContainer::castRay(a, b, objects, hit) && hit.t < best) {
            best = hit.t;
            normal = hit.normal;
            hitType = hit.objectType;
        }
    }
    if (best > 1.0f) return false;
    info.t = best;
    info.point = a + (b - a) * best;
    info.normal = normal;
    info.objectType = hitType;
    return true;
}

//-------------------------------------------------------------------------------------
// graphLOS.cc

uint32_t Loser::mCasts = 0;     // Just for informal profiling, counting casts.
static constexpr float scGraphStepCheck = 0.75f;

bool Loser::haveLOS(const Point3F& src, const Point3F& dst) {
    if (mCheckingFF) {
        // A force field encountered: walkable connections allow it, jettable
        // ones can't go through.
        mCasts++;
        mColl.objectType = 0;
        if (navCastRay(src, dst, mMask | ForceFieldObjectType, mColl)) {
            if (mColl.objectType & ForceFieldObjectType)
                mHitForceField = true;     // and fall through do normal LOS.
            else
                return false;
        }
    }
    mCasts++;
    mColl.objectType = 0;
    return !navCastRay(src, dst, mMask, mColl);
}

// Drop the point down to the collision below (unchanged, false, if none).
bool Loser::hitBelow(Point3F& drop, float down) {
    const Point3F below{drop.x, drop.y, drop.z - down};
    if (navCastRay(drop, below, mMask, mColl)) {
        drop = mColl.point;
        return true;
    }
    return false;
}

// Height above, capped at maxUp.
float Loser::heightUp(const Point3F& from, float maxUp) {
    const Point3F up{from.x, from.y, from.z + maxUp};
    if (haveLOS(from, up)) return maxUp;
    return mColl.point.z - from.z;
}

// LOS fanning one of the points (S-to-D was already checked).
bool Loser::fannedLOS1(const Point3F& S, const Point3F& D, const Point3F& inc, int32_t N) {
    Point3F D0 = D, D1 = D;
    while (N--)
        if (!haveLOS(S, D0 += inc) || !haveLOS(S, D1 -= inc)) return false;
    return true;
}

// LOS fanning both points.
bool Loser::fannedLOS2(const Point3F& S, const Point3F& D, const Point3F& inc, int32_t N) {
    Point3F D0 = D, D1 = D, S0 = S, S1 = S;
    while (N--)
        if (!haveLOS(S0 += inc, D0 += inc) || !haveLOS(S1 -= inc, D1 -= inc)) return false;
    return true;
}

// LOS checks downward looking for changes greater than step height.
bool Loser::walkOverBumps(Point3F S, Point3F D, float inc) {
    Point3F step = (D - S);
    int32_t N = std::max(int32_t(len(step) / inc), 2);
    // D becomes the stepper directly under S.
    const float downDist = 140;
    (D = S).z -= downDist;
    if (haveLOS(S, D)) return false;
    float zdown = mColl.point.z;
    step *= (1.0f / float(N));
    while (N--) {
        if (haveLOS(S += step, D += step)) return false;
        if (mColl.objectType & SimContainer::WaterObjectType) return false;
        const float getZ = mColl.point.z;
        if (std::fabs(getZ - zdown) > scGraphStepCheck) return false;
        // Save the Z, unless not walkable, in which case accumulate.
        if (mColl.normal.z > gNavGlobs.mWalkableDot) zdown = getZ;
    }
    return true;
}

// Fanned walk (SLOW), at less resolution.
bool Loser::fannedBumpWalk(const Point3F& S, const Point3F& D, Point3F inc, int32_t N) {
    Point3F D0 = D, D1 = D, S0 = S, S1 = S;
    inc *= 2.0f;
    while ((N -= 2) >= 0)
        if (!walkOverBumps(S0 += inc, D0 += inc) || !walkOverBumps(S1 -= inc, D1 -= inc)) return false;
    return true;
}

// Lines moved up looking for a free one, with at least ~1.5m of breathing
// room while there's none.
bool Loser::findHopLine(Point3F S, Point3F D, float maxUp, float& freeHt) {
    constexpr float sHopLookInc = 0.2f;
    const float tThresh = (1.5f / len(S - D));
    float total = 0;
    while (total < maxUp) {
        S.z += sHopLookInc;
        D.z += sHopLookInc;
        total += sHopLookInc;
        if (haveLOS(S, D)) {
            freeHt = total;
            return true;
        } else {
            if (mColl.t < tThresh) break;     // look for minimum amount in
            haveLOS(D, S);                    //    ... from both directions
            if (mColl.t < tThresh) break;
        }
    }
    return false;
}

bool Loser::walkOverGaps(Point3F S, Point3F D, float allowedGap) {
    constexpr float GapWalkInc = 0.15f;
    Point3F incVec = (D - S);
    float inc2D = len2D(incVec.x, incVec.y);
    int32_t N = std::max(int32_t(inc2D / GapWalkInc) + 1, 2);
    const float bandWid = gNavGlobs.mStepHeight;
    float saveL = 0, total2D = 0;
    bool wasBelow = false;
    // D is the stepper to do LOS down to
    (D = S).z -= 100;
    incVec *= (1.0f / float(N));
    inc2D *= (1.0f / float(N));
    while (N--) {
        const bool los = !navCastRay(S, D, mMask, mColl);
        // Must handle water-
        if (!los)
            if (mColl.objectType & SimContainer::WaterObjectType) return false;
        // Below the allowed band?
        if (los || mColl.point.z < (S.z - bandWid)) {
            if (wasBelow) {
                if (total2D - saveL > allowedGap) return false;
            } else {
                wasBelow = true;
                saveL = total2D;
            }
        } else {
            wasBelow = false;
            saveL = total2D;
        }
        S += incVec;
        D += incVec;
        total2D += inc2D;
    }
    return true;
}

//-------------------------------------------------------------------------------------

NavigationGraph::NavigationGraph() : mMaxTransients(AbsMaxBotCount * 2) {
    gNavGraph = this;
    sLoadMemUsed = 0;
}

NavigationGraph::~NavigationGraph() {
    if (mTransientStart != -1)
        for (int32_t j = 0; j < mMaxTransients; j++)
            if (GraphNode* transientNode = mNodeList[(size_t)(mTransientStart + j)]) delete transientNode;
    newIncarnation();       // (This deletes the searchers)
    if (gNavGraph == this) gNavGraph = nullptr;
}

void NavigationGraph::onAdd() {
    // GroundPlan::getTerrainObj(): the terrain the mission has created so far.
    ServerContainer::refresh();
    Point3F origin;
    float squareSize;
    mHaveTerrainBlock = ServerContainer::terrainBlock(origin, squareSize);

    // The engine's deadly liquid check is commented out (DMMNOTPRESENT).
    mDeadlyLiquid = false;
    mSubmergedScale = 3.7f;
    mShoreLineScale = 1.41f;

    bool forceNavLoad = dAtob(getGlobal("$GraphForceLoad"));
    // Need to insure presence of NAV due to how code works below...
    if (forceNavLoad && Nav::readFile(graphName(getGlobal("$CurrentMission"), false)).empty())
        forceNavLoad = false;
    if (dAtob(getGlobal("$OFFLINE_NAV_BUILD"))) forceNavLoad = true;

    // Outside single player, no bots means just the spawn graph; single
    // player needs the NAV.
    mIsSpawnGraph = false;
    if (!forceNavLoad && strcasecmp(getGlobal("$CurrentMissionType").c_str(), "SinglePlayer"))
        mIsSpawnGraph = !std::atoi(getGlobal("$HostGameBotCount").c_str());

    if (loadGraph()) {
        if (!mIsSpawnGraph) {
            makeGraph();
            pushBridges();
            clearLoadData();        // remove load data not needed at run time.
        }
    }
    // Not ported: Memory::getMemoryUsed accounting ("Memory consumed = %d").
}

bool NavigationGraph::loadForMission(const std::string& missionName, bool spawnGraph) {
    ServerContainer::refresh();
    Point3F origin;
    float squareSize;
    mHaveTerrainBlock = ServerContainer::terrainBlock(origin, squareSize);
    mDeadlyLiquid = false;
    mSubmergedScale = 3.7f;
    mShoreLineScale = 1.41f;
    mIsSpawnGraph = spawnGraph;
    if (!loadGraph(missionName)) return false;
    if (!mIsSpawnGraph) {
        makeGraph();
        pushBridges();
        clearLoadData();
    }
    return true;
}

void NavigationGraph::clearLoadData() {
    mEdgeInfoList.clear();
    mNodeInfoList.clear();
    mBridgeList.clear();
    mTerrainInfo.consolidated.clear();
    mTerrainInfo.shadowHeights.clear();
}

// Called when a new graph is made. Bots compare the incarnation to know their
// transient nodes and jet partitions are stale.
void NavigationGraph::newIncarnation() {
    mIncarnation = mIncarnation + 1;
    mMainSearcher.reset();
    mLOSSearcher.reset();
    mDistSearcher.reset();
    mJetManager.clear();
}

GraphSearch* NavigationGraph::getMainSearcher() {
    if (!mMainSearcher) mMainSearcher = std::make_unique<GraphSearch>();
    return mMainSearcher.get();
}

GraphSearchLOS* NavigationGraph::getLOSSearcher() {
    if (!mLOSSearcher) mLOSSearcher = std::make_unique<GraphSearchLOS>();
    return mLOSSearcher.get();
}

GraphSearchDist* NavigationGraph::getDistSearcher() {
    if (!mDistSearcher) mDistSearcher = std::make_unique<GraphSearchDist>();
    return mDistSearcher.get();
}

// i.e. for navigation (not spawning)
bool NavigationGraph::gotOneWeCanUse() { return gNavGraph && (gNavGraph->mNumOutdoor || gNavGraph->mNumIndoor); }

bool NavigationGraph::hasSpawnLocs() { return gNavGraph && !gNavGraph->mSpawnList.empty(); }

// The engine prints these in DEBUG builds only.
void NavigationGraph::warning(const char*) {}

//-------------------------------------------------------------------------------------
//    GRAPH LOAD

bool NavigationGraph::loadGraph() { return loadGraph(getGlobal("$CurrentMission")); }

bool NavigationGraph::loadGraph(const std::string& missionName) {
    bool Ok = false;
    const std::string fileName = graphName(missionName, mIsSpawnGraph);
    const std::vector<uint8_t> bytes = Nav::readFile(fileName);
    if (!bytes.empty()) {
        if (!load(bytes, mIsSpawnGraph))
            conPrintf("loadGraph: Failed to load '%s'", fileName.c_str());
        else
            Ok = true;
        if (Ok && !mIsSpawnGraph) {
            // NAV graph needs the spawn data.
            const std::string spawnName = graphName(missionName, true);
            const std::vector<uint8_t> spawnBytes = Nav::readFile(spawnName);
            if (!spawnBytes.empty()) {
                if (!load(spawnBytes, true)) conPrintf("Error loading spawn file into NAV graph");
            } else {
                conPrintf("loadGraph: NAV Ok, but SPN open failed (%s)", spawnName.c_str());
            }
        }
    } else {
        conPrintf("loadGraph: Couldn't open '%s' for read", fileName.c_str());
    }
    return Ok;
}

bool NavigationGraph::load(const std::vector<uint8_t>& bytes, bool isSpawn) {
    GraphStream s(bytes);
    bool Ok = s.read(&mVersion);
    // Some reserved-
    int32_t res;
    Ok &= (s.read(&res) && s.read(&res) && s.read(&res) && s.read(&res));

    // The reduced spawn list. Before load it signals the graph is just a
    // spawn list; after, a non-empty mSpawnList is the condition.
    if (mVersion >= BetterSpawnMode) {
        Ok &= mSpawnList.read(s);
        if (isSpawn) return Ok;
        mSpawnList.reset();
    }

    // Edges and nodes / volumes (one-to-one with the nodes).
    Ok &= readVector1(s, mEdgeInfoList, 60);
    Ok &= readVector1(s, mNodeInfoList, 20);
    Ok &= mNodeVolumes.read(s);

    // Outside-
    Ok &= mTerrainInfo.read(s);

    // Bridges- AssertISV in the engine ("Graph needs rebuilt for this mission").
    if (mVersion < TrimmedBridges) return false;
    Ok &= mBridgeList.read(s);

    // Tables-
    if (mVersion >= AddedPathTable) Ok &= skipPathXRef(s);
    if (mVersion >= AddedLOSTable) {
        if (mVersion >= RevisedLOSToHash) {
            Ok &= mLOSHashTable.read(s);
            mLOSTable = &mLOSHashTable;
        } else {
            // Not ported: the LOSXRefTable of graphs before RevisedLOSToHash
            // (converted to the hash on load); none ship.
            Ok = false;
        }
    }
    return Ok;
}

//-------------------------------------------------------------------------------------
//    Outdoor nodes (graphOutdoors.cc)

namespace {

// Visitor for making the run-time node list.
class UnrollOutdoorList : public GridVisitor {
protected:
    const GraphNodeList& mGrid;
    GraphNodeList& mListOut;
    int32_t getIndex(const GridArea& area) const { return mArea.getIndex(area.point); }

public:
    UnrollOutdoorList(const GridArea& world, const GraphNodeList& grid, GraphNodeList& list)
        : GridVisitor(world), mGrid(grid), mListOut(list) {}
    bool beforeDivide(const GridArea& R, int32_t level) override {
        if (GraphNode* node = mGrid[(size_t)getIndex(R)]) {
            if (node->getLevel() == level) {
                mListOut.push_back(node);
                return false;                             // stop the sub-divide
            }
        }
        return true;                                      // recurse further
    }
    bool atLevelZero(const GridArea& R) override {
        if (GraphNode* node = mGrid[(size_t)getIndex(R)]) mListOut.push_back(node);
        return true;
    }
};

// Points for hooking to the 12 neighbors, in multiples of half the square
// width: Left->Right, then Up.
const Point2I hookCorners[4] = {{0, 0}, {2, 0}, {0, 2}, {2, 2}};
const Point2I hookSides[8] = {{0, 0}, {1, 0}, {0, 0}, {0, 1}, {2, 0}, {2, 1}, {0, 2}, {1, 2}};

// Visitor to hook up the nodes.
class HookOutdoorNodes : public GridVisitor {
protected:
    const GraphNodeList& mGrid;
    const std::vector<uint8_t>& mNeighbors;

    // Hook the node; true if it was a DOWNWARD hook (down by ONE level),
    // which hooks back too. A single hook ACROSS returns false.
    bool hookBothWays(GraphNode* node, const Point2I& where) {
        const int32_t index = mArea.getIndex(where);
        if (index >= 0) {
            if (GraphNode* neighbor = mGrid[(size_t)index]) {
                int32_t srcLevel = node->getLevel();
                int32_t dstLevel = neighbor->getLevel();
                auto* from = static_cast<OutdoorNode*>(node);
                auto* to = static_cast<OutdoorNode*>(neighbor);
                // -1 and 0 are the same level here.
                if (srcLevel < 0) srcLevel = 0;
                if (dstLevel < 0) dstLevel = 0;
                if (srcLevel == dstLevel + 1) {
                    from->pushEdge(to);
                    to->pushEdge(from);
                    return true;
                } else if (srcLevel == dstLevel) {
                    from->pushEdge(to);
                }
            }
        }
        return false;
    }
    int32_t getIndex(const GridArea& R) const { return mArea.getIndex(R.point); }

public:
    HookOutdoorNodes(const GridArea& world, const GraphNodeList& grid, const std::vector<uint8_t>& neighbors)
        : GridVisitor(world), mGrid(grid), mNeighbors(neighbors) {}

    // Hook to all neighbors of lower or equal level (not back when equal:
    // they hook themselves).
    bool beforeDivide(const GridArea& R, int32_t level) override {
        GraphNode* node = mGrid[(size_t)getIndex(R)];
        if (node && node->getLevel() == level) {
            // All 12 neighbors: two on each side, and the four corners.
            int32_t cornerStep = 0, sideStep = 0;
            const int32_t halfWidth = (1 << (level - 1));
            for (int32_t y = -1; y <= 1; y++)
                for (int32_t x = -1; x <= 1; x++)
                    if (x || y) {
                        // Offset only for negative components.
                        const Point2I gridOffset{(x < 0) * x, (y < 0) * y};
                        if (x && y) {
                            Point2I cornerOff = hookCorners[cornerStep++];
                            (cornerOff *= halfWidth) += gridOffset;
                            hookBothWays(node, cornerOff += R.point);
                        } else {
                            for (int32_t adjacent = 0; adjacent < 2; adjacent++) {
                                Point2I sideOff = hookSides[sideStep * 2 + adjacent];
                                sideOff *= halfWidth;
                                sideOff += gridOffset;
                                sideOff += R.point;
                                if (!hookBothWays(node, sideOff)) break;
                            }
                            sideStep++;
                        }
                    }
            return false;       // Stop the subdivision
        }
        return true;            // Recurse further
    }

    // Hook to all neighbors; at this level each hooks itself.
    bool atLevelZero(const GridArea& R) override {
        const int32_t index = getIndex(R);
        if (GraphNode* node = mGrid[(size_t)index])
            for (int32_t dir = 0; dir < 8; dir++)
                if (mNeighbors[(size_t)index] & (1 << dir)) {
                    const int32_t x = TerrainGraphInfo::gridOffs[dir].x;
                    const int32_t y = TerrainGraphInfo::gridOffs[dir].y;
                    if (x && y) {
                        // Diagonals need valid nodes on either side of the line.
                        const int32_t ind1 = mArea.getIndex(R.point + Point2I{x, 0});
                        const int32_t ind2 = mArea.getIndex(R.point + Point2I{0, y});
                        if (ind1 >= 0 && ind2 >= 0 && mGrid[(size_t)ind1] && mGrid[(size_t)ind2])
                            hookBothWays(node, Point2I{x, y} + R.point);
                    } else {
                        hookBothWays(node, Point2I{x, y} + R.point);
                    }
                }
        return true;
    }
};

} // namespace

// The grid of node pointers in the area (consolidated squares point at
// their node over the whole square), the list, and the indices.
int32_t NavigationGraph::setupOutdoorNodes(const GridArea& area_in, const Consolidated& cons, GraphNodeList& grid_out,
                                           GraphNodeList& list_out) {
    mOutdoorNodes.assign(cons.size(), OutdoorNode());
    grid_out.assign((size_t)(area_in.len_x() * area_in.len_y()), nullptr);
    for (size_t i = 0; i < cons.size(); i++) {
        const OutdoorNodeInfo& nodeInfo = cons[i];
        OutdoorNode* outdoorNode = &mOutdoorNodes[i];

        const Point2I gridPoint = nodeInfo.getPoint();
        mTerrainInfo.posToLoc(outdoorNode->mLoc, gridPoint);
        int32_t level = nodeInfo.getLevel();
        outdoorNode->mLevel = (int8_t)level;

        const int32_t ind = mTerrainInfo.posToIndex(gridPoint);
        if (mTerrainInfo.shadowed(ind)) {
            outdoorNode->mHeight = mTerrainInfo.shadowHeight(ind);
            outdoorNode->set(GraphNode::Shadowed);
        } else {
            outdoorNode->mHeight = 1e17f;
            if (mTerrainInfo.submerged(ind)) outdoorNode->set(GraphNode::Submerged);
        }

        //==> Make this get the average normal.
        const int32_t gridShift = gNavGlobs.mSquareShift;
        const Point2F terrGridLoc{float(gridPoint.x << gridShift), float(gridPoint.y << gridShift)};
        if (!ServerContainer::terrainHeight(terrGridLoc, nullptr, &outdoorNode->mNormal, true))
            outdoorNode->mNormal = {0, 0, 1};

        if (level < 0) level = 0;

        // Fill in the grid with pointers to this node.
        const int32_t gridWidth = 1 << level;
        for (int32_t y = 0; y < gridWidth; y++)
            for (int32_t x = 0; x < gridWidth; x++) {
                const Point2I P = Point2I{x, y} + gridPoint;
                grid_out[(size_t)area_in.getIndex(P)] = outdoorNode;
            }

        Point3F middleOff{float(gridWidth << (gridShift - 1)), float(gridWidth << (gridShift - 1)), 0};
        if (level == 0) middleOff *= 0.0f;
        outdoorNode->mLoc += middleOff;
        if (!terrainHeight(outdoorNode->mLoc, &outdoorNode->mLoc.z)) {
            // Made obvious a bug that has been gone for quite a while.
            outdoorNode->mLoc.z = 120;
            warning("graphOutdoors.cc:  No terrain found in middle of grid node");
        }
    }

    // The unroll loop to put into the node list proper.
    UnrollOutdoorList listUnroller(area_in, grid_out, list_out);
    listUnroller.traverse();

    // Larger squares come first in the list.
    for (size_t i = 0; i < list_out.size(); i++) static_cast<OutdoorNode*>(list_out[i])->mIndex = (int16_t)i;
    return (int32_t)list_out.size();
}

// The run time consolidated nodes from the loaded data. The engine's edge
// pool (makeGraph(true)) is an allocation strategy: each node here keeps
// its own edge vector.
void NavigationGraph::makeRunTimeNodes() {
    const GridArea worldArea(mTerrainInfo.originGrid, mTerrainInfo.gridDimensions);
    HookOutdoorNodes hookingVisitor(worldArea, mNodeGrid, mTerrainInfo.neighborFlags);

    mOutdoorNodes.clear();
    mNumOutdoor = 0;
    mNodeList.clear();
    mNodeGrid.clear();
    mNodeList.reserve(mTerrainInfo.consolidated.size() + mNodeInfoList.size());

    // Makes the outdoor grid and starts the node list-
    if (haveTerrain()) mNumOutdoor = setupOutdoorNodes(worldArea, mTerrainInfo.consolidated, mNodeGrid, mNodeList);

    mIndoorNodes.assign(mNodeInfoList.size(), InteriorNode());

    // Hooks up the edges. (The engine walks the grid even without a terrain,
    // reading an empty grid; that case hooks nothing here.)
    if (haveTerrain()) hookingVisitor.traverse();

    // The interior nodes; the last param gives the starting index-
    initInteriorNodes(mEdgeInfoList, mNodeInfoList, (int32_t)mNodeList.size());
    mNumIndoor = (int32_t)mIndoorNodes.size();
    for (InteriorNode& in : mIndoorNodes) mNodeList.push_back(&in);

    findJumpableNodes();

    // The shoreline; pushed back more for lava so bots give it wider berth.
    expandShoreline(0);
    if (mDeadlyLiquid) expandShoreline(1);

    doFinalFixups();
}

// Sets the shoreline bit (edge scaling); callable repeatedly to widen it.
// Outdoor nodes and their outdoor neighbors only.
void NavigationGraph::expandShoreline(uint32_t wave) {
    const uint32_t extendFromMask = (GraphNode::ShoreLine << wave);
    const uint32_t extendToMask = (extendFromMask << 1);
    for (int32_t i = 0; i < mNumOutdoor; i++) {
        if (GraphNode* node = lookupNode(i)) {
            if (node->test(extendFromMask)) {
                GraphEdgeArray edges = node->getEdges(mEdgeBuffer);
                while (GraphEdge* edge = edges++) {
                    if (!edge->isJetting() && (edge->mDest < mNumOutdoor)) {
                        GraphNode* neighbor = lookupNode(edge->mDest);
                        if (!neighbor->test(extendFromMask)) neighbor->set(extendToMask);
                    }
                }
            }
        }
    }
}

int32_t OutdoorNode::getLevel() const { return int32_t(mLevel); }

OutdoorNode::OutdoorNode() {
    mIndex = -1;
    mLoc = {-1, -1, -1};
    mFlags.set(Outdoor);
}

// A random location within the node: at least 1 unit from the grid axes and
// 0.5 in from the sides.
Point3F OutdoorNode::randomLoc() const {
    Point3F loc = location();
    float* xy[2] = {&loc.x, &loc.y};
    const float R = radius();
    for (int32_t i = 0; i < 2; i++) {
        const float off = Nav::gRandGen().randF() * (R - 1.5f) + 1.0f;
        if (Nav::gRandGen().randI() & 1)
            *xy[i] += off;
        else
            *xy[i] -= off;
    }
    gNavGraph->terrainHeight(loc, &loc.z);
    loc.z += 1.0f;
    return loc;
}

//-------------------------------------------------------------------------------------
//    Indoor nodes (graphIndoors.cc)

NodeProximity InteriorNode::containment(const Point3F& loc) const {
    if (gNavGraph->haveVolumes()) return gNavGraph->getContainment(mIndex, loc);
    return Parent::containment(loc);
}

void InteriorNode::init(const IndoorNodeInfo& g, int32_t index, const Point3F& normal) {
    mIndex = (int16_t)index;
    mLoc = g.pos;
    mNormal = normal;
    mFlags.set(Inventory, g.isInventory());
    mFlags.set(Algorithmic, g.isAlgorithmic());
    mFlags.set(BelowPortal, g.isBelowPortal());
    mFlags.set(PotentialSeed, g.isSeed());
}

GraphEdge& InteriorNode::pushOneWay(const GraphEdgeInfo::OneWay& edgeIn) {
    GraphEdge edgeOn;
    edgeOn.mDest = (int16_t)edgeIn.dest;
    if (edgeIn.isJetting()) edgeOn.setJetting();
    mEdges.push_back(edgeOn);
    return mEdges.back();
}

InteriorNode::InteriorNode() {
    mFlags.set(Indoor);
    mMinDim = 0.5f;
    mArea = mMinDim * mMinDim;
}

GraphBoundary::GraphBoundary(const GraphEdgeInfo& edgeInfo) {
    seg[0] = edgeInfo.segPoints[0];
    seg[1] = edgeInfo.segPoints[1];
    normal = {seg[0].y - seg[1].y, seg[1].x - seg[0].x, 0};
    normal *= (1.0f / len(normal));
}

Point3F GraphBoundary::midpoint() const { return (seg[0] + seg[1]) * 0.5f; }

// How to navigate through: the outbound normal, and the point to go through.
void GraphBoundary::setItUp(int32_t nodeIndex, const NodeInfoList& nodes, const GraphVolumeList& volumes) {
    const Point3F mid = scaleBetween(seg[0], seg[1], 0.5f);
    const Point3F nodeCenter = nodes[(size_t)nodeIndex].pos;
    if (mDot(mid - nodeCenter, normal) < 0) normal *= -1.0f;
    // Minimum distance of the midpoint from the walls.
    float D, minDist = 1e9f;
    int32_t numWalls = volumes.planeCount(nodeIndex) - 2;
    const PlaneF* planes = volumes.planeArray(nodeIndex);
    while (numWalls--)
        if ((D = -distToPlane(planes[numWalls], mid)) > 0.01f)
            if (D < minDist) minDist = D;
    distIn = std::min(minDist, 1.2f);
    seekPt = mid - (normal * distIn);
}

bool NavigationGraph::initInteriorNodes(const EdgeInfoList& edges, const NodeInfoList& nodes, int32_t startIndex) {
    mBoundaries.clear();
    mHaveVolumes = (mNodeVolumes.size() == nodes.size());

    std::vector<Point3F> utilityBuffer;
    for (size_t i = 0; i < nodes.size(); i++) {
        Point3F normal{0, 0, 1};
        float minDim = 1.0f;
        const float area = 1.0f;
        if (mHaveVolumes) {
            const PlaneF floor = mNodeVolumes.floorPlane((int32_t)i);
            if (floor.z != 0)
                normal = {-floor.x, -floor.y, -floor.z};
            else
                warning("Graph needs regeneration (missing floor plane)");
            minDim = mNodeVolumes.getMinExt((int32_t)i, utilityBuffer);
        }
        mIndoorNodes[i].init(nodes[i], startIndex + (int32_t)i, normal);
        mIndoorNodes[i].setDims(minDim, area);
    }

    for (const GraphEdgeInfo& E : edges) {
        for (uint32_t k = 0; k < 2; k++) {
            GraphEdgeInfo::OneWay edgeOneWay = E.to[k ^ 1];
            const int32_t srcIndex = E.to[k].dest;
            edgeOneWay.dest += startIndex;
            GraphEdge& edge = mIndoorNodes[(size_t)srcIndex].pushOneWay(edgeOneWay);
            // The boundary information
            if (mHaveVolumes) {
                edge.mBorder = (int16_t)mBoundaries.size();
                GraphBoundary boundary(E);
                boundary.setItUp(srcIndex, nodes, mNodeVolumes);
                mBoundaries.push_back(boundary);
            }
        }
    }
    return (int32_t)nodes.size();
}

//-------------------------------------------------------------------------------------
//    Islands (graphIsland.cc)

namespace {

// Marks islands through the searcher's visitation callback.
class IslandMarker : public GraphSearch {
public:
    int32_t mCurIsland = 0;
    void onQExtraction() override { extractedNode()->setIsland(mCurIsland); }
    // Worlds where the filler won't flood up but will flood down: the
    // default searcher stops at an "infinite" time, so override it.
    float getEdgeTime(const GraphEdge*) override { return 1.0f; }
};

} // namespace

int32_t NavigationGraph::markIslands() {
    mIslandPtrs.clear();
    mIslandSizes.clear();
    mNonTransient.clearFlags(GraphNode::GotIsland);
    mLargestIsland = -1;

    int32_t maxCount = -1;
    // Island-mark expansions until there are no unmarked nodes.
    IslandMarker islandMarker;
    islandMarker.mCurIsland = 0;
    for (GraphNode* node : mNonTransient) {
        if (node) {
            if (!node->gotIsland()) {
                const int32_t N = islandMarker.performSearch(node, nullptr);
                if (N > maxCount) {
                    maxCount = N;
                    mLargestIsland = islandMarker.mCurIsland;
                }
                mIslandSizes.push_back(N);
                mIslandPtrs.push_back(node);
                islandMarker.mCurIsland++;
            }
            if (auto* regular = dynamic_cast<RegularNode*>(node)) regular->transientReserve();
        }
    }
    return (int32_t)mIslandPtrs.size();
}

//-------------------------------------------------------------------------------------
//    graphMake.cc

namespace {

bool cullOneEdgeDup(GraphEdgeList& edges, BitVector& mark) {
    GraphEdgeList::iterator it, cull = edges.end();
    for (it = edges.begin(); it != edges.end(); it++)
        if (mark.test((uint32_t)it->mDest)) {
            cull = it;
            break;
        } else {
            mark.set((uint32_t)it->mDest);
        }
    for (it = edges.begin(); it != edges.end(); it++) mark.clear((uint32_t)it->mDest);
    if (cull != edges.end()) {
        // OVector::erase_fast: the last element into the hole.
        *cull = edges.back();
        edges.pop_back();
        return true;
    }
    return false;
}

} // namespace

// Distances are calculated here (JetManager::initEdge), and other final stuff.
int32_t NavigationGraph::doFinalFixups() {
    GraphEdge edgeBuffOuter[MaxOnDemandEdges];
    sTotalEdgeCount = 0;

    for (GraphNode* node : mNodeList) {
        if (node) {
            GraphEdgeArray edgeListOuter = node->getEdges(edgeBuffOuter);
            sTotalEdgeCount += edgeListOuter.numEdges();
            while (GraphEdge* edgePtrOuter = edgeListOuter++) {
                GraphNode* neighbor = mNodeList[(size_t)edgePtrOuter->mDest];
                mJetManager.initEdge(*edgePtrOuter, node, neighbor);
            }
        }
    }

    // Keep a separate list of the non-transients.
    mTransientStart = (int32_t)mNodeList.size();
    mNonTransient = mNodeList;
    for (int32_t i = 0; i < mMaxTransients; i++) mNodeList.push_back(nullptr);

    // Trim duplicate edges (the interior generator can make parallel edges).
    BitVector marker((uint32_t)mNodeList.size());
    int32_t numDups = 0;
    marker.clear();
    for (GraphNode* node : mNodeList)
        if (node)
            while (cullOneEdgeDup(node->mEdges, marker)) numDups++;
    if (numDups) Console::instance().printf(LogLevel::Info, "%d duplicate edges found on nodes.", numDups);

    markIslands();
    newIncarnation();

    mValidLOSTable = (mLOSTable && mLOSTable->valid(numNodes()));
    mValidPathTable = false;      // this has been dropped...
    return 0;
}

bool NavigationGraph::makeGraph() {
    mPushedBridges = false;
    makeRunTimeNodes();
    Console::instance().printf(LogLevel::Info, "MakeGraph: %d indoor, %d outdoor", mNumIndoor, mNumOutdoor);

    mIndoorPtrs.clear();
    mIndoorTree.clear();
    if (mNumIndoor) {
        mIndoorPtrs.reserve((size_t)mNumIndoor);
        for (int32_t i = 0; i < mNumIndoor; i++) mIndoorPtrs.push_back(mNodeList[(size_t)(i + mNumOutdoor)]);
        // Tree for quickly finding indoor nodes-
        mIndoorTree.makeTree(mIndoorPtrs);
    }
    return true;
}

// Jetting connections need to know for sure the player can jump (a big
// difference in height). Missing a few is Ok; the other way isn't.
int32_t NavigationGraph::findJumpableNodes() {
    int32_t total = 0;
    if (!mIsSpawnGraph) {
        const uint32_t sMask = InteriorObjectType | StaticShapeObjectType | TerrainObjectType;
        const float check45 = std::sqrt(0.5f);
        const Point3F down{0, 0, -6};
        const int32_t numNodes = (mNumIndoor + mNumOutdoor);
        NavRayInfo coll;
        for (int32_t i = 0; i < numNodes; i++) {
            if (GraphNode* node = lookupNode(i)) {
                if (node->getNormal().z > check45) {
                    // Cast several rays down and check their normals.
                    Point3F loc = node->location();
                    loc += Point3F{-0.8f, -0.9f, 2.0f};
                    int32_t j;
                    for (j = 0; j < 4; j++) {
                        // (j & 1 != 0) and (j & 2 != 0) as C parses them: both j & 1.
                        Point3F corner{float(j & 1) * 1.6f, float(j & 1) * 1.7f, 0.0f};
                        corner += loc;
                        if (navCastRay(corner, corner + down, sMask, coll))
                            if (coll.normal.z < check45) break;
                    }
                    if (j == 4) {
                        node->set(GraphNode::Flat);
                        total++;
                    }
                }
            }
        }
    }
    return total;
}

//-------------------------------------------------------------------------------------
//    graphBridge.cc: install the bridge edges onto the made graph.

const char* NavigationGraph::pushBridges() {
    if (mPushedBridges) return "Bridges already pushed - do MakeGraph() to remove them";
    if (mBridgeList.empty()) return "No bridges exist";

    for (GraphBridgeData& B : mBridgeList) {
        if (!B.isReplacement()) {
            for (int32_t k = 0; k < 2; k++) {
                auto* src = static_cast<RegularNode*>(mNodeList[B.nodes[k ^ 0]]);
                auto* dst = static_cast<RegularNode*>(mNodeList[B.nodes[k ^ 1]]);
                GraphEdge& edge = src->pushEdge(dst);
                if (B.mustJet()) {
                    edge.setJetting();
                    if (B.jetClear) edge.setHop(B.jetClear);
                }
                mJetManager.initEdge(edge, src, dst);
            }
        } else {
            mJetManager.replaceEdge(B);
        }
    }
    Console::instance().printf(LogLevel::Info, "Connected %d bridges", (int)mBridgeList.size());
    mPushedBridges = true;
    markIslands();
    return nullptr;
}

//-------------------------------------------------------------------------------------
//    Volumes (graphVolume.cc)

#define MapIndoorIndex(i) ((i) - mNumOutdoor)
static constexpr float PlaneAdjErr = 0.014f;

// Volume info packaged up, with the corners (the ceiling's too if asked).
const GraphVolume& NavigationGraph::fetchVolume(const GraphNode* node, bool above) {
    const int32_t Index = MapIndoorIndex(node->getIndex());
    mTempVolumeBuf.mFloor = mNodeVolumes.floorPlane(Index);
    mTempVolumeBuf.mCeiling = mNodeVolumes.abovePlane(Index);
    mTempVolumeBuf.mPlanes = mNodeVolumes.planeArray(Index);
    mTempVolumeBuf.mCount = mNodeVolumes.planeCount(Index);
    mNodeVolumes.getCorners(Index, mTempVolumeBuf.mCorners, !above);
    return mTempVolumeBuf;
}

// Points inside the volume are a negative distance from the planes; the
// maximum of those is the metric. The floor check is either/or; the
// ceiling distance starts the metric.
NodeProximity NavigationGraph::getContainment(int32_t indoorIdx, const Point3F& point) {
    static const NodeProximity sFarProximity;
    NodeProximity metric;
    const PlaneF floor = mNodeVolumes.floorPlane(indoorIdx = MapIndoorIndex(indoorIdx));

    // Below floor -> not a candidate, return low containment (far)
    metric.mHeight = distToPlane(floor, point);
    if (metric.mHeight > PlaneAdjErr) return sFarProximity;

    float ceilingMetric = distToPlane(mNodeVolumes.abovePlane(indoorIdx), point);
    metric.mAboveC = ceilingMetric;
    // Tricky number (being near the top of volumes sometimes matters); fixes
    // a problem in Beggar's Run.
    if (ceilingMetric > 0.0f) ceilingMetric = std::max(ceilingMetric, 7.0f);

    // The closest wall, the ceiling as start.
    metric.mLateral = ceilingMetric;
    const PlaneF* planes = mNodeVolumes.planeArray(indoorIdx);
    int32_t numWalls = mNodeVolumes.planeCount(indoorIdx) - 2;
    while (--numWalls >= 0) {
        const float D = distToPlane(*planes++, point);
        if (D > metric.mLateral) metric.mLateral = D;
    }
    return metric;
}

bool NavigationGraph::closestPointOnVol(GraphNode* node, const Point3F& point, Point3F& soln) const {
    return mNodeVolumes.closestPoint(MapIndoorIndex(node->getIndex()), point, soln);
}

PlaneF NavigationGraph::getFloorPlane(GraphNode* node) {
    return mNodeVolumes.floorPlane(MapIndoorIndex(node->getIndex()));
}

// Between the floor and ceiling of this node?
bool NavigationGraph::verticallyInside(int32_t indoorIdx, const Point3F& point) {
    const PlaneF floor = mNodeVolumes.floorPlane(indoorIdx = MapIndoorIndex(indoorIdx));
    if (distToPlane(floor, point) <= PlaneAdjErr)
        if (distToPlane(mNodeVolumes.abovePlane(indoorIdx), point) <= PlaneAdjErr) return true;
    return false;
}

float NavigationGraph::heightAboveFloor(int32_t indoorIdx, const Point3F& point) {
    const PlaneF floor = mNodeVolumes.floorPlane(MapIndoorIndex(indoorIdx));
    return -(distToPlane(floor, point) + PlaneAdjErr);
}

int32_t NavigationGraph::indoorIndex(const GraphNode* node) { return MapIndoorIndex(node->getIndex()); }

bool NavigationGraph::inNodeVolume(const GraphNode* node, const Point3F& point) {
    const int32_t nodeIndex = MapIndoorIndex(node->volumeIndex());
    int32_t numWalls = mNodeVolumes.planeCount(nodeIndex) - 2;
    const PlaneF* planes = mNodeVolumes.planeArray(nodeIndex);
    while (numWalls--)
        if (distToPlane(planes[numWalls], point) > PlaneAdjErr) return false;
    return true;
}

//-------------------------------------------------------------------------------------
//    Finding nodes (graphFind.cc)

bool NavigationGraph::possibleToJet(const Point3F& from, const Point3F& to, uint32_t) {
    // To be refined-
    return len(from - to) < 60;
}

int32_t NavigationGraph::getNodesInBox(Box3F worldBox, GraphNodeList& listOut, bool justIndoor) {
    // The box is assumed square in XY: get the "radius".
    const Point3F center = (worldBox.min + worldBox.max) * 0.5f;
    listOut.clear();
    if (!justIndoor && haveTerrain()) {
        const float rad = (worldBox.max.x - worldBox.min.x) * 0.5f;
        const int32_t gridRadius = int32_t(rad / gNavGlobs.mSquareWidth) + 1;
        getNodesInArea(listOut, getGridRectangle(center, gridRadius));
    }
    // The indoor node BSP (this one doesn't clear the list)
    const Point3F radExt{MaxGraphNodeVolRad, MaxGraphNodeVolRad, MaxGraphNodeVolRad};
    worldBox.min -= radExt;
    worldBox.max += radExt;
    mIndoorTree.getIntersecting(listOut, worldBox);
    return (int32_t)listOut.size();
}

bool NavigationGraph::haveMuzzleLOS(int32_t nodeInd1, int32_t nodeInd2) {
    if (nodeInd1 != nodeInd2 && mValidLOSTable) return mLOSTable->muzzleLOS(nodeInd1, nodeInd2);
    return true;
}

const GraphNodeList& NavigationGraph::getVisibleNodes(GraphNode* from, const Point3F& loc, float rad) {
    mUtilityNodeList1.clear();
    mUtilityNodeList2.clear();
    if (from) {
        if (haveTerrain()) {
            const int32_t gridRadius = int32_t(rad / gNavGlobs.mSquareWidth) + 1;
            getNodesInArea(mUtilityNodeList1, getGridRectangle(loc, gridRadius));
        }
        // The indoor node BSP (doesn't clear the list)
        Box3F checkBox = makeBox(loc, loc);
        rad += MaxGraphNodeVolRad;
        const Point3F radExt{rad, rad, rad};
        checkBox.min -= radExt;
        checkBox.max += radExt;
        mIndoorTree.getIntersecting(mUtilityNodeList1, checkBox);

        const int32_t fromInd = from->getIndex();
        for (GraphNode* node : mUtilityNodeList1)
            if (haveMuzzleLOS(fromInd, node->getIndex())) mUtilityNodeList2.push_back(node);
    }
    return mUtilityNodeList2;
}

const GraphNodeList& NavigationGraph::getVisibleNodes(const Point3F& loc, float rad) {
    FindGraphNode finder(loc);
    return getVisibleNodes(finder.closest(), loc, rad);
}

// What constitutes a crossing path: through the border midpoint, else an
// arc corner (non-jetting too, or collisions are missed).
int32_t NavigationGraph::crossingSegs(const GraphNode* node, const GraphEdge* edge, LineSegment* const segBuffer) {
    constexpr float CrossingSegAdjustUp = 0.3f;
    LineSegment* segs = segBuffer;
    Point3F nodeLoc = node->location();
    Point3F destLoc = lookupNode(edge->mDest)->location();
    nodeLoc.z += CrossingSegAdjustUp;
    destLoc.z += CrossingSegAdjustUp;
    if (edge->isBorder()) {
        Point3F crossingPt = getBoundary(edge->mBorder).midpoint();
        crossingPt.z += CrossingSegAdjustUp;
        (segs++)->set(nodeLoc, crossingPt);
        (segs++)->set(crossingPt, destLoc);
    } else {
        Point3F arcCorner;
        if (nodeLoc.z > destLoc.z)
            (arcCorner = destLoc).z = nodeLoc.z;
        else
            (arcCorner = nodeLoc).z = destLoc.z;
        (segs++)->set(nodeLoc, arcCorner);
        (segs++)->set(arcCorner, destLoc);
    }
    return (int32_t)(segs - segBuffer);
}

// The edges the force field blocks (the force field monitor, at mission
// start). The engine enables collision on this field alone and casts
// ForceFieldObjectType rays that must hit it: here the segment against the
// field's box while the field is not open.
// APPROXIMATION: the field's box is its axis-aligned world box (the engine
// collides in the object's space, which differs for a rotated field).
const GraphEdgePtrs& NavigationGraph::getBlockedEdges(const std::string& forceField) {
    mVisibleEdges.clear();
    auto* field = EngineObjects::get<ForceFieldBareObject>(forceField);
    if (!field) return mVisibleEdges;
    Box3F objBox;
    field->worldBox(objBox.min, objBox.max);
    GraphEdge edgeBuffer[MaxOnDemandEdges];
    LineSegment segments[4];
    GraphNodeList consider;
    if (int32_t numNodes = getNodesInBox(objBox, consider)) {
        for (int32_t i = 0; i < numNodes; i++) {
            GraphNode* node = consider[(size_t)i];
            GraphEdgeArray edges = node->getEdges(edgeBuffer);
            while (GraphEdge* edge = edges++)
                for (int32_t j = crossingSegs(node, edge, segments) - 1; j >= 0; j--) {
                    const Point3F A = segments[j].getEnd(0);
                    const Point3F B = segments[j].getEnd(1);
                    // The quick crude test and the field's castRay in one.
                    if (!field->isOpen() && boxCollideLine(objBox, A, B)) {
                        mVisibleEdges.push_back(edge);
                        break;
                    }
                }
        }
    }
    return mVisibleEdges;
}

// The roaming radius off the terrain graph information; 0 if none.
float NavigationGraph::getRoamRadius(const Point3F& loc) {
    SphereF sphere;
    const Point3F loc2D{loc.x, loc.y, 0.0f};
    if (mTerrainInfo.inGraphArea(loc2D))
        if (mTerrainInfo.locToIndexAndSphere(sphere, loc2D) >= 0) return sphere.radius;
    return 0.0f;
}

// Translate into block space; optional normal.
bool NavigationGraph::terrainHeight(Point3F pos, float* height, Point3F* normal) {
    pos -= mTerrainInfo.originWorld;
    if (mHaveTerrainBlock) {
        const Point2F point2F{pos.x, pos.y};
        if (ServerContainer::terrainHeight(point2F, height)) {
            if (normal != nullptr) ServerContainer::terrainHeight(point2F, nullptr, normal, true);
            return true;
        }
    }
    return false;
}

// The terrain node here by grid lookup.
GraphNode* NavigationGraph::findTerrainNode(const Point3F& atLocation) {
    if (haveTerrain()) {
        Point3F loc;
        bool inArea;
        // Bots can enter outside of the area- no height check for those.
        if (mTerrainInfo.inGraphArea(atLocation)) {
            inArea = true;
            loc = atLocation;
        } else {
            inArea = false;
            loc = mTerrainInfo.whereToInbound(atLocation);
        }
        const int32_t gridIndex = mTerrainInfo.locToIndex(loc);
        if (gridIndex >= 0) {
            if (GraphNode* node = mNodeGrid[(size_t)gridIndex]) {
                if (inArea) {
                    float height;
                    if (terrainHeight(loc, &height)) {
                        if (height < (loc.z + 0.04f)) {
                            const float terrHt = node->terrHeight();
                            if (loc.z < (height + terrHt)) return node;
                        }
                    }
                } else {
                    return node;
                }
            }
        }
    }
    return nullptr;
}

// When findTerrainNode() fails: on terrain with a node within one grid
// location.
GraphNode* NavigationGraph::nearbyTerrainNode(const Point3F& loc) {
    GraphNode* node = nullptr;
    if (haveTerrain()) {
        float H;
        const int32_t gridIndex = mTerrainInfo.locToIndex(loc);
        if ((gridIndex >= 0) && terrainHeight(loc, &H) && (H < loc.z + 0.04f)) {
            // The nearest immediately neighboring node.
            const int32_t* offs = mTerrainInfo.indOffs;
            float bestDist = 1e9f, dSq;
            for (int32_t i = 0; i < 8; i++) {
                const int32_t n = gridIndex + offs[i];
                if (validArrayIndex(n, mTerrainInfo.nodeCount))
                    if (GraphNode* N = mNodeGrid[(size_t)n])
                        if ((dSq = lenSquared(loc - N->location())) < bestDist) bestDist = dSq, node = N;
            }
        }
    }
    return node;
}

GraphNode* NavigationGraph::closestNode(const Point3F& loc, float* containment) {
    GraphNode* terrNode = findTerrainNode(loc);
    if (terrNode) return terrNode;

    terrNode = nearbyTerrainNode(loc);
    Box3F box = makeBox(loc, loc);
    box.max += Point3F{0, 0, 20};
    box.max += Point3F{35, 35, 0};
    box.min -= Point3F{0, 0, 80};
    box.min -= Point3F{35, 35, 0};

    GraphNodeList indoor;
    mIndoorTree.getIntersecting(indoor, box);

    // The node with the best containment metric
    GraphNode* bestNode = nullptr;
    float bestMetric = 1e13f;
    for (GraphNode* node : indoor) {
        const float metric = float(node->containment(loc));
        if (metric < bestMetric) {
            bestMetric = metric;
            bestNode = node;
        }
    }
    if (!bestNode) return terrNode;
    if (terrNode) {
        const float terrMetric = len(terrNode->location() - loc) - terrNode->radius();
        if (terrMetric < bestMetric) return terrNode;
    }
    if (containment) *containment = bestMetric;
    return bestNode;
}

//-------------------------------------------------------------------------------------

FindGraphNode::FindGraphNode() { init(); }

FindGraphNode::FindGraphNode(const Point3F& pt, GraphNode* hint) {
    init();
    setPoint(pt, hint);
}

void FindGraphNode::init() {
    mClosest = nullptr;
    // Should be safe value that no-one will use:
    mPoint = {-3.14159e14f, 3.1e13f, -2.22e22f};
}

uint32_t FindGraphNode::calcHash(const Point3F& point) {
    uint32_t hashNums[3];
    std::memcpy(hashNums, &point, sizeof(hashNums));
    const uint32_t val0 = (hashNums[0] >> 7) ^ (hashNums[0] << 5);
    const uint32_t val1 = (hashNums[1] + 77773) & 0xFFFFFFF;
    const uint32_t val2 = (hashNums[2] * 37) & 0xFFFFFFF;
    return (val0 + val1 + val2) % HashTableSize;
}

void FindGraphNode::setPoint(const Point3F& point, GraphNode* hint) {
    if (point == mPoint) return;
    // Hash the point into the cache the graph holds.
    mPoint = point;
    FindGraphNode& cacheEntry = gNavGraph->mFoundNodes[calcHash(mPoint)];
    if (cacheEntry.mPoint == point) {
        mClosest = cacheEntry.mClosest;
    } else {
        // Inside the hint: that is the closest. Not cached, since hints are
        // usually moving locations.
        if (hint && hint->containment(point) < 0) {
            mClosest = hint;
        } else {
            mClosest = gNavGraph->closestNode(point);
            cacheEntry = *this;          // (NULL results too)
        }
    }
}

//-------------------------------------------------------------------------------------
//    Grid <-> World

GridArea NavigationGraph::getGridRectangle(const Point3F& atPos, int32_t gridRadius) {
    GridArea atRect;
    Point2I atGridLoc;
    worldToGrid(atPos, atGridLoc);
    atRect.point.x = atGridLoc.x - gridRadius;
    atRect.point.y = atGridLoc.y - gridRadius;
    atRect.extent.x = atRect.extent.y = (gridRadius << 1);
    return atRect;
}

void NavigationGraph::worldToGrid(const Point3F& wPos, Point2I& gPos) {
    Point3F origin{0, 0, 0};
    float squareSize = 8.0f;
    ServerContainer::terrainBlock(origin, squareSize);
    const float x = (wPos.x - origin.x) / squareSize;
    const float y = (wPos.y - origin.y) / squareSize;
    gPos.x = (int32_t)std::floor(x);
    gPos.y = (int32_t)std::floor(y);
}

Point3F NavigationGraph::gridToWorld(const Point2I& gPos) {
    Point3F retVal;
    if (!mTerrainInfo.posToLoc(retVal, gPos)) retVal = {-1, -1, -1};
    return retVal;
}

GridArea NavigationGraph::getWorldRect() { return GridArea(mTerrainInfo.originGrid, mTerrainInfo.gridDimensions); }

namespace {

// Visitor to find terrain nodes in a rectangular area.
class VisitNodeGrid : public GridVisitor {
protected:
    const GridArea& mWorld;
    const GraphNodeList& mGrid;
    GraphNodeList& mListOut;
    GraphNode* getNodeAt(const GridArea& R) const { return mGrid[(size_t)mWorld.getIndex(R.point)]; }

public:
    VisitNodeGrid(const GridArea& toVisit, const GridArea& worldArea, const GraphNodeList& gridList,
                  GraphNodeList& listOut)
        : GridVisitor(toVisit), mWorld(worldArea), mGrid(gridList), mListOut(listOut) {}
    bool beforeDivide(const GridArea& R, int32_t level) override {
        if (GraphNode* node = getNodeAt(R)) {
            if (node->getLevel() == level) {
                mListOut.push_back(node);
                return false;
            }
        }
        return true;
    }
    bool atLevelZero(const GridArea& R) override {
        if (GraphNode* node = getNodeAt(R)) mListOut.push_back(node);
        return true;
    }
};

} // namespace

int32_t NavigationGraph::getNodesInArea(GraphNodeList& listOut, GridArea toVisit) {
    listOut.clear();
    if (haveTerrain()) {
        const GridArea worldRect = getWorldRect();
        if ((int32_t)mNodeGrid.size() == (worldRect.extent.x * worldRect.extent.y)) {
            if (toVisit.intersect(worldRect)) {
                VisitNodeGrid visitor(toVisit, worldRect, mNodeGrid, listOut);
                visitor.traverse();
            }
        }
    }
    return (int32_t)listOut.size();
}

//-------------------------------------------------------------------------------------
//    graphQueries.cc

float NavigationGraph::fastDistance(const Point3F& p1, const Point3F& p2) {
    if (gotOneWeCanUse()) return gNavGraph->distancef(p1, p2);
    warning("fastDistance() called without usable graph present");
    return len(p1 - p2);
}

// The path table walk was removed from the engine.
float NavigationGraph::distancef(const Point3F& p1, const Point3F& p2) { return len(p1 - p2); }

void NavigationGraph::chokePoints(const Point3F& srcPoint, std::vector<Point3F>& points, float minHideD,
                                  float stopSearchD) {
    if (GraphNode* srcNode = closestNode(srcPoint))
        getLOSSearcher()->findChokePoints(srcNode, points, minHideD, stopSearchD);
    else
        warning("getChokePoints() failed to find a closest node");
}

int32_t NavigationGraph::getChokePoints(const Point3F& srcPoint, std::vector<Point3F>& points, float minHideDist,
                                        float stopSearchDist) {
    points.clear();
    if (gotOneWeCanUse()) {
        if (gNavGraph->validLOSXref())
            gNavGraph->chokePoints(srcPoint, points, minHideDist, stopSearchDist);
        else
            warning("getChokePoints() called without valid LOS XRef table");
    } else {
        warning("getChokePoints() called without (usable) graph in place");
    }
    return (int32_t)points.size();
}

Point3F NavigationGraph::hideOnSlope(const Point3F& from, const Point3F& avoid, float rad, float deg) {
    if (gotOneWeCanUse() && gNavGraph->validLOSXref()) {
        const float angle = (90 - deg) * 3.14159265358979323846f / 180.0f;
        return gNavGraph->getLOSSearcher()->hidingPlace(from, avoid, rad, angle, true);
    }
    return from;
}

Point3F NavigationGraph::hideOnDistance(const Point3F& from, const Point3F& avoid, float rad, float hideLen) {
    if (gotOneWeCanUse() && gNavGraph->validLOSXref())
        return gNavGraph->getLOSSearcher()->hidingPlace(from, avoid, rad, hideLen, false);
    return from;
}

Point3F NavigationGraph::findLOSLocation(const Point3F& from, const Point3F& wantToSee, float minDist,
                                         const SphereF& getCloseTo, float capDist) {
    if (gotOneWeCanUse() && gNavGraph->validLOSXref())
        return gNavGraph->getLOSSearcher()->findLOSLoc(from, wantToSee, minDist, getCloseTo, capDist);
    return from;
}

//-------------------------------------------------------------------------------------
//    graphSpawn.cc

// With a valid spawn list (a SPN file, or the SPN loaded into a NAV graph)
// that is what is used; else a random node in the sphere.
int32_t NavigationGraph::randNode(const Point3F& P, float R, bool indoor, bool outdoor) {
    const SphereF sphere(P, R);
    if (!mSpawnList.empty()) return mSpawnList.getRandom(sphere, indoor, Nav::gRandGen().randI());

    mUtilityNodeList1.clear();
    mUtilityNodeList2.clear();
    // Terrain first since getNodesInArea() clears the list...
    if (outdoor && haveTerrain()) {
        const int32_t gridRad = int32_t(R / gNavGlobs.mSquareWidth) + 1;
        getNodesInArea(mUtilityNodeList1, getGridRectangle(P, gridRad));
    }
    // ... and indoor second since getIntersecting() appends.
    if (indoor) {
        Box3F wBox = makeBox(P, P);
        const Point3F ext{R, R, R};
        wBox.min -= ext;
        wBox.max += ext;
        mIndoorTree.getIntersecting(mUtilityNodeList1, wBox);
    }
    const int32_t count = mUtilityNodeList2.searchSphere(sphere, mUtilityNodeList1);
    if (count > 0) return mUtilityNodeList2[Nav::gRandGen().randI() % (uint32_t)count]->getIndex();
    return -1;
}

// Old system if there's a NAV (no spawn list), else the spawn list.
const Point3F* NavigationGraph::getSpawnLoc(int32_t nodeIndex) {
    if (mSpawnList.empty()) {
        if (validArrayIndex(nodeIndex, numNodes()))
            if (GraphNode* node = lookupNode(nodeIndex)) return &node->location();
    } else {
        if (validArrayIndex(nodeIndex, (int32_t)mSpawnList.size())) return &mSpawnList[(size_t)nodeIndex];
    }
    return nullptr;
}

const Point3F* NavigationGraph::getRandSpawnLoc(int32_t nodeIndex) {
    if (mSpawnList.empty()) {
        if (validArrayIndex(nodeIndex, numNodes()))
            if (GraphNode* node = lookupNode(nodeIndex)) {
                static Point3F sPoint;
                sPoint = node->randomLoc();
                return &sPoint;
            }
    } else {
        if (validArrayIndex(nodeIndex, (int32_t)mSpawnList.size())) return &mSpawnList[(size_t)nodeIndex];
    }
    return nullptr;
}

namespace {
constexpr int CastShift = 6;
constexpr int NumCasts = 1 << CastShift;
constexpr int Periphery = NumCasts / 4 + 1;
constexpr double NavPi = 3.14159265358979323846;
constexpr double AngleInc = (NavPi * 2.0) / double(float(NumCasts));
constexpr float CapDistance = 60.0f;

// NumCasts LOS casts from 1m up: the squared distances and angles; returns
// the minimum distance.
float lookAround(Point3F P, float dists[NumCasts], float qAngles[NumCasts]) {
    float minDist = CapDistance * CapDistance;
    P.z += 1.0f;
    for (int i = 0; i < NumCasts; ++i) {
        float ang = (float)(float(i) * AngleInc);
        qAngles[i] = ang;
        ang = (float)(ang + NavPi / 2.0);
        Point3F D{std::cos(ang), std::sin(ang), 0.0f};
        D *= CapDistance;
        D += P;
        NavRayInfo coll;
        if (navCastRay(P, D, 0xFFFFFFFFu, coll))
            minDist = std::min(dists[i] = lenSquared(coll.point - P), minDist);
        else
            dists[i] = CapDistance * CapDistance;
    }
    return minDist;
}
} // namespace

float NavigationGraph::whereToLook(Point3F P) {
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
        if ((metric = dists[j] - sideDist) > bestMetric) bestMetric = metric, bestIndex = (int)j;
    }
    return qAngles[bestIndex];
}

void NavigationGraph::adjustSpawnLoc(Point3F& point) {
    // Loser(-1).hitBelow(point, 20)
    point.z = (float)(point.z + 1.3);
    NavRayInfo coll;
    if (navCastRay(point, {point.x, point.y, point.z - 20.0f}, 0xFFFFFFFFu, coll)) {
        point = coll.point;
        point.z = (float)(point.z + len2D(coll.normal.x, coll.normal.y) / 2.0);
        point.z = (float)(point.z + 0.2);      // some shapes have problems... cf. ThinIce
    }
}

//-------------------------------------------------------------------------------------
//    graphThreats.cc

bool NavigationGraph::installThreat(const std::string& threat, int32_t team, float rad) {
    if (mIsSpawnGraph) return true;          // Just a spawn graph...
    return mThreats.add(SearchThreat(threat, rad, team));
}
