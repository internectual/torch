#include "sim/path_manager.h"
#include "sim/game_connection.h"
#include "sim/engine_classes.h"
#include "sim/net_object.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <algorithm>

namespace PathManager {

namespace {

std::vector<PathEntry>& serverPaths() {
    static std::vector<PathEntry> paths;
    return paths;
}

void writePath(TorqueBitWriter& w, const PathEntry& path) {
    w.writeU32(path.totalTime);
    w.writeU32((uint32_t)path.positions.size());
    for (size_t j = 0; j < path.positions.size(); ++j) {
        w.writeF32(path.positions[j].x);
        w.writeF32(path.positions[j].y);
        w.writeF32(path.positions[j].z);
        w.writeU32(path.msToNext[j]);
    }
}

// PathManagerEvent::pack: NewPaths (every path) or ModifyPath (one).
void postPathEvent(GameConnection& connection, bool newPaths, uint32_t modified) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = GameConnection::PathManagerEvent;
    event->pack = [newPaths, modified](TorqueBitWriter& w) {
        const auto& paths = serverPaths();
        if (w.writeFlag(newPaths)) {
            w.writeU32((uint32_t)paths.size());
            for (const auto& path : paths) writePath(w, path);
        } else {
            w.writeU32(modified);
            writePath(w, paths[modified]);
        }
    };
    connection.postEvent(event);
}

// PathManager::transmitPath: the changed path to every client that has
// the mission's paths.
void transmitPath(uint32_t id) {
    auto& engine = ScriptEngine::instance();
    ScriptObject* group = engine.findObject("ClientGroup");
    if (!group) return;
    const int count = group->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i) {
        auto* connection = EngineObjects::get<GameConnection>(group->internals["__child" + std::to_string(i)].toString());
        if (connection && connection->missionPathsSent) postPathEvent(*connection, false, id);
    }
}

// Path::finishPath: the markers in sequence order (cmpPathObject: anything
// else first; dQsort's order among equal keys is its own, the stable sort
// keeps the group's), then PathManager::updatePath.
void finishPath(ScriptObject* path) {
    auto& engine = ScriptEngine::instance();
    auto& paths = serverPaths();
    auto index = path->internals.find("__pathIndex");
    uint32_t id;
    if (index == path->internals.end()) {
        id = (uint32_t)paths.size();
        paths.emplace_back();
        path->internals["__pathIndex"] = VMValue((int32_t)id);
    } else {
        id = (uint32_t)index->second.toInt();
    }
    const int count = path->internals["__childCount"].toInt();
    std::vector<std::pair<ScriptObject*, VMValue>> members;
    for (int i = 0; i < count; ++i) {
        const VMValue key = path->internals["__child" + std::to_string(i)];
        members.emplace_back(engine.findObject(key.toString().c_str()), key);
    }
    auto isMarker = [](const ScriptObject* o) { return o && EngineClasses::isA(o->className, "Marker"); };
    std::stable_sort(members.begin(), members.end(), [&](const auto& a, const auto& b) {
        const bool ma = isMarker(a.first), mb = isMarker(b.first);
        if (ma != mb) return !ma;
        if (!ma) return false;
        return Fields::s32(a.first, "seqNum", 0) < Fields::s32(b.first, "seqNum", 0);
    });
    PathEntry entry;
    for (int i = 0; i < count; ++i) {
        path->internals["__child" + std::to_string(i)] = members[i].second;
        ScriptObject* marker = members[i].first;
        if (!isMarker(marker)) continue;
        const auto p = Fields::point(marker, "position", {0, 0, 0});
        entry.positions.push_back({p[0], p[1], p[2]});
        entry.msToNext.push_back((uint32_t)Fields::s32(marker, "msToNext", 1000));
    }
    // DMMTODO: Looping paths.
    for (size_t i = 0; i + 1 < entry.msToNext.size(); ++i) entry.totalTime += entry.msToNext[i];
    paths[id] = std::move(entry);
    transmitPath(id);
}

} // namespace

const std::vector<PathEntry>& paths() { return serverPaths(); }

void transmitPaths(GameConnection& connection) { postPathEvent(connection, true, 0); }

} // namespace PathManager

void registerPathNatives(TorqueScript& ts) {
    using Args = std::vector<VMValue>;
    // cPathMissionLoadDone: every SimGroup under MissionGroup, breadth first;
    // each Path among them finishes.
    ts.registerNative("pathOnMissionLoadDone", [](const Args&) -> VMValue {
        auto& engine = ScriptEngine::instance();
        ScriptObject* missionGroup = engine.findObject("MissionGroup");
        if (!missionGroup) return VMValue("");
        std::vector<ScriptObject*> groups{missionGroup};
        for (size_t i = 0; i < groups.size(); ++i) {
            ScriptObject* group = groups[i];
            const int count = group->internals["__childCount"].toInt();
            for (int c = 0; c < count; ++c) {
                ScriptObject* child = engine.findObject(group->internals["__child" + std::to_string(c)].toString().c_str());
                if (child && engine.isSimGroup(child)) groups.push_back(child);
            }
        }
        for (ScriptObject* group : groups)
            if (EngineClasses::isA(group->className, "Path")) PathManager::finishPath(group);
        return VMValue("");
    });
    ts.registerNative("NetConnection::transmitPaths", [](const Args& args) -> VMValue {
        auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        if (!connection) return VMValue("");
        PathManager::transmitPaths(*connection);
        connection->missionPathsSent = true;
        return VMValue("");
    });
    ts.registerNative("NetConnection::clearPaths", [](const Args& args) -> VMValue {
        if (auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString()))
            connection->missionPathsSent = false;
        return VMValue("");
    });
}
