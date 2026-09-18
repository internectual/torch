#pragma once

#include "game/mission_parser.h"
#include "render/renderer.h"
#include <string>
#include <utility>

enum class ObjectiveState { Hidden, Active, Complete };

struct ObjectivePresentation {
    std::string label;
    ColorF color{1, 1, 1, 1};
    bool visible = false;
};

inline bool objectiveVisibleForTeam(int objectiveTeam, int viewerTeam, bool mapperMode) {
    return mapperMode || objectiveTeam == 0 || objectiveTeam == viewerTeam;
}

inline ColorF objectiveMarkerColor(int objectiveTeam, int viewerTeam, bool mapperMode) {
    if (mapperMode) {
        if (objectiveTeam == 1) return {1.0f, 0.25f, 0.25f, 1.0f};
        if (objectiveTeam == 2) return {0.3f, 0.45f, 1.0f, 1.0f};
        return {1.0f, 0.85f, 0.25f, 1.0f};
    }
    if (objectiveTeam != 0 && objectiveTeam == viewerTeam)
        return {0.2f, 1.0f, 0.3f, 0.95f};
    return {1.0f, 0.2f, 0.2f, 0.95f};
}

inline ObjectivePresentation presentObjective(const AuthoredMissionObjective& objective,
                                              int viewerTeam, bool mapperMode,
                                              ObjectiveState state = ObjectiveState::Active) {
    ObjectivePresentation result;
    result.label = objective.marker.label.empty() ? objective.description : objective.marker.label;
    result.visible = state != ObjectiveState::Hidden &&
                     objectiveVisibleForTeam(objective.marker.teamId, viewerTeam, mapperMode);
    result.color = objectiveMarkerColor(objective.marker.teamId, viewerTeam, mapperMode);
    if (state == ObjectiveState::Complete) result.color.a = 0.45f;
    return result;
}

inline std::string objectiveTaskText(const std::string& line1, const std::string& line2 = {}) {
    if (line1.empty()) return line2;
    if (line2.empty()) return line1;
    return line1 + "\n" + line2;
}

inline std::pair<std::string, std::string> stockTrainingInitialObjective(const std::string& missionName) {
    const size_t marker = missionName.find("Training");
    if (marker == std::string::npos || marker + 8 >= missionName.size()) return {};
    switch (missionName[marker + 8]) {
        case '1': return {"Survey Hanakush Lowlands.", {}};
        case '2': return {"Capture tower at waypoint.", "Eliminate enemy units."};
        case '3': return {"Board the Shrike.", {}};
        case '4': return {"Stay alert for enemy presence.", "Repair sensor at waypoint."};
        case '5': return {"Capture tower at waypoint.", "Implant digital virus."};
        default: return {};
    }
}
