#include "game/hud.h"
#include "game/game.h"
#include "game/movement.h"
#include "render/renderer.h"
#include "core/engine.h"
#include "core/console.h"
#include "game/hud_parity.h"
#include "game/observer_parity.h"
#include "game/menu_parity.h"
#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cctype>

namespace {
struct RemapAction { const char* name; const char* label; };
constexpr RemapAction kRemapActions[] = {
    {"forward", "Forward"}, {"backward", "Backward"},
    {"left", "Strafe Left"}, {"right", "Strafe Right"},
    {"jump", "Jump"}, {"jet", "Jet"}, {"fire", "Fire"},
    {"altfire", "Alt Fire"}, {"reload", "Reload"},
    {"f1", "Free Camera"},
    {"f2", "Orbit Camera"}, {"chat", "Chat"}, {"console", "Console"},
};
constexpr int kRemapActionCount = sizeof(kRemapActions) / sizeof(kRemapActions[0]);
}

struct HUD::Impl {
    std::string targetSearch;
    int targetSelection = 0;
};

HUD::HUD() : impl(new Impl) {}
HUD::~HUD() { delete impl; }

void HUD::init() {}

void HUD::resetState() {
    impl->targetSearch.clear();
    impl->targetSelection = 0;
}

void HUD::render(Game* game) {
    const bool dead = game && game->state() == Game::Dead;
    if (!visible || !game || (game->state() != Game::Playing && !dead)) return;

    auto& input = Engine::instance().platform().input();
    if (game->targetFinderOpen()) {
        impl->targetSearch += input.textInput;
        for (int scancode : input.keyPressQueue) {
            if (scancode == SCANCODE_BACKSPACE && !impl->targetSearch.empty())
                impl->targetSearch.pop_back();
            else if (scancode == SCANCODE_ESCAPE)
                game->closeTargetFinder();
        }
    }

    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    if (!font) return;

    int32_t w = Engine::instance().platform().width();
    int32_t h = Engine::instance().platform().height();

    // Push 2D ortho projection for HUD
    MatrixF ortho;
    ortho.identity();
    ortho.m[0][0] = 2.0f / w;
    ortho.m[1][1] = -2.0f / h;
    ortho.m[0][3] = -1.0f;
    ortho.m[1][3] = 1.0f;
    r.setProjection(ortho);
    MatrixF id; id.identity();
    r.setView(id);
    glDisable(GL_DEPTH_TEST);

    // GameRenderFilters applies these effects for both network and demo
    // snapshots. They are intentionally drawn before the HUD controls.
    const float damageFlash = game->getDamageFlash();
    const float whiteOut = game->getWhiteOut();
    const float blackOut = game->getBlackOut();
    if (damageFlash > 0.0f)
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {1, 0, 0, std::min(damageFlash, 0.76f)});
    if (whiteOut > 0.0f)
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {1.0f, 1.0f, 0.92f, std::min(whiteOut, 1.0f)});
    if (blackOut > 0.0f)
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {0, 0, 0, std::min(blackOut, 1.0f)});

    // GameRenderFilters applies the water filter before the GUI. Lava and
    // quicksand deliberately do not use it.
    if (game->isUnderwater()) {
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {0.2f, 0.6f, 0.6f, 0.3f});
    }

    // Everything the player sees beyond the render filters is the stock GUI
    // (PlayGui, its HUD controls and the scripts that drive them). Torch's
    // own playback tools draw only with the F1 developer overlay.
    char buf[128];
    // Demo playback info (a live connection is not a playback).
    if (Engine::instance().overlayActive() && game->isDemoPlaying() && !game->isLiveClient()) {
        const char* status = "> PLAY";
        ColorF statusColor = {0, 1, 0, 1};
        if (game->demoStepFeedback()) {
            status = ">| STEP";
            statusColor = {0.3f, 0.8f, 1.0f, 1};
        } else if (game->isDemoPaused()) {
            status = "|| PAUSE";
            statusColor = {1, 1, 0, 1};
        } else if (game->isDemoFastForward()) {
            status = ">> FAST";
        }
        // Speed display
        float speed = game->getDemoSpeed();
        char speedBuf[16];
        snprintf(speedBuf, sizeof(speedBuf), "%.1fx", speed);

        const std::string time = T2Demo::formatPlaybackClock(game->getDemoTime());
        const std::string total = T2Demo::formatPlaybackClock(game->getDemoTotalTime());
        snprintf(buf, sizeof(buf), "[%s] %s %s/%s",
                 status, speedBuf, time.c_str(), total.c_str());
        if (font) font->render(buf, 20.0f, 20.0f, statusColor, 2.0f);

        // Graphical progress bar
        const float progress = T2Demo::playbackProgress(
            game->getDemoTime(), game->getDemoTotalTime());
        float barW = 300.0f, barH = 8.0f;
        float bx = 20.0f, by = 42.0f;
        // Background
        Box3F bgBox = {{bx, by - barH, 0}, {bx + barW, by, 0}};
        r.drawBox(bgBox, {0.2f, 0.2f, 0.2f, 0.7f});
        // Fill
        float fw = barW * progress;
        if (fw > 2.0f) {
            Box3F fillBox = {{bx + 1, by - barH + 1, 0}, {bx + fw - 1, by - 1, 0}};
            r.drawBox(fillBox, {0.3f, 1.0f, 0.3f, 0.9f});
        }
        // Border
        r.drawLine({bx, by, 0}, {bx + barW, by, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx + barW, by, 0}, {bx + barW, by - barH, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx + barW, by - barH, 0}, {bx, by - barH, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx, by - barH, 0}, {bx, by, 0}, {1, 1, 1, 0.5f});

        // Ghost count and help text
        if (auto* dp = game->getDemoParser()) {
            int ghostCount = dp->getGhostTracker().size();
            const char* cameraMode = game->demoOrbitCamActive() ? "ORBIT" :
                game->isFreeCamActive() ? "FREE" :
                game->demoFirstPersonCamActive() ? "FIRST" : "REC";
            if (game->demoOrbitCamActive()) {
                snprintf(buf, sizeof(buf), "Ghosts: %d  CAM:%s  [F2]orbit [F4]first [A/D]rot [W/S]zoom [Spc/Shft]ht [R]target",
                         ghostCount, cameraMode);
            } else if (game->isFreeCamActive()) {
                snprintf(buf, sizeof(buf), "Ghosts: %d  CAM:%s  [F1]record [F2]orbit [F4]first [WASD]move [R]target",
                         ghostCount, cameraMode);
            } else {
                snprintf(buf, sizeof(buf), "Ghosts: %d  CAM:%s  [P]ause [.]step [F1]free [F2]orbit [F4]first [E]vents [Tab]score [R]target",
                         ghostCount, cameraMode);
            }
            if (font) font->render(buf, 20.0f, 48.0f, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            // Spectate target indicator
            int sidx = game->getControlGhostIndex();
            if (sidx >= 0) {
                const GhostTracker& gt = game->getDemoParser()->getGhostTracker();
                const GhostEntry* g = gt.getGhost(sidx);
                if (g && !g->playerName.empty()) {
                    if (font) font->render(g->playerName.c_str(), 20.0f, 68.0f, {0.3f, 1.0f, 0.5f, 0.9f}, 2.0f);
                }
            }
        }

        // Event log pane
        // Restore ortho projection for 2D HUD elements
        r.setProjection(ortho);
        MatrixF id; id.identity();
        r.setView(id);
        if (game->demoEventsShown() && font) {
            const auto& events = game->getDemoEventLog();
            int total = (int)events.size();
            int start = std::max(0, total - 20); // show last 20
            float ey = 70.0f;
            // Background
            r.drawBox({{15, ey - 5, 0}, {450, ey + 21 * 15 + 5, 0}}, {0, 0, 0, 0.6f});
            for (int i = start; i < total; i++) {
                const auto& e = events[i];
                int secs = (int)e.time % 60;
                int mins = (int)(e.time / 60) % 60;
                char line[256];
                ColorF col{0.8f, 0.8f, 0.8f, 0.9f};
                if (e.type == 0) { col = {0.3f, 1.0f, 0.3f, 0.9f}; } // chat green
                else if (e.type == 1) { col = {1.0f, 1.0f, 0.3f, 0.9f}; } // server yellow
                if (e.text.size() > 60) {
                    snprintf(line, sizeof(line), "[%02d:%02d] %.60s...", mins, secs, e.text.c_str());
                } else {
                    snprintf(line, sizeof(line), "[%02d:%02d] %s", mins, secs, e.text.c_str());
                }
                font->render(line, 20.0f, ey, col, 2.0f);
                ey += 22.0f;
            }
        }
    }

    if (game->targetFinderOpen() && font) {
        struct Entry { int ghost; std::string label; bool selectable; };
        std::vector<Entry> entries;
        auto matches = [this](std::string label) {
            if (impl->targetSearch.empty()) return true;
            std::string needle = impl->targetSearch;
            for (char& c : label) c = (char)std::tolower((unsigned char)c);
            for (char& c : needle) c = (char)std::tolower((unsigned char)c);
            return label.find(needle) != std::string::npos;
        };
        auto addGhosts = [&](const std::vector<int>& indices, auto getter) {
            for (int index : indices) {
                const GhostEntry* ghost = getter(index);
                // Stock observer target search includes live vehicles as well
                // as players; keep it aligned with observer cycling.
                 if (!ghost || !ObserverParity::isReadySpectatableTarget(
                     ghost->className, ghost->damageState, ghost->hasPosition)) continue;
                if (game->isConnected() && game->activeConnection()) {
                    const auto observer = game->activeConnection()->observerSnapshot();
                    if (!game->isSensorGroupTargetVisible(observer.playerSensorGroup,
                                                          ghost->sensorGroup)) continue;
                }
                const std::string name = ghost->playerName.empty()
                    ? "Ghost " + std::to_string(index) : ghost->playerName;
                if (matches(name)) entries.push_back({index, name, true});
            }
        };
        if (game->isDemoPlaying() && game->getDemoParser()) {
            const auto& tracker = game->getDemoParser()->getGhostTracker();
            addGhosts(tracker.getAllIndices(), [&](int index) { return tracker.getGhost(index); });
        } else if (game->isConnected()) {
            addGhosts(game->getLiveGhostIndices(), [&](int index) { return game->getLiveGhost(index); });
            for (const auto& [teamId, team] : game->getLiveTeamScores()) {
                std::string flag = "Flag " + (team.name.empty() ? std::to_string(teamId) : team.name);
                flag += " [" + team.flagStatus + "]";
                if (matches(flag)) entries.push_back({-1, flag, false});
            }
        }
        const int count = (int)entries.size();
        if (count > 0) {
            impl->targetSelection = std::clamp(impl->targetSelection, 0, count - 1);
            for (int scancode : input.keyPressQueue) {
                if (scancode == SCANCODE_UP) impl->targetSelection = (impl->targetSelection + count - 1) % count;
                if (scancode == SCANCODE_DOWN) impl->targetSelection = (impl->targetSelection + 1) % count;
                if (scancode == SCANCODE_RETURN && entries[impl->targetSelection].selectable)
                    game->selectSpectateTarget(entries[impl->targetSelection].ghost);
            }
        } else {
            impl->targetSelection = 0;
        }
        const float x = (float)w * 0.5f - 260.0f, y = 90.0f;
        r.drawBox({{x, y, 0}, {x + 520.0f, y + 500.0f, 0}}, {0.02f, 0.03f, 0.06f, 0.96f});
        font->render("TARGET FINDER", x + 24, y + 24, {1, 1, 0.3f, 1}, 2.0f);
        char search[256];
        snprintf(search, sizeof(search), "Search: %s", impl->targetSearch.c_str());
        font->render(search, x + 24, y + 58, {0.7f, 0.9f, 1, 1}, 1.5f);
        int row = 0;
        constexpr int visibleRows = 13;
        const int windowStart = HudParity::targetFinderWindowStart(
            impl->targetSelection, count, visibleRows);
        for (int entryIndex = windowStart;
             entryIndex < count && row < visibleRows; ++entryIndex, ++row) {
            const auto& entry = entries[entryIndex];
            const float ry = y + 94.0f + row * 28.0f;
            if (entryIndex == impl->targetSelection)
                r.drawRectFill({x + 12, ry - 3, 0}, {x + 508, ry + 23, 0}, {1, 1, 0, 0.18f});
            const std::string label = (entry.selectable ? "> " : "  ") + entry.label;
            font->render(label.c_str(), x + 24, ry,
                         entryIndex == impl->targetSelection ? ColorF{1, 1, 0, 1} : ColorF{0.85f, 0.85f, 0.85f, 1}, 1.5f);
        }
        if (entries.empty()) font->render("No matching players or flags", x + 24, y + 100, {0.6f, 0.6f, 0.6f, 1}, 1.5f);
        font->render("UP/DOWN select  ENTER follow  ESC/F3 close", x + 24, y + 466, {0.6f, 0.7f, 0.75f, 1}, 1.2f);
    }

}

