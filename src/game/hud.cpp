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
    {"altfire", "Alt Fire"}, {"zoom", "Zoom"}, {"reload", "Reload"},
    {"scoreboard", "Scoreboard"}, {"f1", "Free Camera"},
    {"f2", "Orbit Camera"}, {"chat", "Chat"}, {"console", "Console"},
};
constexpr int kRemapActionCount = sizeof(kRemapActions) / sizeof(kRemapActions[0]);
}

struct HUD::Impl {
    struct Message {
        std::string text;
        ColorF color;
        double start;
        double duration;
    };
    std::vector<Message> messages;
    std::vector<std::string> chatLines;
    std::string chatInput;
    double messageStart = 0;
    std::string targetSearch;
    int targetSelection = 0;
    std::string objectiveLine1;
    std::string objectiveLine2;
};

HUD::HUD() : impl(new Impl) {}
HUD::~HUD() { delete impl; }

void HUD::init() {}

void HUD::resetState() {
    impl->messages.clear();
    impl->chatLines.clear();
    impl->chatInput.clear();
    impl->messageStart = 0;
    impl->targetSearch.clear();
    impl->targetSelection = 0;
    clearObjectiveTask();
}

void HUD::setObjectiveTask(const char* line1, const char* line2) {
    impl->objectiveLine1 = line1 ? line1 : "";
    impl->objectiveLine2 = line2 ? line2 : "";
}

void HUD::clearObjectiveTask() {
    impl->objectiveLine1.clear();
    impl->objectiveLine2.clear();
}

