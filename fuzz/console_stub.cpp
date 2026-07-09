// Headless stub for Console. The loaders only ever call printf(); we make it a
// no-op so we don't have to link the real Console (which drags in ScriptEngine
// and the rest of the engine). Console has no virtual methods and uses a Pimpl,
// so returning a reference to zeroed storage is safe: printf never touches it.

#include "core/console.h"
#include <cstdarg>

__attribute__((no_sanitize("address", "undefined")))
Console& Console::instance() {
    alignas(Console) static char buf[sizeof(Console)];
    return *reinterpret_cast<Console*>(buf);
}

void Console::printf(LogLevel, const char*, ...) {}