void Menu::init() {}

void Menu::update(float dt) {
    (void)dt;
    // Menu input is edge-triggered. Clear the previous-frame state while the
    // menu is hidden so reopening it with a held key cannot swallow the first
    // activation edge from the new menu session.
    static bool prevUp = false, prevDown = false, prevEnter = false, prevEsc = false;
    static bool prevMouseBtn = false, prevBrowserMouseBtn = false;
    if (!active) {
        prevUp = prevDown = prevEnter = prevEsc = false;
        prevMouseBtn = prevBrowserMouseBtn = false;
        return;
    }

    auto& input = Engine::instance().platform().input();
    auto& ren = Engine::instance().renderer();

    bool up = input.keysDown[SCANCODE_UP];
    bool down = input.keysDown[SCANCODE_DOWN];
    bool enter = input.keysDown[SCANCODE_RETURN] &&
                 !input.consumedSc[SCANCODE_RETURN];
    bool esc = input.keysDown[SCANCODE_ESCAPE];

    // Mouse hover: update selectedItem based on mouse Y position
    int mx = input.mouseX;
    int my = input.mouseY;
    if (currentScreen == Main) {
        prevBrowserMouseBtn = false;
        const int itemCount = mainMenuItemCount();
        float startY = 200.0f;
        float itemH = 40.0f;
        float leftX = (float)ren.config().width * 0.5f - 100.0f;
        float rightX = (float)ren.config().width * 0.5f + 100.0f;
        for (int i = 0; i < itemCount; i++) {
            float iy = startY + i * itemH;
            if (my >= iy && my < iy + itemH && mx >= leftX && mx <= rightX) {
                selectedItem = i;
            }
        }
        // Mouse click activates item
        // SDL reports the left button as index 1 (SDL_BUTTON_LEFT), matching the
        // engine's GUI click path (engine.cpp uses mouseButtons[1]).
        bool mouseBtn = input.mouseButtons[1] != 0;
        const bool mousePressed = menuButtonPressed(mouseBtn, prevMouseBtn);
        if (mousePressed && !input.consumedMouse[1]) {
            for (int i = 0; i < itemCount; i++) {
                float iy = startY + i * itemH;
                if (my >= iy && my < iy + itemH && mx >= leftX && mx <= rightX) {
                    selectedItem = i;
                    enter = true;
                }
            }
        }
        if (up && !prevUp) { selectedItem = (selectedItem - 1 + itemCount) % itemCount; }
        if (down && !prevDown) { selectedItem = (selectedItem + 1) % itemCount; }

        if (enter && !prevEnter) {
            switch (selectedItem) {
                case 0:
                    Engine::instance().game().startLocalGame();
                    setActive(false);
                    break;
                case 1: // Server Browser
                    currentScreen = ServerBrowser;
                    selectedItem = 0;
                    break;
                case 2: // Settings
                    currentScreen = Settings;
                    selectedItem = 0;
                    break;
                case 3: // Controls
                    currentScreen = Controls;
                    selectedItem = 0;
                    break;
                case 4: // Credits
                    currentScreen = Credits;
                    selectedItem = 0;
                    break;
                case 5:
                    Engine::instance().quit();
                    break;
                default: break;
            }
        }
    } else if (currentScreen == ServerBrowser) {
        prevMouseBtn = false;
        if (esc && !prevEsc) { currentScreen = Main; selectedItem = 0; }
        // Refresh list
        static double lastRefresh = 0;
        if (Engine::instance().timer().now() - lastRefresh > 2.0) {
            lastRefresh = Engine::instance().timer().now();
            const char* demoMaster = Console::instance().getStringVariable(
                "demoMasterServer", "");
            if (serverBrowserUsesMaster(Engine::instance().demoMode, demoMaster))
                Engine::instance().network().queryMasterServer(demoMaster);
            else
                Engine::instance().network().queryLanServers();
            // Convert network servers to menu entries
            servers.clear();
            for (auto& s : Engine::instance().network().getServerList()) {
                ServerEntry e;
                e.name = s.name.empty() ? "Unnamed Server" : s.name;
                e.map = s.map;
                e.gameType = s.gameType;
                e.players = s.numPlayers;
                e.maxPlayers = s.maxPlayers;
                e.ping = s.ping;
                servers.push_back(e);
            }
        }
        const int browserRow = serverBrowserRowAt(
            (float)mx, (float)my, 20.0f, (float)ren.config().width - 20.0f,
            80.0f, 30.0f, (int)servers.size());
        const bool browserMouseBtn = input.mouseButtons[1] != 0;
        const bool browserMousePressed = menuButtonPressed(browserMouseBtn, prevBrowserMouseBtn);
        if (browserRow >= 0) {
            selServer = browserRow;
            if (browserMousePressed &&
                !input.consumedMouse[1])
                enter = true;
        }
        // Navigate list
        int count = (int)servers.size();
        if (count > 0) {
            selServer = std::clamp(selServer, 0, count - 1);
            if (up && !prevUp) selServer = (selServer - 1 + count) % count;
            if (down && !prevDown) selServer = (selServer + 1) % count;
            if (enter && !prevEnter) {
                // Connect to selected server
                auto netServers = Engine::instance().network().getServerList();
                if (selServer >= 0 && selServer < (int)netServers.size()) {
                    auto& addr = netServers[selServer].addr;
                    std::string addrStr = addr.toString();
                    // Format is "ip:port" or "host:port"
                    auto colon = addrStr.find(':');
                    if (colon != std::string::npos) {
                        std::string host = addrStr.substr(0, colon);
                        uint16_t port = (uint16_t)atoi(addrStr.c_str() + colon + 1);
                        Console::instance().printf(LogLevel::Info, "Connecting to %s:%d", host.c_str(), port);
                        Engine::instance().game().connectToServer(host.c_str(), port);
                        setActive(false);
                    }
                }
            }
        }
    } else if (currentScreen == Settings) {
        prevMouseBtn = prevBrowserMouseBtn = false;
        if (esc && !prevEsc) { currentScreen = Main; selectedItem = 2; }
    } else if (currentScreen == Controls) {
        prevMouseBtn = prevBrowserMouseBtn = false;
        if (remapActive) {
            if (esc && !prevEsc) {
                remapActive = false;
                remapAction = -1;
            } else {
                int key = -1;
                for (int sc = 0; sc < 512; ++sc) {
                    if (input.keysDown[sc] && !remapPrevKeys[sc]) { key = sc; break; }
                }
                if (key >= 0 && key != SCANCODE_ESCAPE) {
                    Engine::instance().setBind(kRemapActions[remapAction].name, key);
                    remapActive = false;
                    remapAction = -1;
                } else {
                    const bool mouseDown = input.mouseButtons[1] || input.mouseButtons[2] || input.mouseButtons[3];
                    if (mouseDown && !remapPrevMouse) {
                        int button = input.mouseButtons[1] ? -1 : input.mouseButtons[2] ? -2 : -3;
                        Engine::instance().setBind(kRemapActions[remapAction].name, button);
                        remapActive = false;
                        remapAction = -1;
                    }
                }
            }
            for (int sc = 0; sc < 512; ++sc) remapPrevKeys[sc] = input.keysDown[sc];
            remapPrevMouse = input.mouseButtons[1] || input.mouseButtons[2] || input.mouseButtons[3];
        } else {
            if (esc && !prevEsc) { currentScreen = Main; selectedItem = 3; }
            if (up && !prevUp) selectedItem = (selectedItem - 1 + kRemapActionCount) % kRemapActionCount;
            if (down && !prevDown) selectedItem = (selectedItem + 1) % kRemapActionCount;
            const bool mouseDown = input.mouseButtons[1] != 0;
            if (mouseDown && !remapPrevMouse) {
                const int row = (input.mouseY - 80) / 30;
                if (input.mouseX >= 20 && input.mouseX <= 520 &&
                    row >= 0 && row < kRemapActionCount) {
                    selectedItem = row;
                    remapAction = row;
                    remapActive = true;
                    remapPrevMouse = true;
                    for (int sc = 0; sc < 512; ++sc) remapPrevKeys[sc] = input.keysDown[sc];
                }
            }
            if (enter && !prevEnter) {
                remapAction = std::clamp(selectedItem, 0, kRemapActionCount - 1);
                remapActive = true;
                for (int sc = 0; sc < 512; ++sc) remapPrevKeys[sc] = input.keysDown[sc];
            }
            remapPrevMouse = mouseDown;
        }
    } else if (currentScreen == Credits) {
        prevMouseBtn = prevBrowserMouseBtn = false;
        if (esc && !prevEsc) {
            currentScreen = Main;
            selectedItem = 4;
        }
    }

    prevUp = up; prevDown = down; prevEnter = enter; prevEsc = esc;
}

