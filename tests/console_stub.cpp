#include "core/console.h"

#include <cstdarg>

Console::Console() : impl(nullptr) {}
Console::~Console() = default;

Console& Console::instance() {
    static Console console;
    return console;
}

void Console::printf(LogLevel, const char*, ...) {}
