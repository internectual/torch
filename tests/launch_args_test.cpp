#include "core/console_args.h"

#include <cassert>

int main() {
    uint16_t port = 0;
    assert(parseConsolePort("28000", port) && port == 28000);
    assert(!parseConsolePort("28000junk", port));
    assert(!parseConsolePort("0", port));
    assert(!parseConsolePort("65536", port));

    std::string host;
    assert(parseConsoleHostPort("example.test:28000", host, port));
    assert(host == "example.test" && port == 28000);
    assert(!parseConsoleHostPort("example.test", host, port));
    assert(!parseConsoleHostPort("a:b:28000", host, port));
    return 0;
}