void HUD::render(Game* game) {
    const bool dead = game && game->state() == Game::Dead;
    const bool liveObserver = game && game->isConnected() &&
        game->activeConnection() && ObserverParity::isLiveObserver(
            game->isConnected(), game->activeConnection()->isObserverMode());
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

    // Save perspective projection for 3D name tag projection
    MatrixF perspProj = r.projectionMatrix();
    MatrixF perspView = r.viewMatrix();

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

    // The reticle belongs to active first-person play, not dead/observer HUDs
    // or the target-finder overlay.
    if (HudParity::crosshairVisible(dead, liveObserver, game->targetFinderOpen()))
        renderCrosshair();

    // GameRenderFilters applies these effects for both network and demo
    // snapshots. They are intentionally drawn before the HUD controls.
    const float damageFlash = game->getDamageFlash();
    const float whiteOut = game->getWhiteOut();
    if (damageFlash > 0.0f)
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}},
                  {0.8f, 0, 0, std::min(damageFlash * 0.5f, 0.6f)});
    if (whiteOut > 0.0f)
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}},
                  {1, 1, 1, std::min(whiteOut * 0.5f, 0.8f)});

    char buf[128];
    if (!dead && !liveObserver) {
        // PlayerData::maxDamage is the local ShapeBase health cap. Using the
        // stock 100-point default makes heavy/custom armor display the wrong
        // bar length and HP denominator.
        const float maxHealth = game->player().maxHealth();
        renderHealthBar(game->player().health(), maxHealth);
        const float maxEnergy = game->player().maxEnergy();
        renderEnergyBar(game->player().energy(), maxEnergy);
        const int weapon = game->player().currentWeapon();
        const int ammo = weapon >= 0 && weapon < game->player().weaponCount()
            ? game->player().weapon(weapon).ammo : -1;
        int maxAmmo = 0;
        if (weapon >= 0 && weapon < game->player().weaponCount()) {
            const int type = game->player().weapon(weapon).type;
            if (type >= 0 && type < gWeaponCount) maxAmmo = gWeaponTable[type].maxAmmo;
        }
        renderAmmo(ammo, maxAmmo);
        snprintf(buf, sizeof(buf), "HP: %.0f/%.0f  AR: %.0f",
                 game->player().health(), maxHealth, game->player().armor());
        if (font) font->render(buf, 20.0f, (float)h - 60.0f, {1, 1, 1, 1}, 2.0f);
        snprintf(buf, sizeof(buf), "EN: %.0f/%.0f  RECHARGE: %.1f",
                 game->player().energy(), maxEnergy, Movement::energyRecharge);
        if (font) font->render(buf, 20.0f, (float)h - 40.0f, {0, 1, 1, 1}, 2.0f);
    }

    if (dead && !liveObserver && font) {
        font->render("YOU ARE DEAD", w * 0.5f - 90.0f, h * 0.5f - 30.0f,
                     {1.0f, 0.25f, 0.2f, 1.0f}, 2.5f);
        font->render("Respawning...", w * 0.5f - 75.0f, h * 0.5f + 10.0f,
                     {0.8f, 0.8f, 0.8f, 1.0f}, 1.5f);
    }

    // Stock Observer HUD keeps the current control target visible even when
    // the camera is not attached to a player.  The native observer snapshot
    // has the same target/client association used by the scoreboard.
    if (liveObserver) {
        const auto observer = game->activeConnection()->observerSnapshot();
        const int selected = game->getSpectateGhostIndex();
        const int targetIndex = selected >= 0 ? selected :
            ObserverParity::controlGhostIndex(observer.controlGhost);
        const GhostEntry* target = game->getLiveGhost(targetIndex);
        const char* targetName = target && !target->playerName.empty()
            ? target->playerName.c_str() : "Free camera";
        snprintf(buf, sizeof(buf), "FOLLOW: %s  [R / RMB] next", targetName);
        if (font) font->render(buf, w * 0.5f - 80.0f, 20.0f,
                               {0.35f, 1.0f, 0.55f, 0.95f}, 2.0f);
    }

    // Messages
    double now = Engine::instance().timer().now();
    for (size_t i = 0; i < impl->messages.size();) {
        auto& msg = impl->messages[i];
        double age = now - msg.start;
        if (msg.duration > 0.0 && age >= msg.duration) {
            impl->messages.erase(impl->messages.begin() + i);
            continue;
        }
        float alpha = HudParity::messageAlpha(age, msg.duration);
        ColorF color = msg.color;
        color.a *= std::clamp(alpha, 0.0f, 1.0f);
        if (font) font->render(msg.text.c_str(), w * 0.5f - 100, h * 0.3f + (float)i * 25,
                               color, 2.0f);
        i++;
    }
    if (font && !impl->chatInput.empty()) {
        font->render(("> " + impl->chatInput).c_str(), 20.0f, h - 40.0f,
                     {1, 1, 1, 1}, 2.0f);
    }

    if (font && (!impl->objectiveLine1.empty() || !impl->objectiveLine2.empty())) {
        const float objectiveY = dead ? 90.0f : 20.0f;
        font->render("OBJECTIVE", 20.0f, objectiveY,
                     {1.0f, 0.85f, 0.25f, 1.0f}, 1.1f);
        if (!impl->objectiveLine1.empty())
            font->render(impl->objectiveLine1.c_str(), 20.0f, objectiveY + 18.0f,
                         {1, 1, 1, 1}, 1.0f);
        if (!impl->objectiveLine2.empty())
            font->render(impl->objectiveLine2.c_str(), 20.0f, objectiveY + 34.0f,
                         {1, 1, 1, 1}, 1.0f);
    }

    // Demo playback info
    if (game->isDemoPlaying()) {
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

        // Chat message overlay (recent demo events)
        if (font) {
            const auto& events = game->getDemoEventLog();
            int total = (int)events.size();
            int chatStart = std::max(0, total - 6);
            float cy = (float)h - 180.0f;
            for (int i = chatStart; i < total; i++) {
                const auto& e = events[i];
                ColorF chatCol = (e.type == 0) ? ColorF{0.3f, 1.0f, 0.3f, 0.8f} :
                                 (e.type == 1) ? ColorF{1.0f, 1.0f, 0.3f, 0.8f} :
                                                  ColorF{0.8f, 0.8f, 0.8f, 0.7f};
                char line[512];
                if (e.text.size() > 80) {
                    snprintf(line, sizeof(line), "<%.60s...", e.text.c_str());
                } else {
                    snprintf(line, sizeof(line), "<%s", e.text.c_str());
                }
                font->render(line, 20.0f, cy, chatCol, 2.0f);
                cy += 20.0f;
            }
        }

        // Player name tags
        if (auto* dp = game->getDemoParser()) {
            const GhostTracker& gt = dp->getGhostTracker();
            auto indices = gt.getAllIndices();
            int w = Engine::instance().renderer().config().width;
            int h = Engine::instance().renderer().config().height;
            // Restore perspective projection for 3D-to-2D projection
            r.setProjection(perspProj);
            r.setView(perspView);
            for (int gi : indices) {
                const GhostEntry* g = gt.getGhost(gi);
                if (!g || g->playerName.empty()) continue;
                // AIPlayer is a Player subclass in Tribes 2. Keep bot name
                // tags consistent with observer targeting and live triggers.
                if (!ObserverParity::isPlayerClass(g->className)) continue;
                 // Demo ghosts retain their native Torque Z-up coordinates;
                 // project the same Y-up position used by shape rendering.
                 const Point3F renderPosition = Math::torquePointToYUp(
                     {g->position.x, g->position.y, g->position.z});
                 float dx = renderPosition.x - r.cameraPos.x;
                 float dy = renderPosition.y - r.cameraPos.y;
                 float dz = renderPosition.z - r.cameraPos.z;
                 float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                 if (dist > 500.0f) continue;
                 // Project world position to screen (above head)
                 Point3F wp = {renderPosition.x, renderPosition.y + 2.5f,
                               renderPosition.z};
                const MatrixF& view = r.viewMatrix();
                const MatrixF& proj = r.projectionMatrix();
                 const float* v = &view.m[0][0];
                 // MatrixF is row-major in its transform/operator* methods.
                 // Using column-major indexing here displaced name tags and
                 // rejected the near half of the visible depth range.
                 float cx = wp.x*v[0]+wp.y*v[1]+wp.z*v[2]+v[3];
                 float cy = wp.x*v[4]+wp.y*v[5]+wp.z*v[6]+v[7];
                 float cz = wp.x*v[8]+wp.y*v[9]+wp.z*v[10]+v[11];
                 float cw = wp.x*v[12]+wp.y*v[13]+wp.z*v[14]+v[15];
                 const float* p = &proj.m[0][0];
                 float nx = cx*p[0]+cy*p[1]+cz*p[2]+cw*p[3];
                 float ny = cx*p[4]+cy*p[5]+cz*p[6]+cw*p[7];
                 float nz = cx*p[8]+cy*p[9]+cz*p[10]+cw*p[11];
                 float nw = cx*p[12]+cy*p[13]+cz*p[14]+cw*p[15];
                 if (nw <= 0 || nz < -nw || nz > nw) continue;
                float invW = 1.0f / nw;
                float sx = (nx*invW*0.5f+0.5f)*w;
                float sy = (-ny*invW*0.5f+0.5f)*h;
                // Background box for name
                float tw = (float)g->playerName.size() * 10.0f;
                r.drawBox({{sx - tw/2 - 4, sy - 2, 0}, {sx + tw/2 + 4, sy + 16, 0}}, {0,0,0,0.5f});
                 // Tribes 2 colors name tags from the replicated sensor group,
                 // not from the selected skin filename. Custom skins must not
                 // make a player's team appear to change.
                 ColorF nameCol = HudParity::teamColor(g->teamId);
                 font->render(g->playerName.c_str(), sx - tw/2, sy, nameCol, 2.0f);
                // Health bar below name
                 // Ghosts replicate their datablock maximum health.  Heavy
                 // armor and custom PlayerData otherwise show an incorrectly
                 // full/empty observer health bar when using a hard-coded 100.
                 float hp = HudParity::resourceFraction(g->health, g->maxHealth);
                float barW = 60.0f, barH = 6.0f;
                float bx = sx - barW/2, by = sy + 18;
                ColorF fgCol = hp > 0.5f ? ColorF{0,1,0,0.9f} : (hp > 0.25f ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f});
                r.drawBox({{bx - 1, by - 1, 0}, {bx + barW + 1, by + barH + 1, 0}}, {0,0,0,0.5f});
                r.drawBox({{bx, by, 0}, {bx + barW * hp, by + barH, 0}}, fgCol);
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

    if (font && !game->isDemoPlaying()) {
        const int total = (int)impl->chatLines.size();
        float cy = (float)h - 180.0f;
        for (int i = std::max(0, total - 6); i < total; ++i) {
            font->render(impl->chatLines[i].c_str(), 20.0f, cy,
                         {0.8f, 1.0f, 0.8f, 0.85f}, 2.0f);
            cy += 20.0f;
        }
    }

    // GameRenderFilters applies the water filter before the GUI. Lava and
    // quicksand deliberately do not use it.
    if (game->isUnderwater()) {
        r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {0.2f, 0.6f, 0.6f, 0.3f});
    }

    // Scoreboard (Tab overlay)
    if (game->scoreboardShown()) {
        renderScoreboard(game);
    }

    // Pause overlay
    if (game->isGamePaused() && font) {
        auto& plat = Engine::instance().platform();
        r.drawBox({{0, 0, 0}, {(float)plat.width(), (float)plat.height(), 0}}, {0, 0, 0, 0.6f});
        font->render("PAUSED", 400, 300, {1, 1, 1, 1}, 3.0f);
        font->render("[ESC] Resume  [Q] Quit to Desktop", 300, 360, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
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

void HUD::renderCrosshair() {
    auto& r = Engine::instance().renderer();
    int32_t w = Engine::instance().platform().width() / 2;
    int32_t h = Engine::instance().platform().height() / 2;
    float s = 8.0f;

    r.drawLine({(float)w - s, (float)h, 0}, {(float)w + s, (float)h, 0}, {0, 1, 0, 1});
    r.drawLine({(float)w, (float)h - s, 0}, {(float)w, (float)h + s, 0}, {0, 1, 0, 1});
}

void HUD::renderHealthBar(float health, float maxHealth) {
    auto& r = Engine::instance().renderer();
    int32_t h = Engine::instance().platform().height();
    float barW = 200.0f, barH = 16.0f;
    float x = 20.0f, y = (float)h - 80.0f;
    const float fill = HudParity::resourceFraction(health, maxHealth);

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (HudParity::resourceFillVisible(health, maxHealth, barW)) {
        float fw = barW * fill;
        Box3F hBox = {{x + 1, y - barH + 1, 0}, {x + fw - 1, y - 1, 0}};
        r.drawBox(hBox, {0.2f, 1.0f, 0.2f, 0.8f});
    }
}

void HUD::renderEnergyBar(float energy, float maxEnergy) {
    auto& r = Engine::instance().renderer();
    int32_t h = Engine::instance().platform().height();
    float barW = 200.0f, barH = 12.0f;
    float x = 20.0f, y = (float)h - 56.0f;
    const float fill = HudParity::resourceFraction(energy, maxEnergy);

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (HudParity::resourceFillVisible(energy, maxEnergy, barW)) {
        float fw = barW * fill;
        Box3F eBox = {{x + 1, y - barH + 1, 0}, {x + fw - 1, y - 1, 0}};
        r.drawBox(eBox, {0.2f, 0.8f, 1.0f, 0.8f});
    }
}

void HUD::renderAmmo(int32_t current, int32_t max) {
    auto& r = Engine::instance().renderer();
    int32_t h = Engine::instance().platform().height();
    float barW = 200.0f, barH = 10.0f;
    float x = 20.0f, y = (float)h - 36.0f;
    if (max <= 0) return;
    const float fill = HudParity::resourceFraction((float)current, (float)max);

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (HudParity::resourceFillVisible((float)current, (float)max, barW)) {
        float fw = barW * fill;
        Box3F aBox = {{x + 1, y - barH + 1, 0}, {x + fw - 1, y - 1, 0}};
        r.drawBox(aBox, {1.0f, 0.8f, 0.2f, 0.8f});
    }
}

void HUD::renderScoreboard(Game* game) {
    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    if (!font) return;

    int32_t w = Engine::instance().platform().width();
    int32_t h = Engine::instance().platform().height();

    // Leave room for the team/flag strip above the player table.
    const bool observerMode = game->isConnected() && game->activeConnection() &&
        game->activeConnection()->isObserverMode();
    const int teamRows = HudParity::scoreboardTeamRows(
        game->isConnected(), observerMode, (int)game->getLiveTeamScores().size());
    float bw = 550.0f, bh = std::max(400.0f, 430.0f + teamRows * 16.0f);
    float bx = (w - bw) * 0.5f, by = (h - bh) * 0.5f;
    r.drawBox(Box3F{{bx, by, 0}, {bx + bw, by + bh, 0}}, {0, 0, 0, 0.7f});

    char buf[256];

    // Title
    if (font) font->render("SCOREBOARD", bx + 10, by + 10, {1, 1, 0, 1}, 2.0f);
    if (game->isConnected() && game->activeConnection() &&
        game->activeConnection()->isObserverMode()) {
        const auto observer = game->activeConnection()->observerSnapshot();
        snprintf(buf, sizeof(buf), "Observed players: %zu  targets: %zu  mission CRC: %08X",
                 observer.players.size(), observer.targets.size(), observer.missionCrc);
        if (font) font->render(buf, bx + 10, by + 30, {0.65f, 0.75f, 0.85f, 0.9f}, 1.0f);
        const char* phase = game->liveMatchEnded() ? "DEBRIEF" :
                            game->liveMatchStarted() ? "MATCH LIVE" : "WARMUP";
        if (font) font->render(phase, bx + 10, by + 40,
                              {0.55f, 0.65f, 0.75f, 0.8f}, 1.0f);
        if (font && (!game->liveMissionDisplayName().empty() ||
                     !game->liveMissionType().empty())) {
            snprintf(buf, sizeof(buf), "%s  %s",
                     game->liveMissionDisplayName().c_str(),
                     game->liveMissionType().c_str());
            font->render(buf, bx + 140, by + 40, {0.75f, 0.75f, 0.55f, 0.9f}, 1.0f);
        }
        const int clockMs = game->liveClockRemainingMs();
        if (clockMs > 0) {
            snprintf(buf, sizeof(buf), "Clock %d:%02d", clockMs / 60000,
                     (clockMs / 1000) % 60);
            if (font) font->render(buf, bx + 420, by + 40, {0.9f, 0.85f, 0.5f, 0.9f}, 1.0f);
        }
        if (font && !game->liveLoadInfoLines().empty()) {
            const auto& line = game->liveLoadInfoLines().front();
            font->render(line.c_str(), bx + 20, by + 385, {0.8f, 0.8f, 0.7f, 0.9f}, 1.0f);
        }
        float teamY = by + 58;
        for (const auto& [teamId, team] : game->getLiveTeamScores()) {
            int playerCount = 0;
            const auto snapshot = game->activeConnection()->observerSnapshot();
            for (const auto& [clientId, clientTeam] : snapshot.clientTeams)
                if (clientTeam == teamId) ++playerCount;
            const char* flag = team.flagStatus == "held" ? "Held" :
                               team.flagStatus == "field" ? "Dropped" : "Home";
            const char* carrier = team.flagStatus == "held" && !team.flagCarrier.empty()
                ? team.flagCarrier.c_str() : "";
            const char* name = team.name.empty() ?
                (teamId == 1 ? "Storm" : teamId == 2 ? "Inferno" : "Team") : team.name.c_str();
            snprintf(buf, sizeof(buf), "%s  %d  (%d)  Flag: %s%s%s",
                     name, team.score, playerCount, flag,
                     carrier[0] ? " " : "", carrier[0] ? carrier : "");
            if (font) font->render(buf, bx + 20, teamY,
                                   {0.8f, 0.85f, 0.45f, 0.95f}, 1.0f);
            teamY += 16.0f;
        }
    }

    // Column headers
    float colX[] = {bx + 20, bx + 140, bx + 250, bx + 325, bx + 395, bx + 455, bx + 505};
    const char* headers[] = {"Player", "Team", "Score", "Damage", "Health", "Ping", "Loss"};
    const float headerY = HudParity::scoreboardHeaderY(by, teamRows);
    for (int i = 0; i < 7; i++) {
        if (font) font->render(headers[i], colX[i], headerY, {1, 1, 1, 1}, 2.0f);
    }

    int row = 0;
    const int maxRows = 15;

    // Use demo player data if available
    if (game->isDemoPlaying()) {
        auto* dp = game->getDemoParser();
        if (dp) {
            auto players = dp->getPlayerInfo();
            std::stable_sort(players.begin(), players.end(), [](const auto& a, const auto& b) {
                return HudParity::scoreboardPlayerBefore(
                    a.teamId, a.score, a.clientId, a.name,
                    b.teamId, b.score, b.clientId, b.name);
            });
            for (auto& p : players) {
                if (row >= maxRows) break;
                float ry = headerY + 26 + row * 22;
                ColorF col = {0.8f, 0.8f, 1.0f, 0.9f};
                 const ColorF teamCol = HudParity::teamColor(p.teamId);
                snprintf(buf, sizeof(buf), "%s", p.name.c_str());
                if (font) font->render(buf, colX[0], ry, col, 2.0f);
                 const char* teamName = HudParity::teamName(p.teamId);
                if (font) font->render(teamName, colX[1], ry, teamCol, 2.0f);
                snprintf(buf, sizeof(buf), "%d", p.score);
                if (font) font->render(buf, colX[2], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
                float dmgPct = (1.0f - p.damage) * 100.0f;
                snprintf(buf, sizeof(buf), "%.0f%%", p.damage * 100.0f);
                if (font) font->render(buf, colX[3], ry, {1, 0.5f, 0.2f, 0.9f}, 2.0f);
                ColorF hc = dmgPct > 66 ? ColorF{0,1,0,0.9f} : dmgPct > 33 ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f};
                 snprintf(buf, sizeof(buf), "%.0f%%", dmgPct);
                 if (font) font->render(buf, colX[4], ry, hc, 2.0f);
                 snprintf(buf, sizeof(buf), "%d", p.ping);
                 if (font) font->render(buf, colX[5], ry, {0.7f, 0.85f, 1.0f, 0.9f}, 2.0f);
                 snprintf(buf, sizeof(buf), "%d%%", p.packetLoss);
                 if (font) font->render(buf, colX[6], ry, {1.0f, 0.7f, 0.4f, 0.9f}, 2.0f);
                 row++;
            }
            if (row == 0 && font) {
                font->render("No player data available", colX[0], by + 80, {0.5f,0.5f,0.5f,1}, 2.0f);
            }
            return;
        }
    }

    // Live game: show all ghosts with kills/deaths from server
    if (game->isConnected()) {
        if (auto* connection = game->activeConnection()) {
            const auto observer = connection->observerSnapshot();
            if (!observer.clientNames.empty()) {
                std::vector<int> clientIds;
                clientIds.reserve(observer.clientNames.size());
                for (const auto& [clientId, name] : observer.clientNames)
                    clientIds.push_back(clientId);
                std::sort(clientIds.begin(), clientIds.end(), [&](int left, int right) {
                    const auto leftName = observer.clientNames.find(left);
                    const auto rightName = observer.clientNames.find(right);
                    const auto leftTeam = observer.clientTeams.find(left);
                    const auto rightTeam = observer.clientTeams.find(right);
                    const auto leftScore = observer.playerScores.find(left);
                    const auto rightScore = observer.playerScores.find(right);
                    return HudParity::scoreboardPlayerBefore(
                        leftTeam == observer.clientTeams.end() ? 0 : leftTeam->second,
                        leftScore == observer.playerScores.end() ? 0 : leftScore->second,
                        left, leftName == observer.clientNames.end() ? "" : leftName->second,
                        rightTeam == observer.clientTeams.end() ? 0 : rightTeam->second,
                        rightScore == observer.playerScores.end() ? 0 : rightScore->second,
                        right, rightName == observer.clientNames.end() ? "" : rightName->second);
                });
                for (const int clientId : clientIds) {
                    if (row >= maxRows) break;
                    const auto nameIt = observer.clientNames.find(clientId);
                    const std::string& name = nameIt->second;
                    const auto team = observer.clientTeams.find(clientId);
                    const auto score = observer.playerScores.find(clientId);
                    const auto ping = observer.playerPings.find(clientId);
                    const auto loss = observer.playerPacketLoss.find(clientId);
                    const auto target = observer.clientTargets.find(clientId);
                    const GhostEntry* ghost = target == observer.clientTargets.end()
                        ? nullptr : game->getLiveGhost(target->second);
                 float ry = headerY + 26 + row * 22;
                    ColorF nameCol = team == observer.clientTeams.end()
                        ? ColorF{0.8f, 0.8f, 1.0f, 0.9f}
                        : HudParity::teamColor(team->second);
                    if (font) font->render(name.c_str(), colX[0], ry, nameCol, 2.0f);
                    const char* teamName = team == observer.clientTeams.end() ? "N/A" :
                        HudParity::teamName(team->second);
                    if (font) font->render(teamName, colX[1], ry, nameCol, 2.0f);
                    snprintf(buf, sizeof(buf), "%d", score == observer.playerScores.end() ? 0 : score->second);
                    if (font) font->render(buf, colX[2], ry, {1, 1, 0, 0.9f}, 2.0f);
                    snprintf(buf, sizeof(buf), "-");
                    if (font) font->render(buf, colX[3], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
                     if (ghost) {
                         const float health = 100.0f * HudParity::resourceFraction(
                             ghost->health, ghost->maxHealth);
                         snprintf(buf, sizeof(buf), "%.0f%%", health);
                     }
                    else snprintf(buf, sizeof(buf), "-");
                    if (font) font->render(buf, colX[4], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
                    snprintf(buf, sizeof(buf), "%d", ping == observer.playerPings.end() ? 0 : ping->second);
                    if (font) font->render(buf, colX[5], ry, {0.7f, 0.85f, 1.0f, 0.9f}, 2.0f);
                    snprintf(buf, sizeof(buf), "%d%%", loss == observer.playerPacketLoss.end() ? 0 : loss->second);
                    if (font) font->render(buf, colX[6], ry, {1.0f, 0.7f, 0.4f, 0.9f}, 2.0f);
                    row++;
                }
                return;
            }
        }
        auto indices = game->getLiveGhostIndices();
        row = 0;
        for (auto idx : indices) {
            if (row >= maxRows) break;
            auto* g = game->getLiveGhost(idx);
            if (!g || !ObserverParity::isPlayerTarget(g->className, g->damageState))
                continue;
             float ry = headerY + 26 + row * 22;
            const ColorF nameCol = g->teamId == 0
                ? ColorF{0.8f, 0.8f, 1.0f, 0.9f}
                : HudParity::teamColor(g->teamId);
            snprintf(buf, sizeof(buf), "%s", g->playerName.empty() ? "Player" : g->playerName.c_str());
            if (font) font->render(buf, colX[0], ry, nameCol, 2.0f);
             const char* teamName = HudParity::teamName(g->teamId);
            if (font) font->render(teamName, colX[1], ry, nameCol, 2.0f);
             snprintf(buf, sizeof(buf), "%d", g->score);
            if (font) font->render(buf, colX[2], ry, {1,1,0,0.9f}, 2.0f);
            snprintf(buf, sizeof(buf), "%d", g->deaths);
            if (font) font->render(buf, colX[3], ry, {1,0.5f,0.2f,0.9f}, 2.0f);
             const float health = 100.0f * HudParity::resourceFraction(
                 g->health, g->maxHealth);
             snprintf(buf, sizeof(buf), "%.0f", health);
            if (font) font->render(buf, colX[4], ry, {0,1,0,0.9f}, 2.0f);
            row++;
        }
        if (row == 0 && font)
             font->render("No live players", colX[0], headerY + 26, {0.5f,0.5f,0.5f,1}, 2.0f);
        return;
    }

    // Fallback: local player only (single-player / demo)
    auto& p = game->player();
    const float ry = headerY + 26.0f;
    const ColorF teamCol = HudParity::teamColor(p.team());
    snprintf(buf, sizeof(buf), "%s", game->config().playerName.c_str());
    if (font) font->render(buf, colX[0], ry, teamCol, 2.0f);
    if (font) font->render(HudParity::teamName(p.team()), colX[1], ry, teamCol, 2.0f);
    snprintf(buf, sizeof(buf), "%.0f", p.score);
    if (font) font->render(buf, colX[2], ry, {1, 1, 1, 1}, 2.0f);
    if (font) font->render("-", colX[3], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
    const float healthPercent = 100.0f * HudParity::resourceFraction(
        p.health(), p.maxHealth());
    snprintf(buf, sizeof(buf), "%.0f%%", healthPercent);
    if (font) font->render(buf, colX[4], ry, {0, 1, 0, 0.9f}, 2.0f);
    if (font) font->render("-", colX[5], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
    if (font) font->render("-", colX[6], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
}

void HUD::renderMessage(const char* text, float duration) {
    if (impl->messages.size() >= 16) impl->messages.erase(impl->messages.begin());
    impl->messages.push_back({text ? text : "", {1, 1, 1, 1},
                              Engine::instance().timer().now(), std::max(0.0f, duration)});
}

void HUD::showMessage(const char* text, const ColorF& color) {
    if (impl->messages.size() >= 16) impl->messages.erase(impl->messages.begin());
    impl->messages.push_back({text ? text : "", color,
                              Engine::instance().timer().now(), 3.0});
}

void HUD::addChatLine(const char* text) {
    if (!text || !*text) return;
    impl->chatLines.emplace_back(text);
    if (impl->chatLines.size() > 50)
        impl->chatLines.erase(impl->chatLines.begin());
}

void HUD::setChatInput(const char* text) {
    impl->chatInput = text ? text : "";
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
