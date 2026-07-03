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
    std::string command;  // TS command to execute when activated
    std::string altCommand; // alternate command (Enter in text fields)
    std::string profileName; // GuiControlProfile name
    bool visible = true;
    bool active = true;
    std::vector<GuiControl*> children;
    GuiControl* parent = nullptr;
    std::function<void()> onClick;

    // Scroll state (for GuiScrollCtrl and similar)
    // scrollY=0 = at bottom (newest), scrollY=maxScroll = at top (oldest)
    float scrollX = 0, scrollY = 0;
    float contentW = 0, contentH = 0; // virtual content size

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
    void pushDialog(const std::string& name);
    void popDialog(const std::string& name);
    bool isDialogActive(const std::string& name);
    GuiControl* activeDialog() { return dialogStack.empty() ? canvas : dialogStack.back(); }
    void update(float dt); // process scheduled events
    void addSchedule(double delay, const std::string& command);
    size_t dialogCount() const { return dialogStack.size(); }

private:
    GuiControl* canvas{};
    std::vector<GuiControl*> dialogStack;

    // Scheduler
    struct ScheduledEvent {
        double triggerTime;
        std::string command;
    };
    std::vector<ScheduledEvent> events;
    void renderControl(GuiControl* ctl);
    GuiControl* hitTest(GuiControl* ctl, int x, int y);

    Texture* checkerTex{};
};
