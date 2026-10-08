#pragma once
// TargetInfo name and type text, shared by the server table, the client
// target copy and the HUDs that print it.
#include <string>

namespace TargetNames {

// The typeTag server.cs/ai.cs give a client's target (allocClientTarget
// ... '_ClientConnection'); commanderMap.cs keys its client entries on it.
inline constexpr const char* ClientConnectionType = "_ClientConnection";

inline bool isClientType(const std::string& type) { return type == ClientConnectionType; }

// A name or type starting with '_' is never shown.
inline bool isShown(const std::string& text) { return !text.empty() && text[0] != '_'; }

// TargetManager::getGameName: "name type", case kept, hidden fields left out.
inline std::string gameName(const std::string& name, const std::string& type) {
    const bool hasName = isShown(name);
    const bool hasType = isShown(type);
    if (hasName) return hasType ? name + " " + type : name;
    return hasType ? type : std::string();
}

} // namespace TargetNames
