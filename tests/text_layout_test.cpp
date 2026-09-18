#include "render/text_layout.h"
#include "game/hud_parity.h"
#include <cassert>
#include <string>

int main() {
    const auto lines = TextLayout::wrapLines(
        "one two three\nfour", 56.0f,
        [](std::string_view s) { return (float)s.size() * 8.0f; });
    assert((lines == std::vector<std::string>{"one two", "three", "four"}));
    assert(HudParity::messageAlpha(0.0, 3.0) == 1.0f);
    assert(HudParity::messageAlpha(3.0, 3.0) == 0.0f);
    return 0;
}
