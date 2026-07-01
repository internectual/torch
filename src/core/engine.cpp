#include "core/engine.h"
#include <GL/glew.h>
#include "fs/vol_archive.h"
#include "fs/vl2_archive.h"
#include "script/torquescript.h"
#include "render/shader.h"
#include "render/renderer.h"
#include "render/dif_loader.h"
#include "game/hud.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <glob.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <sys/stat.h>

struct Engine::Impl {};

Engine::Engine() : impl(new Impl) {}
Engine::~Engine() { delete impl; }

Engine& Engine::instance() {
    static Engine eng;
    return eng;
}

bool Engine::init(int argc, char* argv[]) {
    // Check for help before any output
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stdout, "Torch v0.1.0 — Tribes 2 Open Source Client\n");
            fprintf(stdout, "Usage: torch [options]\n\n");
            fprintf(stdout, "Options:\n");
            fprintf(stdout, "  -data <dir>        Tribes 2 data directory\n");
            fprintf(stdout, "  -nologin           Skip login screen, start local game\n");
            fprintf(stdout, "  -demo <file.rec>   Play a demo recording\n");
            fprintf(stdout, "  -preview <map>     Load a map and take a screenshot\n");
            fprintf(stdout, "  -campos x y z      Preview camera position\n");
            fprintf(stdout, "  -camtarget x y z   Preview camera target\n");
            fprintf(stdout, "  -testshape <path>  Load and display a GLB shape\n");
            fprintf(stdout, "  -testdif <path>    Load and dump DIF interior stats\n");
            fprintf(stdout, "  -online            Enable online mode\n");
            fprintf(stdout, "  -version           Show version\n");
            fprintf(stdout, "  -help              Show this help\n");
            std::exit(0);
        }
        if (strcmp(argv[i], "-version") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stdout, "Torch v0.1.0\n");
            std::exit(0);
        }
    }

    Console::instance().printf(LogLevel::Info, "Torch v0.1.0");

    // Single-instance lock
    lockFd = open("/tmp/torch.lock", O_CREAT | O_RDWR, 0644);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) < 0) {
        if (lockFd >= 0) { close(lockFd); lockFd = -1; }
        Console::instance().printf(LogLevel::Error, "Another instance is already running");
        return false;
    }
    auto releaseLock = [this]() {
        if (lockFd >= 0) { close(lockFd); unlink("/tmp/torch.lock"); lockFd = -1; }
    };

    // Signal handlers for crash cleanup
    {
        static int* lockFdPtr = &lockFd;
        struct sigaction sa{};
        sa.sa_handler = [](int sig) {
            if (lockFdPtr && *lockFdPtr >= 0) { close(*lockFdPtr); unlink("/tmp/torch.lock"); *lockFdPtr = -1; }
            _exit(128 + sig);
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

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
        if ((strcmp(argv[i], "-demo") == 0 || strcmp(argv[i], "--demo") == 0) && i + 1 < argc)
            demoPath = argv[i + 1];
        if (strcmp(argv[i], "-testshape") == 0 && i + 1 < argc)
            testShapePath = argv[i + 1];
        if (strcmp(argv[i], "-testdif") == 0 && i + 1 < argc)
            testDifPath = argv[i + 1];
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
    gui = new GuiRenderer;

    // Platform
    PlatformConfig pconfig;
    pconfig.title = "TORCH";
    pconfig.width = Console::instance().getIntVariable("videoWidth", 1920);
    pconfig.height = Console::instance().getIntVariable("videoHeight", 1080);
    if (!plat->init(pconfig)) { releaseLock(); return false; }

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

    // -testdif: directly from disk, before slow VL2 mounting
    if (!testDifPath.empty()) {
        std::string fullPath = testDifPath;
        { struct stat st; if (stat(fullPath.c_str(), &st) != 0) fullPath = dataDir + "/" + testDifPath; }
        std::ifstream difFile(fullPath, std::ios::binary);
        if (!difFile) {
            Console::instance().printf(LogLevel::Error, "testdif: cannot open '%s'", fullPath.c_str());
        } else {
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(difFile)), {});
            DIFLoadResult r = loadDIF(d.data(), d.size(), fullPath.c_str(), true);
            if (r.loaded) {
                size_t totalVerts = 0, totalTris = 0;
                for (auto& m : r.meshes) { totalVerts += m.vertices.size(); totalTris += m.indices.size() / 3; }
                Console::instance().printf(LogLevel::Info, "--- DIF test: '%s' ---", fullPath.c_str());
                Console::instance().printf(LogLevel::Info, "  Meshes:    %zu", r.meshes.size());
                Console::instance().printf(LogLevel::Info, "  Vertices:  %zu", totalVerts);
                Console::instance().printf(LogLevel::Info, "  Triangles: %zu", totalTris);
                Console::instance().printf(LogLevel::Info, "  Textures:  %zu", r.textures.size());
                Console::instance().printf(LogLevel::Info, "  Lightmaps: %zu", r.lightmaps.size());
                Console::instance().printf(LogLevel::Info, "  Materials: %zu", r.materialNames.size());
                int lLoaded = 0;
                for (auto& lm : r.lightmaps) if (lm.loaded) lLoaded++;
                Console::instance().printf(LogLevel::Info, "  Lightmaps loaded: %d/%zu", lLoaded, r.lightmaps.size());
            } else {
                Console::instance().printf(LogLevel::Error, "testdif: failed to load '%s'", fullPath.c_str());
            }
        }
        exit(0);
    }

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
    if (!ren->init(plat->nativeWindow())) { releaseLock(); return false; }
    ren->config().width = plat->width();
    ren->config().height = plat->height();
    plat->setResizeCallback([this](int w, int h) { ren->onResize(w, h); });

    // Overlay font
    overlayFont = new Font;
    if (overlayFont->loadDefault()) {
        overlayFontOwned = true;
        ren->defaultFont = overlayFont;
    } else {
        delete overlayFont;
        overlayFont = nullptr;
        Console::instance().printf(LogLevel::Warn, "Failed to load overlay font");
    }

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
        if (argc > 1) {
            auto& scr = Engine::instance().script();
            if (scr.ts()) {
                scr.ts()->executeFile(argv[1]);
            } else {
                Console::instance().executeFile(argv[1]);
            }
        }
    });

    con->addCommand("screenshot", [this](int32_t argc, const char* const* argv) {
        const char* path = argc > 1 ? argv[1] : "screenshot.png";
        if (ren->screenshot(path))
            Console::instance().printf(LogLevel::Info, "Screenshot saved: %s", path);
        else
            Console::instance().printf(LogLevel::Error, "Screenshot failed: %s", path);
    }, "screenshot [path] - save a screenshot to path (default: screenshot.png)");

    Console::instance().printf(LogLevel::Info, "Engine initialized successfully");

    // Wire console commands into TorqueScript interpreter
    if (scr->ts()) {
        auto* tsi = scr->ts();
        con->forEach([tsi](const char* name, const Console::ConsoleItem& item) {
            if (item.type == Console::ConsoleItem::Command) {
                tsi->registerNative(name, [item](const auto& args) -> VMValue {
                    std::vector<const char*> argv;
                    std::vector<std::string> storage;
                    argv.push_back(item.name.c_str());
                    for (auto& a : args) {
                        storage.push_back(a.toString());
                        argv.push_back(storage.back().c_str());
                    }
                    if (item.cmd) item.cmd((int32_t)argv.size(), argv.data());
                    return VMValue(1);
                });
            }
        });
        Console::instance().printf(LogLevel::Info, "Registered console commands as TS natives");
    }

      // Skip startup scripts in -demo mode (they often block on login GUI)
      if (demoPath.empty()) {
          // For -nologin: just start the game directly, no GUI scripts
          if (noLogin || !previewMap.empty()) {
              Console::instance().printf(LogLevel::Info, "Skipping startup scripts for -nologin");
          } else {
              auto startupData = fs.read("console_start.cs");
              if (!startupData.empty() && scr->ts()) {
                  Console::instance().printf(LogLevel::Info, "Executing console_start.cs (%zu bytes)", startupData.size());
                  std::string csSource((const char*)startupData.data(), startupData.size());
                  // Remove the slow .gui file loading loop for faster startup
                  size_t pos = 0;
                  while ((pos = csSource.find("findFirstFile(\"*.gui\")", pos)) != std::string::npos) {
                      size_t lineStart = csSource.rfind('\n', pos);
                      if (lineStart == std::string::npos) lineStart = 0;
                      size_t execPos = csSource.find("exec(", pos);
                      if (execPos != std::string::npos) {
                          size_t semiPos = csSource.find(';', execPos);
                          if (semiPos != std::string::npos) {
                              csSource.erase(lineStart, semiPos - lineStart + 1);
                              Console::instance().printf(LogLevel::Info, "  (stripped .gui loading loop)");
                          } else { break; }
                      } else { break; }
                  }
                  csSource += "\n$SkipLogin = true; $pref::AcceptedEULA = true; LoginDone();\n";
                  scr->ts()->execute(csSource, "console_start.cs");
              }
          }
      }

    // Initialize GUI renderer from script-created objects
    gui->init();

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
    } else if (noLogin && demoPath.empty()) {
        Console::instance().printf(LogLevel::Info, "-nologin: starting local game");
        g->startLocalGame();
    }

    // -testshape: load a GLB shape for preview
    if (!testShapePath.empty()) {
        Console::instance().execute(("testshape(" + testShapePath + ")").c_str());
    }

    // -demo mode: load the demo (playback happens in the render loop)
    if (!demoPath.empty()) {
        g->playDemo(demoPath.c_str());
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

        // Pause key toggles TORCH overlay
        {
            static bool prevPause = false;
            bool pauseDown = plat->input().keysDown[SCANCODE_PAUSE];
            if (pauseDown && !prevPause) toggleOverlay();
            prevPause = pauseDown;
        }

        // M key toggles minimap
        {
            static bool prevM = false;
            bool mDown = plat->input().keysDown[SCANCODE_M];
            if (mDown && !prevM) showMinimap = !showMinimap;
            prevM = mDown;
        }

        // ESC toggles pause menu (when not in console)
        if (!showConsole) {
            static bool prevEsc = false;
            bool escDown = plat->input().keysDown[SCANCODE_ESCAPE];
            if (escDown && !prevEsc && g->state() != Game::MenuScreen)
                g->togglePauseGame();
            prevEsc = escDown;
            // Q during pause quits to desktop
            static bool prevQ = false;
            bool qDown = plat->input().keysDown[SCANCODE_Q];
            if (qDown && !prevQ && g->isGamePaused()) {
                quit();
            }
            prevQ = qDown;
        }

        // ~ key toggles console
        {
            static bool prevTilde = false;
            bool tildeDown = plat->input().keysDown[SCANCODE_GRAVE];
            if (tildeDown && !prevTilde) toggleConsole();
            prevTilde = tildeDown;
        }

        // Console text input
        {
            static bool prevBS = false, prevEnter = false, prevEsc = false;
            if (showConsole) {
                const std::string& ti = plat->input().textInput;
                if (!ti.empty()) {
                    for (char c : ti) {
                        if (c >= 0x20 && c <= 0x7e && consoleBuf.size() < 200)
                            consoleBuf += c;
                    }
                }
                // Backspace
                if (plat->input().keysDown[SCANCODE_BACKSPACE] && !prevBS) {
                    if (!consoleBuf.empty()) consoleBuf.pop_back();
                }
                prevBS = plat->input().keysDown[SCANCODE_BACKSPACE];
                // Enter
                if (plat->input().keysDown[SCANCODE_RETURN] && !prevEnter) {
                    executeConsole();
                }
                prevEnter = plat->input().keysDown[SCANCODE_RETURN];
                // Escape closes console without executing
                if (plat->input().keysDown[SCANCODE_ESCAPE] && !prevEsc) {
                    if (showConsole) { showConsole = false; plat->stopTextInput(); }
                }
                prevEsc = plat->input().keysDown[SCANCODE_ESCAPE];
            } else {
                // Reset edge-detection states when console closes
                prevBS = prevEnter = prevEsc = false;
            }
        }

        // Update subsystems
        net->update();
        g->gameServer().update();
        scr->vm()->setVariable("time", (float)now);

        // Process GUI events
        if (gui) gui->update(dt);

        // GUI mouse input
        bool guiHandled = false;
        if (gui && gui->getCanvas()) {
            int mx = plat->input().mouseX;
            int my = plat->input().mouseY;
            bool pressed = plat->input().mouseButtons[1] != 0;
            static bool prevPressed = false;
            if (pressed && !prevPressed) {
                guiHandled = gui->handleInput(mx, my, true);
            }
            prevPressed = pressed;
        }

        // Game update
        if (g->state() != Game::MenuScreen) {
            if (!showConsole && !g->isGamePaused()) {
                // Read input
                Game::InputMove input;
                auto& keys = plat->input().keysDown;
                auto& mButtons = plat->input().mouseButtons;
                input.forward = keys[SCANCODE_W] != 0;
                input.backward = keys[SCANCODE_S] != 0;
                input.left = keys[SCANCODE_A] != 0;
                input.right = keys[SCANCODE_D] != 0;
                input.jump = keys[SCANCODE_SPACE] != 0;
                input.jet = keys[SCANCODE_SPACE] != 0;
                input.freeCam = keys[SCANCODE_F1] != 0;
                input.orbitCam = keys[SCANCODE_F2] != 0;
                input.fire = mButtons[1] != 0;      // Left mouse
                input.altFire = mButtons[3] != 0;   // Right mouse
                input.zoom = mButtons[2] != 0;      // Middle mouse
                input.reload = keys[SCANCODE_R] != 0;
                input.showScoreboard = keys[SCANCODE_TAB] != 0;
                input.demoPause = keys[SCANCODE_P] != 0;
                input.demoStepFrame = keys[SCANCODE_PERIOD] != 0;
                input.demoShowEvents = keys[SCANCODE_E] != 0;
                input.lookDelta = {
                    (float)plat->input().mouseDeltaY * 0.002f,
                    (float)plat->input().mouseDeltaX * 0.002f,
                    0
                };

                // Weapon cycling - cooldown to avoid cycling too fast
                {
                    static float weaponCycleCooldown = 0;
                    weaponCycleCooldown -= dt;
                    int wheel = plat->input().mouseWheel;
                    if (weaponCycleCooldown <= 0 && wheel != 0) {
                        g->player().weaponCycle(wheel > 0 ? 1 : -1);
                        weaponCycleCooldown = 0.2f;
                    }
                }

                // Number keys for direct weapon selection
                static int lastNumKey = 0;
                for (int nk = 0; nk < 9; nk++) {
                    if (keys[30 + nk]) {
                        if (lastNumKey != nk + 1) {
                            g->player().selectWeapon(nk);
                            lastNumKey = nk + 1;
                        }
                        break;
                    }
                    if (lastNumKey && !keys[29 + lastNumKey]) lastNumKey = 0;
                }

                // Toggle relative mouse for FPS
                static bool wasInMenu = true;
                if (g->state() == Game::Playing && wasInMenu) {
                    plat->setRelativeMouse(true);
                    plat->showMouse(false);
                    wasInMenu = false;
                }
                if (g->state() != Game::Playing) wasInMenu = true;

                g->applyInput(input);
            }
            {
                static double lastTiming = 0;
                double t0 = Timer::now();
                g->update(dt);
                double t1 = Timer::now();
                g->render(dt);
                double t2 = Timer::now();
                if (t1 - lastTiming >= 1.0) {
                    lastTiming = t1;
                    Console::instance().printf(LogLevel::Debug, "TIMING: update=%.1fms render=%.1fms total=%.1fms",
                        (t1-t0)*1000, (t2-t1)*1000, (t2-t0)*1000);
                }
            }

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
            // Menu state
             g->menu().update(dt);
             plat->setRelativeMouse(false);
             plat->showMouse(true);
             auto& r = *ren;
             r.beginFrame({0.1f, 0.1f, 0.2f, 1.0f});
             // Render T2 GUI if canvas exists, otherwise fall back to simple menu
             if (gui && gui->getCanvas()) {
                 gui->render();
             } else {
                 g->menu().render();
             }
             r.endFrame();
        }

        // TORCH overlay (rendered on top of everything)
        if (showOverlay) renderOverlay();
        if (showConsole) renderConsole();
        if (showMinimap && g->isDemoPlaying()) renderMinimap();

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

    if (lockFd >= 0) { close(lockFd); unlink("/tmp/torch.lock"); lockFd = -1; }
}

