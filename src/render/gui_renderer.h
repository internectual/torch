#pragma once
#include "render/renderer.h"
#include <string>
#include <vector>
#include <unordered_map>
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

struct FadeState {
    double elapsed = 0.0;       // accumulated fade time in seconds
    float fadeTime = 2.0;       // total fade duration in seconds
    bool fadeOut = true;        // fade out after fading in
    bool done = false;          // animation complete
    float currentAlpha = 0.0;   // current opacity (0=transparent, 1=opaque)
};

class GuiRenderer {
public:
    GuiRenderer();
    ~GuiRenderer();

    void init();
    void refresh();
    void render();
    bool handleInput(int x, int y, bool pressed);

    GuiControl* getCanvas() { return canvas; }
    GuiControl* findControl(const std::string& name);
    GuiControl* soToGui(const std::string& name, GuiControl* parent);
    void pushDialog(const std::string& name);
    void popDialog(const std::string& name);
    void setContent(const std::string& name);
    bool isDialogActive(const std::string& name);
    GuiControl* activeDialog() { return dialogStack.empty() ? canvas : dialogStack.back(); }
    void update(float dt); // process scheduled events
    void addSchedule(double delay, const std::string& command);
    size_t dialogCount() const { return dialogStack.size(); }
    FadeState* getFadeState(const GuiControl* ctl, bool createIfMissing);

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

    // Fade animation tracking for GuiFadeinBitmapCtrl
    std::unordered_map<std::string, FadeState> fadeStates;
    void updateFades(float dt);

    Texture* checkerTex{};
};
