#include "net/master_query.h"
#include <cstdlib>

namespace TorchMaster {

bool parseAddressLine(const std::string& line, std::string& host, uint16_t& port) {
    std::string value = line;
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    value.erase(0, first);
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) return false;
    host = value.substr(0, colon);
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str() + colon + 1, &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 65535) return false;
    for (unsigned char c : host)
        if (c < 0x20 || c == '/' || c == '\\') return false;
    port = (uint16_t)parsed;
    return !host.empty();
}

}
