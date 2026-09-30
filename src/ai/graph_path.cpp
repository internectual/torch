// NavigationPath (ai/graphPath.cc), the indoor path smoothing
// (graphSmooth.cc), threats (graphThreats.cc), the force field monitor
// (graphForceField.cc) and the JetManager (graphJetting.cc).
#include "ai/graph.h"
#include "core/console.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/force_field.h"
#include "sim/shape_base.h"
#include <cmath>
#include <strings.h>

using namespace NavMath;

namespace {
constexpr uint32_t TerrainObjectType = 1u << 2;
constexpr uint32_t InteriorObjectType = 1u << 3;
// TickSec (32 ms ticks)
constexpr float TickSec = 0.032f;
} // namespace

//-------------------------------------------------------------------------------------
//                Search Threat Object

ShapeBase* SearchThreat::threatObject() const {
    return mThreat.empty() ? nullptr : EngineObjects::get<ShapeBase>(mThreat);
}

SearchThreat::SearchThreat(const std::string& threat, float R, int32_t team) {
    mThreat = threat;
    if (ShapeBase* shape = threatObject()) {
        center = {shape->transform[3], shape->transform[7], shape->transform[11]};
        mActive = (shape->damageState == ShapeBase::Enabled);
    }
    radius = R;
    mTeam = (int16_t)team;
}

// Update the threat bit on the nodes this threat affects.
void SearchThreat::updateNodes() {
    GraphThreatSet threatSetOn = 1;
    const GraphThreatSet threatSetOff = ~(threatSetOn <<= mSlot);
    if (mActive)
        for (GraphNode* n : mEffects) n->threats() |= threatSetOn;
    else
        for (GraphNode* n : mEffects) n->threats() &= threatSetOff;
}

// A change of the enabled status changes a bunch of edges: tell the caller.
bool SearchThreat::checkEnableChange() {
    ShapeBase* shape = threatObject();
    const bool enabled = (shape->damageState == ShapeBase::Enabled);
    if (enabled != mActive) {
        mActive = enabled;
        updateNodes();
        return true;
    }
    return false;
}

//-------------------------------------------------------------------------------------
//                   Threat List Management / Configuration

SearchThreats::SearchThreats() {
    std::memset(mTeamCaresAbout, 0, sizeof(mTeamCaresAbout));
    // Threat zero is reserved for the LOS queries' temporary threats; every
    // team "worries" about it.
    mCheck = mCount = 1;
    informOtherTeams(-1, 0, true);
    mThreats[0].mActive = false;
    mSaveTimeMS = navSimTimeMS();
}

// Every other team's care-about set gets this threat's active status.
void SearchThreats::informOtherTeams(int32_t thisTeam, int32_t thisThreat, bool isOn) {
    GraphThreatSet threatSet = 1;
    threatSet <<= thisThreat;
    for (int32_t i = 0; i < GraphMaxTeams; i++)
        if (i != thisTeam && isOn)
            mTeamCaresAbout[i] |= threatSet;
        else
            mTeamCaresAbout[i] &= ~threatSet;
}

GraphThreatSet SearchThreats::getThreatSet(int32_t team) const { return mTeamCaresAbout[team]; }

void SearchThreats::pushTempThreat(GraphNode* losNode, const Point3F& avoidPt, float avoidRad) {
    mThreats[0].mEffects = gNavGraph->getVisibleNodes(losNode, avoidPt, avoidRad);
    mThreats[0].mActive = true;
    mThreats[0].updateNodes();
}

void SearchThreats::popTempThreat() {
    mThreats[0].mActive = false;
    mThreats[0].updateNodes();
    mThreats[0].mEffects.clear();
}

// Once per mission threat at mission startup.
bool SearchThreats::add(const SearchThreat& threat) {
    if (mCount < MaxThreats) {
        const int32_t X = mCount++;
        mThreats[X] = threat;
        informOtherTeams(threat.mTeam, mThreats[X].mSlot = (int16_t)X, true);
        mThreats[X].mEffects = gNavGraph->getVisibleNodes(threat.center, threat.radius);
        mThreats[X].updateNodes();
        return true;
    }
    return false;
}

int32_t SearchThreats::getThreatPtrs(const SearchThreat* list[MaxThreats], GraphThreatSet mask) const {
    int32_t numActive = 0;
    GraphThreatSet bit = 1;
    for (int32_t i = 0; i < mCount; i++, bit <<= 1)
        if ((mask & bit) && mThreats[i].mActive) list[numActive++] = &mThreats[i];
    return numActive;
}

// Keeps the status of the threats current. Returns whether more can be
// afforded (not at the list's end, and no threat had to be rescanned).
bool SearchThreats::checkOne() {
    bool canCheckMore = true;
    if (mCheck < mCount) {
        // (Threats that went away stay; the team follows the assigned one.)
        if (mThreats[mCheck].threatObject()) {
            if (mThreats[mCheck].checkEnableChange()) {
                canCheckMore = false;
                informOtherTeams(mThreats[mCheck].mTeam, mCheck, mThreats[mCheck].mActive);
            }
        }
        mCheck++;
    } else {
        // Skip the reserved first threat-
        mCheck = 1;
        canCheckMore = false;
    }
    return canCheckMore;
}

// Called frequently; works at most once a second.
void SearchThreats::monitorThreats() {
    const uint32_t T = navSimTimeMS();
    if ((T - mSaveTimeMS) > 1000u) {
        mSaveTimeMS = T;
        while (checkOne())
            ;
    }
}

// Whether a leg the path advancer wants goes clear of the threats.
bool SearchThreats::sanction(const Point3F& from, const Point3F& to, int32_t team) const {
    if (mCount) {
        const SearchThreat* threatList[MaxThreats];
        if (int32_t N = getThreatPtrs(threatList, getThreatSet(team))) {
            LineSegment travelLine(from, to);
            while (--N >= 0) {
                const SphereF* sphere = threatList[N];
                if (sphere->isContained(from) || sphere->isContained(to)) return false;
                if (travelLine.distance(sphere->center) < sphere->radius) return false;
            }
        }
    }
    return true;
}

//-------------------------------------------------------------------------------------
//                Force fields (graphForceField.cc)

