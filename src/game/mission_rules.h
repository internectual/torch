#pragma once

#include <cctype>
#include <algorithm>
#include <string>

enum class MissionGameType { Deathmatch, TeamDeathmatch, CaptureTheFlag, Training };

struct MissionRules {
    MissionGameType type = MissionGameType::Deathmatch;
    int scoreLimit = 25;
    bool teamBased = false;
    bool respawn = true;
    bool objectives = false;
    bool matchClock = true;
    bool scoreHud = true;
    bool debrief = true;
};

inline std::string missionRulesLower(std::string value) {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    return value;
}

inline std::string missionRulesCompact(const std::string& value) {
    std::string compact;
    compact.reserve(value.size());
    for (unsigned char c : value)
        if (std::isalnum(c)) compact.push_back((char)c);
    return compact;
}

// Torque resolves mission class names case-insensitively. Keep runtime checks
// from making authored objects depend on the spelling used by the .mis file.
inline bool missionClassIs(const std::string& className, const char* expected) {
    return expected && missionRulesLower(className) == missionRulesLower(expected);
}

// SimObject names are resolved case-insensitively by the Torque console.
// Keep script calls to named mission objects consistent with class lookup.
inline bool missionObjectNameIs(const std::string& authoredName,
                                const std::string& requestedName) {
    return authoredName.size() == requestedName.size() &&
           missionRulesLower(authoredName) == missionRulesLower(requestedName);
}

// InteriorInstance is a renderable SceneObject and supports the same hidden
// state as StaticShape. Mission scripts use this for doors and dynamic map
// geometry, so it must not be rejected by the setHidden bridge.
inline bool missionObjectCanBeHidden(const std::string& className) {
    return missionClassIs(className, "InteriorInstance") ||
           missionClassIs(className, "StaticShape") ||
           missionClassIs(className, "TSStatic") ||
           missionClassIs(className, "Turret") ||
           missionClassIs(className, "Item") ||
           missionClassIs(className, "ForceFieldBare") ||
           missionClassIs(className, "Vehicle") ||
           missionRulesLower(className).ends_with("vehicle");
}

inline bool isStockTrainingMission(const std::string& missionName) {
    std::string name = missionRulesLower(missionName);
    std::replace(name.begin(), name.end(), '\\', '/');
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return name.size() == 9 && name.starts_with("training") &&
           name.back() >= '1' && name.back() <= '5';
}

inline MissionRules stockMissionRules(const std::string& missionName,
                                       const std::string& missionContent = {}) {
    std::string name = missionRulesLower(missionName);
    // Mission filenames can come from native Windows script paths even when
    // the rest of the runtime uses slash-separated paths.
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::string content = missionRulesLower(missionContent);
    const std::string compactContent = missionRulesCompact(content);
    MissionRules rules;
    const bool training = isStockTrainingMission(name);
    // Stock CTF missions do not all embed a game-type marker in the .mis
    // file. Keep the filename fallback aligned with the shipped map set so
    // their team/objective HUD is selected before the mission scripts run.
    const bool filenameCtf = name.find("minotaur") != std::string::npos ||
                      name.find("damnation") != std::string::npos ||
                      name.find("raindance") != std::string::npos ||
                      name.find("katabatic") != std::string::npos ||
                      name.find("scarabrae") != std::string::npos ||
                      name.find("broadside") != std::string::npos ||
                      name.find("icedagger") != std::string::npos;
    const bool explicitCtf = compactContent.find("ctfgame") != std::string::npos ||
                             compactContent.find("capturetheflag") != std::string::npos ||
                             compactContent.find("missiontypesctf") != std::string::npos;
    const bool team = compactContent.find("teamdeathmatch") != std::string::npos ||
                      compactContent.find("teamdm") != std::string::npos ||
                      compactContent.find("missiontypesteam") != std::string::npos;
    const bool ctf = explicitCtf || (filenameCtf && !team);
    const bool authoredObjectives = content.find("aiobjective") != std::string::npos;
    if (training) {
        rules.type = MissionGameType::Training;
        rules.scoreLimit = 0;
        rules.respawn = false;
        // Training1 creates its objectives from Training1.cs; Training2-5
        // author additional AIObjective objects in the mission tree.
        rules.objectives = true;
        rules.matchClock = false;
        rules.scoreHud = false;
        rules.debrief = false;
    } else if (ctf) {
        rules.type = MissionGameType::CaptureTheFlag;
        rules.scoreLimit = 5;
        rules.teamBased = true;
        rules.objectives = true;
    } else if (team) {
        rules.type = MissionGameType::TeamDeathmatch;
        rules.teamBased = true;
    }
    if (!training && authoredObjectives) rules.objectives = true;
    return rules;
}

inline bool missionRulesTeamSpawn(int spawnTeam, int playerTeam) {
    return spawnTeam == playerTeam || spawnTeam == 0;
}
