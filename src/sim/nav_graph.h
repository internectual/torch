#pragma once
// The spawn side of the Tribes 2 AI navigation graph (engine ai/graph*.cc,
// class NavigationGraph). A mission's `new NavigationGraph(NavGraph) {...}`
// loads terrains/<$CurrentMission>.spn (spawn mode, no bots) or
// terrains/<$CurrentMission>.nav plus the .spn (bots or single player) in
// NavigationGraph::onAdd. The retail spawn code queries it through
// navGraphExists(), navGraph.randNode() and navGraph.randNodeLoc().
//
// Ported: the .spn / .nav stream formats (NavigationGraph::load, version 8
// "BetterSpawnMode"), the spawn-list queries (SpawnLocations::getRandom,
// getSpawnLoc, getRandSpawnLoc, adjustSpawnLoc), whereToLook, and the
// engine's global random generator gRandGen (MRandomLCG). Not ported: the
// run-time node graph built from a .nav (makeGraph), so node-based queries
// on a graph without spawn data find nothing (see randNode).
#include "sim/engine_object.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Nav {

// math/mRandom.cc MRandomLCG: Park & Miller "minimal standard" LCG.
class RandomLCG {
public:
    RandomLCG();                       // seeded by generateSeed()
    explicit RandomLCG(int32_t seed) : mSeed(seed) {}
    void setSeed(int32_t seed) { mSeed = seed; }
    int32_t getSeed() const { return mSeed; }
    uint32_t randI();                  // 1 .. 2^31-2
    int32_t randI(int32_t i, int32_t n); // i..n inclusive
    float randF();                     // 0..1
    float randF(float i, float n);
    // The crude LCG the engine seeds new generators from (static state).
    static uint32_t generateSeed();

private:
    int32_t mSeed;
};

// The engine's gRandGen. It is shared with getRandom()/setRandomSeed()/
// getRandomSeed() (math/mathTypes.cc) and the AI code; its initial seed is
// the first generateSeed() value (setRandomSeed is never called by the
// retail scripts).
RandomLCG& gRandGen();
// MRandomLCG::setGlobalRandSeed (setRandomSeed([seed])) without journaling.
void setGlobalRandSeed(uint32_t seed);

struct Point3 {
    float x = 0, y = 0, z = 0;
};

// gServerContainer.castRay: the hit point and surface normal.
struct RayHit {
    Point3 point;
    Point3 normal{0, 0, 1};
};
using RayCastFn = std::function<bool(const Point3& start, const Point3& end, uint32_t mask, RayHit& hit)>;
// Without a ray caster every cast misses.
void setRayCaster(RayCastFn cast);
bool castRay(const Point3& start, const Point3& end, uint32_t mask, RayHit& hit);

// ResourceManager->openStream: the whole file, empty if missing. Defaults to
// Engine::instance().fs().read().
using FileReadFn = std::function<std::vector<uint8_t>(const std::string& path)>;
void setFileReader(FileReadFn read);

} // namespace Nav

class NavGraph : public EngineObject {
public:
    // graph.cc version history; only ChangedToFloorPlan on is loadable.
    enum Version {
        TerrainOnly, HandLaidInterior, AddedBridges, AddedPathTable, AddedLOSTable,
        ChangedToFloorPlan, TrimmedBridges, RevisedLOSToHash, BetterSpawnMode
    };

    // SpawnLocations::Sphere
    struct SpawnSphere {
        Nav::Point3 center;
        float radius = 0;
        bool inside = false;
        int32_t count = 0;
        int32_t offset = 0;
        int32_t res0 = 0;
    };
    // IndoorNodeInfo (the persisted indoor node)
    struct IndoorNodeInfo {
        uint32_t flags = 0;
        uint16_t unused = 0;
        int16_t antecedent = 0;
        Nav::Point3 pos;
    };
    // OutdoorNodeInfo (a consolidated terrain node)
    struct OutdoorNodeInfo {
        int8_t level = -1;
        uint8_t flags = 0;
        uint16_t height = 0;
        int16_t x = -1, y = -1;
    };

    NavGraph();
    ~NavGraph() override;

    // The graph the global queries use (gNavGraph): the latest constructed.
    static NavGraph* current();

    // NavigationGraph::onAdd: reads $GraphForceLoad, $OFFLINE_NAV_BUILD,
    // $CurrentMissionType, $HostGameBotCount and $CurrentMission, then loads.
    void onAdd();
    // onAdd without the script engine: spawn mode reads only the .spn.
    bool loadForMission(const std::string& missionName, bool spawnGraph);
    // NavigationGraph::loadGraph: the .nav (and then the .spn into it) or,
    // in spawn mode, the .spn alone.
    bool loadGraph(const std::string& missionName);
    // NavigationGraph::load on a whole stream.
    bool load(const std::vector<uint8_t>& bytes, bool isSpawn);
    bool loadSpawnGraph(const std::vector<uint8_t>& bytes) { return load(bytes, true); }
    // A .nav: load(bytes, false) then the run-time node counts of makeGraph.
    bool loadNavGraph(const std::vector<uint8_t>& bytes);
    // makeGraph(true), reduced to the run-time node counts.
    void makeGraph();

    // Queries (ai/graph.cc, ai/graphSpawn.cc).
    int32_t randNode(const Nav::Point3& point, float radius, bool indoor, bool outdoor);
    const Nav::Point3* getSpawnLoc(int32_t index) const;
    const Nav::Point3* getRandSpawnLoc(int32_t index) const;
    // randNodeLoc(): getRandSpawnLoc then adjustSpawnLoc (cast down to ground).
    bool randNodeLoc(int32_t index, Nav::Point3& out) const;
    static void adjustSpawnLoc(Nav::Point3& point);
    // NavigationGraph::whereToLook: the Z rotation to face (WhereToLook()
    // returns its negation).
    static float whereToLook(Nav::Point3 point);

    bool gotOneWeCanUse() const { return numOutdoor || numIndoor; }
    bool hasSpawnLocs() const { return !spawnList.empty(); }
    int32_t numNodes() const { return numOutdoor + numIndoor; }
    int32_t numSpawns() const { return (int32_t)spawnList.size(); }
    void printSpawnInfo() const;

    bool isSpawnGraph = false;
    int32_t version = -1;
    // SpawnLocations
    int32_t spawnRes0 = 0;
    std::vector<Nav::Point3> spawnList;
    std::vector<SpawnSphere> spheres;
    // Persisted NAV data kept by this port.
    std::vector<IndoorNodeInfo> indoorNodeInfo;
    std::vector<OutdoorNodeInfo> consolidated;
    int32_t edgeInfoCount = 0;
    int32_t bridgeCount = 0;
    bool haveTerrainGraph = false;
    // Run-time node counts after makeGraph (mNumOutdoor / mNumIndoor).
    int32_t numOutdoor = 0;
    int32_t numIndoor = 0;

private:
    void resetSpawnList();
};

// NavigationGraph console methods ("NavigationGraph::randNode", ...; args[0]
// is the object) and the global navGraphExists() / WhereToLook(); also
// registers the NavigationGraph engine class, which loads on creation.
void registerNavGraphNatives(class TorqueScript& ts);