namespace {

ForceFieldBareObject* fieldOf(const std::string& handle) {
    return handle.empty() ? nullptr : EngineObjects::get<ForceFieldBareObject>(handle);
}

int32_t forceFieldTeam(const std::string& handle) {
    if (ForceFieldBareObject* ff = fieldOf(handle))
        // isTeamControlled: !otherPermiable
        return !ff->dataBool("otherPermiable", false) ? (int32_t)sensorGroupOf(*ff) : 0;
    return 0;
}

// Con::executef(ff, 1, "isPowered")
bool forceFieldUp(const std::string& handle) {
    if (!fieldOf(handle) || !ScriptEngine::exists() || !ScriptEngine::instance().ts()) return false;
    VMValue result;
    if (!ScriptEngine::instance().ts()->callObjectMethod(handle, "isPowered", {}, &result)) return false;
    const std::string text = result.toString();
    return strcasecmp(text.c_str(), "true") == 0 || std::atof(text.c_str()) != 0.0;
}

} // namespace

MonitorForceFields::MonitorForceFields() {
    for (int32_t i = 0; i < GraphMaxTeams; i++) mTeamPartitions[i].setType(GraphPartition::ForceField);
}

void MonitorForceFields::ForceField::updateEdges() {
    const uint32_t team = (mActive ? mTeam : 0);
    for (GraphEdge* e : mEffects) e->setTeam((uint8_t)team);
}

// The container's force fields (by object id), their teams and powered
// state, and the edges each one blocks.
void MonitorForceFields::atMissionStart() {
    std::vector<std::pair<int, std::string>> found;
    if (ScriptEngine::exists())
        for (auto& [key, object] : ScriptEngine::instance().objects)
            if (object && dynamic_cast<ForceFieldBareObject*>(object->engine.get()))
                found.emplace_back(ScriptEngine::instance().objectId(object),
                                   std::to_string(ScriptEngine::instance().objectId(object)));
    std::sort(found.begin(), found.end());

    mFFList.assign(found.size(), ForceField());
    mCount = (int32_t)found.size();
    for (int32_t i = 0; i < mCount; i++) {
        mFFList[(size_t)i].mFF = found[(size_t)i].second;
        mFFList[(size_t)i].mTeam = (uint32_t)forceFieldTeam(mFFList[(size_t)i].mFF);
        mFFList[(size_t)i].mActive = forceFieldUp(mFFList[(size_t)i].mFF);
    }
    // The engine disables collision on all fields but the one checked.
    for (int32_t i = 0; i < mCount; i++) mFFList[(size_t)i].mEffects = gNavGraph->getBlockedEdges(mFFList[(size_t)i].mFF);
    for (int32_t i = 0; i < mCount; i++) mFFList[(size_t)i].updateEdges();
}

// Clear partitions on all teams except one.
void MonitorForceFields::clearPartitions(uint32_t exceptTeam) {
    for (uint32_t i = 0; i < (uint32_t)GraphMaxTeams; i++)
        if (i != exceptTeam) mTeamPartitions[i].clear();
}

// This team had a search fail (only due to force fields): the nodes the
// search visited become a partition.
void MonitorForceFields::informSearchFailed(GraphSearch* searcher, uint32_t team) {
    mTeamPartitions[team].pushPartition(searcher->getPartition());
}

GraphPartition::Answer MonitorForceFields::reachable(uint32_t team, int32_t from, int32_t to) {
    return mTeamPartitions[team].reachable(from, to);
}

// Checked periodically (twice a second at most).
void MonitorForceFields::monitor() {
    const uint32_t T = navSimTimeMS();
    if ((T - mSaveTimeMS) > 500u) {
        mSaveTimeMS = T;
        for (ForceField& ff : mFFList) {
            const uint32_t team = (uint32_t)forceFieldTeam(ff.mFF);
            const bool active = forceFieldUp(ff.mFF);
            if (team == ff.mTeam && active == ff.mActive) continue;
            // Only the old and new teams are invalidated, unless either is
            // team zero (everybody's partition changes).
            if (team != ff.mTeam) {
                if (team && ff.mTeam) {
                    mTeamPartitions[team].clear();
                    mTeamPartitions[ff.mTeam].clear();
                } else {
                    clearPartitions(0);
                }
                ff.mTeam = team;
            }
            if (active != ff.mActive) {
                clearPartitions(ff.mTeam);
                ff.mActive = active;
            }
            ff.updateEdges();
        }
    }
}

//-------------------------------------------------------------------------------------
//                JetManager (graphJetting.cc)

void JetManager::clear() {
    for (JetPartition& p : mPartitions) p.doConstruct();
    mArmorPart.reset();
    mFloodInds[0].clear();
    mFloodInds[1].clear();
}

// Both passes of the armor partitioning: a flood fill with the armor
// partition telling what has been "extracted", with two flipping lists.
int32_t JetManager::floodPartition(uint32_t seed, bool needBoth, float ratings[2]) {
    int32_t numFound = 0;
    int32_t floodSizes[2] = {1, 0};
    int32_t curExtract = 0, curDeposit;
    mArmorPart->set((int32_t)(mFloodInds[0][0] = (uint16_t)seed));

    while (floodSizes[curExtract]) {
        uint16_t* extractFrom = mFloodInds[curExtract].data();
        uint16_t* depositInto = mFloodInds[curDeposit = (curExtract ^ 1)].data();
        for (int32_t i = floodSizes[curExtract] - 1; i >= 0; i--) {
            GraphNode* node = gNavGraph->lookupNode(extractFrom[i]);
            GraphEdgeArray edges = node->getEdges(nullptr);
            while (GraphEdge* edge = edges++) {
                if (!mArmorPart->test(edge->mDest)) {
                    if (!edge->isJetting() || edge->canJet(ratings, needBoth)) {
                        mArmorPart->set(edge->mDest);
                        depositInto[floodSizes[curDeposit]++] = (uint16_t)edge->mDest;
                    }
                }
            }
        }
        numFound += floodSizes[curExtract];
        floodSizes[curExtract] = 0;
        curExtract = curDeposit;
    }
    return numFound;
}

