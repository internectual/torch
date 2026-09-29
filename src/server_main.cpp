// Torch dedicated server: the engine in the retail DedicatedServer launch
// mode (console_start.cs `-dedicated`), headless. Every other argument is
// the retail command line (-mission <name> <type>, -serverprefs <file>,
// -mod <dir>, ...); the port is $Host::Port from the server prefs.
#include "core/engine.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Torch Dedicated Server\n");
            printf("Usage: torch_server [-mission <name> <type>] [-serverprefs <file>] [-mod <dir>] [-data <dir>] [-output <dir>] [-h]\n");
            printf("  -mission <name> <type>  Mission and mission type to host (e.g. TWL_Minotaur CTF)\n");
            printf("  -serverprefs <file>     Server prefs script ($Host::Port, $Host::GameName, ...)\n");
            printf("  -mod <dir>              Add a mod directory\n");
            printf("  -data <dir>             Tribes 2 data directory\n");
            printf("  -output <dir>           Runtime output and console.log directory\n");
            printf("  -h, --help              Show this help\n");
            printf("Console: TorqueScript on stdin (quit() stops the server).\n");
            printf("Diagnostics: console.log is written under -output (default: ~/.torch).\n");
            return 0;
        }
    }
    std::vector<char*> args(argv, argv + argc);
    static char dedicated[] = "-dedicated";
    args.insert(args.begin() + 1, dedicated);
    auto& engine = Engine::instance();
    if (!engine.init((int)args.size(), args.data())) {
        engine.shutdown();
        fprintf(stderr, "Failed to initialize engine\n");
        return 1;
    }
    engine.run();
    engine.shutdown();
    return 0;
}
