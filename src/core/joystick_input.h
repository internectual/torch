#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace JoystickInput {

inline constexpr int MaxAxes = 16;
inline constexpr int MaxButtons = 64;

inline bool equalName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

inline int axisIndex(std::string_view name) {
    static constexpr std::string_view names[] = {
        "xaxis", "yaxis", "zaxis", "rxaxis", "ryaxis", "rzaxis",
        "slider0", "slider1", "slider2", "slider3"
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i)
        if (equalName(name, names[i])) return i;
    return -1;
}

inline int buttonIndex(std::string_view name) {
    if (name.size() <= 6 || !equalName(name.substr(0, 6), "button")) return -1;
    int button = -1;
    const char* begin = name.data() + 6;
    const char* end = name.data() + name.size();
    const auto parsed = std::from_chars(begin, end, button);
    if (parsed.ec != std::errc{} || parsed.ptr != end || button < 0 || button >= MaxButtons)
        return -1;
    return button;
}

inline float normalizeAxis(int16_t raw) {
    return raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
}

inline float applyAxisBinding(float value, bool inverted, bool hasDeadZone,
                              float deadZoneBegin, float deadZoneEnd,
                              bool hasScale, float scaleFactor) {
    if (inverted) value = -value;
    if (hasDeadZone && value >= deadZoneBegin && value <= deadZoneEnd)
        value = 0.0f;
    if (hasScale) value *= scaleFactor;
    return value;
}

inline uint8_t povMask(std::string_view name) {
    if (equalName(name, "upov")) return 0x01;
    if (equalName(name, "rpov")) return 0x02;
    if (equalName(name, "dpov")) return 0x04;
    if (equalName(name, "lpov")) return 0x08;
    if (equalName(name, "upleftov")) return 0x01 | 0x08;
    if (equalName(name, "uprightov")) return 0x01 | 0x02;
    if (equalName(name, "downleftov")) return 0x04 | 0x08;
    if (equalName(name, "downrightov")) return 0x04 | 0x02;
    return 0;
}

inline bool povPressed(uint8_t hat, std::string_view name) {
    const uint8_t mask = povMask(name);
    return mask != 0 && (hat & mask) == mask;
}

} // namespace JoystickInput