// The partition for one armor / energy configuration.
int32_t JetManager::partitionOneArmor(JetPartition& list) {
    const int32_t nodeCount = gNavGraph->numNodes();
    // First time allocations (newIncarnation() clears them). The partition
    // includes the transients so the search code doesn't have to check.
    if (!mArmorPart) {
        mArmorPart = std::make_unique<GraphPartition>();
        mArmorPart->setSize(gNavGraph->numNodesAll());
        mFloodInds[0].assign((size_t)nodeCount, 0);
        mFloodInds[1].assign((size_t)nodeCount, 0);
    }

    int32_t seedNode = 0;
    int32_t total = 0;
    std::vector<int32_t> saveSeedNodes;

    // PASS 1: the 'stranded' component of each partition, flooding only
    // along edges that can be traversed BOTH WAYS.
    list.clear();
    mArmorPart->clear();
    while (seedNode < nodeCount) {
        if (!mArmorPart->test(seedNode)) {
            total += floodPartition((uint32_t)seedNode, true, list.ratings);
            list.pushPartition(*mArmorPart);
            saveSeedNodes.push_back(seedNode);
            if (total == nodeCount) break;
        }
        seedNode++;
    }

    // PASS 2: the downhill component of each partition, if different.
    for (size_t i = 0; i < list.size(); i++) {
        mArmorPart->clear();
        floodPartition((uint32_t)saveSeedNodes[i], false, list.ratings);
        list[i].setDownhill(*mArmorPart);
    }
    // Not ported: Memory::getMemoryUsed accounting (sLoadMemUsed).
    return (int32_t)list.size();
}

// Close enough abilities (so far only the duration has been seen to change).
bool JetManager::Ability::operator==(const Ability& ability) const {
    return std::fabs(acc - ability.acc) < 0.01f && std::fabs(dur - ability.dur) < 0.5f &&
           std::fabs(v0 - ability.v0) < 0.1f;
}

void JetManager::ID::reset() {
    if (NavigationGraph::gotOneWeCanUse()) gNavGraph->jetManager().decRefCount(*this);
    id = -1;
}

JetManager::ID::~ID() {
    if (NavigationGraph::gotOneWeCanUse()) gNavGraph->jetManager().decRefCount(*this);
}

void JetManager::JetPartition::doConstruct() {
    ratings[0] = 0.0f;
    ratings[1] = 0.0f;
    users = 0;
    used = false;
    time = 0;
}

void JetManager::decRefCount(const ID& id) {
    if (id.id >= 0) {
        JetPartition& jetPartition = mPartitions[id.id];
        // This is when it starts to grow old-
        if (--jetPartition.users == 0) jetPartition.time = navSimTimeMS();
    }
}

// Returns true when a new partition was made, telling the nav path to put
// off slow operations a little.
bool JetManager::update(ID& id, const Ability& ability) {
    // Mission cycled or graph rebuilt: the id is stale.
    if (id.incarnation != gNavGraph->incarnation()) {
        id.incarnation = gNavGraph->incarnation();
        id.id = -1;
    }
    if (id.id < 0 || !(mPartitions[id.id].ability == ability)) {
        decRefCount(id);
        // A partition matching this ability (unused ones are kept); else a
        // never-used slot, else the oldest unused one.
        uint32_t oldAge = uint32_t(-1);
        JetPartition* slot = nullptr;
        JetPartition* oldest = nullptr;
        JetPartition* j = mPartitions;
        for (int32_t i = 0; i < AbsMaxBotCount; i++, j++) {
            if (j->ability == ability) {
                j->users++;
                id.id = i;
                return false;
            } else if (!slot && !j->users) {
                if (!j->used)
                    slot = j;
                else if (j->time < oldAge)
                    oldAge = j->time, oldest = j;
            }
        }
        if (!slot) slot = oldest;
        id.id = (int32_t)(slot - mPartitions);
        slot->ability = ability;
        slot->users = 1;
        slot->used = true;
        calcJetRatings(slot->ratings, ability);
        slot->setType(GraphPartition::Armor);
        partitionOneArmor(*slot);
        return true;
    }
    return false;
}

// The physics math is accurate for vertical distance; the lateral is a
// heuristic, scaled down (it increases what the bot thinks it must do).
static constexpr float LateralExtra = 2.00f;
static constexpr float LateralScale = 1.28f;

float JetManager::modifiedLateral(float lateralDist) { return lateralDist * LateralScale + LateralExtra; }

float JetManager::modifiedDistance(float lateralDist, float verticalDist) {
    return verticalDist + modifiedLateral(lateralDist);
}

// The inverse of modifiedLateral().
float JetManager::invertLateralMod(float lateralField) {
    const float result = (lateralField - LateralExtra) * (1.0f / LateralScale);
    return std::max(0.0f, result);
}

// For aiNavJetting's knowing when it has enough energy to launch: the same
// adjusted distance the jetting edges use.
float JetManager::jetDistance(const Point3F& from, const Point3F& to) const {
    const float vertical = (to.z - from.z);
    const float lateral = len2D(to.x - from.x, to.y - from.y);
    if (vertical > 0) return modifiedDistance(lateral, vertical);
    return modifiedLateral(lateral);
}

// All edge initialization comes through here: the partitioning needs the
// mDist of a pair of edges between two nodes to be exactly the same.
void JetManager::initEdge(GraphEdge& edge, const GraphNode* src, const GraphNode* dst) {
    if (!edge.isJetting())
        edge.setInverse(src->edgeScale(dst));
    else
        edge.setInverse(calcJetScale(src, dst));

    // Sort the locations so distances are the same both ways.
    const Point3F& srcLoc = src->location();
    const Point3F& dstLoc = dst->location();
    Point3F high, low;
    if (srcLoc.z > dstLoc.z) {
        high = srcLoc;
        low = dstLoc;
        edge.setDown(edge.isJetting());
    } else {
        high = dstLoc;
        low = srcLoc;
    }
    // Horizontal + some extra (to get the lateral velocity going) + vertical.
    const float absHeight = (high.z - low.z);
    (high -= low).z = 0;
    const float lateral = len(high);
    edge.mDist = modifiedDistance(lateral, absHeight);

    // The lateral distance for hops downward, a little extra for buffer.
    if (edge.isDown()) edge.setLateral(uint8_t(modifiedLateral(lateral)));

    // Conservative so edges evaluate the same both ways for partitioning.
    if (src->flat() && dst->flat()) edge.setJump(true);
}

