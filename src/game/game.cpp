#include "game/game.h"
#include "render/material_parity.h"
#include "render/environment_commands.h"
#include "game/decal_runtime.h"
#include <GL/glew.h>
#include "game/demo.h"
#include "net/v12_registry.h"
#include "game/hud.h"
#include "game/item_parity.h"
#include "game/link_beam.h"
#include "game/physics.h"
#include "game/time_scale.h"
#include "game/projectile_audio.h"
#include "game/wind.h"
#include "game/mission_parser.h"
#include "game/mission_discovery.h"
#include "game/objective_parity.h"
#include <SDL3/SDL.h>
#include "render/renderer.h"
#include "render/dts_animation.h"
#include "render/texture_frames.h"
#include "render/shader.h"
#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/console_args.h"
#include "core/config.h"
#include "core/engine.h"
#include "core/input_parity.h"
#include "script/torquescript.h"
#include <algorithm>
#include <numeric>
#include "fs/file_system.h"
#include "fs/path_policy.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cctype>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <utility>

static void appendWheelNodeOverrides(const GhostEntry& ghost, DTSShape& shape,
                                     float dt, DTSShape::NodeOverride* overrides,
                                     int& overrideCount, int maxOverrides,
                                     float* wheelRotation);

static std::string formatDemoRemoteText(const std::string& templ,
                                        const std::vector<std::string>& values) {
    std::string text = templ;
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string key = "%" + std::to_string(i + 1);
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            text.replace(pos, key.size(), values[i]);
            pos += values[i].size();
        }
    }
    for (size_t pos = 0; (pos = text.find('%', pos)) != std::string::npos;) {
        size_t end = pos + 1;
        while (end < text.size() && std::isdigit((unsigned char)text[end])) ++end;
        if (end == pos + 1) { ++pos; continue; }
        text.erase(pos, end - pos);
    }
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20;
    }), text.end());
    return text;
}

// Forward declarations

static const char* liveFlagStatus(const std::string& status) {
    if (status == "<At Base>" || status == "At Base") return "home";
    if (status == "<In the Field>" || status == "In the Field") return "field";
    return status.empty() ? "home" : "held";
}

// ─── Mission shape helpers ─────────────────────────────────────────
// Read shapeFile values from datablocks created by executed TorqueScript.
static void scanDatablockShapesFromCS(World& world) {
    int found = 0;
    for (const auto& [name, object] : ScriptEngine::instance().objects) {
        if (!object) continue;
        auto shape = object->fields.find("shapeFile");
        if (shape == object->fields.end()) {
            for (auto it = object->fields.begin(); it != object->fields.end(); ++it) {
                std::string fieldName = it->first;
                for (char& c : fieldName)
                    c = (char)std::tolower((unsigned char)c);
                if (fieldName == "shapefile") {
                    shape = it;
                    break;
                }
            }
        }
        if (shape == object->fields.end() || shape->second.toString().empty()) continue;
        std::string path = shape->second.toString();
        if (path.find("shapes/") != 0 && path.find("interiors/") != 0)
            path = "shapes/" + path;
        world.datablockShapes[name] = path;
        found++;
    }
    Console::instance().printf(LogLevel::Debug, "  loaded datablock shapes from script objects: %d found", found);
}

// Check if a .mis mission object class should be rendered as a shape
static bool isRenderableMissionShape(const std::string& className) {
    if (className == "InteriorInstance" || className == "TSStatic" ||
        className == "StaticShape" || className == "ScopeAlwaysShape" ||
        className == "Turret" || className == "Item" ||
        className == "Camera" || className == "WayPoint" || className == "Marker" ||
        className == "BeaconObject" || className == "Debris" ||
        className == "WheeledVehicle" || className == "HoverVehicle" ||
        className == "FlyingVehicle" || className == "ForceFieldBare" ||
        className == "Sentry" || className == "Shrike" || className == "Turbograv" ||
        className == "Wildcat" || className == "Shield" || className == "Vehicle")
        return true;
    // Vehicle subclasses (VehicleData-derived)
    if (className.size() > 9 && className.compare(className.size() - 9, 9, "Vehicle") == 0)
        return true;
    if (className.size() > 5 && className.compare(className.size() - 5, 5, "Turret") == 0)
        return true;
    return false;
}

static const std::string* findDatablockShape(
    const std::unordered_map<std::string, std::string>& shapes,
    const std::string& datablock) {
    auto exact = shapes.find(datablock);
    if (exact != shapes.end()) return &exact->second;
    for (const auto& [name, path] : shapes) {
        if (name.size() != datablock.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < name.size(); i++) {
            if (std::tolower((unsigned char)name[i]) !=
                std::tolower((unsigned char)datablock[i])) {
                equal = false;
                break;
            }
        }
        if (equal) return &path;
    }
    return nullptr;
}

static std::string normalizeShapePath(std::string path) {
    if (!path.empty() && path.back() == '"') path.pop_back();
    if (path.empty()) return {};
    for (char& c : path) {
        if (c == '\\') c = '/';
        c = (char)std::tolower((unsigned char)c);
    }
    if (path.starts_with("shapes/") || path.starts_with("interiors/")) return path;
    return path.ends_with(".dif") ? "interiors/" + path : "shapes/" + path;
}

static std::string normalizeMissionName(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    std::string lower = name;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.starts_with("base/missions/")) name.erase(0, 14);
    else if (lower.starts_with("missions/")) name.erase(0, 9);
    if (name.size() >= 4) {
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".mis") name.resize(name.size() - 4);
    }
    return name;
}

static std::vector<std::string> terrainAssetCandidates(std::string name) {
    for (char& c : name) if (c == '\\') c = '/';
    std::string lower = name;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (!lower.ends_with(".ter")) name += ".ter";
    std::vector<std::string> result{name};
    if (!lower.starts_with("terrains/") && !lower.starts_with("missions/"))
        result.push_back("terrains/" + name);
    return result;
}

static std::vector<uint32_t> parseEmptySquareRuns(const std::string& value) {
    std::vector<uint32_t> runs;
    const char* cursor = value.c_str();
    while (*cursor) {
        char* end = nullptr;
        unsigned long long packed = std::strtoull(cursor, &end, 0);
        if (end == cursor) { ++cursor; continue; }
        if (packed <= 0xffffffffull) runs.push_back((uint32_t)packed);
        cursor = end;
    }
    return runs;
}

static ColorF unpackShockwaveColor(uint32_t packed) {
    return {((packed >> 0) & 0xff) / 255.0f,
            ((packed >> 8) & 0xff) / 255.0f,
            ((packed >> 16) & 0xff) / 255.0f,
            ((packed >> 24) & 0xff) / 255.0f};
}

static ColorF interpolateShockwaveColor(const V12::DecodedDataBlock::ShockwaveData& data,
                                        float normalizedAge) {
    if (data.colors.empty()) return {1, 1, 1, 1};
    const size_t count = std::min(data.colors.size(), data.times.size());
    if (count == 0) return unpackShockwaveColor(data.colors.front());
    if (normalizedAge <= data.times[0]) return unpackShockwaveColor(data.colors[0]);
    for (size_t i = 1; i < count; ++i) {
        if (normalizedAge <= data.times[i]) {
            const float span = data.times[i] - data.times[i - 1];
            const float t = span > 0.0f ? (normalizedAge - data.times[i - 1]) / span : 0.0f;
            const ColorF a = unpackShockwaveColor(data.colors[i - 1]);
            const ColorF b = unpackShockwaveColor(data.colors[i]);
            return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
        }
    }
    return unpackShockwaveColor(data.colors[count - 1]);
}

static const VMValue* scriptField(const ScriptObject* object, const std::string& wanted) {
    if (!object) return nullptr;
    for (const auto& [name, value] : object->fields) {
        if (name.size() == wanted.size()) {
            bool equal = true;
            for (size_t i = 0; i < name.size(); ++i)
                if (std::tolower((unsigned char)name[i]) !=
                    std::tolower((unsigned char)wanted[i])) { equal = false; break; }
            if (equal) return &value;
        }
    }
    return nullptr;
}

static ScriptObject* findScriptObject(const std::string& name) {
    for (const auto& [objectName, object] : ScriptEngine::instance().objects) {
        if (!object || objectName.size() != name.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < name.size(); ++i)
            if (std::tolower((unsigned char)objectName[i]) !=
                std::tolower((unsigned char)name[i])) { equal = false; break; }
        if (equal) return object;
    }
    return nullptr;
}

static float scriptFloat(const ScriptObject* object, const char* field, float fallback = 0.0f) {
    if (const auto* value = scriptField(object, field)) return value->toFloat();
    return fallback;
}

static bool scriptBool(const ScriptObject* object, const char* field, bool fallback = false) {
    if (const auto* value = scriptField(object, field)) return value->toBool();
    return fallback;
}

static const DTSShape::Animation* findAnimation(const DTSShape& shape,
                                                 const char* wanted) {
    if (!wanted) return nullptr;
    std::string name = wanted;
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    for (const auto& animation : shape.animations) {
        std::string candidate = animation.name;
        for (char& c : candidate) c = (char)std::tolower((unsigned char)c);
        if (candidate == name) return &animation;
    }
    return nullptr;
}

static int findFirstNode(const DTSShape& shape,
                         std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const int node = shape.findNode(name);
        if (node >= 0) return node;
    }
    return -1;
}

static Point3F mountedNodePosition(const MatrixF& model, const DTSShape& image,
                                   const char* fallbackNode) {
    const int node = findFirstNode(image, {"muzzlePoint", "MuzzlePoint", "muzzle", fallbackNode});
    if (node >= 0 && node < (int)image.defaultTransforms.size())
        return model.transform({image.defaultTransforms[node].m[0][3],
                                image.defaultTransforms[node].m[1][3],
                                image.defaultTransforms[node].m[2][3]});
    return model.transform({0, 0, 0});
}

static std::string defaultMissionAnimation(const DTSShape* shape) {
    if (!shape || !shape->loaded) return {};
    // Torque's client-side ambient thread runs for mission ShapeBase objects.
    // Prefer it over arbitrary sequence 0; sequence 0 may be an activation
    // sequence whose time-zero pose intentionally hides part of the object.
    if (findAnimation(*shape, "ambient")) return "ambient";
    if (findAnimation(*shape, "power")) return "power";
    return {};
}

// Resolve the shape file path for a mission object. The mission or its
// datablock must identify the asset; class-name guesses are not faithful.
static std::string resolveShapePath(const MisObject& obj,
                                    const std::unordered_map<std::string, std::string>& datablockShapes) {
    std::string shape = getProp(obj.props, "shapename");
    if (!shape.empty()) {
        return normalizeShapePath(shape);
    }
    shape = getProp(obj.props, "interiorFile");
    if (!shape.empty()) {
        return normalizeShapePath(shape);
    }
    // Try datablock InstanceName lookup
    std::string db = getProp(obj.props, "datablock");
    if (!db.empty()) {
        // Property values may have trailing quotes from parser
        if (!db.empty() && db.back() == '"') db.pop_back();
        if (const auto* path = findDatablockShape(datablockShapes, db))
            return *path;
    }
    return "";
}

// ─── Sound helpers ────────────────────────────────────────────────
static void playChatBeep() {
    static SoundBuffer* beepBuf = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // Generate a 440Hz sine wave beep as WAV
        int sampleRate = 22050;
        int duration = 150; // ms
        int numSamples = sampleRate * duration / 1000;
        int dataSize = numSamples * 2; // 16-bit mono
        // Build WAV header
        struct WavHeader {
            char riff[4] = {'R','I','F','F'};
            uint32_t fileSize;
            char wave[4] = {'W','A','V','E'};
            char fmt[4] = {'f','m','t',' '};
            uint32_t fmtSize = 16;
            uint16_t audioFmt = 1; // PCM
            uint16_t channels = 1;
            uint32_t sampleRate;
            uint32_t byteRate;
            uint16_t blockAlign = 2;
            uint16_t bitsPerSample = 16;
            char dataHdr[4] = {'d','a','t','a'};
            uint32_t dataSize;
        };
        WavHeader hdr;
        hdr.sampleRate = sampleRate;
        hdr.byteRate = sampleRate * 2;
        hdr.dataSize = dataSize;
        hdr.fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(sizeof(hdr) + dataSize);
        memcpy(wav.data(), &hdr, sizeof(hdr));
        // Generate sine wave samples
        int16_t* samples = (int16_t*)(wav.data() + sizeof(hdr));
        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;
            float env = 1.0f - (float)i / numSamples; // fade out
            samples[i] = (int16_t)(sinf(t * 440.0f * 6.28318f) * 8000.0f * env);
        }
        // Apply fade-in
        for (int i = 0; i < 200 && i < numSamples; i++)
            samples[i] = (int16_t)(samples[i] * ((float)i / 200.0f));

        beepBuf = new SoundBuffer;
        if (!beepBuf->loadWav(wav.data(), wav.size())) {
            delete beepBuf;
            beepBuf = nullptr;
        }
    }
    if (!beepBuf) return;
    auto& audio = Engine::instance().audio();
    // Reuse a single source for all chat beeps instead of leaking one per call.
    static SoundSource* beepSrc = nullptr;
    if (!beepSrc) beepSrc = audio.createSource();
    if (beepSrc) {
        beepSrc->setVolume(0.3f);
        beepSrc->stop();
        beepSrc->play(beepBuf);
    }
}
#include <unistd.h>
#include <algorithm>
#include <unordered_map>
#include <set>

// ─── 3D to screen projection ─────────────────────────────────
static Point3F worldToScreen(const Point3F& worldPos, const MatrixF& view, const MatrixF& proj, int screenW, int screenH) {
    const float* v = &view.m[0][0];
    float cx = worldPos.x*v[0]+worldPos.y*v[1]+worldPos.z*v[2]+v[3];
    float cy = worldPos.x*v[4]+worldPos.y*v[5]+worldPos.z*v[6]+v[7];
    float cz = worldPos.x*v[8]+worldPos.y*v[9]+worldPos.z*v[10]+v[11];
    float cw = worldPos.x*v[12]+worldPos.y*v[13]+worldPos.z*v[14]+v[15];
    const float* p = &proj.m[0][0];
    float nx = cx*p[0]+cy*p[1]+cz*p[2]+cw*p[3];
    float ny = cx*p[4]+cy*p[5]+cz*p[6]+cw*p[7];
    float nz = cx*p[8]+cy*p[9]+cz*p[10]+cw*p[11];
    float nw = cx*p[12]+cy*p[13]+cz*p[14]+cw*p[15];
    if (nw == 0) return {-999, -999, 0};
    float invW = 1.0f / nw;
    return {(nx*invW*0.5f+0.5f)*screenW, (-ny*invW*0.5f+0.5f)*screenH, nz*invW};
}

static bool parseLiveIndex(const std::string& text, int& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < 0 || parsed >= 1024) return false;
    value = (int)parsed;
    return true;
}

// Forward declarations for demo ghost shape helpers
static bool isRenderableGhostClass(const std::string& className);
static bool isEffectOnlyGhostClass(const std::string& className);

Player::Player() {
    for (int i = 0; i < gWeaponCount; i++) {
        Weapon w;
        w.type = i;
        w.ammo = gWeaponTable[i].maxAmmo > 0 ? gWeaponTable[i].maxAmmo : 9999;
        w.fireTimer = 0;
        w.reloadTimer = 0;
        w.firing = false;
        w.reloading = false;
        weapons.push_back(w);
    }
}
Player::~Player() {}

void Player::update(float dt) {
    if (hp > 0) {
        updateAnimation(dt, false);
    }
}

void Player::loadModel() {
    if (modelLoaded) return;

    auto& fs = Engine::instance().fs();
    auto* ts = ScriptEngine::instance().ts();
    if (!ts) return;

    const std::string currentName = ts->getGlobal("$pref::Player::Current").toString();
    const std::string profile = ts->getGlobal(
        "$pref::Player[" + currentName + "]").toString();
    if (profile.empty()) {
        Console::instance().printf(LogLevel::Error,
            "Player: script did not provide the active player profile");
        return;
    }

    auto field = [](const std::string& value, int index) {
        size_t start = 0;
        for (int i = 0; i < index; i++) {
            start = value.find('\t', start);
            if (start == std::string::npos) return std::string();
            start++;
        }
        size_t end = value.find('\t', start);
        return value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    auto word = [](const std::string& value, int index) {
        size_t start = 0;
        for (int i = 0; i <= index; i++) {
            start = value.find_first_not_of(" \t", start);
            if (start == std::string::npos) return std::string();
            size_t end = value.find_first_of(" \t", start);
            if (i == index) return value.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? value.size() : end;
        }
        return std::string();
    };
    const std::string raceGender = field(profile, 1);
    const std::string sex = word(raceGender, 1).empty() ? "Male" : word(raceGender, 1);
    const std::string race = word(raceGender, 0).empty() ? "Human" : word(raceGender, 0);
    const std::string armorSize = ts->getGlobal("$DefaultPlayerArmor").toString().empty()
        ? "Light" : ts->getGlobal("$DefaultPlayerArmor").toString();
    if (!ts->hasFunction("getArmorDatablock")) {
        Console::instance().printf(LogLevel::Error,
            "Player: script armor resolver is not loaded");
        return;
    }

            static uint32_t resolverId = 0;
    const std::string resolverName =
        "__torch_player_resolver_" + std::to_string(++resolverId);
    auto* resolver = new ScriptObject;
    resolver->name = resolverName;
    resolver->fields["race"] = VMValue(race);
    resolver->fields["sex"] = VMValue(sex);
    ScriptEngine::instance().objects[resolverName] = resolver;
    std::string datablockName;
    if (race == "Bioderm")
        datablockName = armorSize + "MaleBiodermArmor";
    else
        datablockName = armorSize + sex + race + "Armor";
    auto findDataBlock = [&](const std::string& name) {
        auto it = ScriptEngine::instance().objects.find(name);
        if (it != ScriptEngine::instance().objects.end()) return it;
        std::string wanted = name;
        for (char& c : wanted) c = (char)tolower((unsigned char)c);
        for (auto candidate = ScriptEngine::instance().objects.begin();
             candidate != ScriptEngine::instance().objects.end(); ++candidate) {
            std::string key = candidate->first;
            for (char& c : key) c = (char)tolower((unsigned char)c);
            if (key == wanted) return candidate;
        }
        return ScriptEngine::instance().objects.end();
    };
    if (findDataBlock(datablockName) == ScriptEngine::instance().objects.end()) {
        datablockName = ts->callFunction(
            "getArmorDatablock", {VMValue(resolverName), VMValue(armorSize)}).toString();
    }
    ScriptEngine::instance().objects.erase(resolverName);
    delete resolver;

    auto datablock = findDataBlock(datablockName);
    if (datablock == ScriptEngine::instance().objects.end() || !datablock->second) {
        Console::instance().printf(LogLevel::Warn,
            "Player: PlayerData '%s' unavailable; using stock armor path", datablockName.c_str());
        const std::string size = armorSize == "Medium" || armorSize == "Heavy" ? armorSize : "Light";
        const std::string gender = sex == "Female" ? "female" : "male";
        const std::string racePrefix = race == "Bioderm" ? "bioderm_" : "";
        const std::string path = "shapes/" + racePrefix +
            std::string(size == "Light" ? "light" : size == "Medium" ? "medium" : "heavy") +
            "_" + gender + ".dts";
        auto data = fs.read(path.c_str());
        if (!data.empty() && modelShape.load(data.data(), data.size())) {
            modelShape.name = path;
            const std::string skin = field(profile, 2);
            if (!skin.empty()) modelShape.applySkin(skin);
            modelLoaded = true;
            auto weaponData = fs.read("shapes/weapon_disc.dts");
            if (!weaponData.empty()) {
            weaponShape.name = "shapes/weapon_disc.dts";
                weaponLoaded = weaponShape.load(weaponData.data(), weaponData.size());
                loadWeaponModel();
            }
        }
        return;
    }
    auto shapeField = datablock->second->fields.find("shapeFile");
    if (shapeField == datablock->second->fields.end()) {
        for (auto it = datablock->second->fields.begin();
             it != datablock->second->fields.end(); ++it) {
            std::string fieldName = it->first;
            for (char& c : fieldName)
                c = (char)tolower((unsigned char)c);
            if (fieldName == "shapefile") { shapeField = it; break; }
        }
    }
    if (shapeField == datablock->second->fields.end() || shapeField->second.toString().empty()) {
        Console::instance().printf(LogLevel::Error,
            "Player: resolved PlayerData has no shapeFile");
        return;
    }

    const std::string path = normalizeShapePath(shapeField->second.toString());
    auto data = fs.read(path.c_str());
    if (!data.empty()) {
        modelShape.name = path;
        if (modelShape.load(data.data(), data.size())) {
            modelLoaded = true;
            Console::instance().printf(LogLevel::Info,
                "Player: loaded script-selected model '%s'", path.c_str());
            auto weaponData = fs.read("shapes/weapon_disc.dts");
            if (!weaponData.empty()) {
                weaponShape.name = "shapes/weapon_disc.dts";
                weaponLoaded = weaponShape.load(weaponData.data(), weaponData.size());
            }
            loadWeaponModel();
            return;
        }
    }
    Console::instance().printf(LogLevel::Error,
        "Player: native script-selected DTS model '%s' could not be loaded",
        path.c_str());
}

void Player::loadWeaponModel() {
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    std::string itemName = gWeaponTable[weapons[curWeapon].type].name;
    if (itemName == "Spinfusor") itemName = "Disc";
    else if (itemName == "PlasmaGun") itemName = "Plasma";
    else if (itemName == "RepairTool") itemName = "RepairTool";

    auto* item = ScriptEngine::instance().findObject(itemName.c_str());
    std::string path;
    if (item) {
        auto shapeField = item->fields.find("shapeFile");
        if (shapeField != item->fields.end())
            path = normalizeShapePath(shapeField->second.toString());
    }
    if (path.empty()) {
        auto shape = Engine::instance().game().world().datablockShapes.find(itemName);
        if (shape != Engine::instance().game().world().datablockShapes.end())
            path = normalizeShapePath(shape->second);
    }
    if (path.empty()) return;
    auto data = Engine::instance().fs().read(path.c_str());
    if (data.empty()) return;
    DTSShape candidate;
    candidate.name = path;
    if (!candidate.load(data.data(), data.size())) return;
    weaponShape = std::move(candidate);
    weaponLoaded = true;
    weaponAnimTime = 0.0f;
}

void Player::updateAnimation(float dt, bool jetting) {
    animTime += dt;
    weaponAnimTime += dt;

    AnimState newAnim;
    if (hp <= 0) {
        newAnim = Death;
    } else if (jetting && !onGround) {
        newAnim = Jet;
    } else if (!onGround) {
        newAnim = Jump;
    } else if (fabsf(vel.x) > 0.5f || fabsf(vel.z) > 0.5f) {
        newAnim = Run;
    } else {
        newAnim = Stand;
    }

    if (newAnim != anim) {
        anim = newAnim;
        animTime = 0;
    }
}

void Player::render() {
    loadModel();

    if (modelLoaded) {
        auto& r = Engine::instance().renderer();
        auto& game = Engine::instance().game();
        const bool firstPerson = game.state() == Game::Playing &&
            !game.isMapperMode() && !game.isDemoPlaying() && !game.isFreeCamActive();

        // Build model transform
        MatrixF model;
        Point3F ax = {0, 1, 0};
        model.setRotationAxis(ax, -rot.z);
        model.setTranslation(pos);

        if (!firstPerson) {
            r.setModel(model * modelShape.upOrientation());
            const char* animNames[] = { "stand", "run", "jump", "jet", "death" };
            const char* altNames[]  = { "idle",  "run", "jump", "jet", "die"   };
            int idx = (int)anim;
            if (idx >= 0 && idx <= 4) {
                bool found = false;
                for (auto& a : modelShape.animations)
                    if (a.name == animNames[idx]) { found = true; break; }
                modelShape.renderAnimation(found ? animNames[idx] : altNames[idx], animTime);
            } else {
                modelShape.render(0);
            }
        }

        if (weaponLoaded) {
            int weaponMount = weaponShape.findNode("Mountpoint");
            if (weaponMount < 0) weaponMount = weaponShape.findNode("mount0");
            MatrixF weaponModel;
            if (firstPerson) {
                weaponModel = r.viewMatrix().inverse();
                Point3F camera = r.cameraPos;
                Point3F right = weaponModel.transform({1, 0, 0});
                right.x -= camera.x; right.y -= camera.y; right.z -= camera.z;
                Point3F forward = {r.cameraTarget.x - camera.x,
                                   r.cameraTarget.y - camera.y,
                                   r.cameraTarget.z - camera.z};
                float length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
                if (length > 0.0001f) {
                    forward.x /= length; forward.y /= length; forward.z /= length;
                }
                Point3F weaponPos = {camera.x + forward.x * 0.55f + right.x * 0.35f,
                                     camera.y + forward.y * 0.55f + right.y * 0.35f - 0.18f,
                                     camera.z + forward.z * 0.55f + right.z * 0.35f};
                weaponModel.setTranslation(weaponPos);
                MatrixF weaponFrame;
                weaponFrame.setRotationY(Math::PI);
                weaponModel = weaponModel * weaponFrame;
            } else {
                int playerMount = modelShape.findNode("Mount0");
                if (playerMount < 0) playerMount = modelShape.findNode("Mount1");
                weaponModel = model;
                if (playerMount >= 0 && weaponMount >= 0 &&
                    playerMount < (int)modelShape.defaultTransforms.size() &&
                    weaponMount < (int)weaponShape.defaultTransforms.size()) {
                    weaponModel = model * modelShape.defaultTransforms[playerMount] *
                        weaponShape.defaultTransforms[weaponMount].inverse();
                }
            }
            r.setModel(weaponModel);
            const Weapon& currentWeaponState = weapons[curWeapon];
            const char* animationNames[] = {
                currentWeaponState.reloading ? "reload" :
                    (currentWeaponState.fireTimer > 0.0f ? "fire" : "idle"),
                "ambient", "spin", "discSpin", "stand"
            };
            const DTSShape::Animation* animation = nullptr;
            for (const char* name : animationNames) {
                animation = findAnimation(weaponShape, name);
                if (animation) break;
            }
            if (animation)
                weaponShape.renderAnimation(animation->name.c_str(), weaponAnimTime);
            else
                weaponShape.render(0);
        }
    }
}

void Player::applyMove(const Point3F& move, bool jump, bool jet, float dt) {
    if (isDead()) return;
    Game::InputMove input;
    input.left = move.x < -0.001f;
    input.right = move.x > 0.001f;
    input.backward = move.y < -0.001f;
    input.forward = move.y > 0.001f;
    input.jump = jump;
    input.jet = jet;
    Physics physics;
    physics.update(this, std::max(0.0f, dt), input);
}

void Player::selectWeapon(int32_t idx) {
    if (idx >= 0 && idx < (int32_t)weapons.size() && weaponIsSelectable(weapons[idx])) {
        curWeapon = idx;
        weapons[curWeapon].firing = false;
        weapons[curWeapon].reloading = false;
        weapons[curWeapon].reloadTimer = 0;
        loadWeaponModel();
        if (auto* hud = Engine::instance().guiRenderer().findControl("weaponsHud")) {
            std::string nativeName = gWeaponTable[weapons[curWeapon].type].name;
            if (nativeName == "Spinfusor") nativeName = "Disc";
            else if (nativeName == "PlasmaGun") nativeName = "Plasma";
            else if (nativeName == "ELF") nativeName = "ELFGun";
            hud->activeHudSlot = -1;
            for (size_t slot = 0; slot < hud->hudSlots.size(); ++slot) {
                hud->hudSlots[slot].active = hud->hudSlots[slot].name == nativeName;
                if (hud->hudSlots[slot].active) {
                    hud->hudSlots[slot].amount = weapons[curWeapon].ammo;
                    hud->activeHudSlot = (int)slot;
                }
            }
        }
    }
}

void Player::updateWeaponHud() {
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    auto* hud = Engine::instance().guiRenderer().findControl("weaponsHud");
    if (!hud) return;
    std::string nativeName = gWeaponTable[weapons[curWeapon].type].name;
    if (nativeName == "Spinfusor") nativeName = "Disc";
    else if (nativeName == "PlasmaGun") nativeName = "Plasma";
    else if (nativeName == "ELF") nativeName = "ELFGun";
    for (size_t slot = 0; slot < hud->hudSlots.size(); ++slot) {
        if (hud->hudSlots[slot].name == nativeName) {
            hud->hudSlots[slot].amount = weapons[curWeapon].ammo;
            hud->hudSlots[slot].active = true;
            hud->activeHudSlot = (int)slot;
        }
    }
}

void Player::fireWeapon(bool alt) {
    if (isDead()) return;
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    Weapon& w = weapons[curWeapon];
    const WeaponData& wd = gWeaponTable[w.type];
    if (alt != wd.altFire) return;
    if (!w.canFire(eng)) return;

    eng -= wd.energyCost;
    w.fireTimer = wd.fireRate;
    w.firing = true;
    weaponAnimTime = 0.0f;

    Point3F cpos = cameraPos();
    Point3F target = cameraTarget();
    Point3F dir = {target.x - cpos.x, target.y - cpos.y, target.z - cpos.z};
    float dlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 0.001f) return;
    dir.x /= dlen; dir.y /= dlen; dir.z /= dlen;

    Projectile p;
    p.pos = computeProjectileSpawn(cpos, dir);
    p.previousPos = p.pos;
    p.vel = {dir.x * wd.speed, dir.y * wd.speed, dir.z * wd.speed};
    p.type = wd.projectileType;
    p.damage = wd.damage;
    p.splashRadius = wd.splashRadius;
    p.lifetime = 5.0f;
    p.active = true;
    p.ownerId = 0;
    p.weaponType = curWeapon;

    if (wd.projectileType == ProjectileType::Hitscan) {
        p.lifetime = 2.0f;
        p.vel = {dir.x * 500.0f, dir.y * 500.0f, dir.z * 500.0f};
    }

    Engine::instance().game().world().spawnProjectile(p);

    // Play fire sound
    loadWeaponSounds(w);
    if (w.fireSound) {
        auto& audio = Engine::instance().audio();
        auto* src = audio.createSource();
        if (src) {
            src->setPosition(pos);
            src->positional = true;
            src->setVolume(0.5f);
            src->play(w.fireSound);
            // Sources auto-clean via audio system update
        }
    }

    if (wd.maxAmmo > 0) {
        w.ammo--;
        if (w.ammo <= 0) w.ammo = 0;
    }
    updateWeaponHud();
}

void Player::weaponCycle(int32_t dir) {
    if (weapons.empty()) return;
    int32_t next = nextSelectableWeapon(weapons, curWeapon, dir);
    selectWeapon(next);
}

void Player::applyDamage(float amount) {
    // A dead ShapeBase cannot take a second lethal hit before respawn.
    if (hp <= 0.0f && amount >= 0.0f) return;
    if (amount < 0) {
        // Healing
        hp -= amount; // amount is negative, so this adds
        if (hp > 100) hp = 100;
        return;
    }
    if (arm > 0) {
        float absorbed = Math::min(arm, amount * 0.6f);
        arm -= absorbed;
        hp -= amount - absorbed;
    } else {
        hp -= amount;
    }
    if (hp <= 0) {
        hp = 0;
        deaths++;
    }
}

void Player::respawn() {
    hp = 100.0f;
    eng = 100.0f;
    repairRate = 0.0f;
    heatLevel = 0.0f;
    arm = 0.0f;
    vel = {0,0,0};
    pos = Engine::instance().game().world().spawnPoint();
    pos.y += 1.0f;
    onGround = false;
    curWeapon = weapons.empty() ? -1 : 0;
    for (auto& weapon : weapons) {
        weapon.ammo = weapon.type >= 0 && weapon.type < gWeaponCount &&
            gWeaponTable[weapon.type].maxAmmo > 0
            ? gWeaponTable[weapon.type].maxAmmo : 9999;
        weapon.fireTimer = 0.0f;
        weapon.reloadTimer = 0.0f;
        weapon.firing = false;
        weapon.reloading = false;
    }
    loadWeaponModel();
    updateWeaponHud();
}

Point3F Player::cameraPos() const {
    return {pos.x, pos.y + eyeHeight, pos.z};
}

Point3F Player::cameraTarget() const {
    float cx = pos.x + std::sin(rot.z) * std::cos(rot.x);
    float cy = pos.y + eyeHeight + std::sin(rot.x);
    float cz = pos.z + std::cos(rot.z) * std::cos(rot.x);
    return {cx, cy, cz};
}

World::World() {}
World::~World() {}

static int findWaterBody(const std::vector<World::WaterState>& bodies,
                         const std::string& target) {
    if (target.empty()) {
        for (size_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].active) return (int)i;
        return -1;
    }
    char* end = nullptr;
    const long index = std::strtol(target.c_str(), &end, 10);
    if (end != target.c_str() && *end == '\0' && index >= 0 &&
        index < (long)bodies.size() && bodies[index].active)
        return (int)index;
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].active && bodies[i].name == target) return (int)i;
    return -1;
}

bool World::setWaterLevel(const std::string& target, float level) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterLevel(waterBodies[index].level, level)) return false;
    if (index == 0) water = waterBodies[index];
    return true;
}

bool World::setWaterType(const std::string& target, int type) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || type < 0 || type > 7) return false;
    waterBodies[index].liquidType = type;
    if (index == 0) water.liquidType = type;
    return true;
}

bool World::setWaterOpacity(const std::string& target, float opacity) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterOpacity(waterBodies[index].opacity, opacity)) return false;
    waterBodies[index].surfaceColor.a = opacity;
    if (index == 0) water = waterBodies[index];
    return true;
}

bool World::setWaterColor(const std::string& target, const ColorF& color) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterColor(waterBodies[index].surfaceColor, color.r, color.g, color.b)) return false;
    waterBodies[index].surfaceColor.a = color.a;
    waterBodies[index].opacity = color.a;
    if (index == 0) water = waterBodies[index];
    return true;
}

bool World::setSkyColor(const ColorF& color) {
    if (!validEnvironmentColor(color)) return false;
    skyBox.solidColor = color;
    skyBox.useSkyTextures = false;
    return true;
}

bool World::setSkyMaterialList(const std::string& materialList) {
    if (materialList.empty() || materialList.find("..") != std::string::npos ||
        materialList.find('\\') != std::string::npos || materialList.front() == '/')
        return false;
    skyMaterialList = materialList;
    return true;
}

bool World::setSunDirection(const Point3F& direction) {
    if (!validSunDirection(direction)) return false;
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    sunLightDir = {direction.x / length, direction.y / length, direction.z / length};
    sunLightDirUsed = true;
    return true;
}

bool World::setSunColor(const ColorF& color) {
    if (!validEnvironmentColor(color)) return false;
    sunColor = color;
    sunColorUsed = true;
    return true;
}

bool World::setSunAmbient(const ColorF& color) {
    if (!validEnvironmentColor(color)) return false;
    sunAmbient = color;
    return true;
}

bool World::setFogTransition(float duration, float distance, const ColorF* color) {
    if (!std::isfinite(duration) || duration < 0.0f ||
        !std::isfinite(distance) || distance <= 0.0f) return false;
    const float targetDensity = 1.0f / distance;
    if (color && !validEnvironmentColor(*color)) return false;
    fog.transitionStartDensity = fog.density;
    fog.transitionTargetDensity = targetDensity;
    fog.transitionStartColor = fog.color;
    fog.transitionTargetColor = color ? *color : fog.color;
    fog.transitionElapsed = 0.0f;
    fog.transitionDuration = duration;
    fog.transitioning = duration > 0.0f;
    fog.enabled = true;
    if (!fog.transitioning) {
        fog.density = targetDensity;
        fog.distance = distance;
        fog.color = fog.transitionTargetColor;
    }
    return true;
}

void World::cleanupMission() {
    if (auto* ts = Engine::instance().script().ts()) {
        for (const auto& object : worldObjects) {
            if (object.objectName.empty()) continue;
            if (object.className == "StaticShape" || object.className == "TSStatic" ||
                object.className == "Turret" || object.className.ends_with("Turret") ||
                object.className == "Item" ||
                object.className == "Player" || object.className == "Vehicle" ||
                object.className.ends_with("Vehicle"))
                ts->callFunction(object.className + "::onRemove", {VMValue(object.objectName)});
        }
        // An onRemove callback may schedule work, but mission-owned work must
        // never survive the removal pass.
        ts->clearScheduledEvents();
    }
    clearEffects();
    terrainBlock.reset();
    skyBox.reset();
    if (SDL_GL_GetCurrentContext()) {
        for (auto& shape : shapes) {
            for (auto& mesh : shape.meshes) mesh.destroy();
            for (auto& texture : shape.materialTextures) texture.destroy();
            for (auto& texture : shape.lightmaps) texture.destroy();
        }
        for (auto& shape : debrisShapes) {
            for (auto& mesh : shape.meshes) mesh.destroy();
            for (auto& texture : shape.materialTextures) texture.destroy();
            for (auto& texture : shape.lightmaps) texture.destroy();
        }
    }
    worldObjects.clear();
    missionObjectives.clear();
    navGraph = {};
    interiorCollision = {};
    currentSceneState = {};
    cameras.clear();
    shapes.clear();
    debrisShapes.clear();
    projList.clear();
    fogVolumes.clear();
    skyMaterialList.clear();
    fog = {};
    visibleDistance = 1000.0f;
    sunLightDir = {0.5f, 0.8f, 0.6f};
    sunColor = {1, 1, 1, 1};
    sunAmbient = {0.3f, 0.3f, 0.4f, 1.0f};
    sunLightDirUsed = false;
    sunColorUsed = false;
    missionArea = {};
    precipitation = {};
    lightningEnabled = true;
    setTorchWindVelocity({});
    water = {};
    waterBodies.clear();
    loaded = false;
    auto& config = Engine::instance().renderer().config();
    config.fogDensity = -1.0f;
    config.fogColorOverride = false;
}

bool World::load(const char* mapName) {
    Console::instance().printf(LogLevel::Info, "Loading map: %s", mapName);

    if (auto* ts = Engine::instance().script().ts()) ts->clearScheduledEvents();
    // Mission-owned effects must not survive a V12 scene replacement.
    clearEffects();

    // A mission load replaces the native scene graph. Do not let static world
    // geometry or terrain from the previous mission survive a transition.
    terrainBlock.reset();
    skyBox.reset();
    if (SDL_GL_GetCurrentContext()) {
        for (auto& shape : shapes) {
            for (auto& mesh : shape.meshes) mesh.destroy();
            for (auto& texture : shape.materialTextures) texture.destroy();
            for (auto& texture : shape.lightmaps) texture.destroy();
        }
        for (auto& shape : debrisShapes) {
            for (auto& mesh : shape.meshes) mesh.destroy();
            for (auto& texture : shape.materialTextures) texture.destroy();
            for (auto& texture : shape.lightmaps) texture.destroy();
        }
    }
    shapes.clear();
    debrisShapes.clear();
    worldObjects.clear();
    missionObjectives.clear();
    navGraph = {};
    interiorCollision = {};
    currentSceneState = {};
    cameras.clear();
    fogVolumes.clear();
    // Sky and precipitation are mission-owned state.  Reset every field before
    // parsing the next mission; V12 creates a fresh environment object here.
    skyMaterialList.clear();
    skyBox.fogColor = {0.75f, 0.8f, 0.85f, 1.0f};
    skyBox.visibleDistance = 1000.0f;
    skyBox.solidColor = {0, 0, 0, 1};
    skyBox.useSkyTextures = true;
    fog = {};
    auto& fogConfig = Engine::instance().renderer().config();
    fogConfig.fogDensity = -1.0f;
    fogConfig.fogColorOverride = false;
    visibleDistance = 1000.0f;
    sunLightDir = {0.5f, 0.8f, 0.6f};
    sunColor = {1, 1, 1, 1};
    sunAmbient = {0.3f, 0.3f, 0.4f, 1.0f};
    sunLightDirUsed = false;
    sunColorUsed = false;
    setTorchWindVelocity({});
    missionArea = {};
    precipitation = {};
    lightningEnabled = true;
    water = {};
    waterBodies.clear();
    playerSpawn = {0, 5, 0};
    loaded = false;

    auto& fs = Engine::instance().fs();

    // Try to load mission file
    if (mapName && !TorchPath::isSafeLogicalPath(mapName)) {
        Console::instance().printf(LogLevel::Error, "Rejected unsafe mission argument: %s", mapName);
        return false;
    }
    std::string missionName = mapName ? mapName : "";
    missionName = normalizeMissionName(missionName);
    if (!TorchPath::isSafeLogicalPath(missionName.c_str())) {
        Console::instance().printf(LogLevel::Error, "Rejected unsafe mission path: %s", mapName ? mapName : "");
        return false;
    }
    std::string misPath = std::string("missions/") + missionName + ".mis";
    std::string misData = fs.readText(misPath.c_str());

    if (misData.empty()) {
        // FileSystem resolves case-insensitively for both loose and archived
        // assets; try the packed mission form without reintroducing raw input.
        misPath = std::string("missions/") + missionName + ".misPK";
        misData = fs.readText(misPath.c_str());
    }

    // Cloud layer properties (populated from Sky object if .mis available)
    float cloudHeights[3] = {0.7f, 0.5f, 0.3f};
    float cloudSpeeds[3] = {0.3f, 0.15f, 0.08f};
    bool missionHasTerrainBlock = false;

    if (!misData.empty()) {
        Console::instance().printf(LogLevel::Info, "Found mission: %s (%zu bytes, first 30: '%s')", misPath.c_str(), misData.size(),
            misData.substr(0, 30).c_str());

        auto objects = parseMisFile(misData);

        if (const MisObject* graph = findObject(objects, "NavigationGraph"))
            navGraph = authoredNavigationGraph(*graph);
        for (const auto& object : objects)
            if (object.className == "AIObjective")
                missionObjectives.push_back(authoredMissionObjective(object));
        Console::instance().printf(LogLevel::Info, "  navigation graph: %s, objectives: %zu",
            navGraph.graphFile.empty() ? "none" : navGraph.graphFile.c_str(), missionObjectives.size());

        for (const auto& obj : objects) {
            if (obj.className != "Camera") continue;
            std::string dataBlock = getProp(obj.props, "datablock");
            for (char& c : dataBlock)
                c = (char)std::tolower((unsigned char)c);
            if (dataBlock != "observer") continue;

            ObserverCamera camera;
            camera.pos = parsePos(getProp(obj.props, "position"));
            float values[4] = {0, 0, 1, 0};
            const std::string rotation = getProp(obj.props, "rotation");
            if (sscanf(rotation.c_str(), "%f %f %f %f",
                       &values[0], &values[1], &values[2], &values[3]) >= 3) {
                camera.axis = {values[0], values[1], values[2]};
                camera.angleDeg = values[3];
            }
            cameras.push_back(camera);
        }
        Console::instance().printf(LogLevel::Info,
            "  observer cameras: %zu", cameras.size());

        // Find TerrainBlock
        MisObject* terrainObj = findObject(objects, "TerrainBlock");
        if (terrainObj) {
            missionHasTerrainBlock = true;
            std::string terrainFile = getProp(terrainObj->props, "terrainfile");
            Console::instance().printf(LogLevel::Info, "  terrain file: '%s'", terrainFile.c_str());

            // Try loading the .ter file from various paths
            const std::vector<std::string> terPaths = terrainAssetCandidates(terrainFile);

            // Read terrain positioning
            std::string sqStr = getProp(terrainObj->props, "squaresize");
            if (!sqStr.empty()) terrainBlock.squareSize = (float)std::atof(sqStr.c_str());
            terrainBlock.setEmptySquareRuns(
                parseEmptySquareRuns(getProp(terrainObj->props, "emptysquares")));
            Console::instance().printf(LogLevel::Debug, "  terrain squareSize: %.1f", terrainBlock.squareSize);
            std::string hsStr = getProp(terrainObj->props, "heightscale");
            if (!hsStr.empty()) terrainBlock.heightScale = (float)std::atof(hsStr.c_str());
            std::string posStr = getProp(terrainObj->props, "position");
            if (!posStr.empty()) {
                float px, py, pz;
                if (sscanf(posStr.c_str(), "%f %f %f", &px, &py, &pz) == 3) {
                    terrainBlock.worldOffset = {px, pz, -py};
                    Console::instance().printf(LogLevel::Debug, "  terrain position: %.1f %.1f %.1f", px, py, pz);
                }
            }

            for (auto& tp : terPaths) {
                auto terData = fs.read(tp.c_str());
                if (!terData.empty()) {
                    Console::instance().printf(LogLevel::Info, "  loaded terrain: %s", tp.c_str());
                    terrainBlock.load(terData.data(), terData.size());
                    break;
                }
            }

            // Override detail textures from .mis if specified
            for (int i = 0; i < 4; i++) {
                std::string key = "detailtex" + std::to_string(i + 1);
                std::string texName = getProp(terrainObj->props, key.c_str());
                if (!texName.empty() && i < (int)terrainBlock.textureNames.size()) {
                    terrainBlock.textureNames[i] = texName;
                    // Reload the texture
                    std::vector<std::string> exts = {".png", ".jpg", ".bmp", ".bm8", ".gif"};
                    bool found = false;
                    for (auto& ext : exts) {
                        std::string path = "textures/" + texName + ext;
                        auto td = fs.read(path.c_str());
                        if (!td.empty()) {
                            terrainBlock.detailTextures[i].load(td.data(), td.size());
                            if (terrainBlock.detailTextures[i].loaded) {
                                Console::instance().printf(LogLevel::Info, "  detail texture %d overridden: %s", i, path.c_str());
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        for (auto& ext : exts) {
                            std::string path = texName + ext;
                            auto td = fs.read(path.c_str());
                            if (!td.empty()) {
                                terrainBlock.detailTextures[i].load(td.data(), td.size());
                                if (terrainBlock.detailTextures[i].loaded) {
                                    Console::instance().printf(LogLevel::Info, "  detail texture %d overridden: %s", i, path.c_str());
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Read per-layer tiling from .mis
            for (int i = 0; i < 4; i++) {
                std::string key = "texscale" + std::to_string(i + 1);
                std::string val = getProp(terrainObj->props, key.c_str());
                if (!val.empty()) terrainBlock.detailTilings[i] = (float)std::atof(val.c_str());
                // Also try "texTiling" variant
                if (terrainBlock.detailTilings[i] == 0) {
                    key = "textiling" + std::to_string(i + 1);
                    val = getProp(terrainObj->props, key.c_str());
                    if (!val.empty()) terrainBlock.detailTilings[i] = (float)std::atof(val.c_str());
                }
            }

            if (!terrainBlock.loaded)
                Console::instance().printf(LogLevel::Warn,
                    "Terrain: required asset '%s' could not be resolved", terrainFile.c_str());
        }

        // Load GameGrid texture for mission area boundary rendering
        {
            auto gridData = fs.read("textures/special/GameGrid.png");
            if (!gridData.empty()) {
                terrainBlock.gameGrid.load(gridData.data(), gridData.size());
                if (!terrainBlock.gameGrid.loaded)
                    Console::instance().printf(LogLevel::Warn, "Failed to load GameGrid.png");
            }
        }

        // Find Sky
        MisObject* skyObj = findObject(objects, "Sky");
        if (skyObj) {
            skyMaterialList = getProp(skyObj->props, "materialList");
            Console::instance().printf(LogLevel::Info, "  has Sky object, materialList: '%s'", skyMaterialList.c_str());

            // Parse fog from Sky
            std::string fogStr = getProp(skyObj->props, "fogcolor");
            if (!fogStr.empty()) {
                float fr, fg, fb;
                if (sscanf(fogStr.c_str(), "%f %f %f", &fr, &fg, &fb) >= 3) {
                    fog.color = {fr, fg, fb, 1.0f};
                    fog.enabled = true;
                }
            }
            std::string visibleDist = getProp(skyObj->props, "visibledistance");
            if (!visibleDist.empty()) visibleDistance = std::max(0.0f, (float)std::atof(visibleDist.c_str()));
            skyBox.visibleDistance = visibleDistance;
            skyBox.fogColor = fog.color;
            std::string windStr = getProp(skyObj->props, "windVelocity");
            if (!windStr.empty()) {
                float wx, wy, wz;
                if (sscanf(windStr.c_str(), "%f %f %f", &wx, &wy, &wz) == 3)
                    setTorchWindVelocity(Math::torquePointToYUp({wx, wy, wz}));
            }
            std::string fogDist = getProp(skyObj->props, "fogdistance");
            if (!fogDist.empty()) {
                float distance = (float)std::atof(fogDist.c_str());
                if (applyFogDistance(fog.distance, fog.density, distance))
                    fog.enabled = true;
            }
            std::string useSkyTextures = getProp(skyObj->props, "useskytextures");
            if (!useSkyTextures.empty())
                skyBox.useSkyTextures = std::atoi(useSkyTextures.c_str()) != 0;
            for (int i = 1; i <= 3; ++i) {
                const std::string volume = getProp(
                    skyObj->props, ("fogVolume" + std::to_string(i)).c_str());
                float distance = 0.0f, minHeight = 0.0f, maxHeight = 0.0f;
                if (sscanf(volume.c_str(), "%f %f %f", &distance, &minHeight,
                           &maxHeight) == 3 && distance > 0.0f &&
                    maxHeight > minHeight) {
                    fogVolumes.push_back({distance, minHeight, maxHeight});
                }
            }
            skyBox.fogVolumes.clear();
            for (const auto& volume : fogVolumes)
                skyBox.fogVolumes.push_back({volume.visibleDistance, volume.minHeight,
                                             volume.maxHeight, 1.0f});
            std::string solidColor = getProp(skyObj->props, "skysolidcolor");
            if (!solidColor.empty()) {
                float sr, sg, sb;
                if (sscanf(solidColor.c_str(), "%f %f %f", &sr, &sg, &sb) >= 3)
                    skyBox.solidColor = {sr, sg, sb, 1.0f};
            }
            skyBox.fogColor = fog.color;
            // setFogDensity / setFogColor console overrides take precedence over
            // the mission's Sky fog values.
            auto& fogCfg = Engine::instance().renderer().config();
            if (fogCfg.fogDensity >= 0.0f) {
                fog.density = fogCfg.fogDensity;
                fog.distance = (fogCfg.fogDensity > 0.0f) ? 1.0f / fogCfg.fogDensity : 0.0f;
                fog.enabled = true;
            }
            if (fogCfg.fogColorOverride) {
                fog.color = fogCfg.fogColor;
                fog.enabled = true;
            }
            Console::instance().printf(LogLevel::Debug, "  fog: enabled=%d color=(%.2f %.2f %.2f) density=%.4f dist=%.0f",
                fog.enabled, fog.color.r, fog.color.g, fog.color.b, fog.density, fog.distance);

            // Read cloud layer properties
            for (int ci = 0; ci < 3; ci++) {
                std::string hKey = "cloudheightper[" + std::to_string(ci) + "]";
                std::string hStr = getProp(skyObj->props, hKey.c_str());
                if (!hStr.empty()) cloudHeights[ci] = (float)std::atof(hStr.c_str());
                std::string sKey = "cloudspeed" + std::to_string(ci + 1);
                std::string sStr = getProp(skyObj->props, sKey.c_str());
                if (!sStr.empty()) cloudSpeeds[ci] = (float)std::atof(sStr.c_str()) * 1000.0f;
            }
        }

        // Parse Sun from mission for dynamic lighting
        MisObject* sunObj = findObject(objects, "Sun");
        if (sunObj) {
            std::string azStr = getProp(sunObj->props, "azimuth");
            std::string elStr = getProp(sunObj->props, "elevation");
            std::string colStr = getProp(sunObj->props, "color");
            if (!azStr.empty() && !elStr.empty()) {
                float azimuth = (float)std::atof(azStr.c_str()) * (3.14159f / 180.0f);
                float elevation = (float)std::atof(elStr.c_str()) * (3.14159f / 180.0f);
                sunLightDir.x = cosf(elevation) * sinf(azimuth);
                sunLightDir.y = sinf(elevation);
                sunLightDir.z = cosf(elevation) * cosf(azimuth);
                sunLightDirUsed = true;
                Console::instance().printf(LogLevel::Debug, "  sun: azimuth=%.0f elevation=%.0f dir=(%.2f %.2f %.2f)",
                    std::atof(azStr.c_str()), std::atof(elStr.c_str()), sunLightDir.x, sunLightDir.y, sunLightDir.z);
            }
            if (!colStr.empty()) {
                float r, g, b;
                if (sscanf(colStr.c_str(), "%f %f %f", &r, &g, &b) == 3 &&
                    validEnvironmentColor({r, g, b, 1.0f})) {
                    sunColor = {r, g, b, 1.0f};
                    sunColorUsed = true;
                }
            }
            std::string ambStr = getProp(sunObj->props, "ambient");
            if (!ambStr.empty()) {
                float ar, ag, ab;
                if (sscanf(ambStr.c_str(), "%f %f %f", &ar, &ag, &ab) >= 1) {
                    sunAmbient = {ar, ag, ab, 1.0f};
                }
            }

            // Terrain baked lightmap uses the mission sun direction
            if (sunLightDirUsed && terrainBlock.loaded) {
                terrainBlock.lightDir = sunLightDir;
                terrainBlock.bakeLightmap();
            }
        }

        // Parse MissionArea from mission (for boundary visualization)
        for (auto& obj : objects) {
            if (obj.className == "MissionArea") {
                std::string areaStr = getProp(obj.props, "area");
                if (!areaStr.empty()) {
                    float ax, ay, aw, ah;
                    if (sscanf(areaStr.c_str(), "%f %f %f %f", &ax, &ay, &aw, &ah) == 4) {
                        // T2: x=east, y=north/south, z=up → engine: x=east, y=up, z=south
                        missionArea.valid = true;
                        missionArea.x = ax;
                        missionArea.z = -ay;  // T2 y → engine Z (negated: north↔south)
                        missionArea.width = aw;
                        missionArea.height = ah;
                        Console::instance().printf(LogLevel::Info,
                            "  mission area: x=%.0f z=%.0f w=%.0f h=%.0f",
                            missionArea.x, missionArea.z, missionArea.width, missionArea.height);
                    }
                }
                break;
            }
        }

        // Parse Precipitation from mission
        for (auto& obj : objects) {
            if (obj.className == "Precipitation") {
                PrecipitationState ps;
                std::string s;
                // Native V12 names; retain the newer names as aliases.
                s = getProp(obj.props, "maxNumDrops");
                if (s.empty()) s = getProp(obj.props, "numDrops");
                 if (!s.empty()) ps.numDrops = std::atoi(s.c_str());
                 ps.configuredDrops = ps.numDrops;
                 s = getProp(obj.props, "type"); if (!s.empty()) ps.type = std::atoi(s.c_str());
                 s = getProp(obj.props, "percentage"); if (!s.empty()) ps.percentage = (float)std::atof(s.c_str());
                s = getProp(obj.props, "maxRadius");
                if (!s.empty()) ps.boxWidth = std::max(1.0f, (float)std::atof(s.c_str()) * 2.0f);
                s = getProp(obj.props, "boxWidth"); if (!s.empty()) ps.boxWidth = (float)std::atof(s.c_str());
                s = getProp(obj.props, "boxHeight"); if (!s.empty()) ps.boxHeight = (float)std::atof(s.c_str());
                s = getProp(obj.props, "dropSize"); if (!s.empty()) ps.dropSize = (float)std::atof(s.c_str());
                s = getProp(obj.props, "minVelocity");
                if (s.empty()) s = getProp(obj.props, "minSpeed");
                if (!s.empty()) ps.minSpeed = (float)std::atof(s.c_str());
                s = getProp(obj.props, "maxVelocity");
                if (s.empty()) s = getProp(obj.props, "maxSpeed");
                if (!s.empty()) ps.maxSpeed = (float)std::atof(s.c_str());
                s = getProp(obj.props, "position");
                if (!s.empty()) ps.origin = Math::torquePointToYUp(parsePos(s));
                s = getProp(obj.props, "color1");
                if (!s.empty()) {
                    float r, g, b, a = ps.color.a;
                    if (sscanf(s.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)
                        ps.color = {r, g, b, a};
                }
                s = getProp(obj.props, "followCam");
                if (!s.empty()) ps.followCam = (std::atoi(s.c_str()) != 0);
                s = getProp(obj.props, "useWind");
                if (!s.empty()) ps.useWind = (std::atoi(s.c_str()) != 0);
                s = getProp(obj.props, "textureName");
                if (s.empty()) s = getProp(obj.props, "texture");
                if (!s.empty()) {
                    std::vector<float> durations;
                    Engine::instance().renderer().loadTextureFrames(
                        s.c_str(), ps.textures, durations);
                    ps.textureDurations = std::move(durations);
                }
                if (ps.numDrops > 0 && ps.maxSpeed > 0) {
                    ps.active = true;
                    precipitation = ps;
                    precipitation.configuredDrops = ps.numDrops;
                    if (!setPrecipitation(ps.type, ps.percentage)) precipitation = {};
                    Console::instance().printf(LogLevel::Info, "  Precipitation: %d drops, box=%.0fx%.0f, speed=%.1f-%.1f",
                        precipitation.numDrops, precipitation.boxWidth, precipitation.boxHeight,
                        precipitation.minSpeed, precipitation.maxSpeed);
                }
                break;
            }
        }

        // Parse every WaterBlock. WaterBlock positions are the lower-left
        // corner of the fluid region, not the center of the rendered plane.
        for (auto& obj : objects) {
            if (obj.className == "WaterBlock") {
                 WaterState body;
                 body.name = obj.objName;
                float scaleZ = 0.0f;
                std::string scaleStr = getProp(obj.props, "scale");
                if (!scaleStr.empty()) {
                    float sx, sy, sz;
                    if (sscanf(scaleStr.c_str(), "%f %f %f", &sx, &sy, &sz) >= 3) {
                        scaleZ = sz;
                        body.sizeX = std::max(0.0f, sx);
                        body.sizeY = std::max(0.0f, sy);
                        body.size = std::max(body.sizeX, body.sizeY);
                    }
                }
                std::string posStr = getProp(obj.props, "position");
                 if (!posStr.empty()) {
                     float px, py, pz;
                     if (sscanf(posStr.c_str(), "%f %f %f", &px, &py, &pz) == 3 &&
                         std::isfinite(px) && std::isfinite(py) && std::isfinite(pz) &&
                         body.sizeX > 0.0f && body.sizeY > 0.0f) {
                        body.originX = px;
                        body.originZ = -py;
                        body.level = pz + scaleZ;
                        body.active = true;
                    }
                 }
                 std::string liquidType = getProp(obj.props, "liquidType");
                 int parsedLiquidType = body.liquidType;
                 if (!liquidType.empty() && !parseWaterType(liquidType, parsedLiquidType)) {
                     Console::instance().printf(LogLevel::Warn,
                         "WaterBlock '%s': rejected liquidType '%s'", obj.objName.c_str(), liquidType.c_str());
                     continue;
                 }
                 body.liquidType = parsedLiquidType;
                 std::string liquidLower = liquidType;
                 for (char& c : liquidLower) c = (char)std::tolower((unsigned char)c);
                 if (liquidLower.find("lava") != std::string::npos)
                    body.surfaceColor = {0.75f, 0.12f, 0.02f, body.opacity};
                std::string waveMagnitude = getProp(obj.props, "waveMagnitude");
                if (!waveMagnitude.empty())
                    body.waveMagnitude = std::max(0.0f, (float)std::atof(waveMagnitude.c_str()));
                // These are the native WaterBlock fields; baseColor/opacity are
                // not the surface material controls used by the original engine.
                std::string colorStr = getProp(obj.props, "surfaceColor");
                 if (!colorStr.empty()) {
                     float cr, cg, cb;
                     ColorF parsedColor{};
                     if (sscanf(colorStr.c_str(), "%f %f %f", &cr, &cg, &cb) == 3 &&
                         applyWaterColor(parsedColor, cr, cg, cb)) {
                         body.surfaceColor = {cr, cg, cb, body.opacity};
                     } else {
                         Console::instance().printf(LogLevel::Warn,
                             "WaterBlock '%s': rejected surfaceColor '%s'", obj.objName.c_str(), colorStr.c_str());
                         continue;
                     }
                 }
                std::string opacityStr = getProp(obj.props, "surfaceOpacity");
                if (opacityStr.empty()) opacityStr = getProp(obj.props, "opacity");
                 if (!opacityStr.empty()) {
                     const float opacity = (float)std::atof(opacityStr.c_str());
                     if (!applyWaterOpacity(body.opacity, opacity)) {
                         Console::instance().printf(LogLevel::Warn,
                             "WaterBlock '%s': rejected opacity '%s'", obj.objName.c_str(), opacityStr.c_str());
                         continue;
                     }
                     body.surfaceColor.a = body.opacity;
                }
                auto& renderer = Engine::instance().renderer();
                const std::string surfaceTexture = getProp(obj.props, "surfaceTexture");
                if (!surfaceTexture.empty())
                    renderer.loadTextureFrames(surfaceTexture.c_str(), body.surfaceFrames,
                                               body.surfaceFrameDurations);
                const std::string shoreTexture = getProp(obj.props, "shoreTexture");
                if (!shoreTexture.empty())
                    renderer.loadTextureFrames(shoreTexture.c_str(), body.shoreFrames,
                                               body.shoreFrameDurations);
                const std::string envTexture = getProp(obj.props, "envMapTexture");
                if (!envTexture.empty()) {
                    renderer.loadTextureFrames(envTexture.c_str(), body.envFrames,
                                               body.envFrameDurations);
                }
                const std::string envIntensity = getProp(obj.props, "envMapIntensity");
                if (!envIntensity.empty()) body.envIntensity = std::max(0.0f, (float)std::atof(envIntensity.c_str()));
                const std::string shoreDepth = getProp(obj.props, "shoreDepth");
                if (!shoreDepth.empty()) body.shoreDepth = std::max(0.0f, (float)std::atof(shoreDepth.c_str()));
                if (body.active) {
                    waterBodies.push_back(body);
                    // Keep the legacy summary fields useful to callers that
                    // only need the first authored surface.
                    if (!water.active) water = body;
                    Console::instance().printf(LogLevel::Info, "  WaterBlock: level=%.1f size=%.0f opacity=%.2f",
                        body.level, body.size, body.opacity);
                }
            }
        }

        // Collect all unique shape names referenced in the mission
        std::vector<std::string> shapeNames;
        auto addShapeName = [&](const std::string& n) {
            if (n.empty()) return;
            std::string clean = n;
            // Clean trailing quote from mis parsing
            if (!clean.empty() && clean.back() == '"') clean.pop_back();
            // Strip directory prefix so the loader can add the correct one
             clean = normalizeShapePath(clean);
            if (clean.empty()) return;
            bool found = false;
            for (auto& s : shapeNames) if (s == clean) { found = true; break; }
            if (!found) shapeNames.push_back(clean);
        };

        // Scan TorqueScript .cs files for datablock definitions (InstanceName -> shapeFile)
        scanDatablockShapesFromCS(*this);

        // Also extract inline datablock definitions from the .mis file itself
        // (datablock ClassName(InstanceName) { shapeFile = "path" })
        for (auto& obj : objects) {
            std::string shapeFile = getProp(obj.props, "shapefile");
            if (!shapeFile.empty()) {
                // obj.objName is the InstanceName
                std::string fullPath = shapeFile;
                if (fullPath.find("shapes/") != 0 && fullPath.find("interiors/") != 0)
                    fullPath = "shapes/" + fullPath;
                datablockShapes[obj.objName] = fullPath;
                addShapeName(fullPath);
            }
        }

        // Collect shape paths for all renderable mission objects
        // (TSStatic, InteriorInstance, StaticShape, Turret, Item, Camera, etc.)
        for (auto& obj : objects) {
            if (!isRenderableMissionShape(obj.className) && obj.className != "ForceFieldBare") continue;
            std::string shapePath = resolveShapePath(obj, datablockShapes);
            if (!shapePath.empty()) addShapeName(shapePath);
            if (obj.className == "Turret") {
                std::string barrel = getProp(obj.props, "initialbarrel");
                if (const auto* barrelPath = findDatablockShape(datablockShapes, barrel))
                    addShapeName(*barrelPath);
            }
        }

        // Load all unique shapes
        for (auto& shapeName : shapeNames) {
            DTSShape shape;
            shape.name = shapeName;

             std::string lowerShapeName = shapeName;
             for (char& c : lowerShapeName) c = (char)std::tolower((unsigned char)c);
             shape.isInterior = lowerShapeName.ends_with(".dif");

             std::vector<uint8_t> shapeData = Engine::instance().fs().read(shapeName.c_str());
             if (!shapeData.empty()) shape.load(shapeData.data(), shapeData.size());

    if (shape.loaded) {
        Console::instance().printf(LogLevel::Debug, "  loaded world shape: %s", shapeName.c_str());
            } else {
                    Console::instance().printf(LogLevel::Warn, "World asset not loaded: %s", shapeName.c_str());
            }

            shapes.push_back(std::move(shape));
        }

        // V12 chooses a team-compatible authored sphere. Sort by authored name
        // rather than file order so equivalent missions spawn deterministically.
        if (const MisObject* spawn = selectAuthoredSpawn(objects, 1)) {
            playerSpawn = authoredMissionMarker(*spawn).position;
            Console::instance().printf(LogLevel::Debug, "  spawn point: (%.1f, %.1f, %.1f)",
                                        playerSpawn.x, playerSpawn.y, playerSpawn.z);
        }

        // Build collision mesh from DIF interior shapes
        // Prefer hull collision data when available (more accurate)
        // Collision mesh is built after world objects are placed (see below)

        // Place objects from mission, mapping to loaded shapes
        for (auto& obj : objects) {
             if (obj.className == "AudioEmitter") {
                WorldObject emitter;
                emitter.pos = parsePos(getProp(obj.props, "position"));
                emitter.audioEmitter = true;
                emitter.audioFileName = getProp(obj.props, "filename");
                const std::string volume = getProp(obj.props, "volume");
                const std::string is3D = getProp(obj.props, "is3D");
                const std::string looping = getProp(obj.props, "isLooping");
                const std::string minDistance = getProp(obj.props, "minDistance");
                const std::string maxDistance = getProp(obj.props, "maxDistance");
                if (!volume.empty()) emitter.audioVolume = (float)std::atof(volume.c_str());
                if (!is3D.empty()) emitter.audioIs3D = std::atoi(is3D.c_str()) != 0;
                if (!looping.empty()) emitter.audioIsLooping = std::atoi(looping.c_str()) != 0;
                if (!minDistance.empty()) emitter.audioMinDistance = (float)std::atof(minDistance.c_str());
                if (!maxDistance.empty()) emitter.audioMaxDistance = (float)std::atof(maxDistance.c_str());
                addObject(emitter);
                continue;
            }
             if (obj.className == "Marker" || obj.className == "MissionMarker" ||
                  obj.className == "SpawnSphere" ||
                  obj.className == "Trigger" || obj.className == "PhysicalZone") {
                WorldObject marker;
                  marker.className = obj.className;
                  const AuthoredMissionMarker authored = authoredMissionMarker(obj);
                  marker.teamId = authored.teamId;
                  marker.objectName = obj.objName;
                  marker.pos = authored.position;
                  marker.rot = authored.rotation;
                  marker.rotAngleDeg = authored.rotationAngleDeg;
                  marker.scale = authored.scale;
                  marker.label = authored.label;
                  // Markers are mapper guides, never world collision geometry.
                   marker.collidable = false;
                   marker.visible = authoredVisible(obj);
                  if (obj.className == "Trigger") {
                      std::string pointsText = getProp(obj.props, "polyhedron");
                     if (pointsText.empty()) pointsText = getProp(obj.props, "pointList");
                     if (pointsText.empty()) pointsText = getProp(obj.props, "points");
                     const auto numbers = triggerNumbers(pointsText);
                     std::vector<Point3F> localPoints;
                     for (size_t i = 0; i + 2 < numbers.size(); i += 3)
                         localPoints.push_back({numbers[i], numbers[i + 1], numbers[i + 2]});
                     if (localPoints.size() >= 4)
                         marker.trigger = triggerFromVertices(localPoints);
                     else
                         marker.trigger = triggerBox({marker.scale.x * 0.5f, marker.scale.y * 0.5f,
                                                      marker.scale.z * 0.5f});
                     marker.triggerVolume = true;
                 }
                marker.missionVolume = obj.className == "Trigger" ||
                                       obj.className == "PhysicalZone";
                 if (obj.className == "SpawnSphere") marker.volumeRadius = authored.radius;
                 if (obj.className == "PhysicalZone") {
                    const std::string velocity = getProp(obj.props, "velocityMod");
                    const std::string gravity = getProp(obj.props, "gravityMod");
                    const std::string force = getProp(obj.props, "appliedForce");
                    if (!velocity.empty()) marker.physicalVelocityMod = (float)std::atof(velocity.c_str());
                    if (!gravity.empty()) marker.physicalGravityMod = (float)std::atof(gravity.c_str());
                    if (sscanf(force.c_str(), "%f %f %f", &marker.physicalForce.x,
                               &marker.physicalForce.y, &marker.physicalForce.z) != 3)
                        marker.physicalForce = {};
                     const std::string active = getProp(obj.props, "active");
                     if (!active.empty()) marker.physicalActive = std::atoi(active.c_str()) != 0;
                     marker.trigger = triggerBox({marker.scale.x * 0.5f, marker.scale.y * 0.5f,
                                                  marker.scale.z * 0.5f});
                     marker.triggerVolume = true;
                 }
                 addObject(marker);
                 continue;
             }
             if (obj.className == "AIObjective") {
                 const AuthoredMissionObjective authored = authoredMissionObjective(obj);
                 WorldObject objective;
                 objective.className = obj.className;
                 objective.objectName = obj.objName;
                 objective.pos = authored.marker.position;
                 objective.rot = authored.marker.rotation;
                 objective.rotAngleDeg = authored.marker.rotationAngleDeg;
                 objective.scale = authored.marker.scale;
                 objective.teamId = authored.marker.teamId;
                 objective.label = authored.marker.label;
                 objective.missionObjective = true;
                 objective.objectiveMode = authored.mode;
                 objective.objectiveTarget = authored.targetObject;
                 objective.objectiveTargetId = authored.targetObjectId;
                  objective.objectiveWeight = authored.weight[0];
                  for (int i = 0; i < 4; ++i) objective.objectiveWeights[i] = authored.weight[i];
                  objective.objectiveOffense = authored.offense;
                  objective.objectiveDefense = authored.defense;
                  // AI objectives are activated by mission script callbacks.
                  objective.objectiveActive = false;
                  objective.objectiveState = 0;
                  objective.collidable = false;
                 objective.visible = authoredVisible(obj);
                 addObject(objective);
                 continue;
             }
            // Skip infrastructure / non-renderable classes (handled elsewhere)
            if (obj.className == "SimGroup" || obj.className == "MissionArea" ||
                obj.className == "TerrainBlock" || obj.className == "Sky" ||
                obj.className == "Sun" || obj.className == "WaterBlock" ||
                obj.className == "AudioEmitter" || obj.className == "MissionMarker" ||
                obj.className == "SpawnSphere" || obj.className == "NavigationGraph" ||
                obj.className == "AIObjective" || obj.className == "Trigger" ||
                obj.className == "PhysicalZone" || obj.className == "Precipitation" ||
                obj.className == "ParticleEmitter" || obj.className == "ParticleEmissionDummy" ||
                obj.className == "Explosion" || obj.className == "Lightning")
                continue;

            if (!isRenderableMissionShape(obj.className) && obj.className != "ForceFieldBare") continue;

             WorldObject wo;
             wo.objectName = obj.objName;
             wo.pos = parsePos(getProp(obj.props, "position"));
             wo.visible = authoredVisible(obj);
            {
                std::string rotStr = getProp(obj.props, "rotation");
                float vals[4] = {0,0,1,0};
                int count = sscanf(rotStr.c_str(), "%f %f %f %f", &vals[0], &vals[1], &vals[2], &vals[3]);
                if (count >= 3) {
                    wo.rot = {vals[0], vals[1], vals[2]};
                    wo.rotAngleDeg = (count >= 4) ? vals[3] : 0;
                }
            }
            {
                std::string scaleStr = getProp(obj.props, "scale");
                if (!scaleStr.empty()) {
                    float sx, sy, sz;
                    if (sscanf(scaleStr.c_str(), "%f %f %f", &sx, &sy, &sz) == 3)
                        wo.scale = {sx, sy, sz};
                }
            }
             wo.shapeName = resolveShapePath(obj, datablockShapes);
              wo.shapeName = normalizeShapePath(wo.shapeName);
             wo.animName = authoredSequence(obj);
            if (obj.className == "WayPoint") {
                wo.label = getProp(obj.props, "name");
                wo.collidable = false;
            }
            if (obj.className == "ForceFieldBare") {
                wo.forceField = true;
                wo.translucent = true;
                const ScriptObject* datablockObject = findScriptObject(getProp(obj.props, "datablock"));
                auto datablockField = [&](const char* name) -> const VMValue* {
                    return scriptField(datablockObject, name);
                };
                const std::string color = datablockField("color")
                    ? datablockField("color")->toString() : "";
                float cr, cg, cb;
                if (sscanf(color.c_str(), "%f %f %f", &cr, &cg, &cb) >= 3)
                    wo.forceFieldColor = {cr, cg, cb, 1.0f};
                if (datablockField("baseTranslucency"))
                    wo.forceFieldBaseTranslucency = datablockField("baseTranslucency")->toFloat();
                if (datablockField("umapping"))
                    wo.forceFieldUMapping = datablockField("umapping")->toFloat();
                if (datablockField("vmapping"))
                    wo.forceFieldVMapping = datablockField("vmapping")->toFloat();
                if (datablockField("framesPerSec"))
                    wo.forceFieldFramesPerSec = datablockField("framesPerSec")->toFloat();
                if (datablockField("scrollSpeed"))
                    wo.forceFieldScrollSpeed = datablockField("scrollSpeed")->toFloat();
                const int frameCount = datablockField("numFrames")
                    ? std::clamp((int)datablockField("numFrames")->toFloat(), 1, 64) : 1;
                for (int frame = 0; frame < frameCount; ++frame) {
                    const std::string fieldName = "texture[" + std::to_string(frame) + "]";
                    const VMValue* texture = datablockField(fieldName.c_str());
                    if (!texture) continue;
                    std::vector<float> durations;
                    std::vector<uint32_t> loadedFrames;
                    Engine::instance().renderer().loadTextureFrames(
                        texture->toString().c_str(), loadedFrames, durations);
                    wo.forceFieldFrames.insert(wo.forceFieldFrames.end(), loadedFrames.begin(), loadedFrames.end());
                    if (wo.forceFieldFrameDurations.empty())
                        wo.forceFieldFrameDurations = std::move(durations);
                }
                const std::string open = getProp(obj.props, "fieldopen");
                wo.forceFieldOpen = !open.empty() && std::atoi(open.c_str()) != 0;
                wo.boundsRadius = std::sqrt(wo.scale.x * wo.scale.x +
                                             wo.scale.y * wo.scale.y +
                                             wo.scale.z * wo.scale.z) * 0.5f;
            }
            std::string datablock = getProp(obj.props, "datablock");
            std::string datablockLower = datablock;
            for (char& c : datablockLower)
                c = (char)std::tolower((unsigned char)c);
            if (obj.className == "Item" && datablockLower == "flag") {
                int team = obj.objName.starts_with("Team2") ? 2 : 1;
                std::string teamName;
                if (auto* ts = ScriptEngine::instance().ts()) {
                    teamName = ts->getGlobal(
                        "$Host::teamName[" + std::to_string(team) + "]").toString();
                }
                if (teamName.empty())
                    teamName = team == 2 ? "Inferno" : "Storm";
                wo.label = teamName + " Flag";
                if (Engine::instance().game().isMapperMode())
                    wo.animName.clear();
            }
             wo.collidable = authoredCollidable(obj, obj.className != "Item");
             wo.itemPickup = obj.className == "Item";
            if (obj.className == "WayPoint") wo.collidable = false;

            // Find matching shape
            for (auto& s : shapes) {
                if (s.name == wo.shapeName) {
                    wo.shape = &s;
                    // The DTS sequence owns default object visibility and
                    // assembled geometry for mission shapes such as turrets
                    // and stations. Use its native default sequence in all
                    // world modes; do not infer an animation name externally.
                    if (wo.animName.empty()) wo.animName = defaultMissionAnimation(wo.shape);
                    if (Engine::instance().game().isMapperMode() && !wo.animName.empty()) {
                        if (const auto* animation = findAnimation(*wo.shape, wo.animName.c_str()))
                            wo.animTime = animation->looping
                                ? animation->duration * 0.5f
                                : animation->duration;
                    }
                    break;
                }
            }
            if (obj.className == "Turret") {
                wo.mountedShapeName = getProp(obj.props, "initialbarrel");
                const auto* barrelPath = findDatablockShape(datablockShapes, wo.mountedShapeName);
                std::string mountedPath = barrelPath ? *barrelPath : "";
                if (!mountedPath.empty()) {
                    std::string name = mountedPath;
                     name = normalizeShapePath(name);
                    for (auto& s : shapes)
                        if (s.name == name) { wo.mountedShape = &s; break; }
                }
                Console::instance().printf(LogLevel::Info,
                    "  turret mount: base=%s barrel=%s resolved=%s loaded=%s mount0=%d mountpoint=%d",
                    wo.shapeName.c_str(), wo.mountedShapeName.c_str(), mountedPath.c_str(),
                    wo.mountedShape && wo.mountedShape->loaded ? "yes" : "no",
                     wo.shape ? wo.shape->findNode("mount0") : -1,
                     wo.mountedShape ? wo.mountedShape->findNode("Mountpoint") : -1);
                Console::instance().printf(LogLevel::Info,
                    "  turret transform axis=(%.3f %.3f %.3f) angle=%.1f",
                    wo.rot.x, wo.rot.y, wo.rot.z, wo.rotAngleDeg);
            }
            if (wo.shapeName.find("station_inv") != std::string::npos)
                Console::instance().printf(LogLevel::Info,
                    "  station transform axis=(%.3f %.3f %.3f) angle=%.1f",
                    wo.rot.x, wo.rot.y, wo.rot.z, wo.rotAngleDeg);
            addObject(wo);
            if (wo.shape) {
                Console::instance().printf(LogLevel::Info, "  placed: %s (%s) at (%.1f, %.1f, %.1f)",
                    wo.shapeName.c_str(), obj.className.c_str(), wo.pos.x, wo.pos.y, wo.pos.z);
            } else if (!wo.shapeName.empty()) {
                Console::instance().printf(LogLevel::Debug, "  placed (no shape): %s (%s) at (%.1f, %.1f, %.1f)",
                    wo.shapeName.c_str(), obj.className.c_str(), wo.pos.x, wo.pos.y, wo.pos.z);
            }
        }

        // Mission particle dummies and lightning are runtime effects, not
        // renderable shapes. Resolve their authored datablocks from the
        // already executed TorqueScript object table.
        auto loadParticle = [&](const ScriptObject* object,
                                V12::DecodedDataBlock::ParticleData& particle) {
            if (!object) return false;
            particle.dragCoefficient = scriptFloat(object, "dragCoefficient");
            particle.windCoefficient = scriptFloat(object, "windCoefficient");
            particle.gravityCoefficient = scriptFloat(object, "gravityCoefficient");
            particle.inheritedVelFactor = scriptFloat(object, "inheritedVelFactor");
            particle.constantAcceleration = scriptFloat(object, "constantAcceleration");
            particle.lifetimeMS = (uint32_t)std::max(0.0f, scriptFloat(object, "lifetimeMS"));
            particle.lifetimeVarianceMS = (uint32_t)std::max(0.0f, scriptFloat(object, "lifetimeVarianceMS"));
            particle.spinSpeed = scriptFloat(object, "spinSpeed");
            particle.spinRandomMin = scriptFloat(object, "spinRandomMin");
            particle.spinRandomMax = scriptFloat(object, "spinRandomMax");
            particle.useInvAlpha = scriptBool(object, "useInvAlpha");
            if (const auto* texture = scriptField(object, "textureName"))
                particle.textures.push_back(texture->toString());
            for (int i = 0; i < 8; ++i) {
                const std::string suffix = "[" + std::to_string(i) + "]";
                const auto* color = scriptField(object, "colors" + suffix);
                const auto* size = scriptField(object, "sizes" + suffix);
                const auto* time = scriptField(object, "times" + suffix);
                if (!color && !size && !time) break;
                V12::DecodedDataBlock::ParticleKey key;
                if (color) sscanf(color->toString().c_str(), "%f %f %f %f", &key.red, &key.green, &key.blue, &key.alpha);
                if (size) key.size = size->toFloat();
                if (time) key.time = time->toFloat();
                 // Mission scripts use metres; network datablocks use size/50.
                 key.size /= 50.0f;
                 particle.keys.push_back(key);
            }
            return !particle.textures.empty() || !particle.keys.empty();
        };
        auto loadEmitter = [&](const ScriptObject* object, EffectEmitter& emitter) {
            if (!object) return false;
            emitter.emitter.ejectionPeriodMS = (uint32_t)std::max(1.0f, scriptFloat(object, "ejectionPeriodMS", 1));
            emitter.emitter.periodVariance = (uint32_t)std::max(0.0f, scriptFloat(object, "periodVarianceMS"));
            emitter.emitter.ejectionVelocity = (uint32_t)std::lround(std::max(0.0f, scriptFloat(object, "ejectionVelocity")) * 100.0f);
            emitter.emitter.velocityVariance = (uint32_t)std::lround(std::max(0.0f, scriptFloat(object, "velocityVariance")) * 100.0f);
            emitter.emitter.ejectionOffset = (uint32_t)std::lround(std::max(0.0f, scriptFloat(object, "ejectionOffset")) * 100.0f);
            emitter.emitter.thetaMin = (uint32_t)std::max(0.0f, scriptFloat(object, "thetaMin"));
            emitter.emitter.thetaMax = (uint32_t)std::max(0.0f, scriptFloat(object, "thetaMax"));
            emitter.emitter.phiReferenceVel = (uint32_t)std::max(0.0f, scriptFloat(object, "phiReferenceVel"));
            emitter.emitter.phiVariance = (uint32_t)std::max(0.0f, scriptFloat(object, "phiVariance"));
            emitter.emitter.orientParticles = scriptBool(object, "orientParticles");
            emitter.emitter.orientOnVelocity = scriptBool(object, "orientOnVelocity");
            emitter.emitter.useEmitterSizes = scriptBool(object, "useEmitterSizes");
            emitter.emitter.useEmitterColors = scriptBool(object, "useEmitterColors");
            emitter.emitter.lifetimeMS = (uint32_t)std::max(0.0f, scriptFloat(object, "lifetimeMS"));
            emitter.emitter.lifetimeVarianceMS = (uint32_t)std::max(0.0f, scriptFloat(object, "lifetimeVarianceMS"));
            const auto* particle = scriptField(object, "particles");
            return particle && loadParticle(findScriptObject(particle->toString()), emitter.particle);
        };
        for (const auto& obj : objects) {
            if (obj.className == "ParticleEmissionDummy" || obj.className == "ParticleEmitter") {
                std::string name = getProp(obj.props, "emitter");
                if (name.empty()) name = getProp(obj.props, "datablock");
                EffectEmitter emitter;
                if (!loadEmitter(findScriptObject(name), emitter)) continue;
                emitter.pos = Math::torquePointToYUp(parsePos(getProp(obj.props, "position")));
                Point3F axis{0, 0, 1}; float angle = 0.0f;
                sscanf(getProp(obj.props, "rotation").c_str(), "%f %f %f %f", &axis.x, &axis.y, &axis.z, &angle);
                emitter.axis = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(angle))
                    .transformNormal(Math::torquePointToYUp({0, 0, 1}));
                const float length = std::sqrt(emitter.axis.x * emitter.axis.x + emitter.axis.y * emitter.axis.y + emitter.axis.z * emitter.axis.z);
                if (length > 0.0001f) { emitter.axis.x /= length; emitter.axis.y /= length; emitter.axis.z /= length; }
                for (const auto& textureName : emitter.particle.textures) {
                    std::vector<uint32_t> frames; std::vector<float> durations;
                    Engine::instance().renderer().loadTextureFrames(textureName.c_str(), frames, durations);
                    emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
                    emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
                }
                if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
                effectEmitters.push_back(std::move(emitter));
            } else if (obj.className == "Lightning") {
                EffectLightning lightning;
                lightning.pos = Math::torquePointToYUp(parsePos(getProp(obj.props, "position")));
                lightning.scale = parsePos(getProp(obj.props, "scale"));
                if (getProp(obj.props, "scale").empty()) lightning.scale = {1, 1, 1};
                Point3F axis{0, 0, 1}; float angle = 0.0f;
                sscanf(getProp(obj.props, "rotation").c_str(), "%f %f %f %f", &axis.x, &axis.y, &axis.z, &angle);
                lightning.rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(angle));
                auto value = [&](const char* field, float fallback) { const auto s = getProp(obj.props, field); return s.empty() ? fallback : (float)std::atof(s.c_str()); };
                lightning.strikeWidth = value("strikeWidth", 1.0f);
                lightning.strikesPerMinute = value("strikesPerMinute", 0.0f);
                 lightning.strikeRadius = value("strikeRadius", 0.0f);
                 lightning.boltStartRadius = value("boltStartRadius", 0.0f);
                 lightning.chanceToHitTarget = value("chanceToHitTarget", 0.0f);
                auto color = [&](const char* field, ColorF fallback) { float r, g, b, a; const auto s = getProp(obj.props, field); return sscanf(s.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3 ? ColorF{r, g, b, a} : fallback; };
                lightning.color = color("color", lightning.color);
                lightning.fadeColor = color("fadeColor", lightning.fadeColor);
                lightning.nextStrike = lightning.strikesPerMinute > 0.0f ? 60.0f / lightning.strikesPerMinute : 0.0f;
                effectLightnings.push_back(std::move(lightning));
            }
        }
        if (!effectEmitters.empty() || !effectLightnings.empty())
            Console::instance().printf(LogLevel::Info, "  mission effects: %zu particle emitters, %zu lightning objects",
                                       effectEmitters.size(), effectLightnings.size());

        // Register static objects with the interior zone that contains them.
        // This is the small SceneGraph equivalent of InteriorInstance::scopeObject.
        for (size_t managerIndex = 0; managerIndex < worldObjects.size(); managerIndex++) {
            auto& manager = worldObjects[managerIndex];
            if (!manager.shape || !manager.shape->loaded || !manager.shape->isInterior ||
                manager.shape->interiorBSP.empty()) continue;
            MatrixF managerModel;
            if (manager.rotAngleDeg != 0 && (manager.rot.x != 0 || manager.rot.y != 0 || manager.rot.z != 0)) {
                Point3F axis = manager.rot;
                const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                if (length > 0.0001f) {
                    axis.x /= length; axis.y /= length; axis.z /= length;
                    managerModel = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(manager.rotAngleDeg));
                }
            }
            managerModel.setTranslation({manager.pos.x, manager.pos.z, -manager.pos.y});
            if (manager.scale.x != 1.0f || manager.scale.y != 1.0f || manager.scale.z != 1.0f)
                managerModel = managerModel * Math::torqueScaleToYUp(manager.scale);
            managerModel = managerModel * manager.shape->upOrientation();
            const MatrixF inverseManager = managerModel.inverse();
            for (auto& object : worldObjects) {
                if (&object == &manager || object.zoneManager >= 0 || !object.shape) continue;
                const Point3F worldPosition{object.pos.x, object.pos.z, -object.pos.y};
                const int zone = manager.shape->interiorZoneForPoint(inverseManager.transform(worldPosition));
                if (zone >= 0) {
                    object.zoneManager = (int)managerIndex;
                    object.interiorZone = zone;
                }
            }
        }

        // Track pickups separately from their visual. Native Item shapes are
        // rendered as WorldObjects, while shape-less items use the proxy below.
        for (auto& obj : objects) {
            if (obj.className != "Item") continue;
            std::string db = getProp(obj.props, "datablock");
            std::string posStr = getProp(obj.props, "position");

            // Check if this item was already placed as a WorldObject with a shape
            bool hasShape = false;
            Point3F itemPos = parsePos(posStr);
            for (auto& wo : worldObjects) {
                if (wo.shape && wo.shape->loaded &&
                    std::abs(wo.pos.x - itemPos.x) < 0.01f &&
                    std::abs(wo.pos.y - itemPos.y) < 0.01f &&
                    std::abs(wo.pos.z - itemPos.z) < 0.01f) {
                    hasShape = true;
                    break;
                }
            }
             const ItemKind kind = classifyItemKind(db);
             if (kind == ItemKind::None)
                 continue;
             ItemPickup::Type type = kind == ItemKind::Health ? ItemPickup::Health
                 : kind == ItemKind::Energy ? ItemPickup::Energy : ItemPickup::Ammo;

            ItemPickup item;
            item.pos = itemPos;
            item.type = type;
             auto scriptIt = ScriptEngine::instance().objects.find(db);
             if (scriptIt != ScriptEngine::instance().objects.end() && scriptIt->second) {
                 auto* script = scriptIt->second;
                 auto number = [&](const char* field, float fallback) {
                     auto it = script->fields.find(field);
                     return it == script->fields.end() ? fallback : it->second.toFloat();
                 };
                 item.amount = number("amount", item.amount);
                 item.respawnDelay = number("respawnTime", number("respawn", item.respawnDelay));
             }
             item.renderProxy = !hasShape;
             for (size_t i = 0; i < worldObjects.size(); ++i) {
                 auto& wo = worldObjects[i];
                 if (wo.itemPickup && std::abs(wo.pos.x - itemPos.x) < 0.01f &&
                     std::abs(wo.pos.y - itemPos.y) < 0.01f &&
                     std::abs(wo.pos.z - itemPos.z) < 0.01f) {
                     item.worldObjectIndex = (int)i;
                     break;
                 }
             }
            items.push_back(item);
            Console::instance().printf(LogLevel::Debug, "  item (box): %s at (%.1f, %.1f, %.1f)",
                db.c_str(), item.pos.x, item.pos.y, item.pos.z);
        }

        // Build collision mesh from world objects with per-object transforms applied
        {
            std::vector<float> allVerts;
            std::vector<uint32_t> allIndices;
            uint32_t vertBase = 0;

            for (auto& wo : worldObjects) {
                if (!wo.collidable) continue;
                if (wo.forceField) {
                    MatrixF xform;
                    if (wo.rotAngleDeg != 0 && (wo.rot.x != 0 || wo.rot.y != 0 || wo.rot.z != 0)) {
                        Point3F axis = wo.rot;
                        const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                        if (length > 0.0001f) {
                            axis.x /= length; axis.y /= length; axis.z /= length;
                            xform = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(wo.rotAngleDeg));
                        }
                    }
                    xform = xform * Math::torqueScaleToYUp(wo.scale);
                    xform.setTranslation(Math::torquePointToYUp(wo.pos));
                    const Point3F corners[8] = {
                        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
                        {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                        {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
                        {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}
                    };
                    const uint32_t faces[] = {
                        0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                        0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
                        0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2
                    };
                    const uint32_t base = vertBase;
                    for (const auto& corner : corners) {
                        const Point3F v = xform.transform(corner);
                        allVerts.insert(allVerts.end(), {v.x, v.y, v.z});
                    }
                    for (const auto index : faces) allIndices.push_back(base + index);
                    vertBase += 8;
                    continue;
                }
                if (!wo.shape || !wo.shape->loaded || !wo.shape->isInterior) continue;

                // Build transform matrix for this object (same as render code)
                MatrixF xform;
                xform.identity();
                if (wo.rotAngleDeg != 0 && (wo.rot.x != 0 || wo.rot.y != 0 || wo.rot.z != 0)) {
                    Point3F axis = wo.rot;
                    float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                    if (len > 0.0001f) {
                        axis.x /= len; axis.y /= len; axis.z /= len;
                        xform = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(wo.rotAngleDeg));
                    }
                }
                if (wo.scale.x != 1.0f || wo.scale.y != 1.0f || wo.scale.z != 1.0f) {
                    xform = xform * Math::torqueScaleToYUp(wo.scale);
                }
                xform.setTranslation(Math::torquePointToYUp(wo.pos));
                // DIF collision vertices remain in their native Torque frame.
                MatrixF fullXform = xform * Math::czUpToYUp();

                auto addCollisionVerts = [&](const std::vector<float>& cVerts, const std::vector<uint32_t>& cIndices) {
                    uint32_t base = vertBase;
                    for (size_t i = 0; i + 2 < cVerts.size(); i += 3) {
                        Point3F v = {cVerts[i], cVerts[i+1], cVerts[i+2]};
                        Point3F tv = fullXform.transform(v);
                        allVerts.push_back(tv.x);
                        allVerts.push_back(tv.y);
                        allVerts.push_back(tv.z);
                    }
                    for (auto idx : cIndices) allIndices.push_back(base + idx);
                    vertBase += (uint32_t)cVerts.size() / 3;
                };

                if (!wo.shape->collisionVerts.empty() && !wo.shape->collisionIndices.empty()) {
                    addCollisionVerts(wo.shape->collisionVerts, wo.shape->collisionIndices);
                } else {
                    for (auto& mesh : wo.shape->meshes) {
                        std::vector<float> vData;
                        std::vector<uint32_t> iData;
                        for (auto& v : mesh.vertices) {
                            vData.push_back(v.pos.x);
                            vData.push_back(v.pos.y);
                            vData.push_back(v.pos.z);
                        }
                        for (auto idx : mesh.indices) iData.push_back(idx);
                        addCollisionVerts(vData, iData);
                    }
                }
            }

            if (!allIndices.empty()) {
                interiorCollision.addMesh(allVerts.data(), (int)allVerts.size(), allIndices.data(), (int)allIndices.size());
                interiorCollision.build();
                Console::instance().printf(LogLevel::Info, "Collision mesh built: %zu triangles", interiorCollision.triangles.size());
            }
        }

    } else {
        Console::instance().printf(LogLevel::Warn, "No mission file found for '%s', skipping terrain/sky/fog setup", mapName);
    }

    // Generate terrain only if we have a real mission (skip for missing .mis in demo playback)
    if (missionHasTerrainBlock && !terrainBlock.loaded) {
        terrainBlock.load(nullptr, 0);
    }

    // Load sky from mission materialList
    std::vector<std::string> skyFaces;
    std::string emapPath;
    std::vector<std::string> cloudPaths;
    if (!skyMaterialList.empty()) {
        // Try to load the DML file
        std::string dmlPath = "textures/" + skyMaterialList;
        Console::instance().printf(LogLevel::Debug, "  reading DML: %s", dmlPath.c_str());
        auto dmlData = fs.read(dmlPath.c_str());
        Console::instance().printf(LogLevel::Debug, "  DML read returned %zu bytes", dmlData.size());
        if (dmlData.empty()) {
            dmlPath = skyMaterialList;
            Console::instance().printf(LogLevel::Debug, "  trying DML: %s", dmlPath.c_str());
            dmlData = fs.read(dmlPath.c_str());
            Console::instance().printf(LogLevel::Debug, "  DML read returned %zu bytes", dmlData.size());
        }
        if (!dmlData.empty()) {
            std::string dmlContent((const char*)dmlData.data(), dmlData.size());
            // Parse all lines from DML: first 6 = cubemap faces, 7th = emap, 8-10 = cloud layers
            std::vector<std::string> faceNames;
            size_t pos = 0;
            int lineIdx = 0;
            while (pos < dmlContent.size()) {
                while (pos < dmlContent.size() && (dmlContent[pos] == ' ' || dmlContent[pos] == '\t' || dmlContent[pos] == '\r')) pos++;
                if (pos >= dmlContent.size()) break;
                size_t end = pos;
                while (end < dmlContent.size() && dmlContent[end] != '\n') end++;
                std::string line = dmlContent.substr(pos, end - pos);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
                // DML comments and blank lines are not entries.  Counting them
                // shifts the face/emap/cloud slots and makes valid sky lists
                // resolve the wrong assets.
                if (!line.empty() && line.front() != ';') {
                    if (lineIdx < 6)
                        faceNames.push_back(line);
                    else if (lineIdx == 6)
                        emapPath = line;
                    else if (lineIdx >= 7 && lineIdx <= 9)
                        cloudPaths.push_back(line);
                    lineIdx++;
                }
                pos = end + 1;
            }

            Console::instance().printf(LogLevel::Debug, "  DML face names (%zu):", faceNames.size());
            for (auto& fn : faceNames) Console::instance().printf(LogLevel::Debug, "    '%s'", fn.c_str());

            if (faceNames.size() >= 6) {
                std::vector<std::string> exts = {".png", ".jpg", ".bm8"};
                for (auto& fn : faceNames) {
                    bool found = false;
                    for (auto& ext : exts) {
                        std::string texPath = "textures/" + fn + ext;
                        Console::instance().printf(LogLevel::Debug, "  trying: %s", texPath.c_str());
                        auto test = fs.read(texPath.c_str());
                        if (!test.empty()) {
                            Console::instance().printf(LogLevel::Debug, "  FOUND: %s (%zu bytes)", texPath.c_str(), test.size());
                            skyFaces.push_back(texPath);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        for (auto& ext : exts) {
                            std::string texPath = fn + ext;
                            Console::instance().printf(LogLevel::Debug, "  trying (no prefix): %s", texPath.c_str());
                            auto test = fs.read(texPath.c_str());
                            if (!test.empty()) {
                                Console::instance().printf(LogLevel::Debug, "  FOUND: %s (%zu bytes)", texPath.c_str(), test.size());
                                skyFaces.push_back(texPath);
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        Console::instance().printf(LogLevel::Debug, "  NOT FOUND: %s", fn.c_str());
                        skyFaces.clear(); break;
                    }
                }
            } else {
                Console::instance().printf(LogLevel::Warn, "  DML has < 6 face names (%zu)", faceNames.size());
            }
        }
    }

    // Fallback if DML-based loading failed
    if (skyFaces.size() < 6) {
        Console::instance().printf(LogLevel::Warn, "Sky: cubemap unavailable for materialList '%s'; using solid-color fallback", skyMaterialList.c_str());
    }

    if (skyFaces.size() >= 6) {
        skyBox.load(skyFaces);
        Console::instance().printf(LogLevel::Info, "  sky loaded from: %s", skyMaterialList.c_str());

        // Load environment map (sphere map) from DML line 7
        if (!emapPath.empty()) {
            std::string emapFullPath;
            std::vector<std::string> exts = {".png", ".jpg", ".bm8"};
            // Try textures/<path>.<ext> first
            for (auto& ext : exts) {
                std::string p = "textures/" + emapPath + ext;
                Console::instance().printf(LogLevel::Debug, "  trying emap: %s", p.c_str());
                auto ed = fs.read(p.c_str());
                if (!ed.empty()) {
                    emapFullPath = p;
                    skyBox.emap.load(ed.data(), ed.size());
                    Console::instance().printf(LogLevel::Info, "  emap loaded: %s (%zu bytes)", p.c_str(), ed.size());
                    break;
                }
            }
            if (emapFullPath.empty()) {
                // Try without textures/ prefix
                for (auto& ext : exts) {
                    std::string p = emapPath + ext;
                    Console::instance().printf(LogLevel::Debug, "  trying emap (no prefix): %s", p.c_str());
                    auto ed = fs.read(p.c_str());
                    if (!ed.empty()) {
                        skyBox.emap.load(ed.data(), ed.size());
                        Console::instance().printf(LogLevel::Info, "  emap loaded: %s (%zu bytes)", p.c_str(), ed.size());
                        break;
                    }
                }
            }
            if (!skyBox.emap.loaded) {
                Console::instance().printf(LogLevel::Warn,
                    "  native environment map not found: %s", emapPath.c_str());
            }

            // Load cloud layers from DML lines 7-9
            // Use cloud properties from Sky object (read above)
            for (size_t ci = 0; ci < cloudPaths.size() && ci < 3; ci++) {
                Sky::CloudLayer layer;
                layer.scrollSpeed = cloudSpeeds[ci];
                layer.opacity = (ci == 0) ? 0.6f : (ci == 1) ? 0.4f : 0.3f;
                layer.height = cloudHeights[ci];

                bool found = false;
                for (auto& ext : exts) {
                    std::string texPath = "textures/" + cloudPaths[ci] + ext;
                    auto td = fs.read(texPath.c_str());
                    if (!td.empty()) {
                        layer.texture.load(td.data(), td.size());
                        if (layer.texture.loaded) {
                            Console::instance().printf(LogLevel::Info, "  cloud layer %zu loaded: %s", ci, texPath.c_str());
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    for (auto& ext : exts) {
                        std::string texPath = cloudPaths[ci] + ext;
                        auto td = fs.read(texPath.c_str());
                        if (!td.empty()) {
                            layer.texture.load(td.data(), td.size());
                            if (layer.texture.loaded) {
                                Console::instance().printf(LogLevel::Info, "  cloud layer %zu loaded: %s", ci, texPath.c_str());
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) {
                    Console::instance().printf(LogLevel::Debug, "  cloud layer %zu NOT FOUND: %s", ci, cloudPaths[ci].c_str());
                }
                skyBox.cloudLayers.push_back(std::move(layer));
            }
        }
    } else {
        Console::instance().printf(LogLevel::Info, "No sky textures found, generating default");
    }

    loaded = true;
    Console::instance().printf(LogLevel::Info, "Map loaded: %s", mapName);
    return true;
}

bool World::loadTerrain(const char* mapName) {
    // Headless terrain-only loader: parse the mission, find the TerrainBlock, and
    // load just the heightfield. Avoids shape/material/GL loading so a dedicated
    // server can register an authoritative ground-height callback.
    auto& fs = Engine::instance().fs();
    std::string missionName = mapName ? mapName : "";
    for (char& c : missionName) if (c == '\\') c = '/';
    if (missionName.starts_with("base/")) missionName.erase(0, 5);
    if (missionName.starts_with("missions/")) missionName.erase(0, 9);
    if (missionName.ends_with(".mis")) missionName.erase(missionName.size() - 4);
    std::string misPath = std::string("missions/") + missionName + ".mis";
    std::string misData = fs.readText(misPath.c_str());
    if (misData.empty()) {
        misPath = std::string("Missions/") + mapName + ".mis";
        misData = fs.readText(misPath.c_str());
    }
    if (misData.empty()) return false;

    auto objects = parseMisFile(misData);
    MisObject* terrainObj = findObject(objects, "TerrainBlock");
    if (!terrainObj) return false;

    std::string terrainFile = getProp(terrainObj->props, "terrainfile");
    std::string sqStr = getProp(terrainObj->props, "squaresize");
    if (!sqStr.empty()) terrainBlock.squareSize = (float)std::atof(sqStr.c_str());
    terrainBlock.setEmptySquareRuns(
        parseEmptySquareRuns(getProp(terrainObj->props, "emptysquares")));
    std::string hsStr = getProp(terrainObj->props, "heightscale");
    if (!hsStr.empty()) terrainBlock.heightScale = (float)std::atof(hsStr.c_str());
    std::string posStr = getProp(terrainObj->props, "position");
    if (!posStr.empty()) {
        float px, py, pz;
        if (sscanf(posStr.c_str(), "%f %f %f", &px, &py, &pz) == 3)
            terrainBlock.worldOffset = {px, pz, -py};
    }

    const std::vector<std::string> terPaths = terrainAssetCandidates(terrainFile);
    for (auto& tp : terPaths) {
        auto terData = fs.read(tp.c_str());
        if (!terData.empty()) { terrainBlock.load(terData.data(), terData.size()); break; }
    }
    if (!terrainBlock.loaded)
        Console::instance().printf(LogLevel::Warn,
            "Server: required terrain asset '%s' could not be resolved", terrainFile.c_str());
    if (terrainBlock.loaded)
        Console::instance().printf(LogLevel::Info, "Server terrain loaded from '%s'", mapName);
    else
        return false;

    // Server needs MissionArea for boundary checks
    MisObject* maObj = findObject(objects, "MissionArea");
    if (maObj) {
        std::string areaStr = getProp(maObj->props, "area");
        if (!areaStr.empty()) {
            float ax, ay, aw, ah;
            if (sscanf(areaStr.c_str(), "%f %f %f %f", &ax, &ay, &aw, &ah) == 4) {
                missionArea.valid = true;
                missionArea.x = ax;
                missionArea.z = -ay;
                missionArea.width = aw;
                missionArea.height = ah;
            }
            // Load GameGrid texture
            auto gridData = fs.read("textures/special/GameGrid.png");
            if (!gridData.empty()) {
                terrainBlock.gameGrid.load(gridData.data(), gridData.size());
            }
        }
    }

    return terrainBlock.loaded;
}

void World::update(float dt) {
    if (fog.transitioning) {
        fog.transitionElapsed = std::min(fog.transitionElapsed + std::max(0.0f, dt),
                                          fog.transitionDuration);
        const float t = fog.transitionDuration > 0.0f
            ? fog.transitionElapsed / fog.transitionDuration : 1.0f;
        fog.density = Math::lerp(fog.transitionStartDensity,
                                 fog.transitionTargetDensity, t);
        fog.color = {
            Math::lerp(fog.transitionStartColor.r, fog.transitionTargetColor.r, t),
            Math::lerp(fog.transitionStartColor.g, fog.transitionTargetColor.g, t),
            Math::lerp(fog.transitionStartColor.b, fog.transitionTargetColor.b, t),
            Math::lerp(fog.transitionStartColor.a, fog.transitionTargetColor.a, t)};
        fog.distance = fog.density > 0.0f ? 1.0f / fog.density : 0.0f;
        if (t >= 1.0f) fog.transitioning = false;
    }
    auto managerModel = [](const WorldObject& manager) {
        MatrixF model;
        if (manager.rotAngleDeg != 0 && (manager.rot.x != 0 || manager.rot.y != 0 || manager.rot.z != 0)) {
            Point3F axis = manager.rot;
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (length > 0.0001f) {
                axis.x /= length; axis.y /= length; axis.z /= length;
                model = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(manager.rotAngleDeg));
            }
        }
        model.setTranslation({manager.pos.x, manager.pos.z, -manager.pos.y});
        if (manager.scale.x != 1.0f || manager.scale.y != 1.0f || manager.scale.z != 1.0f)
            model = model * Math::torqueScaleToYUp(manager.scale);
        return model * manager.shape->upOrientation();
    };
    for (size_t objectIndex = 0; objectIndex < worldObjects.size(); objectIndex++) {
        auto& object = worldObjects[objectIndex];
        if (!object.shape) continue;
        object.interiorZone = -1;
        if (object.zoneManager >= 0 && object.zoneManager < (int)worldObjects.size() &&
            worldObjects[object.zoneManager].shape && worldObjects[object.zoneManager].shape->isInterior) {
            auto& manager = worldObjects[object.zoneManager];
            const Point3F local = managerModel(manager).inverse().transform(
                {object.pos.x, object.pos.z, -object.pos.y});
            object.interiorZone = manager.shape->interiorZoneForPoint(local);
            if (object.interiorZone >= 0) continue;
        }
        object.zoneManager = -1;
        for (size_t managerIndex = 0; managerIndex < worldObjects.size(); managerIndex++) {
            if (managerIndex == objectIndex) continue;
            auto& manager = worldObjects[managerIndex];
            if (!manager.shape || !manager.shape->loaded || !manager.shape->isInterior ||
                manager.shape->interiorBSP.empty()) continue;
            const Point3F local = managerModel(manager).inverse().transform(
                {object.pos.x, object.pos.z, -object.pos.y});
            const int zone = manager.shape->interiorZoneForPoint(local);
            if (zone >= 0) {
                object.zoneManager = (int)managerIndex;
                object.interiorZone = zone;
                break;
            }
        }
    }

    // Trigger callbacks are local scene behavior.  Network ghost creation and
    // deletion remain owned by the protocol; only already-visible player ghosts
    // participate here, so this cannot manufacture network state.
    if (!Engine::instance().game().isMapperMode()) {
        auto transformTrigger = [](const WorldObject& object, const Point3F& local) {
            Point3F axis = object.rot;
            const float length = std::sqrt(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
            MatrixF rotation;
            if (length > 0.0001f) {
                axis.x /= length; axis.y /= length; axis.z /= length;
                rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(object.rotAngleDeg));
            }
            const Point3F scaled{local.x * object.scale.x, local.y * object.scale.z,
                                 local.z * object.scale.y};
            const Point3F converted = Math::torquePointToYUp(scaled);
            const Point3F rotated = rotation.transform(converted);
            return Point3F{rotated.x + Math::torquePointToYUp(object.pos).x,
                           rotated.y + Math::torquePointToYUp(object.pos).y,
                           rotated.z + Math::torquePointToYUp(object.pos).z};
        };
        auto dispatch = [](const WorldObject& trigger, const char* event,
                           const std::string& actor) {
            auto* ts = ScriptEngine::instance().ts();
            if (!ts) return;
            const std::string names[] = {trigger.objectName + "::" + event,
                                         trigger.className + "::" + event};
            for (const auto& name : names) {
                if (name.size() <= std::strlen(event) + 2 || !ts->hasFunction(name)) continue;
                ts->callFunction(name, {VMValue(trigger.objectName), VMValue(actor)});
                break;
            }
        };
        std::vector<std::pair<std::string, Point3F>> actors;
        const Point3F player = Engine::instance().game().player().position();
        actors.push_back({"Player", player});
        for (int index : Engine::instance().game().getLiveGhostIndices()) {
            const GhostEntry* ghost = Engine::instance().game().getLiveGhost(index);
            if (!ghost || ghost->className != "Player") continue;
            actors.push_back({std::to_string(index), {ghost->renderPos.x, ghost->renderPos.y, ghost->renderPos.z}});
        }
         for (auto& trigger : worldObjects) {
             if (!trigger.triggerVolume) continue;
            std::vector<Point3F> transformed;
            transformed.reserve(trigger.trigger.vertices.size());
            for (const auto& vertex : trigger.trigger.vertices)
                transformed.push_back(transformTrigger(trigger, vertex));
            const TriggerPolyhedron worldHull = triggerFromVertices(transformed);
            std::unordered_set<std::string> current;
             const bool active = trigger.className != "PhysicalZone" || trigger.physicalActive;
             for (const auto& actor : actors) {
                 const bool inside = active && worldHull.contains(actor.second);
                 if (inside) current.insert(actor.first);
             }
             const auto transitions = triggerTransitions(trigger.triggerOccupants, current);
             for (const auto& actor : transitions.entered) dispatch(trigger, "onEnter", actor);
             for (const auto& actor : transitions.left) dispatch(trigger, "onLeave", actor);
         }
    }

    // Update item pickups
    for (auto& item : items) {
        if (!item.active) {
            item.respawnTimer -= dt;
            if (item.respawnTimer <= 0) {
                item.active = true;
                if (item.worldObjectIndex >= 0 && item.worldObjectIndex < (int)worldObjects.size())
                    worldObjects[item.worldObjectIndex].itemActive = true;
            }
            continue;
        }

        // Check player proximity
        auto& game = Engine::instance().game();
        if (game.isMapperMode()) continue;  // No player in mapper mode
        const Point3F& ppos = game.player().position();
        float dx = item.pos.x - ppos.x;
        float dy = item.pos.y - ppos.y;
        float dz = item.pos.z - ppos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist < 2.0f) {
            item.active = false;
            item.respawnTimer = item.respawnDelay;
            if (item.worldObjectIndex >= 0 && item.worldObjectIndex < (int)worldObjects.size())
                worldObjects[item.worldObjectIndex].itemActive = false;

            switch (item.type) {
                case ItemPickup::Health:
                    game.player().applyDamage(-item.amount); // negative = heal
                    break;
                case ItemPickup::Energy:
                    game.player().setEnergy(applyItemAmount(ItemKind::Energy, game.player().energy(),
                                                            item.amount, 100.0f));
                    break;
                case ItemPickup::Ammo: {
                    int32_t cw = game.player().currentWeapon();
                    if (cw >= 0 && cw < (int32_t)game.player().weaponCount())
                        game.player().weapon(cw).ammo = (int)applyItemAmount(
                            ItemKind::Ammo, (float)game.player().weapon(cw).ammo, item.amount, 0.0f);
                    if (cw >= 0 && cw < (int32_t)game.player().weaponCount()) {
                        if (auto* hud = Engine::instance().guiRenderer().findControl("weaponsHud")) {
                            for (auto& slot : hud->hudSlots)
                                if (slot.active) slot.amount = game.player().weapon(cw).ammo;
                        }
                    }
                    break;
                }
            }

            Console::instance().printf(LogLevel::Debug, "Picked up item at (%.1f, %.1f, %.1f)",
                item.pos.x, item.pos.y, item.pos.z);
        }
    }

    // Update explosions
    for (auto& e : explosions) {
        e.lifetime -= dt;
        e.radius += dt * 4.0f;
        // Check explosion against bots
        for (size_t botIndex = 0; botIndex < bots.size(); botIndex++) {
            auto& b = bots[botIndex];
            if (!b.alive) continue;
            if (std::find(e.damagedBots.begin(), e.damagedBots.end(), botIndex) != e.damagedBots.end())
                continue;
            float dx = b.pos.x - e.pos.x;
            float dy = b.pos.y - e.pos.y;
            float dz = b.pos.z - e.pos.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < e.radius) {
                float dmg = 30.0f * (1.0f - dist / e.radius);
                b.health -= dmg;
                e.damagedBots.push_back(botIndex);
                if (b.health <= 0) { b.health = 0; b.alive = false; b.respawnTimer = 5.0f; }
                else { b.lastHitTime = Engine::instance().game().gameTime(); }
            }
        }
    }
    explosions.erase(
        std::remove_if(explosions.begin(), explosions.end(),
            [](const Explosion& e) { return e.lifetime <= 0; }),
        explosions.end()
    );

    // Update projectiles
    for (auto& p : projList) {
        if (!p.active) continue;
        updateProjectile(p, dt);
        if (!p.active) continue;

        // Spawn trail particles
        // Native projectile emitters are time based, not rand() based.
        {
            ColorF trailColor;
            switch (p.type) {
                case ProjectileType::Disc:    trailColor = {1.0f, 0.6f, 0.1f, 0.6f}; break;
                case ProjectileType::Bolt:    trailColor = {0.2f, 0.8f, 1.0f, 0.6f}; break;
                case ProjectileType::Grenade:
                case ProjectileType::Mortar:  trailColor = {0.3f, 1.0f, 0.3f, 0.6f}; break;
                default:                      trailColor = {1.0f, 1.0f, 0.5f, 0.6f}; break;
            }
            spawnTrail(p.pos, trailColor, 0.15f);
        }

        // Check for impact
            float groundH = 0;
            Point3F impactNormal{0, 1, 0};
            if (checkProjectileCollision(p, groundH, impactNormal)) {
                if (impactNormal.y > 0.5f)
                    spawnTrail(p.pos, {0.45f, 0.42f, 0.35f, 0.55f}, 0.25f);
                // Spawn explosion effect
                ColorF expColor;
                switch (p.type) {
                    case ProjectileType::Disc:    expColor = {1.0f, 0.6f, 0.1f, 1.0f}; break;
                    case ProjectileType::Bolt:    expColor = {0.2f, 0.8f, 1.0f, 1.0f}; break;
                    case ProjectileType::Grenade:
                    case ProjectileType::Mortar:  expColor = {0.3f, 1.0f, 0.3f, 1.0f}; break;
                    default:                      expColor = {1.0f, 1.0f, 0.5f, 1.0f}; break;
                }
                Explosion exp;
                exp.pos = p.pos;
                exp.lifetime = 0.5f;
                exp.maxLifetime = 0.5f;
                exp.radius = 1.0f;
                exp.color = expColor;
                explosions.push_back(exp);
                // Spawn particles
                spawnExplosion(p.pos, expColor, 1.5f, 25);

                // Play explosion sound
                if (p.weaponType >= 0 && p.weaponType < gWeaponCount) {
                    auto& audio = Engine::instance().audio();
                    const WeaponData& wd = gWeaponTable[p.weaponType];
                    if (wd.explosionSoundPath) {
                        auto* snd = audio.loadSound(wd.explosionSoundPath);
                        if (snd) {
                            auto* src = audio.createSource();
                            if (src) {
                                src->setPosition(p.pos);
                                src->positional = true;
                                src->setVolume(0.7f);
                                src->play(snd);
                            }
                        }
                    }
                }

                p.active = false;
                p.hasImpacted = true;
            }

            // Apply splash damage near impact
            if (p.hasImpacted && p.splashRadius > 0) {
            auto& game = Engine::instance().game();
            if (!game.isMapperMode()) {  // No player in mapper mode
            const Point3F& ppos = game.player().position();
            float dx = p.pos.x - ppos.x;
            float dy = p.pos.y - ppos.y;
            float dz = p.pos.z - ppos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < p.splashRadius) {
                float factor = 1.0f - dist / p.splashRadius;
                game.player().applyDamage(p.damage * factor * 0.5f);
            }
            } // end mapper mode guard
            // Splash damage bots
            for (auto& b : game.world().bots) {
                if (!b.alive) continue;
                float bdx = p.pos.x - b.pos.x;
                float bdy = p.pos.y - b.pos.y;
                float bdz = p.pos.z - b.pos.z;
                float bdist = sqrtf(bdx*bdx + bdy*bdy + bdz*bdz);
                if (bdist < p.splashRadius) {
                    float factor = 1.0f - bdist / p.splashRadius;
                    b.health -= p.damage * factor;
                    if (b.health <= 0) { b.health = 0; b.alive = false; b.respawnTimer = 5.0f; } else { b.lastHitTime = Engine::instance().game().gameTime(); }
                }
            }
        }
    }

    // Remove inactive projectiles
    projList.erase(
        std::remove_if(projList.begin(), projList.end(),
            [](const Projectile& p) { return !p.active || p.hasImpacted; }),
        projList.end()
    );

    // Update bots
    if (!Engine::instance().game().isMapperMode()) {
    auto& player = Engine::instance().game().player();
    Point3F ppos = player.position();
    for (auto& b : bots) {
        if (b.alive) {
            float dx = ppos.x - b.pos.x;
            float dz = ppos.z - b.pos.z;
            float distToPlayer = sqrtf(dx*dx + dz*dz); (void)distToPlayer;
            float timeSinceHit = dt > 0 ? (Engine::instance().game().gameTime() - b.lastHitTime) : 999.0f;

            if (timeSinceHit < 2.0f && b.health < 70) {
                // Flee from player when low health and recently hit
                float fleeAngle = atan2f(-dx, -dz);
                b.moveYaw = fleeAngle;
                b.pos.x += sinf(fleeAngle) * dt * 6.0f;
                b.pos.z += cosf(fleeAngle) * dt * 6.0f;
            } else if (timeSinceHit < 4.0f) {
                // Recently hit: face player and strafe
                float faceAngle = atan2f(dx, dz);
                b.moveYaw = faceAngle;
                float strafeAngle = faceAngle + 1.57f;
                b.pos.x += sinf(strafeAngle) * dt * 4.0f;
                b.pos.z += cosf(strafeAngle) * dt * 4.0f;
            } else {
                // Patrol: move in a circle around start position
                b.patrolOffset += dt * 2.0f;
                b.pos.x = b.startPos.x + sinf(b.patrolOffset) * 8.0f;
                b.pos.z = b.startPos.z + cosf(b.patrolOffset) * 8.0f;
                b.moveYaw = b.patrolOffset + 3.14159f;
            }
            // Stay near ground
            float th = Engine::instance().game().world().getHeight(b.pos.x, b.pos.z);
            if (b.pos.y < th + 0.5f) b.pos.y = th + 0.5f;
            b.animTime += dt;
        } else {
            b.respawnTimer -= dt;
            if (b.respawnTimer <= 0) {
                b.health = 100.0f;
                b.alive = true;
                b.pos = b.startPos;
            }
        }
    }
    } // end mapper mode guard for bots

    // Advance animation time for world objects
    for (auto& obj : worldObjects) {
        if (!obj.animName.empty() && obj.shape && obj.shape->loaded)
            obj.animTime += dt;
    }

    // Update particles
    updateParticles(dt);

    // Update precipitation
    if (precipitation.active) {
        Point3F camPos = Engine::instance().renderer().cameraPos;
        updatePrecipitation(dt, camPos);
    }
}

void World::updateRendererLights(Renderer& renderer) const {
    std::vector<DynamicPointLight> lights;
    lights.reserve(effectLights.size());
    for (const auto& source : effectLights) {
        const float fade = dynamicLightFade(source.age, source.delay, source.lifetime);
        if (fade <= 0.0f) continue;
        lights.push_back({source.pos.x, source.pos.y, source.pos.z,
                          source.color.r * fade, source.color.g * fade,
                          source.color.b * fade, source.radius, source.falloff});
    }
    renderer.setDynamicLights(lights);
}

void World::render(const Point3F& cameraPos) {
    static float forceFieldTime = 0.0f;
    forceFieldTime += 1.0f / 60.0f;
    if (!loaded) return;

    auto& r = Engine::instance().renderer();

    currentSceneState.cameraPosition = cameraPos;
    currentSceneState.view = r.view;
    currentSceneState.projection = r.projection;
    currentSceneState.interiorVisibleZones.clear();
    currentSceneState.interiorVisibilityComputed.clear();

    // Render sky first (behind everything, depth writes off)
    glDepthMask(GL_FALSE);
    skyBox.fogColor = fog.color;
    skyBox.visibleDistance = visibleDistance;
    skyBox.fogVolumes.clear();
    for (const auto& volume : fogVolumes)
        skyBox.fogVolumes.push_back({volume.visibleDistance, volume.minHeight,
                                     volume.maxHeight, 1.0f});
    skyBox.render(r.view, r.projection, cameraPos.y);
    glDepthMask(GL_TRUE);

    // Sky fog volumes are height bands in the stock mission data.  The native
    // shader already owns the distance ramp, so select the tightest authored
    // ramp for the band containing the camera rather than ignoring these fields.
    float effectiveFogStart = fog.distance;
    float effectiveFogEnd = visibleDistance;
    for (const auto& volume : fogVolumes) {
        if (cameraPos.y < volume.minHeight || cameraPos.y > volume.maxHeight)
            continue;
        effectiveFogEnd = std::min(effectiveFogEnd, volume.visibleDistance);
        effectiveFogStart = std::min(effectiveFogStart,
                                     volume.visibleDistance * 0.5f);
    }

    // Render terrain
    if (terrainBlock.loaded) {
        ShaderManager::getTerrainShader()->bind();
        Point3F terrainLight = sunLightDirUsed ? sunLightDir : Point3F{0.5f, 0.7f, 0.5f};
        bool mapperNoFog = Engine::instance().game().isMapperMode();
        terrainBlock.render(cameraPos, fog.enabled && !mapperNoFog, fog.color, fog.density, &terrainLight,
                            sunColorUsed ? &sunColor : nullptr, &sunAmbient, effectiveFogStart, effectiveFogEnd);
    }

    // Render world objects with default shader
    auto* defShader = ShaderManager::getDefaultShader();
    defShader->bind();
    defShader->setUniform("uCamPos", cameraPos);
    for (int i = 0; i < 3; ++i) {
        ColorF packed{};
        if (i < (int)fogVolumes.size() && fogVolumes[i].visibleDistance > 0.0f) {
            const auto& volume = fogVolumes[i];
            packed = {1.0f / volume.visibleDistance, volume.minHeight,
                      volume.maxHeight, 0.0f};
        }
        defShader->setUniform((std::string("uFogVolume") + std::to_string(i)).c_str(), packed);
    }

    // Apply fog
    const bool mapperNoFog = Engine::instance().game().isMapperMode();
    defShader->setUniform("uFogEnabled", (int32_t)(fog.enabled && !mapperNoFog ? 1 : 0));
    if (fog.enabled) {
        defShader->setUniform("uFogColor", Point3F{fog.color.r, fog.color.g, fog.color.b});
        defShader->setUniform("uFogDensity", fog.density);
        defShader->setUniform("uFogStart", effectiveFogStart);
        defShader->setUniform("uFogEnd", effectiveFogEnd);
    }

    // Apply sun lighting direction from mission data (or default for demo/procedural)
    if (sunLightDirUsed) {
        defShader->setUniform("uLightDir", sunLightDir);
        defShader->setUniform("uSunColor", Point3F{sunColor.r, sunColor.g, sunColor.b});
        defShader->setUniform("uAmbient", Point3F{sunAmbient.r, sunAmbient.g, sunAmbient.b});
    } else {
        // Default sun: 45 degrees elevation, from upper-right
        defShader->setUniform("uLightDir", Point3F{0.5f, 0.7f, 0.5f});
        defShader->setUniform("uSunColor", Point3F{1.0f, 1.0f, 1.0f});
        defShader->setUniform("uAmbient", Point3F{sunAmbient.r, sunAmbient.g, sunAmbient.b});
    }

    // Bind environment map from sky (for reflections on shapes)
    if (skyBox.emap.loaded) {
        skyBox.emap.bind(2);
        defShader->setUniform("uEnvMap", (int32_t)2);
    }

    std::vector<WorldObject*> renderQueue;
    renderQueue.reserve(worldObjects.size());
    for (auto& obj : worldObjects) {
        if (Engine::instance().game().isMapperMode() || obj.boundsRadius <= 0.0f) {
            renderQueue.push_back(&obj);
            continue;
        }
        const Point3F position{obj.pos.x, obj.pos.z, -obj.pos.y};
        const float dx = position.x - cameraPos.x;
        const float dy = position.y - cameraPos.y;
        const float dz = position.z - cameraPos.z;
        const float scale = std::max({std::fabs(obj.scale.x), std::fabs(obj.scale.y),
                                      std::fabs(obj.scale.z), 1.0f});
        if (dx * dx + dy * dy + dz * dz <=
            std::pow(Engine::instance().renderer().config().farPlane + obj.boundsRadius * scale, 2.0f))
            renderQueue.push_back(&obj);
    }
    std::stable_sort(renderQueue.begin(), renderQueue.end(),
        [&](const WorldObject* left, const WorldObject* right) {
            if (left->translucent != right->translucent)
                return !left->translucent;
            const Point3F leftPos{left->pos.x, left->pos.z, -left->pos.y};
            const Point3F rightPos{right->pos.x, right->pos.z, -right->pos.y};
            const float ldx = leftPos.x - cameraPos.x;
            const float ldy = leftPos.y - cameraPos.y;
            const float ldz = leftPos.z - cameraPos.z;
            const float rdx = rightPos.x - cameraPos.x;
            const float rdy = rightPos.y - cameraPos.y;
            const float rdz = rightPos.z - cameraPos.z;
            return ldx * ldx + ldy * ldy + ldz * ldz >
                   rdx * rdx + rdy * rdy + rdz * rdz;
        });
    bool waterRendered = false;
    for (auto* object : renderQueue) {
        auto& obj = *object;
        // Opaque geometry must populate depth before the water surface. Keep
        // translucent world geometry on the far side of this boundary.
        if (!waterRendered && obj.translucent) {
            r.flushSpriteBatch();
            renderWater();
            waterRendered = true;
        }
        if (obj.itemPickup && !obj.itemActive) continue;
        if (!Engine::instance().game().isMapperMode() &&
            !isPositionVisible(obj.pos, cameraPos))
            continue;
        if (obj.shape && obj.shape->loaded) {
             if (!obj.visible) continue;
             const bool mapperMarker = Engine::instance().game().isMapperMode() &&
                 (obj.className == "Marker" || obj.className == "MissionMarker" ||
                  obj.className == "SpawnSphere" || obj.className == "Trigger" ||
                   obj.className == "PhysicalZone" || obj.className == "WayPoint" ||
                   obj.className == "AIObjective");
            if (mapperMarker) {
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
            }
            MatrixF model;
            if (obj.rotAngleDeg != 0 && (obj.rot.x != 0 || obj.rot.y != 0 || obj.rot.z != 0)) {
                // Convert the complete Torque-frame rotation. Applying the
                // basis change to the matrix preserves arbitrary axis-angle
                // rotations without modifying the native DTS model frame.
                Point3F axis = obj.rot;
                float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                if (len > 0.0001f) {
                    axis.x /= len; axis.y /= len; axis.z /= len;
                    model = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
                }
            }
            // Convert position from T2 Z-up to Y-up: (x,y,z) -> (x, z, -y)
            Point3F pos = {obj.pos.x, obj.pos.z, -obj.pos.y};
            model.setTranslation(pos);
            // Apply non-unit scale if present
            if (obj.scale.x != 1.0f || obj.scale.y != 1.0f || obj.scale.z != 1.0f) {
                model = model * Math::torqueScaleToYUp(obj.scale);
            }
            if (obj.shape->nativeDTS) {
                // Native DTS vertices use (x,z,y), while mission coordinates
                // use the proper Z-up-to-Y-up basis (x,z,-y).
                MatrixF shapeFrame;
                shapeFrame.setRotationY(Math::PI);
                model = model * shapeFrame;
            }
            if (!obj.label.empty()) {
                Point3F mn{1e30f, 1e30f, 1e30f};
                Point3F mx{-1e30f, -1e30f, -1e30f};
                std::vector<int32_t> labelMeshes;
                if (!obj.shape->details.empty() &&
                    !obj.shape->details[0].meshIndices.empty()) {
                    labelMeshes = obj.shape->details[0].meshIndices;
                } else {
                    labelMeshes.resize(obj.shape->meshes.size());
                    std::iota(labelMeshes.begin(), labelMeshes.end(), 0);
                }
                for (int32_t meshIndex : labelMeshes) {
                    if (meshIndex < 0 || meshIndex >= (int32_t)obj.shape->meshes.size())
                        continue;
                    const auto& mesh = obj.shape->meshes[meshIndex];
                    MatrixF node = (mesh.nodeIndex >= 0 &&
                                    mesh.nodeIndex < (int)obj.shape->defaultTransforms.size())
                        ? obj.shape->defaultTransforms[mesh.nodeIndex]
                        : MatrixF();
                    for (const auto& vertex : mesh.vertices) {
                        Point3F point = node.transform(vertex.pos);
                        mn.x = std::min(mn.x, point.x);
                        mn.y = std::min(mn.y, point.y);
                        mn.z = std::min(mn.z, point.z);
                        mx.x = std::max(mx.x, point.x);
                        mx.y = std::max(mx.y, point.y);
                        mx.z = std::max(mx.z, point.z);
                    }
                }
                const float flagHeight = mx.y - mn.y;
                obj.labelAnchor = model.transform({
                    (mn.x + mx.x) * 0.5f, mn.y + flagHeight,
                    (mn.z + mx.z) * 0.5f});
                obj.labelAnchorValid = mn.x < mx.x || mn.y < mx.y || mn.z < mx.z;
            }
            r.setModel(model * obj.shape->upOrientation());
            obj.shape->activeInteriorZones.clear();
            if (obj.shape->isInterior && !Engine::instance().game().isMapperMode() &&
                !obj.shape->interiorBSP.empty()) {
                const MatrixF renderModel = model * obj.shape->upOrientation();
                const Point3F localCamera = renderModel.inverse().transform(cameraPos);
                const int zone = obj.shape->interiorZoneForPoint(localCamera);
                obj.shape->interiorVisibleZones(zone, localCamera,
                                                r.projection * r.view * renderModel,
                                                obj.shape->activeInteriorZones);
            }
            if (!obj.animName.empty())
                obj.shape->renderAnimation(obj.animName.c_str(), obj.animTime);
            else
                obj.shape->render(0);
            obj.shape->activeInteriorZones.clear();
            if (mapperMarker) {
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
            }

            // TurretImageData is a separate DTS shape mounted at the turret's
            // mount0 node. Align its Mountpoint back to that node, matching
            // Torque's mounted-image transform instead of drawing only the
            // turret base.
            if (obj.mountedShape && obj.mountedShape->loaded) {
                int mountNode = obj.shape->findNode("mount0");
                int pointNode = obj.mountedShape->findNode("Mountpoint");
                if (pointNode < 0) pointNode = obj.mountedShape->findNode("mountPoint");
                if (pointNode < 0) pointNode = obj.mountedShape->findNode("mount0");
                if (mountNode >= 0 && mountNode < (int)obj.shape->defaultTransforms.size() &&
                    pointNode >= 0 && pointNode < (int)obj.mountedShape->defaultTransforms.size()) {
                    MatrixF mountedModel = model * obj.shape->defaultTransforms[mountNode] *
                        obj.mountedShape->defaultTransforms[pointNode].inverse();
                    // Mounted images use their native mountpoint frame; the
                    // ordinary DTS shape correction must not be applied again.
                    r.setModel(mountedModel);
                    if (Engine::instance().game().isMapperMode()) {
                        const DTSShape::Animation* settled =
                            findAnimation(*obj.mountedShape, "deploy");
                        if (!settled) settled = findAnimation(*obj.mountedShape, "visibility");
                        if (settled)
                            obj.mountedShape->renderAnimation(
                                settled->name.c_str(), settled->duration);
                        else
                            obj.mountedShape->render(0);
                    } else {
                        obj.mountedShape->render(0);
                    }
                } else {
                    Console::instance().printf(LogLevel::Error,
                        "Mounted shape '%s' has no compatible native mount frames",
                        obj.mountedShape->name.c_str());
                }
            }
        }
         if (obj.forceField) {
            MatrixF fieldModel;
            if (obj.rotAngleDeg != 0 && (obj.rot.x != 0 || obj.rot.y != 0 || obj.rot.z != 0)) {
                Point3F axis = obj.rot;
                const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                if (length > 0.0001f) {
                    axis.x /= length; axis.y /= length; axis.z /= length;
                    fieldModel = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
                }
            }
            fieldModel = fieldModel * Math::torqueScaleToYUp(obj.scale);
            fieldModel.setTranslation(Math::torquePointToYUp(obj.pos));
             const Point3F corners[8] = {
                 {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                 {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
            };
            const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
                                      {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            Point3F transformed[8];
            for (int i = 0; i < 8; ++i) transformed[i] = fieldModel.transform(corners[i]);
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            for (const auto& edge : edges)
                r.drawLine(transformed[edge[0]], transformed[edge[1]], {0.25f, 0.85f, 1.0f, 0.65f});
            if (!obj.forceFieldOpen && !obj.forceFieldFrames.empty()) {
                const size_t frame = obj.forceFieldFramesPerSec > 0.0f
                    ? (size_t)(forceFieldTime * obj.forceFieldFramesPerSec) % obj.forceFieldFrames.size() : 0;
                const float u = obj.forceFieldUMapping;
                const float v = obj.forceFieldVMapping;
                const float scroll = forceFieldTime * obj.forceFieldScrollSpeed;
                const ColorF tint = {obj.forceFieldColor.r, obj.forceFieldColor.g,
                                     obj.forceFieldColor.b,
                                     obj.forceFieldColor.a * obj.forceFieldBaseTranslucency};
                const uint32_t texture = obj.forceFieldFrames[frame];
                r.drawTexturedQuad(transformed[0], transformed[1], transformed[2], transformed[3], texture, tint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[4], transformed[7], transformed[6], transformed[5], texture, tint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[0], transformed[4], transformed[5], transformed[1], texture, tint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[3], transformed[2], transformed[6], transformed[7], texture, tint, 0, scroll, u, v + scroll);
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // MissionMarker is only in the runtime scene while editing in the
        // original engine.  Mapper mode is Torch's editor equivalent, so
        // show the authored marker and volume bounds there without changing
        // gameplay collision semantics.
        if (Engine::instance().game().isMapperMode() &&
            (obj.className == "Marker" || obj.className == "MissionMarker" ||
             obj.className == "SpawnSphere" ||
              obj.className == "Trigger" || obj.className == "PhysicalZone" ||
              obj.className == "AIObjective")) {
            const Point3F center = Math::torquePointToYUp(obj.pos);
            MatrixF markerModel;
            Point3F axis = obj.rot;
            const float axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (axisLength > 0.0001f) {
                axis.x /= axisLength; axis.y /= axisLength; axis.z /= axisLength;
                markerModel = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
            }
            markerModel = markerModel * Math::torqueScaleToYUp(obj.scale);
            markerModel.setTranslation(center);
            if (obj.className == "SpawnSphere") {
                constexpr int segments = 24;
                for (int i = 0; i < segments; ++i) {
                    const float a = Math::PI * 2.0f * (float)i / segments;
                    const float b = Math::PI * 2.0f * (float)(i + 1) / segments;
                    const Point3F first = markerModel.transform({std::cos(a) * obj.volumeRadius,
                                                                  0, std::sin(a) * obj.volumeRadius});
                    const Point3F second = markerModel.transform({std::cos(b) * obj.volumeRadius,
                                                                   0, std::sin(b) * obj.volumeRadius});
                    r.drawLine(first, second,
                               {0.3f, 1.0f, 0.3f, 0.8f});
                }
            } else if (obj.missionVolume) {
                if (obj.className == "Trigger" && obj.trigger.vertices.size() >= 4) {
                    Point3F axis = obj.rot;
                    const float length = std::sqrt(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
                    MatrixF rotation;
                    if (length > 0.0001f) {
                        axis.x /= length; axis.y /= length; axis.z /= length;
                        rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
                    }
                    const Point3F center = Math::torquePointToYUp(obj.pos);
                    std::vector<Point3F> vertices;
                    for (const auto& local : obj.trigger.vertices) {
                        const Point3F scaled{local.x * obj.scale.x, local.y * obj.scale.z,
                                             local.z * obj.scale.y};
                        const Point3F rotated = rotation.transform(Math::torquePointToYUp(scaled));
                        vertices.push_back({center.x + rotated.x, center.y + rotated.y, center.z + rotated.z});
                    }
                    for (size_t i = 0; i < vertices.size(); ++i)
                        for (size_t j = i + 1; j < vertices.size(); ++j) {
                            int shared = 0;
                            for (const auto& plane : obj.trigger.planes) {
                                const auto distance = [&](const Point3F& p) {
                                    return plane.x*p.x + plane.y*p.y + plane.z*p.z + plane.w;
                                };
                                if (std::fabs(distance(obj.trigger.vertices[i])) < 0.001f &&
                                    std::fabs(distance(obj.trigger.vertices[j])) < 0.001f) ++shared;
                            }
                            if (shared >= 2) r.drawLine(vertices[i], vertices[j], {1.0f, 0.7f, 0.2f, 0.8f});
                        }
                    continue;
                }
                 const Point3F half{0.5f, 0.5f, 0.5f};
                 const Point3F corners[8] = {
                     markerModel.transform({-half.x,-half.y,-half.z}), markerModel.transform({half.x,-half.y,-half.z}),
                     markerModel.transform({half.x,half.y,-half.z}), markerModel.transform({-half.x,half.y,-half.z}),
                     markerModel.transform({-half.x,-half.y,half.z}), markerModel.transform({half.x,-half.y,half.z}),
                     markerModel.transform({half.x,half.y,half.z}), markerModel.transform({-half.x,half.y,half.z})};
                constexpr int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
                                              {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
                const ColorF color = obj.className == "Trigger"
                    ? ColorF{1.0f, 0.7f, 0.2f, 0.8f}
                    : ColorF{0.2f, 0.7f, 1.0f, 0.8f};
                for (const auto& edge : edges)
                    r.drawLine(corners[edge[0]], corners[edge[1]], color);
            } else {
                r.drawLine(center, markerModel.transform({0, 1, 0}),
                           obj.className == "AIObjective"
                               ? ColorF{1.0f, 0.3f, 0.8f, 0.9f}
                               : ColorF{1.0f, 1.0f, 0.2f, 0.9f});
            }
        }
    }

        // Render mission area boundary (grid lines) in mapper mode
    if (missionArea.valid && Engine::instance().game().isMapperMode()) {
        // T2 GameGrid.png color: yellowish-green
        ColorF gridCol{1.0f, 1.0f, 0.2f, 1.0f};  // bright yellow (GameGrid color)

        float gx0 = missionArea.x;
        float gz_north = missionArea.z;
        float gx1 = gx0 + missionArea.width;
        float gz_south = gz_north - missionArea.height;

        // Only visible when camera is within 100m of the boundary
        float camX = cameraPos.x, camZ = cameraPos.z;
        float distToLeft = std::abs(camX - gx0);
        float distToRight = std::abs(camX - gx1);
        float distToTop = std::abs(camZ - gz_north);
        float distToBottom = std::abs(camZ - gz_south);
        float minDist = std::min({distToLeft, distToRight, distToTop, distToBottom});
        if (minDist > 100.0f) goto skip_grid;

        auto& r = Engine::instance().renderer();
        float gridSize = 64.0f;

        // Set up visible line rendering
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);  // don't occlude other geometry
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        GLfloat oldLineWidth;
        glGetFloatv(GL_LINE_WIDTH, &oldLineWidth);
        glLineWidth(3.0f);

        // Get terrain heights at the four corners
        float hNW = terrainBlock.sampleHeight(gx0, gz_north);
        float hNE = terrainBlock.sampleHeight(gx1, gz_north);
        float hSW = terrainBlock.sampleHeight(gx0, gz_south);
        float hSE = terrainBlock.sampleHeight(gx1, gz_south);
        float maxHeight = std::max({hNW, hNE, hSW, hSE});
        float ceilingH = maxHeight + 150.0f;

        // Draw grid lines on each wall (north/south/east/west) + ceiling perimeter
        // Using GameGrid.png color (T2 yellowish-green), no floor (terrain is ground)

        // North wall: vertical lines + horizontal lines at terrain/ceiling height
        for (float gx = gx0; gx <= gx1; gx += gridSize) {
            float frac = (gx - gx0) / missionArea.width;
            float h = hNW + frac * (hNE - hNW);
            r.drawLine(Point3F{gx, h, gz_north}, Point3F{gx, ceilingH, gz_north}, gridCol);
        }
        // South wall
        for (float gx = gx0; gx <= gx1; gx += gridSize) {
            float frac = (gx - gx0) / missionArea.width;
            float h = hSW + frac * (hSE - hSW);
            r.drawLine(Point3F{gx, h, gz_south}, Point3F{gx, ceilingH, gz_south}, gridCol);
        }
        // West wall
        for (float gz = gz_south; gz <= gz_north; gz += gridSize) {
            float frac = (gz - gz_south) / missionArea.height;
            float h = hSW + frac * (hNW - hSW);
            r.drawLine(Point3F{gx0, h, gz}, Point3F{gx0, ceilingH, gz}, gridCol);
        }
        // East wall
        for (float gz = gz_south; gz <= gz_north; gz += gridSize) {
            float frac = (gz - gz_south) / missionArea.height;
            float h = hSE + frac * (hNE - hSE);
            r.drawLine(Point3F{gx1, h, gz}, Point3F{gx1, ceilingH, gz}, gridCol);
        }
        // Ceiling perimeter (top edges of north/south/east/west walls)
        r.drawLine(Point3F{gx0, ceilingH, gz_north}, Point3F{gx1, ceilingH, gz_north}, gridCol);
        r.drawLine(Point3F{gx0, ceilingH, gz_south}, Point3F{gx1, ceilingH, gz_south}, gridCol);
        r.drawLine(Point3F{gx0, ceilingH, gz_north}, Point3F{gx0, ceilingH, gz_south}, gridCol);
        r.drawLine(Point3F{gx1, ceilingH, gz_north}, Point3F{gx1, ceilingH, gz_south}, gridCol);
        // Ceiling grid lines
        for (float gx = gx0 + gridSize; gx < gx1; gx += gridSize)
            r.drawLine(Point3F{gx, ceilingH, gz_north}, Point3F{gx, ceilingH, gz_south}, gridCol);
        for (float gz = gz_south + gridSize; gz < gz_north; gz += gridSize)
            r.drawLine(Point3F{gx0, ceilingH, gz}, Point3F{gx1, ceilingH, gz}, gridCol);

        // Restore GL state
        glLineWidth(oldLineWidth);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
skip_grid:

    // Render item pickups
    float time = Engine::instance().game().gameTime();
    for (auto& item : items) {
        if (!item.renderProxy) continue;
        if (!item.active) continue;
        float bob = sinf(time * 2.0f + item.pos.x * 0.1f) * 0.3f;
        ColorF col;
        switch (item.type) {
            case ItemPickup::Health: col = {0.2f, 1.0f, 0.2f, 1.0f}; break;
            case ItemPickup::Energy: col = {0.2f, 0.8f, 1.0f, 1.0f}; break;
            case ItemPickup::Ammo:   col = {1.0f, 0.8f, 0.2f, 1.0f}; break;
        }
        Box3F box = {{item.pos.x - 0.4f, item.pos.y - 0.4f + bob, item.pos.z - 0.4f},
                     {item.pos.x + 0.4f, item.pos.y + 0.4f + bob, item.pos.z + 0.4f}};
        r.drawBox(box, col);
    }

    if (!waterRendered) {
        r.flushSpriteBatch();
        renderWater();
    }

    // Render projectiles as sprites after the water transition.
    for (auto& p : projList) {
        if (!p.active) continue;
        ColorF col;
        switch (p.type) {
            case ProjectileType::Disc:    col = {1.0f, 0.6f, 0.1f, 1.0f}; break;
            case ProjectileType::Bolt:    col = {0.2f, 0.8f, 1.0f, 1.0f}; break;
            case ProjectileType::Grenade:
            case ProjectileType::Mortar:  col = {0.3f, 1.0f, 0.3f, 1.0f}; break;
            default:                      col = {1,1,1,1};    break;
        }
        r.drawSprite(p.pos, 0.3f, col);
    }

    // Render explosion sprites with bright center
    for (auto& e : explosions) {
        float t = e.lifetime / e.maxLifetime;
        float size = e.radius * (1.0f + (1.0f - t) * 2.0f);
        // Hot white core (shrinks over time)
        float coreSize = size * 0.3f * t;
        r.drawSprite(e.pos, coreSize, {1.0f, 1.0f, 0.9f, t * 0.8f});
        // Colored fireball (expands, fades)
        r.drawSprite(e.pos, size * 0.5f, {e.color.r, e.color.g, e.color.b, t * 0.4f});
    }
    for (const auto& light : effectLights) {
        const float fade = dynamicLightFade(light.age, light.delay, light.lifetime);
        if (fade <= 0.0f) continue;
        r.drawSprite(light.pos, light.radius * (0.65f + 0.35f * fade),
                     {1.0f, 0.72f, 0.28f, 0.28f * fade});
    }

    // All remaining effects share an explicit depth-tested, depth-read-only
    // state. Their batches cannot leak additive blending into later passes.
    r.beginTransparentPass();
    renderParticles();
    renderPrecipitation();
    r.endTransparentPass();

    // Render bots
    auto* shader = ShaderManager::getDefaultShader();
    if (shader) shader->bind();
    for (auto& b : bots) {
        if (!b.alive) continue;
        if (!b.shape) {
            // Load shape on first render
            auto& fs = Engine::instance().fs();
            for (auto* p : {"shapes/bioderm_light.dts", "shapes/bioderm_medium.dts", "shapes/bioderm_heavy.dts"}) {
                auto d = fs.read(p);
                if (!d.empty()) {
                    DTSShape* s = new DTSShape;
                    s->name = "bot";
                    if (s->load(d.data(), d.size())) {
                        b.shape = s;
                        break;
                    }
                    delete s;
                }
            }
        }
        if (b.shape && b.shape->loaded) {
            MatrixF model;
            Point3F ax = {0, 1, 0};
            model.setRotationAxis(ax, b.moveYaw);
            model.setTranslation(b.pos);
            Engine::instance().renderer().setModel(model * b.shape->upOrientation());
            shader->setUniform("uUseTexture", (int32_t)0);
            shader->setUniform("uUseLightmap", (int32_t)0);
            b.shape->render(0);
        }
        // Health bar above bot
        {
            auto& ren = Engine::instance().renderer();
            auto* font = ren.getFont();
            if (font) {
                Point3F above = {b.pos.x, b.pos.y + 2.5f, b.pos.z};
                Point3F screen = worldToScreen(above, ren.viewMatrix(),
                    ren.projectionMatrix(), ren.config().width, ren.config().height);
                if (screen.z > 0 && screen.x >= 0 && screen.x <= ren.config().width && screen.y >= 0 && screen.y <= ren.config().height) {
                    float barW = 40, barH = 5;
                    float by = screen.y - 15;
                    Engine::instance().renderer().drawBox({{screen.x - barW/2 - 1, by - 1, 0},
                        {screen.x + barW/2 + 1, by + barH + 1, 0}}, {0,0,0,0.5f});
                    float hp = std::max(0.0f, b.health / 100.0f);
                    ColorF hc = hp > 0.5f ? ColorF{0,1,0,0.9f} : hp > 0.25f ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f};
                    Engine::instance().renderer().drawBox({{screen.x - barW/2, by, 0},
                        {screen.x - barW/2 + barW * hp, by + barH, 0}}, hc);
                }
            }
        }
    }
}

void World::spawnBots(int count) {
    bots.clear();
    for (int i = 0; i < count; i++) {
        Bot b;
        b.startPos = {20.0f + (i % 5) * 15.0f, 5.0f, 20.0f + (i / 5) * 15.0f};
        b.pos = b.startPos;
        b.health = 100.0f;
        b.patrolOffset = i * 0.5f;
        b.moveYaw = 0;
        bots.push_back(b);
    }
    Console::instance().printf(LogLevel::Info, "Spawned %d bots", count);
}

void World::addObject(const WorldObject& obj) {
    WorldObject stored = obj;
    if (stored.shape && stored.shape->loaded && stored.boundsRadius <= 0.0f) {
        float radiusSquared = 0.0f;
        for (const auto& mesh : stored.shape->meshes) {
            for (const auto& vertex : mesh.vertices) {
                const float lengthSquared = vertex.pos.x * vertex.pos.x +
                    vertex.pos.y * vertex.pos.y + vertex.pos.z * vertex.pos.z;
                radiusSquared = std::max(radiusSquared, lengthSquared);
            }
        }
        stored.boundsRadius = std::sqrt(radiusSquared);
    }
    if (stored.shape) {
        stored.translucent = std::any_of(
            stored.shape->materialFlags.begin(), stored.shape->materialFlags.end(),
            [](uint32_t flags) {
                return (flags & (MatFlag_Translucent | MatFlag_Additive)) != 0;
            });
    }
    worldObjects.push_back(std::move(stored));
    const auto& added = worldObjects.back();
    if (ScriptEngine::exists() && !added.objectName.empty() &&
         (added.className == "StaticShape" || added.className == "TSStatic" ||
          added.className == "Turret" || added.className.ends_with("Turret") ||
          added.className == "Item" ||
          added.className == "Player" || added.className == "Vehicle" ||
         added.className.ends_with("Vehicle"))) {
        if (auto* ts = ScriptEngine::instance().ts())
            ts->callFunction(added.className + "::onAdd", {VMValue(added.objectName)});
    }
}

namespace {
World::WorldObject* findObjective(std::vector<World::WorldObject>& objects,
                                   const std::string& name) {
    for (auto& object : objects)
        if (object.missionObjective && object.objectName == name) return &object;
    return nullptr;
}

void dispatchObjectiveLifecycle(const World::WorldObject& objective, const char* callback) {
    if (!ScriptEngine::exists()) return;
    auto* ts = ScriptEngine::instance().ts();
    if (!ts || objective.objectName.empty()) return;
    const std::string function = std::string("AIObjective::") + callback;
    // callFunction also resolves native callbacks, which stock compatibility
    // hooks use when no TorqueScript definition was loaded.
    ts->callFunction(function, {VMValue(objective.objectName), VMValue(objective.objectiveState)});
}
}

bool World::setObjectiveActive(const std::string& name, bool active) {
    auto* objective = findObjective(worldObjects, name);
    if (!objective) return false;
    if (objective->objectiveActive == active) return true;
    objective->objectiveActive = active;
    objective->objectiveState = active ? 1 : 0;
    dispatchObjectiveLifecycle(*objective, active ? "onActivate" : "onDeactivate");
    return true;
}

bool World::setObjectiveState(const std::string& name, int state) {
    if (state < 0 || state > 3) return false;
    auto* objective = findObjective(worldObjects, name);
    if (!objective) return false;
    if (objective->objectiveState == state) return true;
    objective->objectiveState = state;
    objective->objectiveActive = state != 2 && state != 3;
    if (state == 1) dispatchObjectiveLifecycle(*objective, "onActivate");
    else if (state == 2) dispatchObjectiveLifecycle(*objective, "onComplete");
    else if (state == 3) dispatchObjectiveLifecycle(*objective, "onFail");
    else dispatchObjectiveLifecycle(*objective, "onDeactivate");
    return true;
}

bool World::setObjectiveTarget(const std::string& name, const std::string& target, int targetId) {
    auto* objective = findObjective(worldObjects, name);
    if (!objective || (target.empty() && targetId < 0)) return false;
    objective->objectiveTarget = target;
    objective->objectiveTargetId = targetId;
    return true;
}

bool World::setObjectiveWeight(const std::string& name, int level, float weight) {
    if (level < 0 || level >= 4 || !std::isfinite(weight)) return false;
    auto* objective = findObjective(worldObjects, name);
    if (!objective) return false;
    objective->objectiveWeights[level] = weight;
    if (level == 0) objective->objectiveWeight = weight;
    return true;
}

bool World::setObjectiveScore(const std::string& name, float score) {
    if (!std::isfinite(score)) return false;
    auto* objective = findObjective(worldObjects, name);
    if (!objective) return false;
    objective->objectiveScore = score;
    return true;
}

bool World::setObjectiveTeam(const std::string& name, int team) {
    if (team < 0) return false;
    auto* objective = findObjective(worldObjects, name);
    if (!objective) return false;
    objective->teamId = team;
    return true;
}

void World::resetTriggerTracking() {
    for (auto& object : worldObjects)
        object.triggerOccupants.clear();
}

bool World::setMissionObjectEnabled(const std::string& name, bool enabled) {
    for (auto& object : worldObjects) {
        if (object.objectName != name) continue;
        if (object.className == "PhysicalZone") {
            object.physicalActive = enabled;
            return true;
        }
        if (object.className == "ForceFieldBare") {
            object.forceFieldOpen = !enabled;
            return true;
        }
        if (object.className == "Item") {
            object.itemActive = enabled;
            return true;
        }
        return false;
    }
    return false;
}

bool World::setMissionObjectHidden(const std::string& name, bool hidden) {
    for (auto& object : worldObjects) {
        if (object.objectName != name) continue;
        if (object.className != "StaticShape" && object.className != "TSStatic" &&
            object.className != "Turret" && object.className != "Item" &&
            object.className != "Vehicle" && !object.className.ends_with("Vehicle"))
            return false;
        object.visible = !hidden;
        return true;
    }
    return false;
}

bool World::setMissionObjectTransform(const std::string& name, const std::string& transform) {
    float values[7]{};
    if (sscanf(transform.c_str(), "%f %f %f %f %f %f %f", &values[0], &values[1],
               &values[2], &values[3], &values[4], &values[5], &values[6]) != 7)
        return false;
    for (float value : values) if (!std::isfinite(value)) return false;
    for (auto& object : worldObjects) {
        if (object.objectName != name) continue;
        if (object.className != "StaticShape" && object.className != "TSStatic" &&
            object.className != "Turret" && object.className != "Item" &&
            object.className != "Vehicle" && !object.className.ends_with("Vehicle"))
            return false;
        object.pos = {values[0], values[1], values[2]};
        object.rot = {values[3], values[4], values[5]};
        object.rotAngleDeg = values[6];
        return true;
    }
    return false;
}

bool World::mountMissionObjectImage(const std::string& name, const std::string& image, int slot) {
    if (image.empty() || slot < 0 || slot >= 8) return false;
    for (auto& object : worldObjects) {
        if (object.objectName != name) continue;
        if (object.className != "Turret" && !object.className.ends_with("Turret") &&
            object.className != "Vehicle" &&
            !object.className.ends_with("Vehicle")) return false;
        object.mountedImages[slot] = image;
        if (slot == 0) object.mountedShapeName = image;
        if (auto* ts = ScriptEngine::instance().ts()) {
            const std::string callback = object.className + "::onMount";
            if (ts->hasFunction(callback))
                ts->callFunction(callback, {VMValue(name), VMValue(image), VMValue(slot)});
        }
        return true;
    }
    return false;
}

bool World::unmountMissionObjectImage(const std::string& name, int slot) {
    if (slot < 0 || slot >= 8) return false;
    for (auto& object : worldObjects) {
        if (object.objectName != name) continue;
        if (object.className != "Turret" && !object.className.ends_with("Turret") &&
            object.className != "Vehicle" &&
            !object.className.ends_with("Vehicle")) return false;
        const std::string image = object.mountedImages[slot];
        object.mountedImages[slot].clear();
        if (slot == 0) {
            object.mountedShapeName.clear();
            object.mountedShape = nullptr;
        }
        if (auto* ts = ScriptEngine::instance().ts()) {
            const std::string callback = object.className + "::onUnmount";
            if (ts->hasFunction(callback))
                ts->callFunction(callback, {VMValue(name), VMValue(image), VMValue(slot)});
        }
        return true;
    }
    return false;
}

bool World::deleteMissionObject(const std::string& name) {
    if (name.empty()) return false;
    auto it = std::find_if(worldObjects.begin(), worldObjects.end(),
        [&name](const WorldObject& object) { return object.objectName == name; });
    if (it == worldObjects.end()) return false;
    if (auto* ts = ScriptEngine::instance().ts()) {
        ts->cancelEventsForObject(name);
        if (!it->className.empty()) {
            const std::string callback = it->className + "::onRemove";
            if (ts->hasFunction(callback)) ts->callFunction(callback, {VMValue(name)});
        }
    }
    worldObjects.erase(it);
    for (auto& object : worldObjects) object.triggerOccupants.erase(name);
    return true;
}

void Game::selectMapperObserverCamera(int index) {
    if (!mapperMode || index < 1 || index > (int)w->observerCameras().size())
        return;
    const auto& camera = w->observerCameras()[index - 1];
    freeCamPos = Math::torquePointToYUp(camera.pos);
    MatrixF rotation = Math::torqueRotationToYUp(
        camera.axis, -Math::DEG2RAD(camera.angleDeg));
    Point3F forward = rotation.transform({0, 0, -1});
    freeCamTarget = {
        freeCamPos.x + forward.x * 100.0f,
        freeCamPos.y + forward.y * 100.0f,
        freeCamPos.z + forward.z * 100.0f,
    };
    setFreeCamTarget(freeCamTarget);
    Console::instance().printf(LogLevel::Info,
        "Mapper: using observer camera %d", index);
}

void World::spawnProjectile(const Projectile& p) {
    projList.push_back(p);
}

float World::getHeight(float x, float z) const {
    // First check interior collision (buildings, etc.)
    if (interiorCollision.loaded) {
        float interiorH = interiorCollision.getHeight(x, z);
        if (interiorH > -1e9f) return interiorH;
    }

    // Fall back to terrain height
    if (!terrainBlock.loaded || terrainBlock.heights.empty() ||
        terrainBlock.isEmptySquare(x, z)) return -1e9f;

    return terrainBlock.sampleHeight(x, z);
}

float World::getFloorHeight(float x, float y, float z) const {
    if (interiorCollision.loaded) {
        const float interior = interiorCollision.getFloorHeight(x, y, z);
        if (interior > -1e9f) return interior;
    }
    if (!terrainBlock.loaded || terrainBlock.heights.empty() || terrainBlock.isEmptySquare(x, z))
        return -1e9f;
    const float terrain = terrainBlock.sampleHeight(x, z);
    return terrain <= y + 0.001f ? terrain : -1e9f;
}

World::PhysicalZoneEffect World::physicalZoneEffect(const Point3F& position) const {
    PhysicalZoneEffect result;
    for (const auto& object : worldObjects) {
        if (object.className != "PhysicalZone" || !object.physicalActive)
            continue;

        // Mission positions/scales are authored in T2's Z-up frame.  World
        // movement uses Y-up, and the existing mapper volume visualization
        // uses the same axis conversion.
        const Point3F center = Math::torquePointToYUp(object.pos);
        const Point3F half{
            std::max(0.5f, std::fabs(object.scale.x) * 0.5f),
            std::max(0.5f, std::fabs(object.scale.z) * 0.5f),
            std::max(0.5f, std::fabs(object.scale.y) * 0.5f)};
        if (std::fabs(position.x - center.x) > half.x ||
            std::fabs(position.y - center.y) > half.y ||
            std::fabs(position.z - center.z) > half.z)
            continue;

        result.velocityMod *= std::max(0.0f, object.physicalVelocityMod);
        result.gravityMod *= std::max(0.0f, object.physicalGravityMod);
        const Point3F force = Math::torquePointToYUp(object.physicalForce);
        result.appliedForce.x += force.x;
        result.appliedForce.y += force.y;
        result.appliedForce.z += force.z;
    }
    return result;
}

void Game::setTimeScale(float value) {
    timeScale = GameTime::clampScale(value);
}

// ─── Particle System ──────────────────────────────────────────

void World::spawnExplosion(const Point3F& pos, const ColorF& color, float radius, int count) {
    // 1. Central bright flash (short-lived)
    {
        Particle p;
        p.pos = pos;
        p.vel = {0, 0, 0};
        p.lifetime = 0.15f;
        p.maxLifetime = 0.15f;
        p.size = radius * 0.8f;
        p.color = {1.0f, 0.95f, 0.8f, 1.0f};
        p.active = true;
        if (particles.size() < 1000) particles.push_back(p);
    }
    // 2. Fireball particles (orange/red, medium life)
    int fireCount = count / 2;
    for (int i = 0; i < fireCount; i++) {
        Particle p;
        float theta = ((float)std::rand() / RAND_MAX) * 3.14159f * 2.0f;
        float phi = ((float)std::rand() / RAND_MAX) * 3.14159f;
        float speed = ((float)std::rand() / RAND_MAX) * radius * 4.0f;
        p.pos = pos;
        p.vel = {sinf(phi) * cosf(theta) * speed, fabsf(cosf(phi)) * speed * 0.8f, sinf(phi) * sinf(theta) * speed};
        p.lifetime = 0.3f + ((float)std::rand() / RAND_MAX) * 0.4f;
        p.maxLifetime = p.lifetime;
        p.size = 0.3f + ((float)std::rand() / RAND_MAX) * 0.5f;
        // Vary from bright yellow to deep orange
        float t = (float)std::rand() / RAND_MAX;
        p.color = {1.0f, 0.4f + t * 0.5f, t * 0.2f, 1.0f};
        p.active = true;
        if (particles.size() < 1000) particles.push_back(p);
    }
    // 3. Smoke particles (dark, slower, longer-lived)
    int smokeCount = count / 3;
    for (int i = 0; i < smokeCount; i++) {
        Particle p;
        float theta = ((float)std::rand() / RAND_MAX) * 3.14159f * 2.0f;
        float phi = ((float)std::rand() / RAND_MAX) * 3.14159f * 0.5f;
        float speed = ((float)std::rand() / RAND_MAX) * radius * 1.5f;
        p.pos = pos;
        p.vel = {sinf(phi) * cosf(theta) * speed, fabsf(cosf(phi)) * speed * 0.5f + 1.0f, sinf(phi) * sinf(theta) * speed};
        p.lifetime = 0.6f + ((float)std::rand() / RAND_MAX) * 0.8f;
        p.maxLifetime = p.lifetime;
        p.size = 0.5f + ((float)std::rand() / RAND_MAX) * 0.8f;
        p.color = {0.3f, 0.3f, 0.3f, 0.6f};
        p.active = true;
        if (particles.size() < 1000) particles.push_back(p);
    }
}

void World::spawnExplosionEffect(const Point3F& pos,
                                 const V12::DecodedDataBlock* projectileData,
                                 const V12::DecodedDataBlock* explosionData,
                                 const std::map<uint32_t, ParsedDataBlock>* dataBlocks,
                                 const Point3F& impactNormal) {
    static constexpr size_t maxEffectInstances = 4096;
    if (!projectileData || !dataBlocks) {
        spawnExplosion(pos, {1.0f, 0.7f, 0.3f, 1.0f}, 2.0f, 15);
        return;
    }
    const auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks->find(id);
        return it == dataBlocks->end() ? nullptr : &it->second.decoded;
    };
    for (uint32_t ref : projectileData->projectileDecalRefs) {
        const auto* block = find(ref);
        if (!block || !block->hasDecal || block->decal.texture.empty()) continue;
        const DecalBasis basis = makeDecalBasis(pos, impactNormal);
        bool duplicate = false;
        for (const auto& existing : effectDecals) {
            if (existing.sourceRef == ref && decalIsDuplicate(basis,
                    makeDecalBasis(existing.pos, existing.normal), ref)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            EffectDecal decal;
            decal.data = block->decal;
            decal.sourceRef = ref;
            decal.normal = basis.normal;
            decal.pos = basis.position;
            decal.sizeX = std::max(0.001f, std::fabs(block->decal.sizeX));
            decal.sizeY = std::max(0.001f, std::fabs(block->decal.sizeY));
            decal.lifetime = std::max(0.1f, block->decal.lifetimeMS / 1000.0f);
            decal.frameSeed = ref * 2654435761u ^ (uint32_t)std::lround(pos.x * 100.0f) ^
                (uint32_t)std::lround(pos.y * 100.0f) ^ (uint32_t)std::lround(pos.z * 100.0f);
            if (block->decal.randomize) decal.angle = (decal.frameSeed / 4294967296.0f) * Math::PI * 2.0f;
            Engine::instance().renderer().loadTextureFrames(
                block->decal.texture.c_str(), decal.textures, decal.textureDurations);
            if (!decal.textures.empty()) decal.texture = decal.textures.front();
            if (effectDecals.size() < maxEffectInstances)
                effectDecals.push_back(std::move(decal));
        }
        break;
    }
    const V12::DecodedDataBlock* selectedExplosion = explosionData;
    if (projectileData->projectileUnderwaterExplosionRef != 0) {
        for (const auto& body : waterBodies) {
            if (pos.x < body.originX || pos.x > body.originX + body.sizeX ||
                pos.z < body.originZ || pos.z > body.originZ + body.sizeY) continue;
            if (body.level - pos.y >= projectileData->projectileDepthTolerance) {
                selectedExplosion = find(projectileData->projectileUnderwaterExplosionRef);
            }
            break;
        }
    }
    if (!selectedExplosion || !selectedExplosion->hasExplosion) {
        spawnExplosion(pos, {1.0f, 0.7f, 0.3f, 1.0f}, 2.0f, 15);
        return;
    }
    const auto addEmitter = [&](uint32_t emitterId, bool burst, int burstCount,
                                float effectLifetime, float effectDelay, const Point3F& origin) {
        const auto* emitterBlock = find(emitterId);
        if (!emitterBlock || !emitterBlock->hasEmitter || emitterBlock->emitter.particleRefs.empty()) return;
        const auto* particleBlock = find(emitterBlock->emitter.particleRefs.front());
        if (!particleBlock || !particleBlock->hasParticle) return;
        EffectEmitter emitter;
        emitter.pos = origin;
        emitter.emitter = emitterBlock->emitter;
        emitter.particle = particleBlock->particle;
        for (const std::string& name : emitter.particle.textures) {
            std::vector<uint32_t> frames;
            std::vector<float> durations;
            Engine::instance().renderer().loadTextureFrames(name.c_str(), frames, durations);
            if (durations.empty() && !frames.empty()) durations.assign(frames.size(), 1.0f);
            emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
            emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
        }
        if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
        emitter.burst = burst;
        emitter.burstCount = std::max(0, burstCount);
        emitter.delay = effectDelay;
        emitter.age = -effectDelay;
        if (effectLifetime > 0.0f) {
            emitter.lifetime = effectLifetime;
        } else {
            const float emitterLifetimeMS = (float)emitter.emitter.lifetimeMS +
                ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) *
                    emitter.emitter.lifetimeVarianceMS;
            emitter.lifetime = std::max(0.0f, emitterLifetimeMS / 1000.0f);
        }
        emitter.nextEmission = burst ? -1.0f : 0.0f;
        emitter.particles.reserve((size_t)std::min(burstCount, 512));
        if (effectEmitters.size() < maxEffectInstances)
            effectEmitters.push_back(std::move(emitter));
    };
    std::vector<const V12::DecodedDataBlock*> explosionStack;
    std::function<void(const V12::DecodedDataBlock*, int, const Point3F&)> spawnGraph;
    spawnGraph = [&](const V12::DecodedDataBlock* block, int depth, const Point3F& origin) {
        if (!block || !block->hasExplosion || depth > 4) return;
        if (std::find(explosionStack.begin(), explosionStack.end(), block) != explosionStack.end()) return;
        explosionStack.push_back(block);
        const auto& effect = block->explosion;
        const Point3F effectOrigin{origin.x, origin.y + effect.offset, origin.z};
        const int density = std::clamp(effect.particleDensity, 0, 512);
        const float lifetimeMS = (float)effect.lifetimeMS +
            ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * effect.lifetimeVarianceMS;
        const float effectLifetime = std::max(0.0f, lifetimeMS / 1000.0f);
        const float effectDelay = std::max(0.0f, (float)effect.delayMS / 1000.0f);
        if (effect.hasLight && effectLights.size() < maxEffectInstances) {
            EffectLight light;
            light.pos = effectOrigin;
            light.delay = effectDelay;
            light.lifetime = effectLifetime > 0.0f ? effectLifetime : 0.1f;
            light.radius = std::clamp(std::fabs(effect.particleRadius), 0.05f, 64.0f);
            light.color = {1.0f, 0.72f, 0.28f, 1.0f};
            effectLights.push_back(light);
        }
        if (effect.shakeCamera && effectCameraShakes.size() < maxEffectInstances) {
            EffectCameraShake shake;
            shake.pos = effectOrigin;
            shake.frequency = effect.shakeFrequency;
            shake.amplitude = effect.shakeAmplitude;
            shake.delay = effectDelay;
            shake.duration = std::max(0.0f, effect.shakeDuration);
            shake.radius = std::max(0.0f, effect.shakeRadius);
            shake.falloff = std::max(0.0f, effect.shakeFalloff);
            effectCameraShakes.push_back(shake);
        }
        if (effect.debrisRef) {
            const auto* debrisBlock = find(effect.debrisRef);
            const int debrisCount = effect.debrisNum > 0 ? std::clamp(effect.debrisNum +
                (int)((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * effect.debrisNumVariance,
                0, 128) : 1;
            for (int debrisIndex = 0; debrisBlock && debrisIndex < debrisCount &&
                 effectDebris.size() < maxEffectInstances; ++debrisIndex) {
                const auto& data = debrisBlock->debris;
                const float random01 = (float)std::rand() / (float)RAND_MAX;
                const float theta = random01 * Math::PI * 2.0f;
                Point3F normal = impactNormal;
                const float normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (normalLength > 0.001f) {
                    normal.x /= normalLength; normal.y /= normalLength; normal.z /= normalLength;
                } else normal = {0, 1, 0};
                EffectDebris debris;
                debris.pos = effectOrigin;
                const float baseSpeed = effect.debrisVelocity != 0.0f
                    ? effect.debrisVelocity : data.velocity;
                const float speedVariance = effect.debrisVelocity != 0.0f
                    ? effect.debrisVelocityVariance : data.velocityVariance;
                const float speed = std::max(0.0f, baseSpeed +
                    (random01 * 2.0f - 1.0f) * speedVariance);
                const float spread = std::max(0.0f, speedVariance * 0.25f);
                Point3F tangent{normal.y, -normal.x, 0};
                const float tangentLength = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
                if (tangentLength < 0.001f) tangent = {1, 0, 0};
                else { tangent.x /= tangentLength; tangent.y /= tangentLength; }
                const Point3F bitangent{normal.y * tangent.z - normal.z * tangent.y,
                                        normal.z * tangent.x - normal.x * tangent.z,
                                        normal.x * tangent.y - normal.y * tangent.x};
                debris.vel = {normal.x * speed + tangent.x * std::cos(theta) * spread + bitangent.x * std::sin(theta) * spread,
                              normal.y * speed + tangent.y * std::cos(theta) * spread + bitangent.y * std::sin(theta) * spread,
                              normal.z * speed + tangent.z * std::cos(theta) * spread + bitangent.z * std::sin(theta) * spread};
                debris.lifetime = std::max(0.05f, (data.lifetimeMS +
                    (random01 * 2.0f - 1.0f) * data.lifetimeVarianceMS) / 1000.0f);
                debris.elasticity = std::clamp(data.elasticity, 0.0f, 1.0f);
                debris.friction = std::clamp(data.friction, 0.0f, 1.0f);
                debris.gravModifier = std::max(0.0f, data.gravModifier);
                debris.terminalVelocity = std::max(0.0f, data.terminalVelocity);
                debris.maxBounces = std::max(0, data.numBounces +
                    (int)((random01 * 2.0f - 1.0f) * data.bounceVariance));
                debris.explodeOnMaxBounce = data.explodeOnMaxBounce;
                debris.rotation = data.minSpin + random01 * (data.maxSpin - data.minSpin);
                std::string path = normalizeShapePath(data.shape.empty() ? debrisBlock->debrisShape : data.shape);
                if (!path.empty()) {
                    auto shapeData = Engine::instance().fs().read(path.c_str());
                    if (!shapeData.empty()) {
                        DTSShape shape;
                        shape.name = path;
                        if (shape.load(shapeData.data(), shapeData.size())) {
                            debris.shapeIndex = (int)debrisShapes.size();
                            debrisShapes.push_back(std::move(shape));
                        }
                    }
                }
                effectDebris.push_back(std::move(debris));
            }
        }
        if (effect.particleEmitterRef)
            addEmitter(effect.particleEmitterRef, true, density, effectLifetime, effectDelay, effectOrigin);
        for (uint32_t ref : effect.emitterRefs)
            if (ref) addEmitter(ref, false, 0, effectLifetime, effectDelay, effectOrigin);
        if (effect.shockwaveRef) {
            const auto* shockwaveBlock = find(effect.shockwaveRef);
            if (shockwaveBlock && shockwaveBlock->hasShockwave) {
                EffectShockwave shockwave;
                shockwave.pos = effectOrigin;
                shockwave.data = shockwaveBlock->shockwave;
                const float delay = (float)shockwave.data.delayMS +
                    ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * shockwave.data.delayVariance;
                shockwave.age = -std::max(0.0f, delay / 1000.0f);
                shockwave.velocity = shockwave.data.velocity;
                const float lifetime = (float)shockwave.data.lifetimeMS +
                    ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * shockwave.data.lifetimeVariance;
                shockwave.lifetime = std::max(0.001f, lifetime / 1000.0f);
                if (!shockwave.data.textures.empty()) {
                    std::vector<uint32_t> frames;
                    std::vector<float> durations;
                    Engine::instance().renderer().loadTextureFrames(
                        shockwave.data.textures.front().c_str(), frames, durations);
                    if (!frames.empty()) shockwave.texture = frames.front();
                }
                if (shockwave.data.textures.size() > 1) {
                    std::vector<uint32_t> frames;
                    std::vector<float> durations;
                    Engine::instance().renderer().loadTextureFrames(
                        shockwave.data.textures[1].c_str(), frames, durations);
                    if (!frames.empty()) shockwave.mapTexture = frames.front();
                }
                if (effectShockwaves.size() < maxEffectInstances)
                    effectShockwaves.push_back(std::move(shockwave));
            }
        }
        for (uint32_t ref : effect.subExplosionRefs) {
            if (effectEmitters.size() >= maxEffectInstances &&
                effectShockwaves.size() >= maxEffectInstances) break;
            if (const auto* sub = find(ref)) spawnGraph(sub, depth + 1, effectOrigin);
        }
        explosionStack.pop_back();
    };
    spawnGraph(selectedExplosion, 0, pos);
}

void World::spawnSplashEffect(const Point3F& inputPos,
                              const V12::DecodedDataBlock& splashBlock,
                              const std::map<uint32_t, ParsedDataBlock>& dataBlocks) {
    if (!splashBlock.hasSplash) return;
    const auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks.find(id);
        return it == dataBlocks.end() ? nullptr : &it->second.decoded;
    };
    Point3F pos = inputPos;
    bool onWater = false;
    for (const auto& body : waterBodies) {
        if (pos.x >= body.originX && pos.x <= body.originX + body.sizeX &&
            pos.z >= body.originZ && pos.z <= body.originZ + body.sizeY) {
            pos.y = body.level;
            onWater = true;
            break;
        }
    }
    if (!onWater) {
        const float terrain = getHeight(pos.x, pos.z);
        if (terrain > -1e8f) pos.y = terrain;
    }

    const auto& data = splashBlock.splash;
    EffectShockwave ring;
    ring.pos = pos;
    ring.data.delayMS = data.delayMS;
    ring.data.delayVariance = data.delayVarianceMS;
    ring.data.lifetimeMS = data.ringLifetime > 0.0f ? (int32_t)data.ringLifetime : data.lifetimeMS;
    ring.data.lifetimeVariance = data.lifetimeVarianceMS;
    ring.data.width = std::fabs(data.width * data.scale.x);
    ring.data.height = data.height * data.scale.y;
    ring.data.velocity = data.velocity * data.scale.x;
    ring.data.acceleration = data.acceleration * data.scale.x;
    ring.data.texWrap = data.texWrap * data.texFactor;
    ring.data.numSegments = (int)data.numSegments;
    ring.data.renderBottom = false;
    ring.radius = data.startRadius * data.scale.x;
    ring.velocity = ring.data.velocity;
    const float delay = (float)data.delayMS +
        ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * data.delayVarianceMS;
    ring.age = -std::max(0.0f, delay / 1000.0f);
    const float lifetime = data.ringLifetime > 0.0f ? data.ringLifetime : (float)data.lifetimeMS;
    ring.lifetime = std::max(0.001f, lifetime / 1000.0f);
    ring.data.colors = data.colors;
    ring.data.times = data.times;
    for (const auto& textureName : data.textures) {
        std::vector<uint32_t> frames;
        std::vector<float> durations;
        Engine::instance().renderer().loadTextureFrames(textureName.c_str(), frames, durations);
        if (!frames.empty()) {
            if (ring.texture == UINT32_MAX) ring.texture = frames.front();
            else if (ring.mapTexture == UINT32_MAX) ring.mapTexture = frames.front();
        }
    }
    ring.drawMapTexture = ring.mapTexture != UINT32_MAX;
    if (effectShockwaves.size() < 4096) effectShockwaves.push_back(std::move(ring));

    for (uint32_t emitterId : data.emitterRefs) {
        const auto* emitterBlock = find(emitterId);
        if (!emitterBlock || !emitterBlock->hasEmitter || emitterBlock->emitter.particleRefs.empty()) continue;
        const auto* particleBlock = find(emitterBlock->emitter.particleRefs.front());
        if (!particleBlock || !particleBlock->hasParticle || effectEmitters.size() >= 4096) continue;
        EffectEmitter emitter;
        emitter.pos = pos;
        emitter.emitter = emitterBlock->emitter;
        emitter.particle = particleBlock->particle;
        emitter.burst = false;
        emitter.lifetime = std::max(0.001f, data.lifetimeMS / 1000.0f);
        emitter.age = -std::max(0.0f, delay / 1000.0f);
        for (const auto& name : emitter.particle.textures) {
            std::vector<uint32_t> frames;
            std::vector<float> durations;
            Engine::instance().renderer().loadTextureFrames(name.c_str(), frames, durations);
            emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
            emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
        }
        if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
        effectEmitters.push_back(std::move(emitter));
    }
    if (data.explosionRef) {
        const auto* explosionData = find(data.explosionRef);
        if (explosionData && explosionData->hasExplosion) {
            V12::DecodedDataBlock projectileData;
            projectileData.projectileExplosionRef = data.explosionRef;
            spawnExplosionEffect(pos, &projectileData, explosionData, &dataBlocks);
        }
    }
}

void World::clearEffects() {
    particles.clear();
    explosions.clear();
    effectEmitters.clear();
    effectShockwaves.clear();
    effectDebris.clear();
    effectDecals.clear();
    effectLightnings.clear();
    effectLights.clear();
    effectCameraShakes.clear();
}

void World::beginProjectileTrailSync() { ++trailGeneration; }

void World::syncProjectileTrail(int ownerId, const Point3F& pos, const Point3F& velocity,
                                const V12::DecodedDataBlock* projectileData,
                                const std::map<uint32_t, ParsedDataBlock>* dataBlocks) {
    if (!projectileData || !dataBlocks || !projectileData->projectileBaseEmitterRef) return;
    auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks->find(id);
        return it == dataBlocks->end() ? nullptr : &it->second.decoded;
    };
    const auto* emitterBlock = find(projectileData->projectileBaseEmitterRef);
    if (!emitterBlock || !emitterBlock->hasEmitter || emitterBlock->emitter.particleRefs.empty()) return;
    const auto* particleBlock = find(emitterBlock->emitter.particleRefs.front());
    if (!particleBlock || !particleBlock->hasParticle) return;
    auto it = std::find_if(effectEmitters.begin(), effectEmitters.end(),
        [ownerId](const EffectEmitter& e) { return e.projectileTrail && e.ownerId == ownerId; });
    if (it == effectEmitters.end()) {
        EffectEmitter emitter;
        emitter.pos = pos; emitter.ownerVelocity = velocity; emitter.ownerId = ownerId;
        emitter.projectileTrail = true; emitter.emitter = emitterBlock->emitter;
        emitter.particle = particleBlock->particle; emitter.nextEmission = 0.0f;
        emitter.axis = velocity;
        const float axisLength = std::sqrt(emitter.axis.x * emitter.axis.x +
            emitter.axis.y * emitter.axis.y + emitter.axis.z * emitter.axis.z);
        if (axisLength > 0.0001f) {
            emitter.axis.x /= axisLength; emitter.axis.y /= axisLength; emitter.axis.z /= axisLength;
        } else emitter.axis = {0, 1, 0};
        for (const std::string& name : emitter.particle.textures) {
            std::vector<uint32_t> frames;
            std::vector<float> durations;
            Engine::instance().renderer().loadTextureFrames(name.c_str(), frames, durations);
            if (durations.empty() && !frames.empty()) durations.assign(frames.size(), 1.0f);
            emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
            emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
        }
        if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
        effectEmitters.push_back(std::move(emitter));
        it = std::prev(effectEmitters.end());
    } else {
        it->pos = pos; it->ownerVelocity = velocity;
        it->axis = velocity;
        const float axisLength = std::sqrt(it->axis.x * it->axis.x +
            it->axis.y * it->axis.y + it->axis.z * it->axis.z);
        if (axisLength > 0.0001f) {
            it->axis.x /= axisLength; it->axis.y /= axisLength; it->axis.z /= axisLength;
        }
    }
    it->trailGeneration = trailGeneration;
}

void World::endProjectileTrailSync() {
    effectEmitters.erase(std::remove_if(effectEmitters.begin(), effectEmitters.end(),
        [this](const EffectEmitter& e) { return e.projectileTrail && e.trailGeneration != trailGeneration; }),
        effectEmitters.end());
}

void World::removeProjectileTrail(int ownerId) {
    effectEmitters.erase(std::remove_if(effectEmitters.begin(), effectEmitters.end(),
        [ownerId](const EffectEmitter& e) { return e.projectileTrail && e.ownerId == ownerId; }),
        effectEmitters.end());
}

void World::spawnTrail(const Point3F& pos, const ColorF& color, float size) {
    Particle p;
    p.pos = pos;
    p.vel = {0, 0, 0};
    p.lifetime = 0.3f;
    p.maxLifetime = 0.3f;
    p.size = size;
    p.color = color;
    p.color.a = 0.5f;
    p.active = true;
    if (particles.size() < 1000) particles.push_back(p);
}

void World::updateParticles(float dt) {
    for (auto& p : particles) {
        if (!p.active) continue;
        p.lifetime -= dt;
        if (p.lifetime <= 0) { p.active = false; continue; }
        p.vel.y -= 5.0f * dt; // gravity
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
        p.size += dt * 0.5f; // expand
        float t = p.lifetime / p.maxLifetime;
        p.color.a = t; // fade out
    }
    // Remove dead particles
    particles.erase(std::remove_if(particles.begin(), particles.end(),
        [](const Particle& p) { return !p.active; }), particles.end());

    for (auto& debris : effectDebris) {
        if (!debris.active) continue;
        debris.age += dt;
        if (debris.age >= debris.lifetime) { debris.active = false; continue; }
        debris.vel.y -= 9.81f * debris.gravModifier * dt;
        if (debris.terminalVelocity > 0.0f) {
            const float speed = std::sqrt(debris.vel.x * debris.vel.x + debris.vel.y * debris.vel.y +
                                          debris.vel.z * debris.vel.z);
            if (speed > debris.terminalVelocity) {
                const float scale = debris.terminalVelocity / speed;
                debris.vel.x *= scale; debris.vel.y *= scale; debris.vel.z *= scale;
            }
        }
        debris.pos.x += debris.vel.x * dt;
        debris.pos.y += debris.vel.y * dt;
        debris.pos.z += debris.vel.z * dt;
        const float floor = getFloorHeight(debris.pos.x, debris.pos.y, debris.pos.z);
        if (floor > -1e8f && debris.pos.y - debris.radius <= floor && debris.vel.y < 0.0f) {
            debris.pos.y = floor + debris.radius;
            debris.vel.y = -debris.vel.y * debris.elasticity;
            debris.vel.x *= std::max(0.0f, 1.0f - debris.friction * dt);
            debris.vel.z *= std::max(0.0f, 1.0f - debris.friction * dt);
            if (++debris.bounces > debris.maxBounces ||
                (std::fabs(debris.vel.y) < 0.15f && std::fabs(debris.vel.x) < 0.15f &&
                 std::fabs(debris.vel.z) < 0.15f))
                if (debris.explodeOnMaxBounce && debris.bounces > debris.maxBounces)
                    spawnExplosion(debris.pos, {1.0f, 0.65f, 0.25f, 1.0f}, 0.7f, 6);
            if (debris.bounces > debris.maxBounces ||
                (std::fabs(debris.vel.y) < 0.15f && std::fabs(debris.vel.x) < 0.15f &&
                 std::fabs(debris.vel.z) < 0.15f))
                debris.active = false;
        } else if (interiorCollision.loaded) {
            Point3F push{};
            if (interiorCollision.sphereCollide(debris.pos, debris.radius, push)) {
                debris.pos.x += push.x; debris.pos.y += push.y; debris.pos.z += push.z;
                debris.vel.y = -debris.vel.y * debris.elasticity;
                debris.vel.x *= std::max(0.0f, 1.0f - debris.friction * dt);
                debris.vel.z *= std::max(0.0f, 1.0f - debris.friction * dt);
                if (++debris.bounces > debris.maxBounces) debris.active = false;
            }
        }
        debris.rotation += dt;
    }
    effectDebris.erase(std::remove_if(effectDebris.begin(), effectDebris.end(),
        [](const EffectDebris& debris) { return !debris.active; }), effectDebris.end());

    auto random01 = []() { return (float)std::rand() / (float)RAND_MAX; };
    for (auto& emitter : effectEmitters) {
        const float previousAge = emitter.age;
        emitter.age += dt;
        if (emitter.age < 0.0f) continue;
        auto emitOne = [&](float ageOffset) {
            EffectParticle p;
            const float theta = Math::DEG2RAD(emitter.emitter.thetaMin + random01() *
                (emitter.emitter.thetaMax - emitter.emitter.thetaMin));
            // phiReferenceVel is angular velocity in degrees/sec. Variance is
            // sampled from [0, variance], as in ParticleSystem.ts.
            const float phi = Math::DEG2RAD(emitter.age * emitter.emitter.phiReferenceVel +
                random01() * emitter.emitter.phiVariance);

            // Start along the emitter axis, then apply theta and phi. The
            // perpendicular basis matches ParticleSystem.ts for arbitrary axes.
            const Point3F axis = emitter.axis;
            Point3F axisX = std::fabs(axis.z) < 0.9f
                ? Point3F{axis.y, -axis.x, 0}
                : Point3F{-axis.z, 0, axis.x};
            const float axisXLength = std::sqrt(axisX.x * axisX.x + axisX.y * axisX.y + axisX.z * axisX.z);
            if (axisXLength > 0.0001f) {
                axisX.x /= axisXLength; axisX.y /= axisXLength; axisX.z /= axisXLength;
            } else axisX = {1, 0, 0};
            Point3F dir{
                axis.x * std::cos(theta) + axisX.x * std::sin(theta),
                axis.y * std::cos(theta) + axisX.y * std::sin(theta),
                axis.z * std::cos(theta) + axisX.z * std::sin(theta)
            };
            const float cosPhi = std::cos(phi), sinPhi = std::sin(phi);
            const Point3F cross{
                axis.y * dir.z - axis.z * dir.y,
                axis.z * dir.x - axis.x * dir.z,
                axis.x * dir.y - axis.y * dir.x
            };
            const float dot = dir.x * axis.x + dir.y * axis.y + dir.z * axis.z;
            dir = {
                dir.x * cosPhi + cross.x * sinPhi + axis.x * dot * (1.0f - cosPhi),
                dir.y * cosPhi + cross.y * sinPhi + axis.y * dot * (1.0f - cosPhi),
                dir.z * cosPhi + cross.z * sinPhi + axis.z * dot * (1.0f - cosPhi)
            };
            const float speed = ((float)emitter.emitter.ejectionVelocity +
                (random01() * 2.0f - 1.0f) * emitter.emitter.velocityVariance) / 100.0f;
            p.pos = emitter.pos;
            p.pos.x += dir.x * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.pos.y += dir.y * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.pos.z += dir.z * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.vel = {dir.x * speed, dir.y * speed, dir.z * speed};
            if (emitter.projectileTrail) {
                p.vel.x += emitter.ownerVelocity.x * emitter.particle.inheritedVelFactor;
                p.vel.y += emitter.ownerVelocity.y * emitter.particle.inheritedVelFactor;
                p.vel.z += emitter.ownerVelocity.z * emitter.particle.inheritedVelFactor;
            }
            p.orientDir = p.vel;
            const float lifeMS = (float)emitter.particle.lifetimeMS +
                (random01() * 2.0f - 1.0f) * emitter.particle.lifetimeVarianceMS;
            p.lifetime = std::max(0.001f, lifeMS / 1000.0f);
            p.size = emitter.particle.keys.empty() ? 1.0f : emitter.particle.keys.front().size * 50.0f;
            p.texture = emitter.texture;
            p.textureIndex = 0;
            if (!emitter.textures.empty()) p.texture = emitter.textures.front();
            p.additive = !emitter.particle.useInvAlpha;
            if (!emitter.particle.keys.empty()) {
                const auto& k = emitter.particle.keys.front();
                p.color = {k.red, k.green, k.blue, k.alpha};
            }
            p.acc = {p.vel.x * emitter.particle.constantAcceleration,
                     p.vel.y * emitter.particle.constantAcceleration,
                     p.vel.z * emitter.particle.constantAcceleration};
            const float spinRandom = emitter.particle.spinRandomMin +
                ((float)std::rand() / RAND_MAX) *
                    (emitter.particle.spinRandomMax - emitter.particle.spinRandomMin);
            p.spinSpeed = emitter.particle.spinSpeed + spinRandom;
            if (!emitter.emitter.overrideAdvances && ageOffset > 0.0f) {
                // Match ParticleSystem.ts: particles emitted part-way through
                // a frame receive the remaining frame advance immediately.
                p.initialAdvance = std::max(0.0f, dt - ageOffset);
                if (p.initialAdvance >= p.lifetime) p.active = false;
            }
            if (emitter.particles.size() < 4096)
                emitter.particles.push_back(p);
        };
        if (emitter.burst && emitter.nextEmission < 0.0f) {
            for (int i = 0; i < emitter.burstCount; ++i) emitOne(0.0f);
            emitter.nextEmission = 0.0f;
        } else if (!emitter.burst) {
            // A long frame can cross more than one ejection period. Torque
            // emits each due particle rather than dropping overdue emissions.
            while (emitter.age >= emitter.nextEmission &&
                   (emitter.lifetime <= 0.0f || emitter.nextEmission <= emitter.lifetime)) {
                const float ageOffset = std::max(0.0f, emitter.age - emitter.nextEmission);
                emitOne(ageOffset);
                const float period = std::max(0.001f,
                    ((float)emitter.emitter.ejectionPeriodMS +
                     (random01() * 2.0f - 1.0f) * emitter.emitter.periodVariance) / 1000.0f);
                emitter.nextEmission += period;
                if (emitter.nextEmission > emitter.age && previousAge < 0.0f) break;
                if (emitter.particles.size() >= 4096) break;
            }
        }
        for (auto& p : emitter.particles) {
            if (!p.active) continue;
            const float particleDt = p.initialAdvance >= 0.0f
                ? std::exchange(p.initialAdvance, -1.0f) : dt;
            p.age += particleDt;
            if (p.age >= p.lifetime) { p.active = false; continue; }
            p.vel.x += p.acc.x * particleDt; p.vel.y += p.acc.y * particleDt; p.vel.z += p.acc.z * particleDt;
            const float drag = std::max(0.0f, 1.0f - emitter.particle.dragCoefficient * particleDt);
            p.vel.x *= drag; p.vel.y *= drag; p.vel.z *= drag;
            const Point3F wind = windAcceleration(emitter.particle.windCoefficient);
            p.vel.x += wind.x * particleDt;
            p.vel.y += wind.y * particleDt;
            p.vel.z += wind.z * particleDt;
            p.vel.y -= emitter.particle.gravityCoefficient * 9.81f * particleDt;
            p.pos.x += p.vel.x * particleDt; p.pos.y += p.vel.y * particleDt; p.pos.z += p.vel.z * particleDt;
            if (emitter.emitter.orientOnVelocity) p.orientDir = p.vel;
            p.spin += p.spinSpeed * particleDt;
            const float t = p.age / p.lifetime;
            if (!emitter.textures.empty()) {
                p.textureIndex = textureFrameIndex(emitter.textureDurations,
                                                    emitter.textures.size(), p.age);
                p.texture = emitter.textures[p.textureIndex];
            }
            if (emitter.particle.keys.size() >= 2) {
                size_t next = 1;
                while (next < emitter.particle.keys.size() && emitter.particle.keys[next].time < t) ++next;
                const auto& b = emitter.particle.keys[std::min(next, emitter.particle.keys.size() - 1)];
                const auto& a = emitter.particle.keys[next < emitter.particle.keys.size() ? next - 1 : next];
                const float span = b.time - a.time;
                const float f = span > 0.0f ? (t - a.time) / span : 0.0f;
                const auto& colorA = emitter.emitter.useEmitterColors &&
                    emitter.emitter.colors.size() > (next < emitter.emitter.colors.size() ? next - 1 : next)
                    ? emitter.emitter.colors[next < emitter.emitter.colors.size() ? next - 1 : next] : a;
                const auto& colorB = emitter.emitter.useEmitterColors &&
                    emitter.emitter.colors.size() > std::min(next, emitter.emitter.colors.size() - 1)
                    ? emitter.emitter.colors[std::min(next, emitter.emitter.colors.size() - 1)] : b;
                p.color = {colorA.red + (colorB.red - colorA.red) * f,
                           colorA.green + (colorB.green - colorA.green) * f,
                           colorA.blue + (colorB.blue - colorA.blue) * f,
                           colorA.alpha + (colorB.alpha - colorA.alpha) * f};
                const float sizeA = emitter.emitter.useEmitterSizes &&
                    emitter.emitter.sizes.size() > (next < emitter.emitter.sizes.size() ? next - 1 : next)
                    ? emitter.emitter.sizes[next < emitter.emitter.sizes.size() ? next - 1 : next] : a.size * 50.0f;
                const float sizeB = emitter.emitter.useEmitterSizes &&
                    emitter.emitter.sizes.size() > std::min(next, emitter.emitter.sizes.size() - 1)
                    ? emitter.emitter.sizes[std::min(next, emitter.emitter.sizes.size() - 1)] : b.size * 50.0f;
                p.size = sizeA + (sizeB - sizeA) * f;
            }
        }
        emitter.particles.erase(std::remove_if(emitter.particles.begin(), emitter.particles.end(),
            [](const EffectParticle& p) { return !p.active; }), emitter.particles.end());
    }
    effectEmitters.erase(std::remove_if(effectEmitters.begin(), effectEmitters.end(),
        [](const EffectEmitter& e) {
            return (e.burst && e.age >= 0.0f && e.particles.empty()) ||
                   (!e.burst && e.lifetime > 0.0f && e.age > e.lifetime);
        }),
        effectEmitters.end());
    for (auto& shockwave : effectShockwaves) {
        const float previousAge = shockwave.age;
        shockwave.age += dt;
        if (shockwave.age < 0.0f) continue;
        // Only the portion after the delay may advance the wave.
        const float activeDt = previousAge < 0.0f
            ? std::min(dt, shockwave.age) : dt;
        shockwave.velocity += shockwave.data.acceleration * activeDt;
        shockwave.radius += shockwave.velocity * activeDt;
    }
    effectShockwaves.erase(std::remove_if(effectShockwaves.begin(), effectShockwaves.end(),
        [](const EffectShockwave& shockwave) {
            const float lifetime = shockwave.lifetime > 0.0f ? shockwave.lifetime :
                std::max(0.001f, shockwave.data.lifetimeMS / 1000.0f);
            return shockwave.age >= lifetime;
        }), effectShockwaves.end());
    for (auto& light : effectLights) light.age += dt;
    effectLights.erase(std::remove_if(effectLights.begin(), effectLights.end(),
        [](const EffectLight& light) { return light.age >= light.delay + light.lifetime; }),
        effectLights.end());
    for (auto& shake : effectCameraShakes) shake.age += dt;
    effectCameraShakes.erase(std::remove_if(effectCameraShakes.begin(), effectCameraShakes.end(),
        [](const EffectCameraShake& shake) { return shake.age >= shake.delay + shake.duration; }),
        effectCameraShakes.end());
    for (auto& lightning : effectLightnings) {
        if (lightning.strikesPerMinute <= 0.0f) continue;
        lightning.age += dt;
        if (lightning.life > 0.0f) lightning.life -= dt;
        if (lightning.age < lightning.nextStrike) continue;
        lightning.age = 0.0f;
        lightning.nextStrike = 60.0f / lightning.strikesPerMinute;
        auto random01 = [&]() {
            lightning.randomSeed = lightning.randomSeed * 1664525u + 1013904223u;
            return (lightning.randomSeed >> 8) * (1.0f / 16777216.0f);
        };
        const Point3F localStart{
            (random01() - 0.5f) * lightning.scale.x,
            lightning.scale.y * 0.5f,
            (random01() - 0.5f) * lightning.scale.z};
        const Point3F localEnd{
            localStart.x + (random01() * 2.0f - 1.0f) * lightning.strikeRadius,
            -lightning.scale.y * 0.5f,
            localStart.z + (random01() * 2.0f - 1.0f) * lightning.strikeRadius};
        const Point3F torqueStart = Math::torquePointToYUp(localStart);
        const Point3F torqueEnd = Math::torquePointToYUp(localEnd);
        lightning.start = {lightning.pos.x + lightning.rotation.transformNormal(torqueStart).x,
                           lightning.pos.y + lightning.rotation.transformNormal(torqueStart).y,
                           lightning.pos.z + lightning.rotation.transformNormal(torqueStart).z};
        lightning.end = {lightning.pos.x + lightning.rotation.transformNormal(torqueEnd).x,
                         lightning.pos.y + lightning.rotation.transformNormal(torqueEnd).y,
                         lightning.pos.z + lightning.rotation.transformNormal(torqueEnd).z};
        lightning.life = 0.12f;
    }
    for (auto& decal : effectDecals) decal.age += dt;
    effectDecals.erase(std::remove_if(effectDecals.begin(), effectDecals.end(),
        [](const EffectDecal& decal) { return decal.age >= decal.lifetime; }), effectDecals.end());
}

Point3F World::cameraShakeOffset(const Point3F& cameraPosition) const {
    Point3F result{};
    for (const auto& shake : effectCameraShakes) {
        const float elapsed = shake.age - shake.delay;
        if (elapsed < 0.0f || shake.duration <= 0.0f) continue;
        const float dx = cameraPosition.x - shake.pos.x;
        const float dy = cameraPosition.y - shake.pos.y;
        const float dz = cameraPosition.z - shake.pos.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (shake.radius > 0.0f && distance >= shake.radius) continue;
        const float proximity = shake.radius > 0.0f
            ? std::pow(std::clamp(1.0f - distance / shake.radius, 0.0f, 1.0f), shake.falloff)
            : 1.0f;
        const float fade = std::clamp(1.0f - elapsed / shake.duration, 0.0f, 1.0f) * proximity;
        result.x += std::sin(elapsed * shake.frequency[0] * 6.2831853f) * shake.amplitude[0] * fade;
        result.y += std::sin(elapsed * shake.frequency[1] * 6.2831853f + 1.7f) * shake.amplitude[1] * fade;
        result.z += std::sin(elapsed * shake.frequency[2] * 6.2831853f + 3.1f) * shake.amplitude[2] * fade;
    }
    return result;
}

bool World::isPositionVisible(const Point3F& torquePosition, const Point3F& cameraPosition) const {
    const Point3F worldPosition{torquePosition.x, torquePosition.z, -torquePosition.y};
    const auto& renderer = Engine::instance().renderer();
    if (currentSceneState.interiorVisibleZones.size() != worldObjects.size()) {
        currentSceneState.interiorVisibleZones.resize(worldObjects.size());
        currentSceneState.interiorVisibilityComputed.assign(worldObjects.size(), 0);
    }
    for (size_t managerIndex = 0; managerIndex < worldObjects.size(); managerIndex++) {
        const auto& manager = worldObjects[managerIndex];
        if (!manager.shape || !manager.shape->loaded || !manager.shape->isInterior ||
            manager.shape->interiorBSP.empty()) continue;
        MatrixF model;
        if (manager.rotAngleDeg != 0 && (manager.rot.x != 0 || manager.rot.y != 0 || manager.rot.z != 0)) {
            Point3F axis = manager.rot;
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (length > 0.0001f) {
                axis.x /= length; axis.y /= length; axis.z /= length;
                model = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(manager.rotAngleDeg));
            }
        }
        model.setTranslation({manager.pos.x, manager.pos.z, -manager.pos.y});
        if (manager.scale.x != 1.0f || manager.scale.y != 1.0f || manager.scale.z != 1.0f)
            model = model * Math::torqueScaleToYUp(manager.scale);
        model = model * manager.shape->upOrientation();
        const MatrixF inverse = model.inverse();
        const int objectZone = manager.shape->interiorZoneForPoint(inverse.transform(worldPosition));
        if (objectZone < 0) continue;
        if (!currentSceneState.interiorVisibilityComputed[managerIndex]) {
            const Point3F localCamera = inverse.transform(cameraPosition);
            const int cameraZone = manager.shape->interiorZoneForPoint(localCamera);
            if (cameraZone >= 0)
                manager.shape->interiorVisibleZones(cameraZone, localCamera,
                    renderer.projection * renderer.view * model,
                    currentSceneState.interiorVisibleZones[managerIndex]);
            currentSceneState.interiorVisibilityComputed[managerIndex] = 1;
        }
        const auto& visible = currentSceneState.interiorVisibleZones[managerIndex];
        return visible.empty() || (objectZone < (int)visible.size() && visible[objectZone]);
    }
    return true;
}

void World::renderParticles() {
    auto& r = Engine::instance().renderer();
    auto* debrisShader = ShaderManager::getDefaultShader();
    if (debrisShader) debrisShader->bind();
    for (const auto& debris : effectDebris) {
        const float alpha = std::clamp(1.0f - debris.age / debris.lifetime, 0.0f, 1.0f);
        if (debris.shapeIndex >= 0 && debris.shapeIndex < (int)debrisShapes.size() &&
            debrisShapes[debris.shapeIndex].loaded) {
            MatrixF model;
            model.setRotationAxis(debris.rotationAxis, debris.rotation);
            model.setTranslation(debris.pos);
            r.setModel(model * debrisShapes[debris.shapeIndex].upOrientation());
            if (debrisShader) {
                debrisShader->setUniform("uUseTexture", (int32_t)1);
                debrisShader->setUniform("uUseLightmap", (int32_t)0);
            }
            debrisShapes[debris.shapeIndex].render(0);
        } else {
            r.drawSprite(debris.pos, debris.radius * 2.0f,
                         {0.8f, 0.55f, 0.25f, alpha});
        }
    }
    std::vector<size_t> decalOrder(effectDecals.size());
    std::iota(decalOrder.begin(), decalOrder.end(), 0);
    std::stable_sort(decalOrder.begin(), decalOrder.end(), [&](size_t a, size_t b) {
        return effectDecals[a].data.renderPriority > effectDecals[b].data.renderPriority;
    });
    for (size_t index : decalOrder) {
        const auto& decal = effectDecals[index];
        const float alpha = decalAlpha(decal.age, decal.lifetime, decal.data.fadeTimeMS);
        size_t frame = decalTextureFrame(decal.textureDurations, decal.textures.size(), decal.age,
                                          decal.lifetime, decal.data.randomize, decal.frameSeed);
        uint32_t texture = decal.textures.empty() ? 0 : decal.textures[frame];
        const uint32_t rows = std::max(1u, decal.data.textureRows), cols = std::max(1u, decal.data.textureCols);
        const uint32_t atlasFrame = decal.data.randomize ? decal.frameSeed % (rows * cols) :
            std::min<uint32_t>((uint32_t)(decal.age / std::max(0.001f, decal.lifetime) * rows * cols), rows * cols - 1);
        const float u0 = (atlasFrame % cols) / (float)cols, u1 = (atlasFrame % cols + 1) / (float)cols;
        const float v0 = (atlasFrame / cols) / (float)rows, v1 = (atlasFrame / cols + 1) / (float)rows;
        r.drawOrientedSpriteRect(decal.pos, decal.sizeX, decal.sizeY, {1, 1, 1, alpha}, decal.normal,
                                 decal.angle, texture, u0, v0, u1, v1, false);
    }
    for (auto& p : particles) {
        if (!p.active) continue;
        r.drawSprite(p.pos, p.size, p.color);
    }
    for (const auto& emitter : effectEmitters) {
        for (const auto& p : emitter.particles) {
            if (p.active) {
                if (emitter.emitter.orientParticles)
                    r.drawOrientedSprite(p.pos, p.size, p.color, p.orientDir, p.spin, p.texture, p.additive);
                else
                    r.drawSprite(p.pos, p.size, p.color, p.texture, p.additive);
            }
        }
    }
    for (const auto& shockwave : effectShockwaves) {
        if (shockwave.age < 0.0f) continue;
        const int segments = std::max(4, std::min(shockwave.data.numSegments, 128));
        const float life = shockwave.lifetime > 0.0f ? shockwave.lifetime :
            std::max(0.001f, shockwave.data.lifetimeMS / 1000.0f);
        ColorF color = interpolateShockwaveColor(shockwave.data,
            std::clamp(shockwave.age / life, 0.0f, 1.0f));
        color.a *= std::clamp(1.0f - shockwave.age / life, 0.0f, 1.0f);
        Point3F shockwaveCenter = shockwave.pos;
        Point3F shockwaveNormal{0, 1, 0};
        if (shockwave.data.mapToTerrain) {
            const float terrainHeight = getHeight(shockwaveCenter.x, shockwaveCenter.z);
            if (terrainHeight <= -1e8f) continue;
            shockwaveCenter.y = terrainHeight;
        }
        if (shockwave.data.orientToNormal) {
            const float eps = 0.5f;
            const float hx = getHeight(shockwaveCenter.x + eps, shockwaveCenter.z) -
                             getHeight(shockwaveCenter.x - eps, shockwaveCenter.z);
            const float hz = getHeight(shockwaveCenter.x, shockwaveCenter.z + eps) -
                             getHeight(shockwaveCenter.x, shockwaveCenter.z - eps);
            shockwaveNormal = {hx * -0.5f, 1.0f, hz * -0.5f};
        }
        const uint32_t texture = shockwave.data.mapToTerrain && shockwave.mapTexture != UINT32_MAX
            ? shockwave.mapTexture : shockwave.texture;
        r.drawShockwaveRing(shockwaveCenter, shockwave.radius, shockwave.data.width,
                            shockwave.data.height, segments, texture, color,
                             shockwave.data.texWrap, true, shockwave.data.renderBottom,
                             shockwaveNormal);
        if (shockwave.drawMapTexture && shockwave.mapTexture != UINT32_MAX) {
            ColorF foamColor = color;
            foamColor.a *= 0.35f;
            r.drawShockwaveRing(shockwaveCenter, shockwave.radius,
                                shockwave.data.width * 1.02f, shockwave.data.height,
                                segments, shockwave.mapTexture, foamColor,
                                shockwave.data.texWrap, false,
                                shockwave.data.renderBottom, shockwaveNormal);
        }
    }
    for (const auto& lightning : effectLightnings) {
        if (!lightningEnabled || !lightning.enabled || lightning.life <= 0.0f) continue;
        const float fade = std::clamp(lightning.life / 0.12f, 0.0f, 1.0f);
        const ColorF color{
            lightning.fadeColor.r + (lightning.color.r - lightning.fadeColor.r) * fade,
            lightning.fadeColor.g + (lightning.color.g - lightning.fadeColor.g) * fade,
            lightning.fadeColor.b + (lightning.color.b - lightning.fadeColor.b) * fade,
            lightning.fadeColor.a + (lightning.color.a - lightning.fadeColor.a) * fade};
        r.drawLine(lightning.start, lightning.end, color);
    }
}

// ─── Precipitation System ─────────────────────────────────────

void World::initPrecipitation(const PrecipitationState& state) {
    precipitation.randomSeed = state.randomSeed;
    precipitation.textureAge = 0.0f;
    precipitation.drops.resize(state.numDrops);
    float halfW = state.boxWidth * 0.5f;
    float halfD = state.boxWidth * 0.5f;
    auto nextRandom = [&]() {
        precipitation.randomSeed = precipitation.randomSeed * 1664525u + 1013904223u;
        return (precipitation.randomSeed >> 8) * (1.0f / 16777216.0f);
    };
    for (auto& d : precipitation.drops) {
        d.pos.x = state.origin.x + nextRandom() * state.boxWidth - halfW;
        d.pos.y = state.origin.y + nextRandom() * state.boxHeight;
        d.pos.z = state.origin.z + nextRandom() * state.boxWidth - halfD;
        float speed = state.minSpeed + nextRandom() * (state.maxSpeed - state.minSpeed);
        const Point3F wind = state.useWind ? getTorchWindVelocity() : Point3F{};
        d.vel = {wind.x, -speed + wind.y, wind.z};
        d.active = true;
    }
}

bool World::setPrecipitation(int type, float percentage) {
    if (type < 0 || type > 7 || !std::isfinite(percentage) || percentage < 0.0f || percentage > 1.0f)
        return false;
    precipitation.type = type;
    precipitation.percentage = percentage;
    if (percentage == 0.0f) {
        precipitation.active = false;
        precipitation.drops.clear();
        return true;
    }
    precipitation.active = true;
    precipitation.numDrops = std::max(1, (int)std::lround(precipitation.configuredDrops * percentage));
    initPrecipitation(precipitation);
    return true;
}

bool World::setPrecipitationEnabled(bool enabled) {
    return setPrecipitation(precipitation.type, enabled ? precipitation.percentage : 0.0f);
}

bool World::setPrecipitationType(int type) {
    return setPrecipitation(type, precipitation.percentage);
}

bool World::setPrecipitationWind(const Point3F& velocity) {
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) return false;
    setTorchWindVelocity(velocity);
    if (precipitation.active && precipitation.useWind)
        for (auto& drop : precipitation.drops) { drop.vel.x = velocity.x; drop.vel.z = velocity.z; }
    return true;
}

bool World::setPrecipitationBox(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) return false;
    precipitation.boxWidth = width;
    precipitation.boxHeight = height;
    if (precipitation.active) initPrecipitation(precipitation);
    return true;
}

bool World::setLightningEnabled(bool enabled) {
    lightningEnabled = enabled;
    if (!enabled) for (auto& lightning : effectLightnings) lightning.life = 0.0f;
    return true;
}

bool World::strikeLightning() {
    if (!lightningEnabled || effectLightnings.empty()) return false;
    bool struck = false;
    for (auto& lightning : effectLightnings) {
        if (!lightning.enabled) continue;
        lightning.age = lightning.nextStrike;
        lightning.nextStrike = 0.0f;
        struck = true;
    }
    return struck;
}

void World::updatePrecipitation(float dt, const Point3F& camPos) {
    if (!precipitation.active || precipitation.drops.empty()) return;
    precipitation.textureAge += std::max(0.0f, dt);

    float halfW = precipitation.boxWidth * 0.5f;
    float halfD = precipitation.boxWidth * 0.5f;
    float boxY = precipitation.origin.y + precipitation.boxHeight;

    for (auto& d : precipitation.drops) {
        if (!d.active) continue;
        d.pos.y += d.vel.y * dt;
        // Reset drop when it falls below the box
        if (d.pos.y < precipitation.origin.y - 5.0f) {
            d.pos.y = boxY;
            precipitation.randomSeed = precipitation.randomSeed * 1664525u + 1013904223u;
            float random = (precipitation.randomSeed >> 8) * (1.0f / 16777216.0f);
            float speed = precipitation.minSpeed + random * (precipitation.maxSpeed - precipitation.minSpeed);
            const Point3F wind = precipitation.useWind ? getTorchWindVelocity() : Point3F{};
            d.vel = {wind.x, -speed + wind.y, wind.z};
        }
    }

    // If following camera, re-center drops around camera
    if (precipitation.followCam) {
        for (auto& d : precipitation.drops) {
            if (!d.active) continue;
            float dx = d.pos.x - camPos.x;
            float dz = d.pos.z - camPos.z;
            if (dx > halfW) d.pos.x -= precipitation.boxWidth;
            else if (dx < -halfW) d.pos.x += precipitation.boxWidth;
            if (dz > halfD) d.pos.z -= precipitation.boxWidth;
            else if (dz < -halfD) d.pos.z += precipitation.boxWidth;
        }
    }
}

void World::renderPrecipitation() {
    if (!precipitation.active || precipitation.drops.empty()) return;
    auto& r = Engine::instance().renderer();
    ColorF dropColor = precipitation.color;
    uint32_t texture = 0;
    if (!precipitation.textures.empty()) {
        const size_t frame = textureFrameIndex(precipitation.textureDurations,
                                               precipitation.textures.size(),
                                               precipitation.textureAge);
        texture = precipitation.textures[frame];
    }
    for (auto& d : precipitation.drops) {
        if (!d.active) continue;
        r.drawSprite(d.pos, precipitation.dropSize, dropColor, texture);
    }
}

void World::renderWater() {
    if (waterBodies.empty()) return;

    auto& r = Engine::instance().renderer();
    float time = Engine::instance().game().gameTime();
    Point3F cam = r.cameraPos;

    auto* waterShdr = ShaderManager::getWaterShader();
    if (!waterShdr) return;
    waterShdr->bind();
    waterShdr->setUniform("uProjection", r.projection);
    waterShdr->setUniform("uView", r.view);
    waterShdr->setUniform("uCamPos", cam);
    for (int i = 0; i < 3; ++i) {
        ColorF packed{};
        if (i < (int)fogVolumes.size() && fogVolumes[i].visibleDistance > 0.0f) {
            const auto& volume = fogVolumes[i];
            packed = {1.0f / volume.visibleDistance, volume.minHeight,
                      volume.maxHeight, 0.0f};
        }
        waterShdr->setUniform((std::string("uFogVolume") + std::to_string(i)).c_str(), packed);
    }
    waterShdr->setUniform("uUseSurfaceTexture", (int32_t)0);
    waterShdr->setUniform("uUseShoreTexture", (int32_t)0);
    waterShdr->setUniform("uUseEnvMap", (int32_t)0);

    // Sun lighting
    Point3F sunDir = sunLightDirUsed ? sunLightDir : Point3F{0.5f, 0.8f, 0.6f};
    waterShdr->setUniform("uSunDir", sunDir);
    waterShdr->setUniform("uSunColor", Point3F{sunColor.r, sunColor.g, sunColor.b});

    // Fog
    const bool renderFog = fog.enabled && !Engine::instance().game().isMapperMode();
    waterShdr->setUniform("uFogEnabled", (int32_t)(renderFog ? 1 : 0));
    if (renderFog) {
        waterShdr->setUniform("uFogColor", Point3F{fog.color.r, fog.color.g, fog.color.b});
        waterShdr->setUniform("uFogDensity", fog.density);
        waterShdr->setUniform("uFogStart", fog.distance);
        waterShdr->setUniform("uFogEnd", Engine::instance().renderer().config().farPlane);
    }

    GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
    GLboolean depthTestWasOn = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLboolean depthWriteWasOn = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasOn);
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    for (const auto& body : waterBodies) {
        if (cam.y > body.level + 80.0f) continue;
        waterShdr->setUniform("uUseSurfaceTexture", (int32_t)0);
        waterShdr->setUniform("uUseShoreTexture", (int32_t)0);
        waterShdr->setUniform("uUseEnvMap", (int32_t)0);
        ColorF waterCol = body.surfaceColor;
        waterShdr->setUniform("uWaterColor", Point3F{waterCol.r, waterCol.g, waterCol.b});
        waterShdr->setUniform("uWaterOpacity", std::clamp(body.opacity, 0.0f, 1.0f) *
                                               std::clamp(waterCol.a, 0.0f, 1.0f));
        uint32_t surfaceTexture = 0;
        if (!body.surfaceFrames.empty()) {
            surfaceTexture = body.surfaceFrames.front();
            if (body.surfaceFrames.size() > 1) {
                float cycle = 0.0f;
                for (float duration : body.surfaceFrameDurations) cycle += std::max(duration, 0.001f);
                float cursor = cycle > 0.0f ? std::fmod(time, cycle) : 0.0f;
                for (size_t frame = 0; frame < body.surfaceFrames.size(); ++frame) {
                    const float duration = frame < body.surfaceFrameDurations.size()
                        ? std::max(body.surfaceFrameDurations[frame], 0.001f) : 1.0f;
                    if (cursor < duration) { surfaceTexture = body.surfaceFrames[frame]; break; }
                    cursor -= duration;
                }
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, surfaceTexture);
            waterShdr->setUniform("uSurfaceTexture", (int32_t)0);
            waterShdr->setUniform("uUseSurfaceTexture", (int32_t)1);
        }
        if (!body.envFrames.empty()) {
            size_t envFrame = 0;
            if (body.envFrames.size() > 1) {
                float cycle = 0.0f;
                for (float duration : body.envFrameDurations) cycle += std::max(duration, 0.001f);
                float cursor = cycle > 0.0f ? std::fmod(time, cycle) : 0.0f;
                for (size_t frame = 0; frame < body.envFrames.size(); ++frame) {
                    const float duration = frame < body.envFrameDurations.size()
                        ? std::max(body.envFrameDurations[frame], 0.001f) : 1.0f;
                    if (cursor < duration) { envFrame = frame; break; }
                    cursor -= duration;
                }
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, body.envFrames[envFrame]);
            waterShdr->setUniform("uEnvMap", (int32_t)1);
            waterShdr->setUniform("uEnvIntensity", body.envIntensity);
            waterShdr->setUniform("uUseEnvMap", (int32_t)1);
        }
        waterShdr->setUniform("uTexOffset", std::fmod(time * body.waveSpeed * 0.01f, 1.0f));
        const int gridRes = 24;
        const float stepX = body.sizeX / gridRes;
        const float stepZ = body.sizeY / gridRes;
        for (int z = 0; z < gridRes; z++) {
            for (int x = 0; x < gridRes; x++) {
            float wx = body.originX + x * stepX;
            float wz = body.originZ + z * stepZ;

            // Skip quads far from camera
            float dx = wx + stepX * 0.5f - cam.x;
            float dz = wz + stepZ * 0.5f - cam.z;
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist > 400.0f) continue;

            // Wave animation
            float wy = body.level;
            wy += (sinf(wx * 0.05f + time) + sinf(wz * 0.05f + time)) *
                  body.waveMagnitude * 0.25f;

            // Build model matrix for this quad
            MatrixF model;
            model.identity();
            model.setTranslation({wx, wy, wz});
            MatrixF scale;
            scale.setScale({stepX, 1.0f, stepZ});
            model = model * scale;
            float shoreFactor = 1.0f;
            if (body.shoreDepth > 0.0f && terrainBlock.loaded && !body.shoreFrames.empty()) {
                const float terrainHeight = terrainBlock.sampleHeight(wx + stepX * 0.5f, wz + stepZ * 0.5f);
                shoreFactor = std::clamp((body.level - terrainHeight) / body.shoreDepth, 0.0f, 1.0f);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, body.shoreFrames.front());
                waterShdr->setUniform("uShoreTexture", (int32_t)2);
                waterShdr->setUniform("uUseShoreTexture", (int32_t)1);
            }
            waterShdr->setUniform("uShoreFactor", shoreFactor);
            waterShdr->setUniform("uModel", model);

            r.drawFilledQuad(stepX, stepZ);
            }
        }
    }

    if (cullWasOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (depthTestWasOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWasOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(depthWriteWasOn);
    glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                        (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
}

Game::Game() : pl(new Player), w(new World) {
    mMenu = new Menu;
    hud = new HUD;
}
Game::~Game() { clearProjectileAudio(); delete pl; delete w; delete hud; }

void Game::clearProjectileAudio() {
    auto& audio = Engine::instance().audio();
    for (auto& [index, source] : projectileSoundSources) audio.releaseSource(source);
    projectileSoundSources.clear();
}

bool Game::init() {
    Console::instance().printf(LogLevel::Info, "Game initialized");

    // Register game console commands
    auto& con = Console::instance();
    con.addCommand("startLocal", [this](int32_t argc, const char* const* argv) {
        startLocalGame();
    });
    con.addCommand("selectWeaponSlot", [this](int32_t argc, const char* const* argv) {
        if (argc > 1) player().selectWeapon(atoi(argv[1]));
    }, "selectWeaponSlot <index> - script-owned weapon selection bridge");
    con.addCommand("cycleWeapon", [this](int32_t argc, const char* const* argv) {
        if (argc > 1 && strcmp(argv[1], "prev") == 0) player().weaponCycle(-1);
        else player().weaponCycle(1);
    }, "cycleWeapon <next|prev> - script-owned weapon cycling bridge");
    con.addCommand("setFov", [this](int32_t argc, const char* const* argv) {
        if (argc > 1) {
            const float fov = (float)atof(argv[1]);
            Engine::instance().renderer().config().fov = fov > 1.0f && fov < 180.0f ? fov : 90.0f;
        }
    }, "setFov <degrees> - script-owned camera FOV bridge");

    con.addCommand("connect", [this](int32_t argc, const char* const* argv) {
        if (argc < 2 || !argv[1][0]) {
            Console::instance().printf(LogLevel::Warn, "Usage: connect <host> [port]");
            return;
        }
        uint16_t port = T2Protocol::DEFAULT_PORT;
        if (argc > 2 && !parseConsolePort(argv[2], port)) {
            Console::instance().printf(LogLevel::Warn, "Invalid port: %s", argv[2]);
            return;
        }
        connectToServer(argv[1], port);
    }, "connect <host> [port] - connect to a server");
    con.addCommand("watchServer", [this](int32_t argc, const char* const* argv) {
        std::string host;
        uint16_t port = 0;
        if (argc < 2 || !parseConsoleHostPort(argv[1], host, port)) {
            Console::instance().printf(LogLevel::Warn, "Usage: watchServer <host:port>");
            return;
        }
        connectToServer(host.c_str(), port, true);
    }, "watchServer <host:port> - connect as an anonymous observer");

    con.addCommand("startServer", [this](int32_t argc, const char* const* argv) {
        uint16_t port = T2Protocol::DEFAULT_PORT;
        if (argc > 1 && !parseConsolePort(argv[1], port)) {
            Console::instance().printf(LogLevel::Warn, "Invalid port: %s", argv[1]);
            return;
        }
        if (argc > 2) {
            const std::string mission = missionLoadPath(argv[2]);
            if (mission.empty()) {
                Console::instance().printf(LogLevel::Warn, "Invalid mission: %s", argv[2]);
                return;
            }
            Console::instance().setVariable("sv_mission", mission.c_str());
        }
        // Wire terrain height callback for server-side collision
        server.setHeightCallback(+[](float x, float z, void* ctx) -> float {
            return static_cast<World*>(ctx)->getHeight(x, z);
        }, &w);
        server.setRayCallback(+[](float ox, float oy, float oz, float dx, float dy, float dz,
                                  float distance, void* ctx) -> bool {
            float hitDistance = 0.0f;
            Point3F hitPoint{}, hitNormal{};
            return static_cast<World*>(ctx)->collision().raycast(
                {ox, oy, oz}, {dx, dy, dz}, distance, hitDistance, hitPoint, hitNormal);
        }, &w);
        server.start(port);
    }, "startServer [port] [mission] - Start a game server on the given port");

    con.addCommand("loadMission", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) {
            Console::instance().printf(LogLevel::Warn, "Usage: loadMission <name>");
            return;
        }
        const std::string mission = missionLoadPath(argv[1]);
        if (mission.empty()) {
            Console::instance().printf(LogLevel::Warn, "Invalid mission: %s", argv[1]);
            return;
        }
        startLocalGame(mission.c_str());
    }, "loadMission <name> - Load and start a mission");

    con.addCommand("startMission", [this](int32_t, const char* const*) {
        if (gameState == Loading || gameState == Playing) return;
        startLocalGame();
    }, "startMission - Start the selected mission");

    con.addCommand("playdemo", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: playdemo <path>"); return; }
        playDemo(argv[1]);
    }, "playdemo <path> - Load and play a Tribes 2 demo file");
    con.addCommand("seekDemoBlock", [this](int32_t argc, const char* const* argv) {
        if (!demoParser || argc < 2) return;
        const int target = std::max(0, atoi(argv[1]));
        auto snapshot = demoSnapshots.upper_bound(target);
        if (snapshot != demoSnapshots.begin()) {
            --snapshot;
            if (!demoParser->restoreSnapshot(snapshot->second)) {
                Console::instance().printf(LogLevel::Warn, "seekDemoBlock: snapshot restore failed");
                return;
            }
        } else {
            demoParser->reset();
        }
        if (!demoParser->seekToBlock(target)) {
            Console::instance().printf(LogLevel::Warn,
                "seekDemoBlock: unable to seek to block %d", target);
            return;
        }
        demoBlocksDone = target;
        demoTime = demoBlocksTotal > 0 && demoTotalTime > 0
            ? demoTotalTime * (float)target / (float)demoBlocksTotal : 0;
        // Parser snapshots restore authoritative demo state, but these are
        // presentation values accumulated by the game loop after the snapshot.
        // Drop them so a seek cannot leak camera effects or events from the
        // discarded timeline into the new one.
        Engine::instance().audio().stopAll();
        clearProjectileAudio();
        demoEventLog.clear();
        damageFlash = -1.0f;
        whiteOut = -1.0f;
        shakeIntensity = 0.0f;
        shakeOffset = {0, 0, 0};
        if (w) w->clearEffects();
        if (demoParser) demoParser->consumeExplosions();
        demoTrails.clear();
          demoHasPos = false;
          demoAuthoredCamera = false;
         demoHasOrientation = false;
          demoInterpolationDt = 0.0f;
         demoMoveBlend = 1.0f;
          demoOrbitCam = false;
          demoFirstPersonCam = demoParser->getInitialBlock().firstPerson;
          controlGhostIndex = demoParser->getInitialBlock().controlObjectGhostIndex;
            demoCameraFov = -1.0f;
         orbitCenterInit = false;
         spectateGhostIndex = -1;
         demoPath.clear();
        demoPathCount = 0;
        Console::instance().printf(LogLevel::Info,
            "Demo seeked to block %d", target);
    }, "seekDemoBlock <index> - seek parser state to a demo block");

    con.addCommand("listdemos", [this](int32_t argc, const char* const* argv) {
        auto& fs = Engine::instance().fs();
        std::vector<std::string> files;
        fs.listFiles("*.demo", files);
        if (files.empty()) {
            Console::instance().printf(LogLevel::Info, "No demo files found (*.demo)");
        } else {
            Console::instance().printf(LogLevel::Info, "Demo files (%zu):", files.size());
            for (auto& f : files)
                Console::instance().printf(LogLevel::Info, "  %s", f.c_str());
        }
    }, "listdemos - List available demo files");

    con.addCommand("testshape", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: testshape <dts_path>"); return; }
        auto& fs = Engine::instance().fs();
        auto data = fs.read(argv[1]);
        if (data.empty()) {
            Console::instance().printf(LogLevel::Warn, "testshape: file not found: %s", argv[1]);
            return;
        }
        testShape = DTSShape{};
        testShape.name = argv[1];
        if (!testShape.load(data.data(), data.size())) {
            Console::instance().printf(LogLevel::Warn, "testshape: failed to load shape");
            testShape = DTSShape{};
            return;
        }
        testShapeLoaded = true;
        Console::instance().printf(LogLevel::Info, "testshape: loaded '%s' (%zu meshes, %zu nodes, %zu anims)",
            argv[1], testShape.meshes.size(), testShape.nodes.size(), testShape.animations.size());
        if (!testShape.animations.empty()) {
            Console::instance().printf(LogLevel::Info, "  animations:");
            for (auto& a : testShape.animations)
                Console::instance().printf(LogLevel::Info, "    %s (%.1fs)", a.name.c_str(), a.duration);
        }
    }, "testshape <path> - Load and display a native DTS shape");

    // ── Shape Viewer ─────────────────────────────────────────────────────
    con.addCommand("shapeviewer", [this](int32_t, const char* const*) {
        enterShapeViewer();
    }, "shapeviewer - Browse all .dts shapes from the data paths");

    con.addCommand("sv_next", [this](int32_t, const char* const*) {
        if (!shapeViewerActive) { Console::instance().printf(LogLevel::Warn, "shapeviewer not active"); return; }
        shapeViewerNext();
    }, "sv_next - Next shape in shape viewer");

    con.addCommand("sv_prev", [this](int32_t, const char* const*) {
        if (!shapeViewerActive) { Console::instance().printf(LogLevel::Warn, "shapeviewer not active"); return; }
        shapeViewerPrev();
    }, "sv_prev - Previous shape in shape viewer");

    con.addCommand("sv_ghosts", [this](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "Total server ghosts: %zu", server.ghostCount());
    }, "sv_ghosts - List server ghost count");

    con.addCommand("sv_spawn", [this](int32_t argc, const char* const* argv) {
        if (argc < 4) { Console::instance().printf(LogLevel::Warn, "Usage: sv_spawn <classId> <x> <y> [z]"); return; }
        int classId = atoi(argv[1]);
        float x = (float)atof(argv[2]);
        float y = (float)atof(argv[3]);
        float z = (argc > 4) ? (float)atof(argv[4]) : 2.0f;
        uint32_t idx = server.spawnGhost(classId, x, y, z);
        if (idx > 0)
            Console::instance().printf(LogLevel::Info, "Spawned ghost idx=%u class=%d at (%.1f, %.1f, %.1f)",
                (unsigned)idx, classId, x, y, z);
    }, "sv_spawn <classId> <x> <y> [z] - Spawn a ghost on the server");

    con.addCommand("sv_removeghost", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_removeghost <index>"); return; }
        uint32_t idx = (uint32_t)atoi(argv[1]);
        if (server.removeGhost(idx))
            Console::instance().printf(LogLevel::Info, "Removed ghost idx=%u", (unsigned)idx);
        else
            Console::instance().printf(LogLevel::Warn, "Ghost idx=%u not found", (unsigned)idx);
    }, "sv_removeghost <index> - Remove a ghost on the server");

    con.addCommand("sv_addbot", [this](int32_t, const char* const*) {
        server.spawnBot();
        Console::instance().printf(LogLevel::Info, "Bot spawned");
    }, "sv_addbot - Spawn an AI bot on the server");

    con.addCommand("kick", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: kick <clientId>"); return; }
        server.kickClient(atoi(argv[1]));
    }, "kick <clientId> - Kick a client by index");

    con.addCommand("ban", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: ban <clientId>"); return; }
        server.banClient(atoi(argv[1]));
    }, "ban <clientId> - Ban a client by IP");

    con.addCommand("unbanall", [this](int32_t, const char* const*) {
        server.clearBans();
    }, "unbanall - Clear the ban list");

    con.addCommand("sv_map", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_map <mission>"); return; }
        server.changeMap(argv[1]);
    }, "sv_map <mission> - Change the current mission");

    con.addCommand("sv_gamemode", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_gamemode <0|1> (0=DM, 1=TDM)"); return; }
        server.setGameMode(atoi(argv[1]));
    }, "sv_gamemode <0|1> - Set game mode (0=Deathmatch, 1=Team Deathmatch)");

    con.addCommand("sv_nat", [this](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "NAT relay: see server console");
    }, "sv_nat - Show NAT relay info");

    con.addCommand("record", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: record <path>"); return; }
        server.startRecording(argv[1]);
    }, "record <path> - Start recording server state to file");

    con.addCommand("stoprecord", [this](int32_t, const char* const*) {
        server.stopRecording();
    }, "stoprecord - Stop recording");

    return true;
}

static void resetGameplayGui(GuiRenderer& gui) {
    for (const char* name : {"weaponsHud", "inventoryHud"}) {
        if (auto* control = gui.findControl(name)) {
            for (auto& slot : control->hudSlots) {
                slot.visible = false;
                slot.active = false;
                slot.bitmap.clear();
                slot.amount = 0;
            }
            control->activeHudSlot = -1;
        }
    }
    if (auto* control = gui.findControl("backpackFrame")) control->visible = false;
    if (auto* control = gui.findControl("backpackText")) {
        control->text.clear();
        control->visible = false;
    }
    if (auto* control = gui.findControl("backpackIcon")) {
        control->bitmap.clear();
        control->visible = false;
    }
    if (auto* control = gui.findControl("dashboardHud")) control->visible = false;
    if (auto* control = gui.findControl("vWeaponsBox")) {
        control->visible = false;
        control->activeHudSlot = -1;
    }
    if (auto* control = gui.findControl("ammoHud")) control->text.clear();
    if (auto* taskList = Engine::instance().script().findObject("TaskList")) {
        taskList->fields["currentTaskClient"] = VMValue("");
        taskList->fields["currentAIObjective"] = VMValue("");
        taskList->fields["currentTaskIsTeam"] = VMValue("0");
        taskList->fields["currentTaskDescription"] = VMValue("");
    }
}

void Game::shutdown() {
    delete mMenu;
    mMenu = nullptr;
    clearMissionAudio();
}

void Game::clearMissionAudio() {
    auto& audio = Engine::instance().audio();
    for (auto& [key, source] : shapeBaseSoundSources)
        if (source) audio.releaseSource(source);
    shapeBaseSoundSources.clear();
    audio.setUnderwater(false);
    audio.clearEnvironmentState();
    for (auto* source : emitterSources) audio.releaseSource(source);
    emitterSources.clear();
    if (ambientSource) audio.releaseSource(ambientSource);
    ambientSource = nullptr;
    ambientSound = nullptr;
}

// ─── AudioProfile scanner ────────────────────────────────────
// Scans game scripts for AudioProfile definitions and builds a
// profile-ID → sound-file-path mapping for demo playback.
struct AudioProfileEntry {
    std::string profileName;
    std::string filename;
};
static std::vector<AudioProfileEntry> s_audioProfiles;
static void scanAudioProfiles() {
    if (!s_audioProfiles.empty()) return;
    auto& fs = Engine::instance().fs();
    std::vector<std::string> scriptFiles;
    fs.listFiles("scripts/*.cs", scriptFiles);
    std::set<std::string> seen;
    for (auto& f : scriptFiles) {
        if (f.size() < 3 || f.substr(f.size() - 3) != ".cs") continue;
        if (f.find(".dso") != std::string::npos) continue;
        if (!seen.insert(f).second) continue;
        auto data = fs.readText(f.c_str());
        if (data.empty()) continue;
        const char* p = data.c_str();
        while (true) {
            const char* db = strstr(p, "datablock AudioProfile(");
            if (!db) break;
            p = db + 23;
            const char* np = strchr(p, ')');
            if (!np) break;
            std::string name(p, np - p);
            const char* ob = strchr(np, '{');
            if (!ob) break;
            const char* cb = strchr(ob, '}');
            if (!cb) break;
            std::string body(ob, cb - ob);
            const char* fk = body.c_str();
            const char* fnq = nullptr;
            while ((fk = strstr(fk, "filename"))) {
                const char* eq = strchr(fk, '=');
                if (!eq) { fk += 8; continue; }
                fnq = strchr(eq, '"');
                if (fnq) break;
                fk += 8;
            }
                    if (fnq) {
                const char* fnq2 = strchr(fnq + 1, '"');
                if (fnq2) {
                    std::string fn(fnq + 1, fnq2 - fnq - 1);
                    if (!fn.empty()) s_audioProfiles.push_back({name,
                        fn.rfind("audio/", 0) == 0 ? fn : "audio/" + fn});
                }
            }
            p = cb + 1;
        }
    }
    Console::instance().printf(LogLevel::Info, "Audio: scanned %zu AudioProfiles from %zu scripts",
        s_audioProfiles.size(), seen.size());
}

static SoundSource* playNativeAudioProfile(AudioSystem& audio,
    const std::map<uint32_t, ParsedDataBlock>& blocks, uint32_t profileId,
    const Point3F& position, bool forceLoop = false) {
    if (!profileId || !audio.config().enabled || audio.config().sfxVolume <= 0.0f)
        return nullptr;
    auto it = blocks.find(profileId);
    if (it == blocks.end() || it->second.decoded.audioFilename.empty()) return nullptr;
    const auto& profile = it->second.decoded;
    std::string path = profile.audioFilename;
    if (path.rfind("audio/", 0) != 0) path = "audio/" + path;
    auto* buffer = audio.loadSound(path.c_str());
    auto* source = buffer ? audio.createSource(false) : nullptr;
    if (!source) return nullptr;
    source->setVolume(profile.audioVolume * audio.config().masterVolume * audio.config().sfxVolume);
    if (profile.audioIs3D) {
        source->setPosition(position);
        source->setDistance(profile.audioMinDistance, profile.audioMaxDistance);
    }
    source->setLooping(forceLoop || profile.audioLooping);
    if (!forceLoop && profile.audioLooping)
        source->setLoopSchedule(profile.audioLoopCount, profile.audioMinLoopGapMs,
                                profile.audioMaxLoopGapMs, profileId);
    source->play(buffer);
    return source;
}

void Game::update(float dt) {
    Engine::instance().audio().advance(dt);
    const float simulationDt = GameTime::scaledDelta(dt, timeScale);
    time += demoPlaying ? std::max(0.0f, dt) : simulationDt;

    if (gameState == Playing) {
        // ─── Demo playback ──────────────────────────────────────
        if (demoPlaying) {
            if (demoPaused && !demoStepRequest) {
                demoJetHeld = false;
                demoInterpolationDt = 0.0f;
                return;
            }
            const bool stepDemo = demoStepRequest;
            const float playbackRate = (demoFastForward || currentInput.jet) ? 4.0f : 1.0f;
            Engine::instance().audio().setPlaybackRate(playbackRate);
            const float blockDuration = T2Demo::playbackBlockDuration(
                demoTotalTime, demoBlocksTotal);
            const float playbackDt = stepDemo ? blockDuration : dt * playbackRate;
            demoInterpolationDt = playbackDt;
            demoTime = std::min(demoTime + playbackDt, demoTotalTime);
            // Decay camera shake
            if (shakeIntensity > 0) {
                shakeIntensity = std::max(0.0f, shakeIntensity - dt * 8.0f);
                const Vec3 unit = T2Demo::cameraShakeOffset(
                    demoTime, {1.0f, 1.0f, 1.0f}, {1.7f, 2.3f, 1.1f},
                    {0.0f, 0.37f, 0.71f});
                shakeOffset = {unit.x * shakeIntensity, unit.y * shakeIntensity,
                               unit.z * shakeIntensity};
                shakeOffset.x *= shakeIntensity;
                shakeOffset.y *= shakeIntensity;
                shakeOffset.z *= shakeIntensity;
            } else {
                shakeOffset = {0,0,0};
            }
            // Decay damage flash and whiteout
            if (damageFlash > 0) damageFlash = std::max(0.0f, damageFlash - dt * 3.0f);
            if (whiteOut > 0) whiteOut = std::max(0.0f, whiteOut - dt * 2.0f);
            int blocksThisFrame;
            if (demoStepRequest) {
                blocksThisFrame = 1;
                demoStepRequest = false;
            } else if (demoFastForward || currentInput.jet) {
                demoJetHeld = currentInput.jet;
                // Use the same playhead-to-block conversion as normal
                // playback. Integer truncation here used to stall 4x playback
                // whenever a frame represented less than one block.
                const int targetDone = T2Demo::playbackTargetBlock(
                    demoTime, demoTotalTime, demoBlocksTotal);
                blocksThisFrame = targetDone - demoBlocksDone;
            } else {
                demoJetHeld = false;
                // Match real-time: catch up to target position
                int targetDone = T2Demo::playbackTargetBlock(
                    demoTime, demoTotalTime, demoBlocksTotal);
                blocksThisFrame = targetDone - demoBlocksDone;
            }
            if (blocksThisFrame < 0) blocksThisFrame = 0;
            if (blocksThisFrame > 500) blocksThisFrame = 500;

            for (int i = 0; i < blocksThisFrame; i++) {
                DemoBlock* block = demoParser->nextBlock();
                if (!block) {
                    Console::instance().printf(LogLevel::Info,
                        "Demo playback complete: %d blocks in %.1f seconds",
                        demoBlocksDone, demoTime);
                    stopDemoPlayback();
                    return;
                }
                demoBlocksDone++;
                const float blockTime = demoBlocksTotal > 0
                    ? demoTotalTime * (float)(demoBlocksDone - 1) / (float)demoBlocksTotal
                    : 0.0f;
                const std::string previousMission = demoParser->currentMission();
                demoParser->setCurrentBlock(demoBlocksDone - 1);
                if (demoParser->currentMission() != previousMission) {
                    // A demo mission change replaces the environment ghost set;
                    // reload the authored atmosphere before applying new ghosts.
                    if (w && !demoParser->currentMission().empty())
                        w->load(demoParser->currentMission().c_str());
                    demoParser->resetMissionState();
                    clearMissionAudio();
                    Engine::instance().audio().stopAll();
                    clearProjectileAudio();
                    demoTrails.clear();
                    if (w) w->clearEffects();
                    targetFinderShown = false;
                    damageFlash = -1.0f;
                    whiteOut = -1.0f;
                    shakeIntensity = 0.0f;
                    auto& missionGui = Engine::instance().guiRenderer();
                    missionGui.clearDialogs();
                    missionGui.setContent("PlayGui");
                    resetGameplayGui(missionGui);
                    if (hud) hud->resetState();
                }

                // Extract position data from move blocks
                if (block->type == T2Demo::BlockTypeMove && block->size >= 64) {
                    DemoMove move = demoParser->readRawMove(block->data.data(), block->data.size());
                    if (std::fabs(move.yaw) > 0.001f || std::fabs(move.pitch) > 0.001f) {
                        demoViewYaw = move.yaw;
                        demoViewPitch = move.pitch;
                        demoHasOrientation = true;
                        if (demoHasPos) {
                            demoPrevCameraTarget = demoCameraTarget;
                            demoCameraTarget = {
                                demoCameraPos.x + std::sin(demoViewYaw) * std::cos(demoViewPitch),
                                demoCameraPos.y + std::cos(demoViewYaw) * std::cos(demoViewPitch),
                                demoCameraPos.z + std::sin(demoViewPitch)
                            };
                            demoMoveBlend = 0.0f;
                        }
                    }
                }

                // Parse packet blocks (GameState, ghost updates, events)
                if (block->type == T2Demo::BlockTypeSendPacket) {
                    demoParser->onSendPacketTrigger();
                } else if (block->type == T2Demo::BlockTypePacket) {
                    PacketData pd = demoParser->parsePacket(block->data.data(), block->data.size(), demoBlocksDone - 1);
                    // Collect chat/server events for the event pane
                    for (size_t eventIndex = 0; eventIndex < pd.events.size(); ++eventIndex) {
                        const auto& ev = pd.events[eventIndex];
                        // Handle audio events
                        if (ev.directAudioProfile && ev.audioProfileId >= 0) {
                            auto& audio = Engine::instance().audio();
                            const uint64_t eventKey = demoAudioEventKey(
                                demoBlocksDone - 1, static_cast<int>(eventIndex), ev);
                            if (!demoAudioEventsPlayed.insert(eventKey).second) continue;
                            if (audio.config().enabled && audio.config().sfxVolume > 0) {
                                scanAudioProfiles();
                                if (ev.audioProfileId < (int)s_audioProfiles.size()) {
                                    const auto& entry = s_audioProfiles[ev.audioProfileId];
                                    auto* buf = audio.loadSound(entry.filename.c_str());
                                    if (buf) {
                                        auto* src = audio.createSource();
                                        if (src) {
                                            src->setVolume(0.3f * audio.config().masterVolume *
                                                audio.config().sfxVolume);
                                            if (ev.hasAudioPosition) {
                                                src->setPosition(Math::torquePointToYUp({
                                                    ev.audioPosition.x, ev.audioPosition.y,
                                                    ev.audioPosition.z}));
                                            }
                                            src->play(buf);
                                        }
                                    }
                                }
                            }
                            continue;
                        }
                        if (ev.message.empty()) continue;
                        std::string displayText = ev.message;
                        if (ev.classId == T2Demo::NetEventClassFirst + 9 &&
                            !ev.arguments.empty()) {
                            const std::string& command = ev.arguments[0];
                            if ((command == "ServerMessage" || command == "ChatMessage") &&
                                ev.arguments.size() >= (command == "ServerMessage" ? 3u : 4u)) {
                                const size_t templateIndex = command == "ServerMessage" ? 2 : 3;
                                std::vector<std::string> values(
                                    ev.arguments.begin() + templateIndex + 1, ev.arguments.end());
                                displayText = formatDemoRemoteText(ev.arguments[templateIndex], values);
                            } else if (command != "ServerMessage" && command != "ChatMessage") {
                                // HUD-only remote commands are state changes, not chat entries.
                                continue;
                            }
                        }
                        if (ev.classId == T2Demo::NetEventClassFirst + 9 &&
                            !ev.arguments.empty() && ev.arguments[0] == "ServerMessage") {
                            std::vector<VMValue> callbackArgs;
                            if (ev.arguments.size() >= 2) {
                                callbackArgs.emplace_back(ev.arguments[1]);
                                callbackArgs.emplace_back(std::string());
                                for (size_t i = 2; i < ev.arguments.size(); ++i)
                                    callbackArgs.emplace_back(ev.arguments[i]);
                                if (auto* ts = Engine::instance().script().ts())
                                    ts->dispatchMessageCallback(ev.arguments[1], callbackArgs);
                            }
                        } else if (ev.classId == T2Demo::NetEventClassFirst + 9) {
                            demoParser->handleHudRemoteCommand(ev.arguments[0], ev.arguments);
                            dispatchHudClientCommand(ev.arguments);
                        }
                        DemoTimedEvent te;
                        te.time = blockTime;
                        te.text = displayText;
                        te.ghostIndex = -1;
                        if (ev.classId == T2Demo::NetEventClassFirst + 22) {
                            te.type = 0; // chat message
                        } else if (ev.classId == T2Demo::NetEventClassFirst + 9) {
                            te.type = (!ev.arguments.empty() && ev.arguments[0] == "ChatMessage")
                                ? 0 : 1; // server command or chat remote
                        } else {
                            te.type = 2; // system
                        }
                        // Try to find source player ghost for chat messages
                        if (te.type == 0 && !pd.ghosts.empty()) {
                            te.ghostIndex = pd.ghosts[0].index;
                        }
                        demoEventLog.push_back(te);
                        if (demoEventLog.size() > 200)
                            demoEventLog.erase(demoEventLog.begin(), demoEventLog.begin() +
                                               (demoEventLog.size() - 200));
                    }
                    // Update compression point from GameState
                     if (pd.gameState.hasCameraTransform) {
                        demoPrevCameraPos = demoCameraPos;
                        demoPrevCameraTarget = demoCameraTarget;
                        demoCameraPos = {pd.gameState.cameraPosition.x,
                                          pd.gameState.cameraPosition.y,
                                          pd.gameState.cameraPosition.z};
                        const Vec3 direction = T2Demo::cameraDirectionFromYawPitch(
                            pd.gameState.cameraYaw, pd.gameState.cameraPitch);
                        demoCameraTarget = {
                            demoCameraPos.x + direction.x,
                            demoCameraPos.y + direction.y,
                            demoCameraPos.z + direction.z};
                         demoMoveBlend = 0.0f;
                         demoHasPos = true;
                         demoAuthoredCamera = true;
                    }
                    if (pd.gameState.controlObjectDirty) {
                        // Full control object update with new ghost index
                        // (position comes from move blocks, not GameState)
                    } else if (pd.gameState.compressionPoint.x != 0 ||
                               pd.gameState.compressionPoint.y != 0 ||
                               pd.gameState.compressionPoint.z != 0) {
                          // Update compression point from partial control update
                          Vec3 cp = pd.gameState.compressionPoint;
                          const int controlIndex = pd.gameState.controlObjectGhostIndex >= 0
                              ? pd.gameState.controlObjectGhostIndex : controlGhostIndex;
                          if (demoParser) {
                              const auto* control = demoParser->getGhostTracker().getGhost(controlIndex);
                              if (control && control->className == "Player") cp.z += 1.5f;
                          }
                          demoPrevCameraPos = demoCameraPos;
                          demoPrevCameraTarget = demoCameraTarget;
                          demoCameraPos = {cp.x, cp.y, cp.z};
                          demoCameraTarget = demoHasOrientation
                              ? Point3F{cp.x + std::sin(demoViewYaw) * std::cos(demoViewPitch),
                                        cp.y + std::cos(demoViewYaw) * std::cos(demoViewPitch),
                                        cp.z + std::sin(demoViewPitch)}
                              : Point3F{cp.x, cp.y + 2.0f, cp.z};
                          demoMoveBlend = 0.0f;
                          demoHasPos = true;
                     }
                    // GameState is sparse; an omitted effect must not erase the
                    // previous effect before its normal client-side decay.
                    if (pd.gameState.hasDamageFlash)
                        damageFlash = pd.gameState.damageFlash;
                    if (pd.gameState.hasWhiteOut)
                        whiteOut = pd.gameState.whiteOut;
                     if (pd.gameState.cameraFov > 0) demoCameraFov = pd.gameState.cameraFov;
                    // Camera shake on damage
                    if (pd.gameState.damageFlash > 0.5f)
                        shakeIntensity = std::max(shakeIntensity, pd.gameState.damageFlash * 3.0f);
                    // Store control object ghost index for highlight
                     if (pd.gameState.controlObjectGhostIndex >= 0)
                         controlGhostIndex = pd.gameState.controlObjectGhostIndex;

                    // Consume pending explosions from projectile parsers
                    auto explosions = demoParser->consumeExplosions();
                    for (auto& exp : explosions) {
                        Point3F expPos = Math::torquePointToYUp({exp.position.x, exp.position.y, exp.position.z});
                        Point3F expNormal = Math::torquePointToYUp({exp.normal.x, exp.normal.y, exp.normal.z});
                        const V12::DecodedDataBlock* projectileData = nullptr;
                        const V12::DecodedDataBlock* explosionData = nullptr;
                        const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                        auto projectileIt = dataBlocks.find((uint32_t)exp.projectileDataBlockId);
                        if (projectileIt != dataBlocks.end()) {
                            projectileData = &projectileIt->second.decoded;
                            auto explosionIt = dataBlocks.find(projectileData->projectileExplosionRef);
                            if (explosionIt != dataBlocks.end())
                                explosionData = &explosionIt->second.decoded;
                        }
                        w->spawnExplosionEffect(expPos, projectileData, explosionData, &dataBlocks, expNormal);
                        shakeIntensity = std::max(shakeIntensity, 1.5f);
                    }

                    // Apply sun data from demo stream if .mis didn't provide it
                    if (!w->sunLightDirUsed) {
                        auto& sd = DemoParser::s_sunData;
                        if (sd.valid) {
                            w->sunLightDir.x = cosf(sd.elevation) * sinf(sd.azimuth);
                            w->sunLightDir.y = sinf(sd.elevation);
                            w->sunLightDir.z = cosf(sd.elevation) * cosf(sd.azimuth);
                            w->sunLightDirUsed = true;
                            w->sunColor = {(float)sd.r / 255.0f, (float)sd.g / 255.0f, (float)sd.b / 255.0f};
                            w->sunColorUsed = true;
                            Console::instance().printf(LogLevel::Info, "Applied sun from demo stream: az=%.0f el=%.0f color=(%d %d %d)",
                                sd.azimuth * 180.0f / 3.14159f, sd.elevation * 180.0f / 3.14159f, sd.r, sd.g, sd.b);
                                }
                            }
                        }
                delete block;
                if (demoPlaying && demoParser && (demoBlocksDone % 500) == 0)
                    demoSnapshots[demoBlocksDone] = demoParser->captureSnapshot();
            }

            // Try to load terrain from ghost data if not yet loaded
            if (demoPlaying && w && !w->terrain()->loaded && !DemoParser::s_pendingTerrainFile.empty()) {
                Console::instance().printf(LogLevel::Info, "Loading terrain from ghost data: %s", DemoParser::s_pendingTerrainFile.c_str());
                auto& fs = Engine::instance().fs();
                std::string tf = DemoParser::s_pendingTerrainFile;
                std::vector<std::string> terPaths = {tf, "terrains/" + tf, tf + ".ter", "terrains/" + tf + ".ter"};
                for (auto& tp : terPaths) {
                    auto terData = fs.read(tp.c_str());
                    if (!terData.empty()) {
                        Console::instance().printf(LogLevel::Info, "  loaded terrain: %s", tp.c_str());
                        w->terrain()->load(terData.data(), terData.size());
                        break;
                    }
                }
                DemoParser::s_pendingTerrainFile.clear(); // only try once
            }

            if (demoPlaying && demoParser) {
                auto applySlots = [](GuiControl* control, const auto& state, int active,
                                     const auto& bitmaps, const std::string& background,
                                     const std::string& highlight, const std::string& infinite) {
                    if (!control) return;
                    if (control->hudSlots.size() < 32) control->hudSlots.resize(32);
                    for (auto& slot : control->hudSlots) {
                        slot.visible = false;
                        slot.active = false;
                        slot.bitmap.clear();
                    }
                    for (const auto& [index, amount] : state) {
                        if (index < 0 || index >= (int)control->hudSlots.size()) continue;
                        control->hudSlots[index].amount = amount;
                        auto bitmap = bitmaps.find(index);
                        if (bitmap != bitmaps.end()) control->hudSlots[index].bitmap = bitmap->second;
                        control->hudSlots[index].visible = true;
                        control->hudSlots[index].active = index == active;
                    }
                    control->activeHudSlot = active;
                    control->fields["backgroundBitmap"] = background;
                    control->fields["highlightBitmap"] = highlight;
                    control->fields["infiniteAmmoBitmap"] = infinite;
                };
                auto& gui = Engine::instance().guiRenderer();
                applySlots(gui.findControl("weaponsHud"), demoParser->getWeaponsHud().slots,
                            demoParser->getWeaponsHud().activeIndex, demoParser->getWeaponsHud().bitmaps,
                            demoParser->getWeaponsHud().backgroundBitmap,
                            demoParser->getWeaponsHud().highlightBitmap,
                            demoParser->getWeaponsHud().infiniteAmmoBitmap);
                applySlots(gui.findControl("inventoryHud"), demoParser->getInventoryHud().slots, -1,
                            demoParser->getInventoryHud().bitmaps,
                            demoParser->getInventoryHud().backgroundBitmap, "", "");
                if (auto* frame = gui.findControl("backpackFrame"))
                    frame->visible = demoParser->getBackpackHud().active;
                if (auto* text = gui.findControl("backpackText"))
                {
                    text->text = demoParser->getBackpackHud().text;
                    text->visible = demoParser->getBackpackHud().active &&
                                    atoi(demoParser->getBackpackHud().text.c_str()) != 0;
                }
                if (auto* icon = gui.findControl("backpackIcon")) {
                    icon->visible = demoParser->getBackpackHud().active;
                    // The stock client derives the bitmap from the pack index.
                    // Preserve that script-side result when the replay command
                    // does not carry an explicit bitmap.
                    if (!demoParser->getBackpackHud().active ||
                        !demoParser->getBackpackHud().bitmap.empty())
                        icon->bitmap = demoParser->getBackpackHud().bitmap;
                }
                if (auto* dashboard = gui.findControl("dashboardHud"))
                    dashboard->visible = demoParser->getVehicleHud().dashboardVisible;
                if (auto* vehicleWeapon = gui.findControl("vWeaponsBox")) {
                    vehicleWeapon->visible = demoParser->getVehicleHud().dashboardVisible;
                    vehicleWeapon->activeHudSlot = demoParser->getVehicleHud().activeWeapon;
                }
                if (auto* ammo = gui.findControl("ammoHud"))
                    ammo->text = demoParser->getAmmoHud().count < 0
                        ? std::string() : std::to_string(demoParser->getAmmoHud().count);
            }

            // Debug: ghost stats every ~500 blocks
            if (demoPlaying && demoParser && (demoBlocksDone % 500) == 0 && demoBlocksDone > 0) {
                const GhostTracker& gt = demoParser->getGhostTracker();
                Console::instance().printf(LogLevel::Debug, "Blocks: %d, Ghosts: %d", demoBlocksDone, gt.size());
                if (gt.size() > 0) {
                    int withPos = 0;
                    for (int i : gt.getAllIndices()) {
                        auto* g = gt.getGhost(i);
                        if (g) {
                            if (g->position.x != 0 || g->position.y != 0 || g->position.z != 0) {
                                withPos++;
                                Console::instance().printf(LogLevel::Debug, "  HAS POS: Ghost[%d] class=%d '%s' pos=(%.1f %.1f %.1f)",
                                    i, g->classId, g->className.c_str(), g->position.x, g->position.y, g->position.z);
                            }
                        }
                    }
                    Console::instance().printf(LogLevel::Debug, "Ghosts: %d total, %d with pos", gt.size(), withPos);
                }
            }

            // Update window title with progress
            int pct = demoBlocksTotal > 0 ? (demoBlocksDone * 100 / demoBlocksTotal) : 0;
            char title[128];
            snprintf(title, sizeof(title), "Torch - Demo [%d%%] %d/%d blocks %d packets",
                pct, demoBlocksDone, demoBlocksTotal, demoPacketsParsed);
            Engine::instance().platform().setTitle(title);

            // Advance interpolation blend (smooth over ~150ms)
            if (demoMoveBlend < 1.0f) {
                demoMoveBlend = std::min(demoMoveBlend + demoInterpolationDt * 6.0f, 1.0f);
            }

            // Free camera toggle during demos (F1)
            static bool prevDemoFreeCam = false;
            if (currentInput.freeCam && !prevDemoFreeCam) {
                freeCamActive = !freeCamActive;
                if (freeCamActive) {
                    freeCamPos = demoHasPos ? demoCameraPos : pl->cameraPos();
                    freeCamTarget = demoHasPos ? demoCameraTarget : pl->cameraTarget();
                }
            }
            prevDemoFreeCam = currentInput.freeCam;
            if (freeCamActive) {
                float camSpeed = 50.0f * dt;
                float yaw = freeCamRot.z;
                float pitch = freeCamRot.x;
                yaw += currentInput.lookDelta.y;
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                freeCamRot = {pitch, 0, yaw};
                Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                Point3F right = {std::cos(yaw), 0, -std::sin(yaw)};
                if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
                if (currentInput.left) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
                if (currentInput.right) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
                    } else if (demoOrbitCam) {
                float orbitSpeed = 60.0f * dt;
                bool orbitInput = currentInput.left || currentInput.right;
                if (currentInput.left) orbitAngle -= orbitSpeed;
                else if (currentInput.right) orbitAngle += orbitSpeed;
                // Auto-rotate when no input (gentle spin to show the scene)
                if (!orbitInput) orbitAngle += dt * 8.0f;
                 if (currentInput.forward) orbitDistance = std::max(20.0f, orbitDistance - 50.0f * dt);
                if (currentInput.backward) orbitDistance = std::min(2000.0f, orbitDistance + 50.0f * dt);
                if (currentInput.jump) orbitHeight = std::min(500.0f, orbitHeight + 30.0f * dt);
                 if (currentInput.jet) orbitHeight = std::max(20.0f, orbitHeight - 30.0f * dt);
             }
            auto& demoKeys = Engine::instance().platform().input().keysDown;
            static bool prevDemoF2 = false, prevDemoF4 = false;
            const bool demoF2 = demoKeys[SCANCODE_F2];
            const bool demoF4 = demoKeys[SCANCODE_F4];
            if (demoF2 && !prevDemoF2) {
                demoOrbitCam = !demoOrbitCam;
                demoFirstPersonCam = false;
                orbitCenterInit = false;
            }
            if (demoF4 && !prevDemoF4) {
                demoFirstPersonCam = !demoFirstPersonCam;
                demoOrbitCam = false;
            }
            prevDemoF2 = demoF2;
            prevDemoF4 = demoF4;

            auto& audio = Engine::instance().audio();
            if (audio.config().enabled) {
                const Point3F camPos = freeCamActive ? freeCamPos : demoCameraPos;
                const Point3F camTarget = freeCamActive ? freeCamTarget : demoCameraTarget;
                Point3F forward = {camTarget.x - camPos.x, camTarget.y - camPos.y,
                                   camTarget.z - camPos.z};
                const float length = std::sqrt(forward.x * forward.x + forward.y * forward.y +
                                               forward.z * forward.z);
                if (length > 0.0001f) {
                    forward.x /= length; forward.y /= length; forward.z /= length;
                }
                audio.update(camPos, {0, 0, 0}, forward, {0, 1, 0},
                             w && w->isUnderwater(camPos));
            }
            return; // skip normal game logic during demo playback
        }

        // Demo playback has its own clock; normal simulation honors the
        // TorqueScript time scale from this point onward.
        dt = simulationDt;

        // Mapper mode: free-fly camera only, no player gameplay
        if (mapperMode) {
            if (freeCamActive) {
                float camSpeed = 50.0f * dt;
                // Speed multipliers: Shift = 3x, Ctrl = 0.25x
                auto& plat = Engine::instance().platform();
                auto& keys = plat.input().keysDown;
                if (keys[SCANCODE_LSHIFT] || keys[SCANCODE_RSHIFT]) camSpeed *= 3.0f;
                if (keys[SCANCODE_LCTRL] || keys[SCANCODE_RCTRL]) camSpeed *= 0.25f;
                float yaw = freeCamRot.z;
                float pitch = freeCamRot.x;
                yaw += currentInput.lookDelta.y;        // normalized mouse X → yaw
                pitch -= currentInput.lookDelta.x;      // mouse Y (vert) → pitch
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                freeCamRot = {pitch, 0, yaw};
                Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                Point3F right = {std::cos(yaw), 0, -std::sin(yaw)};
                if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
                if (currentInput.left) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
                if (currentInput.right) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
            }
            // Update world (projectiles, items, etc.)
            w->update(dt);
            // Update audio listener from camera
            auto& audio = Engine::instance().audio();
            if (audio.config().enabled) {
                Point3F fwd = {std::sin(freeCamRot.z) * std::cos(freeCamRot.x), std::sin(freeCamRot.x), std::cos(freeCamRot.z) * std::cos(freeCamRot.x)};
                audio.update(freeCamPos, {0,0,0}, fwd, {0,1,0}, w->isUnderwater(freeCamPos));
            }
            return;
        }

        // F1 toggle for free camera (edge-triggered)
        static bool prevFreeCam = false;
        if (currentInput.freeCam && !prevFreeCam) {
            freeCamActive = !freeCamActive;
            if (freeCamActive) {
                freeCamPos = pl->cameraPos();
                freeCamTarget = pl->cameraTarget();
                freeCamRot = pl->rotation();
            }
        }
        prevFreeCam = currentInput.freeCam;

        // F2 toggle for orbit camera
        static bool prevF2 = false;
        if (currentInput.orbitCam && !prevF2) {
            demoFirstPersonCam = false;
            demoOrbitCam = !demoOrbitCam;
            if (freeCamActive) { freeCamActive = false; }
        }
        prevF2 = currentInput.orbitCam;

        // F3 toggle for editor mode
        auto& plat = Engine::instance().platform();
        static bool prevF3 = false;
        bool f3Down = plat.input().keysDown[SCANCODE_F3];
        if (f3Down && !prevF3) {
            editorActive = !editorActive;
            if (editorActive) {
                freeCamActive = true;
                Console::instance().printf(LogLevel::Info, "Editor mode %s", editorActive ? "ON" : "OFF");
            }
        }
        prevF3 = f3Down;

        // F4 toggle for first-person camera
        static bool prevF4 = false;
        bool f4Down = plat.input().keysDown[SCANCODE_F4];
        if (f4Down && !prevF4) {
            demoFirstPersonCam = !demoFirstPersonCam;
            demoOrbitCam = false;
            if (freeCamActive) { freeCamActive = false; }
            Console::instance().printf(LogLevel::Info, "First-person camera %s", demoFirstPersonCam ? "ON" : "OFF");
        }
        prevF4 = f4Down;

        // Editor: place ghost on left click, cycle class on scroll
        if (editorActive && freeCamActive) {
            static bool prevClick = false;
            bool click = plat.input().mouseButtons[0];
            if (click && !prevClick) {
                // Place a ghost at the camera's target position
                float dist = 20.0f;
                Point3F dir = {freeCamTarget.x - freeCamPos.x, freeCamTarget.y - freeCamPos.y, freeCamTarget.z - freeCamPos.z};
                float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
                if (len > 0.001f) { dir.x /= len; dir.y /= len; dir.z /= len; }
                Point3F placePos = {freeCamPos.x + dir.x * dist, freeCamPos.y + dir.y * dist, freeCamPos.z + dir.z * dist};
                server.spawnGhost(editorPlaceClass, placePos.x, placePos.y, placePos.z);
                Console::instance().printf(LogLevel::Info, "Placed ghost class=%d at (%.1f,%.1f,%.1f)",
                    editorPlaceClass, placePos.x, placePos.y, placePos.z);
            }
            prevClick = click;
            // Right click to remove nearest ghost
            static bool prevRight = false;
            bool right = plat.input().mouseButtons[1];
            if (right && !prevRight) {
                // Remove last spawned ghost
                if (server.ghostCount() > 0) {
                    // Simple approach: remove ghosts in reverse order via command
                    Console::instance().printf(LogLevel::Info, "Right-click: remove ghost. Use 'sv_removeghost <idx>'");
                }
            }
            prevRight = right;
            // Scroll wheel to cycle class
            static float prevScroll = 0;
            float scroll = plat.input().mouseWheel;
            if (scroll != prevScroll) {
                int delta = (scroll > prevScroll) ? 1 : -1;
                editorPlaceClass += delta;
                if (editorPlaceClass < 0) editorPlaceClass = 62;
                if (editorPlaceClass > 62) editorPlaceClass = 0;
                Console::instance().printf(LogLevel::Info, "Editor: placing class %d", editorPlaceClass);
            }
            prevScroll = scroll;
        }

        if (freeCamActive) {
            // Move free camera using WASD + mouse
            float camSpeed = 50.0f * dt;
            float yaw = freeCamRot.z;
            float pitch = freeCamRot.x;

            // Mouse look
            yaw += currentInput.lookDelta.y;
            pitch -= currentInput.lookDelta.x;
            if (pitch > 1.5f) pitch = 1.5f;
            if (pitch < -1.5f) pitch = -1.5f;
            freeCamRot = {pitch, 0, yaw};

            // Direction vectors
            Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
            Point3F right = {std::cos(yaw), 0, -std::sin(yaw)};

            if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
            if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
            if (currentInput.left) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
            if (currentInput.right) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
            if (currentInput.jump) freeCamPos.y += camSpeed;
            if (currentInput.jet) freeCamPos.y -= camSpeed;

            freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
        } else {
            // Update physics
            Physics physics;
            physics.update(pl, dt, currentInput);

            // Update player animation state
            pl->updateAnimation(dt, currentInput.jet);

            // Update weapon timers
            for (int i = 0; i < pl->weaponCount(); i++) {
                const_cast<Weapon&>(pl->weapon(i)).updateTimers(dt);
            }
            pl->updateWeaponHud();

            // Fire weapon
            if (currentInput.fire) {
                pl->fireWeapon(false);
            }
            if (currentInput.altFire) {
                pl->fireWeapon(true);
            }

            // ─── Chat input ──────────────────────────────────────
            if (cfg.online && activeConn && !activeConn->isObserverMode() &&
                activeConn->state() >= Connection::Connected) {
                static bool chatActive = false;
                static std::string chatBuf;
                auto& plat = Engine::instance().platform();
                hud->setChatInput("");
                bool enterDown = plat.input().keysDown[SCANCODE_RETURN];
                bool escDown = plat.input().keysDown[SCANCODE_ESCAPE];
                if (!chatActive) {
                    static bool prevEnter = false;
                    if (enterDown && !prevEnter) {
                        chatActive = true;
                        chatBuf.clear();
                        plat.startTextInput();
                        plat.setRelativeMouse(false);
                        plat.showMouse(true);
                    }
                    prevEnter = enterDown;
                } else {
                    const std::string& ti = plat.input().textInput;
                    for (char c : ti) {
                        if (c >= 0x20 && c <= 0x7e && chatBuf.size() < 200)
                            chatBuf += c;
                    }
                    static bool prevEnter = false;
                    if (enterDown && !prevEnter && !chatBuf.empty()) {
                        // Native Tribes 2 routes chat through commandToServer.
                        std::string command = "messageSent \"";
                        for (char c : chatBuf) {
                            if (c == '\\' || c == '"') command.push_back('\\');
                            command.push_back(c);
                        }
                        command.push_back('"');
                        activeConn->sendCommandPacket(command.c_str());
                        chatBuf.clear();
                        chatActive = false;
                        plat.stopTextInput();
                        plat.setRelativeMouse(true);
                        plat.showMouse(false);
                    }
                    prevEnter = enterDown;
                    static bool prevEsc = false;
                    if (escDown && !prevEsc) {
                        chatActive = false;
                        chatBuf.clear();
                        plat.stopTextInput();
                        plat.setRelativeMouse(true);
                        plat.showMouse(false);
                    }
                    prevEsc = escDown;
                    // Backspace
                    static bool prevBS = false;
                    if (plat.input().keysDown[SCANCODE_BACKSPACE] && !prevBS && !chatBuf.empty())
                        chatBuf.pop_back();
                    prevBS = plat.input().keysDown[SCANCODE_BACKSPACE];
                    // Keep the editable line separate from expiring popups.
                    hud->setChatInput(chatBuf.c_str());
                }
            }

            // Client-side prediction: store move and send to server
            if (cfg.online && activeConn && activeConn->state() >= Connection::Connected) {
                uint32_t thisSeq = ++moveSeq;
                // Store input for later reconciliation
                pendingMoves.push_back({thisSeq, currentInput, dt});
                if (pendingMoves.size() > 128)
                    pendingMoves.pop_front();

                V12::ClientMove nativeMove;
                nativeMove.x = (currentInput.right ? 1.0f : 0.0f) -
                                (currentInput.left ? 1.0f : 0.0f);
                nativeMove.y = (currentInput.forward ? 1.0f : 0.0f) -
                                (currentInput.backward ? 1.0f : 0.0f);
                nativeMove.z = (currentInput.jump ? 1.0f : 0.0f) -
                                (currentInput.jet ? 1.0f : 0.0f);
                nativeMove.yaw = currentInput.lookDelta.y;
                nativeMove.pitch = currentInput.lookDelta.x;
                nativeMove.trigger[0] = currentInput.fire;
                nativeMove.trigger[1] = currentInput.altFire;
                nativeMove.trigger[2] = currentInput.reload;
                activeConn->sendNativeMove(thisSeq, nativeMove);
            }

            // Reload
            if (currentInput.reload) {
                int32_t cw = pl->currentWeapon();
                if (cw >= 0 && cw < pl->weaponCount()) {
                    auto& weapon = const_cast<Weapon&>(pl->weapon(cw));
                    if (gWeaponTable[weapon.type].maxAmmo > 0 && !weapon.reloading) {
                        weapon.reloading = true;
                        weapon.reloadTimer = gWeaponTable[weapon.type].reloadTime;
                    }
                }
            }
        }

        // Death check (deaths tracked in Player::applyDamage)
        if (pl->health() <= 0 && gameState == Playing) {
            deathTimer = 0.0f;
            currentInput = {};
            pendingMoves.clear();
            w->projectiles().clear();
            setState(Dead);
        }

        // Update world (projectiles, etc.)
        w->update(dt);

        // Update audio listener from camera
        auto& audio = Engine::instance().audio();
        if (audio.config().enabled) {
            Point3F camPos = freeCamActive ? freeCamPos : pl->cameraPos();
            Point3F camTarget = freeCamActive ? freeCamTarget : pl->cameraTarget();
            Point3F forward = {camTarget.x - camPos.x, camTarget.y - camPos.y, camTarget.z - camPos.z};
            float flen = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
            if (flen > 0.0001f) { forward.x /= flen; forward.y /= flen; forward.z /= flen; }
            Point3F up = {0, 1, 0};
            audio.update(camPos, pl->velocity(), forward, up, w->isUnderwater(camPos));
        }
    } else if (gameState == Dead) {
        // Spectator mode when online
        if (cfg.online && activeConn && activeConn->isConnected()) {
            auto isSpectatable = [this](int index) {
                const GhostEntry* ghost = liveGhosts.getGhost(index);
                if (!ghost) return false;
                return ghost->className == "Player" ||
                       ghost->className == "FlyingVehicle" ||
                       ghost->className == "HoverVehicle" ||
                       ghost->className == "WheeledVehicle" ||
                       ghost->className == "Vehicle";
            };
            auto spectatableIndices = liveGhosts.getAllIndices();
            spectatableIndices.erase(
                std::remove_if(spectatableIndices.begin(), spectatableIndices.end(),
                    [&](int index) {
                        return !isSpectatable(index) ||
                               (uint32_t)index == serverPlayerGhostIndex;
                    }),
                spectatableIndices.end());
             if (!spectatableIndices.empty() &&
                (!liveSpectateInit ||
                 std::find(spectatableIndices.begin(), spectatableIndices.end(),
                           spectateGhostIndex) == spectatableIndices.end())) {
                // Native observers already receive the server's current target;
                // use it before falling back to stable ghost order.
                int initial = -1;
                if (activeConn->isObserverMode()) {
                    const int control = (int)activeConn->observerSnapshot().controlGhost;
                    if (std::find(spectatableIndices.begin(), spectatableIndices.end(),
                                  control) != spectatableIndices.end())
                        initial = control;
                }
                if (initial < 0) initial = spectatableIndices.front();
                 liveSpectateInit = true;
                 freeCamActive = false;
                 spectateGhostIndex = initial;
                 liveFollowGhostIndex = -1;
                 liveFollowCenterInit = false;
             }
            // Cycle ghosts with the observer right mouse action / R key.
            static bool prevCycle = false;
            bool cycleNow = observerCyclePressed(currentInput.reload, currentInput.altFire);
            if (cycleNow && !prevCycle) {
                if (!spectatableIndices.empty()) {
                    int cur = 0;
                    for (size_t i = 0; i < spectatableIndices.size(); i++)
                        if (spectatableIndices[i] == spectateGhostIndex) { cur = (int)i; break; }
                     cur = (cur + 1) % (int)spectatableIndices.size();
                     spectateGhostIndex = spectatableIndices[cur];
                     liveFollowGhostIndex = -1;
                     liveFollowCenterInit = false;
                }
            }
            prevCycle = cycleNow;

            // Toggle free cam
            static bool prevFree = false;
            if (currentInput.freeCam && !prevFree) {
                freeCamActive = !freeCamActive;
                if (freeCamActive) {
                    freeCamPos = {0, 10, 0};
                    freeCamTarget = {0, 10, -1};
                    freeCamRot = {0, 0, 0};
                }
            }
            prevFree = currentInput.freeCam;

            // Free cam movement
            if (freeCamActive) {
                float pitch = freeCamRot.x, yaw = freeCamRot.z;
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                yaw -= currentInput.lookDelta.y;
                freeCamRot = {pitch, 0, yaw};
                float camSpeed = 30.0f * dt;
                Point3F fwd = {sinf(yaw)*cosf(pitch), sinf(pitch), cosf(yaw)*cosf(pitch)};
                Point3F right = {cosf(yaw), 0, -sinf(yaw)};
                if (currentInput.forward) { freeCamPos.x += fwd.x*camSpeed; freeCamPos.y += fwd.y*camSpeed; freeCamPos.z += fwd.z*camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x*camSpeed; freeCamPos.y -= fwd.y*camSpeed; freeCamPos.z -= fwd.z*camSpeed; }
                if (currentInput.left) { freeCamPos.x -= right.x*camSpeed; freeCamPos.z -= right.z*camSpeed; }
                if (currentInput.right) { freeCamPos.x += right.x*camSpeed; freeCamPos.z += right.z*camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
            }
        } else {
            // Offline: auto-respawn after delay
            deathTimer += dt;
            if (deathTimer >= DeathRespawn::RespawnDelay) {
                deathTimer = 0.0f;
                 liveSpectateInit = false;
                 liveFollowGhostIndex = -1;
                 liveFollowCenterInit = false;
                 pl->respawn();
                setState(Playing);
            }
        }
    }
}

void Game::render(float dt) {
    if (gameState != Playing && gameState != Dead && !testShapeLoaded && !shapeViewerActive) return;

    auto& eng = Engine::instance();
    auto& r = eng.renderer();
    auto applyShapeBaseAudio = [&](GhostEntry& ghost, int ghostIndex, const Vec3& position) {
        auto& audio = eng.audio();
        if (!audio.isInitialized()) return;
        for (int slot = 0; slot < 4; ++slot) {
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(ghostIndex)) << 3) |
                                 static_cast<uint64_t>(slot);
            const auto& state = ghost.soundThreads[slot];
            auto it = shapeBaseSoundSources.find(key);
            if (!state.valid || !state.playing || state.profileId < 0) {
                if (it != shapeBaseSoundSources.end()) {
                    audio.releaseSource(it->second);
                    shapeBaseSoundSources.erase(it);
                }
                continue;
            }
            if (it == shapeBaseSoundSources.end()) {
                scanAudioProfiles();
                if (state.profileId >= (int)s_audioProfiles.size()) continue;
                auto* buffer = audio.loadSound(s_audioProfiles[state.profileId].filename.c_str());
                auto* source = buffer ? audio.createSource(true) : nullptr;
                if (!source) continue;
                source->setLooping(true);
                source->setVolume(0.3f * audio.config().masterVolume * audio.config().sfxVolume);
                source->play(buffer);
                it = shapeBaseSoundSources.emplace(key, source).first;
            }
            it->second->setPosition(Math::torquePointToYUp({position.x, position.y, position.z}));
        }
    };
    r.beginFrame({0.3f, 0.5f, 0.8f, 1.0f});

    Point3F camPos, camTarget;
    bool cameraCoordinatesConverted = false;
    if (freeCamActive) {
        camPos = freeCamPos;
        camTarget = freeCamTarget;
    } else if (demoPlaying && (demoHasPos || demoFirstPersonCam)) {
        // Orbit camera for spectator mode
        if (demoFirstPersonCam) {
            if (demoHasPos && demoAuthoredCamera) {
                camPos = demoCameraPos;
                camTarget = demoCameraTarget;
            } else {
                camPos = pl ? pl->cameraPos() : Point3F{0, 6, 0};
                camTarget = pl ? pl->cameraTarget() : Point3F{0, 6, 1};
                bool cameraGhostUsed = false;
            // First-person camera: position at spectated ghost's eye level, look in facing direction
            int fpIdx = (spectateGhostIndex >= 0) ? spectateGhostIndex : controlGhostIndex;
            if (fpIdx >= 0 && demoParser) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(fpIdx);
                if (!g || (g->position.x == 0 && g->position.y == 0 && g->position.z == 0)) {
                    for (const int index : demoParser->getGhostTracker().getAllIndices()) {
                        const auto* candidate = demoParser->getGhostTracker().getGhost(index);
                        if (candidate && candidate->className == "Player" &&
                            (candidate->position.x != 0 || candidate->position.y != 0 || candidate->position.z != 0)) {
                            g = candidate;
                            break;
                        }
                    }
                }
                if (g && (g->position.x != 0 || g->position.y != 0 || g->position.z != 0)) {
                    const Vec3& position = g->hasRendered ? g->renderPos : g->position;
                    if (g->className == "Camera" && g->hasCameraEuler) {
                        const float pitch = g->cameraEuler.x;
                        const float yaw = g->cameraEuler.z;
                        camPos = {position.x, position.y, position.z};
                        camTarget = {position.x + std::sin(yaw) * std::cos(pitch) * 10.0f,
                                     position.y + std::sin(pitch) * 10.0f,
                                     position.z + std::cos(yaw) * std::cos(pitch) * 10.0f};
                        cameraGhostUsed = true;
                    } else {
                        camPos = {position.x, position.y, position.z};
                        camPos.y += 1.8f;
                        const float yaw = atan2f(g->renderRotation.x, g->renderRotation.w) * 2.0f;
                        camTarget = {camPos.x + sinf(yaw) * 10.0f,
                                     camPos.y, camPos.z + cosf(yaw) * 10.0f};
                        cameraGhostUsed = true;
                    }
                }
            }
                if (!cameraGhostUsed && w && !w->observerCameras().empty()) {
                const auto& observer = w->observerCameras().front();
                camPos = Math::torquePointToYUp(observer.pos);
                const Point3F forward = Math::torqueCameraForwardToYUp(
                    observer.axis, Math::DEG2RAD(observer.angleDeg));
                camTarget = {camPos.x + forward.x * 10.0f,
                             camPos.y + forward.y * 10.0f,
                             camPos.z + forward.z * 10.0f};
                cameraCoordinatesConverted = true;
                }
            }
        } else if (demoOrbitCam) {
            Point3F targetPos = demoCameraPos;
            // If spectating a specific ghost, track its position
            if (spectateGhostIndex >= 0 && demoParser) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(spectateGhostIndex);
                if (g && (g->position.x != 0 || g->position.y != 0 || g->position.z != 0))
                    targetPos = {g->position.x, g->position.y, g->position.z};
            }
            // Initialize orbit center from target position
            if (!orbitCenterInit) {
                orbitCenter = targetPos;
                orbitCenterInit = true;
            }
            // Smoothly track the target
            float trackSpeed = 0.02f;
            orbitCenter.x += (targetPos.x - orbitCenter.x) * trackSpeed;
            orbitCenter.y += (targetPos.y + 20.0f - orbitCenter.y) * trackSpeed;
            orbitCenter.z += (targetPos.z - orbitCenter.z) * trackSpeed;
            float rad = orbitAngle * (3.14159f / 180.0f);
            camPos.x = orbitCenter.x + sinf(rad) * orbitDistance;
            camPos.z = orbitCenter.z + cosf(rad) * orbitDistance;
            camPos.y = orbitCenter.y + orbitHeight;
            camTarget = orbitCenter;
            camTarget.y += 10.0f; // look slightly above center
        } else if (!demoPlaying && cfg.online && gameState == Dead && liveGhosts.size() > 0) {
            // Live spectator: follow spectated ghost
            auto isSpectatable = [](const GhostEntry* ghost) {
                if (!ghost) return false;
                return ghost->className == "Player" ||
                       ghost->className == "FlyingVehicle" ||
                       ghost->className == "HoverVehicle" ||
                       ghost->className == "WheeledVehicle" ||
                       ghost->className == "Vehicle";
            };
            if (!isSpectatable(liveGhosts.getGhost(spectateGhostIndex))) {
                auto idxs = liveGhosts.getAllIndices();
                auto first = std::find_if(idxs.begin(), idxs.end(),
                    [&](int index) { return isSpectatable(liveGhosts.getGhost(index)); });
                spectateGhostIndex = first == idxs.end() ? -1 : *first;
            }
             const GhostEntry* g = liveGhosts.getGhost(spectateGhostIndex);
             if (g && (g->position.x != 0 || g->position.y != 0 || g->position.z != 0)) {
                if (freeCamActive) {
                    camPos = freeCamPos;
                    camTarget = freeCamTarget;
                } else {
                    const Point3F targetPos{g->position.x, g->position.y, g->position.z};
                    if (liveFollowGhostIndex != spectateGhostIndex) {
                        liveFollowGhostIndex = spectateGhostIndex;
                        liveFollowCenter = targetPos;
                        liveFollowCenterInit = true;
                    }
                    // setOrbitMode follows the target without teleporting the view;
                    // use frame-rate-independent tracking for the native ghost path.
                    const float followAlpha = 1.0f - std::exp(-10.0f * std::max(dt, 0.0f));
                    liveFollowCenter.x += (targetPos.x - liveFollowCenter.x) * followAlpha;
                    liveFollowCenter.y += (targetPos.y - liveFollowCenter.y) * followAlpha;
                    liveFollowCenter.z += (targetPos.z - liveFollowCenter.z) * followAlpha;
                    camPos = {liveFollowCenter.x, liveFollowCenter.y + 4.0f,
                              liveFollowCenter.z - 6.0f};
                    camTarget = liveFollowCenter;
                    camTarget.y += 2.0f;
                }
            } else {
                camPos = freeCamActive ? freeCamPos : Point3F{0, 10, 0};
                camTarget = freeCamActive ? freeCamTarget : Point3F{0, 10, -1};
                liveFollowGhostIndex = -1;
                liveFollowCenterInit = false;
            }
        } else if (demoMoveBlend < 1.0f) {
            float t = demoMoveBlend;
            camPos.x = demoPrevCameraPos.x + (demoCameraPos.x - demoPrevCameraPos.x) * t;
            camPos.y = demoPrevCameraPos.y + (demoCameraPos.y - demoPrevCameraPos.y) * t;
            camPos.z = demoPrevCameraPos.z + (demoCameraPos.z - demoPrevCameraPos.z) * t;
            camTarget.x = demoPrevCameraTarget.x + (demoCameraTarget.x - demoPrevCameraTarget.x) * t;
            camTarget.y = demoPrevCameraTarget.y + (demoCameraTarget.y - demoPrevCameraTarget.y) * t;
            camTarget.z = demoPrevCameraTarget.z + (demoCameraTarget.z - demoPrevCameraTarget.z) * t;
        } else {
            camPos = demoCameraPos;
            camTarget = demoCameraTarget;
        }
    } else if (eng.hasPreviewCam()) {
        camPos = eng.getPreviewCamPos();
        camTarget = eng.getPreviewCamTarget();
    } else if (pl) {
        camPos = pl->cameraPos();
        camTarget = pl->cameraTarget();
    } else {
        camPos = {0, 6, 0};
        camTarget = {0, 6, 1};
    }
    // Apply camera shake
    const Point3F nativeShake = w ? w->cameraShakeOffset(camPos) : Point3F{};
    Point3F finalCam = {camPos.x + shakeOffset.x + nativeShake.x,
                        camPos.y + shakeOffset.y + nativeShake.y,
                        camPos.z + shakeOffset.z + nativeShake.z};
    bool cameraOverride = false;
    // Diagnostic camera override for mapper analysis (TORCH_CAM=px,py,pz,tx,ty,tz)
    if (const char* camOv = getenv("TORCH_CAM")) {
        float v[6] = {0,0,0,0,0,0};
        sscanf(camOv, "%f,%f,%f,%f,%f,%f", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]);
        finalCam = {v[0], v[1], v[2]};
        camTarget = {v[3], v[4], v[5]};
        cameraOverride = true;
    }
    if (demoPlaying && !cameraOverride && !cameraCoordinatesConverted) {
        finalCam = Math::torquePointToYUp(finalCam);
        camTarget = Math::torquePointToYUp(camTarget);
    }
    // Apply FOV from demo stream if available
    float savedFov = r.config().fov;
    if (demoPlaying && demoCameraFov > 0 && demoCameraFov < 180) {
        r.config().fov = demoCameraFov;
    } else if (!mapperMode && currentInput.zoom) {
        r.config().fov = std::max(20.0f, savedFov * 0.5f);
    }
    r.setCamera(finalCam, camTarget, {0, 1, 0});
    r.config().fov = savedFov; // restore for HUD rendering

    // Shadow pass and scene rendering — skip in shape viewer / test shape mode
    if (!shapeViewerActive && !testShapeLoaded) {
        const char* dynShadows = Console::instance().getStringVariable("enableDynamicShadows", "1");
        r.shadowsActive = false;
        if (r.shadowEnabled() && (!dynShadows || atoi(dynShadows) != 0)) {
            // Compute scene bounds from terrain
            Point3F sceneCenter = {0, 0, 0};
            float sceneRadius = 500.0f;
            auto* tb = w->terrain();
            if (tb && tb->loaded) {
                sceneCenter.x = tb->worldOffset.x + tb->size * tb->squareSize * 0.5f;
                sceneCenter.z = tb->worldOffset.z - tb->size * tb->squareSize * 0.5f;
                float maxH = 0;
                for (auto h : tb->heights) if (h > maxH) maxH = h;
                sceneCenter.y = maxH * 0.5f;
                sceneRadius = tb->size * tb->squareSize * 0.8f;
        }
        Point3F lightDir = r.sunDir;
        float llen = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
        if (llen > 0) { lightDir.x /= llen; lightDir.y /= llen; lightDir.z /= llen; }

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        r.beginShadowPass(lightDir, sceneCenter, sceneRadius);

        // Render shadow casters with shadow depth shader
        auto* shadowShader = ShaderManager::getShadowShader();
        if (shadowShader) {
            shadowShader->bind();

            // Render terrain
            auto* tb2 = w->terrain();
            if (tb2 && tb2->loaded) {
                for (auto& mesh : tb2->meshes) {
                    shadowShader->setUniform("uLightMVP", r.lightViewProj());
                    mesh.render();
                }
            }

            // Render placed world objects as shadow casters (same transform as World::render)
            for (auto& obj : w->objects()) {
                if (!obj.shape || !obj.shape->loaded) continue;
                MatrixF model;
                if (obj.rotAngleDeg != 0 && (obj.rot.x != 0 || obj.rot.y != 0 || obj.rot.z != 0)) {
                    Point3F axis = obj.rot;
                    float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                    if (len > 0.0001f) {
                        axis.x /= len; axis.y /= len; axis.z /= len;
                        model = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
                    }
                }
                model.setTranslation(Math::torquePointToYUp(obj.pos));
                if (obj.scale.x != 1.0f || obj.scale.y != 1.0f || obj.scale.z != 1.0f) {
                    model = model * Math::torqueScaleToYUp(obj.scale);
                }
                if (obj.shape->nativeDTS) {
                    // Keep shadow placement identical to the visible shape.
                    MatrixF shapeFrame;
                    shapeFrame.setRotationY(Math::PI);
                    model = model * shapeFrame;
                }
                MatrixF mvp = r.lightViewProj() * model * obj.shape->upOrientation();
                shadowShader->setUniform("uLightMVP", mvp);
                for (auto& mesh : obj.shape->meshes)
                    mesh.render();
            }

            // Render bots
            for (auto& b : w->bots) {
                if (!b.alive || b.respawnTimer > 0) continue;
                if (b.shape && b.shape->loaded) {
                    MatrixF model;
                    model.setTranslation(b.pos);
                    MatrixF mvp = r.lightViewProj() * model * b.shape->upOrientation();
                    shadowShader->setUniform("uLightMVP", mvp);
                    // DTSShape::render binds the lit shader and would therefore
                    // submit bot geometry with stale main-pass uniforms. Shadow
                    // casters must stay on the depth shader.
                    for (auto& mesh : b.shape->meshes)
                        mesh.render();
                }
            }
        }

        r.endShadowPass();

        // Bind shadow map to texture unit 5 and set uniforms for main pass
        float shadowStrength = 0.6f;
        {
            auto* defShader = ShaderManager::getDefaultShader();
            defShader->bind();
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, r.shadowDepthTex);
            defShader->setUniform("uShadowMap", (int32_t)5);
            defShader->setUniform("uShadowStrength", shadowStrength);
            defShader->setUniform("uShadowMatrix", r.shadowMatrix());

            auto* terrShader = ShaderManager::getTerrainShader();
            terrShader->bind();
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, r.shadowDepthTex);
            terrShader->setUniform("uShadowMap", (int32_t)5);
            terrShader->setUniform("uShadowStrength", shadowStrength);
            terrShader->setUniform("uShadowMatrix", r.shadowMatrix());
        }
        r.shadowsActive = true;
    }
    else {
        r.shadowsActive = false;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    w->updateRendererLights(r);
    if (!demoPlaying && activeConn && activeConn->isConnected()) {
        auto lights = r.dynamicLights;
        for (int idx : liveGhosts.getAllIndices()) {
            const GhostEntry* ghost = liveGhosts.getGhost(idx);
            if (!ghost || ghost->className.empty() ||
                ghost->className.find("Projectile") == std::string::npos ||
                !ghost->hasDatablock) continue;
            const auto dataIt = nativeDatablocks.find(ghost->datablockId);
            if (dataIt == nativeDatablocks.end() || !dataIt->second.decoded.projectileHasLight)
                continue;
            const auto& data = dataIt->second.decoded;
            const auto color = Engine::instance().audio().isUnderwater() &&
                               data.projectileHasUnderwaterLightColor
                ? data.projectileUnderwaterLightColor : data.projectileLightColor;
            const Point3F projectilePos = Math::torquePointToYUp(
                {ghost->renderPos.x, ghost->renderPos.y, ghost->renderPos.z});
            lights.push_back({projectilePos.x, projectilePos.y, projectilePos.z,
                              color[0], color[1], color[2], data.projectileLightRadius, 2.0f});
        }
        r.setDynamicLights(lights);
    }
    w->render(finalCam);
    if (pl && !freeCamActive && !demoPlaying && !testShapeLoaded) pl->render();
    } // end if (!shapeViewerActive && !testShapeLoaded)

    if (mapperMode || gameState == Playing) {
        auto* font = r.getFont();
        if (font) {
            for (const auto& obj : w->objects()) {
                if (obj.label.empty()) continue;
                 if ((obj.className == "Marker" || obj.className == "MissionMarker" ||
                      obj.className == "SpawnSphere" || obj.className == "AIObjective") && !mapperMode)
                    continue;
                Point3F anchor = obj.labelAnchorValid
                    ? obj.labelAnchor
                    : Math::torquePointToYUp(obj.pos);
                Point3F screen = worldToScreen(anchor, r.viewMatrix(),
                    r.projectionMatrix(), r.config().width, r.config().height);
                if (screen.z < -1.0f || screen.z > 1.0f ||
                    screen.x < 0 || screen.x > r.config().width ||
                    screen.y < 0 || screen.y > r.config().height)
                    continue;
                 ColorF labelColor{1, 1, 1, 1};
                 if (obj.missionObjective)
                     labelColor = objectiveMarkerColor(obj.teamId, pl ? pl->team() : 0, true);
                 font->render(obj.label.c_str(), screen.x - 35, screen.y - 12,
                     labelColor, 1.0f);
            }
        }
    }

    const bool liveObserver = activeConn && activeConn->isConnected() && activeConn->isObserverMode();
    if (hud && !mapperMode && (gameState == Playing ||
                               (gameState == Dead && !demoPlaying)))
        hud->render(this);

    // Connection status overlay
    if (cfg.online && activeConn && activeConn->isConnected() && liveGhosts.size() == 0) {
        auto* font = r.getFont();
        if (font) font->render("Receiving game data...", 20, 100, {1, 1, 0, 1}, 2.0f);
    }
    if (gameState == Dead && cfg.online) {
        auto* font = r.getFont();
        if (font) {
            char buf[64];
            snprintf(buf, sizeof(buf), "SPECTATOR [%s]", freeCamActive ? "Free Cam" : "Follow");
            font->render(buf, 20, 20, {0, 1, 1, 1}, 2.0f);
            font->render("Fire/Reload: Cycle  F1: Free Cam", 20, 45, {0.5f, 0.8f, 1, 1}, 1.5f);
        }
    }

    // Render test shape (loaded via testshape command)
    if (testShapeLoaded && testShape.loaded) {
        auto* defShader = ShaderManager::getDefaultShader();
        defShader->bind();
        auto& plat = Engine::instance().platform();

        // Compute model bounds once to frame the camera
        static bool boundsInit = false;
        static Point3F center{0,0,0};
        static float fitScale = 1.0f;
        if (!boundsInit) {
            Point3F mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            for (auto& m : testShape.meshes)
                for (auto& v : m.vertices) {
                    if (v.pos.x < mn.x) mn.x = v.pos.x; if (v.pos.y < mn.y) mn.y = v.pos.y; if (v.pos.z < mn.z) mn.z = v.pos.z;
                    if (v.pos.x > mx.x) mx.x = v.pos.x; if (v.pos.y > mx.y) mx.y = v.pos.y; if (v.pos.z > mx.z) mx.z = v.pos.z;
                }
            center = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
            float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
            float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
            fitScale = (radius > 1e-3f) ? (1.0f / radius) : 1.0f;
            boundsInit = true;
        }

        static float viewYaw = 0.6f, viewPitch = 0.25f;
        if (plat.input().mouseButtons[1]) {
            viewYaw   += plat.input().mouseDeltaX * 0.005f;
            viewPitch += plat.input().mouseDeltaY * 0.005f;
            if (viewPitch > 1.5f) viewPitch = 1.5f;
            if (viewPitch < -1.5f) viewPitch = -1.5f;
        }
        MatrixF ry; ry.setRotationY(viewYaw);
        MatrixF rx; rx.setRotationX(viewPitch);
        MatrixF sc; sc.setScale({fitScale, fitScale, fitScale});
        MatrixF tr; tr.setTranslation({-center.x, -center.y, -center.z});
        MatrixF model = ry * rx * testShape.upOrientation() * sc * tr;
        r.setModel(model);

        // Framing camera looking at the (now origin-centered, unit-radius) model
        r.setCamera({0, 0, 2.6f}, {0, 0, 0}, {0, 1, 0});

        testShape.render(0);
    }

    // Render shape viewer (shapeviewer command)
    if (shapeViewerActive && shapeViewerShape.loaded) {
        auto* defShader = ShaderManager::getDefaultShader();
        defShader->bind();
        auto& plat = Engine::instance().platform();

        // Compute model bounds to frame the camera (reset when shape changes)
        // For skeletal models, transform vertices through node world transforms
        if (!shapeViewerBoundsInit) {
            Point3F mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            const auto& nodeWorld = shapeViewerShape.defaultTransforms;
            // Only use detail level 0 meshes for bounds computation
            std::vector<int32_t> boundMeshes;
            if (!shapeViewerShape.details.empty() && !shapeViewerShape.details[0].meshIndices.empty()) {
                boundMeshes = shapeViewerShape.details[0].meshIndices;
            } else {
                boundMeshes.resize(shapeViewerShape.meshes.size());
                for (int32_t i = 0; i < (int32_t)boundMeshes.size(); i++) boundMeshes[i] = i;
            }
            for (int32_t mi : boundMeshes) {
                if (mi < 0 || mi >= (int32_t)shapeViewerShape.meshes.size()) continue;
                auto& m = shapeViewerShape.meshes[mi];
                // Get the node world transform for this mesh
                MatrixF nodeXform;
                nodeXform.identity();
                if (m.nodeIndex >= 0 && m.nodeIndex < (int)nodeWorld.size())
                    nodeXform = nodeWorld[m.nodeIndex];
                // Sample a fraction of vertices for bounds (avoid scanning all)
                int step = std::max(1, (int)m.vertices.size() / 16);
                for (size_t vi = 0; vi < m.vertices.size(); vi += step) {
                    Point3F wp = nodeXform.transform(m.vertices[vi].pos);
                    if (wp.x < mn.x) mn.x = wp.x; if (wp.y < mn.y) mn.y = wp.y; if (wp.z < mn.z) mn.z = wp.z;
                    if (wp.x > mx.x) mx.x = wp.x; if (wp.y > mx.y) mx.y = wp.y; if (wp.z > mx.z) mx.z = wp.z;
                }
            }
            shapeViewerCenter = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
            float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
            float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
            shapeViewerFitScale = (radius > 1e-3f) ? (1.0f / radius) : 1.0f;
            shapeViewerBoundsInit = true;
        }

        // Mouse orbit (left button)
        if (plat.input().mouseButtons[1]) {
            shapeViewerYaw   += plat.input().mouseDeltaX * 0.005f;
            shapeViewerPitch += plat.input().mouseDeltaY * 0.005f;
            if (shapeViewerPitch > 1.5f) shapeViewerPitch = 1.5f;
            if (shapeViewerPitch < -1.5f) shapeViewerPitch = -1.5f;
        }
        MatrixF ry; ry.setRotationY(shapeViewerYaw);
        MatrixF rx; rx.setRotationX(shapeViewerPitch);
        MatrixF sc; sc.setScale({shapeViewerFitScale, shapeViewerFitScale, shapeViewerFitScale});
        MatrixF tr; tr.setTranslation({-shapeViewerCenter.x, -shapeViewerCenter.y, -shapeViewerCenter.z});
        // Orbit in Y-up: ry*rx rotate in Y-up, then C converts Z-up->Y-up (if needed), then sc*tr center/scale in Z-up
        MatrixF model = ry * rx * shapeViewerShape.upOrientation() * sc * tr;
        r.setModel(model);

        r.setCamera({0, 0, 2.6f}, {0, 0, 0}, {0, 1, 0});

        bool animated = false;
        const char* svAnim = getenv("SV_ANIM");
        const char* svStatic = getenv("SV_STATIC");
        if (!svStatic && !shapeViewerShape.animations.empty()) {
            // Allow selecting a specific animation via SV_ANIM env var
            int animIdx = 0;
            if (svAnim) {
                for (size_t ai = 0; ai < shapeViewerShape.animations.size(); ai++)
                    if (shapeViewerShape.animations[ai].name == svAnim) { animIdx = (int)ai; break; }
            }
            const auto& anim = shapeViewerShape.animations[animIdx];
            float animTime = shapeViewerAnimTime;
            if (const char* svTime = getenv("SV_TIME"))
                animTime = atof(svTime) * anim.duration;
            shapeViewerShape.renderAnimation(anim.name.c_str(), fmodf(animTime, anim.duration));
            animated = true;
            shapeViewerAnimTime += dt;
        }
        if (!animated) shapeViewerShape.render(0);
        // Auto-screenshot + exit on first frame (for headless testing)
        if (const char* svShot = getenv("SV_SHOT")) {
            Engine::instance().renderer().screenshot(svShot);
            Console::instance().printf(LogLevel::Info, "SV shot saved: %s", svShot);
            Engine::instance().quit();
        }

        // HUD overlay
        auto* font = r.getFont();
        if (font) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[%d/%zu] %s", shapeViewerIndex + 1, shapeViewerFiles.size(),
                shapeViewerFiles[shapeViewerIndex].c_str());
            font->render(buf, 20, 20, {1, 1, 0, 1}, 1.5f);

            if (!shapeViewerShape.animations.empty()) {
                for (size_t ai = 0; ai < shapeViewerShape.animations.size(); ai++) {
                    const auto& a = shapeViewerShape.animations[ai];
                    snprintf(buf, sizeof(buf), "Anim[%zu]: %s (%.1fs)%s", ai, a.name.c_str(), a.duration,
                        (svAnim && a.name == svAnim) ? " <<<" : "");
                    font->render(buf, 20, 45 + (int)ai * 25, {0.7f, 0.9f, 1, 1}, 1.2f);
                }
            }
            snprintf(buf, sizeof(buf), "Left/Right: cycle  Mouse: orbit  Esc: exit");
            font->render(buf, 20, 65, {0.5f, 0.7f, 0.8f, 1}, 1.0f);
        }
    }

    // Render demo ghost objects as 3D shapes
    if (demoPlaying && demoParser) {
        // Ensure default shader is bound for ghost rendering
        auto* defShader = ShaderManager::getDefaultShader();
        if (defShader) defShader->bind();

        const GhostTracker& gt = demoParser->getGhostTracker();
        std::vector<int> indices = gt.getAllIndices();
        w->beginProjectileTrailSync();
        for (int idx : indices) {
            const GhostEntry* g = gt.getGhost(idx);
            if (!g) continue;

            // Resolve player name from skin name if not already set
            if (g->playerName.empty() && !g->skinName.empty()) {
                const std::string& pn = demoParser->getPlayerNameForSkin(g->skinName);
                if (!pn.empty()) {
                    GhostEntry* mg = const_cast<GhostEntry*>(g);
                    mg->playerName = pn;
                }
            }
            Vec3 p = g->position;
            if (p.x == 0 && p.y == 0 && p.z == 0) continue; // no position data yet

            // Skip world-level objects already rendered by World
            if (!isRenderableGhostClass(g->className)) continue;

            // Interpolate position for smooth rendering (applies to both shape and fallback paths)
            GhostEntry* mg = const_cast<GhostEntry*>(g);
            Vec3 rp = p;
            if (!mg->hasRendered) {
                mg->renderPos = p;
                mg->renderRotation = g->rotation;
                mg->prevPosition = p;
            } else {
                float lerpFactor = 1.0f - expf(-12.0f * demoInterpolationDt);
                mg->renderPos.x += (p.x - mg->renderPos.x) * lerpFactor;
                mg->renderPos.y += (p.y - mg->renderPos.y) * lerpFactor;
                mg->renderPos.z += (p.z - mg->renderPos.z) * lerpFactor;

                // Interpolate rotation via exponential smoothing + renormalization
                if (g->hasRotation) {
                    Vec4 target = g->rotation;
                    float dot = mg->renderRotation.x * target.x +
                                mg->renderRotation.y * target.y +
                                mg->renderRotation.z * target.z +
                                mg->renderRotation.w * target.w;
                    if (dot < 0) { target.x = -target.x; target.y = -target.y; target.z = -target.z; target.w = -target.w; }
                    mg->renderRotation.x += (target.x - mg->renderRotation.x) * lerpFactor;
                    mg->renderRotation.y += (target.y - mg->renderRotation.y) * lerpFactor;
                    mg->renderRotation.z += (target.z - mg->renderRotation.z) * lerpFactor;
                    mg->renderRotation.w += (target.w - mg->renderRotation.w) * lerpFactor;
                    float invLen = 1.0f / sqrtf(mg->renderRotation.x * mg->renderRotation.x +
                                                 mg->renderRotation.y * mg->renderRotation.y +
                                                 mg->renderRotation.z * mg->renderRotation.z +
                                                 mg->renderRotation.w * mg->renderRotation.w);
                    mg->renderRotation.x *= invLen; mg->renderRotation.y *= invLen;
                    mg->renderRotation.z *= invLen; mg->renderRotation.w *= invLen;
                }
            }
            rp = mg->renderPos;

             const bool isProjectile = (g->className.find("Projectile") != std::string::npos ||
                 g->className == "EnergyBolt" || g->className == "LinearFlare" ||
                 g->className.find("Tracer") != std::string::npos);
             const V12::DecodedDataBlock* visualData = nullptr;
              bool hasBaseEmitter = false;
             if (isProjectile && g->hasDatablock) {
                 const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                 auto projectileIt = dataBlocks.find((uint32_t)g->datablockId);
                  if (projectileIt != dataBlocks.end()) {
                      const auto& projectileData = projectileIt->second.decoded;
                      visualData = &projectileData;
                     hasBaseEmitter = projectileData.projectileBaseEmitterRef != 0;
                     if (projectileData.hasProjectileScale) {
                         mg->projectileScale = {
                             std::isfinite(projectileData.projectileScale.x) && projectileData.projectileScale.x > 0.0f
                                 ? projectileData.projectileScale.x : 1.0f,
                             std::isfinite(projectileData.projectileScale.y) && projectileData.projectileScale.y > 0.0f
                                 ? projectileData.projectileScale.y : 1.0f,
                             std::isfinite(projectileData.projectileScale.z) && projectileData.projectileScale.z > 0.0f
                                 ? projectileData.projectileScale.z : 1.0f};
                         mg->hasProjectileScale = true;
                     } else {
                         mg->projectileScale = {1.0f, 1.0f, 1.0f};
                         mg->hasProjectileScale = false;
                     }
                     w->syncProjectileTrail(idx, Math::torquePointToYUp({rp.x, rp.y, rp.z}),
                        Math::torquePointToYUp({g->velocity.x, g->velocity.y, g->velocity.z}),
                        &projectileData, &dataBlocks);
                }
            }

            // Compute velocity from interpolated position (smooth)
            float dx = rp.x - mg->prevPosition.x;
            float dz = rp.z - mg->prevPosition.z;
            float dist = sqrtf(dx*dx + dz*dz);
            float speed = (dt > 0.001f) ? dist / dt : 0;
            mg->isMoving = (speed > 0.1f);
            if (mg->isMoving) {
                mg->moveYaw = atan2f(dx, dz);
                mg->animTime += demoInterpolationDt;
            } else {
                mg->animTime = fmodf(mg->animTime, 10.0f) + demoInterpolationDt * 0.3f; // slow idle
            }
            mg->threadAnimTime += demoInterpolationDt;
             mg->prevPosition = rp;
             mg->hasRendered = true;
              applyShapeBaseAudio(*mg, idx, p);

               if (visualData && visualData->projectileMaterial != V12::DecodedDataBlock::ProjectileMaterial::None) {
                   const auto& data = *visualData;
                   mg->projectileVisualAge += std::max(0.0f, dt);
                   const float fade = projectileVisualFade(data, mg->projectileVisualAge);
                   ColorF color{data.projectileMaterialColor[0], data.projectileMaterialColor[1],
                                data.projectileMaterialColor[2], data.projectileMaterialColor[3] *
                                projectileMaterialAlpha(data) * fade};
                   const bool additive = projectileMaterialAdditive(data);
                   std::vector<uint32_t> materialTextures;
                  for (const auto& name : data.projectileMaterialTextures) {
                      std::vector<uint32_t> frames;
                      std::vector<float> durations;
                      r.loadTextureFrames(name.c_str(), frames, durations);
                      materialTextures.push_back(frames.empty() ? UINT32_MAX : frames.front());
                  }
                   const auto texture = [&](size_t index) {
                       return projectileMaterialTexture(materialTextures, index);
                   };
                   const Point3F materialPos = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                   const Point3F velocity = Math::torquePointToYUp(
                       {g->velocity.x, g->velocity.y, g->velocity.z});
                   const auto layers = projectileVisualLayers(data, mg->projectileVisualAge);
                   for (const auto& layer : layers) {
                       if (layer.alpha <= 0.0f) continue;
                       ColorF layerColor = color;
                       layerColor.a *= std::clamp(layer.alpha, 0.0f, 1.0f);
                        const bool drawTracer = (data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::LinearFlare ||
                            data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Cross) &&
                            data.projectileTracerLength > 0.0f;
                        if (drawTracer) {
                           Point3F direction = velocity;
                           const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
                           if (length > 0.001f) {
                               direction.x /= length; direction.y /= length; direction.z /= length;
                           } else direction = {0, 0, 1};
                           const float trailLength = data.projectileTracerLength * (1.0f + 0.35f * (float)(&layer - layers.data()));
                           const Point3F start{materialPos.x - direction.x * trailLength,
                                               materialPos.y - direction.y * trailLength,
                                               materialPos.z - direction.z * trailLength};
                           const auto quad = projectileBeamQuad(start, materialPos, r.cameraPos,
                               std::max(0.01f, layer.height));
                           if (quad.size() == 4)
                               r.drawTexturedQuad(quad[0], quad[1], quad[2], quad[3],
                                                  texture(layer.textureIndex), layerColor, 0, 0, 1, 1, additive);
                       }
                       const bool drawCross = data.projectileMaterial != V12::DecodedDataBlock::ProjectileMaterial::Cross ||
                           data.projectileRenderCross || data.projectileTracerLength <= 0.0f;
                       if (drawCross) {
                           const float size = data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Cross
                               ? std::max(0.01f, data.projectileCrossSize > 0.0f ? data.projectileCrossSize : layer.width)
                               : layer.width;
                           r.drawOrientedSprite(materialPos, size, layerColor, velocity, layer.angle,
                                                texture(layer.textureIndex), additive);
                       }
                   }
                  if (data.projectileHasLight)
                      r.drawSprite(Math::torquePointToYUp({rp.x, rp.y, rp.z}),
                                   std::clamp(data.projectileLightRadius, 0.05f, 64.0f),
                                   {data.projectileLightColor[0], data.projectileLightColor[1],
                                    data.projectileLightColor[2], 0.22f}, true);
              }

               if (demoParser && (g->className == "ELFProjectile" || g->className == "RepairProjectile")) {
                 const GhostEntry* source = demoParser->getGhostTracker().getGhost(g->linkSourceGhost);
                 const GhostEntry* target = demoParser->getGhostTracker().getGhost(g->linkTargetGhost);
                 if (source && target) {
                     Point3F start = Math::torquePointToYUp(
                         {source->renderPos.x, source->renderPos.y, source->renderPos.z});
                     Point3F end = Math::torquePointToYUp(
                         {target->renderPos.x, target->renderPos.y, target->renderPos.z});
                     start.y += 1.4f;
                     end.y += 1.0f;
                     const auto points = linkBeamPoints(start, end, g->className == "ELFProjectile");
                     const ColorF color = g->className == "ELFProjectile"
                         ? ColorF{0.25f, 0.75f, 1.0f, 0.9f}
                         : ColorF{1.0f, 0.2f, 0.2f, 0.75f};
                     r.drawLineStrip(points, color);
                     r.drawSprite(end, g->className == "ELFProjectile" ? 0.5f : 0.6f,
                                  color, 0, true);
                 }
                  continue;
              }

               const bool stockBeam = g->className == "ShockLanceProjectile" ||
                   g->className == "SniperProjectile" ||
                   g->className == "TracerProjectile" ||
                   g->className == "LinearFlareProjectile";
              if (stockBeam && g->hasBeam) {
                  const Point3F start = Math::torquePointToYUp(
                      {g->beamStart.x, g->beamStart.y, g->beamStart.z});
                  const Point3F end = Math::torquePointToYUp(
                      {g->beamEnd.x, g->beamEnd.y, g->beamEnd.z});
                  const ColorF color = g->className == "ShockLanceProjectile"
                      ? ColorF{0.35f, 0.8f, 1.0f, 0.9f}
                      : g->className == "TracerProjectile"
                          ? ColorF{1.0f, 0.65f, 0.2f, 0.85f}
                          : ColorF{1.0f, 0.85f, 0.25f, 0.85f};
                  const auto quad = projectileBeamQuad(start, end,
                      r.cameraPos, 0.08f);
                  std::vector<Point3F> outline{quad.front()};
                  if (quad.size() == 4) {
                      outline.push_back(quad[1]); outline.push_back(quad[2]);
                      outline.push_back(quad[3]); outline.push_back(quad.front());
                  } else {
                      outline.push_back(quad.back());
                  }
                  r.drawLineStrip(outline, color);
                  continue;
              }

             // Try to get or load the DTS shape for this ghost class
            DTSShape* shape = const_cast<DTSShape*>(g->shape);
             if (!shape && !isEffectOnlyGhostClass(g->className) && g->className.empty() == false) {
                GhostEntry* mutableG = const_cast<GhostEntry*>(g);
                std::string shapeRef = g->shapeName;
                if (shapeRef.empty() && g->hasDatablock && demoParser) {
                    const auto& dataBlocks = demoParser->getInitialBlock().datablockWeaponShapes;
                    auto shapeIt = dataBlocks.find((uint32_t)g->datablockId);
                    if (shapeIt != dataBlocks.end()) shapeRef = normalizeShapePath(shapeIt->second);
                }
                mutableG->shape = getOrLoadDemoShape(g->className, g->skinName, shapeRef);
                shape = mutableG->shape;
            }

            if (shape && shape->loaded) {
                // Apply skin textures on first render (player ghosts only)
                if (!mg->skinApplied && !g->skinName.empty() &&
                    (g->className == "Player" || g->className == "MPB")) {
                    if (shape->applySkin(g->skinName))
                        Console::instance().printf(LogLevel::Debug,
                            "Ghost[%d]: applied skin '%s' to shape '%s'",
                            idx, g->skinName.c_str(), shape->name.c_str());
                    mg->skinApplied = true;
                }

                // Build model matrix
                bool isPlayer = (g->className == "Player" || g->className == "MPB");
                bool isVehicle = (g->className.find("Vehicle") != std::string::npos ||
                                  g->className == "Shrike" || g->className == "Turbograv" ||
                                  g->className == "Shield" || g->className == "Wildcat");
                MatrixF model;
                if (g->hasRotation) {
                    QuatF q(mg->renderRotation.x, mg->renderRotation.y, mg->renderRotation.z, mg->renderRotation.w);
                    model = Math::torqueQuaternionToYUp(q);
                } else if (mg->isMoving || isPlayer) {
                    float yaw = mg->moveYaw;
                    model.setRotationAxis({0, 1, 0}, -yaw);
                } else {
                    model.identity();
                }
                 if (shape->nativeDTS) {
                    MatrixF shapeFrame;
                    shapeFrame.setRotationY(Math::PI);
                     model = model * shapeFrame;
                 }
                 if (isProjectile && mg->hasProjectileScale)
                     model.setScale({mg->projectileScale.x, mg->projectileScale.y,
                                     mg->projectileScale.z});
                // Hover bob for stationary vehicles
                float hoverY = 0.0f;
                if (isVehicle && !mg->isMoving) {
                    hoverY = sinf(demoTime * 2.0f + idx * 1.7f) * 0.15f;
                }
                Point3F renderPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                model.setTranslation({renderPosition.x, renderPosition.y + hoverY, renderPosition.z});
                r.setModel(model * shape->upOrientation());

                // Appearance comes from native material and skin data only.
                if (defShader) defShader->setUniform("uTint", ColorF{1, 1, 1, 1});

                // ShapeBase replaces the normal material map with cloakTexture.
                Texture* cloakTexture = nullptr;
                if (g->cloaked && g->hasDatablock && demoParser) {
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = blocks.find((uint32_t)g->datablockId);
                    if (block != blocks.end() && !block->second.decoded.cloakTexture.empty()) {
                        const std::string& path = block->second.decoded.cloakTexture;
                        cloakTexture = r.loadTexture(path.c_str());
                        if (!cloakTexture) cloakTexture = r.loadTexture(("textures/" + path).c_str());
                        if (!cloakTexture) cloakTexture = r.loadTexture(("textures/" + path + ".png").c_str());
                    }
                    if (!cloakTexture) cloakTexture = r.loadTexture("textures/special/cloakTexture.png");
                }
                shape->cloakTextureOverride = cloakTexture && cloakTexture->loaded ? cloakTexture : nullptr;

                // Apply cloak transparency
                if (g->cloaked) {
                    // Screen-door transparency: alternate pixels are discarded
                    if (defShader) defShader->setUniform("uScreenDoor", 0.5f);
                } else {
                    if (defShader) defShader->setUniform("uScreenDoor", 0.0f);
                }

                // Select the sequence by the index transmitted in the
                // ShapeBase thread state. The DTS owns its sequence names.
                const DTSShape::Animation* animation = nullptr;
                float animationPosition = 0.0f;
                bool isTurret = (g->className == "Turret" || g->className == "Sentry");
                for (const auto& thread : g->threads) {
                    if (!thread.valid || thread.sequence < 0 ||
                        thread.sequence >= (int)shape->animations.size()) continue;
                    if (thread.state == 1 || thread.state == 3) continue;
                    animation = &shape->animations[thread.sequence];
                    const float threadTime = dtsThreadTime(thread.position, animation->duration,
                        mg->threadAnimTime, thread.timescale, thread.atEnd, thread.forward);
                    animationPosition = animation->duration > 0.0f
                        ? threadTime / animation->duration : 0.0f;
                    if (thread.atEnd) animationPosition = thread.forward ? 1.0f : 0.0f;
                    break;
                }
                if (g->damageState >= 2) {
                    for (const char* name : {"hulk", "destroyed", "wreck", "dead"}) {
                        if (const auto* damageAnimation = findAnimation(*shape, name)) {
                            animation = damageAnimation;
                            animationPosition = 1.0f;
                            break;
                        }
                    }
                }

                // Node overrides for turret barrel and player head
                 DTSShape::NodeOverride overrides[8];
                 int numOverrides = 0;
                if (isTurret && shape && (mg->barrelPitch != 0.0f || mg->barrelYaw != 0.0f)) {
                    int barrelNode = shape->findNode("barrel");
                    if (barrelNode < 0) barrelNode = shape->findNode("mount0");
                    if (barrelNode >= 0) {
                        overrides[numOverrides].nodeIndex = barrelNode;
                        if (barrelNode < (int)shape->defaultTransforms.size())
                            overrides[numOverrides].transform = shape->defaultTransforms[barrelNode];
                        else
                            overrides[numOverrides].transform.identity();
                        MatrixF pitchMat, yawMat;
                        pitchMat.setRotationAxis({1, 0, 0}, -mg->barrelPitch);
                        yawMat.setRotationAxis({0, 1, 0}, -mg->barrelYaw);
                        overrides[numOverrides].transform = overrides[numOverrides].transform * yawMat * pitchMat;
                        numOverrides++;
                     }
                 }
                 appendWheelNodeOverrides(*g, *shape, dt, overrides, numOverrides,
                                          8, mg->wheelRotation);
                 // Player head aim direction
                if (isPlayer && shape && (mg->headPitch != 0.0f || mg->headYaw != 0.0f)) {
                    int headNode = shape->findNode("head");
                    if (headNode < 0) headNode = shape->findNode("mount4");
                     if (headNode >= 0 && numOverrides < 8) {
                        overrides[numOverrides].nodeIndex = headNode;
                        if (headNode < (int)shape->defaultTransforms.size())
                            overrides[numOverrides].transform = shape->defaultTransforms[headNode];
                        else
                            overrides[numOverrides].transform.identity();
                        MatrixF pitchMat, yawMat;
                        pitchMat.setRotationAxis({1, 0, 0}, -mg->headPitch);
                        yawMat.setRotationAxis({0, 0, 1}, -mg->headYaw);
                        overrides[numOverrides].transform = overrides[numOverrides].transform * yawMat * pitchMat;
                        numOverrides++;
                    }
                }

                if (!w->isPositionVisible({rp.x, rp.y, rp.z}, camPos))
                    continue;

                if (animation) {
                    shape->renderAnimation(animation->name.c_str(),
                                           animationPosition * animation->duration,
                                           numOverrides > 0 ? overrides : nullptr,
                                           numOverrides);
                } else {
                    shape->render(0, numOverrides > 0 ? overrides : nullptr, numOverrides);
                }
                shape->cloakTextureOverride = nullptr;

                // Render mounted weapons for player ghosts
                if (isPlayer) {
                    for (int img = 0; img < 8; img++) {
                        int16_t dbId = g->mountedImages[img].datablockId;
                        if (dbId < 0) continue;
                        // Weapon images must resolve from the streamed datablock.
                        const char* wPath = nullptr;
                        std::string dynamicPath;
                        if (demoParser) {
                            const auto& ib = demoParser->getInitialBlock();
                            auto wit = ib.datablockWeaponShapes.find(dbId);
                             if (wit != ib.datablockWeaponShapes.end()) {
                                 dynamicPath = wit->second;
                                 wPath = dynamicPath.c_str();
                                 const auto db = ib.dataBlocks.find((uint32_t)dbId);
                                 if (db != ib.dataBlocks.end() && db->second.decoded.hasMountPoint)
                                     mg->mountedImages[img].mountPoint =
                                         (int)db->second.decoded.mountPoint;
                             }
                        }
                        if (!wPath) continue;

                        // Load weapon shape (cached)
                        static std::unordered_map<std::string, DTSShape> weaponCache;
                        auto it = weaponCache.find(wPath);
                        DTSShape* wShape = nullptr;
                        if (it != weaponCache.end()) {
                            wShape = &it->second;
                        } else {
                            auto& fs = Engine::instance().fs();
                            auto data = fs.read(wPath);
                            if (!data.empty()) {
                                auto& entry = weaponCache[wPath];
                                entry.name = wPath;
                                entry.load(data.data(), data.size());
                                if (entry.loaded) wShape = &entry;
                            }
                        }
                        if (!wShape || !wShape->loaded) continue;

                        // Torque mounts the image's Mountpoint node to the
                        // owning shape's mountN node. The image datablock's
                        // mountPoint, rather than the image slot, selects N.
                        MatrixF mountedModel = model * shape->upOrientation();
                        const int mountPoint = mg->mountedImages[img].mountPoint;
                        std::string mountName = "mount" + std::to_string(mountPoint);
                        int mountNode = shape->findNode(mountName.c_str());
                        if (mountNode < 0 && mountPoint == 0) mountNode = shape->findNode("rhand");
                        if (mountNode >= 0 && mountNode < (int)shape->defaultTransforms.size())
                            mountedModel = mountedModel * shape->defaultTransforms[mountNode];
                        const int imageMount = wShape->findNode("Mountpoint");
                        if (imageMount >= 0 && imageMount < (int)wShape->defaultTransforms.size())
                            mountedModel = mountedModel * wShape->defaultTransforms[imageMount].inverse();
                         MatrixF imageModel = mountedModel * wShape->upOrientation();
                         r.setModel(imageModel);
                         wShape->cloakTextureOverride = shape->cloakTextureOverride;
                         const DTSShape::Animation* imageAnimation = nullptr;
                         for (const char* name : {mg->mountedImages[img].isFiring ? "fire" : "idle",
                                                  "ambient", "spin", "stand"}) {
                             imageAnimation = findAnimation(*wShape, name);
                             if (imageAnimation) break;
                         }
                         if (imageAnimation && imageAnimation->duration > 0.0f)
                             wShape->renderAnimation(imageAnimation->name.c_str(),
                                                     fmodf(mg->threadAnimTime, imageAnimation->duration));
                         else
                             wShape->render(0);
                         wShape->cloakTextureOverride = nullptr;

                        // Muzzle flash and particles when firing
                         if (mg->mountedImages[img].isFiring) {
                             Point3F muzzlePos = mountedNodePosition(imageModel, *wShape, "Mountpoint");
                            float flashSize = 0.15f;
                            ColorF flashCol = {1.0f, 0.9f, 0.5f, 0.9f};
                            r.drawSprite(muzzlePos, flashSize, flashCol);
                            // Spawn a few spark particles
                            for (int s = 0; s < 3; s++) {
                                World::Particle spark;
                                spark.pos = muzzlePos;
                                float spread = 0.3f;
                                spark.vel = {
                                    ((float)std::rand() / RAND_MAX - 0.5f) * spread,
                                    ((float)std::rand() / RAND_MAX) * spread * 0.5f,
                                    ((float)std::rand() / RAND_MAX - 0.5f) * spread
                                };
                                spark.lifetime = 0.1f + ((float)std::rand() / RAND_MAX) * 0.15f;
                                spark.maxLifetime = spark.lifetime;
                                spark.size = 0.05f + ((float)std::rand() / RAND_MAX) * 0.05f;
                                spark.color = {1.0f, 0.8f, 0.3f, 1.0f};
                                spark.active = true;
                                if (w->particles.size() < 1000) w->particles.push_back(spark);
                            }
                        }
                    }
                }
            }

            // Shield effect: render a pulsing translucent bubble when shielded
            if (mg->shieldLevel > 0.01f) {
                float sAlpha = mg->shieldLevel * 0.25f;
                bool isPlr = (g->className == "Player" || g->className == "MPB");
                float sSize = isPlr ? 1.0f : 1.8f;
                float pulse = sinf(demoTime * 6.0f) * 0.08f + 0.92f;
                sAlpha *= pulse;
                ColorF shieldCol = {0.3f, 0.6f, 1.0f, sAlpha};
                // Render layered boxes at different scales for sphere approximation
                Point3F shieldPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                for (int i = 0; i < 3; i++) {
                    float scale = 1.0f - i * 0.15f;
                    float a = sAlpha * (1.0f - i * 0.25f);
                    r.drawBox({{shieldPosition.x - sSize * scale, shieldPosition.y - sSize * scale, shieldPosition.z - sSize * scale},
                               {shieldPosition.x + sSize * scale, shieldPosition.y + sSize * scale, shieldPosition.z + sSize * scale}},
                              {0.3f, 0.6f, 1.0f, a});
                }
            }

            // Ground shadow for all renderable ghosts
            if (isRenderableGhostClass(g->className)) {
                Point3F shadowPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                float groundH = 0.0f;
                if (w->terrain() && w->terrain()->loaded)
                    groundH = w->getHeight(shadowPosition.x, shadowPosition.z);
                float shadowY = std::max(groundH, 0.0f);
                float shadowSize = (g->className == "Player" || g->className == "MPB") ? 0.8f : 1.5f;
                float distAboveGround = shadowPosition.y - shadowY;
                if (distAboveGround > 0 && distAboveGround < 50.0f) {
                    float shadowAlpha = std::max(0.05f, 0.4f - distAboveGround * 0.008f);
                    r.drawBox({{shadowPosition.x - shadowSize, shadowY + 0.1f, shadowPosition.z - shadowSize},
                               {shadowPosition.x + shadowSize, shadowY + 0.1f, shadowPosition.z + shadowSize}},
                              {0, 0, 0, shadowAlpha});
                }
            }

            // Highlight ring for control object (recording player)
            if (idx == controlGhostIndex) {
                float pulse = sinf(demoTime * 4.0f) * 0.3f + 0.7f;
                Point3F ringPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                float ringY = ringPosition.y - 0.5f;
                float ringR = 1.2f + pulse * 0.3f;
                int segments = 20;
                std::vector<Point3F> ring;
                for (int i = 0; i <= segments; i++) {
                    float a = (float)i / (float)segments * 6.28318f;
                    ring.push_back({ringPosition.x + cosf(a) * ringR, ringY, ringPosition.z + sinf(a) * ringR});
                }
                r.drawLineStrip(ring, {0.3f, 1.0f, 0.5f, 0.7f + pulse * 0.3f});
            }

            // Update projectile trail
            if (isProjectile) {
                // Color by projectile type
                ColorF trailCol = {0.5f, 1.0f, 1.0f, 1.0f}; // default cyan
                if (g->className.find("Grenade") != std::string::npos)
                    trailCol = {0.3f, 1.0f, 0.3f, 1.0f}; // green
                else if (g->className.find("Seeker") != std::string::npos)
                    trailCol = {1.0f, 0.6f, 0.1f, 1.0f}; // orange
                else if (g->className.find("Linear") != std::string::npos || g->className.find("Sniper") != std::string::npos)
                    trailCol = {0.2f, 0.8f, 1.0f, 1.0f}; // cyan
                else if (g->className.find("Bomb") != std::string::npos)
                    trailCol = {1.0f, 0.3f, 0.1f, 1.0f}; // red-orange
                else if (g->className.find("Shock") != std::string::npos)
                    trailCol = {0.8f, 0.2f, 1.0f, 1.0f}; // purple
                if (hasBaseEmitter) continue;
                auto& trail = demoTrails[idx];
                Point3F trailPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                trail.push_back({trailPosition.x, trailPosition.y, trailPosition.z, 1.0f, trailCol});
                if (trail.size() > 30) trail.erase(trail.begin());
            }
        }
        w->endProjectileTrailSync();

        // A deleted projectile can leave a few interpolated points behind;
        // discard them immediately instead of waiting for their fade timer.
        for (auto it = demoTrails.begin(); it != demoTrails.end();) {
            if (!gt.hasGhost(it->first)) it = demoTrails.erase(it);
            else ++it;
        }

        // Render projectile trails
        if (!demoTrails.empty()) {
            for (auto it = demoTrails.begin(); it != demoTrails.end(); ) {
                auto& pts = it->second;
                // Age and cull trail points
                for (int i = (int)pts.size() - 1; i >= 0; i--) {
                    pts[i].life -= dt * 2.0f;
                    if (pts[i].life <= 0) pts.erase(pts.begin() + i);
                }
                if (pts.empty()) { it = demoTrails.erase(it); continue; } else { ++it; }
                // Draw trail as a fading line strip with glow
                if (pts.size() >= 2) {
                    // Use the first point's color as trail color
                    ColorF baseCol = pts[0].color;
                    float alpha = pts.back().life;
                    // Outer glow pass (thick, faint)
                    for (int g = 0; g < 3; g++) {
                        float expand = (3 - g) * 0.12f;
                        float ga = alpha * 0.12f * (3 - g);
                        std::vector<Point3F> glowPts;
                        for (auto& tp : pts) {
                            float t = &tp - &pts[0];
                            float ptAlpha = (t + 1.0f) / pts.size();
                            glowPts.push_back({tp.x, tp.y + expand * ptAlpha, tp.z});
                        }
                        r.drawLineStrip(glowPts, {baseCol.r, baseCol.g, baseCol.b, ga});
                    }
                    // Core line (bright, thin)
                    std::vector<Point3F> corePts;
                    for (auto& tp : pts) corePts.push_back({tp.x, tp.y, tp.z});
                    r.drawLineStrip(corePts, {baseCol.r * 0.7f + 0.3f, baseCol.g * 0.7f + 0.3f, baseCol.b * 0.7f + 0.3f, alpha * 0.9f});
                }
            }
        }

        // Spectator HUD: name tags and health bars above ghosts
        if (demoPlaying && demoParser) {
            auto* font = r.getFont();
            if (font) {
                int screenW = r.config().width, screenH = r.config().height;
                for (int idx : indices) {
                    const GhostEntry* g = gt.getGhost(idx);
                    if (!g) continue;
                    if (g->position.x == 0 && g->position.y == 0 && g->position.z == 0) continue;
                    if (!isRenderableGhostClass(g->className)) continue;
                    Point3F above = Math::torquePointToYUp({g->renderPos.x, g->renderPos.y, g->renderPos.z});
                    above.y += 2.5f;
                    Point3F screen = worldToScreen(above, r.viewMatrix(), r.projectionMatrix(), screenW, screenH);
                    if (screen.x < 0 || screen.x > screenW || screen.y < 0 || screen.y > screenH) continue;
                    ColorF col{1, 1, 1, 1};
                    std::string sn = g->skinName;
                    for (auto& c : sn) c = (char)tolower(c);
                    if (sn.find("red") != std::string::npos) col = {1, 0.2f, 0.2f, 1};
                    else if (sn.find("blue") != std::string::npos) col = {0.2f, 0.3f, 1, 1};
                    else if (sn.find("green") != std::string::npos) col = {0.2f, 0.8f, 0.2f, 1};
                    std::string label = g->className;
                    if (!g->playerName.empty()) label = g->playerName;
                    font->render(label.c_str(), screen.x - 30, screen.y - 20, col, 1.2f);
                    float barW = 50, barH = 6;
                    float bx = screen.x - barW/2;
                    float by = screen.y + 2;
                    r.drawBox({{bx-1, by-1, 0}, {bx+barW+1, by+barH+1, 0}}, {0, 0, 0, 0.6f});
                    float healthFrac = (g->health > 0) ? (g->health / 100.0f) : 1.0f;
                    ColorF healthCol = healthFrac > 0.5f ? ColorF{0, 1, 0, 0.8f} :
                                      healthFrac > 0.25f ? ColorF{1, 1, 0, 0.8f} : ColorF{1, 0, 0, 0.8f};
                    r.drawBox({{bx, by, 0}, {bx + barW * healthFrac, by + barH, 0}}, healthCol);
                    float ey2 = by + barH + 1;
                    float energyFrac = g->energy / 100.0f;
                    r.drawBox({{bx-1, ey2-1, 0}, {bx+barW+1, ey2+barH+1, 0}}, {0, 0, 0, 0.6f});
                    r.drawBox({{bx, ey2, 0}, {bx + barW * energyFrac, ey2 + barH, 0}}, {0.3f, 0.5f, 1, 0.8f});
                }
            }
        }
    }

    // Render live network ghosts (multiplayer)
    if (!demoPlaying && liveGhosts.size() > 0 && activeConn && activeConn->isConnected()) {
        auto* defShader = ShaderManager::getDefaultShader();
        if (defShader) defShader->bind();

        std::vector<int> indices = liveGhosts.getAllIndices();
         for (int idx : indices) {
             // Skip our own player ghost (we render locally via pl->render)
             if (serverPlayerGhostSynced && (uint32_t)idx == serverPlayerGhostIndex) continue;
             GhostEntry* g = liveGhosts.getMutableGhost(idx);
             if (!g) continue;
             if (activeConn->isObserverMode() &&
                 !isSensorGroupTargetVisible(activeConn->observerSnapshot().playerSensorGroup,
                                             g->sensorGroup)) continue;
            Vec3 p = g->position;
            if (p.x == 0 && p.y == 0 && p.z == 0) continue;
            if (!isRenderableGhostClass(g->className)) continue;

            // Smooth interpolation
            Vec3 rp = p;
            if (!g->hasRendered) {
                g->renderPos = p;
                g->renderRotation = g->rotation;
                g->prevPosition = p;
                g->hasRendered = true;
            } else {
                float lerpFactor = 1.0f - expf(-12.0f * dt);
                g->renderPos.x += (p.x - g->renderPos.x) * lerpFactor;
                g->renderPos.y += (p.y - g->renderPos.y) * lerpFactor;
                g->renderPos.z += (p.z - g->renderPos.z) * lerpFactor;

                // Interpolate rotation
                if (g->hasRotation) {
                    Vec4 target = g->rotation;
                    float dot = g->renderRotation.x * target.x +
                                g->renderRotation.y * target.y +
                                g->renderRotation.z * target.z +
                                g->renderRotation.w * target.w;
                    if (dot < 0) { target.x = -target.x; target.y = -target.y; target.z = -target.z; target.w = -target.w; }
                    g->renderRotation.x += (target.x - g->renderRotation.x) * lerpFactor;
                    g->renderRotation.y += (target.y - g->renderRotation.y) * lerpFactor;
                    g->renderRotation.z += (target.z - g->renderRotation.z) * lerpFactor;
                    g->renderRotation.w += (target.w - g->renderRotation.w) * lerpFactor;
                    float invLen = 1.0f / sqrtf(g->renderRotation.x * g->renderRotation.x +
                                                 g->renderRotation.y * g->renderRotation.y +
                                                 g->renderRotation.z * g->renderRotation.z +
                                                 g->renderRotation.w * g->renderRotation.w);
                    g->renderRotation.x *= invLen; g->renderRotation.y *= invLen;
                    g->renderRotation.z *= invLen; g->renderRotation.w *= invLen;
                }
            }
             rp = g->renderPos;
             g->threadAnimTime += dt;
             applyShapeBaseAudio(*g, idx, p);

            // Try to load a shape for this ghost class
             if (!g->shape && !isEffectOnlyGhostClass(g->className)) {
                g->shape = getOrLoadDemoShape(g->className, g->skinName, g->shapeName);
            }

            if (g->shape && g->shape->loaded) {
                MatrixF model;
                if (g->hasRotation) {
                    QuatF q(g->renderRotation.x, g->renderRotation.y, g->renderRotation.z, g->renderRotation.w);
                    model = Math::torqueQuaternionToYUp(q);
                }
              if (g->shape->nativeDTS) {
                    MatrixF shapeFrame;
                    shapeFrame.setRotationY(Math::PI);
                    model = model * shapeFrame;
              }
              const bool isVehicle = (g->className.find("Vehicle") != std::string::npos ||
                                      g->className == "Shrike" || g->className == "Turbograv" ||
                                      g->className == "Shield" || g->className == "Wildcat");
              Point3F renderPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
             model.setTranslation(renderPosition);
             r.setModel(model * g->shape->upOrientation());
             Texture* cloakTexture = nullptr;
             if (g->cloaked && g->hasDatablock) {
                 auto block = nativeDatablocks.find((uint32_t)g->datablockId);
                 if (block != nativeDatablocks.end() &&
                     !block->second.decoded.cloakTexture.empty()) {
                     const std::string& path = block->second.decoded.cloakTexture;
                     cloakTexture = r.loadTexture(path.c_str());
                     if (!cloakTexture) cloakTexture = r.loadTexture(("textures/" + path).c_str());
                     if (!cloakTexture) cloakTexture = r.loadTexture(("textures/" + path + ".png").c_str());
                 }
                 if (!cloakTexture) cloakTexture = r.loadTexture("textures/special/cloakTexture.png");
             }
             g->shape->cloakTextureOverride = cloakTexture && cloakTexture->loaded ? cloakTexture : nullptr;
             if (defShader) defShader->setUniform("uScreenDoor", g->cloaked ? 0.5f : 0.0f);
              DTSShape::NodeOverride overrides[8]{};
              int overrideCount = 0;
             if ((g->className == "Turret" || g->className == "Sentry") &&
                 g->hasTurretAim) {
                 int barrelNode = g->shape->findNode("barrel");
                 if (barrelNode < 0) barrelNode = g->shape->findNode("mount0");
                 if (barrelNode >= 0) {
                      overrides[overrideCount].nodeIndex = barrelNode;
                      overrides[overrideCount].transform = barrelNode < (int)g->shape->defaultTransforms.size()
                          ? g->shape->defaultTransforms[barrelNode] : MatrixF{};
                      MatrixF pitch, yaw;
                      pitch.setRotationAxis({1, 0, 0}, -g->barrelPitch);
                      yaw.setRotationAxis({0, 1, 0}, -g->barrelYaw);
                      overrides[overrideCount].transform = overrides[overrideCount].transform * yaw * pitch;
                      overrideCount = 1;
                  }
              }
              appendWheelNodeOverrides(*g, *g->shape, dt, overrides, overrideCount,
                                       8, g->wheelRotation);
               const DTSShape::Animation* animation = nullptr;
               float animationPosition = 0.0f;
               for (const auto& thread : g->threads) {
                   if (!thread.valid || thread.sequence < 0 ||
                       thread.sequence >= (int)g->shape->animations.size()) continue;
                   if (thread.state == 1 || thread.state == 3) continue;
                   animation = &g->shape->animations[thread.sequence];
                     const float threadTime = dtsThreadTime(thread.position, animation->duration,
                         g->threadAnimTime, thread.timescale, thread.atEnd, thread.forward);
                    animationPosition = animation->duration > 0.0f
                        ? threadTime / animation->duration : 0.0f;
                   if (thread.atEnd) animationPosition = thread.forward ? 1.0f : 0.0f;
                   break;
               }
               if (animation)
                   g->shape->renderAnimation(animation->name.c_str(),
                                             animationPosition * animation->duration,
                                             overrideCount ? overrides : nullptr, overrideCount);
               else
                   g->shape->render(0, overrideCount ? overrides : nullptr, overrideCount);
               g->shape->cloakTextureOverride = nullptr;

             // ShapeBase images are mounted on every owning shape, not only players.
             static std::unordered_map<std::string, DTSShape> liveImageCache;
             for (int img = 0; img < 8; ++img) {
                 const auto& mounted = g->mountedImages[img];
                 if (mounted.shapePath.empty()) continue;
                 auto imageIt = liveImageCache.find(mounted.shapePath);
                 DTSShape* imageShape = imageIt == liveImageCache.end() ? nullptr : &imageIt->second;
                 if (!imageShape) {
                     auto data = Engine::instance().fs().read(mounted.shapePath.c_str());
                     if (data.empty()) continue;
                     auto& entry = liveImageCache[mounted.shapePath];
                     entry.name = mounted.shapePath;
                     entry.load(data.data(), data.size());
                     imageShape = entry.loaded ? &entry : nullptr;
                 }
                 if (!imageShape) continue;
                 const std::string mountName = "mount" + std::to_string(mounted.mountPoint);
                 int mountNode = g->shape->findNode(mountName);
                 if (mountNode < 0 && mounted.mountPoint == 0)
                     mountNode = g->shape->findNode("rhand");
                 const int imageMount = imageShape->findNode("Mountpoint");
                 if (mountNode < 0 || imageMount < 0 ||
                     mountNode >= (int)g->shape->defaultTransforms.size() ||
                     imageMount >= (int)imageShape->defaultTransforms.size()) continue;
                  MatrixF imageModel = model * g->shape->upOrientation() *
                             g->shape->defaultTransforms[mountNode] *
                             imageShape->defaultTransforms[imageMount].inverse() *
                             imageShape->upOrientation();
                  r.setModel(imageModel);
                  const DTSShape::Animation* imageAnimation = nullptr;
                  for (const char* name : {mounted.isFiring ? "fire" : "idle",
                                           "ambient", "spin", "stand"}) {
                      imageAnimation = findAnimation(*imageShape, name);
                      if (imageAnimation) break;
                  }
                   if (imageAnimation && imageAnimation->duration > 0.0f)
                       imageShape->renderAnimation(imageAnimation->name.c_str(),
                                                   fmodf(g->threadAnimTime, imageAnimation->duration));
                   else
                       imageShape->render(0);
                   if (mounted.isFiring) {
                       const Point3F muzzle = mountedNodePosition(imageModel, *imageShape, "Mountpoint");
                       r.drawSprite(muzzle, 0.15f, {1.0f, 0.9f, 0.5f, 0.9f});
                   }
              }
              if (g->hasShield && g->shieldLevel > 0.01f) {
                  const float pulse = sinf(demoTime * 6.0f) * 0.08f + 0.92f;
                  const float alpha = g->shieldLevel * 0.25f * pulse;
                  const float baseSize = isVehicle ? 1.8f : 1.0f;
                  for (int layer = 0; layer < 3; ++layer) {
                      const float size = baseSize * (1.0f - layer * 0.15f);
                      r.drawBox({{renderPosition.x - size, renderPosition.y - size, renderPosition.z - size},
                                 {renderPosition.x + size, renderPosition.y + size, renderPosition.z + size}},
                                {0.3f, 0.6f, 1.0f, alpha * (1.0f - layer * 0.25f)});
                  }
              }
          }
        }

        // Spectator HUD for live ghosts
        if (!demoPlaying && activeConn && activeConn->isConnected() && liveGhosts.size() > 0) {
            auto* font = r.getFont();
            if (font) {
                std::vector<int> hudIndices = liveGhosts.getAllIndices();
                int screenW = r.config().width, screenH = r.config().height;
                for (int idx : hudIndices) {
                    if (serverPlayerGhostSynced && (uint32_t)idx == serverPlayerGhostIndex) continue;
                    const GhostEntry* g = liveGhosts.getGhost(idx);
                    if (!g) continue;
                    if (g->position.x == 0 && g->position.y == 0 && g->position.z == 0) continue;
                    if (!isRenderableGhostClass(g->className)) continue;
                    Point3F above = Math::torquePointToYUp({g->renderPos.x, g->renderPos.y, g->renderPos.z});
                    above.y += 2.5f;
                    Point3F screen = worldToScreen(above, r.viewMatrix(), r.projectionMatrix(), screenW, screenH);
                    if (screen.x < 0 || screen.x > screenW || screen.y < 0 || screen.y > screenH) continue;
                     ColorF col{1, 1, 1, 1};
                     std::string label = g->isFlag ? "Flag" : g->className;
                     if (g->isFlag) {
                         const auto team = liveTeamScores.find(g->flagTeamId);
                         if (team != liveTeamScores.end() && !team->second.name.empty())
                             label = team->second.name + " Flag";
                     }
                     if (g->isFlag && g->flagTeamId == 1) col = {1, 0.25f, 0.25f, 1};
                     else if (g->isFlag && g->flagTeamId == 2) col = {0.3f, 0.45f, 1, 1};
                     font->render(label.c_str(), screen.x - 30, screen.y - 20, col, 1.2f);
                     if (g->isFlag) continue;
                     float barW = 50, barH = 6;
                    float bx = screen.x - barW/2;
                    float by = screen.y + 2;
                    r.drawBox({{bx-1, by-1, 0}, {bx+barW+1, by+barH+1, 0}}, {0, 0, 0, 0.6f});
                    float healthFrac = (g->health > 0) ? (g->health / 100.0f) : 1.0f;
                    ColorF healthCol = healthFrac > 0.5f ? ColorF{0, 1, 0, 0.8f} :
                                      healthFrac > 0.25f ? ColorF{1, 1, 0, 0.8f} : ColorF{1, 0, 0, 0.8f};
                    r.drawBox({{bx, by, 0}, {bx + barW * healthFrac, by + barH, 0}}, healthCol);
                    float ey2 = by + barH + 1;
                    float energyFrac = (g->energy > 0) ? (g->energy / 100.0f) : 1.0f;
                    r.drawBox({{bx-1, ey2-1, 0}, {bx+barW+1, ey2+barH+1, 0}}, {0, 0, 0, 0.6f});
                    r.drawBox({{bx, ey2, 0}, {bx + barW * energyFrac, ey2 + barH, 0}}, {0.3f, 0.5f, 1, 0.8f});
                }
            }
        }
    }

    // 3D demo path trail
    if (demoPlaying && demoPathCount > 1) {
        // Full path in dim green
        r.drawLineStrip(demoPath, {0.2f, 0.8f, 0.2f, 0.6f});
    }

    // Mapper uses the normal script bootstrap for asset/datablock definitions,
    // but its output is a world-only inspection frame.
    if (!mapperMode) {
        auto& eng2 = Engine::instance();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        eng2.guiRenderer().render();
        glEnable(GL_DEPTH_TEST);
    }

    r.endFrame();
}

void Game::startLocalGame(const char* map) {
    Console::instance().printf(LogLevel::Info, "Starting local game");
    if (demoPlaying) stopDemoPlayback();
    setState(Loading);
    Engine::instance().audio().stopAll();
    clearProjectileAudio();
    clearMissionAudio();

    auto failLocalGame = [&]() {
        setState(MenuScreen);
        if (hud) hud->resetState();
        auto& gui = Engine::instance().guiRenderer();
        gui.clearDialogs();
        resetGameplayGui(gui);
        gui.setContentImmediate(gui.findControl("LobbyGui") ? "LobbyGui" : "LaunchGui");
        Engine::instance().platform().setRelativeMouse(false);
        Engine::instance().platform().showMouse(true);
    };
    std::string missionPath;

    if (map && map[0]) {
        missionPath = map;
        Console::instance().printf(LogLevel::Info, "Using specified mission: %s", missionPath.c_str());
    } else {
        // Dynamically discover available missions
        auto& fs = Engine::instance().fs();
        std::vector<std::string> allEntries;
        fs.listFiles(nullptr, allEntries);
        std::vector<std::string> foundMissions;
        for (auto& e : allEntries) {
            if (missionLower(e).starts_with("missions/") && isMissionFile(e))
                if (const std::string mission = missionLoadPath(e); !mission.empty())
                    foundMissions.push_back(mission);
        }

        if (!foundMissions.empty()) {
            std::sort(foundMissions.begin(), foundMissions.end(), [](const std::string& a, const std::string& b) {
                const std::string al = missionLower(a), bl = missionLower(b);
                return al == bl ? a < b : al < bl;
            });
            foundMissions.erase(std::unique(foundMissions.begin(), foundMissions.end(),
                [](const std::string& a, const std::string& b) {
                    return missionLower(a) == missionLower(b);
                }), foundMissions.end());
            missionPath = foundMissions[0];
            Console::instance().printf(LogLevel::Info, "Found %zu missions, loading: %s", foundMissions.size(), missionPath.c_str());
        } else {
            Console::instance().printf(LogLevel::Error, "No missions found in filesystem. Expected missions/*.mis in mounted stock resources.");
        }

        if (missionPath.empty()) {
            Console::instance().printf(LogLevel::Error, "Cannot start local game: no mission available. Place .mis/.ter files in missions/ directory.");
            failLocalGame();
            return;
        }
    }
    missionPath = missionLoadPath(missionPath);
    if (missionPath.empty()) {
        Console::instance().printf(LogLevel::Error, "Cannot start local game: unsafe mission name");
        failLocalGame();
        return;
    }

    // Classify weather by mission name for ambient audio selection
    weatherType = 0; // dry
    if (missionPath.find("Whiteout") != std::string::npos ||
        missionPath.find("SolsDescent") != std::string::npos)
        weatherType = 1; // cold/windy
    else if (missionPath.find("Training2") != std::string::npos ||
             missionPath.find("Swamp") != std::string::npos)
        weatherType = 2; // wet

    if (w->load(missionPath.c_str())) {
        pl->respawn();
        pl->setTeam(1);
        // Use mission spawn point, or fall back to above terrain center
        Point3F spawnPos = w->spawnPoint();
        float h = w->getHeight(spawnPos.x, spawnPos.z);
        if (spawnPos.y < h + 0.5f) spawnPos.y = h + 2.0f;
        else if (spawnPos.y > h + 10.0f) spawnPos.y = h + 2.0f; // bring down from sky spawns
        pl->setPosition(spawnPos);
        setState(Playing);
        Console::instance().printf(LogLevel::Info, "Game started on '%s'", missionPath.c_str());

        // Clear TS GUI dialogs and switch to in-game view
        auto& gui = Engine::instance().guiRenderer();
         gui.clearDialogs();
         gui.setContent("PlayGui");
         if (hud) {
             const auto initialObjective = stockTrainingInitialObjective(missionPath);
             hud->setObjectiveTask(initialObjective.first.c_str(), initialObjective.second.c_str());
         }
         if (auto* weaponsHud = gui.findControl("weaponsHud")) {
            weaponsHud->visible = true;
            weaponsHud->fields["backgroundBitmap"] = "gui/hud_new_panel";
            weaponsHud->fields["highlightBitmap"] = "gui/hud_new_weaponselect";
            weaponsHud->fields["infiniteAmmoBitmap"] = "gui/hud_infinity";
            weaponsHud->hudSlots.resize(18);
            auto hudName = [&](int slot, const char* field) {
                if (auto* ts = Engine::instance().script().ts())
                    return ts->getGlobal("$WeaponsHudData[" + std::to_string(slot) + "," + field + "]").toString();
                return std::string();
            };
            for (int slot = 0; slot < (int)weaponsHud->hudSlots.size(); ++slot) {
                const std::string itemName = hudName(slot, "itemDataName");
                for (int weapon = 0; weapon < player().weaponCount(); ++weapon) {
                    std::string nativeName = player().weapon(weapon).type >= 0 &&
                        player().weapon(weapon).type < gWeaponCount
                        ? gWeaponTable[player().weapon(weapon).type].name : "";
                    if (nativeName == "Spinfusor") nativeName = "Disc";
                    else if (nativeName == "PlasmaGun") nativeName = "Plasma";
                    if (nativeName != itemName) continue;
                    auto& hudSlot = weaponsHud->hudSlots[slot];
                    hudSlot.name = itemName;
                    hudSlot.bitmap = hudName(slot, "bitmapName");
                    hudSlot.amount = player().weapon(weapon).ammo;
                    hudSlot.visible = true;
                    hudSlot.active = weapon == player().currentWeapon();
                    if (hudSlot.active) weaponsHud->activeHudSlot = slot;
                    break;
                }
            }
        }

        // AudioEmitter is a supported V12 mission object, distinct from the
        // weather loop below.  Use the mission fields directly so authored
        // map ambience survives local playback.
        auto& audio = Engine::instance().audio();
        clearMissionAudio();
        if (audio.config().enabled) {
            for (const auto& object : w->objects()) {
                 if (!object.audioEmitter) continue;
                 std::string fileName = object.audioFileName;
                 // A stock Training2 emitter contains one typo in the quoted
                 // path ("sandpatter1. wav"). Torque ignores that whitespace
                 // while resolving resource names; preserve the supplied
                 // asset and normalize only authored audio paths here.
                 fileName.erase(std::remove_if(fileName.begin(), fileName.end(),
                     [](unsigned char c) { return std::isspace(c); }), fileName.end());
                 if (fileName.empty()) continue;
                SoundBuffer* sound = audio.loadSound(fileName.c_str());
                if (!sound && fileName.rfind("audio/", 0) != 0)
                    sound = audio.loadSound(("audio/" + fileName).c_str());
                if (!sound) continue;
                 auto* source = audio.createSource();
                 if (!source) break;
                const Point3F position = Math::torquePointToYUp(object.pos);
                const bool is3D = object.audioIs3D;
                const bool looping = object.audioIsLooping;
                const float volume = std::max(0.0f, object.audioVolume);
                const float minDistance = std::max(0.001f, object.audioMinDistance);
                const float maxDistance = std::max(minDistance, object.audioMaxDistance);
                 source->setVolume(volume * audio.config().masterVolume *
                     audio.config().sfxVolume);
                source->setLooping(looping);
                if (is3D) {
                    source->setPosition(position);
                    source->setDistance(minDistance, maxDistance);
                }
                source->play(sound);
                 // Keep one-shot emitters owned by the mission too. OpenAL
                 // may retire them between frames, but cleanup must still be
                 // able to release every source deterministically.
                 emitterSources.push_back(source);
            }
        }

        // Start ambient audio
        if (audio.config().enabled) {
            const char* ambPath;
            if (weatherType == 1)      ambPath = "audio/fx/environment/coldwind1.wav";
            else if (weatherType == 2) ambPath = "audio/fx/environment/wetwind.wav";
            else                        ambPath = "audio/fx/environment/drywind.wav";
            if (!Engine::instance().fs().fileExists(ambPath)) {
                Console::instance().printf(LogLevel::Warn, "Ambient sound not found: %s (weather type %d)", ambPath, weatherType);
            }
            ambientSound = audio.loadSound(ambPath);
            if (ambientSound) {
                ambientSource = audio.createSource();
                if (ambientSource) {
                    ambientSource->setLooping(true);
                     ambientSource->setVolume(0.3f * audio.config().masterVolume *
                         audio.config().sfxVolume);
                    ambientSource->play(ambientSound);
                    Console::instance().printf(LogLevel::Info, "Ambient: %s", ambPath);
                }
            }
        }
    } else {
        Console::instance().printf(LogLevel::Error, "Failed to load map '%s'", missionPath.c_str());
        failLocalGame();
    }
}

void Game::dispatchHudClientCommand(const std::vector<std::string>& args) {
    if (args.empty()) return;
    std::string command = args[0];
    std::string lower = command;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    static const std::set<std::string> allowed = {
        "setweaponshudactive", "setweaponshuditem", "setweaponshudammo",
        "setweaponshudbitmap", "setweaponshudbackgroundbmp",
        "setweaponshudhighlightbmp", "setweaponshudinfiniteammobmp",
        "setweaponshudclearall", "setammohudcount", "setbackpackhuditem",
        "setbackpackhudbitmap",
        "setinventoryhudbitmap", "setinventoryhuditem", "setinventoryhudamount",
        "setinventoryhudbackgroundbmp", "setinventoryhudclearall",
        "setvweaponshudactive", "setvweaponshudclearall", "setrepairreticle",
        "setcloakiconon", "setcloakiconoff", "setrepairpackiconon",
        "setrepairpackiconoff", "setshieldiconon", "setshieldiconoff",
        "setsenjamiconon", "setsenjamiconoff", "updatepacktext",
        "checkpassengers", "showpassenger", "sethalftimeclock",
        "setsatchelarmed", "setbeaconnames", "removereticle",
        "startbombersight", "endbombersight", "starteffect", "stopeffect",
         "setpoweraudioprofiles", "setvoiceinfo", "togglehudmode",
         "sethudmode", "displayhuds", "resethud", "toggledashhud",
         "setpowersoundprofiles", "setcontrolobjectreticle",
         "centerprint", "bottomprint", "clearcenterprint", "clearbottomprint",
         "toggleplayhuds",
         "setstationkeys", "setdefaultvehiclekeys", "setweaponryvehiclekeys",
         "setpilotvehiclekeys", "setpassengervehiclekeys",
         "setplaycontent", "pickteammenu", "processpickteam", "pickteam",
         "setfirstperson", "getfirstperson", "vehiclemount", "vehicledismount",
         "missionstartphase1", "missionstartphase2", "missionstartphase3",
         "missionend",
         "playmusic", "stopmusic", "playcdtrack", "stopcd",
         "playerstarttalking", "playerstoppedtalking",
         "chatmessage", "cannedchatmessage", "teamrepairmessage",
         "showvehiclegauges", "stationvehicleshowhud", "stationvehiclehidehud",
         "stationvehiclehidejusthud", "clearpassengers", "protectingstaticobjects",
         "resetcommandmap", "scopecommandermap", "cameraattachresponse",
         "controlobjectresponse", "controlobjectreset",
         "resettasklist", "taskinfo", "potentialteamtask", "potentialtask",
         "taskdeclined", "taskaccepted", "taskcompleted", "taskfailed", "acceptedtask"
    };
    if (!allowed.count(lower)) return;
    if (!command.empty()) command[0] = (char)std::toupper((unsigned char)command[0]);
    if (auto* ts = Engine::instance().script().ts()) {
        // Keep the wire words intact, including empty positional arguments.
        // The script dispatcher performs the case-insensitive clientCmd lookup.
        if (!ts->dispatchClientCommand(args))
            Console::instance().printf(LogLevel::Warn,
                "Client: ignored unknown clientCmd '%s'", args[0].c_str());
    }
    // The retail task callback has a misspelled parameter but writes the
    // correctly spelled field, losing the AI objective in the process.
    if (lower == "taskinfo" && args.size() >= 5) {
        if (auto* taskList = Engine::instance().script().findObject("TaskList")) {
            taskList->fields["currentTaskClient"] = VMValue(args[1]);
            taskList->fields["currentAIObjective"] = VMValue(args[2]);
            taskList->fields["currentTaskIsTeam"] = VMValue(args[3]);
            taskList->fields["currentTaskDescription"] = VMValue(args[4]);
        }
        if (hud) hud->setObjectiveTask(args[4].c_str());
    } else if (lower == "resettasklist") {
        if (auto* taskList = Engine::instance().script().findObject("TaskList")) {
            taskList->fields["currentTaskClient"] = VMValue("");
            taskList->fields["currentAIObjective"] = VMValue("");
            taskList->fields["currentTaskIsTeam"] = VMValue("0");
            taskList->fields["currentTaskDescription"] = VMValue("");
        }
        if (hud) hud->clearObjectiveTask();
    } else if (lower == "taskcompleted" || lower == "taskfailed" || lower == "acceptedtask") {
        if (hud && args.size() >= 2) hud->setObjectiveTask(args[1].c_str());
    }
}

void Game::connectToServer(const char* host, uint16_t port, bool observer, const char* password) {
    if (!host || !host[0] || port == 0) {
        Console::instance().printf(LogLevel::Warn, "Invalid server address");
        return;
    }
    if (demoPlaying) stopDemoPlayback();
    if (activeConn) {
        activeConn->disconnect();
        Engine::instance().network().destroyConnection(activeConn);
        activeConn = nullptr;
    }
    resetLiveMissionState();
    cfg.serverHost = host;
    cfg.serverPort = port;
    cfg.online = true;
    nativeDatablockShapes.clear();

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d...", host, port);
    // Show connecting message
    if (hud) hud->showMessage("Connecting...", ColorF{1, 1, 0, 1});

    auto& net = Engine::instance().network();
    activeConn = net.createConnection();
    activeConn->setPlayerName(cfg.playerName.c_str());
    activeConn->setJoinPassword(password ? password : "");
    activeConn->setObserverMode(observer);
    {
        activeConn->setConnectCallback([this](bool success) {
            if (success) {
                Console::instance().printf(LogLevel::Info, "Connected!");
                if (activeConn->isObserverMode()) {
                    activeConn->sendCommandPacket("setPlayerTeam 0");
                    activeConn->sendCommandPacket("ScopeCommanderMap 1");
                    activeConn->sendCommandPacket("WatchOnly ImaWatcher");
                }
            } else {
                Console::instance().printf(LogLevel::Info, "Connection failed");
                // Transport failures must restore the same world, audio, HUD,
                // dialog, and pointer state as an explicit disconnect.
                resetLiveMissionState();
                cfg.online = false;
                Engine::instance().platform().setRelativeMouse(false);
                Engine::instance().platform().showMouse(true);
                setState(MenuScreen);
            }
        });

        // Install every callback before opening the socket. This keeps a fast
        // local response from racing the launch transition setup.
        // Handle incoming packets
        activeConn->setPacketCallback([this](PacketType type, const uint8_t* data, size_t size) {
            if (type == PacketType::ConnectOK) {
                activeConn->setState(Connection::Connected);
                Console::instance().printf(LogLevel::Info, "Connection established, entering game");
                if (activeConn->isObserverMode()) {
                    liveSpectateInit = false;
                    spectateGhostIndex = -1;
                    liveFollowGhostIndex = -1;
                    liveFollowCenterInit = false;
                    setState(Dead);
                    auto& gui = Engine::instance().guiRenderer();
                    gui.clearDialogs();
                    gui.setContent("PlayGui");
                } else {
                    startLocalGame();
                }
            } else if (type == PacketType::GameData && size > 0) {
                // Check for command packet
                if (data[0] == T2Protocol::GDT_Command && size >= 3) {
                    uint16_t cmdLen = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
                    if (cmdLen > 0 && (size_t)(3 + cmdLen) <= size) {
                        std::string cmd((const char*)data + 3, cmdLen);
                        Console::instance().printf(LogLevel::Warn,
                            "Ignored untrusted legacy server command: %s", cmd.c_str());
                        std::istringstream tokens(cmd);
                        std::vector<std::string> args;
                        std::string token;
                        while (tokens >> token && args.size() < 20) args.push_back(token);
                        if (!args.empty()) dispatchHudClientCommand(args);
                    }
                    return;
                }

                // Handle datablock packets
                if (data[0] == T2Protocol::GDT_Datablock) {
                    T2Protocol::DatablockHeader hdr;
                    const uint8_t* payload;
                    size_t payloadLen;
                    if (T2Protocol::decodeDatablock(data, size, hdr, payload, payloadLen)) {
                        ReceivedDatablock rdb;
                        rdb.hdr = hdr;
                        rdb.payload.assign(payload, payload + payloadLen);
                        receivedDatablocks[hdr.classId].push_back(std::move(rdb));
                        Console::instance().printf(LogLevel::Debug,
                            "Received datablock: class=%u obj=%u idx=%u/%u (%zu bytes)",
                            (unsigned)hdr.classId, (unsigned)hdr.objectId,
                            (unsigned)hdr.index, (unsigned)hdr.total, payloadLen);
                    }
                    return;
                }

                // Handle chat messages
                if (data[0] == T2Protocol::GDT_ChatMessage) {
                    T2Protocol::ChatMessage chat;
                    if (T2Protocol::decodeChat(data, size, chat)) {
                        Console::instance().printf(LogLevel::Info, "[CHAT] %s: %s", chat.sender, chat.text);
                        playChatBeep();
                        if (hud) {
                            std::string line = std::string(chat.sender) + ": " + chat.text;
                            hud->addChatLine(line.c_str());
                        }
                        if (auto* ts = Engine::instance().script().ts()) {
                            if (ts->hasFunction("addMessageHudLine"))
                                ts->callFunction("addMessageHudLine", {
                                    VMValue(std::string(chat.sender) + ": " + chat.text)});
                        }
                    }
                    return;
                }

                // Handle game state packets
                if (data[0] == T2Protocol::GDT_GameState) {
                    T2Protocol::GameStateMessage gs;
                    if (T2Protocol::decodeGameState(data, size, gs)) {
                        serverPlayerGhostIndex = gs.controlObjectGhostIndex;
                        serverPlayerGhostSynced = true;
                        Console::instance().printf(LogLevel::Info,
                            "GameState: control ghost idx=%u", (unsigned)gs.controlObjectGhostIndex);
                    }
                    return;
                }

                // Handle ghost packets (server batches multiple ghosts into one datagram — loop over them)
                if (data[0] == T2Protocol::GDT_Ghost || data[0] == T2Protocol::GDT_GhostAlways) {
                    const uint8_t* gp = data;
                    size_t grem = size;
                    while (grem > 0 && (gp[0] == T2Protocol::GDT_Ghost || gp[0] == T2Protocol::GDT_GhostAlways)) {
                        T2Protocol::GhostMessage gm;
                        if (!T2Protocol::decodeGhostHeader(gp, grem, gm)) break;
                        size_t hdrSize = 1 + 4 + 1 + 4; // GDT + index + type + classId
                        const uint8_t* ghostPayload = gp + hdrSize;
                        size_t ghostPayloadLen = (grem > hdrSize) ? grem - hdrSize : 0;

                        if (gm.type == T2Protocol::Ghost_Delete) {
                            liveGhosts.deleteGhost((int)gm.index);
                            Console::instance().printf(LogLevel::Debug, "Ghost delete idx=%u", (unsigned)gm.index);
                        } else if (gm.type == T2Protocol::Ghost_Create) {
                            if (!liveGhosts.hasGhost((int)gm.index)) {
                                std::string cn;
                                if (const char* name = V12::ghostClassName((size_t)gm.classId)) cn = name;
                                else
                                    cn = "Class" + std::to_string(gm.classId);
                                liveGhosts.createGhost((int)gm.index, gm.classId, cn);
                                // Look up datablock and apply config
                                auto dbs = getDatablocksForClass((uint32_t)gm.classId);
                                if (dbs && !dbs->empty()) {
                                    auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                                    if (ge) {
                                        const auto& payload = dbs->front().payload;
                                        if (payload.size() >= 6) {
                                            float hp;
                                            memcpy(&hp, payload.data(), 4);
                                            ge->maxHealth = hp;
                                            uint16_t nameLen;
                                            memcpy(&nameLen, payload.data() + 4, 2);
                                            if (nameLen > 0 && (size_t)(6 + nameLen) <= payload.size()) {
                                                ge->shapeName.assign((const char*)payload.data() + 6, nameLen);
                                            }
                                        }
                                    }
                                }
                                // Read position from payload
                                if (ghostPayloadLen >= 24) {
                                    float px, py, pz, rx, rz, hp;
                                    uint32_t p = 0;
                                    memcpy(&px, ghostPayload + p, 4); p += 4;
                                    memcpy(&py, ghostPayload + p, 4); p += 4;
                                    memcpy(&pz, ghostPayload + p, 4); p += 4;
                                    memcpy(&rx, ghostPayload + p, 4); p += 4;
                                    memcpy(&rz, ghostPayload + p, 4); p += 4;
                                    memcpy(&hp, ghostPayload + p, 4); p += 4;
                                    auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                                    if (ge) {
                                        ge->position = {px, py, pz};
                                        float halfYaw = rz * 0.5f;
                                        float halfPitch = rx * 0.5f;
                                        float cy = cosf(halfYaw), sy = sinf(halfYaw);
                                        float cp = cosf(halfPitch), sp = sinf(halfPitch);
                                        ge->rotation = {sp * cy, cp * sy, sp * sy, cp * cy};
                                        ge->hasRotation = true;
                                        ge->health = hp;
                                        // Parse kills/deaths/team/name (payload: 24=pos, 8=k/d, 4=team, name)
                                        if (ghostPayloadLen >= 36) {
                                            float fk, fd, fteam;
                                            memcpy(&fk, ghostPayload + p, 4); p += 4;
                                            memcpy(&fd, ghostPayload + p, 4); p += 4;
                                            memcpy(&fteam, ghostPayload + p, 4); p += 4;
                                            ge->kills = (int32_t)fk;
                                            ge->deaths = (int32_t)fd;
                                            ge->teamId = (int32_t)fteam;
                                            // Parse player name if present (null-terminated, bounded)
                                            if (ghostPayloadLen > p && ghostPayload[p] != 0) {
                                                size_t maxName = ghostPayloadLen - p;
                                                size_t nl = 0;
                                                while (nl < maxName && ghostPayload[p + nl] != 0) nl++;
                                                ge->playerName.assign((const char*)ghostPayload + p, nl);
                                            }
                                        }
                                    }
                                }
                                Console::instance().printf(LogLevel::Debug,
                                    "Ghost create idx=%u class=%d (%s)", (unsigned)gm.index, gm.classId, cn.c_str());
                            }
                        } else if (gm.type == T2Protocol::Ghost_Update) {
                            auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                            if (ge && ghostPayloadLen >= 24) {
                                float px, py, pz, rx, rz, hp;
                                uint32_t p = 0;
                                memcpy(&px, ghostPayload + p, 4); p += 4;
                                memcpy(&py, ghostPayload + p, 4); p += 4;
                                memcpy(&pz, ghostPayload + p, 4); p += 4;
                                memcpy(&rx, ghostPayload + p, 4); p += 4;
                                memcpy(&rz, ghostPayload + p, 4); p += 4;
                                memcpy(&hp, ghostPayload + p, 4); p += 4;
                                ge->position = {px, py, pz};
                                float halfYaw = rz * 0.5f;
                                float halfPitch = rx * 0.5f;
                                float cy = cosf(halfYaw), sy = sinf(halfYaw);
                                float cp = cosf(halfPitch), sp = sinf(halfPitch);
                                ge->rotation = {sp * cy, cp * sy, sp * sy, cp * cy};
                                 ge->hasRotation = true;
                                 ge->health = hp;
                                 // Parse kills/deaths/team/name
                                 if (ghostPayloadLen >= 36) {
                                     float fk, fd, fteam;
                                     memcpy(&fk, ghostPayload + p, 4); p += 4;
                                     memcpy(&fd, ghostPayload + p, 4); p += 4;
                                     memcpy(&fteam, ghostPayload + p, 4); p += 4;
                                      ge->kills = (int32_t)fk;
                                      ge->deaths = (int32_t)fd;
                                      ge->teamId = (int32_t)fteam;
                                      if (ghostPayloadLen > p && ghostPayload[p] != 0) {
                                          size_t maxName = ghostPayloadLen - p;
                                          size_t nl = 0;
                                          while (nl < maxName && ghostPayload[p + nl] != 0) nl++;
                                          ge->playerName.assign((const char*)ghostPayload + p, nl);
                                      }
                                  }
                             }
                         }
                         // Advance to the next ghost in the batch (variable-length null-terminated name).
                         size_t consumed = hdrSize;
                         if (gm.type != T2Protocol::Ghost_Delete && ghostPayloadLen >= 36) {
                             size_t avail = ghostPayloadLen - 36;
                             const uint8_t* np = ghostPayload + 36;
                             size_t nl = 0;
                             while (nl < avail && np[nl] != 0) nl++;
                             consumed += 36 + (nl < avail ? nl + 1 : avail);
                         } else {
                             consumed += ghostPayloadLen;
                         }
                         if (consumed > grem) break;
                         gp += consumed;
                         grem -= consumed;
                     }
                     return;
                     }

                     T2Protocol::UpdateMessage update;
                if (T2Protocol::decodeUpdate(data, size, update)) {
                    // Reconcile: set to server state then replay pending moves
                    Point3F serverPos = {update.posX, update.posY, update.posZ};
                    Point3F serverVel = {update.velX, update.velY, update.velZ};
                     if (pl) {
                         reconcile(serverPos, serverVel, update.lastMoveSeq);
                         pl->setRotation({update.rotX, 0, update.rotZ});
                         pl->setHealth(update.health);
                         if ((update.flags & T2Protocol::UPDATE_DEAD) != 0 && gameState == Playing) {
                             deathTimer = 0.0f;
                             currentInput = {};
                             pendingMoves.clear();
                             w->projectiles().clear();
                             setState(Dead);
                         } else if ((update.flags & T2Protocol::UPDATE_DEAD) == 0 &&
                                    update.health > 0.0f && gameState == Dead) {
                             deathTimer = 0.0f;
                             setState(Playing);
                         }
                     }
                }
            }
        });
        activeConn->setCommandCallback([](const std::string& command) {
            if (!command.empty()) {
                Console::instance().printf(LogLevel::Warn,
                    "Ignored untrusted server command: %s", command.c_str());
            }
        });
        activeConn->setClientCommandCallback([this](const std::vector<std::string>& args) {
            dispatchHudClientCommand(args);
        });
         activeConn->setStateCallback([this](const V12::ServerGameState& state) {
             if (!state.sensorGroupListenMasks.empty())
                 liveSensorGroupListenMasks = state.sensorGroupListenMasks;
            if (state.controlPresent && !state.controlDirty) {
                serverPlayerGhostIndex = state.controlGhost;
                serverPlayerGhostSynced = true;
            }
            if (state.hasDamageFlash) damageFlash = state.damageFlash;
            if (state.hasWhiteOut) whiteOut = state.whiteOut;
        });
        activeConn->setEpochCallback([this](uint64_t epoch) {
            Console::instance().printf(LogLevel::Info,
                "Observer protocol epoch reset: %llu",
                (unsigned long long)epoch);
            liveGhosts.clear();
            nativeDatablockShapes.clear();
            nativeDatablocks.clear();
            liveTargets.clear();
            liveMissionCrc = 0;
            liveTeamScores.clear();
            livePlayerScores.clear();
            liveClientTargetIds.clear();
            liveClientNames.clear();
            liveClientTeams.clear();
            liveMatchStarted_ = false;
            liveMatchEnded_ = false;
            liveMissionDisplayName_.clear();
            liveMissionType_.clear();
            liveClockDurationMs_ = 0;
            liveClockReceivedAt_ = 0.0;
            liveLoadInfoLines_.clear();
            liveSpectateInit = false;
            spectateGhostIndex = -1;
        });
        activeConn->setDatablockCallback(
             [this](uint16_t objectId, uint8_t classId, uint16_t, uint16_t,
                    const std::string& className, const V12::DecodedDataBlock& data) {
                 if (!data.shapeFile.empty())
                     nativeDatablockShapes[objectId] = data.shapeFile;
                 ParsedDataBlock block;
                 block.objectId = objectId;
                 block.classId = classId;
                  block.className = className;
                  block.decoded = data;
                  if (className == "AudioProfile" && data.audioDescriptionRef != 0) {
                      auto description = nativeDatablocks.find(data.audioDescriptionRef);
                      if (description != nativeDatablocks.end()) {
                          block.decoded.audioVolume = description->second.decoded.audioVolume;
                           block.decoded.audioLooping = description->second.decoded.audioLooping;
                           block.decoded.audioLoopCount = description->second.decoded.audioLoopCount;
                           block.decoded.audioMinLoopGapMs = description->second.decoded.audioMinLoopGapMs;
                           block.decoded.audioMaxLoopGapMs = description->second.decoded.audioMaxLoopGapMs;
                          block.decoded.audioIs3D = description->second.decoded.audioIs3D;
                          block.decoded.audioMinDistance = description->second.decoded.audioMinDistance;
                          block.decoded.audioMaxDistance = description->second.decoded.audioMaxDistance;
                      }
                  }
                  nativeDatablocks[objectId] = std::move(block);
                  if (className == "AudioDescription") {
                      for (auto& [id, profile] : nativeDatablocks) {
                          if (profile.className != "AudioProfile" ||
                              profile.decoded.audioDescriptionRef != objectId) continue;
                          profile.decoded.audioVolume = data.audioVolume;
                           profile.decoded.audioLooping = data.audioLooping;
                           profile.decoded.audioLoopCount = data.audioLoopCount;
                           profile.decoded.audioMinLoopGapMs = data.audioMinLoopGapMs;
                           profile.decoded.audioMaxLoopGapMs = data.audioMaxLoopGapMs;
                          profile.decoded.audioIs3D = data.audioIs3D;
                          profile.decoded.audioMinDistance = data.audioMinDistance;
                          profile.decoded.audioMaxDistance = data.audioMaxDistance;
                      }
                  }
             });
         activeConn->setTargetCallback(
            [this](const V12::ServerEvent::TargetInfo* info, uint16_t targetId) {
                 if (!info) {
                  liveTargets.erase(targetId);
                      if (GhostEntry* ghost = liveGhosts.getMutableGhost((int)targetId)) {
                          ghost->playerName.clear();
                          ghost->skinName.clear();
                          ghost->targetType.clear();
                          ghost->targetRenderFlags = 0;
                          ghost->isFlag = false;
                          ghost->flagTeamId = 0;
                      }
                     if (spectateGhostIndex == (int)targetId) {
                         spectateGhostIndex = -1;
                         liveFollowGhostIndex = -1;
                         liveFollowCenterInit = false;
                     }
                     return;
                 }
                 auto& target = liveTargets[targetId];
                 target.targetId = targetId;
                 if (info->hasName) { target.hasName = true; target.name = info->name; }
                 if (info->hasSkin) { target.hasSkin = true; target.skin = info->skin; }
                 if (info->hasSkinPreference) { target.hasSkinPreference = true; target.skinPreference = info->skinPreference; }
                 if (info->hasVoice) { target.hasVoice = true; target.voice = info->voice; }
                  if (info->hasType) { target.hasType = true; target.type = info->type; }
                  if (info->hasSensorGroup) { target.hasSensorGroup = true; target.sensorGroup = info->sensorGroup; }
                 if (info->hasDataBlockId) { target.hasDataBlockId = true; target.dataBlockId = info->dataBlockId; }
                 if (info->hasRenderFlags) { target.hasRenderFlags = true; target.renderFlags = info->renderFlags; }
                 if (info->hasVoicePitch) { target.hasVoicePitch = true; target.voicePitch = info->voicePitch; }
                  if (GhostEntry* ghost = liveGhosts.getMutableGhost((int)targetId)) {
                    if (info->hasName && !info->name.empty()) ghost->playerName = info->name;
                      if (info->hasSkin && !info->skin.empty()) ghost->skinName = info->skin;
                       if (info->hasSensorGroup) ghost->sensorGroup = info->sensorGroup;
                       if (info->hasType) ghost->targetType = info->type;
                       if (info->hasRenderFlags) ghost->targetRenderFlags = info->renderFlags;
                     const bool isClientTarget = std::any_of(
                         liveClientTargetIds.begin(), liveClientTargetIds.end(),
                         [targetId](const auto& entry) { return entry.second == (int)targetId; });
                      ghost->isFlag = (target.renderFlags & 0x2) != 0 && !isClientTarget;
                     ghost->flagTeamId = ghost->isFlag ? target.sensorGroup : 0;
                     if (ghost->isFlag) ghost->teamId = target.sensorGroup;
                 }
            });
        activeConn->setMissionCallback([this](uint32_t crc) {
            if (liveMissionCrc != 0 && liveMissionCrc != crc)
                resetLiveMissionState();
            liveMissionCrc = crc;
        });
        activeConn->setServerMessageCallback([this](const std::vector<std::string>& argv) {
            if (argv.empty()) return;
            if (argv[0] == "ChatMessage") {
                if (argv.size() >= 2) {
                     Console::instance().printf(LogLevel::Info, "[CHAT] %s", argv[1].c_str());
                     playChatBeep();
                     if (hud) hud->addChatLine(argv[1].c_str());
                    if (auto* ts = Engine::instance().script().ts();
                        ts && ts->hasFunction("addMessageHudLine"))
                        ts->callFunction("addMessageHudLine", {VMValue(argv[1])});
                }
                return;
            }
            if (argv[0] != "ServerMessage") return;
            if (argv.size() >= 2) {
                std::vector<VMValue> callbackArgs;
                callbackArgs.emplace_back(argv[1]);
                callbackArgs.emplace_back(std::string());
                for (size_t i = 2; i < argv.size(); ++i)
                    callbackArgs.emplace_back(argv[i]);
                if (auto* ts = Engine::instance().script().ts())
                    ts->dispatchMessageCallback(argv[1], callbackArgs);
            }
            if (argv.size() >= 2 && argv[1] == "MsgMissionStart") {
                liveMatchStarted_ = true;
                liveMatchEnded_ = false;
                return;
            }
            if (argv.size() >= 2 && argv[1] == "MsgClientReady") {
                liveMatchStarted_ = false;
                liveMatchEnded_ = false;
                return;
            }
            if (argv.size() >= 2 &&
                (argv[1] == "MsgClearDebrief" || argv[1] == "MsgDebriefResult")) {
                liveMatchEnded_ = true;
                return;
            }
            if (argv.size() >= 5 && argv[1] == "MsgMissionDropInfo") {
                if ((!liveMissionDisplayName_.empty() || !liveMissionType_.empty()) &&
                    (liveMissionDisplayName_ != argv[2] || liveMissionType_ != argv[3]))
                    resetLiveMissionState();
                liveMissionDisplayName_ = argv[2];
                liveMissionType_ = argv[3];
                return;
            }
            if (argv.size() >= 4 && argv[1] == "MsgSystemClock") {
                liveClockDurationMs_ = std::max(0, atoi(argv[3].c_str()));
                liveClockReceivedAt_ = Engine::instance().timer().now();
                return;
            }
            if (argv.size() >= 2 && argv[1] == "MsgLoadInfo") {
                liveLoadInfoLines_.clear();
                return;
            }
            if (argv.size() >= 3 &&
                (argv[1] == "MsgLoadQuoteLine" || argv[1] == "MsgLoadObjectiveLine" ||
                 argv[1] == "MsgLoadRulesLine")) {
                if (liveLoadInfoLines_.size() < 128)
                    liveLoadInfoLines_.push_back(argv[2]);
                return;
            }
            if (argv.size() >= 2 && argv[1] == "MsgLoadInfoDone") return;
            if (argv.size() >= 4 &&
                (argv[1] == "MsgTeamScoreIs" || argv[1] == "MsgTeamScore")) {
                const int teamId = atoi(argv[2].c_str());
                const int score = atoi(argv[3].c_str());
                if (teamId > 0 && teamId < 64) {
                    liveTeamScores[teamId].teamId = teamId;
                    liveTeamScores[teamId].score = score;
                }
                return;
            }
             if (argv.size() >= 6 && argv[1] == "MsgCTFAddTeam") {
                 int teamId = 0;
                 if (!parseLiveIndex(argv[2], teamId) || teamId <= 0 || teamId >= 64) return;
                auto& team = liveTeamScores[teamId];
                team.teamId = teamId;
                team.name = argv[3];
                  team.flagStatus = liveFlagStatus(argv[4]);
                 team.flagCarrier = team.flagStatus == "held" && !argv[4].empty() ? argv[4] : "";
                team.score = atoi(argv[5].c_str());
                return;
            }
            if (argv.size() >= 5 &&
                (argv[1] == "MsgCTFFlagTaken" || argv[1] == "MsgCTFFlagDropped" ||
                 argv[1] == "MsgCTFFlagReturned" || argv[1] == "MsgCTFFlagCapped")) {
                 int teamId = 0;
                 if (!parseLiveIndex(argv[4], teamId) || teamId <= 0 || teamId >= 64) return;
                auto& team = liveTeamScores[teamId];
                team.teamId = teamId;
                team.flagStatus = argv[1] == "MsgCTFFlagTaken" ? "held" :
                                   argv[1] == "MsgCTFFlagDropped" ? "field" : "home";
                 team.flagCarrier = team.flagStatus == "held" && argv[2] != "0" ? argv[2] : "";
                return;
            }
            if (argv.size() >= 4 && argv[1] == "MsgPlayerScore") {
                const int clientId = atoi(argv[2].c_str());
                if (clientId >= 0 && clientId < 1024) {
                    livePlayerScores[clientId] = atoi(argv[3].c_str());
                    auto target = liveClientTargetIds.find(clientId);
                    if (target != liveClientTargetIds.end()) {
                        if (GhostEntry* ghost = liveGhosts.getMutableGhost(target->second))
                            ghost->score = livePlayerScores[clientId];
                    }
                }
            } else if (argv.size() >= 8 && argv[1] == "SetLineHud") {
                auto applyScore = [this](const std::string& name, int score) {
                    for (const auto& [clientId, clientName] : liveClientNames) {
                        if (clientName != name) continue;
                        livePlayerScores[clientId] = score;
                        auto target = liveClientTargetIds.find(clientId);
                        if (target != liveClientTargetIds.end()) {
                            if (GhostEntry* ghost = liveGhosts.getMutableGhost(target->second))
                                ghost->score = score;
                        }
                        break;
                    }
                };
                const int firstScore = atoi(argv[7].c_str());
                if (argv[6].size() > 0) applyScore(argv[6], firstScore);
                if (argv.size() >= 10) {
                    char* end = nullptr;
                    std::strtol(argv[8].c_str(), &end, 10);
                    if (end && *end != '\0')
                        applyScore(argv[8], atoi(argv[9].c_str()));
                }
            } else if (argv.size() >= 6 && argv[1] == "MsgDebriefAddLine") {
                const std::string& name = argv[4];
                char* end = nullptr;
                std::strtol(argv[5].c_str(), &end, 10);
                const bool singleTeam = end && *end == '\0';
                const int scoreIndex = singleTeam ? 5 : 6;
                if ((size_t)scoreIndex < argv.size()) {
                    const int score = atoi(argv[scoreIndex].c_str());
                    for (const auto& [clientId, clientName] : liveClientNames) {
                        if (clientName != name) continue;
                        livePlayerScores[clientId] = score;
                        auto target = liveClientTargetIds.find(clientId);
                        if (target != liveClientTargetIds.end()) {
                            if (GhostEntry* ghost = liveGhosts.getMutableGhost(target->second))
                                ghost->score = score;
                        }
                        break;
                    }
                }
             } else if (argv.size() >= 5 && argv[1] == "MsgClientJoin") {
                 int clientId = 0, targetId = 0;
                 if (parseLiveIndex(argv[3], clientId) && parseLiveIndex(argv[4], targetId)) {
                    liveClientTargetIds[clientId] = targetId;
                     liveClientNames[clientId] = argv[2];
                     if (GhostEntry* ghost = liveGhosts.getMutableGhost(targetId)) {
                         ghost->playerName = argv[2];
                         ghost->isFlag = false;
                         ghost->flagTeamId = 0;
                     }
                    auto score = livePlayerScores.find(clientId);
                    if (score != livePlayerScores.end()) {
                        if (GhostEntry* ghost = liveGhosts.getMutableGhost(targetId))
                            ghost->score = score->second;
                    }
                }
             } else if (argv.size() >= 4 && argv[1] == "MsgClientDrop") {
                 int clientId = 0;
                 if (!parseLiveIndex(argv[3], clientId)) return;
                 const auto target = liveClientTargetIds.find(clientId);
                 if (target != liveClientTargetIds.end() && spectateGhostIndex == target->second) {
                     spectateGhostIndex = -1;
                     liveFollowGhostIndex = -1;
                     liveFollowCenterInit = false;
                 }
                liveClientTargetIds.erase(clientId);
                 liveClientNames.erase(clientId);
                  liveClientTeams.erase(clientId);
                  livePlayerScores.erase(clientId);
            } else if (argv.size() >= 5 && argv[1] == "MsgClientNameChanged") {
                const int clientId = atoi(argv[4].c_str());
                if (clientId >= 0 && clientId < 1024) {
                    liveClientNames[clientId] = argv[3];
                    auto target = liveClientTargetIds.find(clientId);
                    if (target != liveClientTargetIds.end()) {
                        if (GhostEntry* ghost = liveGhosts.getMutableGhost(target->second))
                            ghost->playerName = argv[3];
                    }
                }
            } else if (argv.size() >= 6 && argv[1] == "MsgClientJoinTeam") {
                const int clientId = atoi(argv[4].c_str());
                const int teamId = atoi(argv[5].c_str());
                if (clientId >= 0 && clientId < 1024 && teamId >= 0 && teamId < 64) {
                    liveClientTeams[clientId] = teamId;
                    auto target = liveClientTargetIds.find(clientId);
                    if (target != liveClientTargetIds.end()) {
                        if (GhostEntry* ghost = liveGhosts.getMutableGhost(target->second))
                            ghost->teamId = teamId;
                    }
                }
            }
        });
         activeConn->setGhostCallback([this](const V12::GhostUpdate& update,
                                              const V12::PlayerGhostState* state) {
              if (update.operation == V12::GhostUpdate::Operation::Delete) {
                   auto& audio = Engine::instance().audio();
                   auto projectileSound = projectileSoundSources.find(update.index);
                   if (projectileSound != projectileSoundSources.end()) {
                       audio.releaseSource(projectileSound->second);
                       projectileSoundSources.erase(projectileSound);
                   }
                  for (int slot = 0; slot < 4; ++slot) {
                      const uint64_t key = (static_cast<uint64_t>(update.index) << 3) |
                                           static_cast<uint64_t>(slot);
                      auto sound = shapeBaseSoundSources.find(key);
                      if (sound != shapeBaseSoundSources.end()) {
                          audio.releaseSource(sound->second);
                          shapeBaseSoundSources.erase(sound);
                      }
                  }
                  if (spectateGhostIndex == (int)update.index) {
                     spectateGhostIndex = -1;
                     liveFollowGhostIndex = -1;
                     liveFollowCenterInit = false;
                 }
                 liveGhosts.deleteGhost((int)update.index);
                return;
            }
            if (!liveGhosts.hasGhost((int)update.index)) {
                const char* name = V12::ghostClassName(update.classId);
                liveGhosts.createGhost((int)update.index, update.classId,
                                       name ? name : "Player");
            }
             if (!state) return;
             const bool isProjectile = update.classId == 3 || update.classId == 6 ||
                 update.classId == 7 || update.classId == 9 || update.classId == 13 ||
                 update.classId == 18 || update.classId == 19 || update.classId == 27 ||
                 update.classId == 30 || update.classId == 32 || update.classId == 36 ||
                 update.classId == 44 || update.classId == 46;
             if (isProjectile && state->hasPosition && state->hasDatablock) {
                 auto dataIt = nativeDatablocks.find(state->datablockId);
                 if (dataIt != nativeDatablocks.end()) {
                     auto& audio = Engine::instance().audio();
                     const Point3F projectilePosition = Math::torquePointToYUp(
                         {state->position.x, state->position.y, state->position.z});
                     auto soundIt = projectileSoundSources.find(update.index);
                     if (soundIt == projectileSoundSources.end()) {
                         const auto& data = dataIt->second.decoded;
                         const uint32_t fire = ProjectileAudio::fireProfile(
                             audio.isUnderwater(), data.projectileFireSoundRef,
                             data.projectileWetFireSoundRef);
                         if (fire) playNativeAudioProfile(audio, nativeDatablocks, fire,
                                                          projectilePosition);
                         SoundSource* source = playNativeAudioProfile(audio,
                             nativeDatablocks, data.projectileSoundRef,
                             projectilePosition, true);
                         if (source) projectileSoundSources.emplace(update.index, source);
                     } else if (audio.isSourceAlive(soundIt->second)) {
                         soundIt->second->setPosition(projectilePosition);
                     }
                 }
             }
              if (update.operation == V12::GhostUpdate::Operation::Create &&
                  update.classId == 38 && state->hasPosition && state->hasDatablock) {
                  auto splashIt = nativeDatablocks.find(state->datablockId);
                  if (splashIt != nativeDatablocks.end() && splashIt->second.decoded.hasSplash) {
                      const Point3F position = Math::torquePointToYUp(
                          {state->position.x, state->position.y, state->position.z});
                      w->spawnSplashEffect(position, splashIt->second.decoded, nativeDatablocks);
                  }
              }
             GhostEntry* ghost = liveGhosts.getMutableGhost((int)update.index);
             if (!ghost) return;
             if (state->hasDamageState && state->damageState >= 2) {
                 auto& audio = Engine::instance().audio();
                 for (int slot = 0; slot < 4; ++slot) {
                     const uint64_t key = (static_cast<uint64_t>(update.index) << 3) |
                                          static_cast<uint64_t>(slot);
                     auto sound = shapeBaseSoundSources.find(key);
                     if (sound != shapeBaseSoundSources.end()) {
                         audio.releaseSource(sound->second);
                         shapeBaseSoundSources.erase(sound);
                     }
                 }
             }
             for (const auto& [clientId, targetId] : liveClientTargetIds) {
                 if (targetId == (int)update.index) {
                     auto name = liveClientNames.find(clientId);
                     if (name != liveClientNames.end()) ghost->playerName = name->second;
                     auto team = liveClientTeams.find(clientId);
                     if (team != liveClientTeams.end()) ghost->teamId = team->second;
                    auto score = livePlayerScores.find(clientId);
                     if (score != livePlayerScores.end()) ghost->score = score->second;
                     break;
                 }
             }
             auto target = liveTargets.find((uint16_t)update.index);
             if (target != liveTargets.end()) {
                  if (target->second.hasSensorGroup)
                      ghost->sensorGroup = target->second.sensorGroup;
                  if (target->second.hasName && !target->second.name.empty())
                     ghost->playerName = target->second.name;
                   if (target->second.hasSkin && !target->second.skin.empty())
                       ghost->skinName = target->second.skin;
                   if (target->second.hasType) ghost->targetType = target->second.type;
                   if (target->second.hasRenderFlags)
                       ghost->targetRenderFlags = target->second.renderFlags;
                  const bool isClientTarget = std::any_of(
                      liveClientTargetIds.begin(), liveClientTargetIds.end(),
                      [index = update.index](const auto& entry) { return entry.second == (int)index; });
                      ghost->isFlag = (target->second.renderFlags & 0x2) != 0 && !isClientTarget;
                  ghost->flagTeamId = ghost->isFlag ? target->second.sensorGroup : 0;
                  if (ghost->isFlag) ghost->teamId = target->second.sensorGroup;
              }
             ghost->position = {state->position.x, state->position.y, state->position.z};
            ghost->rotation = {state->rotation.x, state->rotation.y,
                               state->rotation.z, state->rotationW};
            ghost->hasRotation = state->hasRotation;
            ghost->health = state->health;
             if (state->hasDamageState)
                 ghost->damageState = state->damageState;
            ghost->energy = state->energy;
             ghost->headPitch = state->headPitch;
             ghost->headYaw = state->headYaw;
             ghost->barrelPitch = state->barrelPitch;
             ghost->barrelYaw = state->barrelYaw;
              ghost->hasTurretAim = state->hasTurretAim;
              ghost->isMoving = state->moving;
              for (int i = 0; i < 4; ++i) {
                  const auto& incoming = state->threads[i];
                  const bool changed = incoming.valid &&
                      (!ghost->threads[i].valid || ghost->threads[i].sequence != incoming.sequence ||
                       ghost->threads[i].state != incoming.state ||
                       ghost->threads[i].timescale != incoming.timescale ||
                       ghost->threads[i].position != incoming.position ||
                       ghost->threads[i].atEnd != incoming.atEnd);
                  ghost->threads[i].sequence = incoming.sequence;
                  ghost->threads[i].state = incoming.state;
                  ghost->threads[i].timescale = incoming.timescale;
                  ghost->threads[i].position = incoming.position;
                  ghost->threads[i].forward = incoming.forward;
                  ghost->threads[i].atEnd = incoming.atEnd;
                  ghost->threads[i].valid = incoming.valid;
                  if (changed) ghost->threadAnimTime = 0.0f;
              }
              for (int i = 0; i < 4; ++i)
                  ghost->soundThreads[i] = {state->soundThreads[i].profileId,
                                             state->soundThreads[i].playing,
                                             state->soundThreads[i].valid};
               if (state->hasCloak) {
                  ghost->cloaked = state->cloaked;
                  ghost->hasCloak = true;
              }
              if (state->hasShield) {
                  ghost->shieldLevel = state->shieldLevel;
                  ghost->hasShield = true;
              }
                for (int i = 0; i < 8; ++i) {
                   if (!state->mountedImages[i].valid) continue;
                   const bool wasFiring = ghost->mountedImages[i].isFiring;
                   ghost->mountedImages[i] = {};
                  ghost->mountedImages[i].datablockId =
                      (int16_t)state->mountedImages[i].datablockId;
                  ghost->mountedImages[i].loaded = state->mountedImages[i].loaded;
                   ghost->mountedImages[i].isFiring = state->mountedImages[i].firing;
                   if (wasFiring != ghost->mountedImages[i].isFiring)
                       ghost->threadAnimTime = 0.0f;
                  if (state->mountedImages[i].datablockId >= 0) {
                      const auto image = nativeDatablocks.find(
                          (uint32_t)state->mountedImages[i].datablockId);
                      if (image != nativeDatablocks.end()) {
                          ghost->mountedImages[i].mountPoint =
                              image->second.decoded.hasMountPoint
                                  ? (int)image->second.decoded.mountPoint : 0;
                          ghost->mountedImages[i].shapePath =
                              image->second.decoded.shapeFile;
                      }
                  }
               }
               for (int i = 0; i < 6; ++i) {
                   ghost->wheels[i].angularVelocity = state->wheels[i].angularVelocity;
                   ghost->wheels[i].suspension = state->wheels[i].suspension;
                   ghost->wheels[i].lateral = state->wheels[i].lateral;
                   ghost->wheels[i].valid = state->wheels[i].valid;
               }
              if (state->hasDatablock) {
                  auto shapeIt = nativeDatablockShapes.find(state->datablockId);
                  if (shapeIt != nativeDatablockShapes.end())
                      ghost->shapeName = shapeIt->second;
                  auto dataIt = nativeDatablocks.find(state->datablockId);
                  if (dataIt != nativeDatablocks.end() &&
                      dataIt->second.decoded.hasProjectileScale) {
                      const auto& scale = dataIt->second.decoded.projectileScale;
                      ghost->projectileScale = {
                          std::isfinite(scale.x) && scale.x > 0.0f ? scale.x : 1.0f,
                          std::isfinite(scale.y) && scale.y > 0.0f ? scale.y : 1.0f,
                          std::isfinite(scale.z) && scale.z > 0.0f ? scale.z : 1.0f};
                      ghost->hasProjectileScale = true;
                  }
              }
         });
         activeConn->setProjectileImpactCallback(
             [this](uint16_t ghostIndex, uint16_t classId,
                    const V12::ProjectileImpact& impact) {
                 if (!w) return;
                 V12::DecodedDataBlock fallbackData;
                 const V12::DecodedDataBlock* projectileData = &fallbackData;
                 if (impact.hasDatablock) {
                     auto projectileIt = nativeDatablocks.find(impact.datablockId);
                     if (projectileIt != nativeDatablocks.end())
                         projectileData = &projectileIt->second.decoded;
                 }
                 const V12::DecodedDataBlock* explosionData = nullptr;
                 if (projectileData->projectileExplosionRef != 0) {
                     auto explosionIt = nativeDatablocks.find(
                         (uint16_t)projectileData->projectileExplosionRef);
                     if (explosionIt != nativeDatablocks.end())
                     explosionData = &explosionIt->second.decoded;
                 }
                 const Point3F position = Math::torquePointToYUp(
                     {impact.position.x, impact.position.y, impact.position.z});
                 Point3F normal = Math::torquePointToYUp(
                     {impact.normal.x, impact.normal.y, impact.normal.z});
                 const float normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                 if (!std::isfinite(normalLength) || normalLength < 0.001f)
                     normal = {0.0f, 1.0f, 0.0f};
                 else {
                     normal.x /= normalLength;
                     normal.y /= normalLength;
                     normal.z /= normalLength;
                 }
                  w->spawnExplosionEffect(position, projectileData, explosionData,
                                            &nativeDatablocks, normal);
                  const V12::DecodedDataBlock* soundExplosion = explosionData;
                  if (projectileData->projectileUnderwaterExplosionRef != 0 &&
                      Engine::instance().audio().isUnderwater()) {
                      auto underwater = nativeDatablocks.find(
                          projectileData->projectileUnderwaterExplosionRef);
                      if (underwater != nativeDatablocks.end())
                          soundExplosion = &underwater->second.decoded;
                  }
                  if (soundExplosion && soundExplosion->explosion.soundProfileRef != 0)
                      playNativeAudioProfile(Engine::instance().audio(), nativeDatablocks,
                                             soundExplosion->explosion.soundProfileRef, position);
                  auto projectileSound = projectileSoundSources.find(ghostIndex);
                  if (projectileSound != projectileSoundSources.end()) {
                      Engine::instance().audio().releaseSource(projectileSound->second);
                      projectileSoundSources.erase(projectileSound);
                  }
                  w->removeProjectileTrail((int)ghostIndex);
             });
          activeConn->setAudioCallback([this](const V12::ServerEvent& event) {
             auto& audio = Engine::instance().audio();
             if (!audio.config().enabled || audio.config().sfxVolume <= 0)
                 return;
              // Audio profiles are indexed by the server's profile table. The
              // decoded datablock table is authoritative for live packets.
              // Script scanning remains a fallback for older demos that do
              // not contain their datablock definitions.
              auto nativeProfile = nativeDatablocks.find(
                  static_cast<uint32_t>(event.audioProfileId));
              if (nativeProfile != nativeDatablocks.end() &&
                  nativeProfile->second.className == "AudioProfile") {
                  const Point3F position = event.audioHasPosition
                      ? Math::torquePointToYUp({event.audioPosition.x,
                                                event.audioPosition.y,
                                                event.audioPosition.z})
                      : Point3F{};
                  playNativeAudioProfile(audio, nativeDatablocks,
                                         static_cast<uint32_t>(event.audioProfileId),
                                         position);
                  return;
              }
              scanAudioProfiles();
             if (event.audioProfileId < 0 ||
                 event.audioProfileId >= (int)s_audioProfiles.size()) return;
             const auto& profile = s_audioProfiles[event.audioProfileId];
             auto* buffer = audio.loadSound(profile.filename.c_str());
             if (!buffer) return;
             auto* source = audio.createSource();
             if (!source) return;
             source->setVolume(0.3f * audio.config().masterVolume * audio.config().sfxVolume);
             if (event.audioHasPosition)
                 source->setPosition(Math::torquePointToYUp({event.audioPosition.x,
                                                              event.audioPosition.y,
                                                              event.audioPosition.z}));
              source->play(buffer);
          });
        if (!activeConn->connect(host, port)) {
            Console::instance().printf(LogLevel::Error, "Unable to connect to %s:%d", host, port);
            disconnectedCleanup();
        }
      }
}

int Game::liveClockRemainingMs() const {
    if (liveClockDurationMs_ <= 0 || liveClockReceivedAt_ <= 0.0) return 0;
    const double elapsed = Engine::instance().timer().now() - liveClockReceivedAt_;
    return std::max(0, liveClockDurationMs_ - (int)(elapsed * 1000.0));
}

void Game::reconcile(const Point3F& serverPos, const Point3F& serverVel, uint32_t lastProcessedSeq) {
    if (!pl) return;

    // Pop all moves that were processed by server
    while (!pendingMoves.empty() && pendingMoves.front().seq <= lastProcessedSeq)
        pendingMoves.pop_front();

    // Set player to authoritative server state
    pl->setPosition(serverPos);
    pl->setVelocity(serverVel);

    // Re-apply pending moves to stay ahead of server
    Physics physics;
    for (auto& move : pendingMoves) {
        // Reconstruct InputMove from stored data
        physics.update(pl, move.dt, move.input);
    }
}

static std::string extractMapName(const std::string& missionPath) {
    // Extract base name from paths like:
    // "Missions/Katabatic.mis", "base/missions/Training1.mis", "@vl2/missions.vl2/Katabatic.mis"
    std::string name = missionPath;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Remove trailing whitespace
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    return name;
}

// Returns true if a ghost with this class name should be rendered as a 3D model
// (as opposed to being a world-level object already rendered by World::render)
static bool isRenderableGhostClass(const std::string& className) {
    if (className == "InteriorInstance" || className == "StaticShape" ||
        className == "ScopeAlwaysShape" || className == "TSStatic" ||
        className == "TerrainBlock" || className == "Sky" || className == "Sun" ||
        className == "Lightning" || className == "WaterBlock" ||
        className == "MissionArea" || className == "ForceFieldBare")
        return false;
    return true;
}

static bool isEffectOnlyGhostClass(const std::string& className) {
    return className.find("Projectile") != std::string::npos ||
           className == "EnergyBolt" || className == "LinearFlare" ||
           className == "Splash";
}

static void appendWheelNodeOverrides(const GhostEntry& ghost, DTSShape& shape,
                                     float dt, DTSShape::NodeOverride* overrides,
                                     int& overrideCount, int maxOverrides,
                                     float* wheelRotation) {
    if (ghost.className != "WheeledVehicle" || !shape.nativeDTS || !overrides ||
        !wheelRotation)
        return;
    for (int i = 0; i < 6 && overrideCount < maxOverrides; ++i) {
        if (!ghost.wheels[i].valid) continue;
        // Torque's WheeledVehicle uses hub nodes as the wheel anchors. Keep
        // wheelN as a compatibility alias for authored shapes that expose it.
        int wheelNode = shape.findNode("hub" + std::to_string(i));
        if (wheelNode < 0)
            wheelNode = shape.findNode("wheel" + std::to_string(i));
        if (wheelNode < 0) continue;
        if (wheelNode >= (int)shape.defaultTransforms.size()) continue;

        wheelRotation[i] += ghost.wheels[i].angularVelocity * std::max(dt, 0.0f);
        MatrixF lateralAndSuspension;
        lateralAndSuspension.setTranslation({ghost.wheels[i].lateral, 0.0f,
                                              ghost.wheels[i].suspension});
        MatrixF spin;
        spin.setRotationAxis({1, 0, 0}, wheelRotation[i]);
        overrides[overrideCount].nodeIndex = wheelNode;
        overrides[overrideCount].transform = shape.defaultTransforms[wheelNode] *
                                              lateralAndSuspension * spin;
        ++overrideCount;
    }
}

DTSShape* Game::getOrLoadDemoShape(const std::string& className, const std::string& skinName,
                                   const std::string& datablockInstance) {
    if (className == "Camera" || className == "AIObjective" ||
        className == "StationFXPersonal")
        return nullptr;

    // Datablock and skin references determine identity. Do not infer an asset
    // variant from a name fragment; scripts and streamed datablocks own that
    // relationship.
    std::string cacheKey = className + "\n" + datablockInstance + "\n" + skinName;

    // Use cached shape if available
    auto it = demoShapeCache.find(cacheKey);
    if (it != demoShapeCache.end())
        return it->second.loaded ? &it->second : nullptr;

    // Find the shape path for this class
    auto& fs = Engine::instance().fs();
    // First try datablock InstanceName lookup (from .cs scripts + .mis)
    std::string dbShapePath;
    if (!datablockInstance.empty()) {
        auto dbIt = w->datablockShapes.find(datablockInstance);
        if (dbIt != w->datablockShapes.end()) dbShapePath = dbIt->second;
        else dbShapePath = datablockInstance;
    }
    const char* path = dbShapePath.empty() ? nullptr : dbShapePath.c_str();
    if (!path) {
        demoShapeCache[cacheKey] = DTSShape{};
        Console::instance().printf(LogLevel::Error,
            "Demo: no native shapeFile datablock for class '%s'", className.c_str());
        return nullptr;
    }

    // Load only the native asset named by the mission/datablock.
    std::vector<uint8_t> data = fs.read(path);
    if (data.empty()) {
        demoShapeCache[cacheKey] = DTSShape{};
        Console::instance().printf(LogLevel::Error,
            "Demo: native shapeFile '%s' could not be loaded for class '%s'",
            path, className.c_str());
        return nullptr;
    }

    DTSShape shape;
    shape.name = className;
    if (!shape.load(data.data(), data.size())) {
        demoShapeCache[cacheKey] = DTSShape{};
        Console::instance().printf(LogLevel::Error,
            "Demo: native DTS failed to load for class '%s' (%s)",
            className.c_str(), path);
        return nullptr;
    }

    Console::instance().printf(LogLevel::Debug, "Demo: loaded shape for '%s' (%zu meshes)",
        path, shape.meshes.size());

    auto inserted = demoShapeCache.emplace(cacheKey, std::move(shape));
    return inserted.first->second.loaded ? &inserted.first->second : nullptr;
}

bool Game::playDemo(const char* path) {
    if (!path || !path[0]) {
        Console::instance().printf(LogLevel::Warn, "Usage: playdemo <path>");
        return false;
    }
    Console::instance().printf(LogLevel::Info, "Loading demo: %s", path);
    demoPlaying = false;
    demoPaused = false;
    clearMissionAudio();
    Engine::instance().audio().stopAll();
    clearProjectileAudio();
    auto failDemoLoad = [&]() {
        demoPlaying = false;
        demoFastForward = false;
        if (demoParser) {
            delete demoParser;
            demoParser = nullptr;
        }
        if (hud) hud->resetState();
        setState(MenuScreen);
        menu().setActive(false);
        auto& gui = Engine::instance().guiRenderer();
        gui.clearDialogs();
        resetGameplayGui(gui);
        if (gui.findControl("LobbyGui")) gui.setContentImmediate("LobbyGui");
        else gui.setContentImmediate("LaunchGui");
        Engine::instance().platform().setRelativeMouse(false);
        Engine::instance().platform().showMouse(true);
    };

    // Clean up previous parser
    if (demoParser) { delete demoParser; demoParser = nullptr; }
    demoParser = new DemoParser;

    bool demoLoaded = demoParser->loadFile(path);
    if (!demoLoaded) {
        const auto mounted = Engine::instance().fs().read(path);
        if (!mounted.empty())
            demoLoaded = demoParser->loadData(mounted.data(), mounted.size());
    }
    if (!demoLoaded) {
        Console::instance().printf(LogLevel::Error, "Failed to load demo file");
        failDemoLoad();
        return false;
    }

    const auto& hdr = demoParser->getHeader();
    const auto& ib = demoParser->getInitialBlock();
    Console::instance().printf(LogLevel::Info, "  Identity: %s", hdr.identString.c_str());
    Console::instance().printf(LogLevel::Info, "  Protocol: 0x%08X", (unsigned)hdr.protocolVersion);
    Console::instance().printf(LogLevel::Info, "  InitBlock: %u bytes", (unsigned)hdr.initialBlockSize);
    Console::instance().printf(LogLevel::Info, "  Mission: %s", ib.missionName.empty() ? "(unknown)" : ib.missionName.c_str());

    std::string loadMap = extractMapName(ib.missionName);
    if (loadMap.empty()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial block did not provide a mission name");
        failDemoLoad();
        return false;
    }
    auto& demoFs = Engine::instance().fs();
    std::string missionPath = "missions/" + loadMap + ".mis";
    if (!demoFs.fileExists(missionPath.c_str())) {
        std::string suffix = loadMap;
        const auto separator = suffix.rfind('_');
        if (separator != std::string::npos && separator + 1 < suffix.size())
            suffix = suffix.substr(separator + 1);

        std::vector<std::string> missionFiles;
        demoFs.listFiles("missions/*.mis", missionFiles);
        std::string matchedMission;
        for (const auto& candidate : missionFiles) {
            const auto slash = candidate.rfind('/');
            const auto dot = candidate.rfind('.');
            const std::string base = candidate.substr(
                slash == std::string::npos ? 0 : slash + 1,
                dot == std::string::npos ? std::string::npos : dot - slash - 1);
            if (strcasecmp(base.c_str(), suffix.c_str()) == 0) {
                if (!matchedMission.empty()) {
                    matchedMission.clear();
                    break;
                }
                matchedMission = base;
            }
        }
        if (!matchedMission.empty()) {
            loadMap = matchedMission;
            missionPath = "missions/" + loadMap + ".mis";
        }
    }
    if (!demoFs.fileExists(missionPath.c_str())) {
        Console::instance().printf(LogLevel::Error,
            "Demo: native mission '%s' is not mounted", missionPath.c_str());
        failDemoLoad();
        return false;
    }
    Console::instance().printf(LogLevel::Info, "Loading mission map: %s", loadMap.c_str());
    State prevState = gameState;
    startLocalGame(loadMap.c_str());
    if (gameState != Playing) {
        gameState = prevState;
        Console::instance().printf(LogLevel::Error,
            "Demo: native mission '%s' failed to load", missionPath.c_str());
        failDemoLoad();
        return false;
    }
    Engine::instance().guiRenderer().popDialog("ConsoleDlg");
    if (auto* console = Engine::instance().guiRenderer().findControl("ConsoleDlg"))
        console->visible = false;
    if (auto* overlay = Engine::instance().guiRenderer().findControl("FrameOverlayGui"))
        overlay->visible = false;

    // Reset demo path history
    demoPath.clear();
    demoPathCount = 0;
    demoTrails.clear();

    // Reset stats
    demoPacketsParsed = 0;
    demoTime = 0;
    demoInterpolationDt = 0;
    demoPaused = false;
    demoEventLog.clear();
    demoAudioEventsPlayed.clear();
    int totalBlocks = demoParser->getBlockCount();
    const int moveBlocks = demoParser->getMoveBlockCount();
    demoTotalTime = hdr.demoLengthMs > 0 ? hdr.demoLengthMs / 1000.0f :
        (moveBlocks > 0 ? moveBlocks * 0.032f : 1.0f);
    demoBlocksTotal = totalBlocks;
    demoBlocksDone = 0;
    demoSnapshots.clear();
    demoSnapshots[0] = demoParser->captureSnapshot();
    demoFastForward = false; // real-time when invoked from console
    demoFirstPersonCam = demoParser->getInitialBlock().firstPerson;
    controlGhostIndex = demoParser->getInitialBlock().controlObjectGhostIndex;
    demoAuthoredCamera = false;
    demoCameraFov = -1.0f;
    demoOrbitCam = false;
    demoHasOrientation = false;
    demoViewYaw = 0.0f;
    demoViewPitch = 0.0f;

    Console::instance().printf(LogLevel::Info,
        "  Total blocks: %d, move ticks: %d (%.1f seconds)",
        totalBlocks, moveBlocks, demoTotalTime);
    Console::instance().printf(LogLevel::Info, "Demo loaded, starting playback...");
    demoPlaying = true;
    setState(Playing);
    return true;
}

void Game::stopDemoPlayback() {
    demoPlaying = false;
    demoFastForward = false;
    Engine::instance().audio().setPlaybackRate(1.0f);
    Engine::instance().audio().stopAll();
    clearMissionAudio();
    demoAudioEventsPlayed.clear();
    demoPath.clear();
    demoTrails.clear();
    if (w) w->clearEffects();
    targetFinderShown = false;
    if (hud) hud->resetState();
    resetGameplayGui(Engine::instance().guiRenderer());
    if (demoParser) {
        delete demoParser;
        demoParser = nullptr;
    }
    setState(MenuScreen);
    menu().setActive(false);
    auto& gui = Engine::instance().guiRenderer();
    if (gui.findControl("LobbyGui")) gui.setContentImmediate("LobbyGui");
    else gui.setContentImmediate("LaunchGui");
}

void Game::toggleDemoPause() {
    demoPaused = !demoPaused;
    if (demoPaused)
        Engine::instance().audio().pauseAll();
    else {
        Engine::instance().audio().setPlaybackRate(
            (demoFastForward || currentInput.jet) ? 4.0f : 1.0f);
        Engine::instance().audio().resumeAll();
    }
}

void Game::disconnectedCleanup() {
    if (auto* ts = Engine::instance().script().ts(); ts && ts->hasFunction("DisconnectedCleanup"))
        ts->callFunction("DisconnectedCleanup", {});
    if (auto* ts = Engine::instance().script().ts()) ts->clearScheduledEvents();
    if (activeConn) activeConn->disconnect();
    auto& audio = Engine::instance().audio();
    audio.stopAll();
    clearProjectileAudio();
    clearMissionAudio();
    liveGhosts.clear();
    nativeDatablockShapes.clear();
    nativeDatablocks.clear();
    liveTargets.clear();
    liveSensorGroupListenMasks.clear();
    liveMissionCrc = 0;
    liveTeamScores.clear();
    livePlayerScores.clear();
    liveClientTargetIds.clear();
    liveClientNames.clear();
    liveClientTeams.clear();
    liveMatchStarted_ = false;
    liveMatchEnded_ = false;
    liveMissionDisplayName_.clear();
    liveMissionType_.clear();
    liveClockDurationMs_ = 0;
    liveClockReceivedAt_ = 0.0;
    liveLoadInfoLines_.clear();
    liveSpectateInit = false;
    spectateGhostIndex = -1;
    liveFollowGhostIndex = -1;
    liveFollowCenterInit = false;
    targetFinderShown = false;
    demoTrails.clear();
    if (w) w->clearEffects();
    if (w) w->resetTriggerTracking();
    if (hud) hud->resetState();
    auto& gui = Engine::instance().guiRenderer();
    gui.clearDialogs();
    resetGameplayGui(gui);
    Engine::instance().platform().setRelativeMouse(false);
    Engine::instance().platform().showMouse(true);
    setState(MenuScreen);
    menu().setActive(false);
}

void Game::resetLiveMissionState() {
    if (auto* ts = Engine::instance().script().ts()) ts->clearScheduledEvents();
    auto& audio = Engine::instance().audio();
    audio.stopAll();
    clearProjectileAudio();
    clearMissionAudio();
    liveGhosts.clear();
    nativeDatablockShapes.clear();
    nativeDatablocks.clear();
    liveTargets.clear();
    liveSensorGroupListenMasks.clear();
    liveTeamScores.clear();
    livePlayerScores.clear();
    liveClientTargetIds.clear();
    liveClientNames.clear();
    liveClientTeams.clear();
    liveMatchStarted_ = false;
    liveMatchEnded_ = false;
    liveMissionDisplayName_.clear();
    liveMissionType_.clear();
    liveClockDurationMs_ = 0;
    liveClockReceivedAt_ = 0.0;
    liveLoadInfoLines_.clear();
    liveSpectateInit = false;
    liveSpectateRespawned = false;
    liveFollowGhostIndex = -1;
    liveFollowCenterInit = false;
    spectateGhostIndex = -1;
    freeCamActive = false;
    freeCamPos = {0, 10, 0};
    freeCamTarget = {0, 10, -1};
    freeCamRot = {0, 0, 0};
    showScoreboard = false;
    targetFinderShown = false;
    serverPlayerGhostIndex = 0;
    serverPlayerGhostSynced = false;
    damageFlash = -1.0f;
    whiteOut = -1.0f;
    shakeIntensity = 0.0f;
    shakeOffset = {0, 0, 0};
    demoTrails.clear();
    if (w) w->clearEffects();
    if (w) w->resetTriggerTracking();
    if (hud) hud->resetState();
    auto& gui = Engine::instance().guiRenderer();
    gui.clearDialogs();
    gui.setContent("PlayGui");
    resetGameplayGui(gui);
}

void Game::toggleTargetFinder() {
    const bool available = demoPlaying || (activeConn && activeConn->isConnected() &&
                                           activeConn->isObserverMode());
    if (available) targetFinderShown = !targetFinderShown;
}

void Game::selectSpectateTarget(int ghostIndex) {
    if (ghostIndex < 0) return;
    if (demoPlaying) {
        if (demoParser && demoParser->getGhostTracker().getGhost(ghostIndex)) {
            spectateGhostIndex = ghostIndex;
            demoAuthoredCamera = false;
            demoFirstPersonCam = true;
            demoOrbitCam = false;
        }
    } else if (activeConn && activeConn->isObserverMode() && liveGhosts.getGhost(ghostIndex)) {
        spectateGhostIndex = ghostIndex;
        liveFollowGhostIndex = -1;
        liveFollowCenterInit = false;
    }
}

void Game::applyInput(const InputMove& input) {
    currentInput = input;
    showScoreboard = input.showScoreboard;

    // Demo pause toggle on rising edge of P key
    if (demoPlaying && input.demoPause && !previousDemoPause)
        toggleDemoPause();
    previousDemoPause = input.demoPause;

    // Demo step frame on rising edge of . key
    if (demoPlaying && input.demoStepFrame && !previousDemoStep)
        requestDemoStep();
    previousDemoStep = input.demoStepFrame;

    // Demo event log toggle on rising edge of E key
    if (demoPlaying && input.demoShowEvents && !previousDemoEvent)
        toggleDemoEvents();
    previousDemoEvent = input.demoShowEvents;

    // Spectate cycle on the observer right-mouse action or R during playback.
    const bool observerCycle = observerCyclePressed(input.reload, input.altFire);
    if (demoPlaying && observerCycle && !previousObserverCycle) {
        if (demoParser) {
            auto indices = demoParser->getGhostTracker().getAllIndices();
            // Find all Player/MPB class ghosts
            std::vector<int> players;
             for (int i : indices) {
                 const GhostEntry* g = demoParser->getGhostTracker().getGhost(i);
                  if (g && (g->className == "Player" || g->className == "MPB") &&
                      g->damageState == 0)
                      players.push_back(i);
            }
            if (!players.empty()) {
                // Find current spectate index (or control index) in player list
                int current = (spectateGhostIndex >= 0) ? spectateGhostIndex : controlGhostIndex;
                auto it = std::find(players.begin(), players.end(), current);
                if (it != players.end() && ++it != players.end())
                    spectateGhostIndex = *it;
                else
                    spectateGhostIndex = players[0];
                Console::instance().printf(LogLevel::Info, "Spectating ghost %d", spectateGhostIndex);
            }
        }
    } else if (observerCycle && !previousObserverCycle && activeConn && activeConn->isObserverMode()) {
        const auto observer = activeConn->observerSnapshot();
        const auto indices = liveGhosts.getAllIndices();
        std::vector<int> players;
        for (int index : indices) {
            const GhostEntry* g = liveGhosts.getGhost(index);
            if (!g || (g->className != "Player" && g->className != "MPB") ||
                !isSensorGroupTargetVisible(observer.playerSensorGroup, g->sensorGroup)) continue;
            players.push_back(index);
        }
        if (!players.empty()) {
            auto it = std::find(players.begin(), players.end(), spectateGhostIndex);
            spectateGhostIndex = it != players.end() && ++it != players.end() ? *it : players.front();
            liveFollowGhostIndex = -1;
            liveFollowCenterInit = false;
        }
    }
    previousObserverCycle = observerCycle;
}

void Game::resetInputState() {
    currentInput = {};
    previousDemoPause = false;
    previousDemoStep = false;
    previousDemoEvent = false;
    previousObserverCycle = false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shape Viewer — browse all .dts shapes from the data paths
// ═══════════════════════════════════════════════════════════════════════════

#include <filesystem>
namespace fs = std::filesystem;

void Game::enterShapeViewer() {
    shapeViewerFiles.clear();
    shapeViewerIndex = 0;
    shapeViewerActive = true;
    shapeViewerYaw = 0.6f;
    shapeViewerPitch = 0.25f;
    shapeViewerAnimTime = 0;

    // Scan all mounted archives for .dts files
    auto& fsys = Engine::instance().fs();
    std::vector<std::string> allFiles;
    fsys.listFiles(nullptr, allFiles);  // nullptr = list all files

    for (auto& f : allFiles) {
        if (f.size() > 4 && f.rfind(".dts") == f.size() - 4) {
            shapeViewerFiles.push_back(f);
        }
    }

    // Also scan the filesystem data directories directly
    std::string dataBase = torchDataDir();
    for (auto& baseDir : std::initializer_list<std::string>{dataBase, "base"}) {
        std::error_code ec;
        if (!fs::is_directory(baseDir, ec)) continue;
        for (auto& entry : fs::recursive_directory_iterator(baseDir, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file()) continue;
            auto& p = entry.path();
            if (p.extension() == ".dts") {
                std::string rel = p.string();
                // Strip the data dir prefix to get a relative path
                for (auto* prefix : {dataBase.c_str(), "base/"}) {
                    auto pos = rel.find(prefix);
                    if (pos != std::string::npos) {
                        rel = rel.substr(pos + strlen(prefix));
                        break;
                    }
                }
                // Deduplicate
                bool dup = false;
                for (auto& existing : shapeViewerFiles)
                    if (existing == rel) { dup = true; break; }
                if (!dup) shapeViewerFiles.push_back(rel);
            }
        }
    }

    // Sort and deduplicate
    std::sort(shapeViewerFiles.begin(), shapeViewerFiles.end());
    shapeViewerFiles.erase(std::unique(shapeViewerFiles.begin(), shapeViewerFiles.end()), shapeViewerFiles.end());

    Console::instance().printf(LogLevel::Info, "Shape Viewer: found %zu .dts files", shapeViewerFiles.size());

    // An optional viewer selection may identify any native DTS asset.
    if (const char* svStart = getenv("SV_START")) {
        for (int i = 0; i < (int)shapeViewerFiles.size(); i++)
            if (shapeViewerFiles[i].find(svStart) != std::string::npos) { shapeViewerIndex = i; break; }
    }

    shapeViewerLoadCurrent();
}

void Game::shapeViewerNext() {
    if (shapeViewerFiles.empty()) return;
    shapeViewerIndex = (shapeViewerIndex + 1) % (int)shapeViewerFiles.size();
    shapeViewerLoadCurrent();
}

void Game::shapeViewerPrev() {
    if (shapeViewerFiles.empty()) return;
    shapeViewerIndex = (shapeViewerIndex - 1 + (int)shapeViewerFiles.size()) % (int)shapeViewerFiles.size();
    shapeViewerLoadCurrent();
}

void Game::shapeViewerLoadCurrent() {
    if (shapeViewerFiles.empty()) return;
    auto& fsys = Engine::instance().fs();
    const std::string& path = shapeViewerFiles[shapeViewerIndex];

    auto data = fsys.read(path.c_str());
    if (data.empty()) {
        Console::instance().printf(LogLevel::Warn, "Shape Viewer: cannot read '%s'", path.c_str());
        return;
    }

    shapeViewerShape = DTSShape{};
    shapeViewerShape.name = path;
    if (!shapeViewerShape.load(data.data(), data.size())) {
        Console::instance().printf(LogLevel::Warn, "Shape Viewer: failed to load '%s'", path.c_str());
        return;
    }

    shapeViewerAnimTime = 0;
    // Reset bounds for camera framing
    shapeViewerBoundsInit = false;

    Console::instance().printf(LogLevel::Info, "Shape Viewer [%d/%zu]: '%s' (%zu meshes, %zu nodes, %zu anims)",
        shapeViewerIndex + 1, (int)shapeViewerFiles.size(), path.c_str(),
        shapeViewerShape.meshes.size(), shapeViewerShape.nodes.size(),
        shapeViewerShape.animations.size());

    // Diagnostic: report bone keyframe counts
    for (size_t ai = 0; ai < shapeViewerShape.animations.size(); ai++) {
        auto& a = shapeViewerShape.animations[ai];
        if (!a.keyframes.empty()) {
            Console::instance().printf(LogLevel::Info, "  anim[%zu] '%s': %zu BONE keyframes, %zu obj keyframes",
                ai, a.name.c_str(), a.keyframes.size(), a.objectKeyframes.size());
        }
    }
}
