#include "sim/nav_graph.h"
#include "ai/graph.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/timer.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/shape_base.h"
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

std::vector<uint8_t> readFile(const std::string& path) {
    if (fileReader()) return fileReader()(path);
    return Engine::instance().fs().read(path.c_str());
}

} // namespace Nav

namespace {

float dAtof(const std::string& text) { return (float)std::atof(text.c_str()); }
int32_t dAtoi(const std::string& text) { return (int32_t)std::atoi(text.c_str()); }
bool dAtob(const std::string& text) {
    return strcasecmp(text.c_str(), "true") == 0 || dAtof(text) != 0.0f;
}

void conPrint(const char* text) { Console::instance().printf(LogLevel::Info, "%s", text); }

// graph.cc TrackPatch: script functions routed through here so the profiler
// sees them, with timing kept in $patchNAvg / Total / Last / Calls.
struct TrackPatch {
    enum { MaxDepth = 12 };
    uint32_t recursed[MaxDepth + 1] = {};
    uint32_t totalMS = 0, numCalls = 0, depth = 0, maxDepth = 0, lastMS = 0;
    float average = 0.0f;

    VMValue patch(TorqueScript& ts, const std::vector<VMValue>& args, const char* prefix) {
        if (depth <= MaxDepth) recursed[depth]++;
        if (++depth > maxDepth) maxDepth = depth;
        // Con::execute(argc - 1, argv + 1): the named function with the rest.
        const double saveMS = Timer::now() * 1000.0;
        const std::vector<VMValue> callArgs(args.begin() + 1, args.end());
        const VMValue result = ts.callFunction(args[0].toString(), callArgs);
        lastMS = (uint32_t)(Timer::now() * 1000.0 - saveMS);
        totalMS += lastMS;
        average = float(totalMS) / float(++numCalls);
        depth--;
        const std::string p = std::string("$") + prefix;
        ts.setGlobal(p + "Avg", VMValue(average));
        ts.setGlobal(p + "Total", VMValue((int)totalMS));
        ts.setGlobal(p + "Last", VMValue((int)lastMS));
        ts.setGlobal(p + "Calls", VMValue((int)numCalls));
        return result;
    }
};
TrackPatch gTrackProfPatch1, gTrackProfPatch2;

} // namespace

// ---------------------------------------------------------------------------
// Console functions (graph.cc consoleInit). The generation commands
// (saveGraph, setGround, prepLOS, makeLOS, findBridges, cullIslands,
// makeTables, assemble, genDebug, timeTest, check, the debug queries) and
// dumpInfo2File (graph build metrics to NavMetrics.log) are not ported.