// The indices are backwards from what's intuitive (GraphBridgeData).
void JetManager::replaceEdge(GraphBridgeData& B) {
    GraphNode* src = gNavGraph->lookupNode(B.nodes[1]);
    GraphNode* dst = gNavGraph->lookupNode(B.nodes[0]);
    GraphEdge* edge = src->getEdgeTo(dst);
    if (B.isUnreachable()) {
        edge->setSteep(true);
        edge->setJetting();
        edge->setImpossible();
    } else {
        edge->setJetting();
        edge->setHop(B.jetClear);
        initEdge(*edge, src, dst);
    }
}

// The scale factor on jetting edges for deciding when to do them.
float JetManager::calcJetScale(const GraphNode* from, const GraphNode* to) const {
    const float zdiff = (to->location().z - from->location().z);
    if (zdiff > 0) {
        // Base 3.0, plus 1 for every 10 meters going up beyond 20.
        const float per10 = mapValueLinear(zdiff, 20.0f, 120.0f, 0.0f, 10.0f);
        return 3.0f + per10;
    }
    return 1.4f;
}

static constexpr float GravityNum = 0.625f;

// How far thrust and duration go.
float JetManager::calcJetRating(float thrust, float duration) {
    const float tSqrd = duration * duration;
    thrust -= GravityNum;
    // While applying the continuous force-
    const float pureJetDist = (0.5f * thrust * tSqrd * TickSec);
    // The final velocity carries up once the jet is gone.
    const float finalVel = (thrust * duration);
    const float driftUp = (0.5f * finalVel / GravityNum) * finalVel;
    return (pureJetDist + driftUp * TickSec);
}

// 0 - without jumping, 1 - can jump. A little conservative (duration is
// assumed to last as long as energy; recharge is the buffer).
void JetManager::calcJetRatings(float* ratings, const Ability& ability) {
    ratings[0] = calcJetRating(ability.acc, ability.dur);
    ratings[1] = ratings[0] + (ability.v0 * ability.dur * TickSec);
}

const float* JetManager::getRatings(const ID& jetCaps) { return mPartitions[jetCaps.id].ratings; }

GraphPartition::Answer JetManager::reachable(const ID& jetCaps, int32_t from, int32_t to) {
    return mPartitions[jetCaps.id].reachable(from, to);
}

// The share of energy the hop needs, by a binary search (eight iterations
// get within 1%).
float JetManager::estimateEnergy(const ID& jetCaps, const GraphEdge* edge) {
    JetPartition& J = mPartitions[jetCaps.id];
    Ability interpAbility = J.ability;
    float lower = 0.0f, upper = 1.0f;
    float ratings[2];
    for (int32_t i = 0; i < 8; i++) {
        const float interpPercent = ((lower + upper) * 0.5f);
        interpAbility.dur = (interpPercent * J.ability.dur);
        calcJetRatings(ratings, interpAbility);
        if (edge->canJet(ratings))
            upper = interpPercent;
        else
            lower = interpPercent;
    }
    return upper;
}

//-------------------------------------------------------------------------------------
//                Indoor smoothing (graphSmooth.cc)

static constexpr float SegCheck3D = 4.0f;
static constexpr float SegCheck2D = 0.5f;
static constexpr float RoomNeeded = 0.5f;
static constexpr float InsideThreshSquared = RoomNeeded * RoomNeeded;

// Called first.
bool NavigationGraph::useVolumeTraverse(const GraphNode* from, const GraphEdge* to) {
    if (mHaveVolumes)
        if (from->canHaveBorders())      // i.e (indoor | transient)
            if (from->volumeIndex() >= 0)
                if (to && !to->isJetting() && to->isBorder()) return true;
    return false;
}

// Whether the line from currLoc to seekLoc passes through the border (2D:
// the border normal is parallel to the ground).
static bool canFitThrough(const Point3F& currLoc, const Point3F& seekLoc, const GraphBoundary& border) {
    Point3F seek2D = seekLoc, curr2D = currLoc;
    (seek2D -= border.seg[0]).z = 0;
    (curr2D -= border.seg[0]).z = 0;
    const float seekDot = mDot(seek2D, border.normal);
    const float currDot = mDot(curr2D, border.normal);
    if (seekDot > 0.03f && currDot < -0.03f) {
        const float interp = -currDot / (seekDot - currDot);
        const Point3F crossPt = scaleBetween(currLoc, seekLoc, interp);
        // The crossing point well inside: far enough from the endpoints, and
        // the vectors to them pointing away from each other.
        Point3F from0 = crossPt, from1 = crossPt;
        (from0 -= border.seg[0]).z = 0;
        (from1 -= border.seg[1]).z = 0;
        if (lenSquared(from0) > InsideThreshSquared)
            if (lenSquared(from1) > InsideThreshSquared)
                if (mDot(from0, from1) < -0.9f) return true;
    }
    return false;
}

bool NavigationGraph::volumeTraverse(const GraphNode* from, const GraphEdge* to, NavigationPath::State& S) {
    bool canAdvance = false;
    const GraphBoundary& border = mBoundaries[(size_t)to->mBorder];
    const Point3F midpoint = (border.seg[0] + border.seg[1]) * 0.5f;
    BitSet32& visit = S.visit[(size_t)(S.curSeekNode - 1)];

    // The node advancer skipped this fromNode: just get to the other side.
    if (visit.test(NavigationPath::State::Skipped)) {
        const float dot = mDot(S.hereLoc - border.seg[0], border.normal);
        if (dot > 0.05f) canAdvance = true;
    } else {
        if (inNodeVolume(from, S.hereLoc)) {
            visit.set(NavigationPath::State::Visited);
            // Near the (really large) crossing line: head across, else seek.
            const Point3F paraNormal = (border.normal * 33.0f);
            LineSegment crossingLine(midpoint - paraNormal, midpoint + paraNormal);
            // Only head out once (this flipped on small nodes near walls).
            if (!visit.test(NavigationPath::State::Outbound)) {
                if (crossingLine.botDistCheck(S.hereLoc, SegCheck3D, SegCheck2D))
                    visit.set(NavigationPath::State::Outbound);
                else
                    S.seekLoc = border.seekPt;
            }
            if (visit.test(NavigationPath::State::Outbound)) S.seekLoc = midpoint + (border.normal * 2.0f);
        } else {
            // The source was visited; we're in the next node: can we track
            // the next segment?
            if (visit.test(NavigationPath::State::Visited)) {
                LineSegment borderSeg(border.seg[0], border.seg[1]);
                if (!borderSeg.botDistCheck(S.hereLoc, SegCheck3D, 0.2f)) {
                    // A jetting or laid walk connection next: go to the node
                    // location before advancing.
                    if (S.nextEdgePrecise) {
                        S.seekLoc = lookupNode(to->mDest)->location();
                        if (within_2D(S.hereLoc, S.seekLoc, 0.3f)) canAdvance = true;
                    } else {
                        canAdvance = true;
                    }
                } else {
                    S.seekLoc = (midpoint + (border.normal * 2.0f));
                }
            } else {
                Point3F inVec = (border.seg[1] - border.seg[0]);
                const float l = len(inVec);
                normalize(inVec, l > 1.2f ? 0.4f : l / 3);     // the 'pulled in' segment
                LineSegment borderSeg(border.seg[0] + inVec, border.seg[1] - inVec);
                if (borderSeg.botDistCheck(S.hereLoc, SegCheck3D, SegCheck2D))
                    canAdvance = true;
                else
                    S.seekLoc = from->location();
            }
        }
    }
    return canAdvance;
}

