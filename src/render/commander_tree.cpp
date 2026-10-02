// GuiCommanderTree, from the shipped client: the commander screen's list of
// categories (Teammates, Tactical Assets, ..., Waypoints) and the targets in
// each, which selects, hilights and commands through the commander map.
#include "render/commander_map.h"
#include "core/console.h"
#include "core/engine.h"
#include "core/timer.h"
#include "game/demo.h"
#include "game/game.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/net_string_table.h"
#include <GL/glew.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <strings.h>

namespace {

uint32_t virtualMs() { return (uint32_t)(uint64_t)(Timer::now() * 1000.0); }
bool atob(const std::string& s) { return !strcasecmp(s.c_str(), "true") || std::atof(s.c_str()) != 0.0; }

DemoParser* connection() {
    Game& g = Engine::instance().game();
    DemoParser* parser = g.getDemoParser();
    return parser && (g.isLiveClient() || g.isDemoPlaying()) ? parser : nullptr;
}

uint32_t tagOf(const std::string& text) {
    return text.empty() ? 0 : NetStrings::tagId(NetStrings::literal(text));
}

const V12::DecodedDataBlock* dataBlock(int id) {
    DemoParser* parser = connection();
    if (!parser || id < 0) return nullptr;
    const auto& blocks = parser->getInitialBlock().dataBlocks;
    auto it = blocks.find((uint32_t)id);
    return it == blocks.end() ? nullptr : &it->second.decoded;
}

const DemoTargetState* clientTarget(int id) {
    DemoParser* parser = connection();
    if (!parser) return nullptr;
    auto it = parser->getTargets().find(id);
    return it == parser->getTargets().end() ? nullptr : &it->second;
}

// TargetManager::getGameName.
bool gameName(int id, std::string& out, size_t bufSize) {
    if (id < 0 || id >= 512) return false;
    const DemoTargetState* ti = clientTarget(id);
    if (!ti) return false;
    const bool hasName = !ti->name.empty() && ti->name[0] != '_', hasType = !ti->type.empty() && ti->type[0] != '_';
    std::string text = hasName ? (hasType ? ti->name + " " + ti->type : ti->name) : hasType ? ti->type : std::string();
    if (bufSize && text.size() > bufSize - 1) text.resize(bufSize - 1);
    out = text;
    return true;
}

// The ghost that owns a target (TargetInfo::targetObject).
const GhostEntry* targetObject(int id) {
    DemoParser* parser = connection();
    if (!parser || id < 32) return nullptr;
    const GhostTracker& tracker = parser->getGhostTracker();
    for (int index : tracker.getAllIndices())
        if (const GhostEntry* g = tracker.getGhost(index); g && g->targetId == id) return g;
    return nullptr;
}

ClientTargetObject* hudTarget(int id) {
    const int index = HUDTargetList::handleIndex(id);
    const HUDTargetList::Entry* he = index >= 0 ? HUDTargetList::entry((uint32_t)index) : nullptr;
    return he ? he->target : nullptr;
}

ColorF bytes(int r, int g, int b, int a) { return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f}; }

void drawRect(float x, float y, float w, float h, const ColorF& c) {
    Engine::instance().renderer().drawRectFill({x, y, 0}, {x + w, y + h, 0}, c);
}

void drawBitmap(Texture* tex, float x, float y, float w, float h, const ColorF& tint) {
    if (!tex) return;
    Engine::instance().renderer().drawTexturedRectUV({x, y, 0}, {x + w, y + h, 0}, tex->id, 0, 0, 1, 1, &tint);
}

class CommanderTree;

struct Category;

struct Cell {
    virtual ~Cell() = default;
    bool hilight = false;
    int y = 0;
    virtual void render(CommanderTree&, float, float, float, float) {}
    virtual void onMouseEnter(CommanderTree&, const GuiEvent&) {}
    virtual void onMouseLeave(CommanderTree&, const GuiEvent&) {}
    virtual void onMouseDown(CommanderTree&, const GuiEvent&) {}
    virtual void onRightMouseUp(CommanderTree&, const GuiEvent&) {}
    virtual bool getName(CommanderTree&, std::string&, size_t) { return false; }
};

struct EntryType {
    Category* category = nullptr;
    uint32_t typeTag = 0;
    Texture* icon = nullptr;
    ColorF color{1, 1, 1, 1};
    bool control = false;
};

// Entry::mFlags.
enum : uint32_t { HudFlag = 0x1, SelectedFlag = 0x2, CountedFlag = 0x4, SystemFlag = 0x8, ResolvedFlag = 0x10,
                  DamageFlag = 0x20, ControllableFlag = 0x40, ControlledFlag = 0x80 };

