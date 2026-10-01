#pragma once
// GuiCommanderMap (the shipped client's commander map): a top-down view of
// the terrain, interiors and water through the real scene, with an icon per
// client target and HUD target (waypoints and tasks), sensor ranges, the
// mission area, selection and the script's onSelect / issueCommand.
// GuiCommanderTree (commander_tree.cpp) drives it through the entry points
// below.
#include "render/gui_renderer.h"
#include "sim/client_targets.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct Texture;
class TorqueScript;

// CommanderIconImage / CommanderIconData (game/commanderMapIcon.cc).
namespace CommanderIcons {

struct Image {
    enum Type { Static, Animation };
    enum AnimationType { Looping, FlipFlop, OneShot };
    int type = Static;
    bool overlay = false, modulate = true;
    Texture* texture = nullptr;
    std::vector<Texture*> frames; // the DML's MaterialList
    int animationType = Looping;
    int animationSpeed = 100;
    bool getFrameSize(int& w, int& h, uint32_t frame) const;
};

// Base, Active, Inactive, Select, Hilight.
enum { Base, Active, Inactive, Select, Hilight, NumImages };
struct Icon {
    std::array<const Image*, NumImages> images{};
};

// A CommanderIconData script object by name (the client's own icons).
const Icon* fromObject(const std::string& name);
// A received CommanderIconData datablock.
const Icon* fromDataBlock(uint32_t id);

} // namespace CommanderIcons

class CommanderMap : public GuiControlBehavior, public HUDTargetList::Notify {
public:
    // Entry::flags.
    enum : uint32_t {
        PlayerFlag = 0x2, AssignedTaskFlag = 0x4, PotentialTaskFlag = 0x8, WaypointFlag = 0x10,
        InitFlag = 0x20, ProjectedFlag = 0x40, ControlFlag = 0x80, SelectedFlag = 0x100,
        HilightFlag = 0x200, TypeVisibleFlag = 0x400, FriendlyFlag = 0x800, VisibleFlag = 0x1000,
        AnimRestartFlag = 0x2000, DamagedFlag = 0x4000, DestroyedFlag = 0x8000,
        PositionFlag = 0x10000, TaskTargetFlag = 0x20000,
    };
    enum { NumEntries = 0x220, HudBase = 0x200 };
    struct Entry {
        bool used = false;
        int kind = 0;                 // 0 client target, 1 HUD target
        uint32_t flags = 0;
        float pos[3] = {0, 0, 0};
        int sensorGroup = 0;
        const CommanderIcons::Icon* icon = nullptr;
        int ghost = -1;               // kind 0: the target object's ghost
        ClientTargetObject* clientTarget = nullptr; // kind 1
        bool hasObject = false;
        int sx = 0, sy = 0;
        int rect[4] = {0, 0, 0, 0};   // icon rect x, y, w, h
        uint32_t typeTag = 0;
        uint32_t selTime = 0;
        float scale = 1.0f, alpha = 1.0f;
        int id = -1;
        int hudRef = -1;
        std::vector<float> fan;       // sensor fan points (x, y, z)
        float fanPos[3] = {0, 0, 0};
        bool fanValid = false;
        int prev = -1, next = -1;
        uint32_t targetChanges = 0;
    };

    CommanderMap();
    ~CommanderMap() override;
    // The awake map named "CommanderMap", if any.
    static CommanderMap* find();

    bool onWake(GuiControl&) override;
    void onSleep(GuiControl&) override;
    void onPreRender(GuiControl&) override;
    void onRender(GuiControl&, float x, float y) override;
    void onMouseDown(GuiControl&, const GuiEvent&) override;
    void onMouseUp(GuiControl&, const GuiEvent&) override;
    void onMouseMove(GuiControl&, const GuiEvent&) override;
    void onMouseDragged(GuiControl&, const GuiEvent&) override;
    void onMouseEnter(GuiControl&, const GuiEvent&) override;
    void onMouseLeave(GuiControl&, const GuiEvent&) override;
    void onRightMouseDown(GuiControl&, const GuiEvent&) override;
    void onRightMouseUp(GuiControl&, const GuiEvent&) override;
    void onRightMouseDragged(GuiControl&, const GuiEvent&) override;