// Further skips from the current location through outbound segments (one
// at a time; the advancement happens later).
int32_t NavigationGraph::checkIndoorSkip(NavigationPath::State& S) {
    const int32_t startingFrom = (S.curSeekNode - 1);
    int32_t numberSkipped = 0;
    if (!S.visit[(size_t)startingFrom].test(NavigationPath::State::Skipped)) {
        for (int32_t from = startingFrom; from < (int32_t)S.edges.size() - 1; from++) {
            GraphEdge* fromOut = S.edges[(size_t)from];
            GraphEdge* toOut = S.edges[(size_t)from + 1];
            if (fromOut->isBorder() && toOut->isBorder()) {
                const GraphBoundary& toBorder = mBoundaries[(size_t)toOut->mBorder];
                const GraphBoundary& fromBorder = mBoundaries[(size_t)fromOut->mBorder];
                // The segment to the TO outbound place passes through FROM outbound?
                if (canFitThrough(S.hereLoc, toBorder.seekPt, fromBorder)) {
                    // The first from has already been visited....
                    if (from > startingFrom) {
                        S.seekLoc = toBorder.seekPt;
                        S.visit[(size_t)from].set(NavigationPath::State::Skipped);
                        numberSkipped++;
                        continue;
                    }
                }
            }
            break;
        }
    }
    return numberSkipped;
}

//-------------------------------------------------------------------------------------
//                NavigationPath (graphPath.cc)

// Through the construct function: reconstructed on mission cycle.
NavigationPath::NavigationPath() { constructThis(); }

void NavigationPath::constructThis() {
    mState.constructThis();
    mCurEdge = nullptr;
    mTimeSlice = 0;
    mSaveSeekNode = -1;
    mSearchDist = 0.0f;
    mUserStuck = true;
    mForceSearch = true;
    mRepathCounter = 10000;
    mSaveDest = {1e7f, 1e7f, 1e7f};
    setRedoDist();
    mPctThreshSqrd = 1.0f;
    mPathIndoors = mAreIndoors = false;
    mAwaitingSearch = false;
    mTeam = 0;
    mBusyJetting = false;
    mSearchWasValid = true;
    mAdjustSeek = {0, 0, 0};
    mSavedEndpoints[0] = mSavedEndpoints[1] = {0, 0, 0};
    mSaveLastEdge = GraphEdge();
    mStopCounter = 0;
    mCastZ = mCastAng = 0.0;
    mCastAverage = {0, 0, 0};
    mEstimatedEdge = nullptr;
    mEstimatedEnergy = 0.0f;
    mFindHere.init();
}

// The state the smoothing code (graphSmooth) works on.
void NavigationPath::State::constructThis() {
    thisEdgeJetting = false;
    nextEdgeJetting = false;
    nextEdgePrecise = false;
    curSeekNode = 0;
    seekLoc = {0, 0, 0};
    path.clear();
    visit.clear();
    edges.clear();
    hereLoc = {0, 0, 0};
    destLoc = {0, 0, 0};
}

// The node along the path, undoing any bit-flipped transient indices.
GraphNode* NavigationPath::getNode(int32_t idx) {
    const int32_t node = mState.path[(size_t)idx];
    const int32_t toggle = -int32_t(node < 0);
    return gNavGraph->lookupNode(node ^ toggle);
}

// The location in the path; transient endpoints are saved.
Point3F NavigationPath::getLoc(int32_t idx) {
    const int32_t node = mState.path[(size_t)idx];
    if (node >= 0) return gNavGraph->lookupNode(node)->location();
    return mSavedEndpoints[idx > 0];
}

// The endpoints of the search (flagged with negative indices). The edges
// are for advancing; the last one (into the popped destination transient)
// is saved.
void NavigationPath::saveEndpoints(TransientNode* srcNode, TransientNode* dstNode) {
    mSavedEndpoints[0] = srcNode->location();
    mState.path.front() ^= int32_t(-1);
    if (dstNode) {
        mSavedEndpoints[1] = dstNode->location();
        mState.path.back() ^= int32_t(-1);
    }
    mState.edges.assign((size_t)std::max((int32_t)mState.path.size() - 1, 0), nullptr);
    for (size_t i = mState.edges.size(); i > 0; i--) mState.edges[i - 1] = getNode((int32_t)i - 1)->getEdgeTo(getNode((int32_t)i));
    if (!mState.edges.empty()) {
        mSaveLastEdge = *mState.edges.back();
        mState.edges.back() = &mSaveLastEdge;
    }
}

// For the renderer (and the node marking the randomization uses).
void NavigationPath::markRenderPath() {
    for (int32_t i = std::max(mState.curSeekNode - 1, 0); i < (int32_t)mState.path.size(); i++) getNode(i)->setOnPath();
}

