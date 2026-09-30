#pragma once
// The server's PathManager (sim/pathManager.cc) and Path::finishPath
// (sim/simPath.cc): each Path group's Markers, in sequence order, as
// positions and times the clients receive in PathManagerEvents.
#include "core/math.h"
#include <cstdint>
#include <vector>

class GameConnection;
class TorqueScript;

namespace PathManager {

struct PathEntry {
    uint32_t totalTime = 0;
    std::vector<Point3F> positions;
    std::vector<uint32_t> msToNext;
};

// gServerPathManager->mPaths (kept for the process, as the engine does).
const std::vector<PathEntry>& paths();
// PathManager::transmitPaths: every path to one connection (NewPaths).
void transmitPaths(GameConnection& connection);

} // namespace PathManager

// pathOnMissionLoadDone, NetConnection::transmitPaths / clearPaths.
void registerPathNatives(TorqueScript& ts);
