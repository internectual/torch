// The run-time AI navigation graph on a shipped mission: BeggarsRun.nav
// loaded through the file system with its terrain, the node graph built,
// a Dijkstra / A* path found and checked for continuity, a NavigationPath
// driven to a path, and the console API. Skips when the Tribes 2 install is
// not present.
#include "ai/graph.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/nav_graph.h"
#include "sim/sim_state.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <cmath>
#include <cstdio>

using namespace NavMath;

int main() {
    ScriptEngine engine;
    assert(engine.init());
    auto* fs = new FileSystem;
    if (!fs->init({"/home/methodown/t2-linux/base"})) {
        std::printf("nav_graph_test: no Tribes 2 install, skipped\n");
        return 0;
    }
    // The stock install's base archives (terrains/*.nav, *.spn, *.ter).
    std::vector<std::string> archives;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/home/methodown/t2-linux/base", error))
        if (entry.path().extension() == ".vl2") archives.push_back(entry.path().string());
    std::sort(archives.begin(), archives.end());
    for (const auto& path : archives) {
        auto* vl2 = new Vl2Archive;
        if (vl2->open(path.c_str())) fs->addArchive(vl2);
        else delete vl2;
    }
    Engine::instance().filesys = fs;
    Engine::instance().scr = &engine;
    if (Nav::readFile("terrains/BeggarsRun.nav").empty()) {
        std::printf("nav_graph_test: terrains/BeggarsRun.nav missing, skipped\n");
        return 0;
    }
    TorqueScript* ts = engine.ts();
    ts->execute(
        "$CurrentMission = \"BeggarsRun\"; $CurrentMissionType = \"CTF\"; $HostGameBotCount = 1;"
        "new TerrainBlock(Terrain) { terrainFile = \"BeggarsRun.ter\"; squareSize = \"8\";"
        "  emptySquares = \"87960 88216 88472 107946 108202 108458\"; position = \"-1024 -1024 0\"; };"
        "new NavigationGraph(NavGraph) { conjoinAngleDev = \"45\"; };");

    assert(gNavGraph && NavigationGraph::gotOneWeCanUse());
    assert(!gNavGraph->isSpawnGraph() && gNavGraph->haveTerrain());
    const int32_t outdoor = gNavGraph->numOutdoor(), indoor = gNavGraph->numIndoor();
    std::printf("BeggarsRun: %d outdoor + %d indoor nodes, %d edges, %d islands (largest %d), %d bridges, LOS %d\n",
                outdoor, indoor, NavigationGraph::sTotalEdgeCount, gNavGraph->numIslands(), gNavGraph->largestIsland(),
                gNavGraph->numBridges(), gNavGraph->validLOSXref());
    assert(outdoor > 0 && indoor > 0 && gNavGraph->numNodes() == outdoor + indoor);
    assert(NavigationGraph::sTotalEdgeCount > gNavGraph->numNodes());
    assert(gNavGraph->validLOSXref());
    assert(gNavGraph->numSpawns() > 0);   // the .spn loaded into the NAV graph

    // Two far-apart nodes of the largest island.
    GraphNode* from = nullptr;
    GraphNode* to = nullptr;
    float farthest = -1;
    for (int32_t i = 0; i < gNavGraph->numNodes(); i++) {
        GraphNode* n = gNavGraph->lookupNode(i);
        if (n->island() != gNavGraph->largestIsland()) continue;
        if (!from) from = n;
        const float d = len(n->location() - from->location());
        if (d > farthest) farthest = d, to = n;
    }
    assert(from && to && from != to);

    GraphSearch* search = gNavGraph->getMainSearcher();
    search->setAStar(true);
    search->performSearch(from, to);
    std::vector<int32_t> path;
    assert(search->getPathIndices(path));
    assert(path.front() == from->getIndex() && path.back() == to->getIndex());
    float walked = 0;
    for (size_t i = 1; i < path.size(); i++) {
        GraphNode* a = gNavGraph->lookupNode(path[i - 1]);
        GraphNode* b = gNavGraph->lookupNode(path[i]);
        assert(a->getEdgeTo(b));     // continuous: every hop is a graph edge
        walked += len(b->location() - a->location());
    }
    std::printf("A* %d -> %d: %zu nodes, search dist %.1f, walked %.1f (straight %.1f)\n", from->getIndex(),
                to->getIndex(), path.size(), search->searchDist(), walked, farthest);
    assert(walked >= farthest - 0.01f);

    // A NavigationPath between the two locations (as a bot drives it).
    {
        NavigationPath navPath;
        JetManager::Ability ability;
        ability.acc = ability.dur = ability.v0 = 0;   // aiConnection's setPathCapabilities
        navPath.setJetAbility(ability);
        const Point3F src = from->location(), dst = to->location();
        bool valid = navPath.updateLocations(src, dst);
        std::vector<int32_t> last;
        navPath.getLastPath(last);
        std::printf("NavigationPath: valid %d, %zu nodes, dist %.1f, remaining %.1f, indoors %d\n", valid,
                    last.size(), navPath.searchDist(), navPath.distRemaining(), navPath.weAreIndoors());
        assert(valid && last.size() >= 2 && navPath.searchDist() > 0);
        assert(navPath.isPathCurrent() && navPath.distRemaining() > 0);
        assert(navPath.canReachLoc(dst));
        const Point3F seek = navPath.getSeekLoc({0, 0, 0});
        assert(std::isfinite(seek.x) && std::isfinite(seek.y) && std::isfinite(seek.z));
        navPath.missionCycleCleanup();
    }

    // LOS table queries.
    std::vector<Point3F> chokes;
    NavigationGraph::getChokePoints(from->location(), chokes, 10, 65);
    const Point3F losPoint =
        NavigationGraph::findLOSLocation(from->location(), to->location(), 0, SphereF(to->location(), 1e6f), 1e11f);
    std::printf("choke points from %d: %zu; LOS location toward %d: %.1f %.1f %.1f\n", from->getIndex(),
                chokes.size(), to->getIndex(), losPoint.x, losPoint.y, losPoint.z);

    // The console API.
    assert(ts->execute("navGraphExists();").toInt() == 1);
    assert(ts->execute("NavGraph.numNodes();").toInt() == gNavGraph->numNodes());
    assert(ts->execute("NavGraph.randNode(\"0 0 0\", 2000, true, true);").toInt() >= 0);
    ts->execute("NavDetectForceFields();");
    return 0;
}