void registerNavGraphNatives(TorqueScript& ts) {
    EngineObjects::registerClass("NavigationGraph", [] {
        auto graph = std::make_shared<NavigationGraph>();
        graph->onAdd();
        return graph;
    });
    using Args = std::vector<VMValue>;
    auto graphOf = [](const Args& args) -> NavigationGraph* {
        return args.empty() ? nullptr : EngineObjects::get<NavigationGraph>(args[0].toString());
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
        Point3F p{0, 0, 0};
        std::sscanf(text.c_str(), "%f %f %f", &p.x, &p.y, &p.z);
        return p;
    };
    auto format3 = [](const Point3F& p) {
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f", p.x, p.y, p.z);
        return std::string(buffer);
    };
    static const char bogusLocation[] = "0 0 500";

    ts.registerNative("NavigationGraph::randNode", [graphOf, argcOk, vec3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 6, "navGraph.randNode(pt, rad, indoor, outdoor);")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (!graph) return VMValue(-1);
        const int argc = (int)args.size() + 1;
        if (argc >= 4) {
            const Point3F pt = vec3(args[1].toString());
            const float radius = (float)dAtoi(args[2].toString());
            const bool inside = argc > 4 ? dAtob(args[3].toString()) : false;
            const bool outside = argc > 5 ? dAtob(args[4].toString()) : false;
            return VMValue(graph->randNode(pt, radius, inside, outside));
        }
        conPrint("Given a point, radius, and flags for indoor or outdoor");
        conPrint("inclusion, randNode returns a node index (-1 if not found).");
        conPrint("Note use navGraph.nodeLoc() to get location from index");
        return VMValue(-1);
    });

    ts.registerNative("NavigationGraph::nodeLoc", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.nodeLoc(nodeIndex);")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t index = dAtoi(args[1].toString());
            if (const Point3F* pt = graph->getSpawnLoc(index)) return VMValue(format3(*pt));
            Console::instance().printf(LogLevel::Info, "Invalid index (%d) passed to nodeLoc()", index);
        } else {
            conPrint("Gets the location of the specified node or spawn index");
        }
        return VMValue(bogusLocation);
    });

    ts.registerNative("NavigationGraph::randNodeLoc", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.randNodeLoc(nodeIndex);")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t index = dAtoi(args[1].toString());
            if (const Point3F* pt = graph->getRandSpawnLoc(index)) {
                Point3F point = *pt;
                NavigationGraph::adjustSpawnLoc(point);
                return VMValue(format3(point));
            }
            Console::instance().printf(LogLevel::Info, "Invalid index (%d) passed to %s()", index, "randNodeLoc");
        } else {
            conPrint("Finds a random location within the space of the given node");
        }
        return VMValue(bogusLocation);
    });

    ts.registerNative("NavigationGraph::numNodes", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.numNodes();")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        return VMValue(graph ? graph->numNodes() : 0);
    });

    ts.registerNative("NavigationGraph::numSpawns", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.numSpawns()")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        return VMValue(graph ? graph->numSpawns() : 0);
    });

    ts.registerNative("NavigationGraph::getSpawn", [graphOf, argcOk, format3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.getSpawn(which)")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (graph && args.size() == 2) {
            const int32_t which = dAtoi(args[1].toString());
            if (which >= 0 && which < graph->numSpawns()) {
                Point3F point = graph->getSpawn(which);
                NavigationGraph::adjustSpawnLoc(point);
                return VMValue(format3(point));
            }
            Console::instance().printf(LogLevel::Info, "Spawn index %d out of range!", which);
        }
        return VMValue("");
    });

    ts.registerNative("NavigationGraph::spawnInfo", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.spawnInfo();")) return VMValue("");
        if (NavigationGraph* graph = graphOf(args)) graph->printSpawnInfo();
        return VMValue("");
    });

    ts.registerNative("NavigationGraph::loadGraph", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.loadGraph();")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        return VMValue(graph && graph->loadGraph() ? 1 : 0);
    });

    ts.registerNative("NavigationGraph::makeGraph", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.makeGraph();")) return VMValue("");
        if (NavigationGraph* graph = graphOf(args)) graph->makeGraph();
        return VMValue(1);
    });

    // Installs the bridge edges onto a made graph.
    ts.registerNative("NavigationGraph::pushBridges", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "navGraph.pushBridges();")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (!graph) return VMValue(0);
        if (const int32_t islandsBefore = graph->numIslands()) {
            if (const char* errorText = graph->pushBridges()) {
                conPrint(errorText);
            } else {
                if (const int32_t islandsBridged = islandsBefore - graph->numIslands())
                    Console::instance().printf(LogLevel::Info, "%d islands have been bridged", islandsBridged);
                return VMValue(1);
            }
        } else {
            conPrint("Graph hasn't been made");
        }
        return VMValue(0);
    });

    ts.registerNative("NavigationGraph::setGenMode", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 3, "navGraph.setGenMode(nav|spawn);")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (graph && args.size() == 2)
            graph->setGenMode(!strcasecmp(args[1].toString().c_str(), "spawn"));
        else
            conPrint("Set graph generation mode to Nav (default) or Spawn");
        return VMValue(1);
    });

    // The stats half of navGraph.info(); the location half (marking nodes in
    // sight for the editor's rendering) is not ported.
    ts.registerNative("NavigationGraph::info", [graphOf, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 7, "navGraph.info([loc], [rad]);")) return VMValue("");
        NavigationGraph* graph = graphOf(args);
        if (!graph) return VMValue(0);
        const int32_t bridges = graph->numBridges();
        const int32_t edges = NavigationGraph::sTotalEdgeCount;
        const int32_t totalNodes = graph->numNodes();
        auto& con = Console::instance();
        con.printf(LogLevel::Info, "Graph Stats: %d nodes (%d outdoor)", totalNodes, graph->numOutdoor());
        con.printf(LogLevel::Info, "--> %d islands", graph->numIslands());
        con.printf(LogLevel::Info, "--> %d bridges", bridges);
        con.printf(LogLevel::Info, "--> %d edges", edges);
        con.printf(LogLevel::Info, "Graph load memory used:  %d", (int)NavigationGraph::sLoadMemUsed);
        con.printf(LogLevel::Info, "Edge alloc = (%d + %d + 2 x %d) x %d == %d", bridges, edges, totalNodes,
                   (int)sizeof(GraphEdge), (bridges + edges + 2 * totalNodes) * (int)sizeof(GraphEdge));
        con.printf(LogLevel::Info, "NavGraph structure = %d bytes", (int)sizeof(NavigationGraph));
        return VMValue(1);
    });

    // navGraph.preload(name, clamp): holds TextureHandles so the training
    // missions' skins stay loaded. Not ported: the server keeps no textures.
    ts.registerNative("NavigationGraph::preload", [argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 4, 4, "navGraph.preload(name,clamp);")) return VMValue("");
        return VMValue(1);
    });

    // Script registers the (static) turret threats with this.
    ts.registerNative("NavigationGraph::installThreat", [argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 5, "navGraph.installThreat(id, team [,R]);")) return VMValue("");
        const int argc = (int)args.size() + 1;
        // fetchThreatInfo
        if (NavigationGraph::gotOneWeCanUse() && argc >= 4) {
            if (ShapeBase* threat = EngineObjects::get<ShapeBase>(args[1].toString())) {
                const int32_t team = dAtoi(args[2].toString());
                const float rad = argc > 4 ? dAtof(args[3].toString()) : 50.0f;
                return VMValue(gNavGraph->installThreat(threat->handle(), team, rad) ? 1 : 0);
            }
        }
        conPrint("installThreat(): register permanent (static object) threat on the graph");
        return VMValue(0);
    });

    ts.registerNative("NavDetectForceFields", [argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 1, 1, "NavDetectForceFields();")) return VMValue("");
        if (NavigationGraph::gotOneWeCanUse()) gNavGraph->detectForceFields();
        return VMValue(0);
    });

    ts.registerNative("ProfilePatch1", [&ts, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 20, "ProfilePatch1(func, args...);")) return VMValue("");
        return gTrackProfPatch1.patch(ts, args, "patch1");
    });
    ts.registerNative("ProfilePatch2", [&ts, argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 20, "ProfilePatch2(func, args...);")) return VMValue("");
        return gTrackProfPatch2.patch(ts, args, "patch2");
    });

    ts.registerNative("WhereToLook", [argcOk, vec3](const Args& args) -> VMValue {
        if (!argcOk(args, 2, 2, "WhereToLook(playerLoc);")) return VMValue("");
        const Point3F point = vec3(args[0].toString());
        // We get a Z rotation back; the string is for setTransform().
        const float angle = -NavigationGraph::whereToLook(point);
        char buffer[120];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f 0 0 1 %f", point.x, point.y, point.z, angle);
        return VMValue(buffer);
    });

    ts.registerNative("navGraphExists", [argcOk](const Args& args) -> VMValue {
        if (!argcOk(args, 1, 1, "navGraphExists();")) return VMValue("");
        return VMValue(NavigationGraph::gotOneWeCanUse() || NavigationGraph::hasSpawnLocs() ? 1 : 0);
    });
}
