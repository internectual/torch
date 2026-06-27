#include "core/engine.h"
#include "fs/vol_archive.h"
#include "fs/vl2_archive.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <glob.h>

struct Engine::Impl {};

Engine::Engine() : impl(new Impl) {}
Engine::~Engine() { delete impl; }

Engine& Engine::instance() {
    static Engine eng;
    return eng;
}

bool Engine::init(int argc, char* argv[]) {
    Console::instance().printf(LogLevel::Info, "Torch v0.1.0");

    // Parse args
    std::string dataDir = "/home/methodown/t2-linux";
    bool noLogin = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-data") == 0 && i + 1 < argc) dataDir = argv[i + 1];
        if (strcmp(argv[i], "-online") == 0) Console::instance().setVariable("online", "1");
        if (strcmp(argv[i], "-nologin") == 0) noLogin = true;
        if (strcmp(argv[i], "-preview") == 0 && i + 1 < argc) previewMap = argv[i + 1];
        if (strcmp(argv[i], "-campos") == 0 && i + 3 < argc) {
            previewCamPos.x = (float)atof(argv[i + 1]);
            previewCamPos.y = (float)atof(argv[i + 2]);
            previewCamPos.z = (float)atof(argv[i + 3]);
            usePreviewCam = true;
        }
        if (strcmp(argv[i], "-camtarget") == 0 && i + 3 < argc) {
            previewCamTarget.x = (float)atof(argv[i + 1]);
            previewCamTarget.y = (float)atof(argv[i + 2]);
            previewCamTarget.z = (float)atof(argv[i + 3]);
            usePreviewCam = true;
        }
    }

    // Init subsystems
    tim = new Timer;
    con = &Console::instance();
    plat = new Platform;
    filesys = new FileSystem;
    ren = new Renderer;
    aud = new AudioSystem;
    scr = new ScriptEngine;
    net = new NetworkManager;
    g = new Game;

    // Platform
    PlatformConfig pconfig;
    pconfig.title = "TORCH";
    pconfig.width = Console::instance().getIntVariable("videoWidth", 1280);
    pconfig.height = Console::instance().getIntVariable("videoHeight", 720);
    if (!plat->init(pconfig)) return false;

    // File System - add T2 data paths
    std::vector<std::string> paths = {
        dataDir, dataDir + "/base", "./base", "./data",
        "/home/methodown/t2-mapper/docs/base",  // extracted assets
        "/home/methodown/torch/glb"           // decompressed GLB files
    };
    filesys->init(paths);

    // Mount all game archives
    auto& fs = *filesys;
    std::vector<std::string> archives;

    // Scan base directory for .vl2 and .vol files
    auto scanArchives = [&](const std::string& dir) {
        std::vector<std::string> found;
        glob_t globbuf;
        std::string pattern = dir + "/*.vl2";
        if (glob(pattern.c_str(), 0, nullptr, &globbuf) == 0) {
            for (size_t i = 0; i < globbuf.gl_pathc; i++)
                found.push_back(globbuf.gl_pathv[i]);
            globfree(&globbuf);
        }
        pattern = dir + "/*.vol";
        if (glob(pattern.c_str(), 0, nullptr, &globbuf) == 0) {
            for (size_t i = 0; i < globbuf.gl_pathc; i++)
                found.push_back(globbuf.gl_pathv[i]);
            globfree(&globbuf);
        }
        return found;
    };

    // Scan data dir and its subdirs for archives
    archives = scanArchives(dataDir);
    auto baseArchives = scanArchives(dataDir + "/base");
    archives.insert(archives.end(), baseArchives.begin(), baseArchives.end());

    Console::instance().printf(LogLevel::Info, "Found %zu archives", archives.size());
    for (auto& a : archives) {
        if (a.size() > 3 && a.substr(a.size()-3) == "vl2") {
            auto* vl2 = new Vl2Archive;
            if (vl2->open(a.c_str())) {
                fs.addArchive(vl2);
                Console::instance().printf(LogLevel::Debug, "Mounted VL2: %s", a.c_str());
            } else {
                delete vl2;
            }
        } else {
            auto* vol = new VolArchive;
            if (vol->open(a.c_str())) {
                fs.addArchive(vol);
                Console::instance().printf(LogLevel::Debug, "Mounted VOL: %s", a.c_str());
            } else {
                delete vol;
            }
        }
    }

    // Renderer
    if (!ren->init(plat->nativeWindow())) return false;
    ren->config().width = plat->width();
    ren->config().height = plat->height();
    plat->setResizeCallback([this](int w, int h) { ren->onResize(w, h); });

    // Audio
    aud->init();

    // Script engine
    scr->init();

    // Network
    net->init();

    // Game
    g->init();

    // Register console commands
    con->addCommand("quit", [this](int32_t argc, const char* const* argv) {
        quit();
    });

    con->addCommand("echo", [](int32_t argc, const char* const* argv) {
        std::string msg;
        for (int i = 1; i < argc; i++) {
            if (i > 1) msg += " ";
            msg += argv[i];
        }
        Console::instance().printf(LogLevel::Info, "%s", msg.c_str());
    });

    con->addCommand("exec", [](int32_t argc, const char* const* argv) {
        if (argc > 1) Console::instance().executeFile(argv[1]);
    });

    con->addCommand("screenshot", [this](int32_t argc, const char* const* argv) {
        const char* path = argc > 1 ? argv[1] : "screenshot.png";
        if (ren->screenshot(path))
            Console::instance().printf(LogLevel::Info, "Screenshot saved: %s", path);
        else
            Console::instance().printf(LogLevel::Error, "Screenshot failed: %s", path);
    }, "screenshot [path] - save a screenshot to path (default: screenshot.png)");

    Console::instance().printf(LogLevel::Info, "Engine initialized successfully");

    // -preview mode: screenshot after map loads (implies -nologin)
    if (!previewMap.empty()) {
        g->startLocalGame(previewMap.c_str());
        // Auto-compute preview camera over the terrain center if not explicitly set
        if (!usePreviewCam && g->state() == Game::Playing) {
            auto& tb = *g->world().terrain();
            float half = tb.size * tb.squareSize * 0.5f;
            float cx = tb.worldOffset.x + half;
            float cz = tb.worldOffset.z + half;
            float h = g->world().getHeight(cx, cz);
            if (h < 0) h = 0;
            previewCamTarget = {cx, h, cz};
            previewCamPos = {cx, h + half * 0.5f, cz - half * 0.8f};
            usePreviewCam = true;
        }
    } else if (noLogin) {
        Console::instance().printf(LogLevel::Info, "-nologin: starting local game");
        g->startLocalGame();
    }

    return true;
}

