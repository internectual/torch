#pragma once
#include "render/renderer.h"
#include <string>
#include <vector>
#include <functional>

struct ScriptObject;

struct GuiControl {
    std::string name;
    std::string className;
    float posX = 0, posY = 0;
    float extentX = 100, extentY = 30;
    float minExtentX = 8, minExtentY = 8;
    std::string text;
    std::string bitmap;
    bool visible = true;
    bool active = true;
    std::vector<GuiControl*> children;
    GuiControl* parent = nullptr;
    std::function<void()> onClick;

    GuiControl* findChild(const std::string& name);
    void addChild(GuiControl* child);
};

class GuiRenderer {
public:
    GuiRenderer();
    ~GuiRenderer();

    void init();
    void render();
    bool handleInput(int x, int y, bool pressed);

    GuiControl* getCanvas() { return canvas; }
    GuiControl* findControl(const std::string& name);

private:
    GuiControl* canvas{};
    void renderControl(GuiControl* ctl);
    GuiControl* hitTest(GuiControl* ctl, int x, int y);

    Texture* checkerTex{};
};
