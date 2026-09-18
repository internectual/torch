#pragma once

#include <string>

namespace TorchPath {
bool isSafeLogicalPath(const char* path);
bool staysWithinRoot(const char* root, const char* candidate);
bool safeOutputPath(const char* root, const char* logicalName, std::string& result);
}