struct Entry : Cell {
    Category* category = nullptr;
    int targetId = 0;
    uint32_t typeTag = 0;
    uint32_t flags = 0;
    float damage = 0;
    Entry* next = nullptr;
    Entry* prev = nullptr;
    void onMouseDown(CommanderTree&, const GuiEvent&) override;
};

struct ClientNoneEntry : Entry {
    void render(CommanderTree&, float, float, float, float) override;
    void onMouseDown(CommanderTree&, const GuiEvent&) override;
    bool getName(CommanderTree&, std::string&, size_t) override;
};

struct TargetEntry : Entry {
    void render(CommanderTree&, float, float, float, float) override;
    void onMouseEnter(CommanderTree&, const GuiEvent&) override;
    void onMouseLeave(CommanderTree&, const GuiEvent&) override;
    void onMouseDown(CommanderTree&, const GuiEvent&) override;
    void onRightMouseUp(CommanderTree&, const GuiEvent&) override;
    bool getName(CommanderTree&, std::string&, size_t) override;
};

struct Category : Cell {
    int visibleHeight = 0;
    int numVisible = 0;
    int numEntries = 0;
    int type = 0; // 0 clients, 1 targets, 2 waypoints
    CommanderTree* tree = nullptr;
    std::string name, displayText;
    bool open = false;
    int animTime = 0;
    std::vector<EntryType*> types;
    Entry* head = nullptr;
    ~Category() override {
        for (Entry* e = head; e;) {
            Entry* n = e->next;
            delete e;
            e = n;
        }
    }
    void setOpen(bool open);
    void insert(Entry* e);
    void unlink(Entry* e);
    void render(CommanderTree&, float, float, float, float) override;
    void onMouseDown(CommanderTree&, const GuiEvent&) override { setOpen(!open); }
};

Entry* s_lastClicked = nullptr;

