#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
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

inline bool parseConsoleHostPort(std::string_view text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
    if (text.find(':') != colon) return false;
    if (!parseConsolePort(text.substr(colon + 1), port)) return false;
    host.assign(text.substr(0, colon));
    return true;
}