void Engine::run() {
    running = true;

    double lastTime = Timer::now();
    double fpsTimer = 0;
    int frameCount = 0;

    while (running && plat->isRunning()) {
        double now = Timer::now();
        float dt = (float)(now - lastTime);
        lastTime = now;

        // Cap dt
        if (dt > 0.1f) dt = 0.1f;

        // Process events
        if (!plat->processEvents()) break;

        // Update subsystems
        net->update();
        scr->vm()->setVariable("time", (float)now);

        // Game update
        if (g->state() != Game::Menu) {
            // Read input
            Game::InputMove input;
            auto& keys = plat->input().keysDown;
            input.forward = keys[26] != 0;   // W
            input.backward = keys[22] != 0;  // S
            input.left = keys[4] != 0;       // A
            input.right = keys[7] != 0;       // D
            input.jump = keys[44] != 0;       // Space
            input.jet = keys[44] != 0;        // Space (same as jump)
            input.freeCam = keys[58] != 0;     // F1
            input.lookDelta = {
                (float)plat->input().mouseDeltaY * 0.002f,
                (float)plat->input().mouseDeltaX * 0.002f,
                0
            };

            // Toggle relative mouse for FPS
            static bool wasInMenu = true;
            if (g->state() == Game::Playing && wasInMenu) {
                plat->setRelativeMouse(true);
                plat->showMouse(false);
                wasInMenu = false;
            }
            if (g->state() != Game::Playing) wasInMenu = true;

            g->applyInput(input);
            g->update(dt);
            g->render(dt);

            // Preview mode: take screenshot after first render
            if (!previewMap.empty() && !previewDone) {
                char path[256];
                snprintf(path, sizeof(path), "preview_%s.png", previewMap.c_str());
                if (ren->screenshot(path))
                    Console::instance().printf(LogLevel::Info, "Preview saved: %s", path);
                else
                    Console::instance().printf(LogLevel::Error, "Preview screenshot failed");
                previewDone = true;
                quit();
            }
        } else {
            // Menu rendering
            plat->setRelativeMouse(false);
            plat->showMouse(true);
            auto& r = *ren;
            r.beginFrame({0.1f, 0.1f, 0.2f, 1.0f});
            r.endFrame();
        }

        plat->swapBuffers();

        // FPS counter
        frameCount++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            char title[128];
            snprintf(title, sizeof(title), "Torch - %d FPS", frameCount);
            plat->setTitle(title);
            frameCount = 0;
            fpsTimer = 0;
        }
    }
}

void Engine::shutdown() {
    Console::instance().printf(LogLevel::Info, "Shutting down...");

    g->shutdown();
    net->shutdown();
    scr->shutdown();
    aud->shutdown();
    ren->shutdown();
    filesys->shutdown();
    plat->shutdown();

    delete g; g = nullptr;
    delete net; net = nullptr;
    delete scr; scr = nullptr;
    delete aud; aud = nullptr;
    delete ren; ren = nullptr;
    delete filesys; filesys = nullptr;
    delete plat; plat = nullptr;
    delete tim; tim = nullptr;

    Console::instance().printf(LogLevel::Info, "Goodbye");
}

bool Engine::isRunning() const { return running; }