// The path search off the transient nodes.
void NavigationPath::computePath(TransientNode& srcNode, TransientNode& dstNode) {
    mSearchDist = 0.0f;
    mSearchWasValid = false;
    // Search if the push says these two can (probably) reach.
    if (gNavGraph->pushTransientPair(srcNode, dstNode, mTeam, mJetCaps)) {
        mState.path.clear();
        mState.curSeekNode = 0;
        GraphSearch* searcher = gNavGraph->getMainSearcher();
        searcher->setAStar(true);
        searcher->setTeam(mTeam);
        searcher->setThreats(gNavGraph->getThreatSet((int32_t)mTeam));
        searcher->setRandomize(true);
        searcher->setRatings(gNavGraph->jetManager().getRatings(mJetCaps));
        searcher->performSearch(&srcNode, &dstNode);

        if (searcher->getPathIndices(mState.path)) {
            mSearchDist = searcher->searchDist();
            mState.visit.assign(mState.path.size(), BitSet32());
            saveEndpoints(&srcNode, &dstNode);
            mState.curSeekNode = 1;
            setEdgeConstraints();
            mAwaitingSearch = false;
            mSearchWasValid = true;
        } else {
            // Force field management updates this team's partition.
            if (gNavGraph->haveForceFields()) gNavGraph->newPartition(searcher, mTeam);
        }
    }
    // Unconnects from graph.
    gNavGraph->popTransientPair(srcNode, dstNode);
}

void NavigationPath::afterCompute() {
    mSaveDest = mState.destLoc;
    mForceSearch = false;
    mRepathCounter = 0;
    if (mRedoMode == OnPercent) {
        const float pctThresh = (mRedoPercent * mSearchDist);
        mPctThreshSqrd = (pctThresh * pctThresh);
    }
    mUserStuck = false;
}

bool NavigationPath::updateTransients(TransientNode& hereNode, TransientNode& destNode, bool forceRedo) {
    // Nodes could be invalid-
    if (mHook.iGrowOld()) mLocateHere.reset(), mLocateDest.reset();
    if (forceRedo) mLocateHere.forceCheck(), mLocateDest.forceCheck();

    // All the work of figuring out how to connect-
    mLocateHere.update(mState.hereLoc);
    mLocateDest.update(mState.destLoc);

    hereNode.setEdges(mLocateHere.getEdges());
    destNode.setEdges(mLocateDest.getEdges());
    hereNode.setClosest(mLocateHere.bestMatch());
    destNode.setClosest(mLocateDest.bestMatch());
    hereNode.setLoc(mState.hereLoc);
    destNode.setLoc(mState.destLoc);

    // Only hook one way (the Push-Search-Pop of computePath()).
    bool needToJet;
    if (mLocateHere.canHookTo(mLocateDest, needToJet)) {
        GraphEdge& edge = hereNode.pushEdge(&destNode);
        if (needToJet) edge.setJetting();
        gNavGraph->jetManager().initEdge(edge, &hereNode, &destNode);
    }
    return forceRedo;       // not used
}

static constexpr int32_t SearchWaitTicks = 20;
static constexpr int32_t AllowVisitTicks = 21;

// Periodic path updates; true if there are nodes to traverse.
bool NavigationPath::checkPathUpdate(TransientNode& hereNode, TransientNode& destNode) {
    bool recompute = mForceSearch;
    const bool changedSeekNode = (mSaveSeekNode != mState.curSeekNode);
    mSaveSeekNode = mState.curSeekNode;

    if (!recompute) {
        const float threshold = (mRedoMode == OnDist ? mRedoDistSqrd : mPctThreshSqrd);
        const bool canSearch = (++mRepathCounter > SearchWaitTicks) && !userMustJet();
        bool needSearch = (lenSquared(mState.destLoc - mSaveDest) > threshold);
        if (mUserStuck) needSearch = true;
        if (needSearch) {
            // A little extra wait, or search when the node was updated.
            if (canSearch && (changedSeekNode || mRepathCounter > AllowVisitTicks))
                recompute = true;
            else
                mAwaitingSearch = true;
        }
    }
    if (recompute) mAwaitingSearch = true;

    updateTransients(hereNode, destNode, recompute);

    if (recompute) {
        computePath(hereNode, destNode);
        afterCompute();
    }

    if (weAreIndoors()) {
        mAreIndoors = true;
    } else {
        if (checkOutsideAdvance()) advanceByOne();
    }

    // Update path
    if (mState.curSeekNode < (int32_t)mState.path.size()) {
        GraphNode* fromNode = (mState.curSeekNode > 0 ? getNode(mState.curSeekNode - 1) : nullptr);
        if (fromNode && gNavGraph->useVolumeTraverse(fromNode, mCurEdge)) {
            int32_t adv = gNavGraph->checkIndoorSkip(mState);
            if (adv) {
                while (adv--) advanceByOne();
            } else {
                if (gNavGraph->volumeTraverse(fromNode, mCurEdge, mState))
                    if (canAdvance()) advanceByOne();
            }
        } else {
            mState.seekLoc = getLoc(mState.curSeekNode);
            const float threshold = visitRadius() + 0.22f;
            if (within_2D(mState.hereLoc, mState.seekLoc, threshold) && canAdvance()) {
                advanceByOne();
                if (mState.curSeekNode < (int32_t)mState.path.size())
                    mState.seekLoc = getLoc(mState.curSeekNode);    // re-fetch!
            }
        }
    }
    return (mState.curSeekNode < (int32_t)mState.path.size());
}

// The consumer calls this every frame. Returns whether the last search was
// valid.
bool NavigationPath::updateLocations(const Point3F& here, const Point3F& there) {
    // Indoor nodes we are below don't match, hence the raise (the wall scan
    // below knows these numbers).
    (mState.hereLoc = here).z += 0.4f;
    (mState.destLoc = there).z += 0.4f;

    mAreIndoors = false;

    if (!NavigationGraph::gotOneWeCanUse()) {
        mState.seekLoc = mState.destLoc;
        mSearchWasValid = false;
    } else {
        // The threat manager needs frequent calls-
        gNavGraph->threats()->monitorThreats();
        gNavGraph->monitorForceFields();
        // Hook nodes are fetched every frame (graph revisions).
        if (!checkPathUpdate(mHook.getHook1(), mHook.getHook2())) mState.seekLoc = mState.destLoc;
        markRenderPath();
    }

    // The movement code sets this while jetting, for one frame each.
    mBusyJetting = false;

    // Busy-dec the stop counter; wall avoidance when non-zero.
    if (--mStopCounter <= 0)
        mStopCounter = 0;
    else
        checkWallAvoid();

    return mSearchWasValid;
}

