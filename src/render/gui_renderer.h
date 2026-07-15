#pragma once
#include "render/renderer.h"
#include "net/network.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <set>

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
    bool selected = false;
    bool checked = false;
    bool hovered = false;
    int groupNum = 0;
    int cursorPos = 0; // caret position for text edit controls
    std::vector<GuiControl*> children;
    GuiControl* parent = nullptr;
    std::function<void()> onClick;

    // Scroll state (for GuiScrollCtrl and similar)
    // scrollY=0 = at bottom (newest), scrollY=maxScroll = at top (oldest)
    float scrollX = 0, scrollY = 0;
    float contentW = 0, contentH = 0; // virtual content size

    // Tab group fields (for ShellTabGroupCtrl/GM_TabView/LaunchTabView)
    struct Tab { std::string text; bool active; };
    std::vector<Tab> tabs;
    int selectedTab = -1;

    // ShellTextList / GuiListBoxCtrl fields
    std::vector<std::string> listRows;
    int selectedRow = -1;

    // ShellLaunchMenu popup fields
    struct MenuItem { int id; std::string text; bool isSeparator; };
    std::vector<MenuItem> menuItems;
    bool menuOpen = false;

    // GuiServerBrowser fields
    struct ServerBrowserColumn {
        std::string name;
        float width;
        bool sortable;
    };
    std::vector<ServerBrowserColumn> sbColumns;
    std::vector<NetworkManager::ServerInfo> sbServers; // displayed/cached list
    int sbSortCol = -1;
    bool sbSortInc = true;
    int sbSelected = -1;
    double sbLastQueryTime = 0;

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
    bool handleScroll(int x, int y, int wheelDelta);
    GuiControl* hitTest(GuiControl* ctl, int mx, int my);

    GuiControl* getCanvas() { return canvas; }
    GuiControl* findControl(const std::string& name);
    GuiControl* soToGui(const std::string& name, GuiControl* parent);
    void callOnAddOnce(GuiControl* ctl);
    void pushDialog(const std::string& name);
    void popDialog(const std::string& name);
    void setContent(const std::string& name);
    void handleKeyboard(); // process keyboard input for focused text control
    bool isDialogActive(const std::string& name);
    GuiControl* activeDialog() { return dialogStack.empty() ? canvas : dialogStack.back(); }
    void update(float dt); // process scheduled events
    void addSchedule(double delay, const std::string& command);
    size_t dialogCount() const { return dialogStack.size(); }
    GuiControl* getFocused() const { return focusedCtrl; }
    void setFocused(GuiControl* c) { focusedCtrl = c; }
    FadeState* getFadeState(const GuiControl* ctl, bool createIfMissing);

private:
    GuiControl* canvas{};
    GuiControl* focusedCtrl = nullptr;
    std::vector<GuiControl*> dialogStack;

    // Scheduler
    struct ScheduledEvent {
        double triggerTime;
        std::string command;
    };
    std::vector<ScheduledEvent> events;
    void renderControl(GuiControl* ctl);

    // Fade animation tracking for GuiFadeinBitmapCtrl
    std::unordered_map<std::string, FadeState> fadeStates;
    void updateFades(float dt);

    Texture* checkerTex{};
    std::set<std::string> onAddCalled; // track which controls have received onAdd
};