void Engine::renderOverlay() {
    if (!overlayFont) return;
    float sy = 12.0f;

    int line = 0;
    float lx = 16, ly = 16;
    ColorF white = {1,1,1,1};
    ColorF dim = {0.6f, 0.6f, 0.6f, 1};
    ColorF cyan = {0.3f, 0.8f, 1, 1};

    overlayFont->render("=== TORCH Debug Menu ===", lx, ly + (line++) * sy, cyan, 1.0f);
    line++;

    std::string emuStr = "Emulation Version: 25034  [1/2 to switch]";
    overlayFont->render(emuStr.c_str(), lx, ly + (line++) * sy, white, 1.0f);
    line++;

    if (g->getDemoParser()) {
        uint32_t pv = g->getDemoParser()->getHeader().protocolVersion;
        char pvStr[64];
        snprintf(pvStr, sizeof(pvStr), "Detected Protocol: 0x%08X", (unsigned)pv);
        overlayFont->render(pvStr, lx, ly + (line++) * sy, dim, 1.0f);

        if (pv == T2Demo::ProtocolV24834)
            overlayFont->render("  (Tribes 2 v24834 - experimental)", lx, ly + (line++) * sy, dim, 1.0f);
        else if (pv == T2Demo::ProtocolV25034)
            overlayFont->render("  (Tribes 2 v25034 - supported)", lx, ly + (line++) * sy, dim, 1.0f);
        else
            overlayFont->render("  (unknown version)", lx, ly + (line++) * sy, dim, 1.0f);
        line++;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "State: %d  Demo: %s  Blocks: %d/%d",
        (int)g->state(),
        g->isDemoPlaying() ? "PLAYING" : "idle",
        g->getDemoBlocksDone(), g->getDemoBlocksTotal());
    overlayFont->render(buf, lx, ly + (line++) * sy, dim, 1.0f);

    // Current mission (tracks map changes across the demo)
    if (g->getDemoParser()) {
        const std::string& mis = g->getDemoParser()->currentMission();
        if (!mis.empty()) {
            snprintf(buf, sizeof(buf), "Mission: %s", mis.c_str());
            overlayFont->render(buf, lx, ly + (line++) * sy, white, 1.0f);
        }
    }

    line++;
    overlayFont->render("ESC to quit | Pause to close", lx, ly + (line++) * sy, dim, 1.0f);
}

