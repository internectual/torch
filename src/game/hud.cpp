#include "game/hud.h"
#include "game/game.h"
#include "render/renderer.h"
#include "core/engine.h"
#include <GL/glew.h>
#include <algorithm>
#include <vector>

struct HUD::Impl {
    std::vector<std::pair<std::string, double>> messages;
    std::vector<std::string> chatLines;
    double messageStart = 0;
};

HUD::HUD() : impl(new Impl) {}
HUD::~HUD() { delete impl; }

void HUD::init() {}

void HUD::render(Game* game) {
    if (!visible || !game || game->state() != Game::Playing) return;

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
    ortho.m[3][0] = -1.0f;
    ortho.m[3][1] = 1.0f;
    r.setProjection(ortho);
    MatrixF id; id.identity();
    r.setView(id);
    glDisable(GL_DEPTH_TEST);

    // Crosshair
    renderCrosshair();

    // Health
    renderHealthBar(game->player().health(), 100.0f);

    // Energy
    renderEnergyBar(game->player().energy(), 100.0f);

    // HUD text
    char buf[128];
    snprintf(buf, sizeof(buf), "HP: %.0f", game->player().health());
    if (font) font->render(buf, 20.0f, (float)h - 60.0f, {1, 1, 1, 1}, 2.0f);

    snprintf(buf, sizeof(buf), "EN: %.0f", game->player().energy());
    if (font) font->render(buf, 20.0f, (float)h - 40.0f, {0, 1, 1, 1}, 2.0f);

    // Messages
    double now = Engine::instance().timer().now();
    for (size_t i = 0; i < impl->messages.size(); i++) {
        auto& msg = impl->messages[i];
        double age = now - msg.second;
        if (age < 3.0) {
            float alpha = (float)(1.0 - age / 3.0);
            if (font) font->render(msg.first.c_str(), w * 0.5f - 100, h * 0.3f + i * 25,
                                   {1, 1, 1, alpha}, 2.0f);
        }
    }

    // Compass / direction indicator
    {
        float yaw = game->player().rotation().z;
        // Convert to degrees, 0 = north (+Z in T2)
        float deg = yaw * 180.0f / 3.14159f;
        const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        int dirIdx = ((int)((deg + 22.5f) / 45.0f) + 8) % 8;
        snprintf(buf, sizeof(buf), "%s", dirs[dirIdx]);
        if (font) font->render(buf, w * 0.5f - 10, 40.0f, {1, 1, 1, 0.7f}, 2.0f);
    }

    // Weapon info
    int32_t cw = game->player().currentWeapon();
    if (cw >= 0 && cw < game->player().weaponCount()) {
        const Weapon& w = game->player().weapon(cw);
        if (w.type >= 0 && w.type < gWeaponCount) {
            const WeaponData& wd = gWeaponTable[w.type];
            snprintf(buf, sizeof(buf), "%s", wd.name);
            if (font) font->render(buf, 20.0f, h - 120.0f, {1, 1, 0, 1}, 2.0f);

            if (wd.maxAmmo > 0) {
                snprintf(buf, sizeof(buf), "Ammo: %d/%d", w.ammo, wd.maxAmmo);
                if (font) font->render(buf, 20.0f, h - 100.0f, {1, 1, 1, 1}, 2.0f);
            }
        }
    }

    // Demo playback info
    if (game->isDemoPlaying()) {
        // Screen effects from demo game state
        float df = game->getDamageFlash();
        float wo = game->getWhiteOut();
        if (df > 0.0f) {
            float alpha = std::min(df * 0.5f, 0.6f);
            r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {0.8f, 0, 0, alpha});
        }
        if (wo > 0.0f) {
            float alpha = std::min(wo * 0.5f, 0.8f);
            r.drawBox({{0, 0, 0}, {(float)w, (float)h, 0}}, {1, 1, 1, alpha});
        }

        int mins = (int)(game->getDemoTime() / 60);
        int secs = (int)(game->getDemoTime()) % 60;
        int totalMins = (int)(game->getDemoTotalTime() / 60);
        int totalSecs = (int)(game->getDemoTotalTime()) % 60;
        const char* status = "> PLAY";
        if (game->isDemoPaused()) status = "|| PAUSE";
        else if (game->isDemoFastForward()) status = ">> FAST";
        ColorF statusColor = game->isDemoPaused() ? ColorF{1, 1, 0, 1} : ColorF{0, 1, 0, 1};
        // Progress bar (20 chars wide)
        float progress = game->getDemoTotalTime() > 0
            ? game->getDemoTime() / game->getDemoTotalTime() : 0;
        if (progress < 0) progress = 0;
        if (progress > 1) progress = 1;
        int barChars = (int)(progress * 20);
        char bar[32];
        for (int i = 0; i < 20; i++) bar[i] = (i < barChars) ? '#' : '.';
        bar[20] = 0;

        snprintf(buf, sizeof(buf), "[%s] [%s] %02d:%02d/%02d:%02d",
                 status, bar, mins, secs, totalMins, totalSecs);
        if (font) font->render(buf, 20.0f, 20.0f, statusColor, 2.0f);

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
            if (font) font->render(buf, 20.0f, 48.0f, {0.7f, 0.7f, 0.7f, 0.8f}, 2.0f);
            // Spectate target indicator
            int sidx = game->getControlGhostIndex(); // the private field isn't exposed, use controlGhostIndex
            // Actually, let's just access it through the game
            (void)sidx;
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
            const MatrixF& view = r.viewMatrix();
            const MatrixF& proj = r.projectionMatrix();
            auto indices = gt.getAllIndices();
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

    // Clean old messages
    impl->messages.erase(
        std::remove_if(impl->messages.begin(), impl->messages.end(),
            [now](auto& m) { return (now - m.second) > 3.0; }),
        impl->messages.end()
    );
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
    float fill = health / maxHealth;
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
    float fill = energy / maxEnergy;
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

void HUD::renderAmmo(int32_t current, int32_t max) {}

void HUD::renderScoreboard(Game* game) {
    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    if (!font) return;

    int32_t w = Engine::instance().platform().width();
    int32_t h = Engine::instance().platform().height();

    // Larger background for demo scoreboard
    float bw = 550.0f, bh = 400.0f;
    float bx = (w - bw) * 0.5f, by = (h - bh) * 0.5f;
    r.drawBox(Box3F{{bx, by, 0}, {bx + bw, by + bh, 0}}, {0, 0, 0, 0.7f});

    // Title
    if (font) font->render("SCOREBOARD", bx + 10, by + 10, {1, 1, 0, 1}, 2.0f);

    // Column headers
    float colX[] = {bx + 20, bx + 160, bx + 300, bx + 400, bx + 470};
    const char* headers[] = {"Player", "Team", "Skin", "Damage", "Health"};
    for (int i = 0; i < 5; i++) {
        if (font) font->render(headers[i], colX[i], by + 50, {1, 1, 1, 1}, 2.0f);
    }

    char buf[256];
    int row = 0;
    const int maxRows = 15;

    // Use demo player data if available
    if (game->isDemoPlaying()) {
        auto* dp = game->getDemoParser();
        if (dp) {
            const auto& players = dp->getPlayerInfo();
            for (auto& p : players) {
                if (row >= maxRows) break;
                float ry = by + 80 + row * 22;
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
                snprintf(buf, sizeof(buf), "%s", p.skin.c_str());
                if (font) font->render(buf, colX[2], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 2.0f);
                float dmgPct = (1.0f - p.damage) * 100.0f;
                snprintf(buf, sizeof(buf), "%.0f%%", p.damage * 100.0f);
                if (font) font->render(buf, colX[3], ry, {1, 0.5f, 0.2f, 0.9f}, 2.0f);
                ColorF hc = dmgPct > 66 ? ColorF{0,1,0,0.9f} : dmgPct > 33 ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f};
                snprintf(buf, sizeof(buf), "%.0f%%", dmgPct);
                if (font) font->render(buf, colX[4], ry, hc, 2.0f);
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
        auto indices = game->getLiveGhostIndices();
        row = 0;
        for (auto idx : indices) {
            if (row >= maxRows) break;
            auto* g = game->getLiveGhost(idx);
            if (!g || g->className != "Player") continue;
            float ry = by + 80 + row * 22;
            ColorF nameCol = (g->teamId == 1) ? ColorF{1,0.3f,0.3f,0.9f} :
                             (g->teamId == 2) ? ColorF{0.3f,0.4f,1,0.9f} :
                             ColorF{0.8f,0.8f,1,0.9f};
            snprintf(buf, sizeof(buf), "%s", g->playerName.empty() ? "Player" : g->playerName.c_str());
            if (font) font->render(buf, colX[0], ry, nameCol, 2.0f);
            const char* teamName = g->teamId == 1 ? "Red" : g->teamId == 2 ? "Blue" : "N/A";
            if (font) font->render(teamName, colX[1], ry, nameCol, 2.0f);
            snprintf(buf, sizeof(buf), "%d", g->kills);
            if (font) font->render(buf, colX[2], ry, {1,1,0,0.9f}, 2.0f);
            snprintf(buf, sizeof(buf), "%d", g->deaths);
            if (font) font->render(buf, colX[3], ry, {1,0.5f,0.2f,0.9f}, 2.0f);
            snprintf(buf, sizeof(buf), "%.0f", g->health);
            if (font) font->render(buf, colX[4], ry, {0,1,0,0.9f}, 2.0f);
            row++;
        }
        if (row == 0 && font)
            font->render("No live players", colX[0], by + 80, {0.5f,0.5f,0.5f,1}, 2.0f);
        return;
    }

    // Fallback: local player only (single-player / demo)
    row = 0;
    auto& p = game->player();
    snprintf(buf, sizeof(buf), "%s", game->config().playerName.c_str());
    if (font) font->render(buf, colX[0], by + 80 + row * 30, {0, 1, 0, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%.0f", p.score);
    if (font) font->render(buf, colX[1], by + 80 + row * 30, {1, 1, 1, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%d", p.kills);
    if (font) font->render(buf, colX[2], by + 80 + row * 30, {1, 1, 1, 1}, 2.0f);
    snprintf(buf, sizeof(buf), "%d", p.deaths);
    if (font) font->render(buf, colX[3], by + 80 + row * 30, {1, 1, 1, 1}, 2.0f);
}

void HUD::renderMessage(const char* text, float duration) {
    impl->messages.push_back({text, Engine::instance().timer().now()});
}

void HUD::showMessage(const char* text, const ColorF& color) {
    impl->messages.push_back({text, Engine::instance().timer().now()});
}

void HUD::addChatLine(const char* text) {
    impl->chatLines.push_back(text);
    if (impl->chatLines.size() > 50)
        impl->chatLines.erase(impl->chatLines.begin());
}

void Menu::init() {}

void Menu::update(float dt) {
    (void)dt;
    if (!active) return;

    auto& input = Engine::instance().platform().input();

    static bool prevUp = false, prevDown = false, prevEnter = false, prevEsc = false;
    bool up = input.keysDown[SCANCODE_UP];
    bool down = input.keysDown[SCANCODE_DOWN];
    bool enter = input.keysDown[SCANCODE_RETURN];
    bool esc = input.keysDown[SCANCODE_ESCAPE];

    if (currentScreen == Main) {
        const int itemCount = 5;
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
        if (esc && !prevEsc) { currentScreen = Main; selectedItem = 3; }
    }

    prevUp = up; prevDown = down; prevEnter = enter; prevEsc = esc;
}

void Menu::render() {
    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    int32_t w = Engine::instance().platform().width();

    // Simple menu rendering
    if (!active) return;

    switch (currentScreen) {
        case Main: {
            const char* title = "TRIBES 2";
            if (font) font->render(title, w * 0.5f - 80, 100, {1, 1, 0, 1}, 2.0f);

            const char* items[] = {"Start Local Game", "Server Browser", "Settings", "Controls", "Quit"};
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
            const char* binds[] = {
                "WASD / Arrows", "Move",
                "Mouse", "Look",
                "Space", "Jump",
                "Shift", "Jet",
                "Left Click", "Fire",
                "Right Click", "Alt Fire",
                "R", "Reload / Spectate",
                "F1", "Free Camera",
                "F2", "Orbit Camera",
                "P", "Pause Demo",
                "E", "Demo Events",
                "Tab", "Scoreboard",
                "ESC", "Pause / Menu",
                "~", "Console",
                nullptr, nullptr
            };
            int sy = 80;
            for (int i = 0; binds[i]; i += 2) {
                if (font) {
                    font->render(binds[i], 40, sy, {1, 1, 0, 1}, 2.0f);
                    font->render(binds[i+1], 250, sy, {1, 1, 1, 1}, 2.0f);
                }
                sy += 24;
            }
            break;
        }
        default: break;
    }
}
