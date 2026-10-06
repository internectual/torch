#pragma once
#include "render/renderer.h"
#include "net/network.h"
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <set>
#include <array>
#include <memory>

struct ScriptObject;
struct GuiControl;
struct Font;
struct Texture;

// GuiEvent: the canvas mouse point (logical canvas coordinates), the
// modifier keys (SI_SHIFT 0x3, SI_CTRL 0xc) and the click count.
struct GuiEvent {
    int x = 0, y = 0;
    uint8_t modifier = 0;
    int clickCount = 1;
};

// The C++ side of an engine GUI class (its GuiControl virtuals). A control
// whose class has one gets wake/sleep, pre-render, render and the canvas's
// mouse events (with mouse lock and enter/leave) through it.
class GuiControlBehavior {
public:
    virtual ~GuiControlBehavior() = default;
    virtual bool onWake(GuiControl&) { return true; }
    virtual void onSleep(GuiControl&) {}
    virtual void onPreRender(GuiControl&) {}
    // x, y: the control's canvas position.
    virtual void onRender(GuiControl&, float x, float y) {}
    virtual void onMouseDown(GuiControl&, const GuiEvent&) {}
    virtual void onMouseUp(GuiControl&, const GuiEvent&) {}
    virtual void onMouseMove(GuiControl&, const GuiEvent&) {}
    virtual void onMouseDragged(GuiControl&, const GuiEvent&) {}
    virtual void onMouseEnter(GuiControl&, const GuiEvent&) {}
    virtual void onMouseLeave(GuiControl&, const GuiEvent&) {}
    virtual void onRightMouseDown(GuiControl&, const GuiEvent&) {}
    virtual void onRightMouseUp(GuiControl&, const GuiEvent&) {}
    virtual void onRightMouseDragged(GuiControl&, const GuiEvent&) {}
    bool awake = false;
    // GuiControl::awaken ran and onWake failed: not tried again until the
    // control leaves the screen.
    bool wakeFailed = false;
};

namespace GuiBehaviors {
using Factory = std::function<std::shared_ptr<GuiControlBehavior>()>;
void registerClass(const std::string& className, Factory factory);
std::shared_ptr<GuiControlBehavior> create(const std::string& className);
} // namespace GuiBehaviors

// Shared GUI resources (dgl / GuiControlProfile helpers).
namespace GuiShared {
// TextureHandle(name): textures/<name>.
Texture* bitmap(const std::string& name);
Font* profileFont(const std::string& profile);
Font* font(const std::string& face, int size);
// GuiControlProfile::mFontColors[index] (fontColor, fontColorHL, fontColorNA,
// fontColorSEL, fontColors[4..9]); black when unset.
ColorF profileFontColor(const std::string& profile, int index);
// The control's field (its script object's, else its .gui value).
std::string field(const GuiControl& ctl, const char* name, const std::string& fallback = std::string());
// The logical canvas rectangle as GL window pixels (x, y from the bottom).
void canvasToWindow(float x, float y, float w, float h, int out[4]);
// GuiControl::createBitmapArray: numStates x numBitmaps rects (x, y, w, h),
// empty when the bitmap does not split.
std::vector<std::array<int, 4>> createBitmapArray(Texture* texture, int numStates, int numBitmaps);
// GuiCanvas::mouseLock / mouseUnlock.
void mouseLock(GuiControl* ctl);
void mouseUnlock(GuiControl* ctl);
// Canvas->mCursor = the named GuiCursor.
void setCanvasCursor(const std::string& cursor);
// GuiControl::localToGlobalCoord (the control's canvas position).
void canvasPosition(const GuiControl& ctl, float& x, float& y);
} // namespace GuiShared

namespace GuiBitmapStatePolicy {
struct HorizontalSlice { float x = 0.0f, width = 0.0f; };

inline int horizontalStateCount(const std::string& bitmap, int width, int height) {
    if (width <= 0 || height <= 0 || width <= height * 2) return 1;
    const size_t slash = bitmap.find_last_of("/\\");
    std::string name = bitmap.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    for (char& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    // The close control has three 35x29 cells; a height-ratio estimate
    // rounds its 105x29 strip up to four and cuts across adjacent icons.
    if (name == "shll_menuclose") return 3;
    return std::max(1, (width + height / 2) / height);
}

inline HorizontalSlice horizontalSlice(int width, int states, int state) {
    if (width <= 0 || states <= 0) return {};
    if (state < 0) state = 0;
    if (state >= states) state = states - 1;
    const float cellWidth = static_cast<float>(width) / states;
    // Keep linear filtering inside the selected atlas cell; sampling exactly
    // at a state boundary blends its edge with the neighboring close icon.
    return {state * cellWidth + 0.5f, cellWidth - 1.0f};
}
} // namespace GuiBitmapStatePolicy

struct GuiControl {
    std::string name;
    std::string className;
    float posX = 0, posY = 0;
    float extentX = 100, extentY = 30;
    float minExtentX = 8, minExtentY = 8;
    std::string text;
    std::string bitmap;
    float hudValue = 0.0f;
    bool hudValueSet = false;
    struct HudSlot {
        std::string bitmap;
        std::string name;
        int amount = -1;
        bool visible = false;
        bool active = false;
    };
    std::vector<HudSlot> hudSlots;
    int activeHudSlot = -1;
    std::vector<float> netHistory;
    uint64_t netLastReceived = 0;
    uint64_t netLastSent = 0;
    double netLastSample = 0.0;
    std::string command;  // TS command to execute when activated
    std::string altCommand; // alternate command (Enter in text fields)
    std::string profileName; // GuiControlProfile name
    bool visible = true;
    bool active = true;
    bool selected = false;
    bool checked = false;
    bool hovered = false;
    bool previousHovered = false;
    int hoveredTab = -1; // index of tab under mouse within a ShellTabGroupCtrl
    int hoveredItem = -1; // index of item under mouse within an open ShellLaunchMenu popup
    int groupNum = 0;
    int id = 0; // T2 GuiControl id (radio button value written to variable on click)
    int cursorPos = 0; // caret position for text edit controls
    std::vector<GuiControl*> children;
    GuiControl* parent = nullptr;
    std::function<void()> onClick;