void Engine::toggleConsole() {
    showConsole = !showConsole;
    if (showConsole) {
        plat->startTextInput();
    } else {
        plat->stopTextInput();
    }
}

void Engine::executeConsole() {
    if (consoleBuf.empty()) { showConsole = false; plat->stopTextInput(); return; }
    Console::instance().execute(consoleBuf.c_str());
    consoleBuf.clear();
    showConsole = false;
    plat->stopTextInput();
}

void Engine::renderConsole() {
    if (!overlayFont) return;
    auto& r = *ren;
    // Background: dark semi-transparent box covering top quarter
    r.drawBox({{0, 0, 0}, {800, 200, 0}}, {0, 0, 0, 0.75f});
    // Border line
    r.drawLine({0, 202, 0}, {800, 202, 0}, {0.3f, 0.3f, 0.5f, 0.8f});
    // Log history (last 5 lines from console)
    const auto& log = Console::instance().getLog();
    int logSize = (int)log.size();
    int logStart = std::max(0, logSize - 5);
    for (int i = logStart; i < logSize; i++) {
        overlayFont->render(log[i].c_str(), 8, (float)((i - logStart) * 22 + 4),
            ColorF{0.7f, 0.7f, 0.7f, 0.9f}, 1.2f);
    }
    // Input line with cursor
    static float cursorTimer = 0;
    cursorTimer += 0.05f;
    bool cursorOn = ((int)(cursorTimer / 0.4f) % 2) == 0;
    std::string display = "> " + consoleBuf;
    if (cursorOn) display += '_';
    overlayFont->render(display.c_str(), 8, 170, ColorF{0.2f, 0.8f, 0.2f, 1}, 1.5f);
}