class CommanderTree : public GuiControlBehavior, public HUDTargetList::Notify {
public:
    GuiControl* ctl = nullptr;
    std::vector<Category*> categories;
    std::vector<EntryType*> entryTypes;
    std::array<Entry*, CommanderMap::NumEntries> entries{};
    std::array<uint32_t, 512> seenChanges{};
    std::array<bool, 512> seen{};
    bool mouseInside = false;
    GuiEvent lastEvent;
    uint32_t lastTime = 0;
    Cell* hilightCell = nullptr;
    Texture* backdrop = nullptr;
    Texture* header = nullptr;
    Texture* objectControl = nullptr;
    std::array<int, 4> controlSrc[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    Font* headerFont = nullptr;
    Font* entryFont = nullptr;
    Font* clientNoneFont = nullptr;
    bool connected = false;

    CommanderTree() {
        for (uint32_t i = 0; i < HUDTargetList::count(); ++i)
            if (const auto* he = HUDTargetList::entry(i)) hudTargetAdded((uint32_t)(he->handle + CommanderMap::HudBase));
    }
    ~CommanderTree() override { reset(); }

    CommanderMap* map() const { return connected ? CommanderMap::find() : nullptr; }

    std::string field(const char* name, const char* fallback) const {
        return ctl ? GuiShared::field(*ctl, name, fallback) : std::string(fallback);
    }
    int fieldI(const char* name, int fallback) const {
        const std::string v = ctl ? GuiShared::field(*ctl, name) : std::string();
        return v.empty() ? fallback : std::atoi(v.c_str());
    }
    void point(const char* name, int fx, int fy, int& x, int& y) const {
        x = fx, y = fy;
        const std::string v = ctl ? GuiShared::field(*ctl, name) : std::string();
        if (!v.empty()) std::sscanf(v.c_str(), "%d %d", &x, &y);
    }
    ColorF color(const char* name, ColorF fallback) const {
        const std::string v = ctl ? GuiShared::field(*ctl, name) : std::string();
        int r = 0, g = 0, b = 0, a = 255;
        if (v.empty() || std::sscanf(v.c_str(), "%d %d %d %d", &r, &g, &b, &a) < 3) return fallback;
        return bytes(r, g, b, a);
    }
    ColorF fontColor(int i) const { return ctl ? GuiShared::profileFontColor(ctl->profileName, i) : ColorF{0, 0, 0, 1}; }
    int headerHeight() const { return fieldI("headerHeight", 20); }
    int entryHeight() const { return fieldI("entryHeight", 20); }
    int categoryOpenTime() const { return fieldI("categoryOpenTime", 250); }
    std::string clientNoneText() const { return field("clientNoneText", "<None>"); }
    std::vector<int> damageColors() const {
        std::vector<int> out;
        const std::string v = field("damageColors", "0 255 0 20 255 255 0 70 255 0 0 100");
        std::istringstream in(v);
        int n;
        while (in >> n) out.push_back(n);
        return out;
    }

    Category* findCategory(const std::string& name) const {
        for (Category* c : categories)
            if (!strcasecmp(c->name.c_str(), name.c_str())) return c;
        return nullptr;
    }
    EntryType* findEntryType(uint32_t tag) const {
        for (EntryType* t : entryTypes)
            if (t->typeTag == tag) return t;
        return nullptr;
    }

    void reset() {
        for (Category* c : categories) delete c;
        categories.clear();
        for (EntryType* t : entryTypes) delete t;
        entryTypes.clear();
        entries.fill(nullptr);
        seen.fill(false);
        s_lastClicked = nullptr;
        hilightCell = nullptr;
    }

    bool addCategory(const std::string& name, const std::string& displayText, int type) {
        if (findCategory(name)) return false;
        auto* c = new Category;
        if (type == 0) {
            auto* none = new ClientNoneEntry;
            none->flags = SystemFlag;
            none->typeTag = NetStrings::add("CMDTREE__SystemEntryType");
            none->category = c;
            c->tree = this;
            c->insert(none);
        }
        c->type = type;
        c->tree = this;
        c->name = name;
        c->displayText = displayText;
        categories.push_back(c);
        return true;
    }

    bool registerEntryType(const std::string& category, uint32_t tag, bool control, const std::string& icon,
                           const ColorF* color) {
        Category* c = findCategory(category);
        if (!c || findEntryType(tag)) return false;
        auto* t = new EntryType;
        t->category = c;
        t->typeTag = tag;
        if (!icon.empty()) t->icon = GuiShared::bitmap(icon);
        if (color) t->color = *color;
        t->control = control;
        entryTypes.push_back(t);
        c->types.push_back(t);
        return true;
    }

    void addTarget(int id, uint32_t typeTag, bool isHud) {
        Category* cat = nullptr;
        bool control = false;
        if (!isHud) {
            const DemoTargetState* ti = clientTarget(id);
            const V12::DecodedDataBlock* sbd = ti ? dataBlock(ti->dataBlockId) : nullptr;
            if (sbd && !sbd->cmdMiniIconName.empty()) {
                registerEntryType(sbd->cmdCategory, typeTag, false, std::string(), nullptr);
                cat = findCategory(sbd->cmdCategory);
                if (cat && cat->open)
                    if (CommanderMap* m = map()) m->setTypeVisible(typeTag, true);
                control = sbd->shapeCanControl;
            }
        }
        if (!cat) {
            EntryType* et = findEntryType(typeTag);
            if (et && et->icon) control = et->control, cat = et->category;
        }
        if (!cat) return;
        auto* e = new TargetEntry;
        e->targetId = id;
        e->typeTag = typeTag;
        e->flags = (isHud ? HudFlag : 0) | (control ? ControllableFlag : 0);
        entries[id] = e;
        cat->insert(e);
        if (e->category->type == 2) e->flags |= CountedFlag;
    }

    void removeEntry(int id) {
        if (Entry* e = entries[id]) {
            e->category->unlink(e);
            entries[id] = nullptr;
            if (hilightCell == e) hilightCell = nullptr;
            if (s_lastClicked == e) s_lastClicked = nullptr;
            delete e;
        }
    }

    // TargetManagerNotify, from the client's target events.
    void syncTargets() {
        DemoParser* parser = connection();
        if (!parser) return;
        const auto& targets = parser->getTargets();
        for (int i = 0; i < 512; ++i) {
            auto it = targets.find(i);
            if (it == targets.end()) {
                if (seen[i]) {
                    seen[i] = false;
                    removeEntry(i);
                }
                continue;
            }
            if (!seen[i]) {
                seen[i] = true;
                seenChanges[i] = it->second.changes;
                addTarget(i, tagOf(it->second.type), false);
            } else if (seenChanges[i] != it->second.changes) {
                seenChanges[i] = it->second.changes;
                if (entries[i]) {
                    removeEntry(i);
                    addTarget(i, tagOf(it->second.type), false);
                }
            }
        }
    }

    void hudTargetAdded(uint32_t h) override {
        ClientTargetObject* ct = hudTarget((int)h);
        if (!ct || h >= entries.size()) return;
        uint32_t tag = 0;
        switch (ct->type) {
            case ClientTargetObject::AssignedTask: tag = NetStrings::add("CMDMAP__AssignedTaskType"); break;
            case ClientTargetObject::PotentialTask: tag = NetStrings::add("CMDMAP__PotentialTaskType"); break;
            case ClientTargetObject::Waypoint: tag = NetStrings::add("CMDMAP__WayPointType"); break;
        }
        addTarget((int)h, tag, true);
        if (tag) NetStrings::remove(tag);
    }
    void hudTargetRemoved(uint32_t h) override {
        if (h < entries.size()) removeEntry((int)h);
    }
    void hudTargetsCleared() override {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i] && (entries[i]->flags & HudFlag)) removeEntry((int)i);
    }

    bool updateEntries() {
        DemoParser* parser = connection();
        if (!parser) return false;
        syncTargets();
        CommanderMap* m = map();
        const int myGroup = parser->clientSensorGroup();
        for (Category* c : categories) {
            c->numVisible = 0;
            for (Entry* e = c->head; e; e = e->next) {
                e->flags &= ~SelectedFlag;
                if (!(e->flags & SystemFlag) && m)
                    if (CommanderMap::Entry* me = m->findEntry(e->targetId); me && (me->flags & CommanderMap::SelectedFlag))
                        e->flags |= SelectedFlag;
                e->flags &= ~CountedFlag;
                if (e->flags & (SystemFlag | HudFlag)) {
                    c->numVisible++;
                    e->flags |= CountedFlag;
                    continue;
                }
                const DemoTargetState* ti = clientTarget(e->targetId);
                if (!ti || ti->name.empty() || ti->type.empty()) continue;
                // TargetInfo::CommanderListRender (1 << NumHudRenderImages).
                if (ti->sensorGroup != myGroup && !(ti->renderFlags & 0x100)) continue;
                const GhostEntry* obj = targetObject(e->targetId);
                if (!(e->flags & ResolvedFlag) && obj) {
                    e->flags |= ResolvedFlag;
                    if (const V12::DecodedDataBlock* db = dataBlock(obj->datablockId)) {
                        e->flags = (e->flags & ~DamageFlag) | (!db->shapeIsInvincible ? DamageFlag : 0);
                        e->flags = (e->flags & ~ControllableFlag) | (db->shapeCanControl ? ControllableFlag : 0);
                    }
                }
                if ((e->flags & (DamageFlag | ControllableFlag)) && obj) {
                    const float maximum = obj->maxHealth > 0.0f ? obj->maxHealth : 100.0f;
                    e->damage = std::clamp(1.0f - obj->health / maximum, 0.0f, 1.0f);
                    e->flags &= ~ControlledFlag;
                }
                e->flags |= CountedFlag;
                c->numVisible++;
            }
        }
        return true;
    }

    void setHilightCell(Cell* c) {
        if (hilightCell == c) return;
        if (hilightCell) {
            hilightCell->hilight = false;
            hilightCell->onMouseLeave(*this, lastEvent);
        }
        hilightCell = nullptr;
        if (c) {
            c->hilight = true;
            hilightCell = c;
            c->onMouseEnter(*this, lastEvent);
        }
    }

    GuiEvent local(const GuiEvent& ev) const {
        GuiEvent out = ev;
        float x = 0, y = 0;
        if (ctl) GuiShared::canvasPosition(*ctl, x, y);
        out.x = ev.x - (int)x, out.y = ev.y - (int)y;
        return out;
    }

    bool onWake(GuiControl& c) override {
        ctl = &c;
        if (!connection()) return false;
        connected = true;
        if (!CommanderMap::find()) {
            connected = false;
            return false;
        }
        backdrop = GuiShared::bitmap(field("backdropBitmapName", "commander/gui/cmd_gradient"));
        header = GuiShared::bitmap(field("headerBitmapName", "commander/gui/cmd_columnheadbar"));
        headerFont = GuiShared::font(field("headerFontType", "Arial Bold"), fieldI("headerFontSize", 14));
        entryFont = GuiShared::font(field("entryFontType", "Arial"), fieldI("entryFontSize", 13));
        clientNoneFont = GuiShared::font(field("clientNoneFontType", "Arial Bold"), fieldI("clientNoneFontSize", 13));
        objectControl = GuiShared::bitmap(field("objectControlBitmapName", "commander/gui/cmd_control_checkbox"));
        if (!objectControl) return false;
        const auto rects = GuiShared::createBitmapArray(objectControl, 2, 1);
        if (rects.size() < 2) return false;
        controlSrc[0] = rects[0], controlSrc[1] = rects[1];
        for (Category* cat : categories) cat->setOpen(cat->open);
        return backdrop || header || headerFont || entryFont || clientNoneFont;
    }

    void onSleep(GuiControl&) override {
        header = nullptr;
        mouseInside = false;
        connected = false;
        objectControl = nullptr;
    }

    void onPreRender(GuiControl& c) override {
        ctl = &c;
        if (!updateEntries()) return;
        const uint32_t now = virtualMs();
        const int dt = (int)(now - lastTime);
        lastTime = now;
        const int mpY = lastEvent.y;
        const int hh = headerHeight(), eh = entryHeight(), openTime = std::max(1, categoryOpenTime());
        int y = 0;
        for (Category* cat : categories) {
            if (mouseInside && mpY >= y && mpY < y + hh) setHilightCell(cat);
            else {
                cat->hilight = false;
                if (hilightCell == cat) {
                    cat->onMouseLeave(*this, lastEvent);
                    hilightCell = nullptr;
                }
            }
            cat->y = y;
            y += hh;
            cat->animTime = std::max(0, cat->animTime - dt);
            int bottom = y;
            if (cat->open || cat->animTime) {
                const uint32_t full = (uint32_t)(cat->numVisible * eh);
                if (cat->animTime) {
                    const uint32_t part = (uint32_t)(cat->animTime * full) / (uint32_t)openTime;
                    cat->visibleHeight = (int)(cat->open ? full - part : part);
                } else
                    cat->visibleHeight = (int)full;
                bottom = y + cat->visibleHeight;
                for (Entry* e = cat->head; e; e = e->next) {
                    if (!(e->flags & CountedFlag)) continue;
                    if (mouseInside && mpY >= y && mpY < y + eh && (uint32_t)mpY < (uint32_t)bottom) setHilightCell(e);
                    else {
                        e->hilight = false;
                        if (hilightCell == e) {
                            e->onMouseLeave(*this, lastEvent);
                            hilightCell = nullptr;
                        }
                    }
                    e->y = y;
                    y += eh;
                }
            } else
                cat->visibleHeight = 0;
            y = bottom;
        }
        if ((int)c.extentY != y) c.extentY = (float)y;
    }

    void onRender(GuiControl& c, float x, float y) override {
        ctl = &c;
        if (!connected || !CommanderMap::find()) return;
        // The update rect: the control clipped by its parents.
        float ux0 = x, uy0 = y, ux1 = x + c.extentX, uy1 = y + c.extentY;
        for (GuiControl* p = c.parent; p && p->className != "GuiCanvas"; p = p->parent) {
            float px = 0, py = 0;
            GuiShared::canvasPosition(*p, px, py);
            ux0 = std::max(ux0, px), uy0 = std::max(uy0, py);
            ux1 = std::min(ux1, px + p->extentX), uy1 = std::min(uy1, py + p->extentY);
        }
        if (ux1 <= ux0 || uy1 <= uy0) return;
        auto& r = Engine::instance().renderer();
        drawBitmap(backdrop, ux0, uy0, ux1 - ux0, uy1 - uy0, {1, 1, 1, 1});
        const float width = ux1 - ux0;
        const int hh = headerHeight(), eh = entryHeight();
        GLint oldScissor[4];
        glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
        const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
        for (Category* cat : categories) {
            const float cy = y + cat->y;
            const float cy0 = std::max(cy, uy0), cy1 = std::min(cy + hh + cat->visibleHeight, uy1);
            if (cy1 <= cy0) continue;
            r.flushSpriteBatch();
            int win[4];
            GuiShared::canvasToWindow(ux0, cy0, width, cy1 - cy0, win);
            glEnable(GL_SCISSOR_TEST);
            glScissor(win[0], win[1], win[2], win[3]);
            if (cy < uy1 && cy + hh > uy0) cat->render(*this, x, cy, width, (float)hh);
            if (cat->open || cat->animTime)
                for (Entry* e = cat->head; e; e = e->next) {
                    if (!(e->flags & CountedFlag)) continue;
                    const float ey = y + e->y;
                    if (ey < uy1 && ey + eh > uy0) e->render(*this, x, ey, width, (float)eh);
                }
            r.flushSpriteBatch();
        }
        glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
        if (!scissorOn) glDisable(GL_SCISSOR_TEST);
    }

    void onMouseMove(GuiControl&, const GuiEvent& ev) override { lastEvent = local(ev); }
    void onMouseDown(GuiControl&, const GuiEvent& ev) override {
        lastEvent = local(ev);
        if (hilightCell) hilightCell->onMouseDown(*this, lastEvent);
    }
    void onMouseUp(GuiControl&, const GuiEvent& ev) override { lastEvent = local(ev); }
    void onRightMouseDown(GuiControl&, const GuiEvent& ev) override { lastEvent = local(ev); }
    void onRightMouseUp(GuiControl&, const GuiEvent& ev) override {
        lastEvent = local(ev);
        if (hilightCell) hilightCell->onRightMouseUp(*this, lastEvent);
    }
    void onMouseEnter(GuiControl&, const GuiEvent& ev) override {
        mouseInside = true;
        lastEvent = local(ev);
    }
    void onMouseLeave(GuiControl&, const GuiEvent&) override {
        mouseInside = false;
        if (hilightCell) {
            hilightCell->hilight = false;
            hilightCell->onMouseLeave(*this, lastEvent);
            hilightCell = nullptr;
        }
    }
};

