// VMValue conversions for tests that do not link the script engine
// (same formatting as src/script/script_engine.cpp).
#include "script/script_engine.h"
#include <cstdio>
#include <cstdlib>

std::string VMValue::toString() const {
    switch (type) {
        case Int: return std::to_string(i);
        case Float: { char buf[64]; snprintf(buf, sizeof(buf), "%g", f); return buf; }
        case String: return str;
        default: return "";
    }
}
