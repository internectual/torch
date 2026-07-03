#pragma once

#include "core/platform.h"
#include "core/console.h"
#include "core/math.h"
#include "core/timer.h"
#include "core/string_table.h"

#include "render/renderer.h"
#include "render/gui_renderer.h"
#include "audio/audio_system.h"
#include "script/script_engine.h"
#include "fs/file_system.h"
#include "net/network.h"
#include "game/game.h"

class Engine {
public:
    Engine();
    ~Engine();

    bool init(int argc, char* argv[]);
    void run();
    void shutdown();
    bool isRunning() const;

    static Engine& instance();

    Platform& platform() { return *plat; }
    Console& console() { return *con; }
    Renderer& renderer() { return *ren; }
    AudioSystem& audio() { return *aud; }
    FileSystem& fs() { return *filesys; }
    ScriptEngine& script() { return *scr; }
    NetworkManager& network() { return *net; }
    Game& game() { return *g; }
    GuiRenderer& guiRenderer() { return *gui; }
    Timer& timer() { return *tim; }

    void quit() { running = false; }

    // Overlay (TORCH debug menu, triggered by Pause)
    void toggleOverlay() { showOverlay = !showOverlay; }
    bool overlayActive() const { return showOverlay; }

    // Console (now handled by GUI dialog ConsoleDlg)
    bool consoleActive() const { return gui && gui->isDialogActive("ConsoleDlg"); }
    Font* guiFont() const { return overlayFont; }

    Point3F getPreviewCamPos() const { return previewCamPos; }
    Point3F getPreviewCamTarget() const { return previewCamTarget; }
    bool hasPreviewCam() const { return usePreviewCam; }

    std::string testShapePath;
    std::string testDifPath;

private:
    struct Impl;
    Impl* impl;

    Platform* plat{};
    Console* con{};
    Renderer* ren{};
    AudioSystem* aud{};
    FileSystem* filesys{};
    ScriptEngine* scr{};
    NetworkManager* net{};
    Game* g{};
    Timer* tim{};
    GuiRenderer* gui{};

    bool running = false;
    bool previewDone = false;
    std::string previewMap;
    std::string demoPath;
    bool demoMode = false;
    Point3F previewCamPos{0, 200, -400};
    Point3F previewCamTarget{0, 0, 0};
    bool usePreviewCam = false;
    bool showOverlay = false;
    bool showMinimap = true;
    int lockFd = -1;

    void renderOverlay();
    void renderMinimap();
    Font* overlayFont = nullptr;
    bool overlayFontOwned = false;
};