    // HUDTargetListNotify.
    void hudTargetAdded(uint32_t id) override;
    void hudTargetRemoved(uint32_t id) override;
    void hudTargetsCleared() override;

    // The C++ entry points (GuiCommanderTree and the console methods).
    Entry* findEntry(int id);
    bool selectTarget(int id, bool sel, bool notify);
    bool hilightTarget(int id, bool hil);
    void clearSelection(bool keepPlayers);
    void clearHilight();
    void followLastSelected();
    void resetCamera();
    bool selectControlObject();
    bool selectClientTarget(ClientTargetObject* target, bool sel);
    void setTypeVisible(uint32_t typeTag, bool vis);
    bool isTypeVisible(uint32_t typeTag) const;
    void setCameraMove(uint32_t bit, bool on);
    void setMouseMode(int mode);
    int mouseMode() const { return mouseMode_; }
    GuiControl* control() const { return ctl_; }

    uint32_t waypointType() const { return waypointType_; }
    uint32_t locationType() const { return locationType_; }
    uint32_t potentialTaskType() const { return potentialTaskType_; }
    uint32_t assignedTaskType() const { return assignedTaskType_; }

private:
    GuiControl* ctl_ = nullptr;
    std::array<Entry, NumEntries> entries_{};
    int head_ = -1, tail_ = -1;
    int lastSelected_ = -1, followId_ = -1, rightClickId_ = -1;
    uint32_t moveBits_ = 0;
    uint32_t mouseState_ = 0;
    int mouseMode_ = 0;
    GuiEvent curEvent_, lastEvent_;
    int selectRect_[4] = {0, 0, 1, 1};
    int dragStart_[2] = {0, 0};
    bool grabValid_ = false;
    float grabPoint_[3] = {0, 0, 0};
    // Camera (Torque space).
    float boxMin_[3] = {0, 0, 0}, boxMax_[3] = {0, 0, 0};
    float desiredZ_ = 500.0f;
    float camPos_[3] = {0, 0, 500.0f};
    float fov_ = 90.0f;
    float ceiling_ = 0.0f;
    uint32_t lastTime_ = 0;
    bool haveTerrain_ = false;
    float lastExtent_[4] = {0, 0, 0, 0};
    // The projection the frame was drawn with (canvas logical coordinates).
    float viewProj_[16] = {};
    float viewInv_[16] = {};
    float viewport_[4] = {0, 0, 1, 1};
    uint32_t waypointType_ = 0, locationType_ = 0, potentialTaskType_ = 0, assignedTaskType_ = 0;
    std::vector<uint32_t> visibleTypes_;
    const CommanderIcons::Icon* defaultIcon_ = nullptr;
    const CommanderIcons::Icon* waypointIcon_ = nullptr;
    const CommanderIcons::Icon* assignedTaskIcon_ = nullptr;
    const CommanderIcons::Icon* potentialTaskIcon_ = nullptr;
    std::string cursors_[7];
    Texture* edgeTexture_ = nullptr;

    void insert(int index, bool atTail);
    void unlink(int index);
    void resetEntry(Entry& e);
    void targetAdded(int id);
    void targetRemoved(int id);
    void syncTargets();
    void updateEntries();
    void updateHilight();
    bool project(Entry& e);
    bool projectPoint(const float world[3], float out[3]) const;
    void unprojectPoint(const float screen[3], float out[3]) const;
    void setSelected(Entry& e, bool sel, bool notify);
    void setHilighted(Entry& e, bool hil);
    Entry* hitTest(int x, int y);
    void computeHeightBounds();
    void computeMaxZ();
    void clampCamera();
    void updateCamera();
    void updateMouseDrag();
    bool objectPosition(const Entry& e, float out[3]) const;
    void cursorForMode();
    void renderSensors();
    void renderMissionArea();
    void renderIcons(float x, float y);
    void drawIcon(const CommanderIcons::Image* img, const int rect[4], const ColorF& color, uint32_t time);
    bool missionArea(float out[4]) const;
    float fieldF(const char* name, float fallback) const;
    int fieldI(const char* name, int fallback) const;
    bool fieldB(const char* name, bool fallback) const;
    ColorF fieldColor(const char* name, ColorF fallback) const;
};

void registerCommanderNatives(TorqueScript& ts);
void registerCommanderTreeNatives(TorqueScript& ts);