void Category::setOpen(bool o) {
    open = o;
    const int openTime = tree->categoryOpenTime();
    animTime = std::max(0, std::min(openTime - animTime, openTime));
    if (CommanderMap* m = tree->map())
        for (EntryType* t : types) m->setTypeVisible(t->typeTag, open);
    if (auto* ts = ScriptEngine::instance().ts(); ts && tree->ctl)
        ts->callObjectMethod(tree->ctl->name, "onCategoryOpen", {VMValue(name), VMValue(open ? "true" : "false")});
}

// Sorted by type tag, then name (only its first 3 characters, as shipped),
// then target id.
void Category::insert(Entry* e) {
    e->category = this;
    Entry** link = &head;
    Entry* cur = *link;
    bool append = false;
    if (cur && cur->typeTag <= e->typeTag) {
        for (;;) {
            cur = *link;
            if (cur->typeTag == e->typeTag) {
                std::string a, b;
                if (!cur->getName(*tree, a, 256)) break;
                if (!e->getName(*tree, b, 256)) break;
                const int r = strcasecmp(a.c_str(), b.c_str());
                if (r > 0) break;
                if (r == 0 && cur->targetId > e->targetId) break;
            }
            e->prev = cur;
            link = &cur->next;
            if (!*link) {
                append = true;
                break;
            }
            if ((*link)->typeTag > e->typeTag) break;
        }
    }
    if (!append && *link) {
        e->next = *link;
        (*link)->prev = e;
    }
    *link = e;
    numEntries++;
}

