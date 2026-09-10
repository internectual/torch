#pragma once

namespace TorchPath {
bool isSafeLogicalPath(const char* path);
bool staysWithinRoot(const char* root, const char* candidate);
}