    // Scroll state (for GuiScrollCtrl and similar)
    // scrollY=0 = at bottom (newest), scrollY=maxScroll = at top (oldest)
    float scrollX = 0, scrollY = 0;
    float contentW = 0, contentH = 0; // virtual content size
    std::string hScrollBarMode, vScrollBarMode; // "dynamic" / "alwaysOn" / "never"

    // Tab group fields (for ShellTabGroupCtrl/GM_TabView/LaunchTabView)
    struct Tab { std::string text; bool active; int id = 0; int set = 0; };
    std::vector<Tab> tabs;
    int selectedTab = -1;
    // T2 tab sets: addSet(id, bitmapBase, ...) gives tabs in that set their
    // own skin (e.g. set 1 -> gui/shll_horztabbuttonB olive variant)
    std::map<int, std::string> tabSets;

    // ShellTextList / GuiListBoxCtrl fields
    std::vector<std::string> listRows;
    std::vector<int> listRowIds; // parallel ids: global mission id / row id per displayed row
    int selectedRow = -1;
    struct ListColumn {
        int id = 0;
        std::string name;
        float width = 0;
        float minWidth = 0;
        float maxWidth = 0;
        std::string format;
    };
    std::vector<ListColumn> listColumns;

    struct TreeItem {
        int id = 0;
        int parent = 0;
        std::string text;
        std::string data;
        bool expanded = false;
    };
    std::vector<TreeItem> treeItems;
    int selectedTreeItem = 0;
    int nextTreeItemId = 1;

    // ShellLaunchMenu popup fields
    struct MenuItem { int id; std::string text; bool isSeparator; };
    std::vector<MenuItem> menuItems;
    bool menuOpen = false;

    // Lifecycle flag: true if this dialog was pushed during the content's onWake.
    // ESC should never pop base dialogs (e.g. LaunchToolbarDlg) so the sidebar stays.
    bool isBaseDialog = false;


    // Generic named-field storage set from script (e.g. altColor on ShellTabFrame)
    std::map<std::string, std::string> fields;

    // GuiPlayerView mouse-drag rotation
    bool modelRotating = false;
    int lastDragX = -1, lastDragY = -1;
    bool modelZooming = false;

    // ShellSliderCtrl fields
    float sliderValue = 0.5f;  // 0..1 normalized
    float sliderMin = 0.0f, sliderMax = 1.0f;
    int sliderTicks = 0;  // 0 = continuous
    bool sliderDragging = false;
    bool usePlusMinus = false;
    std::string variable; // $pref::* variable binding

    // ShellWindowCtrl fields
    bool windowDragging = false;
    float dragOffsetX = 0, dragOffsetY = 0;

    // Scrollbar thumb drag state
    bool vThumbDragging = false;
    bool hThumbDragging = false;
    float vThumbDragStartY = 0;  // screen Y at drag start
    float vThumbStartScrollY = 0;
    float hThumbDragStartX = 0;  // screen X at drag start
    float hThumbStartScrollX = 0;

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

    // GuiPlayerView fields
    std::string modelShape;   // shape name (e.g. "light_male", "bioderm_medium")
    std::string modelSkin;    // skin name override
    float modelYaw = 0.5f;    // orbit rotation
    float modelPitch = 0.15f;
    float modelZoom = 1.0f;
    int modelSequence = -1;
    float modelAnimTime = 0.0f;

    // The engine class behind the control, if it has one.
    std::shared_ptr<GuiControlBehavior> behavior;

