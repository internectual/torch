#pragma once

#include <algorithm>
#include <string_view>
#include "core/math.h"

namespace HudParity {
inline bool crosshairVisible(bool dead, bool liveObserver, bool targetFinderOpen) {
    return !dead && !liveObserver && !targetFinderOpen;
}

inline const char* teamName(int teamId) {
    // Tribes 2's stock team labels are Storm and Inferno, not generic color
    // names. Team zero is the observer/unassigned team, whose scoreboard label
    // is Observer rather than the placeholder used for missing telemetry.
    return teamId == 1 ? "Storm" : teamId == 2 ? "Inferno" :
           teamId == 0 ? "Observer" : "N/A";
}

inline ColorF teamColor(int teamId) {
    // Team 1 is Storm (blue) and team 2 is Inferno (red) in the stock HUD.
    // The numeric IDs are not a red/blue ordering, so do not infer the color
    // from the team number.
    if (teamId == 1) return {0.3f, 0.4f, 1.0f, 0.9f};
    if (teamId == 2) return {1.0f, 0.3f, 0.3f, 0.9f};
    return {0.5f, 0.5f, 0.5f, 0.8f};
}

inline float messageAlpha(double age, double duration) {
    // Chat timestamps can briefly be uninitialized while a connection/demo is
    // being established.  Letting NaN reach the clamp produces a NaN vertex
    // alpha, which makes the notification disappear instead of matching the
    // stock HUD's fully visible initial state.
    if (!std::isfinite(duration) || duration <= 0.0) return 1.0f;
    if (!std::isfinite(age)) return age > 0.0 ? 0.0f : 1.0f;
    return std::clamp((float)(1.0 - age / duration), 0.0f, 1.0f);
}

inline float resourceFraction(float value, float maximum = 100.0f) {
    if (!std::isfinite(value) || !std::isfinite(maximum) || maximum <= 0.0f)
        return 0.0f;
    return std::clamp(value / maximum, 0.0f, 1.0f);
}

// HUD bars reserve one pixel on each side for their outline. Do not emit an
// inverted fill box when a valid resource is below that two-pixel inset.
inline bool resourceFillVisible(float value, float maximum, float barWidth) {
    return resourceFraction(value, maximum) * std::max(0.0f, barWidth) > 2.0f;
}

inline float scoreboardHeaderY(float top, int teamRows) {
    return top + 86.0f + std::max(0, teamRows) * 16.0f;
}

inline int scoreboardTeamRows(bool connected, bool observerMode, int teamRows) {
    return connected && observerMode ? std::max(0, teamRows) : 0;
}

// The stock scoreboard groups players by team, then ranks each team by score.
// Keep the final tie-breakers stable because demo ghost discovery order is not.
inline bool scoreboardPlayerBefore(int teamA, int scoreA, int clientA,
                                   std::string_view nameA, int teamB, int scoreB,
                                   int clientB, std::string_view nameB) {
    const int normalizedTeamA = teamA < 0 ? 0 : teamA;
    const int normalizedTeamB = teamB < 0 ? 0 : teamB;
    if (normalizedTeamA != normalizedTeamB)
        return normalizedTeamA < normalizedTeamB;
    if (scoreA != scoreB) return scoreA > scoreB;
    if (clientA >= 0 && clientB >= 0 && clientA != clientB)
        return clientA < clientB;
    if ((clientA >= 0) != (clientB >= 0)) return clientA >= 0;
    return nameA < nameB;
}

// Keep the selected target visible when the observer list is taller than the
// stock target-finder panel.
inline int targetFinderWindowStart(int selection, int count, int visibleRows) {
    if (count <= 0 || visibleRows <= 0) return 0;
    const int maxStart = std::max(0, count - visibleRows);
    return std::clamp(selection - visibleRows + 1, 0, maxStart);
}

// Sensor group zero is the unassigned group; authored Tribes 2 groups are
// numbered from one through the configured count.
inline bool sensorGroupInRange(int group, int count) {
    return group > 0 && group <= count && group < 32;
}
} // namespace HudParity
