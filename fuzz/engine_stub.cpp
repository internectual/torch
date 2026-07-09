// Link-only stub for Engine. The raw loader entry points we fuzz
// (loadDIF/loadDTS/loadGLB bytes variants) never call Engine::instance() -- only
// the *FromFile helpers do, which we don't invoke. We just need the symbol to
// resolve so the loaders link. Returning zeroed storage is safe because it is
// never actually used.

#include "core/engine.h"

__attribute__((no_sanitize("address", "undefined")))
Engine& Engine::instance() {
    alignas(Engine) static char buf[sizeof(Engine)];
    return *reinterpret_cast<Engine*>(buf);
}