// Assumes a graph and a node within the list being sought.
int32_t NavigationPath::checkOutsideAdvance() {
    if (canAdvance()) {
        const int32_t seekNode = mState.curSeekNode + 1;
        if (seekNode < (int32_t)mState.path.size() && !mState.nextEdgeJetting) {
            // Can we get to the next one (2D)? Registered threats first.
            Point3F srcLoc(mState.hereLoc);
            Point3F dstLoc(getLoc(seekNode));
            if (gNavGraph->threats()->sanction(srcLoc, dstLoc, (int32_t)mTeam)) {
                srcLoc.z = dstLoc.z = 0.0f;
                const float pct = gNavGraph->mTerrainInfo.checkOpenTerrain(srcLoc, dstLoc);
                if (pct == 1.0f) return 1;
            }
        }
    }
    return 0;
}

bool NavigationPath::canAdvance() {
    if (mState.curSeekNode < (int32_t)mState.path.size())
        if (!mState.thisEdgeJetting) return true;
    return false;
}

float NavigationPath::visitRadius() {
    if (mState.nextEdgePrecise || mAreIndoors || mPathIndoors) return 0.4f;
    return 2.6f;
}

// Whether the next destination is a jetting edge. Only called when a path
// is computed or advanced.
void NavigationPath::setEdgeConstraints() {
    mState.thisEdgeJetting = false;
    mState.nextEdgeJetting = false;
    mState.nextEdgePrecise = false;
    mPathIndoors = false;
    mCurEdge = nullptr;

    if (mState.curSeekNode > 0 && mState.curSeekNode < (int32_t)mState.path.size()) {
        GraphNode* curSeekNode = getNode(mState.curSeekNode);
        GraphNode* prevSeekNode = getNode(mState.curSeekNode - 1);
        mCurEdge = mState.edges[(size_t)(mState.curSeekNode - 1)];
        if (curSeekNode->indoor() || prevSeekNode->indoor()) mPathIndoors = true;

        mState.thisEdgeJetting = mCurEdge->isJetting();
        // The hop over is a U8 with 3 bits of precision.
        if (mState.thisEdgeJetting) {
            mNavJetting.init();
            if (mCurEdge->hasHop()) mNavJetting.mHopOver = mCurEdge->getHop();
        }
        if (mState.thisEdgeJetting) mState.seekLoc = getLoc(mState.curSeekNode);

        if (mState.curSeekNode + 1 < (int32_t)mState.path.size()) {
            GraphEdge* nextEdge = mState.edges[(size_t)mState.curSeekNode];
            mState.nextEdgeJetting = nextEdge->isJetting();
            // Go to the node location itself.
            mState.nextEdgePrecise = (mState.nextEdgeJetting || !nextEdge->isBorder());
        }
    }
}

// A visited node is marked for avoidance (path randomizing); not large
// terrain nodes or transients.
void NavigationPath::randomization() {
    GraphNode* node = getNode(mState.curSeekNode);
    if (!node->transient() && node->getLevel() <= 2) node->setAvoid(60000);
}

void NavigationPath::advanceByOne() {
    randomization();
    mState.curSeekNode++;
    setEdgeConstraints();
}

// The user is done jetting.
void NavigationPath::informJetDone() {
    randomization();
    mState.curSeekNode++;
    setEdgeConstraints();
}

bool NavigationPath::userMustJet() const { return (!mState.path.empty() && mState.thisEdgeJetting); }

void NavigationPath::forceSearch() { mForceSearch = true; }

// The randomization gets the user off the path: back up through any that
// were skipped, then forward by one.
void NavigationPath::informStuck(const Point3F&, const Point3F&) {
    int32_t start = std::max(mState.curSeekNode - 1, 1);
    const int32_t end = std::min(mState.curSeekNode + 1, (int32_t)mState.path.size() - 1);
    while (start > 1 && mState.visit[(size_t)start].test(State::Skipped)) start--;
    while (start < end) {
        GraphNode* node = getNode(start++);
        node->setAvoid(15 * 1000, true);
    }
    mUserStuck = true;
}

// gVelSquaredThresh is -1 in the engine ("needs more testing"), so the stop
// counter never climbs past its cap here.
void NavigationPath::informProgress(const Point3F& vel) {
    static constexpr float gVelSquaredThresh = -1.0f;
    const float velSquared = lenSquared(vel);
    if (velSquared < gVelSquaredThresh) {
        mStopCounter += 2;
    } else {
        // Limits how much we have to count down (the fade below).
        mStopCounter = std::min(mStopCounter, 7);
    }
}

// Rotating LOS checks (moving up and down too) for walls to move away from
// while the stop counter persists.
void NavigationPath::checkWallAvoid() {
    static const double sCycleAngle = 137.3 * 3.14159265358979323846 / 180.0;
    static constexpr double sCycleHeight = 0.37747;
    static constexpr uint32_t sCycleMask = (InteriorObjectType | TerrainObjectType);
    static constexpr float sAverageTheNew = (1.0f / 8.0f);
    // (1.0 - sAverageTheOld) in the engine: its own zero-initialized self.
    static constexpr float sAverageTheOld = 1.0f;
    constexpr double M_2PI_ = 3.14159265358979323846 * 2.0;

    if (mStopCounter > 3) {
        if ((mCastAng += sCycleAngle) > M_2PI_) mCastAng -= M_2PI_;
        if ((mCastZ += sCycleHeight) > 1.9) mCastAng -= 1.9;   // (sic)
        const float angle32 = float(mCastAng);
        const Point3F startPoint{mState.hereLoc.x, mState.hereLoc.y, mState.hereLoc.z + float(mCastZ)};
        Point3F lineOut{std::cos(angle32), std::sin(angle32), 0};
        (lineOut *= 7.0f) += startPoint;
        // APPROXIMATION: on a miss the engine's RayInfo point is left unset;
        // here it is the origin. (Unreachable: see informProgress.)
        NavRayInfo coll;
        navCastRay(startPoint, lineOut, sCycleMask, coll);
        coll.point -= startPoint;
        coll.point *= sAverageTheNew;
        mCastAverage *= sAverageTheOld;
        mCastAverage += coll.point;
    } else {
        // The first frames free or stuck: fade down the average.
        mCastAverage *= (2.0f / 3.0f);
    }
}