void Category::unlink(Entry* e) {
    if (e->prev) e->prev->next = e->next;
    else head = e->next;
    if (e->next) e->next->prev = e->prev;
    e->prev = e->next = nullptr;
    numEntries--;
}

void Category::render(CommanderTree& t, float x, float y, float w, float h) {
    drawBitmap(t.header, x, y, w, h, {1, 1, 1, 1});
    ColorF c = t.fontColor(numVisible ? 0 : 2);
    if (hilight) c = t.fontColor(1);
    int ox = 10, oy = 4;
    t.point("headerTextOffset", 10, 4, ox, oy);
    if (t.headerFont) t.headerFont->render(displayText.c_str(), x + ox, y + oy, c);
}

void Entry::onMouseDown(CommanderTree&, const GuiEvent&) { s_lastClicked = this; }

bool ClientNoneEntry::getName(CommanderTree& t, std::string& out, size_t) {
    out = t.clientNoneText();
    return true;
}

void ClientNoneEntry::render(CommanderTree& t, float x, float y, float w, float h) {
    Font* f = t.clientNoneFont;
    if (!f) return;
    const std::string text = t.clientNoneText();
    const int tw = (int)f->measure(text.c_str()).x, th = f->charHeight;
    if (hilight) drawRect(x, y, w, h, t.color("entryHilightColor", bytes(0, 0, 255, 100)));
    f->render(text.c_str(), x + ((int)w / 2 - tw / 2), y + ((int)h / 2 - th / 2), t.fontColor(3));
}

