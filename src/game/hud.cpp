#include "game/hud.h"
#include "game/game.h"
#include "render/renderer.h"
#include "core/engine.h"
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

    // Crosshair
    renderCrosshair();

    // Health
    renderHealthBar(game->player().health(), 100.0f);

    // Energy
    renderEnergyBar(game->player().energy(), 100.0f);

    // HUD text
    char buf[128];
    snprintf(buf, sizeof(buf), "HP: %.0f", game->player().health());
    if (font) font->render(buf, 20.0f, h - 60.0f, {1, 1, 1, 1}, 1.0f);

    snprintf(buf, sizeof(buf), "EN: %.0f", game->player().energy());
    if (font) font->render(buf, 20.0f, h - 40.0f, {0, 1, 1, 1}, 1.0f);

    // Messages
    double now = Engine::instance().timer().now();
    for (size_t i = 0; i < impl->messages.size(); i++) {
        auto& msg = impl->messages[i];
        double age = now - msg.second;
        if (age < 3.0) {
            float alpha = (float)(1.0 - age / 3.0);
            if (font) font->render(msg.first.c_str(), w * 0.5f - 100, h * 0.3f + i * 25,
                                   {1, 1, 1, alpha}, 1.0f);
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
        if (font) font->render(buf, w * 0.5f - 10, 40.0f, {1, 1, 1, 0.7f}, 1.0f);
    }

    // Weapon info
    int32_t cw = game->player().currentWeapon();
    if (cw >= 0 && cw < game->player().weaponCount()) {
        const Weapon& w = game->player().weapon(cw);
        if (w.type >= 0 && w.type < gWeaponCount) {
            const WeaponData& wd = gWeaponTable[w.type];
            snprintf(buf, sizeof(buf), "%s", wd.name);
            if (font) font->render(buf, 20.0f, h - 120.0f, {1, 1, 0, 1}, 1.2f);

            if (wd.maxAmmo > 0) {
                snprintf(buf, sizeof(buf), "Ammo: %d/%d", w.ammo, wd.maxAmmo);
                if (font) font->render(buf, 20.0f, h - 100.0f, {1, 1, 1, 1}, 1.0f);
            }
        }
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
                snprintf(buf, sizeof(buf), "Ghosts: %d  [F2]orbit [A/D]rot [W/S]zoom [Spc/Shft]ht",
                         ghostCount);
            } else {
                snprintf(buf, sizeof(buf), "Ghosts: %d  [P]ause [.]step [F1]free [F2]orbit [E]vents [Tab]score",
                         ghostCount);
            }
            if (font) font->render(buf, 20.0f, 48.0f, {0.7f, 0.7f, 0.7f, 0.8f}, 1.4f);
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
                font->render(line, 20.0f, ey, col, 1.0f);
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
        font->render("[ESC] Resume  [Q] Quit", 350, 360, {0.7f, 0.7f, 0.7f, 0.8f}, 1.5f);
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
    int32_t w = Engine::instance().platform().width();
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
    int32_t w = Engine::instance().platform().width();
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
    float colX[] = {bx + 20, bx + 220, bx + 350, bx + 470};
    const char* headers[] = {"Player", "Skin", "Damage", "Health"};
    for (int i = 0; i < 4; i++) {
        if (font) font->render(headers[i], colX[i], by + 50, {1, 1, 1, 1}, 1.2f);
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
                snprintf(buf, sizeof(buf), "%s", p.name.c_str());
                if (font) font->render(buf, colX[0], ry, col, 1.0f);
                snprintf(buf, sizeof(buf), "%s", p.skin.c_str());
                if (font) font->render(buf, colX[1], ry, {0.6f, 0.6f, 0.6f, 0.8f}, 1.0f);
                float dmgPct = (1.0f - p.damage) * 100.0f;
                snprintf(buf, sizeof(buf), "%.0f%%", p.damage * 100.0f);
                if (font) font->render(buf, colX[2], ry, {1, 0.5f, 0.2f, 0.9f}, 1.0f);
                ColorF hc = dmgPct > 66 ? ColorF{0,1,0,0.9f} : dmgPct > 33 ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f};
                snprintf(buf, sizeof(buf), "%.0f%%", dmgPct);
                if (font) font->render(buf, colX[3], ry, hc, 1.0f);
                row++;
            }
            if (row == 0 && font) {
                font->render("No player data available", colX[0], by + 80, {0.5f,0.5f,0.5f,1}, 1.2f);
            }
            return;
        }
    }

    // Fallback: local player only (default behavior)
    row = 0;
    auto& p = game->player();
    snprintf(buf, sizeof(buf), "%s", game->config().playerName.c_str());
    if (font) font->render(buf, colX[0], by + 80 + row * 30, {0, 1, 0, 1}, 1.2f);
    snprintf(buf, sizeof(buf), "%.0f", p.score);
    if (font) font->render(buf, colX[1], by + 80 + row * 30, {1, 1, 1, 1}, 1.2f);
    snprintf(buf, sizeof(buf), "%d", p.kills);
    if (font) font->render(buf, colX[2], by + 80 + row * 30, {1, 1, 1, 1}, 1.2f);
    snprintf(buf, sizeof(buf), "%d", p.deaths);
    if (font) font->render(buf, colX[3], by + 80 + row * 30, {1, 1, 1, 1}, 1.2f);
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
    // Menu logic
}

void Menu::render() {
    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();
    int32_t w = Engine::instance().platform().width();
    int32_t h = Engine::instance().platform().height();

    // Simple menu rendering
    if (!active) return;

    switch (currentScreen) {
        case Main: {
            const char* title = "TRIBES 2";
            if (font) font->render(title, w * 0.5f - 80, 100, {1, 1, 0, 1}, 2.0f);

            const char* items[] = {"Start Local Game", "Server Browser", "Settings", "Controls", "Quit"};
            for (int i = 0; i < 5; i++) {
                if (font) font->render(items[i], w * 0.5f - 80, 200 + i * 40, {1, 1, 1, 1}, 1.2f);
            }
            break;
        }
        case ServerBrowser: {
            if (font) font->render("Server Browser", 20, 20, {1, 1, 0, 1}, 1.5f);
            for (size_t i = 0; i < servers.size(); i++) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s | %s | %d/%d | %dms",
                    servers[i].name.c_str(), servers[i].map.c_str(),
                    servers[i].players, servers[i].maxPlayers, servers[i].ping);
                if (font) font->render(buf, 20, 80 + i * 30, {1, 1, 1, 1}, 1.0f);
            }
            break;
        }
        default: break;
    }
}
