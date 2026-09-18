#include "game/hud.h"
#include "game/game.h"
#include "game/movement.h"
#include "render/renderer.h"
#include "core/engine.h"
#include "game/hud_parity.h"
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
    const bool liveObserver = dead &&
        game->isConnected() && game->activeConnection() &&
        game->activeConnection()->isObserverMode();
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

    // Crosshair
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
        const float maxHealth = 100.0f;
        renderHealthBar(game->player().health(), maxHealth);
        renderEnergyBar(game->player().energy(), 100.0f);
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
        snprintf(buf, sizeof(buf), "EN: %.0f/100  RECHARGE: %.1f",
                 game->player().energy(), Movement::energyRecharge);
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
        const int targetIndex = selected >= 0 ? selected : (int)observer.controlGhost;
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
        int mins = (int)(game->getDemoTime() / 60);
        int secs = (int)(game->getDemoTime()) % 60;
        int totalMins = (int)(game->getDemoTotalTime() / 60);
        int totalSecs = (int)(game->getDemoTotalTime()) % 60;
        const char* status = "> PLAY";
        if (game->isDemoPaused()) status = "|| PAUSE";
        else if (game->isDemoFastForward()) status = ">> FAST";
        ColorF statusColor = game->isDemoPaused() ? ColorF{1, 1, 0, 1} : ColorF{0, 1, 0, 1};
        // Speed display
        float speed = game->getDemoSpeed();
        char speedBuf[16];
        snprintf(speedBuf, sizeof(speedBuf), "%.1fx", speed);

        snprintf(buf, sizeof(buf), "[%s] %s %02d:%02d/%02d:%02d",
                 status, speedBuf, mins, secs, totalMins, totalSecs);
        if (font) font->render(buf, 20.0f, 20.0f, statusColor, 2.0f);

        // Graphical progress bar
        float progress = game->getDemoTotalTime() > 0
            ? game->getDemoTime() / game->getDemoTotalTime() : 0;
        if (progress < 0) progress = 0;
        if (progress > 1) progress = 1;
        float barW = 300.0f, barH = 8.0f;
        float bx = 20.0f, by = 42.0f;
        // Background
        Box3F bgBox = {{bx, by - barH, 0}, {bx + barW, by, 0}};
        r.drawBox(bgBox, {0.2f, 0.2f, 0.2f, 0.7f});
        // Fill
        float fw = barW * progress;
        Box3F fillBox = {{bx + 1, by - barH + 1, 0}, {bx + fw - 1, by - 1, 0}};
        r.drawBox(fillBox, {0.3f, 1.0f, 0.3f, 0.9f});
        // Border
        r.drawLine({bx, by, 0}, {bx + barW, by, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx + barW, by, 0}, {bx + barW, by - barH, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx + barW, by - barH, 0}, {bx, by - barH, 0}, {1, 1, 1, 0.5f});
        r.drawLine({bx, by - barH, 0}, {bx, by, 0}, {1, 1, 1, 0.5f});

        // Ghost count and help text
        if (auto* dp = game->getDemoParser()) {
            int ghostCount = dp->getGhostTracker().size();
            if (game->demoOrbitCamActive()) {
                snprintf(buf, sizeof(buf), "Ghosts: %d  [F2]orbit [A/D]rot [W/S]zoom [Spc/Shft]ht [R]target",
                         ghostCount);
            } else {
                snprintf(buf, sizeof(buf), "Ghosts: %d  [P]ause [.]step [F1]free [F2]orbit [E]vents [Tab]score [R]target",
                         ghostCount);
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
                // Only show tags for Player-class ghosts within range
                if (g->className != "Player" && g->className != "MPB") continue;
                float dx = g->position.x - r.cameraPos.x;
                float dy = g->position.y - r.cameraPos.y;
                float dz = g->position.z - r.cameraPos.z;
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                if (dist > 500.0f) continue;
                // Project world position to screen (above head)
                Point3F wp = {g->position.x, g->position.y + 2.5f, g->position.z};
                const MatrixF& view = r.viewMatrix();
                const MatrixF& proj = r.projectionMatrix();
                const float* v = &view.m[0][0];
                float cx = wp.x*v[0]+wp.y*v[4]+wp.z*v[8]+v[12];
                float cy = wp.x*v[1]+wp.y*v[5]+wp.z*v[9]+v[13];
                float cz = wp.x*v[2]+wp.y*v[6]+wp.z*v[10]+v[14];
                float cw = wp.x*v[3]+wp.y*v[7]+wp.z*v[11]+v[15];
                const float* p = &proj.m[0][0];
                float nx = cx*p[0]+cy*p[4]+cz*p[8]+cw*p[12];
                float ny = cx*p[1]+cy*p[5]+cz*p[9]+cw*p[13];
                float nz = cx*p[2]+cy*p[6]+cz*p[10]+cw*p[14];
                float nw = cx*p[3]+cy*p[7]+cz*p[11]+cw*p[15];
                if (nw == 0 || nz < 0) continue;
                float invW = 1.0f / nw;
                float sx = (nx*invW*0.5f+0.5f)*w;
                float sy = (-ny*invW*0.5f+0.5f)*h;
                // Background box for name
                float tw = (float)g->playerName.size() * 10.0f;
                r.drawBox({{sx - tw/2 - 4, sy - 2, 0}, {sx + tw/2 + 4, sy + 16, 0}}, {0,0,0,0.5f});
                // Team color dot
                std::string sn = g->skinName;
                for (auto& c : sn) c = (char)tolower(c);
                ColorF nameCol = {1,1,0,1};
                if (sn.find("red") != std::string::npos) nameCol = {1,0.3f,0.3f,1};
                else if (sn.find("blue") != std::string::npos) nameCol = {0.3f,0.4f,1,1};
                else if (sn.find("green") != std::string::npos) nameCol = {0.3f,1,0.3f,1};
                font->render(g->playerName.c_str(), sx - tw/2, sy, nameCol, 2.0f);
                // Health bar below name
                float hp = std::max(0.0f, std::min(1.0f, g->health / 100.0f));
                float barW = 60.0f, barH = 6.0f;
                float bx = sx - barW/2, by = sy + 18;
                ColorF bgCol = {0.2f, 0.0f, 0.0f, 0.7f};
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
                if (!ghost || (ghost->className != "Player" && ghost->className != "MPB")) continue;
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
        for (const auto& entry : entries) {
            if (row >= 13) break;
            const float ry = y + 94.0f + row * 28.0f;
            if (row == impl->targetSelection)
                r.drawRectFill({x + 12, ry - 3, 0}, {x + 508, ry + 23, 0}, {1, 1, 0, 0.18f});
            const std::string label = (entry.selectable ? "> " : "  ") + entry.label;
            font->render(label.c_str(), x + 24, ry,
                         row == impl->targetSelection ? ColorF{1, 1, 0, 1} : ColorF{0.85f, 0.85f, 0.85f, 1}, 1.5f);
            ++row;
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
    float fill = maxHealth > 0.0f ? health / maxHealth : 0.0f;
    if (fill > 1) fill = 1;
    if (fill < 0) fill = 0;

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (fill > 0) {
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
    float fill = maxEnergy > 0.0f ? energy / maxEnergy : 0.0f;
    if (fill > 1) fill = 1;
    if (fill < 0) fill = 0;

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (fill > 0) {
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
    float fill = (float)current / (float)max;
    if (fill > 1) fill = 1;
    if (fill < 0) fill = 0;

    r.drawLine({x, y, 0}, {x + barW, y, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y, 0}, {x + barW, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x + barW, y - barH, 0}, {x, y - barH, 0}, {1, 1, 1, 0.5f});
    r.drawLine({x, y - barH, 0}, {x, y, 0}, {1, 1, 1, 0.5f});

    if (fill > 0) {
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
    const int teamRows = game->isConnected() ? (int)game->getLiveTeamScores().size() : 0;
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
            const auto& players = dp->getPlayerInfo();
            for (auto& p : players) {
                if (row >= maxRows) break;
                float ry = headerY + 26 + row * 22;
                ColorF col = {0.8f, 0.8f, 1.0f, 0.9f};
                ColorF teamCol = {0.5f, 0.5f, 0.5f, 0.8f};
                if (p.teamId == 0) teamCol = {1, 0.3f, 0.3f, 0.9f};   // Red team
                else if (p.teamId == 1) teamCol = {0.3f, 0.4f, 1, 0.9f};  // Blue team
                else if (p.teamId == 2) teamCol = {0.3f, 1, 0.3f, 0.9f};  // Green team
                snprintf(buf, sizeof(buf), "%s", p.name.c_str());
                if (font) font->render(buf, colX[0], ry, col, 2.0f);
                const char* teamName = "N/A";
                if (p.teamId == 0) teamName = "Red";
                else if (p.teamId == 1) teamName = "Blue";
                else if (p.teamId == 2) teamName = "Green";
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
                for (const auto& [clientId, name] : observer.clientNames) {
                    if (row >= maxRows) break;
                    const auto team = observer.clientTeams.find(clientId);
                    const auto score = observer.playerScores.find(clientId);
                    const auto ping = observer.playerPings.find(clientId);
                    const auto loss = observer.playerPacketLoss.find(clientId);
                    const auto target = observer.clientTargets.find(clientId);
                    const GhostEntry* ghost = target == observer.clientTargets.end()
                        ? nullptr : game->getLiveGhost(target->second);
                 float ry = headerY + 26 + row * 22;
                    ColorF nameCol = {0.8f, 0.8f, 1.0f, 0.9f};
                    if (team != observer.clientTeams.end() && team->second == 1)
                        nameCol = {1.0f, 0.3f, 0.3f, 0.9f};
                    else if (team != observer.clientTeams.end() && team->second == 2)
                        nameCol = {0.3f, 0.4f, 1.0f, 0.9f};
                    if (font) font->render(name.c_str(), colX[0], ry, nameCol, 2.0f);
                    const char* teamName = team == observer.clientTeams.end() ? "N/A" :
                        (team->second == 1 ? "Red" : team->second == 2 ? "Blue" : "N/A");
                    if (font) font->render(teamName, colX[1], ry, nameCol, 2.0f);
                    snprintf(buf, sizeof(buf), "%d", score == observer.playerScores.end() ? 0 : score->second);
                    if (font) font->render(buf, colX[2], ry, {1, 1, 0, 0.9f}, 2.0f);
                    snprintf(buf, sizeof(buf), "-");
                    if (font) font->render(buf, colX[3], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
                    if (ghost) snprintf(buf, sizeof(buf), "%.0f%%", ghost->health);
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
            if (!g || g->className != "Player") continue;
             float ry = headerY + 26 + row * 22;
            ColorF nameCol = (g->teamId == 1) ? ColorF{1,0.3f,0.3f,0.9f} :
                             (g->teamId == 2) ? ColorF{0.3f,0.4f,1,0.9f} :
                             ColorF{0.8f,0.8f,1,0.9f};
            snprintf(buf, sizeof(buf), "%s", g->playerName.empty() ? "Player" : g->playerName.c_str());
            if (font) font->render(buf, colX[0], ry, nameCol, 2.0f);
            const char* teamName = g->teamId == 1 ? "Red" : g->teamId == 2 ? "Blue" : "N/A";
            if (font) font->render(teamName, colX[1], ry, nameCol, 2.0f);
             snprintf(buf, sizeof(buf), "%d", g->score);
            if (font) font->render(buf, colX[2], ry, {1,1,0,0.9f}, 2.0f);
            snprintf(buf, sizeof(buf), "%d", g->deaths);
            if (font) font->render(buf, colX[3], ry, {1,0.5f,0.2f,0.9f}, 2.0f);
            snprintf(buf, sizeof(buf), "%.0f", g->health);
            if (font) font->render(buf, colX[4], ry, {0,1,0,0.9f}, 2.0f);
            row++;
        }
        if (row == 0 && font)
             font->render("No live players", colX[0], headerY + 26, {0.5f,0.5f,0.5f,1}, 2.0f);
        return;
    }

    // Fallback: local player only (single-player / demo)
    row = 0;
    auto& p = game->player();
    snprintf(buf, sizeof(buf), "%s", game->config().playerName.c_str());
    if (font) font->render(buf, colX[0], by + 108 + row * 30, {0, 1, 0, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%.0f", p.score);
    if (font) font->render(buf, colX[1], by + 108 + row * 30, {1, 1, 1, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%d", p.kills);
    if (font) font->render(buf, colX[2], by + 108 + row * 30, {1, 1, 1, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%d", p.deaths);
    if (font) font->render(buf, colX[3], by + 108 + row * 30, {1, 1, 1, 1}, 2.0f);
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
    if (!active) return;

    auto& input = Engine::instance().platform().input();
    auto& ren = Engine::instance().renderer();

    static bool prevUp = false, prevDown = false, prevEnter = false, prevEsc = false;
    bool up = input.keysDown[SCANCODE_UP];
    bool down = input.keysDown[SCANCODE_DOWN];
    bool enter = input.keysDown[SCANCODE_RETURN] &&
                 !input.consumedSc[SCANCODE_RETURN];
    bool esc = input.keysDown[SCANCODE_ESCAPE];

    // Mouse hover: update selectedItem based on mouse Y position
    int mx = input.mouseX;
    int my = input.mouseY;
    if (currentScreen == Main) {
        const int itemCount = 5;
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
        static bool prevMouseBtn = false;
        // SDL reports the left button as index 1 (SDL_BUTTON_LEFT), matching the
        // engine's GUI click path (engine.cpp uses mouseButtons[1]).
        bool mouseBtn = input.mouseButtons[1] != 0;
        if (mouseBtn && !prevMouseBtn && !input.consumedMouse[1]) {
            for (int i = 0; i < itemCount; i++) {
                float iy = startY + i * itemH;
                if (my >= iy && my < iy + itemH && mx >= leftX && mx <= rightX) {
                    selectedItem = i;
                    enter = true;
                }
            }
        }
        prevMouseBtn = mouseBtn;
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
                case 4:
                    Engine::instance().quit();
                    break;
                default: break;
            }
        }
    } else if (currentScreen == ServerBrowser) {
        if (esc && !prevEsc) { currentScreen = Main; selectedItem = 0; }
        // Refresh list
        static double lastRefresh = 0;
        if (Engine::instance().timer().now() - lastRefresh > 2.0) {
            lastRefresh = Engine::instance().timer().now();
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
        // Navigate list
        int count = (int)servers.size();
        if (count > 0) {
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
        if (esc && !prevEsc) { currentScreen = Main; selectedItem = 2; }
    } else if (currentScreen == Controls) {
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

            const char* items[] = {"Start Local Game", "Server Browser", "Settings", "Controls", "Quit"};
            // Hover/selection highlight box behind the active item
            {
                float hx = w * 0.5f - 100.0f;
                float hy = 200.0f + (float)selectedItem * 40.0f;
                r.drawRectFill({hx, hy, 0.0f}, {hx + 200.0f, hy + 40.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.18f});
            }
            for (int i = 0; i < 5; i++) {
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
                    if (font) font->render(buf, 20, 80 + i * 30, {1, 1, 1, 1}, 2.0f);
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
        default: break;
    }
}