    GuiControl* findChild(const std::string& name);
    void addChild(GuiControl* child);
    bool owns(const GuiControl* node) const {
        if (!node) return false;
        for (auto* current = node; current; current = current->parent)
            if (current == this) return true;
        return false;
    }
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
    bool handleSecondaryInput(int x, int y);
    bool handleDrag(int x, int y); // continuous mouse-move while button held
    void handleDragRelease(); // stop all dragging on mouse-up
    bool handleScroll(int x, int y, int wheelDelta);
    // Convert SDL window coordinates to the logical GUI canvas used for hit tests.
    void mapMouse(int physicalX, int physicalY, int& logicalX, int& logicalY) const;
    GuiControl* hitTest(GuiControl* ctl, int mx, int my);
    GuiControl* hitTestTop(int mx, int my); // dialogStack (top-down) then canvas
    GuiControl* launchPopupAt(int mx, int my); // open ShellLaunchMenu popup containing (mx,my)
    GuiControl* popupMenuAt(int mx, int my); // open GuiPopUpMenuCtrl dropdown item at (mx,my)

    // Shared layout math so rendering, hit-testing and hover agree exactly.
    static void tabLayoutParams(const GuiControl* grp, float& maxTabW, float& tabSpacing);
    static void launchPopupGeometry(const GuiControl* lm, float x, float y,
                                    float& popX, float& popY, float& popW, float& popH, float& lineH);
    // Launch-toolbar style (lnch_Tab skin): compact, text-sized buttons with
    // a state dot — unlike fixed-width shll_horztabbutton tab books.
    static bool launchStyleTabs(const GuiControl* grp);
    static float launchTabWidth(const GuiControl* grp, int ti, float maxTabW);

    GuiControl* getCanvas() { return canvas; }
    GuiControl* findControl(const std::string& name);
    bool removeControl(const std::string& name);
    // T2 ActionMap key-name <-> SDL3 scancode mapping (see gui_renderer.cpp).
    static const char* scancodeToKeyName(int sc);
    static int keyNameToScancode(const std::string& name);
    GuiControl* soToGui(const std::string& name, GuiControl* parent);
    void callOnAddOnce(GuiControl* ctl);
    void pushDialog(const std::string& name);

    void popDialog(const std::string& name);
    void clearDialogs();
    bool makeFirstResponder(const std::string& name, bool focus);
    std::vector<GuiControl*>& dialogStackForDebug() { return dialogStack; }
    // GuiCanvas cursor state: cursorOn/cursorOff, showCursor/hideCursor,
    // setCursor (the default GuiCursor), updateCursorState.
    void setCursorOn(bool on) { cursorOn_ = on; }
    bool isCursorOn() const { return cursorOn_; }
    void setShowCursor(bool show) { showCursor_ = show; }
    void setDefaultCursor(const std::string& cursor) { defaultCursor_ = cursor; }
    void updateCursorState();
    // GuiCanvas mouse dispatch to behaviour controls: true when such a
    // control took the event (or holds the mouse lock).
    bool dispatchBehaviorMouse(int x, int y, bool left, bool right, uint8_t modifier);
    // A visible GameTSCtrl (PlayGui) in the canvas content: the 3D world
    // is drawn only through one.
    bool contentShowsWorld() const;

    // Last instance pushed under each dialog name. A popped dialog leaves
    // the stack but stays alive; re-pushing it must reuse THAT object (with
    // its accumulated child state, e.g. LaunchTabView's tabs) rather than
    // building a fresh empty one.
    std::unordered_map<std::string, GuiControl*> lastPushed;
    // Responder to restore when a transient dialog is removed.
    std::unordered_map<GuiControl*, GuiControl*> focusBeforeDialog;
    void setContent(const std::string& name);
    void setContentImmediate(const std::string& name);
    void handleKeyboard(); // process keyboard input for focused text control
    bool isDialogActive(const std::string& name);
    // If the topmost dialog contains a GuiInputCtrl (e.g. RemapDlg), return it
    // so the "~" console toggle and other global keys can be suppressed while
    // the user chooses a binding. Returns null when not capturing.
    GuiControl* activeKeyCapture() const;
    GuiControl* activeDialog() { return dialogStack.empty() ? canvas : dialogStack.back(); }
    GuiControl* getDialog(size_t i) const { return i < dialogStack.size() ? dialogStack[i] : nullptr; }
    void update(float dt); // process scheduled events
    void addSchedule(double delay, const std::string& command);
    size_t dialogCount() const { return dialogStack.size(); }
    GuiControl* getFocused() const { return focusedCtrl; }
    bool inBaseDialogPush = false;
    void setFocused(GuiControl* c) { focusedCtrl = c; }
    FadeState* getFadeState(const GuiControl* ctl, bool createIfMissing);

private:
    GuiControl* canvas{};
    GuiControl* focusedCtrl = nullptr;
    GuiControl* pressedCtrl = nullptr;
    GuiControl* selectedList = nullptr;
    std::vector<GuiControl*> dialogStack;
    bool cursorOn_ = true, showCursor_ = true;
    std::string defaultCursor_;
    bool settingsWindowBorderCaptured_ = false;
    bool settingsWindowWasBordered_ = false;
    void restoreSettingsWindowBorder();

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

    std::set<std::string> onAddCalled; // track which controls have received onAdd
};