void Menu::render() {
    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    float w = (float)Engine::instance().platform().width();
    float h = (float)Engine::instance().platform().height();

    // 2D pixel-space projection (top-left origin, y-down) matching Font::render,
    // so highlight rects and text align with mouse coordinates.
    MatrixF ortho; ortho.identity();
    ortho.m[0][0] = 2.0f / w;
    ortho.m[1][1] = -2.0f / h;
    ortho.m[0][3] = -1.0f;
    ortho.m[1][3] = 1.0f;
    r.setProjection(ortho);
    r.setView(MatrixF{});
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    // Simple menu rendering
    if (!active) return;

    switch (currentScreen) {
        case Main: {
            const char* title = "TRIBES 2";
            if (font) font->render(title, w * 0.5f - 80, 100, {1, 1, 0, 1}, 2.0f);

            const char* items[] = {"Start Local Game", "Server Browser", "Settings", "Controls", "Credits", "Quit"};
            // Hover/selection highlight box behind the active item
            {
                float hx = w * 0.5f - 100.0f;
                float hy = 200.0f + (float)selectedItem * 40.0f;
                r.drawRectFill({hx, hy, 0.0f}, {hx + 200.0f, hy + 40.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.18f});
            }
            for (int i = 0; i < mainMenuItemCount(); i++) {
                float ix = w * 0.5f - 80;
                float iy = 200 + i * 40;
                ColorF col = (i == selectedItem) ? ColorF{1, 1, 0, 1} : ColorF{1, 1, 1, 1};
                if (font) font->render(items[i], ix, iy, col, 2.0f);
                if (i == selectedItem) {
                    // Arrow indicator
                    if (font) font->render(">", ix - 20, iy, {1, 1, 0, 1}, 2.0f);
                }
            }
            break;
        }
        case ServerBrowser: {
            if (font) font->render("Server Browser", 20, 20, {1, 1, 0, 1}, 2.0f);
            if (font) font->render("[ESC] Back", 20, 700, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            if (servers.empty() && font) {
                font->render("No servers found.", 20, 80, {0.5f, 0.5f, 0.5f, 1}, 2.0f);
            } else {
                for (size_t i = 0; i < servers.size(); i++) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "%s | %s | %d/%d | %dms",
                        servers[i].name.c_str(), servers[i].map.c_str(),
                        servers[i].players, servers[i].maxPlayers, servers[i].ping);
                    const float rowY = 80.0f + (float)i * 30.0f;
                    if ((int)i == selServer)
                        r.drawRectFill({20.0f, rowY, 0.0f},
                                       {w - 20.0f,
                                        rowY + 30.0f, 0.0f},
                                       {1.0f, 1.0f, 0.0f, 0.18f});
                    if (font) font->render(buf, 20, rowY, {1, 1, 1, 1}, 2.0f);
                }
            }
            break;
        }
        case Settings: {
            if (font) font->render("Settings", 20, 20, {1, 1, 0, 1}, 2.0f);
            if (font) font->render("[ESC] Back", 20, 700, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            int sy = 80;
            // Resolution
            auto& r2 = Engine::instance().renderer();
            if (font) {
                char res[64];
                snprintf(res, sizeof(res), "Resolution: %dx%d", r2.config().width, r2.config().height);
                font->render(res, 20, sy, {1, 1, 1, 1}, 2.0f);
            }
            sy += 30;
            // Volume
            auto& audio = Engine::instance().audio();
            if (font) {
                char vol[64];
                snprintf(vol, sizeof(vol), "Master Volume: %d%%", (int)(audio.config().masterVolume * 100));
                font->render(vol, 20, sy, {1, 1, 1, 1}, 2.0f);
            }
            sy += 30;
            if (font) font->render("SFX Volume: see master", 20, sy, {0.6f, 0.6f, 0.6f, 1}, 2.0f);
            sy += 30;
            if (font) font->render("(Settings are read-only in this build)", 20, sy, {0.5f, 0.5f, 0.5f, 1}, 2.0f);
            break;
        }
        case Controls: {
            if (font) font->render("Controls", 20, 20, {1, 1, 0, 1}, 2.0f);
            if (font) font->render("[ESC] Back", 20, 700, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            char bindText[64];
            int sy = 80;
            for (int i = 0; i < kRemapActionCount; ++i) {
                const bool selected = i == selectedItem;
                if (selected)
                    r.drawRectFill({20, (float)sy - 3, 0}, {520, (float)sy + 25, 0},
                                   {1, 1, 0, 0.16f});
                if (font) {
                    font->render(kRemapActions[i].label, 40, sy,
                                 selected ? ColorF{1, 1, 0, 1} : ColorF{1, 1, 1, 1}, 1.5f);
                    snprintf(bindText, sizeof(bindText), "%s",
                             Engine::instance().scancodeName(
                                 Engine::instance().getBind(kRemapActions[i].name)));
                    font->render(bindText, 300, sy, {0.8f, 0.8f, 0.8f, 1}, 1.5f);
                }
                sy += 30;
            }
            if (remapActive) {
                r.drawRectFill({120, 260, 0}, {620, 360, 0}, {0, 0, 0, 0.92f});
                if (font) {
                    snprintf(bindText, sizeof(bindText), "Press a key for %s",
                             kRemapActions[remapAction].label);
                    font->render(bindText, 160, 290, {1, 1, 0, 1}, 1.5f);
                    font->render("ESC cancels", 270, 325, {0.8f, 0.8f, 0.8f, 1}, 1.2f);
                }
            }
            break;
        }
        case Credits: {
            if (font) font->render("Credits", 20, 20, {1, 1, 0, 1}, 2.0f);
            if (font) font->render("[ESC] Back", 20, 700,
                                  {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            if (font) {
                font->render("Torch", 20, 100, {1, 1, 1, 1}, 2.0f);
                font->render("A Tribes 2 engine reimplementation", 20, 135,
                             {0.8f, 0.8f, 0.8f, 1}, 1.5f);
                font->render("Original Tribes 2 assets and gameplay compatibility", 20, 175,
                             {0.8f, 0.8f, 0.8f, 1}, 1.5f);
            }
            break;
        }
        default: break;
    }
}
