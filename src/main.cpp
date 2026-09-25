#include "core/engine.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    auto& engine = Engine::instance();

    if (!engine.init(argc, argv)) {
        engine.shutdown();
        fprintf(stderr, "Failed to initialize engine\n");
        return 1;
    }

    engine.run();
    engine.shutdown();

    return 0;
}
