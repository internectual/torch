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
    const std::string name = missionRulesLower(missionName);
    const std::string content = missionRulesLower(missionContent);
    MissionRules rules;
    const bool training = isStockTrainingMission(name);
    const bool ctf = name.find("minotaur") != std::string::npos ||
                     name.find("damnation") != std::string::npos ||
                     content.find("ctfgame") != std::string::npos ||
                     content.find("capturetheflag") != std::string::npos ||
                     content.find("missiontypes = ctf") != std::string::npos;
    const bool team = content.find("teamdeathmatch") != std::string::npos ||
                      content.find("teamdm") != std::string::npos ||
                      content.find("missiontypes = team") != std::string::npos;
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
