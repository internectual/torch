#include "fs/asset_policy.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace TorchAssets {

bool isOriginalRuntimePath(std::string_view path) {
    // These directories are produced by Torch or conversion tools, not by a
    // stock Tribes 2 installation. Keep the check case-insensitive because
    // the original game is commonly run from a case-insensitive filesystem.
    size_t componentStart = 0;
    while (componentStart <= path.size()) {
        const size_t slash = path.find('/', componentStart);
        const auto component = path.substr(componentStart,
            slash == std::string_view::npos ? path.size() - componentStart : slash - componentStart);
        std::string folded(component);
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (folded == "generated" || folded == "converted" || folded == "cache" ||
            folded == "glb" || folded == "output") return false;
        if (slash == std::string_view::npos) break;
        componentStart = slash + 1;
    }
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
        ".gft", ".wav", ".ogg", ".mis", ".mispk", ".cs", ".gui", ".dso",
         ".vl2", ".vol", ".rec", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".dds"
    };
    for (const auto candidate : allowed)
        if (extension == candidate) return true;
    return false;
}

} // namespace TorchAssets