void Engine::renderMinimap() {
    const auto& path = g->getDemoPath();
    if (path.empty()) return;

    // Compute bounds of all path points (x,z)
    float minX = path[0].x, maxX = path[0].x;
    float minZ = path[0].z, maxZ = path[0].z;
    for (auto& p : path) {
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.z < minZ) minZ = p.z;
        if (p.z > maxZ) maxZ = p.z;
    }
    float rangeX = maxX - minX;
    float rangeZ = maxZ - minZ;
    if (rangeX < 1.0f) rangeX = 1.0f;
    if (rangeZ < 1.0f) rangeZ = 1.0f;

    auto w = (float)plat->width();
    auto h = (float)plat->height();
    float mapSize = 200.0f;
    float margin = 16.0f;
    float ox = w - mapSize - margin; // right edge
    float oy = h - mapSize - margin; // bottom edge

    // Build orthographic projection mapping minimap area to NDC
    MatrixF ortho;
    ortho.identity();
    ortho.m[0][0] = 2.0f / mapSize;
    ortho.m[1][1] = -2.0f / mapSize; // flip Y so top = north
    ortho.m[3][0] = -1.0f - (2.0f * ox / mapSize);
    ortho.m[3][1] = 1.0f + (2.0f * oy / mapSize);

    // Background: dark rectangle (draw as 2 triangles via line shader approximation)
    auto* ls = ShaderManager::getLineShader();
    if (!ls) return;
    ls->bind();
    ls->setUniform("uProjection", ortho);
    MatrixF id; id.identity();
    ls->setUniform("uView", id);

    // Draw border rectangle using lines
    {
        ls->setUniform("uColor", Point3F{0.3f, 0.3f, 0.4f});
        std::vector<Point3F> border = {
            {ox, oy, 0}, {ox + mapSize, oy, 0}, {ox + mapSize, oy + mapSize, 0},
            {ox, oy + mapSize, 0}, {ox, oy, 0}
        };
        size_t bc = border.size();
        std::vector<float> verts(bc * 3);
        for (size_t i = 0; i < bc; i++) { verts[i*3] = border[i].x; verts[i*3+1] = border[i].y; verts[i*3+2] = 0; }
        uint32_t vao, vbo;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)bc);
        glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo);
    }

    // Draw path trail in green
    if (path.size() > 1) {
        ls->setUniform("uColor", Point3F{0.2f, 0.8f, 0.2f});
        size_t pc = path.size();
        // Subsample to avoid too many verts
        int step = (int)(pc / 4000) + 1;
        std::vector<float> verts;
        verts.reserve((pc / step + 1) * 3);
        for (size_t i = 0; i < pc; i += step) {
            float nx = ox + (path[i].x - minX) / rangeX * mapSize;
            float ny = oy + (path[i].z - minZ) / rangeZ * mapSize;
            verts.push_back(nx); verts.push_back(ny); verts.push_back(0);
        }
        uint32_t vao, vbo;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(verts.size() / 3));
        glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo);
    }

    // Current position marker (red cross)
    if (g->demoHasPosition() && !path.empty()) {
        auto& last = path.back();
        float cx = ox + (last.x - minX) / rangeX * mapSize;
        float cy = oy + (last.z - minZ) / rangeZ * mapSize;
        float ms = 4;
        ls->setUniform("uColor", Point3F{1.0f, 0.2f, 0.2f});
        std::vector<Point3F> cross = {
            {cx - ms, cy, 0}, {cx + ms, cy, 0},
            {cx, cy - ms, 0}, {cx, cy + ms, 0}
        };
        std::vector<float> verts(cross.size() * 3);
        for (size_t i = 0; i < cross.size(); i++) { verts[i*3] = cross[i].x; verts[i*3+1] = cross[i].y; verts[i*3+2] = 0; }
        uint32_t vao, vbo;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_LINES, 0, (GLsizei)(verts.size() / 3));
        glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo);
    }
}

bool Engine::isRunning() const { return running; }