void ClientNoneEntry::onMouseDown(CommanderTree& t, const GuiEvent&) {
    if (CommanderMap* m = t.map()) m->clearSelection(false);
}

bool TargetEntry::getName(CommanderTree&, std::string& out, size_t) {
    if (flags & HudFlag) {
        ClientTargetObject* ct = hudTarget(targetId);
        if (!ct || ct->text.empty()) return false;
        out = ct->text;
        return true;
    }
    // (The shipped tree passes a buffer size of 4.)
    return gameName(targetId, out, 4);
}

void TargetEntry::onMouseEnter(CommanderTree& t, const GuiEvent&) {
    if (CommanderMap* m = t.map()) m->hilightTarget(targetId, true);
}
void TargetEntry::onMouseLeave(CommanderTree& t, const GuiEvent&) {
    if (CommanderMap* m = t.map()) m->hilightTarget(targetId, false);
}

void TargetEntry::onMouseDown(CommanderTree& t, const GuiEvent& ev) {
    CommanderMap* m = t.map();
    if (!m) return;
    if (hilight && (flags & ControllableFlag) && !(flags & ControlledFlag)) {
        int rx = 128, ry = 2, rw = 16, rh = 16;
        const std::string v = t.field("objectControlRect", "128 2 16 16");
        std::sscanf(v.c_str(), "%d %d %d %d", &rx, &ry, &rw, &rh);
        const int px = ev.x, py = ev.y - y;
        if (px >= rx && px < rx + rw && py >= ry && py < ry + rh) {
            if (auto* ts = ScriptEngine::instance().ts())
                ts->callObjectMethod(t.ctl->name, "controlObject", {VMValue(std::to_string(targetId))});
            return;
        }
    }
    if (s_lastClicked == this && ev.clickCount > 1) {
        m->followLastSelected();
        s_lastClicked = this;
        return;
    }
    s_lastClicked = this;
    CommanderMap::Entry* me = m->findEntry(targetId);
    if (!me) return;
    m->clearSelection(category->type == 0);
    m->selectTarget(targetId, !(me->flags & CommanderMap::SelectedFlag), true);
}

