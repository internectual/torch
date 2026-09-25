#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>

// Split the comma-separated argument body used by Torque console commands.
// Quotes protect commas and empty arguments are significant to stock scripts.
inline std::vector<std::string> splitConsoleArguments(std::string_view body) {
    std::vector<std::string> args;
    if (body.empty()) return args;
    std::string current;
    bool quoted = false;
    bool escaped = false;
    int nested = 0;
    for (char c : body) {
        if (escaped) {
            current += c;
            escaped = false;
        } else if (c == '\\' && quoted) {
            escaped = true;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '(') {
            ++nested;
            current += c;
        } else if (!quoted && c == ')' && nested > 0) {
            --nested;
            current += c;
        } else if (c == ',' && !quoted && nested == 0) {
            while (!current.empty() && std::isspace((unsigned char)current.back())) current.pop_back();
            size_t first = 0;
            while (first < current.size() && std::isspace((unsigned char)current[first])) ++first;
            args.push_back(current.substr(first));
            current.clear();
        } else {
            current += c;
        }
    }
    if (escaped) current += '\\';
    while (!current.empty() && std::isspace((unsigned char)current.back())) current.pop_back();
    size_t first = 0;
    while (first < current.size() && std::isspace((unsigned char)current[first])) ++first;
    args.push_back(current.substr(first));
    return args;
}

inline bool parseConsolePort(std::string_view text, uint16_t& port) {
    if (text.empty()) return false;
    std::string value(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno == ERANGE || end != value.c_str() + value.size() || parsed < 1 || parsed > 65535)
        return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

inline bool parseNonNegativeInt(std::string_view text, int& value) {
    if (text.empty()) return false;
    std::string input(text);
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(input.c_str(), &end, 10);
    if (errno == ERANGE || end != input.c_str() + input.size() || parsed < 0 ||
        parsed > INT_MAX)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

inline bool parseConsoleHostPort(std::string_view text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
    if (text.find(':') != colon) return false;
    if (!parseConsolePort(text.substr(colon + 1), port)) return false;
    host.assign(text.substr(0, colon));
    return true;
}

// Demo playback is a client launch mode as well as a console command. Keep
// both historical command-line spellings on the same parsing path.
inline std::string findDemoLaunchPath(int argc, char* const argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "-demo" || option == "--demo" || option == "-playdemo")
            return argv[i + 1];
    }
    return {};
}

// Both explicit demo builds and recording playback bypass the retail login
// transition. Playback still uses the normal client GUI after bootstrap.
inline bool shouldSkipLogin(bool demoMode, bool playback, bool requestedNoLogin) {
    return requestedNoLogin || demoMode || playback;
}

// The stock login dialog is also used for the offline client.  Offline login
// completes locally; an explicitly online launch must remain in the account
// flow instead of silently starting a local mission.
inline bool loginCanCompleteOffline(bool online) {
    return !online;
}

// An explicit master URL wins; the demo-only setting must never affect the
// normal retail path.
inline std::string selectMasterServerUrl(bool demoMode, std::string_view requested,
                                         std::string_view demoMasterServer,
                                         std::string_view retailMasterServer) {
    if (!requested.empty()) return std::string(requested);
    return std::string(demoMode ? demoMasterServer : retailMasterServer);
}

// Demo builds may be distributed with different network permissions. Keep
// this independent from recording playback and make it deterministic in tests.
inline bool allowDemoConnection(bool demoMode, bool observer, bool allowConnect,
                                bool allowWatch) {
    if (!demoMode) return true;
    return observer ? allowWatch : allowConnect;
}