// The location to seek (away from a collision point while stopped).
const Point3F& NavigationPath::getSeekLoc(const Point3F&) {
    if (mStopCounter) {
        mAdjustSeek = mCastAverage;
        mAdjustSeek *= float(mStopCounter);
        mAdjustSeek += mState.hereLoc;
    } else {
        mAdjustSeek = mState.seekLoc;
    }
    return mAdjustSeek;
}

// The jetting ability, to the jet manager (the AI code reads it off the
// player).
void NavigationPath::setJetAbility(const JetManager::Ability& ability) {
    if (gNavGraph) {
        if (gNavGraph->jetManager().update(mJetCaps, ability)) {
            // A bunch of work just happened, let's forestall searches a little-
            mRepathCounter = std::min(SearchWaitTicks >> 1, mRepathCounter);
        }
    }
}

// How far across open terrain from src to dst (dstLoc is updated): a share
// from 0 to 1.
float NavigationPath::checkOpenTerrain(const Point3F& srcLoc, Point3F& dstLoc) {
    if (NavigationGraph::gotOneWeCanUse()) return gNavGraph->mTerrainInfo.checkOpenTerrain(srcLoc, dstLoc);
    dstLoc = srcLoc;
    return 0.0f;
}

// A point dist along the path.
Point3F NavigationPath::getLocOnPath(float dist) {
    Point3F currLoc(mState.hereLoc);
    for (int32_t i = mState.curSeekNode; i < (int32_t)mState.path.size() && dist > 0.01f; i++) {
        const Point3F nextLoc = getLoc(i);
        Point3F diffVec = nextLoc;
        const float l = len(diffVec -= currLoc);
        if (l < dist) {
            currLoc = nextLoc;
            dist -= l;
        } else {
            currLoc += (diffVec *= (dist / l));
            break;
        }
    }
    return currLoc;
}

// The distance remaining on the path, capped at maxCare.
float NavigationPath::distRemaining(float maxCare) {
    Point3F currLoc(mState.hereLoc);
    float dist = 0.0f;
    for (int32_t i = mState.curSeekNode; i < (int32_t)mState.path.size(); i++) {
        const Point3F nextLoc = getLoc(i);
        if ((dist += len(nextLoc - currLoc)) > maxCare) return maxCare;
        currLoc = nextLoc;
    }
    return dist;
}

// The next jetting edge within maxDist: its estimated energy (worked out
// once per edge).
float NavigationPath::jetWillNeedEnergy(float maxDist) {
    if (!mState.thisEdgeJetting) {
        float accumDist = 0.0f;
        for (int32_t i = mState.curSeekNode; i < (int32_t)mState.edges.size(); i++) {
            // len() to the first node only (the bot's moving), else the edge.
            float legDist;
            if (i == mState.curSeekNode)
                legDist = len(getLoc(i) - mState.hereLoc);
            else
                legDist = mState.edges[(size_t)(i - 1)]->mDist;
            if ((accumDist += legDist) < maxDist) {
                GraphEdge* nextEdge = mState.edges[(size_t)i];
                if (nextEdge->isJetting()) return estimateEnergy(nextEdge);
            }
        }
    }
    return 0.0f;
}

float NavigationPath::estimateEnergy(const GraphEdge* edge) {
    if (mEstimatedEdge != edge) {
        mEstimatedEnergy = gNavGraph->jetManager().estimateEnergy(mJetCaps, edge);
        mEstimatedEdge = edge;
    }
    return mEstimatedEnergy;
}

// True if outside, with the roam radius.
bool NavigationPath::locationIsOutdoors(const Point3F& location, float* roamRadPtr) {
    float roamRadius = 1e12f;      // Default is outdoors, with unlimited roam room.
    if (NavigationGraph::gotOneWeCanUse()) {
        if (!gNavGraph->haveTerrain()) return false;
        const Point3F tempLocation{location.x, location.y, 0.0f};
        if (gNavGraph->mTerrainInfo.inGraphArea(tempLocation)) {
            SphereF sphere;
            if (gNavGraph->mTerrainInfo.locToIndexAndSphere(sphere, tempLocation) >= 0)
                roamRadius = sphere.radius;
            else
                return false;
        }
    }
    if (roamRadPtr) *roamRadPtr = roamRadius;
    return true;
}

// False without a graph.
bool NavigationPath::weAreIndoors() const {
    if (NavigationGraph::gotOneWeCanUse()) {
        if (!gNavGraph->haveTerrain())
            return true;
        else if (gNavGraph->mTerrainInfo.inGraphArea(mState.hereLoc))
            return !gNavGraph->findTerrainNode(mState.hereLoc);
    }
    return false;
}

bool NavigationPath::getPathNodeLoc(int32_t ahead, Point3F& loc) {
    const int32_t dest = mState.curSeekNode + ahead;
    if (dest < (int32_t)mState.path.size() && dest > 0) {
        GraphEdge* beforeEdge = mState.edges[(size_t)(dest - 1)];
        if (!beforeEdge->isJetting()) {
            if (beforeEdge->isBorder())
                loc = gNavGraph->getBoundary(beforeEdge->mBorder).seekPt;
            else
                loc = getNode(dest)->location();
            return true;
        }
    }
    return false;
}

// Does the bot have to jet into a vehicle?
bool NavigationPath::intoMount() const {
    if (mLocateDest.isMounted())
        if (mState.curSeekNode == (int32_t)mState.path.size() - 1) return true;
    return false;
}

bool NavigationPath::canReachLoc(const Point3F& dst) {
    if (NavigationGraph::gotOneWeCanUse()) {
        // mFindHere remembers the closest node searches.
        mFindHere.setPoint(mState.hereLoc, mFindHere.closest());
        FindGraphNode findDest(dst);
        return gNavGraph->canReachLoc(mFindHere, findDest, mTeam, mJetCaps);
    }
    return true;
}

void NavigationPath::missionCycleCleanup() {
    constructThis();
    mJetCaps.reset();
    mNavJetting.init();
    mHook.reset();
    mFindHere.init();
    mLocateHere.cleanup();
    mLocateDest.cleanup();
}
