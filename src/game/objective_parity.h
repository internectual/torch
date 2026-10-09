#pragma once

#include "game/mission_parser.h"
#include "render/renderer.h"
#include <cctype>
#include <string>
#include <utility>

enum class ObjectiveState { Hidden, Active, Complete };

struct ObjectivePresentation {
    std::string label;
    ColorF color{1, 1, 1, 1};
    bool visible = false;
};

inline bool objectiveVisibleForTeam(int objectiveTeam, int viewerTeam, bool mapperMode) {
    // Observers have no team, but Tribes 2 still shows them both teams'
    // objective markers so they can follow the match.
    return mapperMode || viewerTeam == 0 || objectiveTeam == 0 || objectiveTeam == viewerTeam;
}

inline ColorF objectiveMarkerColor(int objectiveTeam, int viewerTeam, bool mapperMode) {
    if (mapperMode || viewerTeam == 0) {
        // Stock Tribes 2 assigns Storm (team 1) blue and Inferno (team 2)
        // red, matching the scoreboard and team HUD colors. Observers have no
        // allegiance, so they need the authored team colors rather than the
        // enemy-marker color used for a playable viewer.
        if (objectiveTeam == 1) return {0.3f, 0.45f, 1.0f, 1.0f};
        if (objectiveTeam == 2) return {1.0f, 0.25f, 0.25f, 1.0f};
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
