#include "fs/path_policy.h"
#include <filesystem>
#include <string>

namespace TorchPath {

bool staysWithinRoot(const char* root, const char* candidate) {
    if (!root || !candidate) return false;
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    error.clear();
    const std::filesystem::path canonicalCandidate =
        std::filesystem::weakly_canonical(candidate, error);
    if (error) return false;
    const std::filesystem::path relative =
        canonicalCandidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative == ".") return false;
    const auto first = relative.begin();
    return first != relative.end() && first->string() != "..";
}

bool isSafeLogicalPath(const char* path) {
    if (!path || !path[0]) return false;
    const std::string value(path);
    if (value.find('\0') != std::string::npos || value.find('\\') != std::string::npos)
        return false;
    const std::filesystem::path logical(value);
    if (logical.is_absolute() || logical.has_root_name() || logical.has_root_directory())
        return false;
    for (const auto& component : logical) {
        if (component == ".." || component == ".") return false;
    }
    return true;
}

}
