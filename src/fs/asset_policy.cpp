#include "fs/asset_policy.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace TorchAssets {

bool isOriginalRuntimePath(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return true; // Extensionless paths are valid Torque resource names.

    std::string extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    // This is intentionally an allowlist. In particular, GLB and other
    // converted formats must never become an implicit runtime dependency.
    static constexpr std::string_view allowed[] = {
        ".dts", ".dsq", ".dif", ".ter", ".dml", ".ifl", ".bm8",
        ".gft", ".wav", ".ogg", ".mis", ".cs", ".gui", ".dso",
        ".vl2", ".vol", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"
    };
    for (const auto candidate : allowed)
        if (extension == candidate) return true;
    return false;
}

} // namespace TorchAssets
