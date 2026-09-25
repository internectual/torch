#include "render/text_layout.h"
#include "game/hud_parity.h"
#include <cassert>
#include <limits>
#include <string>

int main() {
    const auto lines = TextLayout::wrapLines(
        "one two three\nfour", 56.0f,
        [](std::string_view s) { return (float)s.size() * 8.0f; });
    assert((lines == std::vector<std::string>{"one two", "three", "four"}));
    const auto whitespaceLines = TextLayout::wrapLines(
        "one\ttwo\r\nthree", 56.0f,
        [](std::string_view s) { return (float)s.size() * 8.0f; });
    assert((whitespaceLines == std::vector<std::string>{"one two", "three"}));
    const auto blankLines = TextLayout::wrapLines(
        "one\n \t\nthree", 56.0f,
        [](std::string_view s) { return (float)s.size() * 8.0f; });
    assert((blankLines == std::vector<std::string>{"one", "", "three"}));
    const auto trailingBlankLine = TextLayout::wrapLines(
        "one\n", 56.0f,
        [](std::string_view s) { return (float)s.size() * 8.0f; });
    assert((trailingBlankLine == std::vector<std::string>{"one", ""}));
    const auto invalidWidth = TextLayout::wrapLines(
        "alpha", std::numeric_limits<float>::quiet_NaN(),
        [](std::string_view s) { return (float)s.size(); });
    assert(invalidWidth.empty());
    assert(HudParity::messageAlpha(0.0, 3.0) == 1.0f);
    assert(HudParity::messageAlpha(3.0, 3.0) == 0.0f);
    assert(HudParity::messageAlpha(std::numeric_limits<double>::quiet_NaN(), 3.0) == 1.0f);
    assert(HudParity::messageAlpha(1.0, std::numeric_limits<double>::quiet_NaN()) == 1.0f);
    return 0;
}
