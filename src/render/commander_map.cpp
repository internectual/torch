// GuiCommanderMap, from the shipped client (see commander_map.h).
#include "render/commander_map.h"
#include <GL/glew.h>
#include "core/engine.h"
#include "core/console.h"
#include "core/math.h"
#include "core/timer.h"
#include "game/game.h"
#include "game/demo.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "sim/engine_classes.h"
#include "sim/net_string_table.h"
#include "sim/sim_state.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <strings.h>

namespace {

constexpr double DegToRad = 0.017453292519943295;
constexpr double RadToDeg = 57.29577951308232;

uint32_t virtualMs() { return (uint32_t)(uint64_t)(Timer::now() * 1000.0); }
uint32_t simMs() { return (uint32_t)(SimState::simTime() * 1000.0); }

bool atob(const std::string& s) { return !strcasecmp(s.c_str(), "true") || std::atof(s.c_str()) != 0.0; }

Game& game() { return Engine::instance().game(); }

// GameConnection::getServerConnection: a live or recorded connection.
DemoParser* connection() {
    Game& g = game();
    DemoParser* parser = g.getDemoParser();
    return parser && (g.isLiveClient() || g.isDemoPlaying()) ? parser : nullptr;
}

ColorF colorBytes(uint32_t packed) {
    return {(packed & 0xff) / 255.0f, ((packed >> 8) & 0xff) / 255.0f, ((packed >> 16) & 0xff) / 255.0f,
            ((packed >> 24) & 0xff) / 255.0f};
}

// The client's NetStringTable id of a received tag's text.
uint32_t tagOf(const std::string& text) {
    return text.empty() ? 0 : NetStrings::tagId(NetStrings::literal(text));
}

// Torque Z-up to Torch Y-up.
Point3F yUp(const float p[3]) { return {p[0], p[2], -p[1]}; }

const V12::DecodedDataBlock* dataBlock(int id) {
    DemoParser* parser = connection();
    if (!parser || id < 0) return nullptr;
    const auto& blocks = parser->getInitialBlock().dataBlocks;
    auto it = blocks.find((uint32_t)id);
    return it == blocks.end() ? nullptr : &it->second.decoded;
}

void ghostPosition(const GhostEntry& ghost, float out[3]) {
    const Vec3& p = ghost.hasRendered ? ghost.renderPos : ghost.position;
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}

// The terrain height at a Torque point, or false off the terrain.
bool terrainHeight(float x, float y, float& h) {
    TerrainBlock* t = game().world().terrain();
    if (!t || !t->loaded) return false;
    h = t->sampleHeight(x, -y);
    return true;
}

// Container::castRay against the client's terrain (TerrainObjectType),
// interiors (InteriorObjectType) and water (WaterObjectType).
enum { TerrainMask = 4, InteriorMask = 8, WaterMask = 16 };
bool castRay(const float start[3], const float end[3], uint32_t mask, float hit[3]) {
    float best = 2.0f;
    const float d[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len <= 0.0f) return false;
    if (mask & TerrainMask) {
        TerrainBlock* t = game().world().terrain();
        if (t && t->loaded) {
            const float step = std::max(0.25f, t->squareSize * 0.25f);
            const int n = std::max(1, (int)std::ceil(len / step));
            auto below = [&](float s) {
                float h = 0;
                const float px = start[0] + d[0] * s, py = start[1] + d[1] * s, pz = start[2] + d[2] * s;
                return terrainHeight(px, py, h) && pz <= h;
            };
            float prev = 0.0f;
            if (!below(0.0f))
                for (int i = 1; i <= n; ++i) {
                    const float s = (float)i / n;
                    if (below(s)) {
                        float lo = prev, hi = s;
                        for (int k = 0; k < 16; ++k) {
                            const float mid = (lo + hi) * 0.5f;
                            (below(mid) ? hi : lo) = mid;
                        }
                        best = std::min(best, hi);
                        break;
                    }
                    prev = s;
                }
        }
    }
    if (mask & InteriorMask) {
        const auto& mesh = game().world().collision();
        if (mesh.loaded) {
            const Point3F o = yUp(start);
            const Point3F dir{d[0] / len, d[2] / len, -d[1] / len};
            float t = 0;
            Point3F pos, normal;
            if (mesh.raycast(o, dir, len, t, pos, normal)) best = std::min(best, t / len);
        }
    }
    if ((mask & WaterMask) && d[2] != 0.0f) {
        // The water surface (WaterBlock top), when the mission has one.
        const float level = game().world().waterLevel();
        const float s = (level - start[2]) / d[2];
        if (level != 0.0f && s >= 0.0f && s <= 1.0f) best = std::min(best, s);
    }
    if (best > 1.0f) return false;
    for (int i = 0; i < 3; ++i) hit[i] = start[i] + d[i] * best;
    return true;
}

void mul4(const MatrixF& a, const MatrixF& b, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a.m[r][k] * b.m[k][c];
            out[r * 4 + c] = sum;
        }
}

bool invert4(const float m[16], float inv[16]) {
    float t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] * det;
    return true;
}

CommanderMap* s_map = nullptr;

} // namespace

// --- CommanderIconImage / CommanderIconData ---------------------------------

namespace CommanderIcons {

bool Image::getFrameSize(int& w, int& h, uint32_t frame) const {
    if (type == Static) {
        if (!texture || frame) return false;
        w = texture->width, h = texture->height;
        return true;
    }
    if (frame >= frames.size() || !frames[frame]) return false;
    w = frames[frame]->width, h = frames[frame]->height;
    return true;
}

namespace {

// CommanderIconImage::construct("type image overlay modulate <animType animSpeed>").
std::unique_ptr<Image> construct(const std::string& desc) {
    if (desc.empty()) return nullptr;
    std::istringstream in(desc);
    std::string typeStr, imageStr, overlayStr, modulateStr;
    if (!(in >> typeStr >> imageStr >> overlayStr >> modulateStr)) {
        Console::instance().printf(LogLevel::Error, "CommanderIconImage::construct: invalid fields");
        return nullptr;
    }
    auto image = std::make_unique<Image>();
    auto failed = [&]() -> std::unique_ptr<Image> {
        Console::instance().printf(LogLevel::Error, "CommanderIconImage::construct: failed to construct image '%s'",
                                   desc.c_str());
        return nullptr;
    };
    if (!strcasecmp(typeStr.c_str(), "static")) image->type = Image::Static;
    else if (!strcasecmp(typeStr.c_str(), "animation")) image->type = Image::Animation;
    else return failed();
    image->overlay = atob(overlayStr);
    image->modulate = atob(modulateStr);
    if (image->type == Image::Animation) {
        std::string animType, animSpeed;
        if (!(in >> animType >> animSpeed)) return failed();
        if (!strcasecmp(animType.c_str(), "looping")) image->animationType = Image::Looping;
        else if (!strcasecmp(animType.c_str(), "flipflop")) image->animationType = Image::FlipFlop;
        else if (!strcasecmp(animType.c_str(), "oneshot")) image->animationType = Image::OneShot;
        else return failed();
        image->animationSpeed = std::atoi(animSpeed.c_str());
        // The DML: one texture name per line.
        const auto data = Engine::instance().fs().read(("textures/commander/icons/" + imageStr + ".dml").c_str());
        if (data.empty()) return failed();
        std::istringstream lines(std::string(data.begin(), data.end()));
        std::string line;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            Texture* t = GuiShared::bitmap(line);
            if (!t) return failed();
            image->frames.push_back(t);
        }
        if (image->frames.empty()) return failed();
    } else {
        image->texture = GuiShared::bitmap("commander/icons/" + imageStr);
        if (!image->texture) return failed();
    }
    return image;
}

struct Built {
    Icon icon;
    std::vector<std::unique_ptr<Image>> owned;
};

const Icon* build(const std::string& key, const std::array<std::string, NumImages>& desc) {
    static std::map<std::string, std::pair<std::array<std::string, NumImages>, std::unique_ptr<Built>>> cache;
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == desc) return &it->second.second->icon;
    auto built = std::make_unique<Built>();
    for (int i = 0; i < NumImages; ++i) {
        auto image = construct(desc[i]);
        built->icon.images[i] = image.get();
        if (image) built->owned.push_back(std::move(image));
    }
    auto& slot = cache[key];
    slot.first = desc;
    slot.second = std::move(built);
    return &slot.second->icon;
}

} // namespace

const Icon* fromObject(const std::string& name) {
    ScriptObject* object = name.empty() ? nullptr : ScriptEngine::instance().findObject(name.c_str());
    if (!object || !EngineClasses::isA(object->className, "CommanderIconData")) return nullptr;
    static const char* const names[] = {"baseImage", "activeImage", "inactiveImage", "selectImage", "hilightImage"};
    std::array<std::string, NumImages> desc;
    for (int i = 0; i < NumImages; ++i) {
        auto field = object->fields.find(names[i]);
        if (field == object->fields.end()) field = object->fields.find("images[" + std::to_string(i) + "]");
        if (field == object->fields.end()) field = object->fields.find("images" + std::to_string(i));
        if (field != object->fields.end()) desc[i] = field->second.toString();
    }
    return build("object:" + std::to_string(ScriptEngine::instance().objectId(object)), desc);
}

const Icon* fromDataBlock(uint32_t id) {
    const V12::DecodedDataBlock* data = dataBlock((int)id);
    if (!data) return nullptr;
    return build("datablock:" + std::to_string(id), data->commanderImages);
}

} // namespace CommanderIcons

// --- GuiCommanderMap -----------------------------------------------------------

CommanderMap::CommanderMap() {
    for (auto& e : entries_) resetEntry(e);
    waypointType_ = NetStrings::add("CMDMAP__WayPointType");
    locationType_ = NetStrings::add("CMDMAP__LocationType");
    potentialTaskType_ = NetStrings::add("CMDMAP__PotentialTaskType");
    assignedTaskType_ = NetStrings::add("CMDMAP__AssignedTaskType");
    if (auto* ts = ScriptEngine::instance().ts()) {
        ts->setGlobal("$CMD_WAYPOINTTYPEID", VMValue((int32_t)waypointType_));
        ts->setGlobal("$CMD_LOCATIONTYPEID", VMValue((int32_t)locationType_));
        ts->setGlobal("$CMD_POTENTIALTASKTYPEID", VMValue((int32_t)potentialTaskType_));
        ts->setGlobal("$CMD_ASSIGNEDTASKTYPEID", VMValue((int32_t)assignedTaskType_));
    }
    // The HUD targets already in the list (the shipped map is notified of
    // each as it is added).
    for (uint32_t i = 0; i < HUDTargetList::count(); ++i)
        if (const auto* he = HUDTargetList::entry(i)) hudTargetAdded((uint32_t)(he->handle + HudBase));
}

