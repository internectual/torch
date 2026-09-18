#pragma once

#include <algorithm>
#include <cctype>
#include <string>

enum class ItemKind { None, Health, Energy, Ammo };

inline ItemKind classifyItemKind(std::string name) {
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.find("health") != std::string::npos || name.find("repair") != std::string::npos)
        return ItemKind::Health;
    if (name.find("energy") != std::string::npos)
        return ItemKind::Energy;
    if (name.find("ammo") != std::string::npos)
        return ItemKind::Ammo;
    return ItemKind::None;
}

inline float applyItemAmount(ItemKind kind, float value, float amount, float maximum) {
    if (kind == ItemKind::Ammo) return value + std::max(0.0f, amount);
    return std::clamp(value + std::max(0.0f, amount), 0.0f, maximum);
}
