#include "game/sky_parity.h"
#include <cassert>

int main() {
    const auto entries = parseSkyMaterialList(
        "north\n"
        "south // trailing note\n"
        "\n"
        "; skipped entry\n"
        "east\n"
        "west # trailing note\n"
        "top\n"
        "bottom\n"
        "cloud_a\n"
        "cloud_b\n");

    assert(entries.faces.size() == 6);
    assert(entries.faces[1] == "south");
    assert(entries.faces[2] == "east");
    assert(entries.faces[5] == "bottom");
    assert(entries.environment == "cloud_a");
    assert(entries.clouds.size() == 1);
    assert(entries.clouds[0] == "cloud_b");
    return 0;
}
