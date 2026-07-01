#pragma once
#include "game/game.h"
#include "render/renderer.h"
#include <functional>
#include <vector>

class HUD {
public:
    HUD();
    ~HUD();

    void init();
    void render(Game* game);
    void renderCrosshair();
    void renderHealthBar(float health, float maxHealth);
    void renderEnergyBar(float energy, float maxEnergy);
    void renderAmmo(int32_t current, int32_t max);
    void renderScoreboard(Game* game);
    void renderMessage(const char* text, float duration = 3.0f);

    void showMessage(const char* text, const ColorF& color = {1,1,1,1});
    void addChatLine(const char* text);

    void setVisible(bool v) { visible = v; }
    bool isVisible() const { return visible; }

private:
    struct Impl;
    Impl* impl;
    bool visible = true;
};

class Menu {
public:
    Menu() = default;
    ~Menu() = default;

    enum Screen {
        Main,
        ServerBrowser,
        Settings,
        Controls,
        Credits
    };

    void init();
    void update(float dt);
    void render();

    void setScreen(Screen s) { currentScreen = s; }
    Screen screen() const { return currentScreen; }

    bool isActive() const { return active; }
    void setActive(bool a) { active = a; }

    struct MenuItem {
        const char* text;
        std::function<void()> action;
    };

    struct ServerEntry {
        std::string name;
        std::string map;
        std::string gameType;
        int32_t players;
        int32_t maxPlayers;
        int32_t ping;
    };

    std::vector<ServerEntry>& serverList() { return servers; }
    int32_t selectedServer() const { return selServer; }

private:
    Screen currentScreen = Main;
    bool active = true;
    int32_t selectedItem = 0;
    std::vector<ServerEntry> servers;
    int32_t selServer = 0;
};