void TargetEntry::onRightMouseUp(CommanderTree& t, const GuiEvent& ev) {
    if (flags & SystemFlag) return;
    CommanderMap* m = t.map();
    DemoParser* parser = connection();
    auto* ts = ScriptEngine::instance().ts();
    if (!m || !parser || !ts || !m->control()) return;
    float gx = 0, gy = 0;
    GuiShared::canvasPosition(*t.ctl, gx, gy);
    const std::string pos = std::to_string(ev.x + (int)gx) + " " + std::to_string(ev.y + (int)gy);
    if (flags & HudFlag) {
        ClientTargetObject* ct = hudTarget(targetId);
        if (!ct || !ct->script) return;
        ts->callObjectMethod(m->control()->name, "issueCommand",
                             {VMValue(std::to_string(ScriptEngine::instance().objectId(ct->script))),
                              VMValue(std::to_string(typeTag)), VMValue("-1"),
                              VMValue(std::to_string(parser->clientSensorGroup())), VMValue(pos)});
    } else {
        const DemoTargetState* ti = clientTarget(targetId);
        if (!ti || !targetObject(targetId)) return;
        const float zero[3] = {0, 0, 0};
        ClientTargetObject* ct = ClientTargets::create(-1, zero);
        if (!ct || !ct->script) return;
        ct->targetId = targetId;
        ts->callObjectMethod(m->control()->name, "issueCommand",
                             {VMValue(std::to_string(ScriptEngine::instance().objectId(ct->script))),
                              VMValue(std::to_string(tagOf(ti->type))), VMValue(std::to_string(tagOf(ti->name))),
                              VMValue(std::to_string(ti->sensorGroup)), VMValue(pos)});
    }
}

void TargetEntry::render(CommanderTree& t, float x, float y, float w, float h) {
    DemoParser* parser = connection();
    CommanderMap* m = t.map();
    if (!parser || !m) return;
    ColorF col{1, 1, 1, 1};
    Texture* icon = nullptr;
    if (!(flags & HudFlag)) {
        const DemoTargetState* ti = clientTarget(targetId);
        if (ti) {
            const uint32_t packed = parser->sensorGroupColor(parser->clientSensorGroup(), ti->sensorGroup);
            col = bytes(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff, (packed >> 24) & 0xff);
            if (const V12::DecodedDataBlock* sbd = dataBlock(ti->dataBlockId); sbd && !sbd->cmdMiniIconName.empty())
                icon = GuiShared::bitmap(sbd->cmdMiniIconName);
        }
    }
    if (!icon) {
        EntryType* et = t.findEntryType(typeTag);
        if (!et) return;
        icon = et->icon;
        col = et->color;
    }
    CommanderMap::Entry* me = m->findEntry(targetId);
    if (!me) return;
    const ColorF hil = t.color("entryHilightColor", bytes(0, 0, 255, 100));
    const ColorF sel = t.color("entrySelectColor", bytes(255, 0, 0, 100));
    auto& r = Engine::instance().renderer();
    if ((me->flags & (CommanderMap::SelectedFlag | CommanderMap::HilightFlag)) ==
        (CommanderMap::SelectedFlag | CommanderMap::HilightFlag)) {
        // A horizontal gradient from the hilight colour to the select colour.
        const ColorF colors[4] = {hil, sel, sel, hil};
        r.drawTexturedQuadColors({x + 0.5f, y + 0.5f, 0}, {x + w + 0.5f, y + 0.5f, 0}, {x + w + 0.5f, y + h + 0.5f, 0},
                                 {x + 0.5f, y + h + 0.5f, 0}, UINT32_MAX, colors, 0, 0, 1, 1, false);
    } else {
        if (me->flags & CommanderMap::SelectedFlag) drawRect(x, y, w, h, sel);
        if (me->flags & CommanderMap::HilightFlag) drawRect(x, y, w, h, hil);
    }
    if (icon) {
        ColorF tint = col;
        if (flags & DamageFlag) {
            const int d = std::clamp((int)(damage * 100.0f), 0, 100);
            const auto dc = t.damageColors();
            for (size_t i = 3; i < dc.size(); i += 4)
                if (d <= dc[i]) {
                    tint = bytes(dc[i - 3], dc[i - 2], dc[i - 1], 255);
                    break;
                }
        }
        int ix = 2, iy = 1;
        t.point("entryIconOffset", 2, 1, ix, iy);
        drawBitmap(icon, x + ix, y + iy, (float)icon->width, (float)icon->height, tint);
    }
    int tx = 20, ty = 3;
    t.point("entryTextOffset", 20, 3, tx, ty);
    const ColorF textColor = t.fontColor(4);
    if (flags & HudFlag) {
        ClientTargetObject* ct = hudTarget(targetId);
        if (ct && !ct->text.empty() && t.entryFont) t.entryFont->render(ct->text.c_str(), x + tx, y + ty, textColor);
    } else {
        std::string name;
        if (!gameName(targetId, name, 256)) return;
        if (t.entryFont) t.entryFont->render(name.c_str(), x + tx, y + ty, textColor);
    }
    if ((flags & ControllableFlag) && t.objectControl) {
        int rx = 128, ry = 2, rw = 16, rh = 16;
        std::sscanf(t.field("objectControlRect", "128 2 16 16").c_str(), "%d %d %d %d", &rx, &ry, &rw, &rh);
        const auto& src = t.controlSrc[(flags & ControlledFlag) ? 1 : 0];
        const float tw = (float)t.objectControl->width, th = (float)t.objectControl->height;
        const ColorF white{1, 1, 1, 1};
        r.drawTexturedRectUV({x + rx, y + ry, 0}, {x + rx + rw, y + ry + rh, 0}, t.objectControl->id, src[0] / tw,
                             src[1] / th, (src[0] + src[2]) / tw, (src[1] + src[3]) / th, &white);
    }
}

