#pragma once
#include <cstdint>
#include <string>

namespace TorchMaster {
bool parseAddressLine(const std::string& line, std::string& host, uint16_t& port);
}
