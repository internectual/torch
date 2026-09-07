#pragma once

#include <string_view>

namespace TorchAssets {

// Runtime assets must be files that the stock Tribes 2 installation can
// provide. Converted interchange formats are deliberately not accepted.
bool isOriginalRuntimePath(std::string_view path);

} // namespace TorchAssets
