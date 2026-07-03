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
#include <sstream>
#include <unordered_set>
#include <algorithm>
#include <ctime>

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
            fprintf(stdout, "  -demo <file.rec>   Play a demo recording\n");
            fprintf(stdout, "  -preview <map>     Load a map and take a screenshot\n");
            fprintf(stdout, "  -campos x y z      Preview camera position\n");
            fprintf(stdout, "  -camtarget x y z   Preview camera target\n");
            fprintf(stdout, "  -testshape <path>  Load and display a GLB shape\n");
            fprintf(stdout, "  -testdif <path>    Load and dump DIF interior stats\n");
            fprintf(stdout, "  -preload <files>   Comma-separated scripts/guis to preload\n");
            fprintf(stdout, "  -version           Show version\n");
            fprintf(stdout, "  -previewImg <path> Preview PNG to show in bottom-right corner\n");
            fprintf(stdout, "  -help              Show this help\n\n");
            fprintf(stdout, "Script args are passed through unmodified to the init script.\n\n");
            fprintf(stdout, "Controls:\n");
            fprintf(stdout, "  WASD               Move / free camera\n");
            fprintf(stdout, "  Mouse              Look around\n");
            fprintf(stdout, "  Left Click         Fire weapon\n");
            fprintf(stdout, "  Right Click        Alt fire\n");
            fprintf(stdout, "  Space              Jump / Jet\n");
            fprintf(stdout, "  F1                 Free camera toggle\n");
            fprintf(stdout, "  F2                 Orbit camera (demo)\n");
            fprintf(stdout, "  R                  Cycle spectate target (demo)\n");
            fprintf(stdout, "  P                  Pause demo\n");
            fprintf(stdout, "  .                  Step demo frame\n");
            fprintf(stdout, "  Tab                Scoreboard\n");
            fprintf(stdout, "  ~                  Console\n");
            fprintf(stdout, "  Pause              Debug overlay\n");
            fprintf(stdout, "  ESC                Pause / Quit\n");
            fprintf(stdout, "  1-0                Select weapon\n");
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

    // Load config from torch.cfg
    std::string dataDir = ".";
    {
        std::ifstream cfg("torch.cfg");
        if (!cfg) {
            // Try next to the executable
            std::string exePath = argc > 0 ? argv[0] : "";
            auto sl = exePath.rfind('/');
            if (sl != std::string::npos)
                cfg.open(exePath.substr(0, sl + 1) + "torch.cfg");
        }
        if (cfg) {
            std::string line;
            while (std::getline(cfg, line)) {
                // Skip comments and empty lines
                if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                // Trim whitespace
                auto trim = [](std::string& s) {
                    while (!s.empty() && (s[0]==' '||s[0]=='\t'||s[0]=='\r')) s.erase(0,1);
                    while (!s.empty() && (s.back()==' '||s.back()=='\t'||s.back()=='\r')) s.pop_back();
                };
                trim(key); trim(val);
                if (key == "dataDir") dataDir = val;
                if (key == "previewImg") previewImgPath = val;
                if (key == "preload") {
                    // Comma-separated list of files to load at startup
                    size_t pos = 0, comma;
                    while ((comma = val.find(',', pos)) != std::string::npos) {
                        std::string f = val.substr(pos, comma - pos);
                        auto trim = [](std::string& s) { while (!s.empty()&&s[0]==' ') s.erase(0,1); while (!s.empty()&&s.back()==' ') s.pop_back(); };
                        trim(f);
                        if (!f.empty()) preloadFiles.push_back(f);
                        pos = comma + 1;
                    }
                    std::string f = val.substr(pos);
                    auto trim = [](std::string& s) { while (!s.empty()&&s[0]==' ') s.erase(0,1); while (!s.empty()&&s.back()==' ') s.pop_back(); };
                    trim(f);
                    if (!f.empty()) preloadFiles.push_back(f);
                }
            }
        }
    }
    if (dataDir == ".") dataDir = "/home/methodown/t2-linux"; // fallback
    Console::instance().setVariable("dataDir", dataDir.c_str());

    // Parse args
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
        if (strcmp(argv[i], "-previewImg") == 0 && i + 1 < argc)
            previewImgPath = argv[i + 1];
        if (strcmp(argv[i], "-preload") == 0 && i + 1 < argc) {
            std::string val = argv[i + 1];
            size_t pos = 0, comma;
            while ((comma = val.find(',', pos)) != std::string::npos) {
                std::string f = val.substr(pos, comma - pos);
                auto trim = [](std::string& s) { while (!s.empty()&&s[0]==' ') s.erase(0,1); while (!s.empty()&&s.back()==' ') s.pop_back(); };
                trim(f);
                if (!f.empty()) preloadFiles.push_back(f);
                pos = comma + 1;
            }
            std::string f = val.substr(pos);
            auto trim = [](std::string& s) { while (!s.empty()&&s[0]==' ') s.erase(0,1); while (!s.empty()&&s.back()==' ') s.pop_back(); };
            trim(f);
            if (!f.empty()) preloadFiles.push_back(f);
        }
    }

    // Store command-line args for display/passthrough to init scripts
    for (int i = 0; i < argc; i++) {
        if (!cmdArgs.empty()) cmdArgs += " ";
        cmdArgs += argv[i];
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

    // Login flow commands
    con->addCommand("LoginDone", [this](int32_t, const char* const*) {
        gui->popDialog("LoginDlg");
        Console::instance().printf(LogLevel::Info, "Login complete, starting game");
        g->startLocalGame();
    }, "LoginDone() - transition from login to game");

    con->addCommand("LoginProcess", [](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "LoginProcess called (stub)");
        Console::instance().execute("LoginDone()");
    }, "LoginProcess - attempt login");

    con->addCommand("CreateAccount", [](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "CreateAccount called (stub)");
    });

    con->addCommand("PasswordProcess", [](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "PasswordProcess called (stub)");
    });

    con->addCommand("Disconnect", [](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "Disconnect called");
    });

    // Init script path management
    Console::instance().setVariable("initScript", "console_start.cs");
    con->addCommand("setScriptPath", [](int32_t argc, const char* const* argv) {
        if (argc > 1) {
            Console::instance().setVariable("initScript", argv[1]);
            Console::instance().printf(LogLevel::Info, "Init script set to: %s", argv[1]);
        } else {
            Console::instance().printf(LogLevel::Info, "Init script: %s", Console::instance().getStringVariable("initScript", ""));
        }
    }, "setScriptPath <path> - set the init script path (relative to dataDir)");

    Console::instance().printf(LogLevel::Info, "Engine initialized successfully");

    // Load essential scripts: profiles first, then startup
    if (scr->ts()) {
        auto* ts = scr->ts();
        // Try loading the main profile definitions
        const char* profileScripts[] = {
            "gui/guiProfiles.cs",
            "scripts/EditorProfiles.cs",
            nullptr
        };
        for (int i = 0; profileScripts[i]; i++) {
            auto sdata = fs.read(profileScripts[i]);
            if (!sdata.empty()) {
                Console::instance().printf(LogLevel::Info, "Loading script: %s (%zu bytes)", profileScripts[i], sdata.size());
                ts->execute(std::string((const char*)sdata.data(), sdata.size()), profileScripts[i]);
            } else {
                Console::instance().printf(LogLevel::Debug, "Script not found: %s", profileScripts[i]);
            }
        }
        Console::instance().printf(LogLevel::Info, "Profiles loaded: %zu script objects", scr->objects.size());
    }

    // Execute preload files (from torch.cfg preload = ...)
    if (scr->ts() && !preloadFiles.empty()) {
        auto* ts = scr->ts();
        Console::instance().printf(LogLevel::Info, "Preloading %zu file(s)...", preloadFiles.size());
        for (auto& pf : preloadFiles) {
            auto pdata = fs.read(pf.c_str());
            if (!pdata.empty()) {
                Console::instance().printf(LogLevel::Info, "Preload: %s (%zu bytes)", pf.c_str(), pdata.size());
                ts->execute(std::string((const char*)pdata.data(), pdata.size()), pf);
            } else {
                Console::instance().printf(LogLevel::Warn, "Preload: file not found: %s", pf.c_str());
            }
        }
    }

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

    // Load essential GUI files for HUD, menus, and login
    {
        const char* guiFiles[] = {
            "gui/PlayGui.gui",
            "gui/GameGui.gui",
            "gui/ChatGui.gui",
            "gui/HUDDlgs.gui",
            "gui/LoadingGui.gui",
            "gui/CenterPrint.gui",
            "gui/EULADlg.gui",
            "gui/LoginDlg.gui",
            "gui/ImmSplashDlg.gui",
            "gui/CreateAccountDlg.gui",
            "gui/LoginMessageBoxDlg.gui",
            "gui/LoginMessagePopupDlg.gui",
            "gui/EditAccountDlg.gui",
            "gui/PickLoginInfoDlg.gui",
            "gui/PasswordDlg.gui",
            "gui/PickTeamDlg.gui",
            "gui/MessageBoxDlg.gui",
            "gui/MessagePopupDlg.gui",
            "gui/OptionsDlg.gui",
            "gui/ServerInfoDlg.gui",
            "gui/RecordingsDlg.gui",
            "gui/DemoPlaybackDlg.gui",
            "gui/DemoLoadProgressDlg.gui",
            "gui/TrainingGui.gui",
            "gui/ConsoleDlg.gui",
            nullptr
        };
        for (int i = 0; guiFiles[i]; i++) {
            if (scr->ts()) {
                auto guiData = fs.read(guiFiles[i]);
                if (!guiData.empty()) {
                    std::string src((const char*)guiData.data(), guiData.size());
                    scr->ts()->execute(src, guiFiles[i]);
                    Console::instance().printf(LogLevel::Info, "Loaded GUI: %s", guiFiles[i]);
                } else {
                    Console::instance().printf(LogLevel::Debug, "GUI not found: %s", guiFiles[i]);
                }
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
    } else if (noLogin) {
        Console::instance().printf(LogLevel::Info, "-nologin: dev panel (F1 overlay, ~ console, Pause debug)");
    } else if (demoPath.empty() && previewMap.empty()) {
        Console::instance().printf(LogLevel::Info, "Pushing login dialog");
        gui->pushDialog("LoginDlg");
        // Set RubyEnabled so the login scripts don't block
        Console::instance().setVariable("RubyEnabled", "1");
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
        if (!gui->isDialogActive("ConsoleDlg")) {
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

        // ~ key toggles console via GUI ConsoleDlg
        {
            static bool prevTilde = false;
            bool tildeDown = plat->input().keysDown[SCANCODE_GRAVE];
            if (tildeDown && !prevTilde) {
                if (gui->isDialogActive("ConsoleDlg")) {
                    gui->popDialog("ConsoleDlg");
                    plat->stopTextInput();
                    if (g->state() == Game::Playing) {
                        plat->setRelativeMouse(true);
                        plat->showMouse(false);
                    }
                } else {
                    gui->pushDialog("ConsoleDlg");
                    plat->startTextInput();
                    plat->setRelativeMouse(false);
                    plat->showMouse(true);
                }
            }
            prevTilde = tildeDown;
        }

        // Console text input handled by GuiConsoleEditCtrl
        {
            static bool prevBS = false, prevEnter = false, prevEsc = false;
            static bool prevUp = false, prevDown = false;
            static bool prevPgUp = false, prevPgDn = false, prevHome = false, prevEnd = false;
            static bool prevLeft = false, prevRight = false;
            static std::vector<std::string> history;
            static int historyIdx = -1;
            bool guiConsoleActive = gui && gui->isDialogActive("ConsoleDlg");
            if (guiConsoleActive) {
                GuiControl* entry = gui->findControl("ConsoleEntry");
                const std::string& ti = plat->input().textInput;
                if (!ti.empty()) {
                    if (entry) {
                        for (char c : ti) {
                            if (c >= 0x20 && c <= 0x7e && entry->text.size() < 200)
                                entry->text += c;
                        }
                    }
                }
                // Backspace
                if (plat->input().keysDown[SCANCODE_BACKSPACE] && !prevBS) {
                    if (entry && !entry->text.empty()) entry->text.pop_back();
                }
                prevBS = plat->input().keysDown[SCANCODE_BACKSPACE];
                // Up arrow - history back
                if (plat->input().keysDown[SCANCODE_UP] && !prevUp) {
                    if (!history.empty()) {
                        if (historyIdx < 0) historyIdx = (int)history.size() - 1;
                        else if (historyIdx > 0) historyIdx--;
                        if (entry) entry->text = history[historyIdx];
                    }
                }
                prevUp = plat->input().keysDown[SCANCODE_UP];
                // Down arrow - history forward
                if (plat->input().keysDown[SCANCODE_DOWN] && !prevDown) {
                    if (!history.empty() && historyIdx >= 0) {
                        if (historyIdx < (int)history.size() - 1) {
                            historyIdx++;
                            if (entry) entry->text = history[historyIdx];
                        } else {
                            historyIdx = -1;
                            if (entry) entry->text.clear();
                        }
                    }
                }
                prevDown = plat->input().keysDown[SCANCODE_DOWN];
                // Enter - execute console command
                if (plat->input().keysDown[SCANCODE_RETURN] && !prevEnter) {
                    if (entry && !entry->text.empty()) {
                        std::string cmd = entry->text;
                        history.push_back(cmd);
                        historyIdx = -1;
                        entry->text.clear();
                        Console::instance().printf(LogLevel::Info, "==> %s", cmd.c_str());
                        // @ prefix: send to AI via file IPC
                        if (!cmd.empty() && cmd[0] == '@') {
                            std::string query = cmd.substr(1);
                            // Trim leading whitespace
                            while (!query.empty() && query[0] == ' ') query.erase(0, 1);
                            if (!query.empty()) {
                                std::ofstream qf("/tmp/torch_ai_query.txt");
                                if (qf) qf << query;
                                Console::instance().printf(LogLevel::Info, "[AI] %s", query.c_str());
                            }
                        } else {
                            Console::instance().execute(cmd.c_str());
                        }
                    }
                }
                prevEnter = plat->input().keysDown[SCANCODE_RETURN];
                // Escape closes console
                if (plat->input().keysDown[SCANCODE_ESCAPE] && !prevEsc) {
                    gui->popDialog("ConsoleDlg");
                }
                prevEsc = plat->input().keysDown[SCANCODE_ESCAPE];
                // Scroll: PageUp/PageDown, Home/End, Left/Right
                auto findScroll = [&]() -> GuiControl* {
                    std::function<GuiControl*(GuiControl*)> f = [&](GuiControl* c) -> GuiControl* {
                        if (!c) return nullptr;
                        if (c->className == "GuiScrollCtrl" || c->className == "ShellScrollCtrl") return c;
                        for (auto* ch : c->children) { auto* r = f(ch); if (r) return r; }
                        return nullptr;
                    };
                    return f(gui->activeDialog());
                };
                GuiControl* sc = findScroll();
                if (sc) {
                    float pageH = sc->extentY * 0.9f;
                    if (plat->input().keysDown[SCANCODE_PAGEUP] && !prevPgUp) sc->scrollY += pageH;
                    if (plat->input().keysDown[SCANCODE_PAGEDOWN] && !prevPgDn) sc->scrollY -= pageH;
                    if (plat->input().keysDown[SCANCODE_HOME] && !prevHome) sc->scrollY = 1e10f;
                    if (plat->input().keysDown[SCANCODE_END] && !prevEnd) sc->scrollY = 0;
                    if (plat->input().keysDown[SCANCODE_LEFT] && !prevLeft) sc->scrollX -= 8;
                    if (plat->input().keysDown[SCANCODE_RIGHT] && !prevRight) sc->scrollX += 8;
                }
                prevPgUp = plat->input().keysDown[SCANCODE_PAGEUP];
                prevPgDn = plat->input().keysDown[SCANCODE_PAGEDOWN];
                prevHome = plat->input().keysDown[SCANCODE_HOME];
                prevEnd = plat->input().keysDown[SCANCODE_END];
                prevLeft = plat->input().keysDown[SCANCODE_LEFT];
                prevRight = plat->input().keysDown[SCANCODE_RIGHT];
            } else {
                prevBS = prevEnter = prevEsc = prevUp = prevDown = false;
                prevPgUp = prevPgDn = prevHome = prevEnd = prevLeft = prevRight = false;
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

        // Mouse wheel: GUI scroll or weapon cycle (outside console check)
        {
            static float weaponCycleCooldown = 0;
            weaponCycleCooldown -= dt;
            int wheel = plat->input().mouseWheel;
            if (wheel != 0) {
                bool scrolled = false;
                if (gui) {
                    std::function<GuiControl*(GuiControl*)> findScroll = [&](GuiControl* ctl) -> GuiControl* {
                        if (!ctl) return nullptr;
                        if (ctl->className == "GuiScrollCtrl" || ctl->className == "ShellScrollCtrl")
                            return ctl;
                        for (auto* ch : ctl->children) {
                            auto* r = findScroll(ch);
                            if (r) return r;
                        }
                        return nullptr;
                    };
                    GuiControl* sc = findScroll(gui->activeDialog());
                    if (sc) { sc->scrollY += (wheel > 0 ? 30 : -30); scrolled = true; }
                }
                if (!scrolled && weaponCycleCooldown <= 0) {
                    g->player().weaponCycle(wheel > 0 ? 1 : -1);
                    weaponCycleCooldown = 0.2f;
                }
            }
        }

        // Game update + 3D render (if playing / test shape)
        bool isPlaying = (g->state() != Game::MenuScreen || g->isTestShapeLoaded());
        if (isPlaying) {
            if (!gui->isDialogActive("ConsoleDlg") && !g->isGamePaused()) {
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
                input.fire = mButtons[1] != 0;
                input.altFire = mButtons[3] != 0;
                input.zoom = mButtons[2] != 0;
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
                static int lastNumKey = 0;
                for (int nk = 0; nk < 9; nk++) {
                    if (keys[30 + nk]) {
                        if (lastNumKey != nk + 1) { g->player().selectWeapon(nk); lastNumKey = nk + 1; }
                        break;
                    }
                    if (lastNumKey && !keys[29 + lastNumKey]) lastNumKey = 0;
                }
                static bool wasInMenu = true;
                if (g->state() == Game::Playing && wasInMenu) { plat->setRelativeMouse(true); plat->showMouse(false); wasInMenu = false; }
                if (g->state() != Game::Playing) wasInMenu = true;
                g->applyInput(input);
            }
            {
                static double lastTiming = 0;
                double t0 = Timer::now();
                g->update(dt);
                double t1 = Timer::now();
                g->render(dt);  // 3D render with own beginFrame/endFrame
                double t2 = Timer::now();
                if (t1 - lastTiming >= 5.0) {
                    lastTiming = t1;
                    Console::instance().printf(LogLevel::Debug, "TIMING: update=%.1fms render=%.1fms total=%.1fms",
                        (t1-t0)*1000, (t2-t1)*1000, (t2-t0)*1000);
                }
            }
            // Preview mode
            if (!previewMap.empty() && !previewDone) {
                char path[256];
                snprintf(path, sizeof(path), "preview_%s.png", previewMap.c_str());
                if (ren->screenshot(path)) Console::instance().printf(LogLevel::Info, "Preview saved: %s", path);
                else Console::instance().printf(LogLevel::Error, "Preview screenshot failed");
                previewDone = true; quit();
            }
        }

        // ─── Dev panel (always rendered) ───────────────────────────────────
        g->menu().update(dt);
        plat->setRelativeMouse(false);
        plat->showMouse(true);
        {
            auto& r = *ren;
            int w = (int)plat->width(), h = (int)plat->height();
            bool menuState = !isPlaying;

            if (menuState) r.beginFrame({0.05f, 0.05f, 0.08f, 1.0f});

            // Ortho projection for 2D rendering
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            {
                MatrixF ortho;
                ortho.identity();
                ortho.m[0][0] = 2.0f / w;
                ortho.m[1][1] = -2.0f / h;
                ortho.m[3][0] = -1.0f;
                ortho.m[3][1] = 1.0f;
                r.setProjection(ortho);
                r.setView(MatrixF{});
                ren->setViewport(0, 0, w, h);
            }

            // Poll AI response file
            {
                struct stat st;
                static time_t lastRespCheck = 0;
                time_t now = time(nullptr);
                if (now != lastRespCheck && stat("/tmp/torch_ai_response.txt", &st) == 0 && st.st_size > 0) {
                    std::ifstream rf("/tmp/torch_ai_response.txt");
                    std::string resp;
                    if (rf) {
                        std::getline(rf, resp, '\0'); // read all
                        if (!resp.empty()) {
                            Console::instance().printf(LogLevel::Info, "[AI] %s", resp.c_str());
                        }
                    }
                    rf.close();
                    // Remove response file after reading
                    std::ofstream clr("/tmp/torch_ai_response.txt", std::ios::trunc);
                    clr.close();
                    unlink("/tmp/torch_ai_response.txt");
                }
                lastRespCheck = now;
            }

            // Right-side dev info panel (clipped to canvas bottom at y=480)
            if (overlayFont) {
                char buf[256];
                int ly = 4;
                const int rightX = 650;
                const int canvasBottom = 480;

                // dataDir
                std::string cfgDir = Console::instance().getStringVariable("dataDir", ".");
                overlayFont->render(("dataDir: " + cfgDir).c_str(), rightX, ly, {0.5f, 0.5f, 0.5f, 1}, 1.0f); ly += 14;

                // Editable init path
                static std::string editBuf;
                static bool pathFocused = false;
                static int pathCursor = 0;
                static float blink = 0;
                if (!pathFocused) editBuf = Console::instance().getStringVariable("initScript", "console_start.cs");
                std::string sp = editBuf;
                overlayFont->render("init:", rightX, ly, {0.5f, 0.8f, 1, 1}, 1.0f);
                float pathX = (float)(rightX + 50);
                ColorF pathCol = pathFocused ? ColorF{0,1,0.5f,1} : ColorF{1,1,1,1};
                overlayFont->render(sp.c_str(), pathX, (float)ly, pathCol, 1.0f);
                if (pathFocused) {
                    blink += 0.05f;
                    if ((int)(blink / 0.4f) % 2 == 0)
                        overlayFont->render("_", pathX + (float)pathCursor * 8.0f, (float)ly, {0,1,0.5f,1}, 1.0f);
                }
                float launchX = pathX + std::max((float)sp.size(), 1.0f) * 8.0f + 10;
                float launchY = (float)ly;
                overlayFont->render(" [Launch]", launchX, launchY, {0,1,0.5f,1}, 1.0f); ly += 16;

                // Editable args field
                static std::string argsBuf;
                static bool argsFocused = false;
                static int argsCursor = 0;
                static float argsBlink = 0;
                int argsY = ly;
                if (!argsFocused) argsBuf = cmdArgs;
                overlayFont->render("args:", rightX, ly, {0.5f, 0.8f, 1, 1}, 1.0f);
                float argsX = (float)(rightX + 50);
                ColorF argsCol = argsFocused ? ColorF{0,1,0.5f,1} : ColorF{1,1,1,1};
                overlayFont->render(argsBuf.c_str(), argsX, (float)ly, argsCol, 1.0f);
                if (argsFocused) {
                    argsBlink += 0.05f;
                    if ((int)(argsBlink / 0.4f) % 2 == 0)
                        overlayFont->render("_", argsX + (float)argsCursor * 8.0f, (float)ly, {0,1,0.5f,1}, 1.0f);
                }
                ly += 16;

                // Launch/Enter handling
                static bool prevLaunchEnter = false, prevLaunchClick = false;
                static bool prevPathBS = false, prevPathEsc = false, prevPathDel = false;
                static bool prevPathLeft = false, prevPathRight = false, prevPathHome = false, prevPathEnd = false;
                static bool prevArgsBS = false, prevArgsEsc = false, prevArgsDel = false;
                static bool prevArgsLeft = false, prevArgsRight = false, prevArgsHome = false, prevArgsEnd = false;
                bool launchEnter = plat->input().keysDown[SCANCODE_RETURN];
                auto doLaunch = [&]() {
                    std::string path = pathFocused ? editBuf : Console::instance().getStringVariable("initScript", "console_start.cs");
                    if (!path.empty() && scr->ts()) {
                        auto sdata = Engine::instance().fs().read(path.c_str());
                        if (!sdata.empty()) {
                            Console::instance().printf(LogLevel::Info, "Launching script: %s (%zu bytes)", path.c_str(), sdata.size());
                            scr->ts()->execute(std::string((const char*)sdata.data(), sdata.size()), path);
                            Console::instance().setVariable("initScript", path.c_str());
                            std::ofstream of("torch.cfg", std::ios::app);
                            if (of) of << "initScript = " << path << std::endl;
                        } else {
                            Console::instance().printf(LogLevel::Warn, "Script not found: %s", sp.c_str());
                        }
                    }
                };
                if (launchEnter && !prevLaunchEnter && !gui->isDialogActive("ConsoleDlg")) doLaunch();
                prevLaunchEnter = launchEnter;

                // Mouse click handling for path/args/launch
                if (plat->input().mouseButtons[1] && !prevLaunchClick) {
                    float mx = (float)plat->input().mouseX, my = (float)plat->input().mouseY;
                    if (mx >= launchX && mx <= launchX + 72 && my >= launchY && my <= launchY + 14)
                        doLaunch();
                    else if (mx >= pathX && mx <= pathX + (float)editBuf.size() * 8.0f + 30 && my >= launchY && my <= launchY + 14)
                        pathFocused = !pathFocused;
                    else if (mx >= argsX && mx <= argsX + (float)argsBuf.size() * 8.0f + 30 && my >= argsY && my <= argsY + 14)
                        argsFocused = !argsFocused;
                }
                prevLaunchClick = plat->input().mouseButtons[1];

                // Text input for path field
                if (pathFocused) {
                    const std::string& ti = plat->input().textInput;
                    for (char c : ti) {
                        if (c >= 0x20 && c <= 0x7e && editBuf.size() < 200) {
                            editBuf.insert(editBuf.begin() + pathCursor, c);
                            pathCursor++;
                        }
                    }
                    if (plat->input().keysDown[SCANCODE_BACKSPACE] && !prevPathBS) { if (pathCursor > 0) { editBuf.erase(pathCursor - 1, 1); pathCursor--; } }
                    prevPathBS = plat->input().keysDown[SCANCODE_BACKSPACE];
                    if (plat->input().keysDown[SCANCODE_DELETE] && !prevPathDel) { if (pathCursor < (int)editBuf.size()) editBuf.erase(pathCursor, 1); }
                    prevPathDel = plat->input().keysDown[SCANCODE_DELETE];
                    if (plat->input().keysDown[SCANCODE_LEFT] && !prevPathLeft) pathCursor--;
                    if (plat->input().keysDown[SCANCODE_RIGHT] && !prevPathRight) pathCursor++;
                    prevPathLeft = plat->input().keysDown[SCANCODE_LEFT];
                    prevPathRight = plat->input().keysDown[SCANCODE_RIGHT];
                    if (plat->input().keysDown[SCANCODE_HOME] && !prevPathHome) pathCursor = 0;
                    if (plat->input().keysDown[SCANCODE_END] && !prevPathEnd) pathCursor = (int)editBuf.size();
                    prevPathHome = plat->input().keysDown[SCANCODE_HOME];
                    prevPathEnd = plat->input().keysDown[SCANCODE_END];
                    if (pathCursor < 0) pathCursor = 0;
                    if (pathCursor > (int)editBuf.size()) pathCursor = (int)editBuf.size();
                    if (launchEnter && !prevLaunchEnter) { Console::instance().setVariable("initScript", editBuf.c_str()); pathFocused = false; doLaunch(); }
                    if (plat->input().keysDown[SCANCODE_ESCAPE] && !prevPathEsc) { pathFocused = false; }
                    prevPathEsc = plat->input().keysDown[SCANCODE_ESCAPE];
                } else {
                    prevPathBS = prevPathEsc = prevPathDel = false;
                    prevPathLeft = prevPathRight = false;
                    prevPathHome = prevPathEnd = false;
                    pathCursor = (int)editBuf.size();
                }

                // Text input for args field
                if (argsFocused) {
                    const std::string& ti = plat->input().textInput;
                    for (char c : ti) {
                        if (c >= 0x20 && c <= 0x7e && argsBuf.size() < 500) {
                            argsBuf.insert(argsBuf.begin() + argsCursor, c);
                            argsCursor++;
                        }
                    }
                    if (plat->input().keysDown[SCANCODE_BACKSPACE] && !prevArgsBS) { if (argsCursor > 0) { argsBuf.erase(argsCursor - 1, 1); argsCursor--; } }
                    prevArgsBS = plat->input().keysDown[SCANCODE_BACKSPACE];
                    if (plat->input().keysDown[SCANCODE_DELETE] && !prevArgsDel) { if (argsCursor < (int)argsBuf.size()) argsBuf.erase(argsCursor, 1); }
                    prevArgsDel = plat->input().keysDown[SCANCODE_DELETE];
                    if (plat->input().keysDown[SCANCODE_LEFT] && !prevArgsLeft) argsCursor--;
                    if (plat->input().keysDown[SCANCODE_RIGHT] && !prevArgsRight) argsCursor++;
                    prevArgsLeft = plat->input().keysDown[SCANCODE_LEFT];
                    prevArgsRight = plat->input().keysDown[SCANCODE_RIGHT];
                    if (plat->input().keysDown[SCANCODE_HOME] && !prevArgsHome) argsCursor = 0;
                    if (plat->input().keysDown[SCANCODE_END] && !prevArgsEnd) argsCursor = (int)argsBuf.size();
                    prevArgsHome = plat->input().keysDown[SCANCODE_HOME];
                    prevArgsEnd = plat->input().keysDown[SCANCODE_END];
                    if (argsCursor < 0) argsCursor = 0;
                    if (argsCursor > (int)argsBuf.size()) argsCursor = (int)argsBuf.size();
                    if (plat->input().keysDown[SCANCODE_ESCAPE] && !prevArgsEsc) { argsFocused = false; }
                    prevArgsEsc = plat->input().keysDown[SCANCODE_ESCAPE];
                } else {
                    prevArgsBS = prevArgsEsc = prevArgsDel = false;
                    prevArgsLeft = prevArgsRight = false;
                    prevArgsHome = prevArgsEnd = false;
                    argsCursor = (int)argsBuf.size();
                }

                ly += 2;
                overlayFont->render("=== Torch GUI Dev ===", rightX, ly, {0.3f, 0.8f, 1, 1}, 1.5f); ly += 18;
                snprintf(buf, sizeof(buf), "FPS: %d  dt: %.1fms", frameCount, dt * 1000);
                overlayFont->render(buf, rightX, ly, {0.6f, 0.6f, 0.6f, 1}, 1.0f); ly += 16;
                snprintf(buf, sizeof(buf), "GUI dialogs: %zu", gui ? gui->dialogCount() : 0);
                overlayFont->render(buf, rightX, ly, {0.6f, 0.6f, 0.6f, 1}, 1.0f); ly += 16;
                overlayFont->render("F1: overlay  ~: console  Enter:launch", rightX, ly, {0.4f, 0.4f, 0.4f, 1}, 1.0f); ly += 16;
                ly += 4;
                // Object tree view — replaces old console log
                overlayFont->render("--- Object Tree ---", rightX, ly, {0.3f, 0.8f, 1, 1}, 1.0f); ly += 18;
                {
                    // Build parent→children map from all script objects
                    static std::unordered_map<std::string, std::vector<std::string>> treeChildren;
                    static std::vector<std::string> treeRoots;
                    static bool treeDirty = true;
                    if (treeDirty) {
                        treeChildren.clear();
                        treeRoots.clear();
                        // Gather all named objects
                        std::vector<std::string> allNames;
                        for (auto& kv : scr->objects)
                            if (!kv.second->name.empty()) allNames.push_back(kv.second->name);
                        // Also collect unnamed objects
                        for (auto& kv : scr->objects) {
                            // If the object has a parent, link it
                            auto it = kv.second->internals.find("parent");
                            if (it != kv.second->internals.end() && !it->second.toString().empty()) {
                                std::string parentName = it->second.toString();
                                if (scr->objects.count(parentName))
                                    treeChildren[parentName].push_back(kv.first);
                            } else {
                                // No parent, check if it's a SimGroup or root-level object
                                if (kv.second->className == "SimGroup" || kv.second->className.find("Sim") == 0)
                                    treeRoots.push_back(kv.first);
                            }
                        }
                        // Roots that weren't caught: any named object not a child of something else
                        for (auto& n : allNames) {
                            auto it = scr->objects.find(n);
                            if (it == scr->objects.end()) continue;
                            auto pit = it->second->internals.find("parent");
                            bool hasParent = (pit != it->second->internals.end() && !pit->second.toString().empty() && scr->objects.count(pit->second.toString()));
                            if (!hasParent && std::find(treeRoots.begin(), treeRoots.end(), n) == treeRoots.end())
                                treeRoots.push_back(n);
                        }
                        // Let unnamed GuiControls parented to a SimGroup show up via the parent's children field
                        treeDirty = false;
                    }

                    static std::unordered_set<std::string> expandedNodes;
                    static std::string selectedObject;
                    static int treeScroll = 0;
                    const int treeIndent = 14;

                    // Draw tree items in available space
                    int treeY = ly;
                    int maxTreeH = canvasBottom - treeY - 2;
                    int itemH = 14;
                    int maxItems = maxTreeH / itemH;

                    // Build flat display list: roots → expanded children recursively
                    std::vector<std::pair<std::string, int>> displayList;
                    std::function<void(const std::string&, int)> addNode = [&](const std::string& nodeName, int depth) {
                        if ((int)displayList.size() >= treeScroll + maxItems) return;
                        if (depth > 10) return; // safety
                        auto it = scr->objects.find(nodeName);
                        if (it == scr->objects.end()) return;
                        displayList.push_back({nodeName, depth});

                        bool isSimGroup = (it->second->className == "SimGroup");
                        bool hasChildren = (treeChildren.count(nodeName) && !treeChildren[nodeName].empty());
                        if (!hasChildren && isSimGroup) {
                            // Check if any unnamed objects have this as parent
                            for (auto& kv : scr->objects) {
                                if (kv.second->name.empty()) continue;
                                auto pit = kv.second->internals.find("parent");
                                if (pit != kv.second->internals.end() && pit->second.toString() == nodeName) {
                                    hasChildren = true;
                                    break;
                                }
                            }
                        }

                        if (expandedNodes.count(nodeName) && hasChildren) {
                            // Show children
                            if (treeChildren.count(nodeName)) {
                                for (auto& child : treeChildren[nodeName])
                                    addNode(child, depth + 1);
                            }
                            // Also show SimGroup children via parent field
                            for (auto& kv : scr->objects) {
                                if (kv.second->name.empty() || kv.first == nodeName) continue;
                                auto pit = kv.second->internals.find("parent");
                                if (pit != kv.second->internals.end() && pit->second.toString() == nodeName) {
                                    // Only add if not already in treeChildren
                                    bool already = false;
                                    if (treeChildren.count(nodeName))
                                        for (auto& c : treeChildren[nodeName])
                                            if (c == kv.first) { already = true; break; }
                                    if (!already) addNode(kv.first, depth + 1);
                                }
                            }
                        }
                    };
                    for (auto& root : treeRoots) addNode(root, 0);

                    // Handle mouse clicks
                    {
                        static bool prevTreeClick = false;
                        bool treeClick = plat->input().mouseButtons[1];
                        if (treeClick && !prevTreeClick) {
                            float mx = (float)plat->input().mouseX, my = (float)plat->input().mouseY;
                            int idx = (int)(my - treeY) / itemH;
                            if (mx >= rightX && idx >= 0 && idx + treeScroll < (int)displayList.size() && idx < maxItems) {
                                auto& entry = displayList[idx + treeScroll];
                                auto* obj = scr->objects[entry.first];
                                int ex = rightX + entry.second * treeIndent;
                                if (mx >= ex - 12 && mx < ex) {
                                    if (expandedNodes.count(entry.first))
                                        expandedNodes.erase(entry.first);
                                    else
                                        expandedNodes.insert(entry.first);
                                } else {
                                    selectedObject = entry.first;
                                }
                            }
                        }
                        prevTreeClick = treeClick;
                    }

                    // Scroll with mouse wheel
                    static int prevWheel = 0;
                    int wheel = plat->input().mouseWheel;
                    if (wheel != prevWheel) {
                        treeScroll -= (wheel - prevWheel);
                        if (treeScroll < 0) treeScroll = 0;
                        int maxScroll = std::max(0, (int)displayList.size() - maxItems);
                        if (treeScroll > maxScroll) treeScroll = maxScroll;
                    }
                    prevWheel = wheel;

                    // Render visible items
                    for (int i = treeScroll; i < (int)displayList.size() && i < treeScroll + maxItems; i++) {
                        auto& entry = displayList[i];
                        auto* obj = scr->objects[entry.first];
                        if (!obj) continue;
                        int yPos = treeY + (i - treeScroll) * itemH;
                        int xPos = rightX + entry.second * treeIndent;

                        // Expand [+] for objects with children
                        auto* childMap = obj->className == "SimGroup" ? &treeChildren : nullptr;
                        bool hasKids = (treeChildren.count(entry.first) && !treeChildren[entry.first].empty());
                        if (!hasKids && obj->className == "SimGroup") {
                            for (auto& kv : scr->objects) {
                                if (kv.second->name.empty()) continue;
                                auto pit = kv.second->internals.find("parent");
                                if (pit != kv.second->internals.end() && pit->second.toString() == entry.first) {
                                    hasKids = true; break;
                                }
                            }
                        }
                        if (hasKids) {
                            bool isExp = expandedNodes.count(entry.first);
                            overlayFont->render(isExp ? "[-]" : "[+]", xPos - 14, yPos, {0.5f, 1, 0.5f, 1}, 1.0f);
                        }

                        // Object name
                        ColorF objCol = (entry.first == selectedObject) ? ColorF{0.3f, 1, 0.8f, 1} : ColorF{0.8f, 0.8f, 0.8f, 1};
                        std::string label = obj->name.empty() ? "<unnamed>" : obj->name;
                        if (!obj->className.empty()) label += " [" + obj->className + "]";
                        overlayFont->render(label.c_str(), xPos, yPos, objCol, 1.0f);
                    }

                    // Store selected object name for inspector tab
                    {
                        static std::string prevSelected;
                        static std::vector<std::pair<std::string, std::string>> cachedFields;
                        if (selectedObject != prevSelected) {
                            prevSelected = selectedObject;
                            cachedFields.clear();
                            auto* obj = selectedObject.empty() ? nullptr : scr->objects[selectedObject];
                            if (obj) {
                                cachedFields.push_back({"className", obj->className});
                                cachedFields.push_back({"name", obj->name});
                                for (auto& f : obj->internals)
                                    cachedFields.push_back({f.first, f.second.toString()});
                                for (auto& f : obj->fields)
                                    cachedFields.push_back({f.first, f.second.toString()});
                            }
                        }
                        // Make cachedFields accessible to the inspector tab
                        // Use Console variable as a bridge
                        if (!selectedObject.empty())
                            Console::instance().setVariable("__selectedObj", selectedObject.c_str());
                        else
                            Console::instance().setVariable("__selectedObj", "");
                    }
                }
            }

            // GUI canvas at fixed 640x480 upper-left
            ren->setViewport(0, 0, w, h);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, h - 480, 640, 480);
            if (gui && gui->getCanvas()) gui->render();
            glDisable(GL_SCISSOR_TEST);

            // Border around GUI canvas
            {
                MatrixF borderOrtho;
                borderOrtho.identity();
                borderOrtho.m[0][0] = 2.0f / w;
                borderOrtho.m[1][1] = -2.0f / h;
                borderOrtho.m[3][0] = -1.0f;
                borderOrtho.m[3][1] = 1.0f;
                r.setProjection(borderOrtho);
                r.setView(MatrixF{});
                r.drawRectFill({0, 0, 0}, {640, 2, 0}, {0, 1, 0.5f, 1});
                r.drawRectFill({0, 478, 0}, {640, 480, 0}, {0, 1, 0.5f, 1});
                r.drawRectFill({0, 0, 0}, {2, 480, 0}, {0, 1, 0.5f, 1});
                r.drawRectFill({638, 0, 0}, {640, 480, 0}, {0, 1, 0.5f, 1});
            }

            // Horizontal separator across full width at canvas bottom
            r.drawRectFill({0, 479, 0}, {(float)w, 480, 0}, {0, 1, 0.5f, 0.6f});
            r.drawRectFill({0, 480, 0}, {(float)w, 481, 0}, {0.1f, 0.1f, 0.12f, 0.8f});

            // ─── Bottom tabbed panel ─────────────────────────────────────
            {
                const int tabBarY = 480 + 2;
                const int tabH = 22;
                const int panelH = h - tabBarY;
                if (panelH > tabH) {
                    // Tab bar
                    static int activeTab = 0;
                    const char* tabNames[] = {"Console", "Telnet", "Inspector", "Stack Trace"};
                    const int numTabs = 4;
                    int tabX = 4;
                    // Handle tab clicks
                    {
                        static bool prevTabClick = false;
                        bool tabClick = plat->input().mouseButtons[1];
                        if (tabClick && !prevTabClick) {
                            float mx = (float)plat->input().mouseX, my = (float)plat->input().mouseY;
                            int tx = tabX;
                            for (int t = 0; t < numTabs; t++) {
                                int tw = (int)strlen(tabNames[t]) * 9 + 12;
                                if (my >= tabBarY && my < tabBarY + tabH && mx >= tx && mx < tx + tw)
                                    activeTab = t;
                                tx += tw + 2;
                            }
                        }
                        prevTabClick = tabClick;
                    }
                    // Draw tab bar background
                    r.drawRectFill({0, (float)tabBarY, 0}, {(float)w, (float)(tabBarY + tabH + 4), 0}, {0.1f, 0.1f, 0.12f, 0.9f});
                    // Draw tabs
                    int tx = tabX;
                    for (int t = 0; t < numTabs; t++) {
                        int tw = (int)strlen(tabNames[t]) * 9 + 12;
                        ColorF tabCol = (t == activeTab) ? ColorF{0.3f, 0.7f, 1, 0.9f} : ColorF{0.2f, 0.2f, 0.25f, 0.8f};
                        r.drawRectFill({(float)tx, (float)tabBarY, 0}, {(float)(tx + tw), (float)(tabBarY + tabH), 0}, tabCol);
                        if (overlayFont)
                            overlayFont->render(tabNames[t], (float)(tx + 6), (float)(tabBarY + 4), {1,1,1,0.9f}, 1.0f);
                        tx += tw + 2;
                    }
                    // Tab content area
                    int contentY = tabBarY + tabH + 4;
                    int contentH = h - contentY - 2;
                    if (contentH > 10) {
                        r.drawRectFill({2, (float)contentY, 0}, {(float)(w - 2), (float)(contentY + contentH), 0}, {0.08f, 0.08f, 0.1f, 0.85f});
                        if (overlayFont) {
                            if (activeTab == 0) {
                                // Console tab: show full log
                                auto& log = Console::instance().getLog();
                                int start = std::max(0, (int)log.size() - contentH / 12);
                                int lY = contentY + 2;
                                for (int i = start; i < (int)log.size() && lY + 12 <= contentY + contentH; i++, lY += 12) {
                                    ColorF col{0.5f, 0.5f, 0.5f, 0.9f};
                                    const std::string& line = log[i];
                                    if (line.find("[ERROR]") == 0) col = {1, 0.3f, 0.3f, 0.9f};
                                    else if (line.find("[WARN]") == 0) col = {1, 0.8f, 0.3f, 0.9f};
                                    else if (line.find("[INFO]") == 0) col = {0.5f, 0.8f, 1, 0.9f};
                                    overlayFont->render(line.c_str(), 6, lY, col, 1.0f);
                                }
                            } else if (activeTab == 1) {
                                overlayFont->render("Telnet console - not connected", 6, contentY + 2, {0.5f,0.5f,0.5f,0.8f}, 1.0f);
                            } else if (activeTab == 2) {
                                // Inspector: show selected object's properties
                                std::string sel = Console::instance().getStringVariable("__selectedObj", "");
                                if (sel.empty()) {
                                    overlayFont->render("Click an object in the tree to inspect it", 6, contentY + 2, {0.5f,0.5f,0.5f,0.8f}, 1.0f);
                                } else {
                                    auto it = scr->objects.find(sel);
                                    if (it == scr->objects.end()) {
                                        std::string msg = "Object not found: " + sel;
                                        overlayFont->render(msg.c_str(), 6, contentY + 2, {1,0.3f,0.3f,0.9f}, 1.0f);
                                    } else {
                                        auto* obj = it->second;
                                        int iY = contentY + 2;
                                        auto drawLine = [&](const std::string& key, const std::string& val) {
                                            if (iY + 12 > contentY + contentH) return;
                                            std::string line = key + ": " + val;
                                            overlayFont->render(line.c_str(), 6, iY, {0.7f,0.7f,0.7f,0.9f}, 1.0f);
                                            iY += 12;
                                        };
                                        drawLine("className", obj->className);
                                        drawLine("name", obj->name);
                                        for (auto& f : obj->internals)
                                            drawLine(f.first, f.second.toString());
                                        for (auto& f : obj->fields)
                                            drawLine(f.first, f.second.toString());
                                    }
                                }
                            } else if (activeTab == 3) {
                                overlayFont->render("Stack trace - not implemented", 6, contentY + 2, {0.5f,0.5f,0.5f,0.8f}, 1.0f);
                            }
                        }
                    }
                }
            }

            // Preview image in bottom-right corner
            if (!previewImgPath.empty()) {
                static uint32_t previewTex = 0;
                static int pW = 0, pH = 0;
                if (!previewTex) {
                    auto* tex = ren->loadTexture(previewImgPath.c_str());
                    if (tex) { previewTex = tex->id; pW = tex->width; pH = tex->height; }
                }
                if (previewTex) {
                    int maxW = w / 4, maxH = h / 4;
                    if (maxW < 100) maxW = 100;
                    if (maxH < 100) maxH = 100;
                    int dW = pW, dH = pH;
                    if (dW > maxW) { dH = dH * maxW / dW; dW = maxW; }
                    if (dH > maxH) { dW = dW * maxH / dH; dH = maxH; }
                    int rx = w - dW - 6, ry = h - dH - 6;
                    r.drawRectFill({(float)(rx - 2), (float)(ry - 2)}, {(float)(rx + dW + 2), (float)(ry + dH + 2)}, {0.2f, 0.2f, 0.2f, 0.8f});
                    r.drawTexturedRectUV({(float)rx, (float)ry}, {(float)(rx + dW), (float)(ry + dH)}, previewTex, 0, 0, 1, 1);
                }
            }

            if (menuState) r.endFrame();
        }

        // TORCH overlay + minimap
        ren->setViewport(0, 0, plat->width(), plat->height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        if (showOverlay) renderOverlay();
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
    float sy = 24.0f;

    int line = 0;
    float lx = 16, ly = 16;
    ColorF white = {1,1,1,1};
    ColorF dim = {0.6f, 0.6f, 0.6f, 1};
    ColorF cyan = {0.3f, 0.8f, 1, 1};

    overlayFont->render("=== TORCH Debug Menu ===", lx, ly + (line++) * sy, cyan, 2.0f);
    line++;

    std::string emuStr = "Emulation Version: 25034  [1/2 to switch]";
    overlayFont->render(emuStr.c_str(), lx, ly + (line++) * sy, white, 2.0f);
    line++;

    if (g->getDemoParser()) {
        uint32_t pv = g->getDemoParser()->getHeader().protocolVersion;
        char pvStr[64];
        snprintf(pvStr, sizeof(pvStr), "Detected Protocol: 0x%08X", (unsigned)pv);
        overlayFont->render(pvStr, lx, ly + (line++) * sy, dim, 2.0f);

        if (pv == T2Demo::ProtocolV24834)
            overlayFont->render("  (Tribes 2 v24834 - experimental)", lx, ly + (line++) * sy, dim, 2.0f);
        else if (pv == T2Demo::ProtocolV25034)
            overlayFont->render("  (Tribes 2 v25034 - supported)", lx, ly + (line++) * sy, dim, 2.0f);
        else
            overlayFont->render("  (unknown version)", lx, ly + (line++) * sy, dim, 2.0f);
        line++;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "State: %d  Demo: %s  Blocks: %d/%d",
        (int)g->state(),
        g->isDemoPlaying() ? "PLAYING" : "idle",
        g->getDemoBlocksDone(), g->getDemoBlocksTotal());
    overlayFont->render(buf, lx, ly + (line++) * sy, dim, 2.0f);

    // Current mission (tracks map changes across the demo)
    if (g->getDemoParser()) {
        const std::string& mis = g->getDemoParser()->currentMission();
        if (!mis.empty()) {
            snprintf(buf, sizeof(buf), "Mission: %s", mis.c_str());
            overlayFont->render(buf, lx, ly + (line++) * sy, white, 2.0f);
        }
    }

    line++;
    overlayFont->render("ESC to quit | Pause to close", lx, ly + (line++) * sy, dim, 2.0f);
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
