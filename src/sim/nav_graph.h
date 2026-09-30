#pragma once
// The engine services the AI navigation graph (ai/graph.h, class
// NavigationGraph) shares with the rest of the server: the global random
// generator gRandGen (MRandomLCG), the ray caster hook, the file reader, and
// the graph's console functions and methods (graph.cc consoleInit).
//
// A mission's `new NavigationGraph(NavGraph) {...}` loads
// terrains/<$CurrentMission>.spn (spawn mode, no bots) or
// terrains/<$CurrentMission>.nav plus the .spn (bots or single player) in
// NavigationGraph::onAdd, and for a NAV builds the run-time node graph.
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

// gServerContainer.castRay over the static world: the hit point and surface
// normal (the graph itself casts through navCastRay, ai/graph.h).
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
std::vector<uint8_t> readFile(const std::string& path);

} // namespace Nav

// NavigationGraph console methods ("NavigationGraph::randNode", ...; args[0]
// is the object) and the global functions (navGraphExists, WhereToLook,
// NavDetectForceFields, ProfilePatch1/2); also registers the
// NavigationGraph engine class, which loads on creation.
void registerNavGraphNatives(class TorqueScript& ts);
