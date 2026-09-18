#include "game/mission_discovery.h"

#include <cassert>

int main() {
    const auto metadata = parseMissionMetadata(
        "MISSIONS/Official/Desert.MISPK",
        "  // DisplayName = The Desert\r\n"
        "// MissionTypes = CTF Siege\n"
        "new TerrainBlock() { terrainFile = \"terrains/Desert.ter\"; };\n");
    assert(metadata.file == "Official/Desert");
    assert(metadata.displayName == "The Desert");
    assert(metadata.types == "CTF Siege");
    assert(!missionIsSinglePlayer(metadata));
    assert(missionMapName("base/missions/Official/Desert.MISPK") == "Official/Desert");

    const auto single = parseMissionMetadata("missions/training.MIS",
                                             "// MissionTypes = CTF SinglePlayer\n");
    assert(missionIsSinglePlayer(single));
    const auto quoted = parseMissionMetadata("missions/quoted.mis",
        "// MissionTypes = \"CTF SinglePlayer\"\n");
    assert(missionIsSinglePlayer(quoted));
    assert(missionLoadPath("missions/Official/Desert.MIS") == "Official/Desert");
    assert(missionLoadPath("missions/../outside.mis").empty());
    assert(missionLoadPath("missions\\outside.mis").empty());
    assert(missionPreviewCandidates("missions/../outside").empty());

    const auto previews = missionPreviewCandidates("official/Desert");
    assert(previews.size() == 3);
    assert(previews[0] == "missions/official/Desert.jpg");
    assert(previews[1] == "missions/official/Desert.png");
    return 0;
}
