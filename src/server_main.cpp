#include "core/engine.h"
#include "core/console.h"
#include "core/console_args.h"
#include "game/game.h"
#include "net/protocol.h"
#include "net/network.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Torch Dedicated Server\n");
            printf("Usage: torch_server [-p port] [-m mission] [-data dir] [-output dir] [-h]\n");
            printf("  -p <port>          Listen port (default: 28000)\n");
            printf("  -m <mission>       Load a mission or terrain path\n");
            printf("  -data <dir>        Tribes 2 data directory\n");
            printf("  -output <dir>      Runtime output and console.log directory\n");
            printf("  -h, --help         Show this help\n");
            printf("Console: startServer, loadMission, and quit are available in client mode.\n");
            printf("Diagnostics: console.log is written under -output (default: ~/.torch).\n");
            return 0;
        }
    }
    auto& engine = Engine::instance();
    if (!engine.init(argc, argv)) {
        engine.shutdown();
        fprintf(stderr, "Failed to initialize engine\n");
        return 1;
    }

    uint16_t port = T2Protocol::DEFAULT_PORT;
    const char* mission = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (!parseConsolePort(argv[++i], port)) {
                fprintf(stderr, "Invalid server port\n");
                engine.shutdown();
                return 1;
            }
        }
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) mission = argv[++i];
    }

    Console::instance().printf(LogLevel::Info, "Torch Dedicated Server starting on port %d...", port);

    GameServer server;
    World world;
    if (mission) {
        Console::instance().setVariable("sv_mission", mission);
        if (world.loadTerrain(mission)) {
            server.setHeightCallback(+[](float x, float z, void* ctx) -> float {
                return static_cast<World*>(ctx)->getHeight(x, z);
            }, &world);
        } else {
            Console::instance().printf(LogLevel::Warn,
                "Server: no terrain loaded for '%s'; using flat ground (y=2)", mission);
        }
    }
    if (!server.start(port)) {
        fprintf(stderr, "Failed to start server on port %d\n", port);
        engine.shutdown();
        return 1;
    }

    Console::instance().printf(LogLevel::Info, "Server running. Type 'quit' to stop.");

    const int stdinFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdinFlags >= 0) fcntl(STDIN_FILENO, F_SETFL, stdinFlags | O_NONBLOCK);
    bool running = true;
    while (running) {
        server.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Check for console input (non-blocking)
        static char cmdBuf[256];
        static int cmdPos = 0;
        int c = getchar();
        if (c != EOF) {
            if (c == '\n') {
                cmdBuf[cmdPos] = 0;
                if (strcmp(cmdBuf, "quit") == 0 || strcmp(cmdBuf, "exit") == 0) {
                    running = false;
                } else {
                    Console::instance().execute(cmdBuf);
                }
                cmdPos = 0;
            } else if (cmdPos < 255) {
                cmdBuf[cmdPos++] = (char)c;
            }
        }
    }

    server.stop();
    if (stdinFlags >= 0) fcntl(STDIN_FILENO, F_SETFL, stdinFlags);
    engine.shutdown();
    Console::instance().printf(LogLevel::Info, "Server shut down.");
    return 0;
}