CommanderTree* treeOf(const std::vector<VMValue>& args) {
    if (args.empty() || !Engine::instance().hasGuiRenderer()) return nullptr;
    GuiControl* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
    if (!ctl || ctl->className != "GuiCommanderTree") return nullptr;
    if (!ctl->behavior) ctl->behavior = GuiBehaviors::create(ctl->className);
    auto* tree = dynamic_cast<CommanderTree*>(ctl->behavior.get());
    if (tree) tree->ctl = ctl;
    return tree;
}

int countTargets(Category* c, bool selected, int index) {
    if (!c) return index < 0 ? 0 : -1;
    int n = 0;
    for (Entry* e = c->head; e; e = e->next) {
        if (!(e->flags & CountedFlag) || (e->flags & SystemFlag)) continue;
        if (selected && !(e->flags & SelectedFlag)) continue;
        if (index >= 0 && n == index) return e->targetId;
        n++;
    }
    return index < 0 ? n : -1;
}

} // namespace

void registerCommanderTreeNatives(TorqueScript& ts) {
    GuiBehaviors::registerClass("GuiCommanderTree", [] { return std::make_shared<CommanderTree>(); });
    using Args = std::vector<VMValue>;
    ts.registerNative("GuiCommanderTree::addCategory", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        if (!t || args.size() < 4) return VMValue(0);
        const std::string type = args[3].toString();
        int kind;
        if (!strcasecmp(type.c_str(), "clients")) kind = 0;
        else if (!strcasecmp(type.c_str(), "targets")) kind = 1;
        else if (!strcasecmp(type.c_str(), "waypoints")) kind = 2;
        else return VMValue(0);
        return VMValue(t->addCategory(args[1].toString(), args[2].toString(), kind) ? 1 : 0);
    });
    ts.registerNative("GuiCommanderTree::openCategory", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        if (t && args.size() > 2)
            if (Category* c = t->findCategory(args[1].toString())) c->setOpen(atob(args[2].toString()));
        return VMValue("");
    });
    ts.registerNative("GuiCommanderTree::registerEntryType", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        if (!t || args.size() < 6) return VMValue(0);
        int r = 0, g = 0, b = 0;
        std::sscanf(args[5].toString().c_str(), "%d %d %d", &r, &g, &b);
        const ColorF color = bytes(r, g, b, 255);
        const uint32_t tag = (uint32_t)std::atoi(args[2].toString().c_str());
        return VMValue(t->registerEntryType(args[1].toString(), tag, atob(args[3].toString()), args[4].toString(), &color) ? 1 : 0);
    });
    ts.registerNative("GuiCommanderTree::reset", [](const Args& args) -> VMValue {
        if (CommanderTree* t = treeOf(args)) t->reset();
        return VMValue("");
    });
    ts.registerNative("GuiCommanderTree::getNumTargets", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        return VMValue(t && args.size() > 1 ? countTargets(t->findCategory(args[1].toString()), false, -1) : 0);
    });
    ts.registerNative("GuiCommanderTree::getTarget", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        if (!t || args.size() < 3) return VMValue(-1);
        const int idx = std::atoi(args[2].toString().c_str());
        return VMValue(idx < 0 ? -1 : countTargets(t->findCategory(args[1].toString()), false, idx));
    });
    ts.registerNative("GuiCommanderTree::getNumSelectedTargets", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        return VMValue(t && args.size() > 1 ? countTargets(t->findCategory(args[1].toString()), true, -1) : 0);
    });
    ts.registerNative("GuiCommanderTree::getSelectedTarget", [](const Args& args) -> VMValue {
        CommanderTree* t = treeOf(args);
        if (!t || args.size() < 3) return VMValue(-1);
        const int idx = std::atoi(args[2].toString().c_str());
        return VMValue(idx < 0 ? -1 : countTargets(t->findCategory(args[1].toString()), true, idx));
    });
}