CommanderMap::~CommanderMap() {
    if (s_map == this) s_map = nullptr;
}

CommanderMap* CommanderMap::find() {
    if (!Engine::instance().hasGuiRenderer()) return nullptr;
    GuiControl* ctl = Engine::instance().guiRenderer().findControl("CommanderMap");
    if (!ctl || ctl->className != "GuiCommanderMap") return nullptr;
    if (!ctl->behavior) ctl->behavior = GuiBehaviors::create(ctl->className);
    auto* map = dynamic_cast<CommanderMap*>(ctl->behavior.get());
    if (map) map->ctl_ = ctl;
    return map;
}

float CommanderMap::fieldF(const char* name, float fallback) const {
    const std::string v = ctl_ ? GuiShared::field(*ctl_, name) : std::string();
    return v.empty() ? fallback : (float)std::atof(v.c_str());
}
int CommanderMap::fieldI(const char* name, int fallback) const {
    const std::string v = ctl_ ? GuiShared::field(*ctl_, name) : std::string();
    return v.empty() ? fallback : std::atoi(v.c_str());
}
bool CommanderMap::fieldB(const char* name, bool fallback) const {
    const std::string v = ctl_ ? GuiShared::field(*ctl_, name) : std::string();
    return v.empty() ? fallback : atob(v);
}
ColorF CommanderMap::fieldColor(const char* name, ColorF fallback) const {
    const std::string v = ctl_ ? GuiShared::field(*ctl_, name) : std::string();
    int r = 0, g = 0, b = 0, a = 255;
    if (v.empty() || std::sscanf(v.c_str(), "%d %d %d %d", &r, &g, &b, &a) < 3) return fallback;
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

// MissionArea::smMissionArea (x, y, w, h), Torque space.
bool CommanderMap::missionArea(float out[4]) const {
    const auto& ma = game().world().missionArea;
    if (!ma.valid) return false;
    out[0] = ma.x, out[1] = -ma.z, out[2] = ma.width, out[3] = ma.height;
    return true;
}

// --- entries ------------------------------------------------------------------

void CommanderMap::resetEntry(Entry& e) {
    const int id = e.id;
    (void)id;
    e.used = false;
    e.flags = 0;
    e.sensorGroup = 0;
    e.icon = nullptr;
    e.typeTag = 0;
    e.alpha = 1.0f;
    e.scale = 1.0f;
    e.fanPos[0] = e.fanPos[1] = e.fanPos[2] = 0;
    e.id = -1;
    e.hudRef = -1;
    e.prev = e.next = -1;
    e.fanValid = false;
    e.fan.clear();
}

void CommanderMap::insert(int index, bool atTail) {
    Entry& e = entries_[index];
    if (atTail) {
        e.prev = tail_;
        e.next = -1;
        if (tail_ != -1) entries_[tail_].next = index;
        else head_ = index;
        tail_ = index;
    } else {
        e.next = head_;
        e.prev = -1;
        if (head_ != -1) entries_[head_].prev = index;
        else tail_ = index;
        head_ = index;
    }
}

void CommanderMap::unlink(int index) {
    Entry& e = entries_[index];
    if (e.prev != -1) entries_[e.prev].next = e.next;
    else if (head_ == index) head_ = e.next;
    if (e.next != -1) entries_[e.next].prev = e.prev;
    else if (tail_ == index) tail_ = e.prev;
    e.prev = e.next = -1;
}

CommanderMap::Entry* CommanderMap::findEntry(int id) {
    if (id < 0 || id >= NumEntries) return nullptr;
    return entries_[id].used ? &entries_[id] : nullptr;
}

void CommanderMap::targetAdded(int id) {
    Entry& e = entries_[id];
    e.used = true;
    e.kind = 0;
    e.id = id;
    insert(id, false);
}

void CommanderMap::targetRemoved(int id) {
    if (lastSelected_ == id) lastSelected_ = -1;
    if (followId_ == id) followId_ = -1;
    if (rightClickId_ == id) rightClickId_ = -1;
    unlink(id);
    resetEntry(entries_[id]);
}

void CommanderMap::hudTargetAdded(uint32_t id) {
    if (id >= NumEntries || entries_[id].used) return;
    Entry& e = entries_[id];
    e.used = true;
    e.kind = 1;
    e.id = (int)id;
    insert((int)id, true);
}

void CommanderMap::hudTargetRemoved(uint32_t id) {
    if (id < NumEntries && entries_[id].used) targetRemoved((int)id);
}

void CommanderMap::hudTargetsCleared() {
    for (int i = 0; i < NumEntries; ++i)
        if (entries_[i].used && entries_[i].kind == 1) hudTargetRemoved((uint32_t)i);
}

// TargetManagerNotify: targetAdded / targetRemoved / targetChanged, as the
// client's TargetInfo events change its targets.
void CommanderMap::syncTargets() {
    DemoParser* parser = connection();
    static const std::map<int, DemoTargetState> none;
    const auto& targets = parser ? parser->getTargets() : none;
    for (int i = 0; i < HudBase; ++i) {
        Entry& e = entries_[i];
        auto it = targets.find(i);
        if (e.used && e.kind == 0 && it == targets.end()) targetRemoved(i);
    }
    for (const auto& [id, target] : targets) {
        if (id < 0 || id >= HudBase) continue;
        Entry& e = entries_[id];
        if (!e.used) {
            targetAdded(id);
            e.targetChanges = target.changes;
        } else if (e.targetChanges != target.changes) {
            e.targetChanges = target.changes;
            e.flags &= ~InitFlag;
        }
    }
}

bool CommanderMap::isTypeVisible(uint32_t typeTag) const {
    return std::find(visibleTypes_.begin(), visibleTypes_.end(), typeTag) != visibleTypes_.end();
}

void CommanderMap::setTypeVisible(uint32_t typeTag, bool vis) {
    for (int i = head_; i != -1; i = entries_[i].next) {
        Entry& e = entries_[i];
        if (e.typeTag != typeTag) continue;
        e.flags = (e.flags & ~TypeVisibleFlag) | (vis ? TypeVisibleFlag : 0);
        if (!vis) {
            if (lastSelected_ == e.id) lastSelected_ = -1;
            if (followId_ == e.id) followId_ = -1;
            if (rightClickId_ == e.id) rightClickId_ = -1;
            setSelected(e, false, true);
        }
    }
    auto it = std::find(visibleTypes_.begin(), visibleTypes_.end(), typeTag);
    if (it != visibleTypes_.end() && vis) return;
    if (vis) visibleTypes_.push_back(typeTag);
    else if (it != visibleTypes_.end()) {
        *it = visibleTypes_.back();
        visibleTypes_.pop_back();
    }
}

bool CommanderMap::objectPosition(const Entry& e, float out[3]) const {
    if (e.kind == 1) {
        if (!e.clientTarget) return false;
        std::copy(e.clientTarget->lastTargetPos, e.clientTarget->lastTargetPos + 3, out);
        return true;
    }
    DemoParser* parser = connection();
    const GhostEntry* ghost = parser && e.ghost >= 0 ? parser->getGhostTracker().getGhost(e.ghost) : nullptr;
    if (!ghost) return false;
    ghostPosition(*ghost, out);
    return true;
}

void CommanderMap::updateEntries() {
    DemoParser* parser = connection();
    if (!parser) return;
    syncTargets();
    const uint32_t now = virtualMs();
    const int myGroup = parser->clientSensorGroup();
    const int ctrlGhost = game().getControlGhostIndex();
    // TargetInfo::targetObject: the ghost that set the target (ids 32 and up).
    std::array<int, HudBase> objectOf;
    objectOf.fill(-1);
    const GhostTracker& tracker = parser->getGhostTracker();
    for (int index : tracker.getAllIndices())
        if (const GhostEntry* g = tracker.getGhost(index); g && g->targetId >= 32 && g->targetId < HudBase)
            objectOf[g->targetId] = index;
    const auto& targets = parser->getTargets();
    for (int i = head_; i != -1; i = entries_[i].next) {
        Entry& e = entries_[i];
        if (e.kind == 0) {
            auto ti = targets.find(e.id);
            const int ghostIndex = objectOf[e.id];
            const GhostEntry* obj = ghostIndex >= 0 ? tracker.getGhost(ghostIndex) : nullptr;
            const V12::DecodedDataBlock* db =
                obj && EngineClasses::isA(obj->className, "ShapeBase") ? dataBlock(obj->datablockId) : nullptr;
            if (!obj || !db || ti == targets.end()) {
                e.flags &= ~VisibleFlag;
                e.hasObject = false;
                e.ghost = -1;
                continue;
            }
            if (!(e.flags & InitFlag) || !e.hasObject || e.ghost != ghostIndex) {
                e.flags = 0;
                e.ghost = ghostIndex;
                e.hasObject = true;
                if (EngineClasses::isA(obj->className, "Player")) e.flags |= PlayerFlag;
                e.icon = db->cmdIconRef ? CommanderIcons::fromDataBlock(db->cmdIconRef) : nullptr;
                e.typeTag = tagOf(ti->second.type);
                if (isTypeVisible(e.typeTag)) e.flags |= TypeVisibleFlag;
                e.sensorGroup = ti->second.sensorGroup;
                e.flags |= InitFlag;
            }
            e.flags = (e.flags & ~VisibleFlag) | (parser->isTargetVisibleToSensor(e.id) ? VisibleFlag : 0);
            e.hudRef = -1;
            e.flags &= ~TaskTargetFlag;
            e.flags = (e.flags & ~FriendlyFlag) | (e.sensorGroup == myGroup ? FriendlyFlag : 0);
            if (e.flags & VisibleFlag) {
                ghostPosition(*obj, e.pos);
                e.flags |= PositionFlag;
            }
            e.flags &= ~(DamagedFlag | DestroyedFlag);
            if (obj->damageState != 0) e.flags |= DamagedFlag;
            if (obj->damageState == 2) e.flags |= DestroyedFlag;
            e.flags = (e.flags & ~ControlFlag) | (ghostIndex == ctrlGhost ? ControlFlag : 0);
        }
        if (e.kind == 1) {
            const int index = HUDTargetList::handleIndex(e.id);
            const HUDTargetList::Entry* he = index >= 0 ? HUDTargetList::entry((uint32_t)index) : nullptr;
            ClientTargetObject* ct = he ? he->target : nullptr;
            if (!ct) continue;
            if (!(e.flags & InitFlag)) {
                e.flags &= SelectedFlag;
                switch (ct->type) {
                    case ClientTargetObject::AssignedTask:
                        e.flags |= AssignedTaskFlag, e.typeTag = assignedTaskType_, e.icon = assignedTaskIcon_;
                        break;
                    case ClientTargetObject::PotentialTask:
                        e.flags |= PotentialTaskFlag, e.typeTag = potentialTaskType_, e.icon = potentialTaskIcon_;
                        break;
                    case ClientTargetObject::Waypoint:
                        e.flags |= WaypointFlag, e.typeTag = waypointType_, e.icon = waypointIcon_;
                        break;
                    default: continue;
                }
                if (isTypeVisible(e.typeTag)) e.flags |= TypeVisibleFlag;
                e.clientTarget = ct;
                e.hasObject = true;
                std::copy(ct->lastTargetPos, ct->lastTargetPos + 3, e.pos);
                e.sensorGroup = myGroup;
                e.flags |= FriendlyFlag | InitFlag;
            }
            e.clientTarget = ct;
            if (ct->targetId == -1) e.flags |= VisibleFlag;
            else {
                // Attached to a target: drawn as an overlay on it.
                e.flags &= ~VisibleFlag;
                Entry* t = findEntry(ct->targetId);
                if (t && !(t->flags & TaskTargetFlag)) {
                    t->hudRef = e.id;
                    if (!(t->flags & VisibleFlag) && (t->flags & PositionFlag)) {
                        t->flags |= TaskTargetFlag | VisibleFlag;
                        if (project(*t)) t->flags |= ProjectedFlag;
                    }
                }
            }
        }
        if (!(e.flags & TypeVisibleFlag)) e.flags &= ~VisibleFlag;
        if ((e.flags & VisibleFlag) && project(e)) e.flags |= ProjectedFlag;
        else e.flags &= ~ProjectedFlag;
        if (e.flags & AnimRestartFlag) {
            e.flags &= ~AnimRestartFlag;
            e.selTime = now;
        }
    }
}

// GuiTSCtrl::project with the frame's matrices: canvas coordinates.
bool CommanderMap::projectPoint(const float world[3], float out[3]) const {
    const Point3F p = yUp(world);
    const float v[4] = {p.x, p.y, p.z, 1.0f};
    float c[4];
    for (int r = 0; r < 4; ++r) c[r] = viewProj_[r * 4] * v[0] + viewProj_[r * 4 + 1] * v[1] + viewProj_[r * 4 + 2] * v[2] + viewProj_[r * 4 + 3];
    if (c[3] <= 0.0f) return false;
    const float nx = c[0] / c[3], ny = c[1] / c[3], nz = c[2] / c[3];
    const float winz = nz * 0.5f + 0.5f;
    if (winz < 0.0f || winz > 1.0f) return false;
    out[0] = viewport_[0] + (nx * 0.5f + 0.5f) * viewport_[2];
    out[1] = viewport_[1] + (0.5f - ny * 0.5f) * viewport_[3];
    out[2] = winz;
    return true;
}

// GuiTSCtrl::unproject.
void CommanderMap::unprojectPoint(const float screen[3], float out[3]) const {
    const float nx = (screen[0] - viewport_[0]) / viewport_[2] * 2.0f - 1.0f;
    const float ny = 1.0f - (screen[1] - viewport_[1]) / viewport_[3] * 2.0f;
    const float nz = screen[2] * 2.0f - 1.0f;
    float c[4];
    for (int r = 0; r < 4; ++r) c[r] = viewInv_[r * 4] * nx + viewInv_[r * 4 + 1] * ny + viewInv_[r * 4 + 2] * nz + viewInv_[r * 4 + 3];
    if (c[3] == 0.0f) c[3] = 1.0f;
    // Y-up back to Torque.
    out[0] = c[0] / c[3], out[1] = -c[2] / c[3], out[2] = c[1] / c[3];
}

bool CommanderMap::project(Entry& e) {
    float s[3];
    if (!projectPoint(e.pos, s)) return false;
    e.sx = (int)s[0], e.sy = (int)s[1];
    const CommanderIcons::Image* img = e.icon && e.icon->images[CommanderIcons::Base] ? e.icon->images[CommanderIcons::Base]
                                       : defaultIcon_ ? defaultIcon_->images[CommanderIcons::Base] : nullptr;
    int w = 0, h = 0;
    if (!img || !img->getFrameSize(w, h, 0)) return true;
    const float dx = e.pos[0] - camPos_[0], dy = e.pos[1] - camPos_[1], dz = e.pos[2] - camPos_[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int maxDim = std::max(w, h);
    const float t = (float)std::tan(fov_ * 0.5 * DegToRad);
    const float W = 2.0f * dist * t;
    const float iconProjLen = fieldF("iconProjLen", 20.0f);
    const float px = ctl_->extentX / W * iconProjLen;
    const int minIcon = fieldI("minIconSize", 8), maxIcon = fieldI("maxIconSize", 40);
    const float scale = px < minIcon ? minIcon / (float)maxDim : px > maxIcon ? maxIcon / (float)maxDim : px / (float)maxDim;
    const int iw = (int)(w * scale), ih = (int)(h * scale);
    e.rect[0] = e.sx - iw / 2, e.rect[1] = e.sy - ih / 2, e.rect[2] = iw, e.rect[3] = ih;
    e.flags |= ProjectedFlag;
    e.scale = scale;
    const float r = iconProjLen / W;
    const float minD = fieldF("minDistanceScale", 0.02f), maxD = fieldF("maxDistanceScale", 0.05f);
    e.alpha = r < minD ? 0.0f : r > maxD ? 1.0f : (r - minD) / std::max(maxD - minD, 0.0001f);
    return true;
}

// --- selection ------------------------------------------------------------------

void CommanderMap::setSelected(Entry& e, bool sel, bool notify) {
    if (((e.flags & SelectedFlag) != 0) == sel) return;
    if (!sel && e.id == lastSelected_) lastSelected_ = -1;
    else lastSelected_ = e.id;
    if (sel && !(e.flags & HilightFlag) && !(e.flags & SelectedFlag)) e.flags |= AnimRestartFlag;
    if (e.flags & PotentialTaskFlag) {
        const int index = HUDTargetList::handleIndex(e.id);
        if (HUDTargetList::Entry* he = index >= 0 ? HUDTargetList::entry((uint32_t)index) : nullptr) {
            if (sel) he->canTimeout = false;
            else if (!(e.flags & HilightFlag)) {
                he->canTimeout = true;
                he->doneTime = simMs() + (uint32_t)ScriptEngine::instance().ts()->getGlobal("$clientTargetTimeout").toInt();
            }
        }
    }
    e.flags = (e.flags & ~SelectedFlag) | (sel ? SelectedFlag : 0);
    if (!notify || !(e.flags & FriendlyFlag)) return;
    auto* ts = ScriptEngine::instance().ts();
    if (!ts || !ctl_) return;
    if (e.kind == 0) {
        DemoParser* parser = connection();
        if (!e.hasObject || !parser) return;
        auto ti = parser->getTargets().find(e.id);
        const uint32_t nameTag = ti == parser->getTargets().end() ? 0 : tagOf(ti->second.name);
        const uint32_t typeTag = ti == parser->getTargets().end() ? 0 : tagOf(ti->second.type);
        ts->callObjectMethod(ctl_->name, "onSelect",
                             {VMValue(std::to_string(e.id)), VMValue(std::to_string(nameTag)),
                              VMValue(std::to_string(typeTag)), VMValue(std::to_string(sel ? 1 : 0))});
    } else {
        ts->callObjectMethod(ctl_->name, "onSelect",
                             {VMValue(std::to_string(e.id)), VMValue("-1"), VMValue(std::to_string(e.typeTag)),
                              VMValue(std::to_string(sel ? 1 : 0))});
    }
}

void CommanderMap::setHilighted(Entry& e, bool hil) {
    if (hil && !(e.flags & HilightFlag) && !(e.flags & SelectedFlag)) e.flags |= AnimRestartFlag;
    if ((e.flags & PotentialTaskFlag) && (((e.flags & HilightFlag) != 0) != hil)) {
        const int index = HUDTargetList::handleIndex(e.id);
        if (HUDTargetList::Entry* he = index >= 0 ? HUDTargetList::entry((uint32_t)index) : nullptr) {
            if (hil) he->canTimeout = false;
            else if (!(e.flags & SelectedFlag)) {
                he->canTimeout = true;
                he->doneTime = simMs() + (uint32_t)ScriptEngine::instance().ts()->getGlobal("$clientTargetTimeout").toInt();
            }
        }
    }
    e.flags = (e.flags & ~HilightFlag) | (hil ? HilightFlag : 0);
}

bool CommanderMap::selectTarget(int id, bool sel, bool notify) {
    if (id < 0 || id >= NumEntries) return false;
    Entry* e = findEntry(id);
    if (!e) return false;
    setSelected(*e, sel, notify);
    return true;
}

bool CommanderMap::hilightTarget(int id, bool hil) {
    if (id < 0 || id >= NumEntries) return false;
    Entry* e = findEntry(id);
    if (!e) return false;
    setHilighted(*e, hil);
    return true;
}

void CommanderMap::clearSelection(bool keepPlayers) {
    for (int i = head_; i != -1; i = entries_[i].next)
        if (!keepPlayers || !(entries_[i].flags & PlayerFlag)) setSelected(entries_[i], false, true);
    lastSelected_ = -1;
}

void CommanderMap::clearHilight() {
    for (int i = head_; i != -1; i = entries_[i].next) setHilighted(entries_[i], false);
}

CommanderMap::Entry* CommanderMap::hitTest(int x, int y) {
    Entry* best = nullptr;
    float bestD = 0;
    for (int i = head_; i != -1; i = entries_[i].next) {
        Entry& e = entries_[i];
        if ((e.flags & (InitFlag | ProjectedFlag | VisibleFlag)) != (InitFlag | ProjectedFlag | VisibleFlag)) continue;
        const int* r = e.rect;
        if (!(x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3])) continue;
        const int dx = r[0] + r[2] / 2 - x, dy = r[1] + r[3] / 2 - y;
        const float d = std::sqrt((float)(dx * dx + dy * dy));
        if (!best || d < bestD) best = &e, bestD = d;
    }
    return best;
}

void CommanderMap::updateHilight() {
    if (mouseMode_ != 0) {
        clearHilight();
        return;
    }
    Entry* hit = hitTest(lastEvent_.x, lastEvent_.y);
    if (hit) setHilighted(*hit, true);
    if (mouseState_ & 4) {
        for (int i = head_; i != -1; i = entries_[i].next) {
            Entry& e = entries_[i];
            const uint32_t need = PlayerFlag | InitFlag | ProjectedFlag | FriendlyFlag | VisibleFlag;
            const bool overlap = e.rect[0] < selectRect_[0] + selectRect_[2] && selectRect_[0] < e.rect[0] + e.rect[2] &&
                                 e.rect[1] < selectRect_[1] + selectRect_[3] && selectRect_[1] < e.rect[1] + e.rect[3];
            setHilighted(e, (e.flags & need) == need && overlap);
        }
    } else {
        for (int i = head_; i != -1; i = entries_[i].next)
            if (&entries_[i] != hit) setHilighted(entries_[i], false);
    }
}

bool CommanderMap::selectControlObject() {
    DemoParser* parser = connection();
    const int ctrl = game().getControlGhostIndex();
    const GhostEntry* ghost = parser && ctrl >= 0 ? parser->getGhostTracker().getGhost(ctrl) : nullptr;
    if (!ghost) return false;
    clearSelection(false);
    return selectTarget(ghost->targetId, true, true);
}

bool CommanderMap::selectClientTarget(ClientTargetObject* ct, bool sel) {
    for (uint32_t i = 0; const HUDTargetList::Entry* he = HUDTargetList::entry(i); ++i) {
        if (he->target != ct) continue;
        Entry* e = findEntry(he->handle + HudBase);
        if (!e) continue;
        if (ct->type == ClientTargetObject::PotentialTask) e->flags |= PotentialTaskFlag;
        clearSelection(false);
        setSelected(*e, sel, true);
        return true;
    }
    return false;
}

void CommanderMap::followLastSelected() {
    if (lastSelected_ == -1) {
        for (int i = head_; i != -1; i = entries_[i].next)
            if (entries_[i].hasObject && (entries_[i].flags & SelectedFlag)) {
                followId_ = entries_[i].id;
                return;
            }
    }
    followId_ = lastSelected_;
    if (followId_ == -1) {
        camPos_[0] = (boxMin_[0] + boxMax_[0]) * 0.5f;
        camPos_[1] = (boxMin_[1] + boxMax_[1]) * 0.5f;
        desiredZ_ = camPos_[2] = boxMax_[2];
    }
}

// --- camera -----------------------------------------------------------------------

void CommanderMap::resetCamera() {
    camPos_[0] = (boxMin_[0] + boxMax_[0]) * 0.5f;
    camPos_[1] = (boxMin_[1] + boxMax_[1]) * 0.5f;
    desiredZ_ = camPos_[2] = boxMax_[2];
    followId_ = -1;
}

void CommanderMap::computeHeightBounds() {
    TerrainBlock* t = game().world().terrain();
    float ma[4];
    if (!connection() || !t || !t->loaded || !missionArea(ma)) return;
    const int sq = std::max(1, (int)t->squareSize);
    float minH = 1e30f, maxH = -1e30f;
    const int x0 = (int)ma[0] / sq, y0 = (int)ma[1] / sq;
    // heightMap[((y & 0xff) << 8) + (x & 0xff)]: the grid point's height
    // where the terrain block (at -1024, -1024) puts it.
    for (int i = (int)ma[2] / sq + 1; i >= 0; i--)
        for (int j = (int)ma[3] / sq + 1; j >= 0; j--) {
            const int gx = (x0 + i) & 0xff, gy = (y0 + j) & 0xff;
            float h = 0;
            if (!terrainHeight(t->worldOffset.x + gx * t->squareSize, -t->worldOffset.z + gy * t->squareSize, h)) continue;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    // Interiors and static shapes: the top of their world box.
    for (const auto& obj : game().world().objects()) {
        if (!obj.shape || !obj.visible) continue;
        const bool staticObject = obj.shape->isInterior || EngineClasses::isA(obj.className, "StaticShape") ||
                                  EngineClasses::isA(obj.className, "TSStatic");
        if (!staticObject) continue;
        const float top = obj.shape->hasHeaderBounds ? obj.pos.z + obj.shape->headerBoundsMax.z * obj.scale.z
                                                     : obj.pos.z + obj.shape->boundsRadius() * std::max(obj.scale.z, 1.0f);
        maxH = std::max(maxH, top);
    }
    const float cameraOffset = fieldF("cameraOffset", 10.0f);
    boxMin_[2] = minH + cameraOffset;
    ceiling_ = maxH + cameraOffset;
}

void CommanderMap::computeMaxZ() {
    float ma[4];
    if (!ctl_ || !missionArea(ma)) return;
    const float w = ctl_->extentX, h = ctl_->extentY;
    const float screenDiag = std::sqrt(w * w + h * h);
    const float A = screenDiag * 0.5f * (1.0f - fieldF("screenFrameSize", 0.05f));
    const float maDiag = std::sqrt(ma[2] * ma[2] + ma[3] * ma[3]);
    const float t = (float)std::tan(0.7853981852531433);
    boxMax_[2] = ceiling_ + ((0.5f * maDiag) * (w / (2 * t))) / A;
    boxMin_[0] = ma[0], boxMin_[1] = ma[1], boxMax_[0] = ma[0] + ma[2], boxMax_[1] = ma[1] + ma[3];
    resetCamera();
}

void CommanderMap::clampCamera() {
    if (camPos_[0] >= boxMin_[0] && camPos_[0] <= boxMax_[0] && camPos_[1] >= boxMin_[1] && camPos_[1] <= boxMax_[1] &&
        desiredZ_ >= boxMin_[2] && desiredZ_ <= boxMax_[2])
        return;
    camPos_[0] = std::clamp(camPos_[0], boxMin_[0], std::max(boxMin_[0], boxMax_[0]));
    camPos_[1] = std::clamp(camPos_[1], boxMin_[1], std::max(boxMin_[1], boxMax_[1]));
    desiredZ_ = std::max(std::min(desiredZ_, boxMax_[2]), boxMin_[2]);
    camPos_[2] = std::max(desiredZ_, ceiling_);
}

void CommanderMap::updateCamera() {
    const uint32_t now = virtualMs();
    if (lastTime_) {
        float dir[3] = {0, 0, 0};
        if (moveBits_ & 1) dir[0] = -1.0f;
        if (moveBits_ & 2) dir[0] += 1.0f;
        if (moveBits_ & 4) dir[1] += 1.0f;
        if (moveBits_ & 8) dir[1] += -1.0f;
        if (moveBits_ & 0x10) dir[2] += -1.0f;
        if (moveBits_ & 0x20) dir[2] += 1.0f;
        // Written back to the fields.
        int moveTime = fieldI("cameraMoveTime", 500), zoomTime = fieldI("cameraZoomTime", 300);
        if (moveTime < 100 || zoomTime < 100) {
            moveTime = std::max(100, moveTime);
            zoomTime = std::max(100, zoomTime);
            if (ScriptObject* object = ScriptEngine::instance().findObject(ctl_->name.c_str())) {
                object->fields["cameraMoveTime"] = VMValue((int32_t)moveTime);
                object->fields["cameraZoomTime"] = VMValue((int32_t)zoomTime);
            }
        }
        const float t = (float)std::tan(0.7853981852531433);
        const float h = desiredZ_ - boxMin_[2] + fieldF("cameraOffset", 10.0f);
        const float dt = (float)(int32_t)(now - lastTime_);
        const float moveDist = (h * 2 * t) * (dt / (float)moveTime);
        const float zoomDist = h * 0.5f * (dt / (float)zoomTime);
        if (followId_ != -1) {
            Entry* e = findEntry(followId_);
            float p[3];
            if (!e || !e->hasObject || !objectPosition(*e, p)) followId_ = -1;
            else camPos_[0] = p[0], camPos_[1] = p[1];
        }
        camPos_[0] += dir[0] * moveDist;
        camPos_[1] += dir[1] * moveDist;
        desiredZ_ += dir[2] * zoomDist;
        if (dir[0] != 0 || dir[1] != 0) followId_ = -1;
        desiredZ_ = std::max(std::min(desiredZ_, boxMax_[2]), boxMin_[2]);
        camPos_[2] = desiredZ_;
        if (ceiling_ > desiredZ_) {
            // Below the obstacles: the eye stays above them, the fov narrows.
            const float half = std::atan2(desiredZ_ * t, ceiling_);
            camPos_[2] = ceiling_;
            fov_ = (float)(2 * half * RadToDeg);
        } else
            fov_ = 90.0f;
    }
    lastTime_ = now;
}

void CommanderMap::updateMouseDrag() {
    if ((mouseState_ & 8) && mouseMode_ == 1 && (mouseState_ & 1)) {
        if (grabValid_) {
            const float scr[3] = {(float)curEvent_.x, (float)curEvent_.y, 1.0f};
            float w[3];
            unprojectPoint(scr, w);
            float d[3] = {w[0] - camPos_[0], w[1] - camPos_[1], w[2] - camPos_[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0.0f && d[2] != 0.0f) {
                for (float& c : d) c /= len;
                const float t = (camPos_[2] - grabPoint_[2]) / -d[2];
                camPos_[0] = grabPoint_[0] - d[0] * t;
                camPos_[1] = grabPoint_[1] - d[1] * t;
                camPos_[2] = grabPoint_[2] - d[2] * t;
            }
        } else {
            camPos_[0] = camPos_[0] - (float)(curEvent_.x - lastEvent_.x) * 0.5f;
            camPos_[1] = camPos_[1] - (float)(lastEvent_.y - curEvent_.y) * 0.5f;
        }
    }
    lastEvent_ = curEvent_;
}

void CommanderMap::setCameraMove(uint32_t bit, bool on) { moveBits_ = (moveBits_ & ~bit) | (on ? bit : 0); }

void CommanderMap::cursorForMode() {
    switch (mouseMode_) {
        case 0: GuiShared::setCanvasCursor(cursors_[1]); break;
        case 1: GuiShared::setCanvasCursor(cursors_[2]); break;
        case 2: GuiShared::setCanvasCursor(cursors_[4]); break;
    }
}

void CommanderMap::setMouseMode(int mode) {
    mouseMode_ = mode;
    moveBits_ = 0;
    if (mouseState_ & 8) cursorForMode();
}

// --- lifecycle ------------------------------------------------------------------------

bool CommanderMap::onWake(GuiControl& ctl) {
    ctl_ = &ctl;
    s_map = this;
    // loadCursors: every GuiCursor must exist.
    static const char* const cursorFields[7] = {"defaultCursor", "arrowCursor", "handCursor", "moveCursor",
                                                "zoomCursor",    "addCursor",   "removeCursor"};
    static const char* const cursorDefaults[7] = {"DefaultCursor", "CMDCursorArrow", "CMDCursorHandOpen",
                                                  "CMDCursorHandClosed", "CMDCursorZoom", "CMDCursorSelectAdd",
                                                  "CMDCursorSelectRemove"};
    bool cursors = true;
    for (int i = 0; i < 7; ++i) {
        cursors_[i] = GuiShared::field(ctl, cursorFields[i], cursorDefaults[i]);
        ScriptObject* c = ScriptEngine::instance().findObject(cursors_[i].c_str());
        if (!c || !EngineClasses::isA(c->className, "GuiCursor")) cursors = false;
    }
    if (!cursors) {
        Console::instance().printf(LogLevel::Error, "GuiCommanderMap::onWake: failed to load mouse cursors!");
        return false;
    }
    GuiShared::setCanvasCursor(cursors_[1]);
    if (!connection()) {
        Console::instance().printf(LogLevel::Error, "GuiCommanderMap::onWake: failed to locate game connection");
        return false;
    }
    TerrainBlock* terrain = game().world().terrain();
    if (!terrain || !terrain->loaded) {
        Console::instance().printf(LogLevel::Error, "GuiCommanderMap::onWake: failed to locate terrain object");
        return false;
    }
    if (!haveTerrain_) {
        haveTerrain_ = true;
        computeHeightBounds();
        computeMaxZ();
    }
    // loadIcons.
    defaultIcon_ = CommanderIcons::fromObject(GuiShared::field(ctl, "defaultIconName", "CMDDefaultIcon"));
    waypointIcon_ = CommanderIcons::fromObject(GuiShared::field(ctl, "waypointIconName", "CMDWaypointIcon"));
    assignedTaskIcon_ = CommanderIcons::fromObject(GuiShared::field(ctl, "assignedTaskIconName", "CMDAssignedTaskIcon"));
    potentialTaskIcon_ = CommanderIcons::fromObject(GuiShared::field(ctl, "potentialTaskIconName", "CMDPotentialTaskIcon"));
    if (!defaultIcon_ || !waypointIcon_ || !assignedTaskIcon_ || !potentialTaskIcon_) {
        Console::instance().printf(LogLevel::Error, "GuiCommanderMap::onWake: failed to load icons");
        return false;
    }
    lastTime_ = 0;
    clampCamera();
    updateEntries();
    const std::string edge = GuiShared::field(ctl, "edgeMarkerTextureName", "commander/gui/cmd_offscreen_arrow");
    edgeTexture_ = edge.empty() ? nullptr : GuiShared::bitmap(edge);
    lastExtent_[0] = ctl.posX, lastExtent_[1] = ctl.posY, lastExtent_[2] = ctl.extentX, lastExtent_[3] = ctl.extentY;
    mouseState_ = 0;
    moveBits_ = 0;
    return true;
}

void CommanderMap::onSleep(GuiControl&) {
    edgeTexture_ = nullptr;
    mouseState_ = 0;
    moveBits_ = 0;
    clearHilight();
}

void CommanderMap::onPreRender(GuiControl& ctl) {
    ctl_ = &ctl;
    // GuiCommanderMap::resize: a new position or extent re-fits the camera.
    if (ctl.posX != lastExtent_[0] || ctl.posY != lastExtent_[1] || ctl.extentX != lastExtent_[2] ||
        ctl.extentY != lastExtent_[3]) {
        computeMaxZ();
        lastExtent_[0] = ctl.posX, lastExtent_[1] = ctl.posY, lastExtent_[2] = ctl.extentX, lastExtent_[3] = ctl.extentY;
    }
    updateMouseDrag();
    updateCamera();
    clampCamera();
}

// --- rendering ------------------------------------------------------------------------

void CommanderMap::drawIcon(const CommanderIcons::Image* img, const int rect[4], const ColorF& color, uint32_t time) {
    if (!img) return;
    const ColorF tint = img->modulate ? color : ColorF{1, 1, 1, 1};
    Texture* tex = nullptr;
    if (img->type == CommanderIcons::Image::Static) tex = img->texture;
    else {
        const int n = (int)img->frames.size(), spd = std::max(1, img->animationSpeed);
        int f = -1;
        switch (img->animationType) {
            case CommanderIcons::Image::Looping: f = (int)((time % (uint32_t)(spd * n)) / spd); break;
            case CommanderIcons::Image::FlipFlop:
                if (n > 1) {
                    f = (int)((time % (uint32_t)((2 * n - 2) * spd)) / spd);
                    if (f >= n) f = n - (f - n) - 2;
                } else f = 0;
                break;
            case CommanderIcons::Image::OneShot:
                if ((int32_t)time < spd * n) f = (int)(time / spd);
                break;
        }
        if (f < 0) return;
        // (The shipped build clamps to n, one past the last frame.)
        f = std::max(0, std::min(f, n - 1));
        tex = img->frames[f];
    }
    if (!tex) return;
    Engine::instance().renderer().drawTexturedRectUV({(float)rect[0], (float)rect[1], 0},
                                                     {(float)(rect[0] + rect[2]), (float)(rect[1] + rect[3]), 0},
                                                     tex->id, 0, 0, 1, 1, &tint);
}

void CommanderMap::renderSensors() {
    DemoParser* parser = connection();
    if (!parser) return;
    auto& r = Engine::instance().renderer();
    const int frameAlpha = fieldI("sensorSphereFrameAlpha", 75), fillAlpha = fieldI("sensorSphereFillAlpha", 30);
    const int detail = std::max(3, fieldI("sensorSphereDetailLevel", 64));
    const bool renderAll = fieldB("renderSensors", false);
    for (int i = head_; i != -1; i = entries_[i].next) {
        Entry& e = entries_[i];
        if (e.kind != 0 || (e.flags & (FriendlyFlag | InitFlag)) != (FriendlyFlag | InitFlag)) continue;
        const GhostEntry* ghost = e.ghost >= 0 ? parser->getGhostTracker().getGhost(e.ghost) : nullptr;
        if (!ghost || !EngineClasses::isA(ghost->className, "ShapeBase")) continue;
        const V12::DecodedDataBlock* db = dataBlock(ghost->datablockId);
        if (!db) continue;
        const float radius = (float)db->sensorRadius;
        if (radius == 0) continue;
        if (!(e.flags & (SelectedFlag | HilightFlag)) && !renderAll) continue;
        if (e.flags & DamagedFlag) continue;
        const float c[3] = {e.pos[0], e.pos[1], e.pos[2] + 1.0f};
        bool moved = false;
        if (e.fanValid) {
            const float dx = c[0] - e.fanPos[0], dy = c[1] - e.fanPos[1], dz = c[2] - e.fanPos[2];
            moved = std::sqrt(dx * dx + dy * dy + dz * dz) > 8.0f;
        } else {
            std::copy(c, c + 3, e.fanPos);
            e.fanValid = true;
        }
        if (e.fan.empty() || moved) {
            // buildSensorFan: the disc's rim clipped by the terrain.
            e.fan.assign((size_t)(detail + 1) * 3, 0.0f);
            const float step = (float)(6.283185307179586 / detail);
            std::copy(c, c + 3, e.fan.begin());
            float a = step;
            for (int k = 1; k <= detail; ++k, a += step) {
                float* p = &e.fan[(size_t)k * 3];
                p[0] = std::cos(a) * radius + c[0], p[1] = std::sin(a) * radius + c[1], p[2] = c[2];
                float hit[3];
                if (castRay(c, p, TerrainMask, hit)) std::copy(hit, hit + 3, p);
            }
            if (moved) std::copy(c, c + 3, e.fanPos);
        }
        const ColorF frame{db->sensorColor[0] / 255.0f, db->sensorColor[1] / 255.0f, db->sensorColor[2] / 255.0f,
                           frameAlpha / 255.0f};
        const ColorF fill{frame.r, frame.g, frame.b, fillAlpha / 255.0f};
        glEnable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        // drawSensorCircle: 62 segments at the eye's disc height.
        {
            const float step = 0.10134170204401016f;
            float a = step;
            Point3F prev{};
            for (int k = 0; k <= 62; ++k, a += step) {
                const float p[3] = {std::cos(a) * radius + c[0], std::sin(a) * radius + c[1], c[2]};
                const Point3F q = yUp(p);
                if (k) r.drawLine(prev, q, frame);
                prev = q;
            }
        }
        // drawSensorFan: the fill (additive), then its rim.
        const float uv[3][2] = {{0, 0}, {0, 0}, {0, 0}};
        const ColorF colors[3] = {fill, fill, fill};
        const Point3F centre = yUp(&e.fan[0]);
        for (int k = 1; k <= detail; ++k) {
            const Point3F tri[3] = {centre, yUp(&e.fan[(size_t)k * 3]),
                                    yUp(&e.fan[(size_t)(k == detail ? 1 : k + 1) * 3])};
            r.drawTexturedTriangle(tri, uv, colors, UINT32_MAX, true);
        }
        for (int k = 1; k <= detail; ++k)
            r.drawLine(yUp(&e.fan[(size_t)k * 3]), yUp(&e.fan[(size_t)(k == detail ? 1 : k + 1) * 3]), frame);
        r.flushSpriteBatch();
        glEnable(GL_DEPTH_TEST);
    }
}

void CommanderMap::renderMissionArea() {
    float ma[4];
    if (!missionArea(ma)) return;
    auto& r = Engine::instance().renderer();
    const float H = ceiling_ - fieldF("cameraOffset", 10.0f);
    const float x0 = ma[0], y0 = ma[1], x1 = ma[0] + ma[2], y1 = ma[1] + ma[3];
    const float w = ma[2], h = ma[3];
    const ColorF fill = fieldColor("missionAreaFillColor", {60 / 255.0f, 60 / 255.0f, 60 / 255.0f, 80 / 255.0f});
    const ColorF frame = fieldColor("missionAreaFrameColor", {128 / 255.0f, 0, 0, 1});
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    auto v = [](float x, float y, float z) {
        const float p[3] = {x, y, z};
        return yUp(p);
    };
    const Point3F walls[4][4] = {{v(x0, y0, 0), v(x1, y0, 0), v(x1, y0, H), v(x0, y0, H)},
                                 {v(x0, y1, 0), v(x1, y1, 0), v(x1, y1, H), v(x0, y1, H)},
                                 {v(x0, y0, 0), v(x0, y1, 0), v(x0, y1, H), v(x0, y0, H)},
                                 {v(x1, y0, 0), v(x1, y1, 0), v(x1, y1, H), v(x1, y0, H)}};
    for (const auto& q : walls)
        for (int k = 0; k < 4; ++k) r.drawLine(q[k], q[(k + 1) % 4], frame);
    for (const auto& q : walls) r.drawTexturedQuad(q[0], q[1], q[2], q[3], UINT32_MAX, fill, 0, 0, 1, 1, false);
    // The outside, darkened one mission-area size wide.
    r.drawTexturedQuad(v(x0 - w, y1 + h, H), v(x1 + w, y1 + h, H), v(x1 + w, y1, H), v(x0 - w, y1, H), UINT32_MAX, fill, 0, 0, 1, 1, false);
    r.drawTexturedQuad(v(x0 - w, y1, H), v(x0 - w, y0, H), v(x0, y0, H), v(x0, y1, H), UINT32_MAX, fill, 0, 0, 1, 1, false);
    r.drawTexturedQuad(v(x1, y1, H), v(x1 + w, y1, H), v(x1 + w, y0, H), v(x1, y0, H), UINT32_MAX, fill, 0, 0, 1, 1, false);
    r.drawTexturedQuad(v(x0 - w, y0, H), v(x1 + w, y0, H), v(x1 + w, y0 - h, H), v(x0 - w, y0 - h, H), UINT32_MAX, fill, 0, 0, 1, 1, false);
    r.flushSpriteBatch();
}

void CommanderMap::renderIcons(float, float) {
    DemoParser* parser = connection();
    if (!parser || !ctl_) return;
    auto& r = Engine::instance().renderer();
    const uint32_t now = virtualMs();
    const int myGroup = parser->clientSensorGroup();
    std::vector<Entry*> edgeList, selList, playerList;
    const bool edges = fieldB("enableEdgeMarkers", true);
    const ColorF selectedColor = fieldColor("selectedObjectColor", {1, 0, 1, 128 / 255.0f});
    const ColorF hilightColor = fieldColor("hilightedObjectColor", {244 / 255.0f, 0, 0, 128 / 255.0f});
    const bool renderText = fieldB("renderText", true);
    const int textOffset = fieldI("textOffset", 2);
    Font* font = GuiShared::profileFont(ctl_->profileName);
    const ColorF fontColor = GuiShared::profileFontColor(ctl_->profileName, 0);
    const float markerAngle = fieldF("playerMarkerAngle", 30.0f), markerOffset = fieldF("playerMarkerOffset", 8.0f);
    const float markerLen = fieldF("playerMarkerLen", 8.0f);
    const ColorF markerColor = fieldColor("playerMarkerColor", {0, 1, 0, 200 / 255.0f});
    auto imageOf = [&](const Entry& e, int idx) -> const CommanderIcons::Image* {
        if (e.icon && e.icon->images[idx]) return e.icon->images[idx];
        return defaultIcon_ ? defaultIcon_->images[idx] : nullptr;
    };
    auto sizedRect = [&](const Entry& e, const CommanderIcons::Image* img, int out[4]) {
        int w = 0, h = 0;
        img->getFrameSize(w, h, 0);
        const int sw = (int)(w * e.scale), sh = (int)(h * e.scale);
        out[0] = e.sx - (sw + 1) / 2, out[1] = e.sy - (sh + 1) / 2, out[2] = sw, out[3] = sh;
    };
    int it = head_;
    for (;;) {
        Entry* e;
        if (it != -1) {
            e = &entries_[it];
            it = e->next;
            if (e->flags & (SelectedFlag | HilightFlag)) { selList.push_back(e); continue; }
            if (e->flags & PlayerFlag) { playerList.push_back(e); continue; }
        } else if (!playerList.empty()) {
            e = playerList.back();
            playerList.pop_back();
        } else if (!selList.empty()) {
            e = selList.back();
            selList.pop_back();
        } else break;
        if ((e->flags & (VisibleFlag | InitFlag)) != (VisibleFlag | InitFlag) || !e->hasObject) continue;
        if (edges && e->icon) {
            const bool onScreen = (e->flags & ProjectedFlag) && e->rect[0] < ctl_->posX + ctl_->extentX &&
                                  ctl_->posX < e->rect[0] + e->rect[2] && e->rect[1] < ctl_->posY + ctl_->extentY &&
                                  ctl_->posY < e->rect[1] + e->rect[3];
            if (!onScreen && (e->flags & (SelectedFlag | HilightFlag))) {
                edgeList.push_back(e);
                continue;
            }
        }
        if (!(e->flags & ProjectedFlag)) continue;
        const bool friendly = e->flags & FriendlyFlag;
        // The base image, in the sensor group's colour.
        const CommanderIcons::Image* img = imageOf(*e, CommanderIcons::Base);
        if (!img) continue;
        drawIcon(img, e->rect, colorBytes(parser->sensorGroupColor(myGroup, e->sensorGroup)), now);
        // Inactive when damaged, else active.
        img = imageOf(*e, (e->flags & DamagedFlag) ? CommanderIcons::Inactive : CommanderIcons::Active);
        int rect[4];
        if (img) {
            sizedRect(*e, img, rect);
            drawIcon(img, rect, {1, 1, 1, 1}, now);
        }
        if (!(e->flags & AnimRestartFlag)) {
            if ((e->flags & SelectedFlag) && friendly) {
                img = imageOf(*e, CommanderIcons::Select);
                if (!img) continue;
                sizedRect(*e, img, rect);
                drawIcon(img, rect, selectedColor, now - e->selTime);
            } else if (e->flags & HilightFlag) {
                img = imageOf(*e, CommanderIcons::Hilight);
                if (!img) continue;
                sizedRect(*e, img, rect);
                drawIcon(img, rect, hilightColor, now - e->selTime);
            }
        }
        // The task or waypoint on this target.
        if (e->hudRef != -1 && entries_[e->hudRef].used) {
            const Entry& h = entries_[e->hudRef];
            img = h.icon && h.icon->images[CommanderIcons::Base] ? h.icon->images[CommanderIcons::Base]
                  : defaultIcon_ ? defaultIcon_->images[CommanderIcons::Base] : nullptr;
            if (img) {
                sizedRect(*e, img, rect);
                drawIcon(img, rect, (e->flags & TaskTargetFlag) ? ColorF{1, 0, 0, 1} : ColorF{0, 0, 1, 1}, now);
            }
        }
        // The name.
        if (font && ((renderText && e->alpha != 0) || (e->flags & (SelectedFlag | HilightFlag)))) {
            std::string text;
            if (e->kind == 0) {
                // TargetManager::getGameName: "name type"; '_' names are skipped.
                auto ti = parser->getTargets().find(e->id);
                if (ti != parser->getTargets().end()) {
                    const std::string& name = ti->second.name;
                    const std::string& type = ti->second.type;
                    const bool hasName = !name.empty() && name[0] != '_', hasType = !type.empty() && type[0] != '_';
                    text = hasName ? (hasType ? name + " " + type : name) : hasType ? type : std::string();
                }
            } else if (e->clientTarget) {
                text = e->clientTarget->text;
            }
            if (!text.empty()) {
                const int tw = (int)font->measure(text.c_str()).x;
                ColorF fc = fontColor;
                if (!(e->flags & (SelectedFlag | HilightFlag))) fc.a = (float)(uint8_t)(int)(fc.a * 255.0f * e->alpha) / 255.0f;
                font->render(text.c_str(), (float)(e->sx - tw / 2), (float)(e->sy + (e->rect[3] + 1) / 2 + textOffset), fc);
            }
        }
        // The player's heading.
        if (!(e->flags & PlayerFlag) || e->alpha == 0) continue;
        const GhostEntry* ghost = e->ghost >= 0 ? parser->getGhostTracker().getGhost(e->ghost) : nullptr;
        if (!ghost) continue;
        const Vec4& q = ghost->hasRendered && ghost->hasRotation ? ghost->renderRotation : ghost->rotation;
        // (0, 1, 0) by the rotation.
        const float fx = 2 * (q.x * q.y - q.w * q.z), fy = 1 - 2 * (q.x * q.x + q.z * q.z);
        if (fx == 0 && fy == 0) continue;
        float dx = fx, dy = -fy;
        const float dl = std::sqrt(dx * dx + dy * dy);
        dx /= dl, dy /= dl;
        const float W = markerLen * (float)std::tan(markerAngle * DegToRad) * e->alpha;
        const int iw = e->rect[2], ih = e->rect[3];
        float R;
        if (iw == ih) R = iw * 0.5f + markerOffset;
        else {
            float k, minor, major;
            if (ih > iw) k = 1 - dy * dy, minor = iw * 0.5f, major = ih * 0.5f;
            else k = 1 - dx * dx, minor = ih * 0.5f, major = iw * 0.5f;
            const float ecc = std::sqrt(1 - (minor * minor) / (major * major));
            R = major - major * (ecc * ecc * 0.5f) * k + markerOffset * e->alpha;
        }
        const float bx = e->sx + dx * R, by = e->sy + dy * R;
        const Point3F tri[3] = {{bx - dy * W, by + dx * W, 0},
                                {bx + dx * markerLen * e->alpha, by + dy * markerLen * e->alpha, 0},
                                {bx + dy * W, by - dx * W, 0}};
        ColorF gc = colorBytes(parser->sensorGroupColor(myGroup, e->sensorGroup));
        gc.a = (float)(uint8_t)(int)(markerColor.a * 255.0f * e->alpha) / 255.0f;
        const float uv[3][2] = {{0, 0}, {0, 0}, {0, 0}};
        const ColorF colors[3] = {gc, gc, gc};
        r.drawTexturedTriangle(tri, uv, colors, UINT32_MAX, false);
    }
    // Off-screen markers for selected or hilighted entries.
    if (!edgeList.empty() && edgeTexture_) {
        const float px = ctl_->posX, py = ctl_->posY, ex = ctl_->extentX, ey = ctl_->extentY;
        const float planes[4][4] = {{-1, 0, 0, px}, {1, 0, 0, -(px + ex)}, {0, -1, 0, py}, {0, 1, 0, -(py + ey)}};
        const float cx = px + (ex + 1) * 0.5f, cy = py + (ey + 1) * 0.5f;
        const float size = (float)fieldI("edgeMarkerSize", 32);
        for (Entry* e : edgeList) {
            const float ddx = e->pos[0] - camPos_[0], ddy = e->pos[1] - camPos_[1];
            float dir[2] = {ddx, -ddy};
            const float len = (ddx * ddx <= 0.0001f && ddy * ddy <= 0.0001f) ? 0 : std::sqrt(ddx * ddx + ddy * ddy);
            if (len > 0.0001f) dir[0] /= len, dir[1] /= len;
            float best = 1e10f;
            int bi = -1;
            for (int p = 0; p < 4; ++p) {
                const float den = planes[p][0] * dir[0] + planes[p][1] * dir[1];
                if (den < 0) continue;
                const float t = -(planes[p][0] * cx + planes[p][1] * cy + planes[p][3]) / den;
                if (t <= best) best = t, bi = p;
            }
            if (bi == -1) continue;
            float hx = cx + dir[0] * best, hy = cy + dir[1] * best;
            if (planes[bi][0] != 0) hx = -planes[bi][0] * planes[bi][3];
            else if (planes[bi][1] != 0) hy = -planes[bi][1] * planes[bi][3];
            const float v = std::clamp(-dir[1], -1.0f, 1.0f);
            float ang = std::atan2(std::sqrt(1 - v * v), v);
            if (dir[0] < 0) ang = -ang;
            const float s = std::sin(ang), co = std::cos(ang);
            static const float tc[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            Point3F corners[4];
            for (int k = 0; k < 4; ++k) {
                const float tx = tc[k][0] - 0.5f, ty = tc[k][1];
                corners[k] = {hx + (tx * co - ty * s) * size, hy + (tx * s + ty * co) * size, 0};
            }
            const ColorF col = (e->flags & SelectedFlag) ? selectedColor : hilightColor;
            r.drawTexturedQuad(corners[0], corners[1], corners[2], corners[3], edgeTexture_->id, col, 0, 0, 1, 1, false);
        }
    }
}

void CommanderMap::onRender(GuiControl& ctl, float x, float y) {
    ctl_ = &ctl;
    if (!connection() || !game().world().terrain() || !game().world().terrain()->loaded) return;
    auto& r = Engine::instance().renderer();
    r.flushSpriteBatch();
    // GuiTSCtrl::onRender / processCameraQuery: a straight-down camera.
    const float nearPlane = std::max(camPos_[2] - ceiling_, 0.1f);
    const float farPlane = camPos_[2];
    const float aspect = ctl.extentX / std::max(1.0f, ctl.extentY);
    const float hfov = (float)(fov_ * DegToRad);
    const float vfov = 2.0f * std::atan(std::tan(hfov * 0.5f) / aspect);
    MatrixF proj, view;
    proj.perspective(vfov, aspect, nearPlane, std::max(farPlane, nearPlane + 1.0f));
    const Point3F eye = yUp(camPos_);
    view.lookAt(eye, {eye.x, eye.y - 1.0f, eye.z}, {0, 0, -1});
    mul4(proj, view, viewProj_);
    invert4(viewProj_, viewInv_);
    viewport_[0] = x, viewport_[1] = y, viewport_[2] = ctl.extentX, viewport_[3] = ctl.extentY;

    updateEntries();
    if (mouseState_ & 8) updateHilight();

    const MatrixF savedProj = r.projectionMatrix(), savedView = r.view;
    GLint oldViewport[4];
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    GLint oldScissor[4];
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    int win[4];
    GuiShared::canvasToWindow(x, y, ctl.extentX, ctl.extentY, win);
    glViewport(win[0], win[1], win[2], win[3]);
    glEnable(GL_SCISSOR_TEST);
    glScissor(win[0], win[1], win[2], win[3]);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    r.setProjection(proj);
    r.setView(view);
    // renderScene(TerrainObjectType | InteriorObjectType | WaterObjectType).
    game().world().renderCommanderScene(eye);
    r.flushSpriteBatch();
    renderSensors();
    if (fieldB("renderMissionArea", true)) renderMissionArea();
    r.flushSpriteBatch();

    // The 2D pass (dglSetClipRect).
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    r.setProjection(savedProj);
    r.setView(savedView);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    renderIcons(x, y);
    if (mouseState_ & 4) {
        const ColorF c = fieldColor("mouseSelectRectColor", {1, 1, 0, 1});
        const float x0 = (float)selectRect_[0], y0 = (float)selectRect_[1];
        const float x1 = x0 + selectRect_[2], y1 = y0 + selectRect_[3];
        r.drawRectFill({x0, y0, 0}, {x1, y0 + 1, 0}, c);
        r.drawRectFill({x0, y1 - 1, 0}, {x1, y1, 0}, c);
        r.drawRectFill({x0, y0, 0}, {x0 + 1, y1, 0}, c);
        r.drawRectFill({x1 - 1, y0, 0}, {x1, y1, 0}, c);
    }
    r.flushSpriteBatch();
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (!scissorOn) glDisable(GL_SCISSOR_TEST);
}

// --- mouse ------------------------------------------------------------------------------

void CommanderMap::onMouseEnter(GuiControl&, const GuiEvent& ev) {
    mouseState_ = 8;
    moveBits_ = 0;
    cursorForMode();
    lastEvent_ = curEvent_ = ev;
}

void CommanderMap::onMouseLeave(GuiControl&, const GuiEvent& ev) {
    mouseState_ = 0;
    GuiShared::setCanvasCursor(cursors_[1]);
    lastEvent_ = curEvent_ = ev;
}

void CommanderMap::onMouseMove(GuiControl&, const GuiEvent& ev) { curEvent_ = ev; }
void CommanderMap::onRightMouseDragged(GuiControl&, const GuiEvent& ev) { curEvent_ = ev; }

void CommanderMap::onMouseDown(GuiControl& ctl, const GuiEvent& ev) {
    curEvent_ = ev;
    mouseState_ |= 1;
    GuiShared::mouseLock(&ctl);
    if (mouseMode_ == 0) {
        Entry* hit = hitTest(ev.x, ev.y);
        const bool mod = (ev.modifier & 0xf) != 0;
        if (hit && (hit->flags & SelectedFlag)) return;
        for (int i = head_; i != -1; i = entries_[i].next)
            if (!mod || !(entries_[i].flags & PlayerFlag)) setSelected(entries_[i], false, true);
        lastSelected_ = -1;
    } else if (mouseMode_ == 2) {
        moveBits_ |= 0x10;
    } else if (mouseMode_ == 1) {
        const float scr[3] = {(float)ev.x, (float)ev.y, 1.0f};
        float world[3];
        unprojectPoint(scr, world);
        float d[3] = {world[0] - camPos_[0], world[1] - camPos_[1], world[2] - camPos_[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 0) for (float& c : d) c /= len;
        const float end[3] = {camPos_[0] + d[0] * 2000.0f, camPos_[1] + d[1] * 2000.0f, camPos_[2] + d[2] * 2000.0f};
        grabValid_ = castRay(camPos_, end, TerrainMask | InteriorMask | WaterMask, grabPoint_);
        GuiShared::setCanvasCursor(cursors_[3]);
        followId_ = -1;
    }
}

void CommanderMap::onMouseDragged(GuiControl&, const GuiEvent& ev) {
    if ((mouseState_ & 1) && mouseMode_ == 0) {
        if (!(mouseState_ & 4)) {
            GuiShared::setCanvasCursor(cursors_[5]);
            mouseState_ |= 4;
            dragStart_[0] = ev.x, dragStart_[1] = ev.y;
        }
        selectRect_[0] = std::min(dragStart_[0], ev.x);
        selectRect_[1] = std::min(dragStart_[1], ev.y);
        selectRect_[2] = std::max(1, std::abs(dragStart_[0] - ev.x));
        selectRect_[3] = std::max(1, std::abs(dragStart_[1] - ev.y));
    }
    curEvent_ = ev;
}

void CommanderMap::onMouseUp(GuiControl& ctl, const GuiEvent& ev) {
    if (mouseState_ & 1) {
        if (mouseMode_ == 0) {
            if (mouseState_ & 4) {
                const uint32_t need = PlayerFlag | InitFlag | ProjectedFlag | FriendlyFlag | VisibleFlag;
                for (int i = head_; i != -1; i = entries_[i].next) {
                    Entry& e = entries_[i];
                    if ((e.flags & need) != need) continue;
                    // Inclusive-edge RectI intersect.
                    const int ix0 = std::max(e.rect[0], selectRect_[0]), iy0 = std::max(e.rect[1], selectRect_[1]);
                    const int ix1 = std::min(e.rect[0] + e.rect[2] - 1, selectRect_[0] + selectRect_[2] - 1);
                    const int iy1 = std::min(e.rect[1] + e.rect[3] - 1, selectRect_[1] + selectRect_[3] - 1);
                    if (ix1 - ix0 + 1 <= 0 || iy1 - iy0 + 1 <= 0 || !e.hasObject) continue;
                    if (ev.modifier & 0xc) setSelected(e, !(e.flags & SelectedFlag), true);
                    else setSelected(e, true, true);
                }
                mouseState_ &= ~4u;
            } else {
                Entry* hit = hitTest(ev.x, ev.y);
                if (hit && (hit->flags & InitFlag)) setSelected(*hit, !(hit->flags & SelectedFlag), true);
            }
        } else if (mouseMode_ == 2)
            moveBits_ |= 0x10;
        GuiShared::mouseUnlock(&ctl);
    }
    moveBits_ = 0;
    if (mouseState_ & 8) cursorForMode();
    curEvent_ = ev;
    mouseState_ &= ~1u;
}

void CommanderMap::onRightMouseDown(GuiControl& ctl, const GuiEvent& ev) {
    curEvent_ = ev;
    mouseState_ |= 2;
    if (mouseMode_ == 0) {
        Entry* hit = hitTest(ev.x, ev.y);
        rightClickId_ = hit && hit->hasObject ? hit->id : -1;
    } else if (mouseMode_ == 2)
        moveBits_ |= 0x20;
    GuiShared::mouseLock(&ctl);
}

void CommanderMap::onRightMouseUp(GuiControl& ctl, const GuiEvent& ev) {
    GuiShared::mouseUnlock(&ctl);
    curEvent_ = ev;
    DemoParser* parser = connection();
    auto* ts = ScriptEngine::instance().ts();
    if (parser && ts) {
        if (mouseMode_ == 2) moveBits_ &= ~0x20u;
        else if (mouseMode_ == 0 && !(mouseState_ & 4)) {
            const float scr[3] = {(float)ev.x, (float)ev.y, 1.0f};
            float world[3];
            unprojectPoint(scr, world);
            float d[3] = {world[0] - camPos_[0], world[1] - camPos_[1], world[2] - camPos_[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0) for (float& c : d) c /= len;
            const float end[3] = {camPos_[0] + d[0] * 2000.0f, camPos_[1] + d[1] * 2000.0f, camPos_[2] + d[2] * 2000.0f};
            float hit[3];
            if (castRay(camPos_, end, TerrainMask | InteriorMask | WaterMask, hit)) {
                const std::string pos = std::to_string(ev.x) + " " + std::to_string(ev.y);
                // new ClientTarget(-1, -1, 0 0 0), in the server connection.
                auto newTarget = [&]() -> int {
                    const float zero[3] = {0, 0, 0};
                    ClientTargetObject* ct = ClientTargets::create(-1, zero);
                    return ct && ct->script ? ScriptEngine::instance().objectId(ct->script) : 0;
                };
                auto setPos = [&](int id) {
                    if (auto* ct = EngineObjects::get<ClientTargetObject>(std::to_string(id)))
                        std::copy(hit, hit + 3, ct->lastTargetPos);
                };
                if (rightClickId_ != -1) {
                    Entry* e = findEntry(rightClickId_);
                    if (e && e->hudRef != -1) e = findEntry(e->hudRef);
                    if (e && e->hasObject) {
                        if (e->kind == 0) {
                            const int id = newTarget();
                            setPos(id);
                            auto ti = parser->getTargets().find(e->id);
                            if (auto* ct = EngineObjects::get<ClientTargetObject>(std::to_string(id))) ct->targetId = e->id;
                            if (ti == parser->getTargets().end()) rightClickId_ = -1;
                            else
                                ts->callObjectMethod(ctl.name, "issueCommand",
                                                     {VMValue(std::to_string(id)), VMValue(std::to_string(tagOf(ti->second.type))),
                                                      VMValue(std::to_string(tagOf(ti->second.name))),
                                                      VMValue(std::to_string(ti->second.sensorGroup)), VMValue(pos)});
                        } else if (e->kind == 1 && e->clientTarget && e->clientTarget->script) {
                            ts->callObjectMethod(ctl.name, "issueCommand",
                                                 {VMValue(std::to_string(ScriptEngine::instance().objectId(e->clientTarget->script))),
                                                  VMValue(std::to_string(e->typeTag)), VMValue("-1"),
                                                  VMValue(std::to_string(e->sensorGroup)), VMValue(pos)});
                        }
                    }
                } else {
                    // Bare ground: a location command.
                    const int id = newTarget();
                    setPos(id);
                    ts->callObjectMethod(ctl.name, "issueCommand",
                                         {VMValue(std::to_string(id)), VMValue(std::to_string(locationType_)), VMValue("-1"),
                                          VMValue("-1"), VMValue(pos)});
                }
            }
        }
    }
    moveBits_ = 0;
    if (mouseState_ & 8) cursorForMode();
    mouseState_ &= ~2u;
}

// --- console --------------------------------------------------------------------------------

void registerCommanderNatives(TorqueScript& ts) {
    GuiBehaviors::registerClass("GuiCommanderMap", [] { return std::make_shared<CommanderMap>(); });
    registerCommanderTreeNatives(ts);
    // The ids the scripts compare type tags with (set by the map's
    // constructor; the .gui's tree registers types with them on load).
    ts.setGlobal("$CMD_WAYPOINTTYPEID", VMValue((int32_t)NetStrings::add("CMDMAP__WayPointType")));
    ts.setGlobal("$CMD_LOCATIONTYPEID", VMValue((int32_t)NetStrings::add("CMDMAP__LocationType")));
    ts.setGlobal("$CMD_POTENTIALTASKTYPEID", VMValue((int32_t)NetStrings::add("CMDMAP__PotentialTaskType")));
    ts.setGlobal("$CMD_ASSIGNEDTASKTYPEID", VMValue((int32_t)NetStrings::add("CMDMAP__AssignedTaskType")));
    using Args = std::vector<VMValue>;
    auto map = [](const Args& args) -> CommanderMap* {
        if (args.empty() || !Engine::instance().hasGuiRenderer()) return nullptr;
        GuiControl* ctl = Engine::instance().guiRenderer().findControl(args[0].toString());
        if (!ctl || ctl->className != "GuiCommanderMap") return nullptr;
        if (!ctl->behavior) ctl->behavior = GuiBehaviors::create(ctl->className);
        auto* m = dynamic_cast<CommanderMap*>(ctl->behavior.get());
        if (m && !m->control()) CommanderMap::find();
        return m;
    };
    ts.registerNative("GuiCommanderMap::cameraMove", [map](const Args& args) -> VMValue {
        CommanderMap* m = map(args);
        if (!m || args.size() < 3) return VMValue("");
        static const std::pair<const char*, uint32_t> dirs[] = {{"left", 1}, {"right", 2}, {"up", 4},
                                                                 {"down", 8}, {"in", 0x10}, {"out", 0x20}};
        for (const auto& [name, bit] : dirs)
            if (!strcasecmp(args[1].toString().c_str(), name)) m->setCameraMove(bit, atob(args[2].toString()));
        return VMValue("");
    });
    ts.registerNative("GuiCommanderMap::setTargetTypeVisible", [map](const Args& args) -> VMValue {
        if (CommanderMap* m = map(args); m && args.size() > 2)
            m->setTypeVisible((uint32_t)std::atoi(args[1].toString().c_str()), atob(args[2].toString()));
        return VMValue("");
    });
    ts.registerNative("GuiCommanderMap::followLastSelected", [map](const Args& args) -> VMValue {
        if (CommanderMap* m = map(args)) m->followLastSelected();
        return VMValue("");
    });
    ts.registerNative("GuiCommanderMap::resetCamera", [map](const Args& args) -> VMValue {
        if (CommanderMap* m = map(args)) m->resetCamera();
        return VMValue("");
    });
    ts.registerNative("GuiCommanderMap::getMouseMode", [map](const Args& args) -> VMValue {
        CommanderMap* m = map(args);
        static const char* const modes[] = {"select", "move", "zoom"};
        return VMValue(m && m->mouseMode() >= 0 && m->mouseMode() < 3 ? modes[m->mouseMode()] : "");
    });
    ts.registerNative("GuiCommanderMap::setMouseMode", [map](const Args& args) -> VMValue {
        CommanderMap* m = map(args);
        if (!m || args.size() < 2) return VMValue("");
        const std::string mode = args[1].toString();
        if (!strcasecmp(mode.c_str(), "select")) m->setMouseMode(0);
        else if (!strcasecmp(mode.c_str(), "move")) m->setMouseMode(1);
        else if (!strcasecmp(mode.c_str(), "zoom")) m->setMouseMode(2);
        else Console::instance().printf(LogLevel::Error, "GuiCommanderMap::cSetMouseMode: invalid mode '%s'", mode.c_str());
        return VMValue("");
    });
    ts.registerNative("GuiCommanderMap::selectControlObject", [map](const Args& args) -> VMValue {
        CommanderMap* m = map(args);
        return VMValue(m && m->selectControlObject() ? 1 : 0);
    });
    ts.registerNative("GuiCommanderMap::selectClientTarget", [map](const Args& args) -> VMValue {
        CommanderMap* m = map(args);
        if (!m || args.size() < 3) return VMValue(0);
        auto* ct = EngineObjects::get<ClientTargetObject>(args[1].toString());
        if (!ct) {
            Console::instance().printf(LogLevel::Error, "GuiCommanderMap::cSelectClientTarget: not a clientTarget %s",
                                       args[1].toString().c_str());
            return VMValue(0);
        }
        return VMValue(m->selectClientTarget(ct, atob(args[2].toString())) ? 1 : 0);
    });
}
