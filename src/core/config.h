#pragma once

// Version defines (derived from CMake project version)
#ifndef TORCH_VERSION_MAJOR
#define TORCH_VERSION_MAJOR 0
#endif
#ifndef TORCH_VERSION_MINOR
#define TORCH_VERSION_MINOR 1
#endif
#ifndef TORCH_VERSION_PATCH
#define TORCH_VERSION_PATCH 0
#endif

#define TORCH_VERSION_STRING "0.1.0"

// Temp directory resolution: checks TORCH_TMPDIR, TMPDIR, XDG_RUNTIME_DIR, falls back to /tmp
#include <cstdlib>
#include <cstring>
#include <string>

inline std::string torchTempDir() {
    if (const char* v = std::getenv("TORCH_TMPDIR")) return v;
    if (const char* v = std::getenv("TMPDIR")) return v;
    if (const char* v = std::getenv("XDG_RUNTIME_DIR")) return v;
    return "/tmp";
}

// Data directory resolution: checks TORCH_T2DATA, falls back to "base"
inline std::string torchDataDir() {
    if (const char* v = std::getenv("TORCH_T2DATA")) return v;
    return "base";
}
