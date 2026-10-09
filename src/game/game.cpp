#include "sim/engine_classes.h"
#include "game/game.h"
#include "game/live_move.h"
#include "game/voice_stream.h"
#include "audio/gsm_codec.h"
#include "sim/client_targets.h"
#include "sim/sim_state.h"
#include "game/player_animation.h"
#include "game/shape_lighting.h"
#include "game/collision.h"
#include "render/dts_loader.h"
#include "game/precipitation_parity.h"
#include "game/movement.h"
#include "game/damage_parity.h"
#include "render/material_parity.h"
#include "render/environment_commands.h"
#include "render/projected_shadows.h"
#include "render/dif_lighting_instance.h"
#include "game/timeline_random.h"
#include "core/timer.h"
#include "game/decal_runtime.h"
#include <GL/glew.h>
#include "game/demo.h"
#include "net/v12_registry.h"
#include "game/hud.h"
#include "game/item_parity.h"
#include "game/link_beam.h"
#include "game/projectile_physics.h"
#include "game/item_physics.h"
#include "game/dts_triggers.h"
#include "net/remote_command.h"
#include "game/material_property_map.h"
#include "game/physics.h"
#include "game/time_scale.h"
#include "game/projectile_audio.h"
#include "game/wind.h"
#include "game/water_parity.h"
#include "game/observer_parity.h"
#include "game/ghost_parity.h"
#include "game/demo_text.h"
#include "game/mission_parser.h"
#include "game/mission_rules.h"
#include "game/mission_discovery.h"
#include "game/objective_parity.h"
#include "game/ctf_runtime.h"
#include "game/trigger.h"
#include "game/animation_parity.h"
#include "sim/game_connection.h"
#include "sim/player.h"
#include "game/particle_parity.h"
#include "game/sky_parity.h"
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
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cctype>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <utility>

struct Game::VoicePlayback {
    TorchGsm::Decoder decoder;
    SoundSource* source = nullptr;
    int lastSequence = -1;
    std::chrono::steady_clock::time_point lastReceived = std::chrono::steady_clock::now();
    bool finished = false;
};

static constexpr int kMaxPrecipitationDrops = 65536;

static int parsePrecipitationDropCount(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 0) return 0;
    return (int)std::min<long>(value, kMaxPrecipitationDrops);
}

static void appendWheelNodeOverrides(const GhostEntry& ghost, DTSShape& shape,
                                     float dt, DTSShape::NodeOverride* overrides,
                                     int& overrideCount, int maxOverrides,
                                     float* wheelRotation);

// Forward declarations

static const char* liveFlagStatus(const std::string& status) {
    switch (CtfRuntime::classifyStatusToken(status)) {
        case CtfRuntime::StatusToken::Home: return "home";
        case CtfRuntime::StatusToken::Field: return "field";
        case CtfRuntime::StatusToken::Held: return "held";
    }
    return "home";
}

// GameBase::onTargetInfoChanged: the ghost shows its target's current info;
// a slot with none (freed, or not sent yet) leaves it blank.
static void applyLiveTargetInfoToGhost(GhostEntry& ghost,
                                       const V12::ServerEvent::TargetInfo* target) {
    ghost.playerName = target && target->hasName ? target->name : std::string();
    ghost.skinName = target && target->hasSkin ? target->skin : std::string();
    ghost.targetType = target && target->hasType ? target->type : std::string();
    if (target && target->hasSensorGroup) ghost.sensorGroup = target->sensorGroup;
    ghost.targetRenderFlags = target && target->hasRenderFlags ? target->renderFlags : 0;
    // Render bit 0x2 marks a CTF flag's target; on a client target it marks
    // the flag's carrier instead (CTFGame.cs).
    ghost.isFlag = (ghost.targetRenderFlags & 0x2) != 0 &&
                   !TargetNames::isClientType(ghost.targetType) &&
                   !ObserverParity::isPlayerClass(ghost.className);
    ghost.flagTeamId = ghost.isFlag ? ghost.sensorGroup : 0;
    if (ghost.isFlag) ghost.teamId = ghost.sensorGroup;
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
        world.datablockShapes[object->name.empty() ? name : object->name] = path;
        found++;
    }
    Console::instance().printf(LogLevel::Debug, "  loaded datablock shapes from script objects: %d found", found);
}

// Check if a .mis mission object class should be rendered as a shape
static bool isRenderableMissionShape(const std::string& className) {
    // Mission class names are case-insensitive at the console/script layer.
    // Keeping this lookup case-sensitive made custom missions silently omit
    // otherwise valid shape objects when their class spelling differed.
    std::string normalized = className;
    for (char& c : normalized) c = (char)std::tolower((unsigned char)c);
    if (normalized == "interiorinstance" || normalized == "tsstatic" ||
        normalized == "staticshape" || normalized == "scopealwaysshape" ||
        normalized == "turret" || normalized == "item" ||
        normalized == "camera" || normalized == "waypoint" || normalized == "marker" ||
        normalized == "beaconobject" || normalized == "debris" ||
        normalized == "wheeledvehicle" || normalized == "hovervehicle" ||
        normalized == "flyingvehicle" || normalized == "forcefieldbare" ||
        normalized == "sentry" || normalized == "shrike" || normalized == "turbograv" ||
        normalized == "wildcat" || normalized == "shield" || normalized == "vehicle")
        return true;
    // Vehicle subclasses (VehicleData-derived)
    if (normalized.size() > 9 && normalized.compare(normalized.size() - 9, 9, "vehicle") == 0)
        return true;
    if (normalized.size() > 5 && normalized.compare(normalized.size() - 5, 5, "turret") == 0)
        return true;
    return false;
}

static bool isMissionLifecycleObject(const World::WorldObject& object) {
    // Class callbacks also apply to unnamed SimObjects. Only the optional
    // object-specific callback requires an object name.
    return !object.className.empty();
}

static void dispatchMissionLifecycle(const World::WorldObject& object, const char* event) {
    if (!ScriptEngine::exists() || !isMissionLifecycleObject(object)) return;
    auto* ts = ScriptEngine::instance().ts();
    if (!ts) return;

    // Torque resolves an object method before falling back to its class
    // method.  This matters for missions that customize one trigger or item
    // without replacing the shared datablock class callback.
    if (!object.objectName.empty()) {
        const std::string objectCallback = object.objectName + "::" + event;
        if (ts->hasFunction(objectCallback)) {
            ts->callFunction(objectCallback, {VMValue(object.objectName)});
            return;
        }
    }
    const std::string classCallback = object.className + "::" + event;
    if (ts->hasFunction(classCallback))
        ts->callFunction(classCallback, {VMValue(object.objectName)});
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
    return ScriptEngine::instance().findObject(name.c_str());
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

// Renders a mounted image with its state machine's threads: the state
// sequence, the flash visibility sequence, and the always-running "ambient"
// and "spin" threads (ShapeBaseImageData::preload).
// Players' and vehicles' projected shadows (one pool for the session).
static ProjectedShadows& projectedShadows() {
    static ProjectedShadows pool;
    return pool;
}

// The shocklance zap (Tribes2.exe client zap object FUN_006518a0, as ported
// by t2-mapper shockLance.ts): the struck object's own meshes redrawn 5%
// larger about its origin, additive, no depth write, no fog, with the
// lance's textures cycling ten times a second, projected object-linear at
// 0.25 repeats/m along its longest axis and scrolling (2 age, age), fading
// 1 - age / zapDuration.
struct ShockZapRequest {
    float spawnTime = 0.0f;
    float zapDuration = 0.0f;
    std::vector<uint32_t> textures;
};
static std::unordered_map<int, ShockZapRequest>& shockZapRequests() {
    static std::unordered_map<int, ShockZapRequest> requests;
    return requests;
}

// Frame = round(fmod(age, 1/10) x 10 x 2.9999): texture[0..3] ten times a
// second (the fourth is past the three lightning frames).
static int zapFrameIndex(float age) {
    const float phase = std::fmod(age, 0.1f) * 10.0f;
    return std::clamp((int)std::lround(phase * 2.9999f), 0, 3);
}

static void drawShockZap(Renderer& r, const std::vector<ShadowCaptureDraw>& draws, size_t count,
                         const Point3F& origin, float age, const ShockZapRequest& zap) {
    if (count == 0 || zap.textures.empty() || zap.zapDuration <= 0.0f) return;
    static Shader shader;
    static bool tried = false;
    if (!tried) {
        tried = true;
        shader.load(R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uPlaneS;
uniform vec3 uPlaneT;
uniform vec3 uScroll;
out vec2 vUV;
void main() {
    vUV = vec2(dot(uPlaneS, aPos), dot(uPlaneT, aPos)) + uScroll.xy;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)", R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform float uAlpha;
void main() { FragColor = vec4(texture(uTexture, vUV).rgb, uAlpha); }
)");
    }
    if (!shader.loaded) return;
    // Texgen planes along the longest Torque axis of the bind-pose meshes:
    // X unless Y is strictly the longest (Z, the height, also selects X).
    Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < count; ++i)
        for (const auto& v : draws[i].mesh->vertices) {
            lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
            hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
        }
    const float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
    const bool alongY = ey > ez && ey > ex;
    constexpr float texgen = 0.25f;
    const Point3F planeS = alongY ? Point3F{0, texgen, 0} : Point3F{texgen, 0, 0};
    const Point3F planeT{0, 0, texgen};
    // 5% larger about the struck object's origin.
    MatrixF toOrigin, scale, back;
    toOrigin.identity(); scale.identity(); back.identity();
    toOrigin.setTranslation(origin);
    scale.m[0][0] = scale.m[1][1] = scale.m[2][2] = 1.05f;
    back.setTranslation({-origin.x, -origin.y, -origin.z});
    const MatrixF zapScale = toOrigin * scale * back;
    const MatrixF viewProj = r.projection * r.view;

    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    const GLboolean blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    shader.bind();
    const uint32_t texture = zap.textures[std::min(zap.textures.size() - 1, (size_t)zapFrameIndex(age))];
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    shader.setUniform("uTexture", (int32_t)0);
    shader.setUniform("uAlpha", 1.0f - age / zap.zapDuration);
    shader.setUniform("uPlaneS", planeS);
    shader.setUniform("uPlaneT", planeT);
    shader.setUniform("uScroll", Point3F{2.0f * age, age, 0.0f});
    for (size_t i = 0; i < count; ++i) {
        shader.setUniform("uMVP", viewProj * zapScale * draws[i].model);
        draws[i].mesh->render();
    }
    glDepthMask(depthMask);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!blend) glDisable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
}

// Torque haze at `dist`: quadratic from fogDistance to visibleDistance.
static float hazeAt(const World& world, float dist) {
    const float start = world.fog.distance, end = world.visibleDistance;
    if (!world.fog.enabled || !(end > start) || dist <= start) return 0.0f;
    if (dist >= end) return 1.0f;
    const float ramp = (dist - start) / (end - start) - 1.0f;
    return std::clamp(1.0f - ramp * ramp, 0.0f, 1.0f);
}

// Static-world ray (interiors and terrain) from a to b in Y-up space, for
// client projectile flight. Terrain hits only a downward crossing of a solid
// square, so rays from inside underground bases and through holes pass.
static bool castStaticRay(World& world, const Point3F& a, const Point3F& b,
                          ProjectilePhysics::RayHit& hit) {
    const Point3F d{b.x - a.x, b.y - a.y, b.z - a.z};
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (length < 1e-6f) return false;
    const Point3F dir{d.x / length, d.y / length, d.z / length};
    float best = length;
    bool found = false;
    float hitDist; Point3F hitPos, hitNormal;
    if (world.collision().raycast(a, dir, length, hitDist, hitPos, hitNormal) && hitDist <= best) {
        best = hitDist; hit.point = hitPos; hit.normal = hitNormal; found = true;
    }
    const auto* terrain = world.terrain();
    if (terrain && terrain->loaded) {
        auto below = [&](const Point3F& p) {
            return p.y < world.getHeight(p.x, p.z) && !terrain->isEmptySquare(p.x, p.z);
        };
        const float step = 0.5f;
        bool wasBelow = below(a);
        float prev = 0.0f;
        for (float s = std::min(step, best); ; s = std::min(s + step, best)) {
            const Point3F p{a.x + dir.x * s, a.y + dir.y * s, a.z + dir.z * s};
            const bool isBelow = below(p);
            if (isBelow && !wasBelow) {
                float lo = prev, hi = s;
                for (int k = 0; k < 12; ++k) {
                    const float mid = 0.5f * (lo + hi);
                    const Point3F q{a.x + dir.x * mid, a.y + dir.y * mid, a.z + dir.z * mid};
                    (below(q) ? hi : lo) = mid;
                }
                if (hi <= best) {
                    best = hi;
                    hit.point = {a.x + dir.x * hi, a.y + dir.y * hi, a.z + dir.z * hi};
                    hit.normal = terrainNormalFromHeights(
                        world.getHeight(hit.point.x - 1.0f, hit.point.z), world.getHeight(hit.point.x + 1.0f, hit.point.z),
                        world.getHeight(hit.point.x, hit.point.z - 1.0f), world.getHeight(hit.point.x, hit.point.z + 1.0f));
                    found = true;
                }
                break;
            }
            wasBelow = isBelow;
            prev = s;
            if (s >= best) break;
        }
    }
    if (found) hit.t = best / length;
    return found;
}

// ShapeBase::castRay: segment a -> b against each LOS-(i+9) detail (or
// Collision-(i+1) when absent) as last posed; nearest hit parameter in
// [0, 1], or -1. Player::castRay tests the PlayerData box instead (pass its
// Torque boxSize). Rigid meshes follow their node; skinned meshes are already
// in shape space.
static float raycastShape(const DTSShape& shape, const MatrixF& renderModel,
                          const Point3F& a, const Point3F& b, const float* playerBox = nullptr) {
    if (playerBox) {
        // mObjBox: (-x/2, -y/2, 0) .. (x/2, y/2, z) in Torque object space.
        const MatrixF toShape = renderModel.inverse();
        const Point3F sa = toShape.transform(a), sb = toShape.transform(b);
        const Point3F lo = Math::torquePointToYUp({-playerBox[0] * 0.5f, playerBox[1] * 0.5f, 0.0f});
        const Point3F hi = Math::torquePointToYUp({playerBox[0] * 0.5f, -playerBox[1] * 0.5f, playerBox[2]});
        const float mins[3] = {std::min(lo.x, hi.x), std::min(lo.y, hi.y), std::min(lo.z, hi.z)};
        const float maxs[3] = {std::max(lo.x, hi.x), std::max(lo.y, hi.y), std::max(lo.z, hi.z)};
        const float s0[3] = {sa.x, sa.y, sa.z}, s1[3] = {sb.x, sb.y, sb.z};
        float first = 0.0f, last = 1.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float d = s1[axis] - s0[axis];
            if (std::fabs(d) < 1e-9f) {
                if (s0[axis] < mins[axis] || s0[axis] > maxs[axis]) return -1.0f;
                continue;
            }
            float t0 = (mins[axis] - s0[axis]) / d, t1 = (maxs[axis] - s0[axis]) / d;
            if (t0 > t1) std::swap(t0, t1);
            first = std::max(first, t0);
            last = std::min(last, t1);
            if (last < first) return -1.0f;
        }
        return first;
    }
    const auto& nodeWorld = shape.animatedNodeWorld.size() == shape.nodes.size()
        ? shape.animatedNodeWorld : shape.defaultTransforms;
    auto detailMeshes = [&](const std::string& wanted) -> const std::vector<int32_t>* {
        for (const auto& detail : shape.utilityDetails)
            if (detail.name.size() == wanted.size() &&
                std::equal(detail.name.begin(), detail.name.end(), wanted.begin(),
                           [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); }))
                return &detail.meshIndices;
        return nullptr;
    };
    float best = -1.0f;
    for (int i = 0; i < 8; ++i) {
        const std::vector<int32_t>* meshes = detailMeshes("LOS-" + std::to_string(i + 9));
        if (!meshes) meshes = detailMeshes("Collision-" + std::to_string(i + 1));
        if (!meshes) continue;
        for (int32_t mi : *meshes) {
            if (mi < 0 || mi >= (int)shape.meshes.size()) continue;
            const MeshData& mesh = shape.meshes[mi];
            const bool skinned = mi < (int)shape.skins.size() && shape.skins[mi].hasSkin;
            MatrixF transform = renderModel;
            if (!skinned && mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeWorld.size())
                transform = renderModel * nodeWorld[mesh.nodeIndex];
            std::vector<Point3F> world(mesh.vertices.size());
            for (size_t v = 0; v < mesh.vertices.size(); ++v) world[v] = transform.transform(mesh.vertices[v].pos);
            for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
                const uint32_t i0 = mesh.indices[k], i1 = mesh.indices[k + 1], i2 = mesh.indices[k + 2];
                if (i0 >= world.size() || i1 >= world.size() || i2 >= world.size()) continue;
                const float t = segmentTriangle(a, b, world[i0], world[i1], world[i2]);
                if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
            }
        }
    }
    return best;
}

// The shape's Collision-1..8 details (ShapeBase::buildConvex uses
// mShapeInstance's collisionDetails, never LOS-N), in Torque world space with
// outward face normals. The DTSShape is shared between instances, so its
// animated pose belongs to whichever instance rendered last; hulls use the
// default node transforms.
static void appendShapeCollisionTriangles(const DTSShape& shape, const MatrixF& renderModel,
                                          std::vector<PlayerPrediction::Triangle>& out) {
    const auto& nodeWorld = shape.defaultTransforms;
    auto toTorque = [](const Point3F& p) { return Point3F{p.x, -p.z, p.y}; };
    for (int i = 0; i < 8; ++i) {
        const std::string wanted = "Collision-" + std::to_string(i + 1);
        const DTSShape::UtilityDetail* detail = nullptr;
        for (const auto& d : shape.utilityDetails)
            if (strcasecmp(d.name.c_str(), wanted.c_str()) == 0) { detail = &d; break; }
        if (!detail) continue;
        for (int32_t mi : detail->meshIndices) {
            if (mi < 0 || mi >= (int)shape.meshes.size()) continue;
            const MeshData& mesh = shape.meshes[mi];
            const bool skinned = mi < (int)shape.skins.size() && shape.skins[mi].hasSkin;
            MatrixF transform = renderModel;
            if (!skinned && mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeWorld.size())
                transform = renderModel * nodeWorld[mesh.nodeIndex];
            std::vector<Point3F> world(mesh.vertices.size());
            for (size_t v = 0; v < mesh.vertices.size(); ++v)
                world[v] = toTorque(transform.transform(mesh.vertices[v].pos));
            if (world.empty()) continue;
            // Each collision mesh is a convex hull: its faces point away from
            // the hull's centre, whatever the authored winding.
            Point3F centre{0, 0, 0};
            for (const Point3F& p : world) centre = PlayerPrediction::add(centre, p);
            centre = PlayerPrediction::mul(centre, 1.0f / (float)world.size());
            for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
                const uint32_t i0 = mesh.indices[k], i1 = mesh.indices[k + 1], i2 = mesh.indices[k + 2];
                if (i0 >= world.size() || i1 >= world.size() || i2 >= world.size()) continue;
                const Point3F a = world[i0], b = world[i1], c = world[i2];
                Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(b, a), PlayerPrediction::sub(c, a));
                if (PlayerPrediction::dot(n, PlayerPrediction::sub(a, centre)) < 0) n = PlayerPrediction::mul(n, -1.0f);
                const float len = std::sqrt(PlayerPrediction::dot(n, n));
                if (len < 1e-12f) continue;
                out.push_back({a, b, c, PlayerPrediction::mul(n, 1.0f / len)});
            }
        }
    }
}

static void renderMountedImage(DTSShape& shape, const WeaponImage::Animation& animation,
                               float now) {
    std::vector<DTSShape::BlendThread> threads;
    auto duration = [&](int index) {
        return index >= 0 && index < (int)shape.animations.size()
            ? shape.animations[index].duration : 0.0f;
    };
    auto addThread = [&](const WeaponImage::Thread& thread, bool flash) {
        if (!thread.valid() || thread.sequence >= (int)shape.animations.size()) return;
        const auto& clip = shape.animations[thread.sequence];
        const float scale = thread.scaleSequence >= 0 ? duration(thread.scaleSequence) : -1.0f;
        const float position = WeaponImage::threadPosition(
            thread, now, clip.duration, !flash && clip.looping, scale);
        threads.push_back({thread.sequence, position * clip.duration});
    };
    if (animation.valid()) {
        addThread(animation.anim(), false);
        addThread(animation.flash(), true);
    }
    auto named = [&](const char* name) -> int {
        const DTSShape::Animation* found = findAnimation(shape, name);
        return found ? (int)(found - shape.animations.data()) : -1;
    };
    const int ambient = named("ambient");
    if (ambient >= 0 && duration(ambient) > 0.0f) {
        const float since = animation.valid() ? std::max(0.0f, now - animation.mountedAt()) : now;
        threads.push_back({ambient, std::fmod(since, duration(ambient))});
    }
    const int spin = named("spin");
    if (spin >= 0 && duration(spin) > 0.0f && animation.valid()) {
        const float time = std::fmod(animation.spinTime(now), duration(spin));
        threads.push_back({spin, time < 0.0f ? time + duration(spin) : time});
    }
    if (threads.empty()) {
        shape.animatedNodeWorld.clear(); // bind pose; the shape is shared
        shape.render(DTSShape::SelectDetail);
        return;
    }
    const DTSShape::BlendThread primary = threads.front();
    shape.renderAnimationIndex(primary.animationIndex, primary.time, nullptr, 0,
                               threads.size() > 1 ? threads.data() + 1 : nullptr,
                               (int)threads.size() - 1, DTSShape::SelectDetail);
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
    static SoundSource* beepSrc = nullptr;
    static AudioSystem* beepAudio = nullptr;
    static bool tried = false;
    auto& audio = Engine::instance().audio();
    if (beepAudio != &audio ||
        (beepBuf && !audio.isBufferAlive(beepBuf)) ||
        (beepSrc && !audio.isSourceAlive(beepSrc))) {
        beepBuf = nullptr;
        beepSrc = nullptr;
        tried = false;
        beepAudio = &audio;
    }
    if (!audio.isInitialized()) return;
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
    // Reuse a single source for all chat beeps instead of leaking one per call.
    if (!beepSrc) beepSrc = audio.createSource(true);
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
// Screen-space drawing inside the 3D pass: labels projected with
// worldToScreen are pixel coordinates, and Font/drawBox use whatever
// projection is active, so switch to the HUD's pixel ortho for the scope.
struct ScreenSpace2D {
    Renderer& r;
    MatrixF savedProjection, savedView;
    explicit ScreenSpace2D(Renderer& renderer)
        : r(renderer), savedProjection(renderer.projectionMatrix()), savedView(renderer.viewMatrix()) {
        MatrixF ortho;
        ortho.identity();
        ortho.m[0][0] = 2.0f / (float)std::max(1, r.config().width);
        ortho.m[1][1] = -2.0f / (float)std::max(1, r.config().height);
        ortho.m[0][3] = -1.0f;
        ortho.m[1][3] = 1.0f;
        r.setProjection(ortho);
        MatrixF id;
        id.identity();
        r.setView(id);
    }
    ~ScreenSpace2D() {
        r.flushSpriteBatch();
        r.setProjection(savedProjection);
        r.setView(savedView);
    }
};

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

static bool botFloorContact(const World& world, const Point3F& position,
                            float& floorY, Point3F& normal) {
    floorY = world.getFloorHeight(position.x, position.y, position.z);
    normal = {0.0f, 1.0f, 0.0f};
    const auto& collision = world.collision();
    float rayT = 0.0f;
    Point3F point{};
    if (collision.loaded && collision.raycast(
            {position.x, position.y + 0.2f, position.z}, {0, -1, 0},
            2.0f, rayT, point, normal) && normal.y >= 0.55f) {
        floorY = point.y;
        return true;
    }

    constexpr float sampleSpacing = 0.25f;
    const float left = world.getHeight(position.x - sampleSpacing, position.z);
    const float right = world.getHeight(position.x + sampleSpacing, position.z);
    const float back = world.getHeight(position.x, position.z - sampleSpacing);
    const float front = world.getHeight(position.x, position.z + sampleSpacing);
    if (left <= -1.0e9f || right <= -1.0e9f ||
        back <= -1.0e9f || front <= -1.0e9f || floorY <= -1.0e9f)
        return false;
    normal = terrainNormalFromHeights(left, right, back, front, sampleSpacing);
    return normal.y >= 0.55f;
}

// Forward declarations for demo ghost shape helpers
static bool isRenderableGhostClass(const std::string& className);
static bool isEffectOnlyGhostClass(const std::string& className);

Player::Player() {
    for (int i = 0; i < gWeaponCount; i++) {
        Weapon w;
        w.type = i;
        w.ammo = weaponInitialAmmo(gWeaponTable[i]);
        w.fireTimer = 0;
        w.reloadTimer = 0;
        w.firing = false;
        w.reloading = false;
        weapons.push_back(w);
    }
}
Player::~Player() {}

void Player::update(float dt) {
    // Do not let a stalled frame turn passive PlayerData repair into a
    // multi-second heal. Normal one-second script timing remains intact, but
    // extreme render hitches are bounded like the rest of simulation state.
    if (std::isfinite(dt) && dt > 1.0f) dt = 1.0f;
    if (hp > 0) {
        hp = applyRepairRate(hp, repairRate, dt, maxHp);
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
    // Without a warrior the client connects with empty fields and
    // GameConnection::onConnect falls back to a Human Male (below).

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
    ScriptEngine::instance().addObject(resolver);
    std::string datablockName;
    if (race == "Bioderm")
        datablockName = armorSize + "MaleBiodermArmor";
    else
        datablockName = armorSize + sex + race + "Armor";
    if (!ScriptEngine::instance().findObject(datablockName.c_str())) {
        datablockName = ts->callFunction(
            "getArmorDatablock", {VMValue(resolverName), VMValue(armorSize)}).toString();
    }
    ScriptEngine::instance().removeObject(resolver);
    delete resolver;

    ScriptObject* datablock = ScriptEngine::instance().findObject(datablockName.c_str());
    if (!datablock) {
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
    if (const auto* maxDamage = scriptField(datablock, "maxDamage"))
        setMaxHealth(maxDamage->toFloat());
    if (const auto* maxEnergy = scriptField(datablock, "maxEnergy"))
        setMaxEnergy(maxEnergy->toFloat());
    // PlayerData owns the passive ShapeBase repair rate. Without loading this
    // authored field, repair stations and custom armor profiles can never
    // restore health even though the simulation tick applies the rate.
    if (const auto* repair = scriptField(datablock, "repairRate"))
        setRepairRate(repair->toFloat());
    auto shapeField = datablock->fields.find("shapeFile");
    if (shapeField == datablock->fields.end()) {
        for (auto it = datablock->fields.begin();
             it != datablock->fields.end(); ++it) {
            std::string fieldName = it->first;
            for (char& c : fieldName)
                c = (char)tolower((unsigned char)c);
            if (fieldName == "shapefile") { shapeField = it; break; }
        }
    }
    if (shapeField == datablock->fields.end() || shapeField->second.toString().empty()) {
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
    // A loadout can temporarily have no usable slot (for example while a
    // respawn inventory is being rebuilt). Do not retain the previous model
    // or let render() index the old selection.
    weaponLoaded = false;
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    if (weapons[curWeapon].type < 0 || weapons[curWeapon].type >= gWeaponCount) return;
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
    const float animationDt = animationDelta(dt);
    animTime += animationDt;
    weaponAnimTime += animationDt;

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
        if (modelShape.nativeDTS) model = model * Math::nativeDtsFrame();
        model.setTranslation(pos);

        if (!firstPerson) {
            r.setModel(model * modelShape.upOrientation());
            const char* animNames[] = { "stand", "run", "jump", "jet", "death" };
            const char* altNames[]  = { "idle",  "run", "jump", "jet", "die"   };
            int idx = (int)anim;
            if (idx >= 0 && idx <= 4) {
                // Torque animation names are case-insensitive.  Custom player
                // shapes commonly capitalize sequence names differently from
                // the stock model, so exact matching incorrectly selected the
                // fallback sequence (or rendered the bind pose).
                const auto* animation = findAnimation(modelShape, animNames[idx]);
                modelShape.renderAnimation(animation ? animation->name.c_str() : altNames[idx],
                                           animTime, nullptr, 0, DTSShape::SelectDetail);
            } else {
                modelShape.render(DTSShape::SelectDetail);
            }
        }

        if (weaponLoaded && curWeapon >= 0 &&
            curWeapon < (int32_t)weapons.size()) {
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
                weaponModel = weaponModel * Math::nativeDtsFrame();
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
                weaponShape.renderAnimation(animation->name.c_str(), weaponAnimTime, nullptr, 0,
                                            DTSShape::SelectDetail);
            else
                weaponShape.render(DTSShape::SelectDetail);
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
        if (idx != curWeapon && curWeapon >= 0 && curWeapon < (int32_t)weapons.size()) {
            // Weapon changes interrupt reloads rather than completing them in
            // the background while another weapon is equipped.
            cancelWeaponReload(weapons[curWeapon].reloading,
                               weapons[curWeapon].reloadTimer);
            // Firing is also image-local. Keep the old weapon from showing a
            // stale muzzle/fire sequence when it is selected again before its
            // cooldown has elapsed.
            cancelWeaponFirePresentation(weapons[curWeapon].firing);
        }
        curWeapon = idx;
        weapons[curWeapon].firing = false;
        if (weaponNeedsReloadOnSelect(weapons[curWeapon])) {
            const WeaponData& data = gWeaponTable[weapons[curWeapon].type];
            beginWeaponReload(weapons[curWeapon].reloading,
                              weapons[curWeapon].reloadTimer,
                              weapons[curWeapon].firing,
                              weapons[curWeapon].fireTimer,
                              data.reloadTime);
        }
        loadWeaponModel();
        if (auto* hud = Engine::instance().guiRenderer().findControl("weaponsHud")) {
            if (weapons[curWeapon].type < 0 || weapons[curWeapon].type >= gWeaponCount) return;
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
    if (weapons[curWeapon].type < 0 || weapons[curWeapon].type >= gWeaponCount) return;
    std::string nativeName = gWeaponTable[weapons[curWeapon].type].name;
    if (nativeName == "Spinfusor") nativeName = "Disc";
    else if (nativeName == "PlasmaGun") nativeName = "Plasma";
    else if (nativeName == "ELF") nativeName = "ELFGun";
    // The stock HUD has exactly one active weapon slot. Remote HUD updates
    // can leave an older slot active, so clear it before refreshing ammo.
    hud->activeHudSlot = -1;
    for (size_t slot = 0; slot < hud->hudSlots.size(); ++slot) {
        hud->hudSlots[slot].active = hud->hudSlots[slot].name == nativeName;
        if (hud->hudSlots[slot].active) {
            hud->hudSlots[slot].amount = weapons[curWeapon].ammo;
            hud->activeHudSlot = (int)slot;
        }
    }
}

void Player::fireWeapon(bool alt) {
    if (isDead()) return;
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    Weapon& w = weapons[curWeapon];
    if (w.type < 0 || w.type >= gWeaponCount) return;
    const WeaponData& wd = gWeaponTable[w.type];
    if (!weaponFireModeAllowed(wd, alt)) return;
    if (!w.canFire(eng)) return;

    // A zero-length camera ray can occur while the view is being rebuilt.
    // Do not consume energy/ammo or start the fire cooldown without a shot.
    Point3F cpos = cameraPos();
    Point3F target = cameraTarget();
    Point3F dir = {target.x - cpos.x, target.y - cpos.y, target.z - cpos.z};
    if (!weaponAimUsable(dir)) return;

    eng -= wd.energyCost;
    w.fireTimer = wd.fireRate;
    w.firing = true;
    weaponAnimTime = 0.0f;

    const float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir.x /= dlen; dir.y /= dlen; dir.z /= dlen;

    Projectile p;
    p.pos = computeProjectileSpawn(cpos, dir);
    p.previousPos = p.pos;
    // Physical projectiles inherit the shooter's momentum in Tribes 2.  If
    // this is omitted, firing while skiing or jetting makes rounds visibly
    // lag behind the player and changes their range relative to the server.
    p.vel = projectileLaunchVelocity(dir, wd.speed, vel);
    p.type = wd.projectileType;
    p.damage = wd.damage;
    p.splashRadius = wd.splashRadius;
    p.lifetime = 5.0f;
    p.active = true;
    p.ownerId = 0;
    // Projectile effects are datablock-owned.  The selected slot is only a
    // loadout position and can differ from the weapon type when a loadout is
    // reordered; using it here plays the wrong impact sound (or none at all).
    p.weaponType = weaponEffectType(w);

    if (wd.projectileType == ProjectileType::Hitscan) {
        p.lifetime = 2.0f;
        // A rifle trace must cover its full range this tick. Advancing it at
        // 500 units/second made fast targets and nearby walls appear one or
        // more frames late, unlike the native hitscan weapons.
        p.pos = hitscanEndpoint(p.previousPos, dir);
        p.vel = {0.0f, 0.0f, 0.0f};
    }

    Engine::instance().game().world().spawnProjectile(p);

    // Play fire sound
    loadWeaponSounds(w);
    if (w.fireSound) {
        auto& audio = Engine::instance().audio();
        auto* src = audio.createSource();
        if (src) {
            // Keep fire audio at the muzzle. Hitscan projectiles move their
            // current position to the trace endpoint during spawn setup.
            src->setPosition(p.previousPos);
            src->positional = true;
            src->setVolume(0.5f);
            src->play(w.fireSound);
            // Sources auto-clean via audio system update
        }
    }

    if (wd.maxAmmo > 0) {
        w.ammo--;
        if (w.ammo <= 0) w.ammo = 0;
        // Finite weapon datablocks use their reload time when the magazine is
        // exhausted.  Without starting this timer, weapons with a reloadTime
        // could fire their last round once and then remain permanently empty.
        if (weaponNeedsReload(wd, w.ammo)) {
            beginWeaponReload(w.reloading, w.reloadTimer, w.firing, w.fireTimer,
                              wd.reloadTime);
        }
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
    if (hp <= 0.0f) return;
    // Native damage paths only accept finite values.  Letting a malformed
    // script or network update store NaN here makes every later health
    // comparison false and leaves the player visibly alive but unkillable.
    if (!isValidDamageAmount(amount)) return;
    if (amount < 0) {
        // Healing
        hp -= amount; // amount is negative, so this adds
        if (hp > maxHp) hp = maxHp;
        return;
    }
    if (Engine::instance().hasGame())
        Engine::instance().game().recordDamageFlash(amount);
    if (arm > 0) {
        applyArmorDamage(hp, arm, amount);
        if (hp <= 0.0f) {
            hp = 0.0f;
            deaths++;
        }
    } else {
        hp -= amount;
        if (hp <= 0.0f) {
            hp = 0.0f;
            deaths++;
        }
    }
}

float Game::getBlackOut() const { return liveConnection ? liveConnection->getBlackOut() : 0.0f; }

void Game::recordDamageFlash(float amount) {
    damageFlash = std::max(damageFlash, damageFlashForAmount(amount, pl->maxHealth()));
}

void Player::respawn() {
    hp = maxHp;
    eng = maxEng;
    // repairRate belongs to the authored PlayerData, not to one life.
    heatLevel = 0.0f;
    arm = 0.0f;
    vel = {0,0,0};
    // A new ShapeBase life starts in its stand sequence. Leaving the death
    // state here renders the previous death pose until the next animation tick.
    anim = Stand;
    animTime = 0.0f;
    // SpawnSphere carries the initial facing direction as well as the position.
    // Resetting yaw to zero makes rotated team starts face the wrong objective.
    Point3F spawnRotation{};
    if (Engine::instance().hasGame()) {
        if (!Engine::instance().game().world().spawnTransformForTeam(
                teamId, pos, spawnRotation)) {
            pos = Engine::instance().game().world().spawnPointForTeam(teamId);
            spawnRotation = {};
        }
    }
    rot = spawnRotation;
    pos.y += 1.0f;
    onGround = false;
    // Respawn starts a fresh movement state; the previous jump cooldown must
    // not suppress the first jump of the new life.
    jumpHeld = false;
    jumpCooldown = 0.0f;
    // Loadouts are allowed to contain unavailable or invalid slots.  Native
    // respawn equips the first usable weapon rather than leaving the player on
    // an empty slot that cannot fire or be represented by the HUD.
    curWeapon = -1;
    for (auto& weapon : weapons) {
        weapon.ammo = weapon.type >= 0 && weapon.type < gWeaponCount
            ? weaponInitialAmmo(gWeaponTable[weapon.type]) : 0;
        weapon.fireTimer = 0.0f;
        weapon.reloadTimer = 0.0f;
        weapon.firing = false;
        weapon.reloading = false;
    }
    for (size_t index = 0; index < weapons.size(); ++index) {
        if (weaponIsSelectable(weapons[index])) {
            curWeapon = static_cast<int32_t>(index);
            break;
        }
    }
    if (Engine::instance().hasGame()) {
        loadWeaponModel();
        updateWeaponHud();
    }
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

Point3F World::spawnPointForTeam(int teamId) const {
    Point3F position{}, rotation{};
    if (spawnTransformForTeam(teamId, position, rotation)) return position;
    return playerSpawn;
}

bool World::spawnTransformForTeam(int teamId, Point3F& position,
                                  Point3F& rotation) const {
    // SpawnSphere positions are retained in their authored Torque frame on
    // WorldObjects, so convert only after selecting the team-compatible one.
    // Stock missions commonly author several spheres per team. Reusing the
    // first one makes every respawn visibly stack at the same location.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<const WorldObject*> candidates;
        for (const auto& object : worldObjects) {
            if (!missionClassIs(object.className, "SpawnSphere")) continue;
            const bool wanted = pass == 0 ? object.teamId == teamId : object.teamId == 0;
            if (wanted) candidates.push_back(&object);
        }
        if (!candidates.empty()) {
            size_t& cursor = spawnCursors[teamId];
            const WorldObject* selected = candidates[cursor % candidates.size()];
            ++cursor;
            position = Math::torquePointToYUp(selected->pos);
            // Player yaw uses the same +Z-forward convention as cameraTarget.
            rotation = {0.0f, 0.0f,
                        spawnYawFromTorqueRotation(selected->rot,
                                                   selected->rotAngleDeg)};
            return true;
        }
    }
    return false;
}

static int findWaterBody(const std::vector<World::WaterState>& bodies,
                         const std::string& target) {
    if (target.empty()) {
        for (size_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].active) return (int)i;
        return -1;
    }
    char* end = nullptr;
    const long index = std::strtol(target.c_str(), &end, 10);
    if (end != target.c_str() && *end == '\0' && index >= 0) {
        int ordinal = 0;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (waterBodyOrdinalMatches(bodies[i].active, ordinal, (int)index))
                return (int)i;
            if (bodies[i].active) ++ordinal;
        }
    }
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].active && waterBodyNameMatches(bodies[i].name, target))
            return (int)i;
    return -1;
}

static bool isLegacyWaterBody(const std::vector<World::WaterState>& bodies,
                              int index) {
    if (index < 0 || index >= (int)bodies.size() || !bodies[index].active)
        return false;
    for (int i = 0; i < index; ++i)
        if (bodies[i].active) return false;
    return true;
}

bool World::setWaterLevel(const std::string& target, float level) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterLevel(waterBodies[index].level, level)) return false;
    if (isLegacyWaterBody(waterBodies, index)) water = waterBodies[index];
    return true;
}

bool World::setWaterType(const std::string& target, int type) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || type < 0 || type > 7) return false;
    waterBodies[index].liquidType = type;
    if (isLegacyWaterBody(waterBodies, index)) water.liquidType = type;
    return true;
}

bool World::setWaterOpacity(const std::string& target, float opacity) {
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterOpacity(waterBodies[index].opacity, opacity)) return false;
    waterBodies[index].surfaceColor.a = opacity;
    if (isLegacyWaterBody(waterBodies, index)) water = waterBodies[index];
    return true;
}

bool World::setWaterColor(const std::string& target, const ColorF& color) {
    // Water color also carries the authored surface opacity. Reject the whole
    // value before changing RGB so malformed alpha cannot make the renderer
    // silently discard the surface.
    if (!validEnvironmentColor(color)) return false;
    const int index = findWaterBody(waterBodies, target);
    if (index < 0 || !applyWaterColor(waterBodies[index].surfaceColor, color.r, color.g, color.b)) return false;
    waterBodies[index].surfaceColor.a = color.a;
    waterBodies[index].opacity = color.a;
    if (isLegacyWaterBody(waterBodies, index)) water = waterBodies[index];
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

void World::shadowReceiversInBox(const Point3F& lo, const Point3F& hi, const Point3F& lightDir,
                                 std::vector<Point3F>& out) const {
    auto facing = [&](const Point3F& n) {
        return n.x * lightDir.x + n.y * lightDir.y + n.z * lightDir.z < ShadowProjection::ReceiverFacing;
    };
    auto inBox = [&](const Point3F& a, const Point3F& b, const Point3F& c) {
        return std::max({a.x, b.x, c.x}) >= lo.x && std::min({a.x, b.x, c.x}) <= hi.x &&
               std::max({a.y, b.y, c.y}) >= lo.y && std::min({a.y, b.y, c.y}) <= hi.y &&
               std::max({a.z, b.z, c.z}) >= lo.z && std::min({a.z, b.z, c.z}) <= hi.z;
    };
    if (terrainBlock.loaded) {
        std::vector<Point3F> tris;
        terrainBlock.appendTrianglesInRect(lo.x, lo.z, hi.x, hi.z, tris);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const Point3F &a = tris[i], &b = tris[i + 1], &c = tris[i + 2];
            if (!inBox(a, b, c)) continue;
            // Terrain winding faces up; its normal is (c - a) x (b - a).
            const Point3F e1{c.x - a.x, c.y - a.y, c.z - a.z}, e2{b.x - a.x, b.y - a.y, b.z - a.z};
            Point3F n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            if (n.y < 0.0f) n = {-n.x, -n.y, -n.z};
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len < 1e-8f || !facing({n.x / len, n.y / len, n.z / len})) continue;
            out.insert(out.end(), {a, b, c});
        }
    }
    const CollisionGrid& grid = lightProbeGrid;
    if (grid.resX > 0 && grid.resZ > 0 && !lightProbeTris.empty()) {
        const int cx0 = std::max(0, (int)std::floor((lo.x - grid.minX) / grid.cellW));
        const int cx1 = std::min(grid.resX - 1, (int)std::floor((hi.x - grid.minX) / grid.cellW));
        const int cz0 = std::max(0, (int)std::floor((lo.z - grid.minZ) / grid.cellH));
        const int cz1 = std::min(grid.resZ - 1, (int)std::floor((hi.z - grid.minZ) / grid.cellH));
        std::vector<int> seen;
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx)
                for (int ti : grid.cells[(size_t)cz * grid.resX + cx]) seen.push_back(ti);
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (int ti : seen) {
            const CollisionTri& tri = lightProbeTris[ti];
            if (!facing(tri.normal) || !inBox(tri.v0, tri.v1, tri.v2)) continue;
            out.insert(out.end(), {tri.v0, tri.v1, tri.v2});
        }
    }
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
    spawnCursors.clear();
    if (auto* ts = Engine::instance().script().ts()) {
        if (loaded || !ScriptEngine::instance().missionObjects().empty())
            ts->dispatchMissionCallback("onMissionEnd", {});
        const auto missionObjects = Engine::instance().script().missionObjects();
        std::vector<std::string> order;
        for (const auto& object : missionObjects)
            if (object.parentName.empty()) {
                auto part = Engine::instance().script().missionDeletionOrder(object.name);
                order.insert(order.end(), part.begin(), part.end());
            }
        for (const auto& name : order) {
            auto it = std::find_if(missionObjects.begin(), missionObjects.end(),
                [&name](const ScriptMissionObject& object) { return object.name == name; });
            if (it != missionObjects.end()) {
                WorldObject callbackObject;
                callbackObject.className = it->className;
                callbackObject.objectName = it->name;
                dispatchMissionLifecycle(callbackObject, "onRemove");
            }
        }
        // Cancel only mission-owned work. Global timers must survive mission
        // teardown, while callbacks scheduled by onRemove must not escape it.
        ScriptEngine::instance().cancelMissionEvents();
    }
    ScriptEngine::instance().clearMissionObjects();
    clearEffects();
    terrainBlock.reset();
    skyBox.reset();
    Engine::instance().renderer().clearMissingTextureCache();
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
    // Pickups are mission-owned too. Leaving them behind makes a subsequent
    // mission retain stale items even though its scene graph was replaced.
    items.clear();
    missionObjectives.clear();
    navGraph = {};
    interiorCollision = {};
    lightProbeTris.clear();
    lightProbeInfo.clear();
    lightProbeGrid = {};
    lightmapPixelCache.clear();
    currentSceneState = {};
    cameras.clear();
    shapes.clear();
    debrisShapes.clear();
    debrisShapeIndex.clear();
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
    if (SDL_GL_GetCurrentContext())
        for (auto& body : waterBodies) {
            if (body.fluidVbo) glDeleteBuffers(1, &body.fluidVbo);
            if (body.fluidVao) glDeleteVertexArrays(1, &body.fluidVao);
        }
    waterBodies.clear();
    loaded = false;
    auto& config = Engine::instance().renderer().config();
    config.fogDensity = -1.0f;
    config.fogColorOverride = false;
}

bool Game::deleteMissionObjectIfPresent(const std::string& name) {
    if (!w) return false;
    for (const auto& object : w->objects())
        if (object.objectName == name) return w->deleteMissionObject(name);
    return false;
}

bool World::load(const char* mapName) {
    Console::instance().printf(LogLevel::Info, "Loading map: %s", mapName);

    // Do not destroy the live mission until the replacement can be read.
    if (mapName && !TorchPath::isSafeLogicalPath(mapName)) return false;
    const std::string requestedMission = missionLoadPath(mapName ? mapName : "");
    if (requestedMission.empty() || !TorchPath::isSafeLogicalPath(requestedMission.c_str())) {
        Console::instance().printf(LogLevel::Error, "Rejected unsafe mission reference: %s",
                                   mapName ? mapName : "");
        return false;
    }
    auto& fs = Engine::instance().fs();
    std::string misPath;
    std::string misData;
    if (!resolveMissionFile(fs, requestedMission, misPath, misData)) {
        Console::instance().printf(LogLevel::Warn,
            "Mission unavailable; preserving current world: %s", requestedMission.c_str());
        return false;
    }
    auto parsedObjects = parseMisFile(misData);
    if (parsedObjects.empty()) {
        Console::instance().printf(LogLevel::Warn,
            "Mission contains no objects: %s", misPath.c_str());
        return false;
    }
    return loadObjects(mapName, misPath, std::move(parsedObjects));
}

// Builds the world from mission objects: a parsed .mis, or the scene a demo's
// SceneObject ghosts describe.
bool World::loadObjects(const char* mapName, const std::string& misPath,
                        std::vector<MisObject> parsedObjects) {
    if (parsedObjects.empty()) return false;
    auto& fs = Engine::instance().fs();

    cleanupMission();

    // Mission-owned effects must not survive a V12 scene replacement.  Do not
    // clear the whole scheduler here: global script timers outlive missions.
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
    debrisShapeIndex.clear();
    worldObjects.clear();
    missionObjectives.clear();
    navGraph = {};
    interiorCollision = {};
    lightProbeTris.clear();
    lightProbeInfo.clear();
    lightProbeGrid = {};
    lightmapPixelCache.clear();
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
    if (SDL_GL_GetCurrentContext())
        for (auto& body : waterBodies) {
            if (body.fluidVbo) glDeleteBuffers(1, &body.fluidVbo);
            if (body.fluidVao) glDeleteVertexArrays(1, &body.fluidVao);
        }
    waterBodies.clear();
    playerSpawn = {0, 5, 0};
    loaded = false;

    // Cloud layer properties (populated from Sky object if .mis available)
    // Sky constructor defaults (cloudHeightPer, cloudSpeed1..3).
    float cloudHeights[3] = {0.35f, 0.25f, 0.2f};
    float cloudSpeeds[3] = {0.0001f, 0.0002f, 0.0003f};
    bool missionHasTerrainBlock = false;

    if (!parsedObjects.empty()) {
        Console::instance().printf(LogLevel::Info, "Found mission: %s (%zu objects)", misPath.c_str(),
            parsedObjects.size());

        auto objects = std::move(parsedObjects);

        std::vector<ScriptMissionObject> missionObjects;
        missionObjects.reserve(objects.size());
        for (const auto& object : objects) {
            if (object.objName.empty()) continue;
            ScriptMissionObject record;
            record.className = object.className;
            record.name = object.objName;
            record.parentName = object.parentName;
            for (const auto& property : object.props)
                record.fields[property.name] = VMValue(property.value);
            missionObjects.push_back(std::move(record));
        }
        ScriptEngine::instance().setMissionObjects(std::move(missionObjects), true);

        if (const MisObject* graph = findObject(objects, "NavigationGraph"))
            navGraph = authoredNavigationGraph(*graph);
        for (const auto& object : objects)
            if (missionClassEquals(object.className, "AIObjective"))
                missionObjectives.push_back(authoredMissionObjective(object));
        Console::instance().printf(LogLevel::Info, "  navigation graph: %s, objectives: %zu",
            navGraph.graphFile.empty() ? "none" : navGraph.graphFile.c_str(), missionObjectives.size());

        for (const auto& obj : objects) {
            if (!missionClassEquals(obj.className, "Camera")) continue;
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
                    terrainBlock.load(terData.data(), terData.size(), false);
                    break;
                }
            }

            // TerrainBlock.detailTexture (e.g. "details/lushdet1").
            terrainBlock.overlayDetailTexture = 0;
            {
                const std::string detailName = getProp(terrainObj->props, "detailtexture");
                std::vector<uint32_t> frames;
                std::vector<float> durations;
                if (!detailName.empty() &&
                    Engine::instance().renderer().loadTextureFrames(detailName.c_str(), frames, durations) &&
                    !frames.empty()) {
                    GLint w = 0, h = 0;
                    glBindTexture(GL_TEXTURE_2D, frames.front());
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    if (w > 0 && h > 0) {
                        const float blockSize = terrainBlock.squareSize * 256.0f;
                        terrainBlock.overlayDetailTexture = frames.front();
                        terrainBlock.overlayDetailTiling[0] = blockSize * 62.0f / (float)w;
                        terrainBlock.overlayDetailTiling[1] = blockSize * 62.0f / (float)h;
                        Console::instance().printf(LogLevel::Info, "  terrain detail texture: %s (%dx%d)",
                                                   detailName.c_str(), w, h);
                    }
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
                if (sscanf(windStr.c_str(), "%f %f %f", &wx, &wy, &wz) == 3) {
                    setTorchWindVelocity(Math::torquePointToYUp({wx, wy, wz}));
                    skyBox.windX = wx;
                    skyBox.windY = wy;
                }
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
                 float distance = 0.0f, minHeight = 0.0f, maxHeight = 0.0f, percentage = 1.0f;
                 if (sscanf(volume.c_str(), "%f %f %f %f", &distance, &minHeight,
                            &maxHeight, &percentage) >= 3 && distance > 0.0f &&
                     percentage >= 0.0f && percentage <= 1.0f &&
                     maxHeight > minHeight) {
                     fogVolumes.push_back({distance, minHeight, maxHeight, percentage});
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
                if (!sStr.empty()) cloudSpeeds[ci] = (float)std::atof(sStr.c_str());
            }
        }

        // Parse Sun from mission for dynamic lighting
        MisObject* sunObj = findObject(objects, "Sun");
        if (sunObj) {
            std::string azStr = getProp(sunObj->props, "azimuth");
            std::string elStr = getProp(sunObj->props, "elevation");
            std::string colStr = getProp(sunObj->props, "color");
            std::string dirStr = getProp(sunObj->props, "direction");
            float tdx, tdy, tdz;
            if (!dirStr.empty() && sscanf(dirStr.c_str(), "%f %f %f", &tdx, &tdy, &tdz) == 3) {
                // Sun::direction is the Torque-space direction light travels;
                // sunLightDir points from the scene toward the sun.
                const Point3F travel = Math::torquePointToYUp({tdx, tdy, tdz});
                if (setSunDirection({-travel.x, -travel.y, -travel.z}))
                    Console::instance().printf(LogLevel::Debug, "  sun: direction=(%.3f %.3f %.3f)", tdx, tdy, tdz);
            } else if (!azStr.empty() && !elStr.empty()) {
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
                float ar = 0.0f, ag = 0.0f, ab = 0.0f;
                if (sscanf(ambStr.c_str(), "%f %f %f", &ar, &ag, &ab) == 3 &&
                    std::isfinite(ar) && std::isfinite(ag) && std::isfinite(ab)) {
                    sunAmbient = {ar, ag, ab, 1.0f};
                }
            }

            // Terrain baked lightmap uses the mission sun direction
            // (baked once the interiors are in place, below)
            if (sunLightDirUsed && terrainBlock.loaded)
                terrainBlock.lightDir = sunLightDir;
        }

        // Parse MissionArea from mission (for boundary visualization)
        for (auto& obj : objects) {
            // Mission object class names are case-insensitive in Torque.
            // Keep the boundary overlay working for missions authored with a
            // different class spelling, just like the other mission passes.
            if (!missionClassEquals(obj.className, "MissionArea")) continue;
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

        // Parse Precipitation from mission
        for (auto& obj : objects) {
            if (missionClassEquals(obj.className, "Precipitation")) {
                const std::string dataBlockName = missionLower(getProp(obj.props, "dataBlock"));
                const ScriptObject* dataBlock = nullptr;
                for (const auto& [objectName, object] : ScriptEngine::instance().objects)
                    if (object && !dataBlockName.empty() && missionLower(objectName) == dataBlockName &&
                        missionLower(object->className) == "precipitationdata") { dataBlock = object; break; }
                setScenePrecipitation([&](const char* field) { return getProp(obj.props, field); },
                    [&](const char* wanted) -> std::string {
                        if (dataBlock)
                            for (const auto& [key, value] : dataBlock->fields)
                                if (missionLower(key) == wanted) return value.toString();
                        return {};
                    });
                break;
            }
        }

        // Parse every WaterBlock. WaterBlock positions are the lower-left
        // corner of the fluid region, not the center of the rendered plane.
        for (auto& obj : objects) {
            if (missionClassEquals(obj.className, "WaterBlock")) {
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
                        body.originZ = waterOriginZ(py, body.sizeY);
                        // WaterBlock::UpdateFluidRegion: fluid space is +1024.
                        body.fluidX0 = px + 1024.0f;
                        body.fluidY0 = py + 1024.0f;
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
                const std::string removeWetEdges = getProp(obj.props, "removeWetEdges");
                if (!removeWetEdges.empty()) body.removeWetEdges = std::atoi(removeWetEdges.c_str()) != 0;
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
            if (!isRenderableMissionShape(obj.className) &&
                !missionClassIs(obj.className, "ForceFieldBare")) continue;
            std::string shapePath = resolveShapePath(obj, datablockShapes);
            if (!shapePath.empty()) addShapeName(shapePath);
            if (missionClassIs(obj.className, "Turret")) {
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
            playerSpawn = authoredMissionWorldPosition(authoredMissionMarker(*spawn));
            Console::instance().printf(LogLevel::Debug, "  spawn point: (%.1f, %.1f, %.1f)",
                                        playerSpawn.x, playerSpawn.y, playerSpawn.z);
        }

        // Build collision mesh from DIF interior shapes
        // Prefer hull collision data when available (more accurate)
        // Collision mesh is built after world objects are placed (see below)

        // Place objects from mission, mapping to loaded shapes
        for (auto& obj : objects) {
             if (missionClassIs(obj.className, "AudioEmitter")) {
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
             if (missionClassIs(obj.className, "Marker") ||
                  missionClassIs(obj.className, "MissionMarker") ||
                  missionClassIs(obj.className, "SpawnSphere") ||
                  missionClassIs(obj.className, "Trigger") ||
                  missionClassIs(obj.className, "PhysicalZone")) {
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
                   if (missionClassIs(obj.className, "Trigger") ||
                       missionClassIs(obj.className, "PhysicalZone")) {
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
                          marker.trigger = triggerUnitBox();
                     marker.triggerVolume = true;
                 }
                  marker.missionVolume = missionClassIs(obj.className, "Trigger") ||
                                         missionClassIs(obj.className, "PhysicalZone");
                  if (missionClassIs(obj.className, "SpawnSphere")) marker.volumeRadius = authored.radius;
                  if (missionClassIs(obj.className, "PhysicalZone")) {
                    const std::string velocity = getProp(obj.props, "velocityMod");
                    const std::string gravity = getProp(obj.props, "gravityMod");
                    const std::string force = getProp(obj.props, "appliedForce");
                     if (!velocity.empty())
                         marker.physicalVelocityMod = std::clamp((float)std::atof(velocity.c_str()), -40.0f, 40.0f);
                     if (!gravity.empty())
                         marker.physicalGravityMod = std::clamp((float)std::atof(gravity.c_str()), -40.0f, 40.0f);
                    if (sscanf(force.c_str(), "%f %f %f", &marker.physicalForce.x,
                               &marker.physicalForce.y, &marker.physicalForce.z) != 3)
                        marker.physicalForce = {};
                     const std::string active = getProp(obj.props, "active");
                     if (!active.empty()) marker.physicalActive = std::atoi(active.c_str()) != 0;
                       marker.triggerVolume = true;
                 }
                 addObject(marker);
                 continue;
             }
              if (missionClassIs(obj.className, "AIObjective")) {
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
                  objective.collidable = false;
                 objective.visible = authoredVisible(obj);
                 addObject(objective);
                 continue;
             }
            // Skip infrastructure / non-renderable classes (handled elsewhere)
             if (missionClassIs(obj.className, "SimGroup") ||
                 missionClassIs(obj.className, "MissionArea") ||
                 missionClassIs(obj.className, "TerrainBlock") ||
                 missionClassIs(obj.className, "Sky") ||
                 missionClassIs(obj.className, "Sun") ||
                 missionClassIs(obj.className, "WaterBlock") ||
                 missionClassIs(obj.className, "AudioEmitter") ||
                 missionClassIs(obj.className, "MissionMarker") ||
                 missionClassIs(obj.className, "SpawnSphere") ||
                 missionClassIs(obj.className, "NavigationGraph") ||
                 missionClassIs(obj.className, "AIObjective") ||
                 missionClassIs(obj.className, "Trigger") ||
                 missionClassIs(obj.className, "PhysicalZone") ||
                 missionClassIs(obj.className, "Precipitation") ||
                 missionClassIs(obj.className, "ParticleEmitter") ||
                 missionClassIs(obj.className, "ParticleEmissionDummy") ||
                 missionClassIs(obj.className, "Explosion") ||
                 missionClassIs(obj.className, "Lightning"))
                 continue;

            if (!isRenderableMissionShape(obj.className) &&
                !missionClassIs(obj.className, "ForceFieldBare")) continue;

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
             if (const auto* emap = scriptField(findScriptObject(getProp(obj.props, "datablock")), "emap"))
                 wo.emap = emap->toBool();
             if (missionClassIs(obj.className, "Item")) {
                 // The object's own rotate field, else its ItemData's.
                 std::string rotate = getProp(obj.props, "rotate");
                 if (rotate.empty())
                     if (const auto* field = scriptField(findScriptObject(getProp(obj.props, "datablock")), "rotate"))
                         rotate = field->toString();
                 wo.rotate = rotate == "1" || rotate == "true";
             }
             if (missionClassIs(obj.className, "InteriorInstance"))
                 if (const std::string ghost = getProp(obj.props, "ghostindex"); !ghost.empty())
                     wo.ghostIndex = std::atoi(ghost.c_str());
             if (missionClassIs(obj.className, "WayPoint")) {
                wo.label = getProp(obj.props, "name");
                wo.collidable = false;
            }
              if (missionClassIs(obj.className, "ForceFieldBare")) {
                 wo.forceField = true;
                 wo.translucent = true;
                if (const std::string ghost = getProp(obj.props, "ghostindex"); !ghost.empty())
                    wo.ghostIndex = std::atoi(ghost.c_str());
                // A demo's ForceFieldBareData comes from the recording.
                const V12::DecodedDataBlock* recorded = nullptr;
                if (sceneDataBlocks) {
                    auto it = sceneDataBlocks->find((uint32_t)std::atoi(getProp(obj.props, "datablockid").c_str()));
                    if (it != sceneDataBlocks->end() && it->second.decoded.hasForceField)
                        recorded = &it->second.decoded;
                }
                std::deque<VMValue> recordedValues;
                const ScriptObject* datablockObject = recorded ? nullptr
                    : findScriptObject(getProp(obj.props, "datablock"));
                auto datablockField = [&](const char* name) -> const VMValue* {
                    if (!recorded) return scriptField(datablockObject, name);
                    const std::string field = name;
                    auto value = [&](const std::string& v) {
                        recordedValues.push_back(VMValue(v));
                        return &recordedValues.back();
                    };
                    auto number = [&](double v) { char b[32]; snprintf(b, sizeof(b), "%g", v); return value(b); };
                    auto color = [&](const std::array<float, 4>& c) {
                        char b[96]; snprintf(b, sizeof(b), "%g %g %g %g", c[0], c[1], c[2], c[3]); return value(b); };
                    if (field == "color") return color(recorded->forceFieldColor);
                    if (field == "powerOffColor") return color(recorded->forceFieldPowerOffColor);
                    if (field == "baseTranslucency") return number(recorded->forceFieldBaseTranslucency);
                    if (field == "powerOffTranslucency") return number(recorded->forceFieldPowerOffTranslucency);
                    if (field == "fadeMS") return number(recorded->forceFieldFadeMS);
                    if (field == "umapping") return number(recorded->forceFieldUMapping);
                    if (field == "vmapping") return number(recorded->forceFieldVMapping);
                    if (field == "framesPerSec") return number(recorded->forceFieldFramesPerSec);
                    if (field == "scrollSpeed") return number(recorded->forceFieldScrollSpeed);
                    if (field == "numFrames") return number(recorded->forceFieldNumFrames);
                    for (size_t i = 0; i < recorded->forceFieldTextures.size(); ++i)
                        if (field == "texture[" + std::to_string(i) + "]")
                            return recorded->forceFieldTextures[i].empty() ? nullptr : value(recorded->forceFieldTextures[i]);
                    return nullptr;
                };
                const std::string color = datablockField("color")
                    ? datablockField("color")->toString() : "";
                float cr, cg, cb;
                if (sscanf(color.c_str(), "%f %f %f", &cr, &cg, &cb) >= 3)
                    wo.forceFieldColor = {cr, cg, cb, 1.0f};
                 if (datablockField("baseTranslucency"))
                     wo.forceFieldBaseTranslucency = datablockField("baseTranslucency")->toFloat();
                 const std::string powerOffColor = datablockField("powerOffColor")
                     ? datablockField("powerOffColor")->toString() : "";
                 float pr, pg, pb;
                 if (sscanf(powerOffColor.c_str(), "%f %f %f", &pr, &pg, &pb) >= 3)
                     wo.forceFieldPowerOffColor = {pr, pg, pb, 1.0f};
                 if (datablockField("powerOffTranslucency"))
                     wo.forceFieldPowerOffTranslucency = datablockField("powerOffTranslucency")->toFloat();
                 if (datablockField("fadeMS"))
                     wo.forceFieldFadeMS = std::max(0.0f, datablockField("fadeMS")->toFloat());
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
                 wo.forceFieldFadePosition = wo.forceFieldOpen ? wo.forceFieldFadeMS : 0.0f;
                  wo.boundsRadius = std::sqrt(wo.scale.x * wo.scale.x +
                                              wo.scale.y * wo.scale.y +
                                              wo.scale.z * wo.scale.z) * 0.5f;
              }
            std::string datablock = getProp(obj.props, "datablock");
            std::string datablockLower = datablock;
            for (char& c : datablockLower)
                c = (char)std::tolower((unsigned char)c);
            if (missionClassIs(obj.className, "Item") && datablockLower == "flag") {
                const int team = CtfRuntime::missionFlagTeam(obj.objName);
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
             wo.collidable = authoredCollidable(obj, !missionClassIs(obj.className, "Item"));
             wo.itemPickup = missionClassIs(obj.className, "Item");
             if (missionClassIs(obj.className, "WayPoint")) wo.collidable = false;

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
             if (missionClassIs(obj.className, "Turret")) {
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
             if (missionClassIs(obj.className, "ParticleEmissionDummy") ||
                 missionClassIs(obj.className, "ParticleEmitter")) {
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
             } else if (missionClassIs(obj.className, "Lightning")) {
                addSceneLightning([&](const char* field) { return getProp(obj.props, field); });
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
             if (!missionClassIs(obj.className, "Item")) continue;
            std::string db = getProp(obj.props, "datablock");
            std::string posStr = getProp(obj.props, "position");

             Point3F itemPos = parsePos(posStr);
             const ItemKind kind = classifyItemKind(db);
             if (kind == ItemKind::None)
                 continue;
             ItemPickup::Type type = kind == ItemKind::Health ? ItemPickup::Health
                 : kind == ItemKind::Energy ? ItemPickup::Energy : ItemPickup::Ammo;

              ItemPickup item;
              item.pos = itemWorldPosition(itemPos);
             item.type = type;
             // The generic ItemPickup default is the health/energy amount.
             // Ammo datablocks use the smaller native fallback even when the
             // script datablock is unavailable entirely.
             item.amount = defaultItemPickupAmount(kind);
              if (ScriptObject* script = ScriptEngine::instance().findObject(db.c_str())) {
                  auto number = [&](const char* field, float fallback) {
                      // Torque datablock fields are case-insensitive.  Do not
                      // make custom ItemData spellings fall back to stock
                      // values merely because their key capitalization differs.
                      const auto* value = scriptField(script, field);
                      return value ? value->toFloat() : fallback;
                  };
                  item.amount = itemPickupAmount(kind, number("amount", item.amount));
                  item.respawnDelay = itemRespawnDelay(
                      number("respawnTime", number("respawn", item.respawnDelay)));
             }
             for (size_t i = 0; i < worldObjects.size(); ++i) {
                 auto& wo = worldObjects[i];
                  if (wo.itemPickup && std::abs(wo.pos.x - itemPos.x) < 0.01f &&
                      std::abs(wo.pos.y - itemPos.y) < 0.01f &&
                      std::abs(wo.pos.z - itemPos.z) < 0.01f) {
                      item.worldObjectIndex = (int)i;
                      item.active = wo.itemActive;
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
                        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
                    };
                    const uint32_t faces[] = {
                        0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                        0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
                        0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2
                    };
                    const uint32_t base = vertBase;
                    for (const auto& corner : corners) {
                        // The unit box is in Torque object space.
                        const Point3F v = xform.transform(Math::torquePointToYUp(corner));
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

                // Render triangles carry the lightmap data the shape lighting
                // probe samples under a roof, in both lighting modes.
                const int objectIndex = (int)(&wo - worldObjects.data());
                for (const auto& mesh : wo.shape->meshes) {
                    auto lightmapId = [&](const std::vector<int16_t>& indices) -> uint32_t {
                        const int index = mesh.materialIdx >= 0 && mesh.materialIdx < (int)indices.size()
                            ? indices[mesh.materialIdx] : -1;
                        return index >= 0 && index < (int)wo.shape->lightmaps.size() &&
                            wo.shape->lightmaps[index].loaded ? wo.shape->lightmaps[index].id : 0u;
                    };
                    for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
                        const auto& a = mesh.vertices[mesh.indices[k]];
                        const auto& b = mesh.vertices[mesh.indices[k + 1]];
                        const auto& c = mesh.vertices[mesh.indices[k + 2]];
                        CollisionTri tri;
                        tri.v0 = fullXform.transform(a.pos);
                        tri.v1 = fullXform.transform(b.pos);
                        tri.v2 = fullXform.transform(c.pos);
                        const Point3F e1{tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
                        const Point3F e2{tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
                        Point3F n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        if (len < 1e-8f) continue;
                        tri.normal = {n.x / len, n.y / len, n.z / len};
                        lightProbeTris.push_back(tri);
                        LightProbeTriangle info;
                        info.uv[0] = a.uv2; info.uv[1] = b.uv2; info.uv[2] = c.uv2;
                        info.lightmap = lightmapId(wo.shape->materialLightmapIndex);
                        info.alarmLightmap = lightmapId(wo.shape->materialAlarmLightmapIndex);
                        info.object = objectIndex;
                        info.outsideVisible = mesh.interiorOutsideVisible;
                        lightProbeInfo.push_back(info);
                    }
                }

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

            if (!lightProbeTris.empty()) lightProbeGrid.build(lightProbeTris);
            // Mission lighting: the sun swept over the heightfield, with the
            // interiors as occluders (TerrainProxy::light).
            if (terrainBlock.loaded) {
                std::function<bool(const Point3F&)> occluder;
                Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
                for (const auto& tri : lightProbeTris)
                    for (const Point3F* v : {&tri.v0, &tri.v1, &tri.v2}) {
                        lo = {std::min(lo.x, v->x), std::min(lo.y, v->y), std::min(lo.z, v->z)};
                        hi = {std::max(hi.x, v->x), std::max(hi.y, v->y), std::max(hi.z, v->z)};
                    }
                Point3F L = sunLightDir;
                const float len = std::sqrt(L.x * L.x + L.y * L.y + L.z * L.z);
                if (!lightProbeTris.empty() && len > 0.0f && L.y > 0.0f) {
                    L = {L.x / len, L.y / len, L.z / len};
                    occluder = [this, lo, hi, L](const Point3F& p) {
                        // Above every roof nothing can shade the point.
                        if (p.y > hi.y) return false;
                        // Only rays that meet the interiors' union box are cast.
                        float t0 = 0.0f, t1 = (hi.y - p.y) / L.y + 1.0f;
                        const float o[3] = {p.x, p.y, p.z}, d[3] = {L.x, L.y, L.z};
                        const float bmin[3] = {lo.x, lo.y, lo.z}, bmax[3] = {hi.x, hi.y, hi.z};
                        for (int a = 0; a < 3; ++a) {
                            if (std::fabs(d[a]) < 1e-8f) {
                                if (o[a] < bmin[a] || o[a] > bmax[a]) return false;
                                continue;
                            }
                            float n = (bmin[a] - o[a]) / d[a], f = (bmax[a] - o[a]) / d[a];
                            if (n > f) std::swap(n, f);
                            t0 = std::max(t0, n); t1 = std::min(t1, f);
                            if (t0 > t1) return false;
                        }
                        float t; Point3F pos, normal;
                        return lightProbeGrid.raycast(lightProbeTris, p, L, (hi.y - p.y) / L.y + 1.0f, t, pos, normal);
                    };
                }
                terrainBlock.bakeLightmap(occluder);
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
            // Parse all lines from DML: first 6 = cubemap faces, 7th = emap,
            // 8-10 = cloud layers. Comments and blanks do not consume slots.
            const auto entries = parseSkyMaterialList(dmlContent);
            const auto& faceNames = entries.faces;
            emapPath = entries.environment;
            cloudPaths = entries.clouds;

            Console::instance().printf(LogLevel::Debug, "  DML face names (%zu):", faceNames.size());
            for (auto& fn : faceNames) Console::instance().printf(LogLevel::Debug, "    '%s'", fn.c_str());

            if (faceNames.size() >= 6) {
                // A material list may name a face with its extension
                // (Starfallen.dml); the name as written comes first.
                std::vector<std::string> exts = {"", ".png", ".jpg", ".bm8"};
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

    if (skyFaces.size() < 6 && !skyMaterialList.empty())
        Console::instance().printf(LogLevel::Error, "Sky: materialList '%s' could not be resolved", skyMaterialList.c_str());

    if (skyFaces.size() >= 6) {
        skyBox.load(skyFaces);
        Console::instance().printf(LogLevel::Info, "  sky loaded from: %s", skyMaterialList.c_str());

        const std::vector<std::string> exts = {".png", ".jpg", ".bm8"};
        // Load environment map (sphere map) from DML line 7
        if (!emapPath.empty()) {
            std::string emapFullPath;
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

        }

        // Cloud layers are independent of the optional environment map.
        // Stock DML files commonly omit the emap while still defining clouds.
        for (size_t ci = 0; ci < cloudPaths.size() && ci < 3; ci++) {
            Sky::CloudLayer layer;
            layer.speed = cloudSpeeds[ci];
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
    } else {
        Console::instance().printf(LogLevel::Info, "Sky: the mission names no material list");
    }

    loaded = true;
    Console::instance().printf(LogLevel::Info, "Map loaded: %s", mapName);
    if (auto* ts = Engine::instance().script().ts())
        ts->dispatchMissionCallback("onMissionStart", {VMValue(mapName ? mapName : "")});
    return true;
}

bool World::loadTerrain(const char* mapName) {
    // Headless terrain-only loader: parse the mission, find the TerrainBlock, and
    // load just the heightfield. Avoids shape/material/GL loading so a dedicated
    // server can register an authoritative ground-height callback.
    auto& fs = Engine::instance().fs();
    std::string misPath;
    std::string misData;
    if (!resolveMissionFile(fs, mapName ? mapName : "", misPath, misData)) return false;

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
    // Keep all world-owned timers on the same bounded simulation tick as
    // movement and projectiles.  Without this, a paused/invalid frame can
    // poison effect timers, while a hitch can teleport bots and animation.
    dt = precipitationDelta(dt);
    if (dt <= 0.0f) return;
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
            const Point3F converted = triggerLocalPointToYUp(local, object.scale);
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
        if (triggerActorIsActive(!Engine::instance().game().player().isDead()))
            actors.push_back({"Player", player});
        for (int index : Engine::instance().game().getLiveGhostIndices()) {
            const GhostEntry* ghost = Engine::instance().game().getLiveGhost(index);
             // MPBs and AIPlayers derive from Player in stock Tribes 2.  They
             // must participate in mission trigger volumes just like a human
             // player; filtering on the exact base class makes bot/vehicle
             // missions silently miss onEnter/onLeave callbacks.
              if (!ghost || !ObserverParity::isPositionReady(ghost->hasPosition) ||
                  !ObserverParity::isPlayerTarget(
                      ghost->className, ghost->damageState)) continue;
             // Trigger evaluation runs before the render interpolation pass.
             // Using renderPos here leaves remote players at their previous
             // (often zero) position for one or more frames, so mission
             // triggers fail to fire reliably in multiplayer.
              actors.push_back({std::to_string(index),
                                triggerNetworkPointToYUp({ghost->position.x,
                                                         ghost->position.y,
                                                         ghost->position.z})});
        }
         for (auto& trigger : worldObjects) {
             if (!trigger.triggerVolume) continue;
            std::vector<Point3F> transformed;
            transformed.reserve(trigger.trigger.vertices.size());
            for (const auto& vertex : trigger.trigger.vertices)
                transformed.push_back(transformTrigger(trigger, vertex));
            const TriggerPolyhedron worldHull = triggerFromVertices(transformed);
            std::unordered_set<std::string> current;
              const bool active = !missionClassIs(trigger.className, "PhysicalZone") ||
                                  trigger.physicalActive;
             for (const auto& actor : actors) {
                 const bool inside = active && worldHull.contains(actor.second);
                 if (inside) current.insert(actor.first);
             }
              const auto transitions = triggerTransitions(trigger.triggerOccupants, current);
              for (const auto& actor : transitions.entered) dispatch(trigger, "onEnter", actor);
              for (const auto& actor : transitions.left) dispatch(trigger, "onLeave", actor);
              trigger.triggerOccupants = std::move(current);
          }
      }

    // Update item pickups
    for (auto& item : items) {
        // Script deactivation is persistent; only advance respawn state for
        // items that are currently enabled by their mission object.
        if (!item.enabled) continue;
        if (!item.active) {
            if (itemRespawnReady(item.respawnTimer, dt)) {
                item.active = true;
                if (item.worldObjectIndex >= 0 && item.worldObjectIndex < (int)worldObjects.size())
                    worldObjects[item.worldObjectIndex].itemActive = true;
            } else {
                item.respawnTimer -= dt;
            }
            continue;
        }

        // Check player proximity
        auto& game = Engine::instance().game();
        const bool missionVisible = item.worldObjectIndex < 0 ||
            item.worldObjectIndex >= (int)worldObjects.size() ||
            worldObjects[item.worldObjectIndex].visible;
        if (!itemCanBeCollected(item.enabled, item.active, missionVisible)) continue;
        // Dead ShapeBases cannot collect items; otherwise a health pickup is
        // consumed even though applyDamage intentionally ignores dead players.
        if (game.isMapperMode() || game.player().isDead()) continue;
        const Point3F& ppos = game.player().position();
        float dx = item.pos.x - ppos.x;
        float dy = item.pos.y - ppos.y;
        float dz = item.pos.z - ppos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (itemWithinPickupRange(dist)) {
            const ItemKind kind = item.type == ItemPickup::Health ? ItemKind::Health :
                item.type == ItemPickup::Energy ? ItemKind::Energy : ItemKind::Ammo;
            int32_t currentWeapon = game.player().currentWeapon();
            float current = 0.0f;
            float maximum = 0.0f;
            bool hasAmmoWeapon = false;
            if (kind == ItemKind::Health) {
                current = game.player().health();
                // PlayerData::maxDamage is the ShapeBase health cap.  Health
                // items must use the authored armor maximum, not the stock
                // 100-point default, or high-health armor can never refill.
                maximum = game.player().maxHealth();
            } else if (kind == ItemKind::Energy) {
                current = game.player().energy();
                maximum = game.player().maxEnergy();
            } else if (currentWeapon >= 0 && currentWeapon < (int32_t)game.player().weaponCount()) {
                const int weaponType = game.player().weapon(currentWeapon).type;
                if (weaponType >= 0 && weaponType < gWeaponCount &&
                    weaponAcceptsAmmoPickup(gWeaponTable[weaponType])) {
                    hasAmmoWeapon = true;
                    current = (float)game.player().weapon(currentWeapon).ammo;
                    maximum = (float)gWeaponTable[weaponType].maxAmmo;
                }
            }
            if (!itemPickupWouldApply(kind, current, item.amount, maximum) ||
                (kind == ItemKind::Ammo && !hasAmmoWeapon)) continue;

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
                                                             item.amount, game.player().maxEnergy()));
                    break;
                case ItemPickup::Ammo: {
                    int32_t cw = currentWeapon;
                    if (cw >= 0 && cw < (int32_t)game.player().weaponCount()) {
                        const int weaponType = game.player().weapon(cw).type;
                        const float maxAmmo = weaponType >= 0 && weaponType < gWeaponCount
                            ? (float)gWeaponTable[weaponType].maxAmmo : 0.0f;
                        game.player().weapon(cw).ammo = (int)applyItemAmount(
                            ItemKind::Ammo, (float)game.player().weapon(cw).ammo, item.amount, maxAmmo);
                    }
                    // Keep each weapon slot's reserve independent.  Updating
                    // every active slot made a pickup overwrite unrelated
                    // weapon counters with the currently selected weapon's
                    // ammo.
                    game.player().updateWeaponHud();
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

        // Check dynamic bot targets along the full movement segment before
        // terrain/interior collision. Direct hits consume the projectile and
        // do not receive a second full splash-damage application.
        bool directImpact = false;
        int directBotIndex = -1;
        float directHitT = 1.0f;
        Projectile worldProbe = p;
        float probeGround = 0.0f;
        Point3F probeNormal{0, 1, 0};
        const bool probeImpact = checkProjectileCollision(worldProbe, probeGround, probeNormal);
        const float segmentDx = p.pos.x - p.previousPos.x;
        const float segmentDy = p.pos.y - p.previousPos.y;
        const float segmentDz = p.pos.z - p.previousPos.z;
        const float segmentLengthSq = segmentDx * segmentDx + segmentDy * segmentDy + segmentDz * segmentDz;
        const float probeDx = worldProbe.pos.x - p.previousPos.x;
        const float probeDy = worldProbe.pos.y - p.previousPos.y;
        const float probeDz = worldProbe.pos.z - p.previousPos.z;
        const float probeLengthSq = probeDx * probeDx + probeDy * probeDy + probeDz * probeDz;
        const bool worldBlocksBots = probeImpact ||
            (segmentLengthSq > 1.0e-6f && probeLengthSq + 1.0e-4f < segmentLengthSq);
        const float worldHitT = segmentLengthSq > 1.0e-6f
            ? std::clamp(std::sqrt(probeLengthSq / segmentLengthSq), 0.0f, 1.0f) : 1.0f;
        for (size_t botIndex = 0; botIndex < bots.size(); ++botIndex) {
            auto& bot = bots[botIndex];
            if (!bot.alive) continue;
            float hitT = 0.0f;
            // Bots use the same ShapeBase capsule bounds as network players.
            // The old sphere-only test made shots grazing a player's head or
            // feet miss locally even though the authoritative path accepted
            // them.
            if (!segmentPlayerHit(p.previousPos, p.pos, bot.pos, hitT)) continue;
            if (worldBlocksBots && hitT > worldHitT + 1.0e-4f) continue;
            // Resolve the first target along the sweep, not the first target
            // in mission/object insertion order. This matters when several
            // players line up behind one another.
            if (directImpact && hitT >= directHitT) continue;
            directImpact = true;
            directHitT = hitT;
            directBotIndex = (int)botIndex;
        }
        if (directImpact) {
            auto& bot = bots[(size_t)directBotIndex];
            p.pos = {p.previousPos.x + (p.pos.x - p.previousPos.x) * directHitT,
                     p.previousPos.y + (p.pos.y - p.previousPos.y) * directHitT,
                     p.previousPos.z + (p.pos.z - p.previousPos.z) * directHitT};
            if (isValidDamageAmount(p.damage)) {
                bot.health = applySignedDamage(bot.health, p.damage);
                if (bot.health <= 0.0f) {
                    bot.health = 0.0f;
                    bot.alive = false;
                     bot.respawnTimer = DeathRespawn::RespawnDelay;
                    Engine::instance().game().player().recordKill();
                } else {
                    bot.lastHitTime = Engine::instance().game().gameTime();
                }
            }
        }

        float groundH = 0;
        Point3F impactNormal{0, 1, 0};
        if (directImpact)
            markProjectileImpact(p);
        if (directImpact || checkProjectileCollision(p, groundH, impactNormal)) {
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

                markProjectileImpact(p);
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
               const auto& collision = game.world().collision();
               const bool visible = projectileSplashCanReach(
                   collision.loaded, !collision.loaded ||
                       collision.lineOfSight(p.pos, ppos));
               if (dist < p.splashRadius && visible) {
                   game.player().applyDamage(projectileSplashEffect(
                       p.damage, dist, p.splashRadius));
                   if (p.damage > 0.0f) {
                       const Point3F impulse = projectileSplashImpulse(
                           ppos, p.pos, dist, p.splashRadius);
                       Point3F velocity = game.player().velocity();
                       velocity.x += impulse.x;
                       velocity.y += impulse.y;
                       velocity.z += impulse.z;
                       game.player().setVelocity(velocity);
                   }
              }
            } // end mapper mode guard
            // Splash damage bots
            for (size_t botIndex = 0; botIndex < game.world().bots.size(); ++botIndex) {
                auto& b = game.world().bots[botIndex];
                if (!b.alive) continue;
                if ((int)botIndex == directBotIndex) continue;
                float bdx = p.pos.x - b.pos.x;
                 float bdy = p.pos.y - b.pos.y;
                  float bdz = p.pos.z - b.pos.z;
                  float bdist = sqrtf(bdx*bdx + bdy*bdy + bdz*bdz);
                  const auto& collision = game.world().collision();
                  const bool visible = projectileSplashCanReach(
                      collision.loaded, !collision.loaded ||
                          collision.lineOfSight(p.pos, b.pos));
                  if (bdist < p.splashRadius && visible) {
                      const float splashDamage = projectileSplashEffect(
                          p.damage, bdist, p.splashRadius);
                     if (isValidDamageAmount(p.damage)) {
                         b.health = applySignedDamage(b.health, splashDamage);
                         if (b.health <= 0) {
                              b.health = 0;
                              b.alive = false;
                               b.respawnTimer = DeathRespawn::RespawnDelay;
                              game.player().recordKill();
                          } else {
                              b.lastHitTime = Engine::instance().game().gameTime();
                          }
                      }
                }
            }
        }

        // Hitscan weapons have already represented their complete trace this
        // tick. Retire a miss as well as a hit so open-air rifle fire does not
        // leave a visible endpoint/trail alive for its fallback lifetime.
        if (projectileResolvesThisTick(p.type)) p.active = false;
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
            // AIPlayers are ShapeBases too. Resolve their patrol/strafe move
            // against loaded interiors so bots cannot walk through walls while
            // the local player is correctly blocked by the same collision mesh.
            const auto& collisionMesh = this->collision();
            if (collisionMesh.loaded) {
                Point3F pushOut{};
                if (collisionMesh.sphereCollide(b.pos, 0.5f, pushOut)) {
                    b.pos.x += pushOut.x;
                    b.pos.y += pushOut.y;
                    b.pos.z += pushOut.z;
                }
            }
            // Match player grounding on ramps and interior floors. A scalar
            // floor + 0.5 clamp makes bots float on slopes and ignores the
            // walkable-normal test used by the native controller.
            float floor = 0.0f;
            Point3F floorNormal{};
            if (botFloorContact(*this, b.pos, floor, floorNormal)) {
                const float contactY = Movement::contactHeight(floor, 0.5f, floorNormal);
                if (b.pos.y < contactY) b.pos.y = contactY;
            }
            b.animTime += dt;
        } else {
            b.respawnTimer -= dt;
            if (b.respawnTimer <= 0) {
                b.health = 100.0f;
                b.alive = true;
                b.pos = b.startPos;
                DeathRespawn::resetBotMotion(b.patrolOffset, b.moveYaw,
                                              b.animTime, b.lastHitTime);
            }
        }
    }
    } // end mapper mode guard for bots

    // Advance animation time for world objects
    for (auto& obj : worldObjects) {
        if (!obj.animName.empty() && obj.shape && obj.shape->loaded)
            obj.animTime += dt;
        if (obj.forceField) {
            const float target = obj.forceFieldOpen ? obj.forceFieldFadeMS : 0.0f;
            const float step = std::max(dt, 0.0f) * 1000.0f;
            if (obj.forceFieldFadePosition < target)
                obj.forceFieldFadePosition = std::min(target, obj.forceFieldFadePosition + step);
            else if (obj.forceFieldFadePosition > target)
                obj.forceFieldFadePosition = std::max(target, obj.forceFieldFadePosition - step);
        }
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
                          source.color.b * fade, source.radius});
    }
    renderer.setDynamicLights(lights);
}

// Precipitation from its object fields (`get`) and its PrecipitationData
// fields (`data`, lower-case names).
void World::setScenePrecipitation(const std::function<std::string(const char*)>& get,
                                  const std::function<std::string(const char*)>& data) {
    PrecipitationState ps;
    std::string s;
    // Native V12 names; retain the newer names as aliases.
    s = get("maxNumDrops");
    if (s.empty()) s = get("numDrops");
     if (!s.empty()) ps.numDrops = parsePrecipitationDropCount(s);
     ps.configuredDrops = ps.numDrops;
     s = get("type"); if (!s.empty()) ps.type = std::atoi(s.c_str());
     s = get("percentage"); if (!s.empty()) ps.percentage = (float)std::atof(s.c_str());
    s = get("maxRadius");
    if (!s.empty()) ps.boxWidth = std::max(1.0f, (float)std::atof(s.c_str()) * 2.0f);
    s = get("boxWidth"); if (!s.empty()) ps.boxWidth = (float)std::atof(s.c_str());
    s = get("boxHeight"); if (!s.empty()) ps.boxHeight = (float)std::atof(s.c_str());
    s = get("dropSize"); if (!s.empty()) ps.dropSize = (float)std::atof(s.c_str());
    s = get("minVelocity");
    if (s.empty()) s = get("minSpeed");
    if (!s.empty()) ps.minSpeed = (float)std::atof(s.c_str());
    s = get("maxVelocity");
    if (s.empty()) s = get("maxSpeed");
    if (!s.empty()) ps.maxSpeed = (float)std::atof(s.c_str());
    s = get("position");
    if (!s.empty()) ps.origin = Math::torquePointToYUp(parsePos(s));
    s = get("color1");
    if (!s.empty()) {
        float r, g, b, a = ps.color.a;
        if (sscanf(s.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)
            ps.color = {r, g, b, a};
    }
    s = get("followCam");
    if (!s.empty()) ps.followCam = (std::atoi(s.c_str()) != 0);
    s = get("useWind");
    if (!s.empty()) ps.useWind = (std::atoi(s.c_str()) != 0);
    // PrecipitationData (scripts/weather.cs) owns the drop
    // material list and quad size.
    if (std::string v = data("sizex"); !v.empty()) ps.sizeX = (float)std::atof(v.c_str());
    if (std::string v = data("sizey"); !v.empty()) ps.sizeY = (float)std::atof(v.c_str());
    if (std::string v = data("type"); !v.empty() && get("type").empty()) ps.type = std::atoi(v.c_str());
    // PrecipitationData::onAdd clamps sizes to (0, 20].
    if (ps.sizeX <= 0.0f || ps.sizeX > 20.0f) ps.sizeX = 1.0f;
    if (ps.sizeY <= 0.0f || ps.sizeY > 20.0f) ps.sizeY = 1.0f;
    if (const std::string dml = data("materiallist"); !dml.empty()) {
        auto& fs = Engine::instance().fs();
        std::string content = fs.readText(("textures/" + dml).c_str());
        if (content.empty()) content = fs.readText(dml.c_str());
        std::stringstream lines(content);
        std::string material;
        while (std::getline(lines, material)) {
            while (!material.empty() && std::isspace((unsigned char)material.back()))
                material.pop_back();
            if (material.empty()) continue;
            std::vector<float> durations;
            Engine::instance().renderer().loadTextureFrames(
                material.c_str(), ps.textures, durations);
            ps.textureDurations = std::move(durations);
            break; // drops use the list's first material
        }
    }
    s = get("textureName");
    if (s.empty()) s = get("texture");
    if (!s.empty() && ps.textures.empty()) {
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
}

// ForceFieldBare::setClientState from a demo ghost's StateChangeMask:
// Open and Opening head to fadeMS, Closing and Closed to 0; an Opening or
// Closing update also carries the current position.
void World::syncForceFieldGhost(int ghostIndex, int state, uint32_t position, int updates) {
    for (auto& object : worldObjects) {
        if (!object.forceField || object.ghostIndex != ghostIndex ||
            object.forceFieldStateUpdates == updates) continue;
        object.forceFieldStateUpdates = updates;
        object.forceFieldOpen = state == 0 || state == 1;
        if (state == 0) object.forceFieldFadePosition = object.forceFieldFadeMS;
        else if (state == 3) object.forceFieldFadePosition = 0.0f;
        else object.forceFieldFadePosition = std::clamp((float)position, 0.0f, object.forceFieldFadeMS);
    }
}

DIFLightingInstance& World::interiorLighting(WorldObject& object) {
    if (!object.interiorLight)
        object.interiorLight = std::make_shared<DIFLightingInstance>(Engine::instance().game().gameTime());
    return *object.interiorLight;
}

void World::syncInteriorAlarmGhost(int ghostIndex, bool alarm) {
    for (auto& object : worldObjects) {
        if (object.ghostIndex != ghostIndex || !object.shape || !object.shape->isInterior ||
            !object.shape->interiorLighting.hasAlarmState) continue;
        if (interiorLighting(object).setAlarm(*object.shape, alarm, Engine::instance().game().gameTime())) {
            Console::instance().printf(LogLevel::Debug, "InteriorInstance ghost %d '%s': alarm %s",
                                       ghostIndex, object.shapeName.c_str(), alarm ? "on" : "off");
            // Power transitions change the lighting of shapes standing still.
            ++interiorLightingVersion;
        }
    }
}

void World::playerTrianglesInBox(const Point3F& min, const Point3F& max,
                                 std::vector<PlayerPrediction::Triangle>& out) const {
    auto toTorque = [](const Point3F& p) { return Point3F{p.x, -p.z, p.y}; };
    auto push = [&](const Point3F& a, const Point3F& b, const Point3F& c, Point3F n) {
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len < 1e-12f) return;
        out.push_back({a, b, c, {n.x / len, n.y / len, n.z / len}});
    };
    // Torque box to Y-up extents.
    const float minX = min.x, maxX = max.x, minZ = -max.y, maxZ = -min.y;
    // Terrain: a heightfield's collision face points up.
    if (terrainBlock.loaded) {
        std::vector<Point3F> tris;
        terrainBlock.appendTrianglesInRect(minX, minZ, maxX, maxZ, tris);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const Point3F a = toTorque(tris[i]), b = toTorque(tris[i + 1]), c = toTorque(tris[i + 2]);
            if (std::max({a.z, b.z, c.z}) < min.z || std::min({a.z, b.z, c.z}) > max.z) continue;
            Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(b, a), PlayerPrediction::sub(c, a));
            if (n.z < 0) n = PlayerPrediction::mul(n, -1.0f);
            push(a, b, c, n);
        }
    }
    // Interiors: the collision hulls' faces point toward free space.
    const auto& mesh = interiorCollision;
    const auto& grid = mesh.grid;
    if (mesh.loaded && grid.resX > 0 && grid.resZ > 0) {
        auto cell = [&](float v, float lo, float w, int res) {
            return std::clamp((int)std::floor((v - lo) / w), 0, res - 1);
        };
        const int ix0 = cell(minX, grid.minX, grid.cellW, grid.resX), ix1 = cell(maxX, grid.minX, grid.cellW, grid.resX);
        const int iz0 = cell(minZ, grid.minZ, grid.cellH, grid.resZ), iz1 = cell(maxZ, grid.minZ, grid.cellH, grid.resZ);
        std::vector<int> seen;
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix)
                for (int ti : grid.cells[(size_t)iz * grid.resX + ix]) {
                    if (std::find(seen.begin(), seen.end(), ti) != seen.end()) continue;
                    seen.push_back(ti);
                    const auto& t = mesh.triangles[ti];
                    const Point3F a = toTorque(t.v0), b = toTorque(t.v1), c = toTorque(t.v2);
                    if (std::max({a.x, b.x, c.x}) < min.x || std::min({a.x, b.x, c.x}) > max.x ||
                        std::max({a.y, b.y, c.y}) < min.y || std::min({a.y, b.y, c.y}) > max.y ||
                        std::max({a.z, b.z, c.z}) < min.z || std::min({a.z, b.z, c.z}) > max.z) continue;
                    push(a, b, c, toTorque(t.normal));
                }
    }
    // Closed force fields: their box, faces outward.
    for (const auto& object : worldObjects) {
        if (!object.forceField || object.forceFieldOpen) continue;
        MatrixF rotation;
        if (object.rotAngleDeg != 0 && (object.rot.x != 0 || object.rot.y != 0 || object.rot.z != 0)) {
            Point3F axis = object.rot;
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (length > 0.0001f) {
                axis = {axis.x / length, axis.y / length, axis.z / length};
                rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(object.rotAngleDeg));
            }
        }
        const MatrixF box = rotation * Math::torqueScaleToYUp(object.scale);
        const Point3F origin = Math::torquePointToYUp(object.pos);
        Point3F corner[8];
        Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < 8; ++i) {
            // The unit box is in Torque object space.
            const Point3F y = box.transform(Math::torquePointToYUp(
                {(float)(i & 1), (float)((i >> 1) & 1), (float)((i >> 2) & 1)}));
            corner[i] = toTorque({origin.x + y.x, origin.y + y.y, origin.z + y.z});
            lo = {std::min(lo.x, corner[i].x), std::min(lo.y, corner[i].y), std::min(lo.z, corner[i].z)};
            hi = {std::max(hi.x, corner[i].x), std::max(hi.y, corner[i].y), std::max(hi.z, corner[i].z)};
        }
        if (hi.x < min.x || lo.x > max.x || hi.y < min.y || lo.y > max.y || hi.z < min.z || lo.z > max.z) continue;
        const Point3F centre = PlayerPrediction::mul(PlayerPrediction::add(lo, hi), 0.5f);
        static const int faces[12][3] = {{0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}, {0, 1, 5}, {0, 5, 4},
                                         {2, 6, 7}, {2, 7, 3}, {0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}};
        for (const auto& f : faces) {
            const Point3F a = corner[f[0]], b = corner[f[1]], c = corner[f[2]];
            Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(b, a), PlayerPrediction::sub(c, a));
            if (PlayerPrediction::dot(n, PlayerPrediction::sub(a, centre)) < 0) n = PlayerPrediction::mul(n, -1.0f);
            push(a, b, c, n);
        }
    }
}

float World::waterSurfaceAt(float x, float y) const {
    float best = std::numeric_limits<float>::quiet_NaN();
    for (const auto& body : waterBodies) {
        if (!waterBodyCanAffectSurface(body.active, body.liquidType)) continue;
        if (!waterBodyContainsHorizontal(x, -y, body.originX, body.originZ, body.sizeX, body.sizeY)) continue;
        if (!(body.level <= best)) best = body.level;
    }
    return best;
}

void World::addSceneLightning(const std::function<std::string(const char*)>& get) {
    EffectLightning lightning;
    lightning.pos = Math::torquePointToYUp(parsePos(get("position")));
    lightning.scale = parsePos(get("scale"));
    if (get("scale").empty()) lightning.scale = {1, 1, 1};
    Point3F axis{0, 0, 1}; float angle = 0.0f;
    sscanf(get("rotation").c_str(), "%f %f %f %f", &axis.x, &axis.y, &axis.z, &angle);
    lightning.rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(angle));
    auto value = [&](const char* field, float fallback) { const auto s = get(field); return s.empty() ? fallback : (float)std::atof(s.c_str()); };
    lightning.strikeWidth = value("strikeWidth", 1.0f);
    lightning.strikesPerMinute = value("strikesPerMinute", 0.0f);
    lightning.strikeRadius = value("strikeRadius", 0.0f);
    lightning.boltStartRadius = value("boltStartRadius", 0.0f);
    lightning.chanceToHitTarget = value("chanceToHitTarget", 0.0f);
    auto color = [&](const char* field, ColorF fallback) { float r, g, b, a; const auto s = get(field); return sscanf(s.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3 ? ColorF{r, g, b, a} : fallback; };
    lightning.color = color("color", lightning.color);
    lightning.fadeColor = color("fadeColor", lightning.fadeColor);
    lightning.nextStrike = lightning.strikesPerMinute > 0.0f ? 60.0f / lightning.strikesPerMinute : 0.0f;
    effectLightnings.push_back(std::move(lightning));
}

void World::addFootPuff(const Point3F& pos, uint32_t emitterRef, float radius, int count,
                        const ColorF colors[2], const std::map<uint32_t, ParsedDataBlock>& dataBlocks) {
    auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks.find(id);
        return it == dataBlocks.end() ? nullptr : &it->second.decoded;
    };
    const auto* emitterBlock = find(emitterRef);
    if (!emitterBlock || !emitterBlock->hasEmitter || emitterBlock->emitter.particleRefs.empty()) return;
    const auto* particleBlock = find(emitterBlock->emitter.particleRefs.front());
    if (!particleBlock || !particleBlock->hasParticle) return;
    EffectEmitter emitter;
    emitter.pos = pos;
    emitter.axis = {0, 1, 0};
    emitter.emitter = emitterBlock->emitter;
    emitter.particle = particleBlock->particle;
    // ParticleEmitter::setColors: the terrain's two puff colours, then
    // transparent white.
    emitter.emitter.useEmitterColors = true;
    emitter.emitter.colors.assign(4, {});
    for (int i = 0; i < 4; ++i) {
        const ColorF c = i < 2 ? colors[i] : ColorF{1, 1, 1, 0};
        emitter.emitter.colors[i].red = c.r;
        emitter.emitter.colors[i].green = c.g;
        emitter.emitter.colors[i].blue = c.b;
        emitter.emitter.colors[i].alpha = c.a;
    }
    for (const auto& textureName : emitter.particle.textures) {
        std::vector<uint32_t> frames; std::vector<float> durations;
        Engine::instance().renderer().loadTextureFrames(textureName.c_str(), frames, durations);
        emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
        emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
    }
    if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
    emitter.burst = true;
    emitter.burstCount = std::clamp(count, 0, 512);
    emitter.radialRadius = std::max(0.0f, radius);
    emitter.radialNormal = {0, 1, 0};
    emitter.nextEmission = -1.0f;
    if (effectEmitters.size() < 4096) effectEmitters.push_back(std::move(emitter));
}

void World::addFootprint(const Point3F& pos, const Point3F& normal, const Point3F& forward, uint32_t decalRef,
                         const std::map<uint32_t, ParsedDataBlock>& dataBlocks) {
    auto it = dataBlocks.find(decalRef);
    if (it == dataBlocks.end() || !it->second.decoded.hasDecal || it->second.decoded.decal.texture.empty()) return;
    const auto& data = it->second.decoded.decal;
    EffectDecal decal;
    decal.data = data;
    decal.sourceRef = decalRef;
    const DecalBasis basis = makeDecalBasis(pos, normal);
    decal.pos = basis.position;
    decal.normal = basis.normal;
    decal.up = forward;
    decal.sizeX = std::max(0.001f, std::fabs(data.sizeX));
    decal.sizeY = std::max(0.001f, std::fabs(data.sizeY));
    decal.lifetime = std::max(0.1f, data.lifetimeMS / 1000.0f);
    Engine::instance().renderer().loadTextureFrames(data.texture.c_str(), decal.textures, decal.textureDurations);
    if (!decal.textures.empty()) decal.texture = decal.textures.front();
    if (effectDecals.size() < 4096) effectDecals.push_back(std::move(decal));
}

void World::addSceneEmitter(const Point3F& pos, const Point3F& axis,
                            const V12::DecodedDataBlock::ParticleEmitterData& emitterData,
                            const V12::DecodedDataBlock::ParticleData& particle) {
    EffectEmitter emitter;
    emitter.pos = pos;
    emitter.axis = axis;
    emitter.emitter = emitterData;
    emitter.particle = particle;
    for (const auto& textureName : emitter.particle.textures) {
        std::vector<uint32_t> frames; std::vector<float> durations;
        Engine::instance().renderer().loadTextureFrames(textureName.c_str(), frames, durations);
        emitter.textures.insert(emitter.textures.end(), frames.begin(), frames.end());
        emitter.textureDurations.insert(emitter.textureDurations.end(), durations.begin(), durations.end());
    }
    if (!emitter.textures.empty()) emitter.texture = emitter.textures.front();
    effectEmitters.push_back(std::move(emitter));
}

void World::render(const Point3F& cameraPos, float dt) {
    static float forceFieldTime = 0.0f;
    // ForceField animation is simulation-time driven in Torque. Using a fixed
    // increment per render makes frames and UV scrolling run at the wrong rate
    // on non-60 Hz displays and continue while the game is paused.
    if (std::isfinite(dt) && dt > 0.0f)
        forceFieldTime += std::min(dt, 0.1f);
    if (!loaded) return;

    auto& r = Engine::instance().renderer();

    currentSceneState.cameraPosition = cameraPos;
    currentSceneState.view = r.view;
    currentSceneState.projection = r.projection;
    currentSceneState.interiorVisibleZones.clear();
    currentSceneState.interiorVisibilityComputed.clear();

    // Render sky first (behind everything, depth writes off); the commander
    // map's pass (renderScene(TerrainObjectType | InteriorObjectType |
    // WaterObjectType)) has none.
    if (!commanderPass) {
    glDepthMask(GL_FALSE);
    skyBox.fogColor = fog.color;
    skyBox.visibleDistance = visibleDistance;
    skyBox.fogVolumes.clear();
    for (const auto& volume : fogVolumes)
        skyBox.fogVolumes.push_back({volume.visibleDistance, volume.minHeight,
                                     volume.maxHeight, volume.percentage});
    skyBox.render(r.view, r.projection, cameraPos.y);
    glDepthMask(GL_TRUE);
    }

    // Haze runs from the Sky's fogDistance to visibleDistance; fog volumes add
    // their own band fog in the shader.
    const float effectiveFogStart = fog.distance;
    const float effectiveFogEnd = visibleDistance;

    // Render terrain
    if (terrainBlock.loaded) {
        ShaderManager::getTerrainShader()->bind();
        Point3F terrainLight = sunLightDirUsed ? sunLightDir : Point3F{0.5f, 0.7f, 0.5f};
        bool mapperNoFog = Engine::instance().game().isMapperMode() || commanderPass;
        terrainBlock.render(cameraPos, fog.enabled && !mapperNoFog, fog.color, fog.density, &terrainLight,
                            sunColorUsed ? &sunColor : nullptr, &sunAmbient, effectiveFogStart, effectiveFogEnd);
    }

    // Render world objects with default shader
    auto* defShader = ShaderManager::getDefaultShader();
    defShader->bind();
    defShader->setUniform("uCamPos", cameraPos);
    for (int i = 0; i < 3; ++i) {
        ColorF packed{};
        if (!commanderPass && i < (int)fogVolumes.size() && fogVolumes[i].visibleDistance > 0.0f) {
            const auto& volume = fogVolumes[i];
            packed = {1.0f / volume.visibleDistance, volume.minHeight,
                       volume.maxHeight, volume.percentage};
        }
        defShader->setUniform((std::string("uFogVolume") + std::to_string(i)).c_str(), packed);
    }
    // Volume fog rows span the terrain height range, each centred half a
    // step up (SceneGraph::buildFogTexture); no terrain, no rows.
    float fogRowBase = 0.0f, fogRowStep = 0.0f;
    if (terrain() && terrain()->loaded) {
        float lo, hi;
        terrain()->heightRange(lo, hi);
        fogRowStep = std::max(0.0f, (hi - lo) / 64.0f);
        fogRowBase = lo + fogRowStep * 0.5f;
    }
    defShader->setUniform("uFogRowBase", fogRowBase);
    defShader->setUniform("uFogRowStep", fogRowStep);

    // Apply fog
    const bool mapperNoFog = Engine::instance().game().isMapperMode() || commanderPass;
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

    // The fixed indoor shape light (installLights), toward the light.
    {
        const Point3F towardLight = Math::torquePointToYUp({
            -ShapeLighting::InteriorDirection[0], -ShapeLighting::InteriorDirection[1],
            -ShapeLighting::InteriorDirection[2]});
        defShader->setUniform("uShapeInteriorDir", towardLight);
    }

    // Bind environment map from sky (for reflections on shapes)
    if (skyBox.emap.loaded) {
        skyBox.emap.bind(2);
        defShader->setUniform("uEnvMap", (int32_t)2);
    }

    std::vector<WorldObject*> renderQueue;
    renderQueue.reserve(worldObjects.size());
    for (auto& obj : worldObjects) {
        // Hidden SceneObjects must not contribute any render pass. ForceField
        // geometry is drawn outside the normal shape branch below, so the
        // later shape visibility check cannot suppress it.
        if (!obj.visible) continue;
        if (commanderPass) {
            if (obj.shape && obj.shape->isInterior) renderQueue.push_back(&obj);
            continue;
        }
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
        // Objects inside an interior are scoped by its visible zones. An
        // interior culls its own zones (activeInteriorZones below); its origin
        // lying in a zone the camera cannot see must not hide all of it.
        const bool isInterior = obj.shape && obj.shape->isInterior;
        if (!Engine::instance().game().isMapperMode() && !isInterior &&
            !isObjectVisible(obj, cameraPos))
            continue;
        if (obj.shape && obj.shape->loaded) {
             if (!obj.visible) continue;
             const bool mapperMarker = Engine::instance().game().isMapperMode() &&
                  (missionClassIs(obj.className, "Marker") ||
                   missionClassIs(obj.className, "MissionMarker") ||
                   missionClassIs(obj.className, "SpawnSphere") ||
                   missionClassIs(obj.className, "Trigger") ||
                   missionClassIs(obj.className, "PhysicalZone") ||
                   missionClassIs(obj.className, "WayPoint") ||
                   missionClassIs(obj.className, "AIObjective"));
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
            if (obj.rotate) {
                // A rotating Item turns once every 3 s about its up axis.
                MatrixF spin;
                spin.setRotationAxis({0, 1, 0},
                    std::fmod(Engine::instance().game().gameTime() / 3.0f, 1.0f) * 2.0f * Math::PI);
                model = spin * model;
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
                model = model * Math::nativeDtsFrame();
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
            obj.shape->emapEnabled = obj.emap;
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
            if (!obj.shape->isInterior)
                applyShapeLighting(*obj.shape, obj.shapeLight, model * obj.shape->upOrientation(),
                                   dt * 1000.0f);
            if (obj.shape->isInterior && obj.shape->interiorLighting.animated()) {
                DIFLightingInstance& lighting = interiorLighting(obj);
                lighting.prepare(*obj.shape, Engine::instance().game().gameTime());
                obj.shape->interiorLightingInstance = &lighting;
            }
            if (!obj.animName.empty())
                obj.shape->renderAnimation(obj.animName.c_str(), obj.animTime, nullptr, 0,
                                           DTSShape::SelectDetail);
            else
                obj.shape->render(DTSShape::SelectDetail);
            obj.shape->interiorLightingInstance = nullptr;
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
                // Mounted images are lit with their owner (ShapeBase renders
                // them inside its own light set).
                obj.mountedShape->lighting = obj.shape->lighting;
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
                                settled->name.c_str(), settled->duration, nullptr, 0,
                                DTSShape::SelectDetail);
                        else
                            obj.mountedShape->render(DTSShape::SelectDetail);
                    } else {
                        obj.mountedShape->render(DTSShape::SelectDetail);
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
            Point3F transformed[8];
            // The unit box is in Torque object space (+y is Y-up -z).
            for (int i = 0; i < 8; ++i) transformed[i] = fieldModel.transform(Math::torquePointToYUp(corners[i]));
            const float fieldAlpha = obj.forceFieldFadeMS > 0.0f
                ? std::clamp(1.0f - obj.forceFieldFadePosition / obj.forceFieldFadeMS, 0.0f, 1.0f)
                : (obj.forceFieldOpen ? 0.0f : 1.0f);
            // ForceFieldBare renders every face of its box additively and
            // double-sided.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_FALSE);
              if (fieldAlpha > 0.0f && !obj.forceFieldFrames.empty()) {
                 const size_t frame = obj.forceFieldFrameDurations.size() == obj.forceFieldFrames.size()
                     ? textureFrameIndex(obj.forceFieldFrameDurations,
                                         obj.forceFieldFrames.size(), forceFieldTime)
                     : (obj.forceFieldFramesPerSec > 0.0f
                         ? (size_t)(forceFieldTime * obj.forceFieldFramesPerSec) % obj.forceFieldFrames.size()
                         : 0);
                const float u = obj.forceFieldUMapping;
                const float v = obj.forceFieldVMapping;
                const float scroll = forceFieldTime * obj.forceFieldScrollSpeed;
                 const ColorF tint = {
                     obj.forceFieldPowerOffColor.r +
                         (obj.forceFieldColor.r - obj.forceFieldPowerOffColor.r) * fieldAlpha,
                     obj.forceFieldPowerOffColor.g +
                         (obj.forceFieldColor.g - obj.forceFieldPowerOffColor.g) * fieldAlpha,
                     obj.forceFieldPowerOffColor.b +
                         (obj.forceFieldColor.b - obj.forceFieldPowerOffColor.b) * fieldAlpha,
                     obj.forceFieldPowerOffTranslucency +
                         (obj.forceFieldBaseTranslucency - obj.forceFieldPowerOffTranslucency) * fieldAlpha};
                // ForceFieldBare::renderObject: the colour fades to the fog
                // colour (alpha 0) by the fog at the field's distance.
                ColorF fieldTint = tint;
                if (!Engine::instance().game().isMapperMode()) {
                    const Point3F centre = fieldModel.transform({0.5f, 0.5f, 0.5f});
                    const float dx = centre.x - cameraPos.x, dy = centre.y - cameraPos.y, dz = centre.z - cameraPos.z;
                    const float fogAmount = hazeAt(*this, std::sqrt(dx * dx + dy * dy + dz * dz));
                    fieldTint = {tint.r + (fog.color.r - tint.r) * fogAmount, tint.g + (fog.color.g - tint.g) * fogAmount,
                                 tint.b + (fog.color.b - tint.b) * fogAmount, tint.a * (1.0f - fogAmount)};
                }
                const uint32_t texture = obj.forceFieldFrames[frame];
                r.drawTexturedQuad(transformed[0], transformed[1], transformed[2], transformed[3], texture, fieldTint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[4], transformed[7], transformed[6], transformed[5], texture, fieldTint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[0], transformed[4], transformed[5], transformed[1], texture, fieldTint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[3], transformed[2], transformed[6], transformed[7], texture, fieldTint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[0], transformed[3], transformed[7], transformed[4], texture, fieldTint, 0, scroll, u, v + scroll);
                r.drawTexturedQuad(transformed[1], transformed[5], transformed[6], transformed[2], texture, fieldTint, 0, scroll, u, v + scroll);
            }
            glDepthMask(GL_TRUE);
            if (cullWasEnabled) glEnable(GL_CULL_FACE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_BLEND);
        }

        // MissionMarker is only in the runtime scene while editing in the
        // original engine.  Mapper mode is Torch's editor equivalent, so
        // show the authored marker and volume bounds there without changing
        // gameplay collision semantics.
        if (Engine::instance().game().isMapperMode() &&
            (missionClassIs(obj.className, "Marker") ||
             missionClassIs(obj.className, "MissionMarker") ||
             missionClassIs(obj.className, "SpawnSphere") ||
             missionClassIs(obj.className, "Trigger") ||
             missionClassIs(obj.className, "PhysicalZone") ||
             missionClassIs(obj.className, "AIObjective"))) {
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
            if (missionClassIs(obj.className, "SpawnSphere")) {
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
                if (missionClassIs(obj.className, "Trigger") && obj.trigger.vertices.size() >= 4) {
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
                 const ColorF color = missionClassIs(obj.className, "Trigger")
                    ? ColorF{1.0f, 0.7f, 0.2f, 0.8f}
                    : ColorF{0.2f, 0.7f, 1.0f, 0.8f};
                for (const auto& edge : edges)
                    r.drawLine(corners[edge[0]], corners[edge[1]], color);
            } else {
                r.drawLine(center, markerModel.transform({0, 1, 0}),
                            missionClassIs(obj.className, "AIObjective")
                               ? ColorF{1.0f, 0.3f, 0.8f, 0.9f}
                               : ColorF{1.0f, 1.0f, 0.2f, 0.9f});
            }
        }
    }
    if (commanderPass) {
        if (!waterRendered) {
            r.flushSpriteBatch();
            renderWater();
        }
        return;
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
            b.shape->render(DTSShape::SelectDetail);
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
                    ScreenSpace2D screenSpace(ren);
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
    dispatchMissionLifecycle(added, "onAdd");
}

void World::resetTriggerTracking() {
    for (auto& object : worldObjects)
        object.triggerOccupants.clear();
}

static std::string resolveMissionObjectName(const std::string& value);

bool World::setMissionObjectEnabled(const std::string& name, bool enabled) {
    const std::string resolvedName = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolvedName) continue;
        if (missionClassIs(object.className, "PhysicalZone")) {
            object.physicalActive = enabled;
            triggerVolumeChanged(object.triggerOccupants);
            return true;
        }
        if (missionClassIs(object.className, "ForceFieldBare")) {
            object.forceFieldOpen = !enabled;
            return true;
        }
        if (missionClassIs(object.className, "Item")) {
            object.itemActive = enabled;
            // Item pickups have a separate runtime record used by the
            // collection loop. Keep it in lockstep with the mission object;
            // otherwise deactivate() only hides the shape while the player
            // can still collect the item.
            for (auto& item : items) {
                if (item.worldObjectIndex < 0 ||
                    item.worldObjectIndex >= (int)worldObjects.size() ||
                     &worldObjects[item.worldObjectIndex] != &object)
                    continue;
                item.enabled = enabled;
                if (!enabled) {
                    item.active = false;
                } else {
                    item.active = itemActiveAfterEnable(item.active, item.respawnTimer);
                    if (item.active) item.respawnTimer = 0.0f;
                }
                object.itemActive = item.active;
            }
            return true;
        }
        return false;
    }
    return false;
}

bool World::setMissionObjectHidden(const std::string& name, bool hidden) {
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!missionObjectCanBeHidden(object.className)) return false;
        object.visible = !hidden;
        return true;
    }
    return false;
}

static bool supportsMissionTransform(const World::WorldObject& object) {
    // All authored scene objects have a transform in Torque, including
    // volumes and markers that do not render a DTS shape.
    return !object.objectName.empty() && object.className != "AudioEmitter";
}

static std::string resolveMissionObjectName(const std::string& value) {
    char* end = nullptr;
    const long id = std::strtol(value.c_str(), &end, 10);
    if (end && *end == '\0' && id > 0) {
        for (const auto& object : ScriptEngine::instance().missionObjects())
            if (object.id == id) return object.name;
    }
    for (const auto& object : ScriptEngine::instance().missionObjects())
        if (missionObjectNameIs(object.name, value)) return object.name;
    return value;
}

bool World::setMissionObjectPosition(const std::string& name, const Point3F& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        object.pos = position;
        if (object.triggerVolume) triggerVolumeChanged(object.triggerOccupants);
        // Item pickups keep a converted runtime position separate from the
        // authored WorldObject transform. Keep both in sync when scripts move
        // an item, otherwise its mesh and collection volume diverge.
        if (missionClassIs(object.className, "Item")) {
            for (auto& item : items) {
                if (item.worldObjectIndex < 0 ||
                    item.worldObjectIndex >= (int)worldObjects.size() ||
                    &worldObjects[item.worldObjectIndex] != &object)
                    continue;
                item.pos = itemWorldPosition(position);
            }
        }
        return true;
    }
    return false;
}

bool World::getMissionObjectPosition(const std::string& name, Point3F& position) const {
    const std::string resolved = resolveMissionObjectName(name);
    for (const auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        position = object.pos;
        return true;
    }
    return false;
}

bool World::setMissionObjectRotation(const std::string& name, const Point3F& axis, float angleDeg) {
    if (!std::isfinite(axis.x) || !std::isfinite(axis.y) || !std::isfinite(axis.z) || !std::isfinite(angleDeg)) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        object.rot = axis;
        object.rotAngleDeg = angleDeg;
        if (object.triggerVolume) triggerVolumeChanged(object.triggerOccupants);
        return true;
    }
    return false;
}

bool World::getMissionObjectRotation(const std::string& name, Point3F& axis, float& angleDeg) const {
    const std::string resolved = resolveMissionObjectName(name);
    for (const auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        axis = object.rot;
        angleDeg = object.rotAngleDeg;
        return true;
    }
    return false;
}

bool World::setMissionObjectScale(const std::string& name, const Point3F& scale) {
    if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
        scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        object.scale = scale;
        if (object.triggerVolume) triggerVolumeChanged(object.triggerOccupants);
        return true;
    }
    return false;
}

bool World::getMissionObjectScale(const std::string& name, Point3F& scale) const {
    const std::string resolved = resolveMissionObjectName(name);
    for (const auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        scale = object.scale;
        return true;
    }
    return false;
}

bool World::setMissionObjectTransform(const std::string& name, const std::string& transform) {
    float values[7]{};
    const char* cursor = transform.c_str();
    for (float& value : values) {
        while (std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        char* end = nullptr;
        value = std::strtof(cursor, &end);
        if (end == cursor) return false;
        cursor = end;
    }
    while (std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (*cursor != '\0') return false;
    for (float value : values) if (!std::isfinite(value)) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        object.pos = {values[0], values[1], values[2]};
        if (object.triggerVolume) triggerVolumeChanged(object.triggerOccupants);
        // setTransform is also a supported script path for moving Item
        // objects. Keep the pickup volume aligned with the rendered item just
        // as setMissionObjectPosition does.
        if (missionClassIs(object.className, "Item")) {
            for (auto& item : items) {
                if (item.worldObjectIndex < 0 ||
                    item.worldObjectIndex >= (int)worldObjects.size() ||
                    &worldObjects[item.worldObjectIndex] != &object)
                    continue;
                item.pos = itemWorldPosition(object.pos);
            }
        }
        object.rot = {values[3], values[4], values[5]};
        object.rotAngleDeg = values[6];
        return true;
    }
    return false;
}

bool World::getMissionObjectTransform(const std::string& name, std::string& transform) const {
    const std::string resolved = resolveMissionObjectName(name);
    for (const auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        if (!supportsMissionTransform(object)) return false;
        char value[160];
        snprintf(value, sizeof(value), "%g %g %g %g %g %g %g",
                 object.pos.x, object.pos.y, object.pos.z,
                 object.rot.x, object.rot.y, object.rot.z, object.rotAngleDeg);
        transform = value;
        return true;
    }
    return false;
}

bool World::mountMissionObjectImage(const std::string& name, const std::string& image, int slot) {
    if (image.empty() || slot < 0 || slot >= 8) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        const std::string lowerClass = missionRulesLower(object.className);
        if (!missionClassIs(object.className, "Turret") && !lowerClass.ends_with("turret") &&
            !missionClassIs(object.className, "Vehicle") &&
            !lowerClass.ends_with("vehicle")) return false;
        object.mountedImages[slot] = image;
        if (slot == 0) object.mountedShapeName = image;
        if (auto* ts = ScriptEngine::instance().ts()) {
            const std::string callback = object.className + "::onMount";
            if (ts->hasFunction(callback))
                 ts->callFunction(callback, {VMValue(resolved), VMValue(image), VMValue(slot)});
        }
        return true;
    }
    return false;
}

bool World::unmountMissionObjectImage(const std::string& name, int slot) {
    if (slot < 0 || slot >= 8) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        const std::string lowerClass = missionRulesLower(object.className);
        if (!missionClassIs(object.className, "Turret") && !lowerClass.ends_with("turret") &&
            !missionClassIs(object.className, "Vehicle") &&
            !lowerClass.ends_with("vehicle")) return false;
        const std::string image = object.mountedImages[slot];
        object.mountedImages[slot].clear();
        if (slot == 0) {
            object.mountedShapeName.clear();
            object.mountedShape = nullptr;
        }
        if (auto* ts = ScriptEngine::instance().ts()) {
            const std::string callback = object.className + "::onUnmount";
            if (ts->hasFunction(callback))
                 ts->callFunction(callback, {VMValue(resolved), VMValue(image), VMValue(slot)});
        }
        return true;
    }
    return false;
}

bool World::getMissionObjectImage(const std::string& name, int slot, std::string& image) const {
    if (slot < 0 || slot >= 8) return false;
    const std::string resolved = resolveMissionObjectName(name);
    for (const auto& object : worldObjects) {
        if (object.objectName != resolved) continue;
        const std::string lowerClass = missionRulesLower(object.className);
        if (!missionClassIs(object.className, "Turret") && !lowerClass.ends_with("turret") &&
            !missionClassIs(object.className, "Vehicle") && !lowerClass.ends_with("vehicle")) return false;
        image = object.mountedImages[slot];
        return true;
    }
    return false;
}

bool World::deleteMissionObject(const std::string& name) {
    if (name.empty()) return false;
    const std::string resolved = resolveMissionObjectName(name);
    auto it = std::find_if(worldObjects.begin(), worldObjects.end(),
        [&resolved](const WorldObject& object) { return object.objectName == resolved; });
    if (it == worldObjects.end()) return false;
    if (auto* ts = ScriptEngine::instance().ts())
        // Console APIs accept either an object name or its numeric ID. Event
        // ownership and trigger occupants are keyed by the canonical name;
        // using the original numeric token leaves callbacks from a deleted
        // mission object alive until their original deadline.
        ts->cancelEventsForObject(resolved);
    dispatchMissionLifecycle(*it, "onRemove");
    const int removedIndex = (int)std::distance(worldObjects.begin(), it);
    items.erase(std::remove_if(items.begin(), items.end(),
        [removedIndex](const ItemPickup& item) {
            return item.worldObjectIndex == removedIndex;
        }), items.end());
    worldObjects.erase(it);
    for (auto& item : items)
        if (item.worldObjectIndex > removedIndex) --item.worldObjectIndex;
    for (auto& object : worldObjects) object.triggerOccupants.erase(resolved);
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


bool World::sampleInteriorLight(int triangle, const Point3F& point, ShapeLighting::Color& out) {
    if (triangle < 0 || triangle >= (int)lightProbeInfo.size()) return false;
    const LightProbeTriangle& info = lightProbeInfo[triangle];
    const CollisionTri& tri = lightProbeTris[triangle];
    // The interior's current mode picks the lightmap; animated lights are
    // not sampled (the base image).
    const bool alarm = info.object >= 0 && info.object < (int)worldObjects.size() &&
        worldObjects[info.object].interiorLight && worldObjects[info.object].interiorLight->alarm();
    const uint32_t lightmap = alarm ? info.alarmLightmap : info.lightmap;
    if (!lightmap) return false;
    auto cached = lightmapPixelCache.find(lightmap);
    if (cached == lightmapPixelCache.end()) {
        LightmapPixels pixels;
        glBindTexture(GL_TEXTURE_2D, lightmap);
        GLint w = 0, h = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            pixels.width = w;
            pixels.height = h;
            pixels.rgba.resize((size_t)w * h * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.rgba.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        cached = lightmapPixelCache.emplace(lightmap, std::move(pixels)).first;
    }
    const LightmapPixels& px = cached->second;
    if (px.width <= 0) return false;
    // Barycentric lightmap UV at the hit point.
    const Point3F v0{tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    const Point3F v1{tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    const Point3F v2{point.x - tri.v0.x, point.y - tri.v0.y, point.z - tri.v0.z};
    auto dot = [](const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    const float d20 = dot(v2, v0), d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    float wb = 0.0f, wc = 0.0f;
    if (std::fabs(denom) > 1e-12f) {
        wb = (d11 * d20 - d01 * d21) / denom;
        wc = (d00 * d21 - d01 * d20) / denom;
    }
    const float wa = 1.0f - wb - wc;
    const float u = info.uv[0].x * wa + info.uv[1].x * wb + info.uv[2].x * wc;
    const float v = info.uv[0].y * wa + info.uv[1].y * wb + info.uv[2].y * wc;
    // Bilinear, clamped to the edge, as the lightmap sampler.
    const float fx = std::clamp(u * px.width - 0.5f, 0.0f, (float)(px.width - 1));
    const float fy = std::clamp(v * px.height - 0.5f, 0.0f, (float)(px.height - 1));
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = std::min(x0 + 1, px.width - 1), y1 = std::min(y0 + 1, px.height - 1);
    const float tx = fx - x0, ty = fy - y0;
    float channel[3];
    for (int c = 0; c < 3; ++c) {
        auto at = [&](int x, int y) { return (float)px.rgba[((size_t)y * px.width + x) * 4 + c]; };
        channel[c] = ((at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
                      (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty) / 255.0f;
    }
    out = {channel[0], channel[1], channel[2]};
    if (info.outsideVisible) {
        // Outside-visible surfaces add clamp(sun*NdotL + ambient).
        const float len = std::sqrt(dot(sunLightDir, sunLightDir));
        const float nDotL = len > 0.0f ? std::max(dot(tri.normal, sunLightDir) / len, 0.0f) : 0.0f;
        out.r = std::min(1.0f, out.r + std::min(1.0f, sunColor.r * nDotL + sunAmbient.r));
        out.g = std::min(1.0f, out.g + std::min(1.0f, sunColor.g * nDotL + sunAmbient.g));
        out.b = std::min(1.0f, out.b + std::min(1.0f, sunColor.b * nDotL + sunAmbient.b));
    }
    return true;
}

bool World::probeShapeLighting(const Point3F& center, int& mode, ShapeLighting::Color& color) {
    const float reach = ShapeLighting::ProbeReach;
    float t = 0.0f;
    Point3F hit{}, normal{};
    int triangle = -1;
    if (!lightProbeTris.empty() &&
        lightProbeGrid.raycast(lightProbeTris, center, {0, 1, 0}, reach, t, hit, normal, &triangle)) {
        // Under a roof: the interior lighting at the floor below.
        triangle = -1;
        if (!lightProbeGrid.raycast(lightProbeTris, center, {0, -1, 0}, reach, t, hit, normal, &triangle))
            return false;
        // Interiors without lighting data light shapes white.
        if (!sampleInteriorLight(triangle, hit, color)) color = {1.0f, 1.0f, 1.0f};
        mode = ShapeLighting::Interior;
        return true;
    }
    const float ndotl = terrainBlock.loaded ? terrainBlock.sampleLightmapNdotL(center.x, center.z) : -1.0f;
    if (ndotl >= 0.0f && terrainBlock.contains(center.x, center.z)) {
        color = ShapeLighting::terrainTexelLighting(ndotl, {sunColor.r, sunColor.g, sunColor.b},
                                                    {sunAmbient.r, sunAmbient.g, sunAmbient.b});
        mode = ShapeLighting::Terrain;
        return true;
    }
    mode = ShapeLighting::Sun;
    color = {1.0f, 1.0f, 1.0f};
    return true;
}

void World::applyShapeLighting(DTSShape& shape, ShapeLighting::State& state,
                               const MatrixF& renderModel, float dtMs) {
    const Point3F center = renderModel.transform(shape.boundsCenter());
    if (ShapeLighting::needsProbe(state, center.x, center.y, center.z) ||
        state.interiorLightingVersion != interiorLightingVersion) {
        state.interiorLightingVersion = interiorLightingVersion;
        int mode = state.mode;
        ShapeLighting::Color color = state.target;
        if (probeShapeLighting(center, mode, color))
            ShapeLighting::recordProbe(state, center.x, center.y, center.z, mode, color);
        else
            state.hasProbe = true, state.lastX = center.x, state.lastY = center.y, state.lastZ = center.z;
    }
    ShapeLighting::advance(state, dtMs);
    shape.lighting.mode = state.mode;
    shape.lighting.color = state.mode == ShapeLighting::Sun
        ? ColorF{1.0f, 1.0f, 1.0f, 1.0f}
        : ColorF{state.color.r, state.color.g, state.color.b, 1.0f};
}

float World::getHeight(float x, float z) const {
    // First check interior collision (buildings, etc.)
    if (interiorCollision.loaded) {
        float interiorH = interiorCollision.getHeight(x, z);
        if (interiorH > -1e9f) return interiorH;
    }

    // Fall back to terrain height
    if (!terrainBlock.loaded || terrainBlock.heights.empty() ||
        !terrainBlock.contains(x, z) ||
        terrainBlock.isEmptySquare(x, z)) return -1e9f;

    return terrainBlock.sampleHeight(x, z);
}

float World::getFloorHeight(float x, float y, float z) const {
    if (interiorCollision.loaded) {
        const float interior = interiorCollision.getFloorHeight(x, y, z);
        if (interior > -1e9f) return interior;
    }
    if (!terrainBlock.loaded || terrainBlock.heights.empty() ||
        !terrainBlock.contains(x, z) || terrainBlock.isEmptySquare(x, z))
        return -1e9f;
    // A floor query must remain valid while an actor is penetrating the
    // surface.  Rejecting terrain above the query point makes a player that
    // crosses the terrain in one tick lose its floor and fall through it.
    // Callers already distinguish a floor from a ceiling by the query
    // direction/state, so return the authored terrain height unconditionally.
    return terrainBlock.sampleHeight(x, z);
}

World::PhysicalZoneEffect World::physicalZoneEffect(const Point3F& position) const {
    PhysicalZoneEffect result;
    for (const auto& object : worldObjects) {
        if (!missionClassIs(object.className, "PhysicalZone") || !object.physicalActive)
            continue;

        // Mission positions/scales are authored in T2's Z-up frame.  World
        // movement uses Y-up, and the existing mapper volume visualization
        // uses the same axis conversion.
        const Point3F center = Math::torquePointToYUp(object.pos);
        if (!transformedTriggerContains(object.trigger, position, center,
                                        object.scale, object.rot, object.rotAngleDeg))
            continue;

        result.velocityMod *= physicalZoneModifier(object.physicalVelocityMod);
        result.gravityMod *= physicalZoneModifier(object.physicalGravityMod);
        const Point3F force = physicalZoneForceToYUp(
            object.physicalForce, object.rot, object.rotAngleDeg);
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
                                 const Point3F& impactNormal, int tick, bool endedWithDecal) {
    static constexpr size_t maxEffectInstances = 4096;
    if (!projectileData || !dataBlocks) return;
    const auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks->find(id);
        return it == dataBlocks->end() ? nullptr : &it->second.decoded;
    };
    if (endedWithDecal) for (uint32_t ref : projectileData->projectileDecalRefs) {
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
            decal.up = basis.bitangent;
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
    uint32_t selectedExplosionId = projectileData->projectileExplosionRef;
    if (projectileData->projectileUnderwaterExplosionRef != 0) {
        float highestWaterLevel = -std::numeric_limits<float>::infinity();
        for (const auto& body : waterBodies) {
            if (!waterBodyCanAffectSurface(body.active, body.liquidType)) continue;
            if (!waterBodyContainsHorizontal(pos.x, pos.z, body.originX,
                                             body.originZ, body.sizeX, body.sizeY))
                continue;
            if (body.level > highestWaterLevel &&
                body.level - pos.y >= projectileData->projectileDepthTolerance) {
                selectedExplosion = find(projectileData->projectileUnderwaterExplosionRef);
                selectedExplosionId = projectileData->projectileUnderwaterExplosionRef;
                highestWaterLevel = body.level;
            }
        }
    }
    if (!selectedExplosion || !selectedExplosion->hasExplosion) return;
    spawnExplosionData(selectedExplosion, selectedExplosionId, pos, *dataBlocks, impactNormal, tick);
}

void World::spawnExplosionData(const V12::DecodedDataBlock* explosion, uint32_t explosionId,
                               const Point3F& pos, const std::map<uint32_t, ParsedDataBlock>& dataBlocks,
                               const Point3F& impactNormal, int tick, int depth) {
    static constexpr size_t maxEffectInstances = 4096;
    if (!explosion || !explosion->hasExplosion) return;
    const auto find = [&](uint32_t id) -> const V12::DecodedDataBlock* {
        auto it = dataBlocks.find(id);
        return it == dataBlocks.end() ? nullptr : &it->second.decoded;
    };
    const auto addEmitter = [&](uint32_t emitterId, bool burst, int burstCount,
                                float effectLifetime, float effectDelay, const Point3F& origin,
                                TimelineRandom& random, float radialRadius = -1.0f) {
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
        emitter.radialRadius = radialRadius;
        emitter.radialNormal = impactNormal;
        emitter.delay = effectDelay;
        emitter.age = -effectDelay;
        if (effectLifetime > 0.0f) {
            emitter.lifetime = effectLifetime;
        } else {
            const float emitterLifetimeMS = (float)emitter.emitter.lifetimeMS +
                (float)random.intInclusive(emitter.emitter.lifetimeVarianceMS);
            emitter.lifetime = std::max(0.0f, emitterLifetimeMS / 1000.0f);
        }
        emitter.nextEmission = burst ? -1.0f : 0.0f;
        emitter.particles.reserve((size_t)std::min(burstCount, 512));
        if (effectEmitters.size() < maxEffectInstances)
            effectEmitters.push_back(std::move(emitter));
    };
    std::vector<const V12::DecodedDataBlock*> explosionStack;
    // Explosion::onAdd at `addTick`, `startDelay` seconds after this call
    // (a sub-explosion's onAdd runs when its parent explodes).
    std::function<void(const V12::DecodedDataBlock*, uint32_t, int, const Point3F&, int, float)> spawnGraph;
    spawnGraph = [&](const V12::DecodedDataBlock* block, uint32_t blockId, int depth, const Point3F& origin,
                     int addTick, float startDelay) {
        if (!block || !block->hasExplosion || depth > 4) return;
        if (std::find(explosionStack.begin(), explosionStack.end(), block) != explosionStack.end()) return;
        explosionStack.push_back(block);
        const auto& effect = block->explosion;
        const Point3F effectOrigin = origin;
        const Point3F torqueOrigin{origin.x, -origin.z, origin.y};
        const int density = std::clamp(effect.particleDensity, 0, 512);
        // The explosion's own shape and its "ambient" sequence.
        int shapeIndex = -1;
        if (!effect.shape.empty()) {
            const std::string path = normalizeShapePath(effect.shape);
            if (auto cached = explosionShapeIndex.find(path); cached != explosionShapeIndex.end()) {
                shapeIndex = cached->second;
            } else {
                auto shapeData = Engine::instance().fs().read(path.c_str());
                DTSShape shape;
                shape.name = path;
                if (!shapeData.empty() && shape.load(shapeData.data(), shapeData.size())) {
                    shapeIndex = (int)explosionShapes.size();
                    explosionShapes.push_back(std::move(shape));
                }
                explosionShapeIndex[path] = shapeIndex;
            }
        }
        int ambientIndex = -1;
        float ambientDuration = 0.0f;
        if (shapeIndex >= 0) {
            const auto& anims = explosionShapes[shapeIndex].animations;
            for (size_t i = 0; i < anims.size(); ++i) {
                if (missionLower(anims[i].name) != "ambient") continue;
                ambientIndex = (int)i;
                ambientDuration = anims[i].duration;
                break;
            }
        }
        // Explosion::onAdd: delay and lifetime +- randI() % (2v + 1) - v;
        // explode() replaces the lifetime with the ambient duration /
        // |playSpeed|. Both count from onAdd; processTick explodes once the
        // delay has passed and deletes at the lifetime, so an explosion whose
        // lifetime ends while it is still waiting is never seen.
        TimelineRandom random = timelineRandom({(double)blockId, (double)addTick, torqueOrigin.x,
                                                torqueOrigin.y, torqueOrigin.z});
        const int delayMS = std::max(0, effect.delayMS + random.intInclusive(effect.delayVarianceMS));
        const int armedLifetimeMS = effect.lifetimeMS + random.intInclusive(effect.lifetimeVarianceMS);
        const int lifetimeMS = ambientDuration > 0.0f && effect.playSpeed != 0.0f
            ? (int)std::lround(ambientDuration / std::fabs(effect.playSpeed) * 1000.0f) : armedLifetimeMS;
        const int explodeTicks = delayMS > 0 ? delayMS / 32 + 1 : 0;
        const int armedLifetimeTicks = std::max(1, (armedLifetimeMS + 31) / 32);
        if (explodeTicks > 0 && explodeTicks >= armedLifetimeTicks) {
            explosionStack.pop_back();
            return;
        }
        const float explodeAt = (float)explodeTicks * 0.032f;       // after onAdd
        const float lifetime = std::max(0.0f, (float)lifetimeMS / 1000.0f);  // from onAdd
        const float effectDelay = startDelay + explodeAt;
        // Emitters are fed for as long as the explosion lives after exploding.
        const float effectLifetime = std::max(0.0f, lifetime - explodeAt);
        // Retail ExplosionData sends only a hasLight flag; its light colors
        // and radii are not networked and default to a zero radius, so a
        // client explosion casts no light.
        if (shapeIndex >= 0 && effectExplosionShapes.size() < maxEffectInstances) {
            EffectExplosionShape instance;
            instance.pos = effectOrigin;
            instance.shapeIndex = shapeIndex;
            // The instance clock runs from its own onAdd.
            instance.age = -startDelay;
            instance.delay = explodeAt;
            instance.playSpeed = effect.playSpeed;
            instance.faceViewer = effect.faceViewer;
            instance.times = effect.times;
            instance.sizes = effect.sizes;
            instance.lifetime = lifetime;
            instance.ambientIndex = ambientIndex;
            instance.roll = random() * 2.0f * Math::PI;
            effectExplosionShapes.push_back(std::move(instance));
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
            // Explosion::launchDebris: debrisNum +- debrisNumVariance pieces
            // from 0.5 above the explosion, each along randomDir(normal,
            // theta, phi) at debrisVelocity +- debrisVelocityVariance.
            const auto* debrisBlock = find(effect.debrisRef);
            if (debrisBlock && debrisBlock->hasDebris) {
                const auto& data = debrisBlock->debris;
                const int debrisCount = std::max(0, effect.debrisNum + random.intInclusive(effect.debrisNumVariance));
                const Point3F launchPos{effectOrigin.x, effectOrigin.y + 0.5f, effectOrigin.z};
                const int shapeIndex = debrisShapeFor(data.shape);
                for (int debrisIndex = 0; debrisIndex < debrisCount &&
                     effectDebris.size() < maxEffectInstances; ++debrisIndex) {
                    const Point3F dir = DebrisPhysics::randomDir(impactNormal,
                        (float)effect.debrisThetaMin, (float)effect.debrisThetaMax,
                        (float)effect.debrisPhiMin, (float)effect.debrisPhiMax, random);
                    const float speed = effect.debrisVelocity +
                        (random() * 2.0f - 1.0f) * effect.debrisVelocityVariance;
                    launchDebris(data, launchPos, {dir.x * speed, dir.y * speed, dir.z * speed}, shapeIndex,
                                 -1, 0.2f, MatrixF{}, effectDelay, depth, dataBlocks, random);
                }
            }
        }
        if (effect.particleEmitterRef)
            // Explosion::explode: particleDensity particles within
            // particleRadius about the impact normal.
            addEmitter(effect.particleEmitterRef, true, density, effectLifetime, effectDelay, effectOrigin, random,
                       std::max(0.0f, effect.particleRadius));
        for (uint32_t ref : effect.emitterRefs)
            if (ref) addEmitter(ref, false, 0, effectLifetime, effectDelay, effectOrigin, random);
        if (effect.shockwaveRef) {
            const auto* shockwaveBlock = find(effect.shockwaveRef);
            if (shockwaveBlock && shockwaveBlock->hasShockwave) {
                EffectShockwave shockwave;
                shockwave.pos = effectOrigin;
                shockwave.data = shockwaveBlock->shockwave;
                const float delay = (float)shockwave.data.delayMS +
                    (float)random.intInclusive(shockwave.data.delayVariance);
                shockwave.age = -(effectDelay + std::max(0.0f, delay / 1000.0f));
                shockwave.velocity = shockwave.data.velocity;
                const float shockLifetime = (float)shockwave.data.lifetimeMS +
                    (float)random.intInclusive(shockwave.data.lifetimeVariance);
                shockwave.lifetime = std::max(0.001f, shockLifetime / 1000.0f);
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
        // Explosion::explode adds the sub-explosions; each one's onAdd
        // scatters it by its `offset` along a random direction in the
        // hemisphere above the impact (the normal taken as up).
        const int explodeTick = addTick + explodeTicks;
        TimelineRandom subRandom = timelineRandom({(double)blockId, (double)explodeTick, torqueOrigin.x,
                                                   torqueOrigin.y, torqueOrigin.z});
        for (uint32_t ref : effect.subExplosionRefs) {
            if (effectEmitters.size() >= maxEffectInstances &&
                effectShockwaves.size() >= maxEffectInstances) break;
            const auto* sub = find(ref);
            if (!sub || !sub->hasExplosion) continue;
            Point3F subOrigin = effectOrigin;
            const float offset = sub->explosion.offset;
            if (std::fabs(offset) > 1e-4f) {
                const float dx = subRandom() * 2.0f - 1.0f, dy = subRandom() * 2.0f - 1.0f, dz = subRandom();
                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len <= 0.0f) len = 1.0f;
                const Point3F shift = Math::torquePointToYUp({dx / len * offset, dy / len * offset, dz / len * offset});
                subOrigin = {subOrigin.x + shift.x, subOrigin.y + shift.y, subOrigin.z + shift.z};
            }
            spawnGraph(sub, ref, depth + 1, subOrigin, explodeTick, effectDelay);
        }
        explosionStack.pop_back();
    };
    spawnGraph(explosion, explosionId, depth, pos, tick, 0.0f);
}

int World::debrisShapeFor(const std::string& shapeName) {
    const std::string path = normalizeShapePath(shapeName);
    if (path.empty()) return -1;
    auto cached = debrisShapeIndex.find(path);
    if (cached == debrisShapeIndex.end()) {
        int index = -1;
        auto shapeData = Engine::instance().fs().read(path.c_str());
        DTSShape shape;
        shape.name = path;
        if (!shapeData.empty() && shape.load(shapeData.data(), shapeData.size())) {
            index = (int)debrisShapes.size();
            debrisShapes.push_back(std::move(shape));
        } else {
            Console::instance().printf(LogLevel::Warn, "Debris: unable to load shape '%s'", path.c_str());
        }
        cached = debrisShapeIndex.emplace(path, index).first;
    }
    return cached->second;
}

void World::launchDebris(const V12::DecodedDataBlock::DebrisData& data, const Point3F& pos, const Point3F& vel,
                         int shapeIndex, int partObject, float partRadius, const MatrixF& orientation,
                         float delay, int depth, const std::map<uint32_t, ParsedDataBlock>& dataBlocks,
                         TimelineRandom& random) {
    EffectDebris debris;
    debris.body = DebrisPhysics::init(data, pos, vel, partRadius, random);
    debris.data = data;
    debris.id = ++nextDebrisId;
    debris.delay = std::max(0.0f, delay);
    debris.shapeIndex = shapeIndex;
    debris.partObject = partObject;
    debris.orientation = orientation;
    debris.depth = depth;
    debris.dataBlocks = &dataBlocks;
    // Debris::onAdd: emitters[0] and [1], with emitter sizes {1, 2, 3} and
    // {0, 1, 2} where the emitter takes its sizes from the instance.
    for (int slot = 0; slot < 2; ++slot) {
        const uint32_t ref = data.emitterRefs[slot];
        auto emitterBlock = ref ? dataBlocks.find(ref) : dataBlocks.end();
        if (emitterBlock == dataBlocks.end() || !emitterBlock->second.decoded.hasEmitter ||
            emitterBlock->second.decoded.emitter.particleRefs.empty()) continue;
        auto particleBlock = dataBlocks.find(emitterBlock->second.decoded.emitter.particleRefs.front());
        if (particleBlock == dataBlocks.end() || !particleBlock->second.decoded.hasParticle) continue;
        EffectEmitter emitter;
        emitter.debrisOwner = debris.id;
        emitter.debrisSlot = slot;
        emitter.pos = pos;
        emitter.emitter = emitterBlock->second.decoded.emitter;
        if (emitter.emitter.useEmitterSizes)
            emitter.emitter.sizes = slot == 0 ? std::vector<float>{1.0f, 2.0f, 3.0f}
                                              : std::vector<float>{0.0f, 1.0f, 2.0f};
        emitter.particle = particleBlock->second.decoded.particle;
        emitter.age = -debris.delay;
        emitter.delay = debris.delay;
        emitter.nextEmission = 0.0f;
        emitter.stopped = debris.delay > 0.0f;
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
    }
    effectDebris.push_back(std::move(debris));
}

void World::blowUpShape(const V12::DecodedDataBlock& shapeData, DTSShape* shape, const MatrixF& renderModel,
                        const Point3F& damageDir, const std::map<uint32_t, ParsedDataBlock>& dataBlocks, int tick) {
    // The world box centre: mObjBox (TSShape::bounds) under the transform.
    Point3F localCenter{0, 0, 0};
    if (shape) {
        localCenter = shape->boundsCenter();
        if (shape->hasHeaderBounds) {
            const Point3F& lo = shape->headerBoundsMin;
            const Point3F& hi = shape->headerBoundsMax;
            localCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        }
    }
    const Point3F center = renderModel.transform(localCenter);
    uint32_t explosionRef = shapeData.shapeExplosionRef;
    if (shapeData.shapeUnderwaterExplosionRef) {
        const float level = waterSurfaceAt(center.x, -center.z);
        if (std::isfinite(level) && level > center.y) explosionRef = shapeData.shapeUnderwaterExplosionRef;
    }
    if (explosionRef) {
        auto explosion = dataBlocks.find(explosionRef);
        if (explosion != dataBlocks.end())
            spawnExplosionData(&explosion->second.decoded, explosionRef, center, dataBlocks, damageDir, tick);
    }
    // TSPartInstance::breakShape of debrisShapeName: one piece per visible
    // object under the first subshape, each launched within 50 degrees of
    // the damage direction. Without a debris datablock the pieces use
    // DebrisData's defaults.
    if (shapeData.debrisShape.empty()) return;
    const int shapeIndex = debrisShapeFor(shapeData.debrisShape);
    if (shapeIndex < 0) return;
    const DTSShape& debrisShape = debrisShapes[shapeIndex];
    V12::DecodedDataBlock::DebrisData data;
    if (shapeData.shapeDebrisRef) {
        auto block = dataBlocks.find(shapeData.shapeDebrisRef);
        if (block != dataBlocks.end() && block->second.decoded.hasDebris) data = block->second.decoded.debris;
    }
    // The world rotation without the shape's own up conversion.
    MatrixF orientation = renderModel * (shape ? shape->upOrientation() : MatrixF{}).inverse();
    orientation.setTranslation({0, 0, 0});
    TimelineRandom random = timelineRandom({(double)shapeData.shapeExplosionRef, (double)tick,
                                            center.x, center.y, center.z});
    const int root = debrisShape.subShapeFirstNode.empty() ? -1 : debrisShape.subShapeFirstNode.front();
    const auto& nodeWorld = debrisShape.defaultTransforms;
    for (size_t object = 0; object < debrisShape.objectStartMesh.size(); ++object) {
        if (object < debrisShape.objectDefaults.size() && debrisShape.objectDefaults[object].vis < 0.01f) continue;
        const int first = debrisShape.objectStartMesh[object];
        const int count = object < debrisShape.objectNumMeshes.size() ? debrisShape.objectNumMeshes[object] : 0;
        // The object's highest detail mesh: the first of its meshes drawn by
        // detail 0.
        int meshIndex = -1;
        if (!debrisShape.details.empty())
            for (int32_t mi : debrisShape.details.front().meshIndices)
                if (mi >= first && mi < first + count) { meshIndex = mi; break; }
        if (meshIndex < 0 || meshIndex >= (int)debrisShape.meshes.size()) continue;
        const MeshData& mesh = debrisShape.meshes[meshIndex];
        if (mesh.vertices.empty()) continue;
        int node = mesh.nodeIndex;
        while (node >= 0 && node != root && node < (int)debrisShape.nodes.size())
            node = debrisShape.nodes[node].parentIndex;
        if (node < 0 || node != root) continue;
        const MatrixF toShape = mesh.nodeIndex >= 0 && mesh.nodeIndex < (int)nodeWorld.size()
            ? nodeWorld[mesh.nodeIndex] : MatrixF{};
        Point3F lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& vertex : mesh.vertices) {
            const Point3F p = toShape.transform(vertex.pos);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        // TSPartInstance's radius: half the box diagonal, halved again.
        const float radius = 0.25f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) +
                                               (hi.z - lo.z) * (hi.z - lo.z));
        const Point3F dir = DebrisPhysics::randomDir(damageDir, 0.0f, 50.0f, 0.0f, 360.0f, random);
        if (effectDebris.size() >= 4096) break;
        launchDebris(data, center, dir, shapeIndex, (int)object, radius, orientation, 0.0f, 0, dataBlocks, random);
    }
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
    float highestWaterLevel = -std::numeric_limits<float>::infinity();
    for (const auto& body : waterBodies) {
        if (!waterBodyCanAffectSurface(body.active, body.liquidType)) continue;
        if (waterBodyContainsHorizontal(pos.x, pos.z, body.originX,
                                        body.originZ, body.sizeX, body.sizeY) &&
            body.level > highestWaterLevel) {
            pos.y = body.level;
            onWater = true;
            highestWaterLevel = body.level;
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
    effectExplosionShapes.clear();
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
    for (auto& e : effectEmitters)
        if (e.nodeEmitter && e.trailGeneration != trailGeneration) e.stopped = true;
    effectEmitters.erase(std::remove_if(effectEmitters.begin(), effectEmitters.end(),
        [this](const EffectEmitter& e) {
            return (e.projectileTrail && e.trailGeneration != trailGeneration) ||
                   (e.nodeEmitter && e.stopped && e.particles.empty());
        }),
        effectEmitters.end());
}

void World::syncNodeEmitter(int64_t key, uint32_t emitterRef, const Point3F& pos,
                            const Point3F& velocity, const Point3F& axis,
                            const std::map<uint32_t, ParsedDataBlock>& dataBlocks,
                            float emitScale) {
    auto it = std::find_if(effectEmitters.begin(), effectEmitters.end(),
        [key](const EffectEmitter& e) { return e.nodeEmitter && !e.stopped && e.nodeKey == key; });
    if (it == effectEmitters.end()) {
        auto emitterBlock = dataBlocks.find(emitterRef);
        if (emitterBlock == dataBlocks.end() || !emitterBlock->second.decoded.hasEmitter ||
            emitterBlock->second.decoded.emitter.particleRefs.empty()) return;
        auto particleBlock = dataBlocks.find(emitterBlock->second.decoded.emitter.particleRefs.front());
        if (particleBlock == dataBlocks.end() || !particleBlock->second.decoded.hasParticle) return;
        EffectEmitter emitter;
        emitter.nodeEmitter = true;
        emitter.nodeKey = key;
        emitter.emitter = emitterBlock->second.decoded.emitter;
        emitter.particle = particleBlock->second.decoded.particle;
        emitter.nextEmission = 0.0f;
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
    }
    it->pos = pos;
    it->ownerVelocity = velocity;
    it->emitScale = emitScale;
    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    it->axis = length > 1e-4f ? Point3F{axis.x / length, axis.y / length, axis.z / length}
                              : Point3F{0, 1, 0};
    it->trailGeneration = trailGeneration;
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
    if (!particleTickIsUsable(dt)) return;
    for (auto& p : particles) {
        if (!p.active) continue;
        p.lifetime = particleLifetimeAfterTick(p.lifetime, dt);
        if (p.lifetime <= 0) { p.active = false; continue; }
        if (!std::isfinite(p.maxLifetime) || p.maxLifetime <= 0.0f) {
            p.active = false;
            continue;
        }
        p.vel.y -= 5.0f * dt; // gravity
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
        p.size += dt * 0.5f; // expand
        float t = p.lifetime / p.maxLifetime;
        p.color.a = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f); // fade out
    }
    // Remove dead particles
    particles.erase(std::remove_if(particles.begin(), particles.end(),
        [](const Particle& p) { return !p.active; }), particles.end());

    for (auto& shape : effectExplosionShapes) {
        shape.age += dt;
        if (shape.age >= shape.lifetime) shape.active = false;
    }
    effectExplosionShapes.erase(std::remove_if(effectExplosionShapes.begin(), effectExplosionShapes.end(),
        [](const EffectExplosionShape& shape) { return !shape.active; }), effectExplosionShapes.end());

    // Debris::processTick in 32 ms ticks: the ray is the static world
    // (terrain and interiors; force fields are not in the mask) plus water
    // unless the datablock ignores it.
    {
        const float gravity = debrisGravityAcceleration(Engine::instance().game().getGravity(), 1.0f);
        struct PendingExplosion { uint32_t ref; Point3F pos; int depth; const std::map<uint32_t, ParsedDataBlock>* blocks; };
        std::vector<PendingExplosion> pending;
        for (auto& debris : effectDebris) {
            if (!debris.active) continue;
            float advance = dt;
            if (debris.delay > 0.0f) {
                debris.delay -= dt;
                if (debris.delay > 0.0f) continue;
                advance = -debris.delay;
                debris.delay = 0.0f;
            }
            const bool water = !debris.data.ignoreWater;
            const ProjectilePhysics::CastRay cast = [&](const Point3F& a, const Point3F& b,
                                                        ProjectilePhysics::RayHit& hit) {
                bool found = castStaticRay(*this, a, b, hit);
                if (water) {
                    // Crossing a liquid surface downward.
                    const float level = waterSurfaceAt(b.x, -b.z);
                    if (std::isfinite(level) && a.y >= level && b.y < level && a.y != b.y) {
                        const float t = (a.y - level) / (a.y - b.y);
                        if (!found || t < hit.t) {
                            hit.t = t;
                            hit.point = {a.x + (b.x - a.x) * t, level, a.z + (b.z - a.z) * t};
                            hit.normal = {0, 1, 0};
                            found = true;
                        }
                    }
                }
                return found;
            };
            debris.tickTime += advance;
            while (debris.active && debris.tickTime >= DebrisPhysics::TickSeconds) {
                debris.tickTime -= DebrisPhysics::TickSeconds;
                debris.body.age += DebrisPhysics::TickSeconds;
                if (debris.body.age >= debris.body.lifetime) { debris.active = false; break; }
                if (DebrisPhysics::step(debris.body, debris.data, gravity, cast) ==
                        DebrisPhysics::StepResult::MaxBounce && debris.data.explodeOnMaxBounce) {
                    if (debris.data.explosionRef && debris.dataBlocks)
                        pending.push_back({debris.data.explosionRef, debris.body.pos, debris.depth + 1,
                                           debris.dataBlocks});
                    debris.active = false;
                }
            }
            // Debris::updateEmitters: emit along -velocity from the piece.
            const Point3F& v = debris.body.vel;
            const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            const Point3F axis = speed > 1e-6f ? Point3F{-v.x / speed, -v.y / speed, -v.z / speed} : Point3F{0, 1, 0};
            for (auto& emitter : effectEmitters) {
                if (emitter.debrisOwner != debris.id) continue;
                emitter.pos = debris.body.pos;
                emitter.ownerVelocity = v;
                emitter.axis = axis;
                emitter.stopped = !debris.active;
            }
        }
        effectDebris.erase(std::remove_if(effectDebris.begin(), effectDebris.end(),
            [](const EffectDebris& debris) { return !debris.active; }), effectDebris.end());
        for (const auto& explosion : pending) {
            auto block = explosion.blocks->find(explosion.ref);
            if (block != explosion.blocks->end())
                spawnExplosionData(&block->second.decoded, explosion.ref, explosion.pos, *explosion.blocks,
                                   {0, 1, 0}, 0, explosion.depth);
        }
    }

    auto random01 = []() { return (float)std::rand() / (float)RAND_MAX; };
    for (auto& emitter : effectEmitters) {
        const float previousAge = emitter.age;
        // emitParticles(..., dt * scale): a contrail emits over part of the frame.
        emitter.age += emitter.nodeEmitter ? dt * emitter.emitScale : dt;
        if (emitter.age < 0.0f) continue;
        auto emitOne = [&](float ageOffset, const Point3F& axis, const Point3F& base) {
            EffectParticle p;
            const float theta = Math::DEG2RAD(emitter.emitter.thetaMin + random01() *
                (emitter.emitter.thetaMax - emitter.emitter.thetaMin));
            // phiReferenceVel is angular velocity in degrees/sec. Variance is
            // sampled from [0, variance], as in ParticleSystem.ts.
            const float phi = Math::DEG2RAD(emitter.age * emitter.emitter.phiReferenceVel +
                random01() * emitter.emitter.phiVariance);

            // Start along the emitter axis, then apply theta and phi. The
            // perpendicular basis matches ParticleSystem.ts for arbitrary axes.
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
            p.pos = base;
            p.pos.x += dir.x * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.pos.y += dir.y * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.pos.z += dir.z * (float)emitter.emitter.ejectionOffset / 100.0f;
            p.vel = {dir.x * speed, dir.y * speed, dir.z * speed};
            if (emitter.projectileTrail || emitter.nodeEmitter || emitter.debrisOwner) {
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
            if (emitter.radialRadius >= 0.0f) {
                Point3F az = emitter.radialNormal;
                const float azLen = std::sqrt(az.x * az.x + az.y * az.y + az.z * az.z);
                az = azLen > 1e-6f ? Point3F{az.x / azLen, az.y / azLen, az.z / azLen} : Point3F{0, 1, 0};
                const Point3F ref = std::fabs(az.y) < 0.98f ? Point3F{0, 1, 0} : Point3F{0, 0, -1};
                auto crossN = [](const Point3F& a, const Point3F& b) {
                    Point3F c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
                    const float l = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                    return l > 1e-6f ? Point3F{c.x / l, c.y / l, c.z / l} : Point3F{1, 0, 0};
                };
                const Point3F ay = crossN(az, ref), ax = crossN(az, ay);
                const float r = emitter.radialRadius;
                for (int i = 0; i < emitter.burstCount; ++i) {
                    const float u = r * (1.0f - 2.0f * random01()), v = r * (1.0f - 2.0f * random01());
                    const float w = r * random01();
                    const Point3F offset{ax.x * u + ay.x * v + az.x * w, ax.y * u + ay.y * v + az.y * w,
                                         ax.z * u + ay.z * v + az.z * w};
                    const float l = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
                    const Point3F axis = l > 1e-6f ? Point3F{offset.x / l, offset.y / l, offset.z / l} : az;
                    emitOne(0.0f, axis, {emitter.pos.x + offset.x, emitter.pos.y + offset.y, emitter.pos.z + offset.z});
                }
            } else {
                for (int i = 0; i < emitter.burstCount; ++i) emitOne(0.0f, emitter.axis, emitter.pos);
            }
            emitter.nextEmission = 0.0f;
        } else if (!emitter.burst && !emitter.stopped) {
            // A long frame can cross more than one ejection period. Torque
            // emits each due particle rather than dropping overdue emissions.
            while (emitter.age >= emitter.nextEmission &&
                   (emitter.lifetime <= 0.0f || emitter.nextEmission <= emitter.lifetime)) {
                const float ageOffset = std::max(0.0f, emitter.age - emitter.nextEmission);
                emitOne(ageOffset, emitter.axis, emitter.pos);
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
                std::vector<float> keyTimes;
                keyTimes.reserve(emitter.particle.keys.size());
                for (const auto& key : emitter.particle.keys) keyTimes.push_back(key.time);
                const ParticleKeyInterpolation interpolation =
                    particleKeyInterpolation(t, keyTimes);
                const auto& a = emitter.particle.keys[interpolation.lower];
                const auto& b = emitter.particle.keys[interpolation.upper];
                const float f = interpolation.fraction;
                const auto& colorA = emitter.emitter.useEmitterColors &&
                    emitter.emitter.colors.size() > interpolation.lower
                    ? emitter.emitter.colors[interpolation.lower] : a;
                const auto& colorB = emitter.emitter.useEmitterColors &&
                    emitter.emitter.colors.size() > interpolation.upper
                    ? emitter.emitter.colors[interpolation.upper] : b;
                p.color = {colorA.red + (colorB.red - colorA.red) * f,
                           colorA.green + (colorB.green - colorA.green) * f,
                           colorA.blue + (colorB.blue - colorA.blue) * f,
                           colorA.alpha + (colorB.alpha - colorA.alpha) * f};
                const float sizeA = emitter.emitter.useEmitterSizes &&
                    emitter.emitter.sizes.size() > interpolation.lower
                    ? emitter.emitter.sizes[interpolation.lower] : a.size * 50.0f;
                const float sizeB = emitter.emitter.useEmitterSizes &&
                    emitter.emitter.sizes.size() > interpolation.upper
                    ? emitter.emitter.sizes[interpolation.upper] : b.size * 50.0f;
                p.size = sizeA + (sizeB - sizeA) * f;
            }
        }
        emitter.particles.erase(std::remove_if(emitter.particles.begin(), emitter.particles.end(),
            [](const EffectParticle& p) { return !p.active; }), emitter.particles.end());
    }
    effectEmitters.erase(std::remove_if(effectEmitters.begin(), effectEmitters.end(),
        [this](const EffectEmitter& e) {
            if (e.debrisOwner) {
                if (!e.stopped || !e.particles.empty()) return false;
                // A delayed piece's emitter waits for its launch.
                return std::none_of(effectDebris.begin(), effectDebris.end(),
                    [&](const EffectDebris& d) { return d.id == e.debrisOwner; });
            }
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

// SceneGraph scopes an object into every zone its world box overlaps: it is
// drawn when any of them is visible. Test the box's centre and corners
// (Torque space): a force field's box spans position + rotation x scale, a
// shape's its bounds sphere around its origin.
bool World::isObjectVisible(const WorldObject& obj, const Point3F& cameraPosition) const {
    std::vector<Point3F> points;
    if (obj.forceField) {
        MatrixF rotation;
        if (obj.rotAngleDeg != 0 && (obj.rot.x != 0 || obj.rot.y != 0 || obj.rot.z != 0)) {
            Point3F axis = obj.rot;
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (length > 0.0001f) {
                axis = {axis.x / length, axis.y / length, axis.z / length};
                rotation = Math::torqueRotationToYUp(axis, -Math::DEG2RAD(obj.rotAngleDeg));
            }
        }
        const MatrixF box = rotation * Math::torqueScaleToYUp(obj.scale);
        const Point3F origin = Math::torquePointToYUp(obj.pos);
        for (int i = 0; i < 9; ++i) {
            const Point3F local = i == 8 ? Point3F{0.5f, 0.5f, 0.5f}
                : Point3F{(float)(i & 1), (float)((i >> 1) & 1), (float)((i >> 2) & 1)};
            const Point3F yUp = box.transform(Math::torquePointToYUp(local)); // as the force-field pass places it
            points.push_back({origin.x + yUp.x, -(origin.z + yUp.z), origin.y + yUp.y});
        }
    } else {
        points.push_back(obj.pos);
        const float radius = obj.boundsRadius * std::max({std::fabs(obj.scale.x), std::fabs(obj.scale.y),
                                                          std::fabs(obj.scale.z), 1.0f});
        if (radius > 0.0f)
            for (int i = 0; i < 8; ++i)
                points.push_back({obj.pos.x + ((i & 1) ? radius : -radius),
                                  obj.pos.y + ((i & 2) ? radius : -radius),
                                  obj.pos.z + ((i & 4) ? radius : -radius)});
    }
    for (const Point3F& point : points)
        if (isPositionVisible(point, cameraPosition)) return true;
    return false;
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
        if (debris.delay > 0.0f || debris.shapeIndex < 0 || debris.shapeIndex >= (int)debrisShapes.size() ||
            !debrisShapes[debris.shapeIndex].loaded) continue;
        DTSShape& shape = debrisShapes[debris.shapeIndex];
        // Between the last two ticks; spin X then Z in object space.
        const DebrisPhysics::Body& b = debris.body;
        const float f = std::clamp(debris.tickTime / DebrisPhysics::TickSeconds, 0.0f, 1.0f);
        const Point3F pos{b.prevPos.x + (b.pos.x - b.prevPos.x) * f, b.prevPos.y + (b.pos.y - b.prevPos.y) * f,
                          b.prevPos.z + (b.pos.z - b.prevPos.z) * f};
        const float rotX = b.prevRotX + (b.rotX - b.prevRotX) * f;
        const float rotZ = b.prevRotZ + (b.rotZ - b.prevRotZ) * f;
        MatrixF model = debris.orientation *
            Math::torqueRotationToYUp({0, 0, 1}, Math::DEG2RAD(rotZ)) *
            Math::torqueRotationToYUp({1, 0, 0}, Math::DEG2RAD(rotX));
        model.setTranslation(pos);
        r.setModel(model * shape.upOrientation());
        if (debrisShader) {
            debrisShader->setUniform("uUseTexture", (int32_t)1);
            debrisShader->setUniform("uUseLightmap", (int32_t)0);
        }
        // DebrisData::fade: the last second fades out.
        shape.alphaScale = debris.data.fade ? std::clamp(b.lifetime - b.age, 0.0f, 1.0f) : 1.0f;
        shape.onlyObject = debris.partObject;
        shape.animatedNodeWorld.clear();
        shape.render(DTSShape::SelectDetail);
        shape.onlyObject = -1;
        shape.alphaScale = 1.0f;
    }
    for (auto& instance : effectExplosionShapes) {
        if (instance.age < instance.delay || instance.shapeIndex < 0 ||
            instance.shapeIndex >= (int)explosionShapes.size()) continue;
        DTSShape& shape = explosionShapes[instance.shapeIndex];
        if (!shape.loaded) continue;
        // Size keyframes over the whole lifetime (Explosion::processTick).
        std::array<float, 3> size{1.0f, 1.0f, 1.0f};
        const auto& times = instance.times;
        const auto& sizes = instance.sizes;
        if (!sizes.empty()) {
            const float t = instance.lifetime > 0.0f
                ? std::clamp(instance.age / instance.lifetime, 0.0f, 1.0f) : 1.0f;
            size = sizes.back();
            if (times.size() == sizes.size() && t <= times.front()) size = sizes.front();
            for (size_t i = 0; i + 1 < sizes.size() && i + 1 < times.size(); ++i) {
                if (t >= times[i] && t <= times[i + 1]) {
                    const float span = times[i + 1] - times[i];
                    const float f = span > 0.0f ? (t - times[i]) / span : 0.0f;
                    for (int c = 0; c < 3; ++c)
                        size[c] = sizes[i][c] + (sizes[i + 1][c] - sizes[i][c]) * f;
                    break;
                }
            }
        }
        MatrixF orient;
        orient.identity();
        if (instance.faceViewer) {
            // Point the explosion's +Z at the camera, then roll about it.
            Point3F fwd{r.cameraPos.x - instance.pos.x, r.cameraPos.y - instance.pos.y,
                        r.cameraPos.z - instance.pos.z};
            const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
            if (len > 1e-4f) {
                fwd = {fwd.x / len, fwd.y / len, fwd.z / len};
                Point3F up{0, 1, 0};
                if (std::fabs(fwd.y) > 0.999f) up = {1, 0, 0};
                Point3F right{up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z,
                              up.x * fwd.y - up.y * fwd.x};
                const float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
                right = {right.x / rl, right.y / rl, right.z / rl};
                up = {fwd.y * right.z - fwd.z * right.y, fwd.z * right.x - fwd.x * right.z,
                      fwd.x * right.y - fwd.y * right.x};
                orient.m[0][0] = right.x; orient.m[0][1] = up.x; orient.m[0][2] = fwd.x;
                orient.m[1][0] = right.y; orient.m[1][1] = up.y; orient.m[1][2] = fwd.y;
                orient.m[2][0] = right.z; orient.m[2][1] = up.z; orient.m[2][2] = fwd.z;
            }
            MatrixF roll;
            roll.setRotationAxis({0, 0, 1}, instance.roll);
            orient = orient * roll;
        }
        MatrixF translate, scale, flip;
        translate.identity();
        translate.setTranslation(instance.pos);
        scale.identity();
        scale.setScale({size[0], size[1], size[2]});
        // Explosion::prepModelView faces the shape opposite to projectiles;
        // the native frame keeps that facing without mirroring the shape.
        flip = Math::nativeDtsFrame();
        r.setModel(translate * orient * scale * flip * shape.upOrientation());
        if (debrisShader) {
            debrisShader->setUniform("uUseTexture", (int32_t)1);
            debrisShader->setUniform("uUseLightmap", (int32_t)0);
        }
        const float elapsed = std::max(0.0f, instance.age - instance.delay);
        if (instance.ambientIndex >= 0)
            shape.renderAnimationIndex(instance.ambientIndex, elapsed * instance.playSpeed,
                                       nullptr, 0, nullptr, 0, DTSShape::SelectDetail);
        else
            shape.render(DTSShape::SelectDetail);
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
        // Decals keep a world-fixed orientation (not toward the camera).
        const Point3F up = decal.up.x != 0.0f || decal.up.y != 0.0f || decal.up.z != 0.0f
            ? decal.up : makeDecalBasis(decal.pos, decal.normal).bitangent;
        r.drawOrientedSpriteRect(decal.pos, decal.sizeX, decal.sizeY, {1, 1, 1, alpha}, decal.normal,
                                 decal.angle, texture, u0, v0, u1, v1, false, &up);
    }
    for (auto& p : particles) {
        if (!p.active) continue;
        r.drawSprite(p.pos, p.size, p.color);
    }
    for (const auto& emitter : effectEmitters) {
        for (const auto& p : emitter.particles) {
            if (p.active) {
                // Oriented particles lie along their direction; billboards
                // spin (spinSpeed is degrees per second).
                if (emitter.emitter.orientParticles)
                    r.drawVelocitySprite(p.pos, p.size, p.color, p.orientDir, p.texture, p.additive);
                else
                    r.drawSprite(p.pos, p.size, p.color, p.texture, p.additive, p.spin * Math::PI / 180.0f);
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
    const int dropCount = std::clamp(state.numDrops, 0, kMaxPrecipitationDrops);
    precipitation.drops.resize((size_t)dropCount);
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
        d.cell = std::min(15, (int)(nextRandom() * 16.0f));
        d.active = true;
    }
}

bool World::setPrecipitation(int type, float percentage) {
    if (type < 0 || type > 7 || !std::isfinite(percentage) || percentage < 0.0f || percentage > 1.0f)
        return false;
    precipitation.type = type;
    if (percentage > 0.0f)
        precipitation.configuredPercentage = percentage;
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
    return setPrecipitation(precipitation.type,
                             enabled ? precipitation.configuredPercentage : 0.0f);
}

bool World::setPrecipitationType(int type) {
    return setPrecipitation(type, precipitation.percentage);
}

bool World::setPrecipitationWind(const Point3F& velocity) {
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) return false;
    const Point3F oldWind = getTorchWindVelocity();
    setTorchWindVelocity(velocity);
    if (precipitation.active && precipitation.useWind)
        for (auto& drop : precipitation.drops)
            drop.vel = precipitationWindAdjustedVelocity(drop.vel, oldWind, velocity);
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
    dt = precipitationDelta(dt);
    if (dt <= 0.0f) return;
    precipitation.textureAge += dt;

    float boxY = precipitation.origin.y + precipitation.boxHeight;

    for (auto& d : precipitation.drops) {
        if (!d.active) continue;
        // Wind advects drops in all three axes; only moving Y makes weather
        // appear vertical even though the authored wind is replicated locally.
        d.pos = precipitationAdvancedPosition(d.pos, d.vel, dt);
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
            d.pos.x = precipitationRecenterCoordinate(
                d.pos.x, camPos.x, precipitation.boxWidth);
            d.pos.z = precipitationRecenterCoordinate(
                d.pos.z, camPos.z, precipitation.boxWidth);
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
    if (texture && precipitation.sizeX > 0.0f && precipitation.sizeY > 0.0f) {
        // Precipitation::renderObject: camera-facing sizeX x sizeY quads, each
        // drop using one cell of the 4x4 drop atlas, alpha blended.
        const MatrixF& view = r.viewMatrix();
        const Point3F right{view.m[0][0], view.m[0][1], view.m[0][2]};
        const Point3F up{view.m[1][0], view.m[1][1], view.m[1][2]};
        const float hx = precipitation.sizeX * 0.5f, hy = precipitation.sizeY * 0.5f;
        for (auto& d : precipitation.drops) {
            if (!d.active) continue;
            const Point3F rx{right.x * hx, right.y * hx, right.z * hx};
            const Point3F uy{up.x * hy, up.y * hy, up.z * hy};
            const Point3F a{d.pos.x - rx.x - uy.x, d.pos.y - rx.y - uy.y, d.pos.z - rx.z - uy.z};
            const Point3F b{d.pos.x + rx.x - uy.x, d.pos.y + rx.y - uy.y, d.pos.z + rx.z - uy.z};
            const Point3F c{d.pos.x + rx.x + uy.x, d.pos.y + rx.y + uy.y, d.pos.z + rx.z + uy.z};
            const Point3F e{d.pos.x - rx.x + uy.x, d.pos.y - rx.y + uy.y, d.pos.z - rx.z + uy.z};
            const float u0 = (float)(d.cell % 4) * 0.25f, v0 = (float)(d.cell / 4) * 0.25f;
            r.drawTexturedQuad(a, b, c, e, texture, dropColor, u0, v0 + 0.25f, u0 + 0.25f, v0, false);
        }
        return;
    }
    // Without a drop material the engine has nothing to draw.
    if (!texture) return;
    for (auto& d : precipitation.drops) {
        if (!d.active) continue;
        r.drawSprite(d.pos, precipitation.dropSize, dropColor, texture);
    }
}

void World::renderWater() {
    if (waterBodies.empty()) return;

    auto& r = Engine::instance().renderer();
    // fluid::m_Seconds runs on the platform's virtual clock.
    const float seconds = Engine::instance().game().gameTime();
    const Point3F cam = r.cameraPos;
    // The eye in fluid space: Torque XY + 1024.
    const Point3F eye{cam.x + 1024.0f, -cam.z + 1024.0f, cam.y};

    auto* waterShdr = ShaderManager::getWaterShader();
    if (!waterShdr) return;
    waterShdr->bind();
    waterShdr->setUniform("uProjection", r.projection);
    waterShdr->setUniform("uView", r.view);
    waterShdr->setUniform("uCamPos", cam);
    waterShdr->setUniform("uSeconds", seconds);
    waterShdr->setUniform("uEye", eye);
    for (int i = 0; i < 3; ++i) {
        ColorF packed{};
        if (i < (int)fogVolumes.size() && fogVolumes[i].visibleDistance > 0.0f) {
            const auto& volume = fogVolumes[i];
            packed = {1.0f / volume.visibleDistance, volume.minHeight,
                      volume.maxHeight, volume.percentage};
        }
        waterShdr->setUniform((std::string("uFogVolume") + std::to_string(i)).c_str(), packed);
    }
    {
        float rowBase = 0.0f, rowStep = 0.0f;
        if (terrain() && terrain()->loaded) {
            float lo, hi;
            terrain()->heightRange(lo, hi);
            rowStep = std::max(0.0f, (hi - lo) / 64.0f);
            rowBase = lo + rowStep * 0.5f;
        }
        waterShdr->setUniform("uFogRowBase", rowBase);
        waterShdr->setUniform("uFogRowStep", rowStep);
    }
    const bool renderFog = fog.enabled && !Engine::instance().game().isMapperMode();
    waterShdr->setUniform("uFogEnabled", (int32_t)(renderFog ? 1 : 0));
    if (renderFog) {
        waterShdr->setUniform("uFogColor", Point3F{fog.color.r, fog.color.g, fog.color.b});
        waterShdr->setUniform("uFogStart", fog.distance);
        waterShdr->setUniform("uFogEnd", visibleDistance);
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
    // The shader composites the four fluid passes: out = color + dst * keep.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); // fluidRender.cc

    for (auto& body : waterBodies) {
        // WaterBlock also represents non-liquid volumes (for example fog or
        // damage regions).  They affect neither the native water surface nor
        // its rendered material.
        if (!waterBodyCanAffectSurface(body.active, body.liquidType)) continue;
        if (body.surfaceFrames.empty()) continue;
        const FluidInfo info = fluidSetInfo(body.fluidX0, body.fluidY0, body.sizeX, body.sizeY);
        if (!body.fluidBuilt) {
            // The accepted blocks' 5x5 vertices, in fluid space (one rep).
            const float* heights = terrainBlock.loaded && terrainBlock.heights.size() >= 256 * 256
                ? terrainBlock.heights.data() : nullptr;
            const auto accept = fluidAcceptMask(info, body.level, body.waveMagnitude,
                                                body.removeWetEdges, heights);
            std::vector<float> vertices;
            const float step = info.step4 / 4.0f;
            for (int by = 0; by < info.blocksY; ++by)
                for (int bx = 0; bx < info.blocksX; ++bx) {
                    if (!accept[(size_t)by * info.blocksX + bx]) continue;
                    const float x0 = info.squareX0 * 8.0f + bx * info.step4;
                    const float y0 = info.squareY0 * 8.0f + by * info.step4;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const float ax = x0 + x * step, ay = y0 + y * step;
                            const float quad[6][2] = {{ax, ay}, {ax + step, ay}, {ax + step, ay + step},
                                                      {ax, ay}, {ax + step, ay + step}, {ax, ay + step}};
                            for (const auto& v : quad) { vertices.push_back(v[0]); vertices.push_back(v[1]); }
                        }
                }
            body.fluidVertexCount = (int)(vertices.size() / 2);
            if (body.fluidVertexCount > 0) {
                glGenVertexArrays(1, &body.fluidVao);
                glGenBuffers(1, &body.fluidVbo);
                glBindVertexArray(body.fluidVao);
                glBindBuffer(GL_ARRAY_BUFFER, body.fluidVbo);
                glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                glBindVertexArray(0);
            }
            body.fluidBuilt = true;
            body.fluidHighRes = info.highRes;
            body.fluidStep4 = info.step4;
        }
        if (body.fluidVertexCount <= 0) continue;

        // fluid::SetInfo clamps opacity and environment intensity to [0, 1].
        const float opacity = std::clamp(body.opacity, 0.0f, 1.0f);
        const float waveFactor = body.waveMagnitude * 0.25f;
        const float surfaceAtEye = body.level +
            (std::sin(eye.x * 0.05f + seconds) + std::sin(eye.y * 0.05f + seconds)) * waveFactor;
        waterShdr->setUniform("uSurfaceZ", body.level);
        waterShdr->setUniform("uWaveFactor", waveFactor);
        waterShdr->setUniform("uOpacity", opacity);
        waterShdr->setUniform("uSurfaceAtEye", surfaceAtEye);
        waterShdr->setUniform("uStep1", body.fluidStep4 / 4.0f);
        waterShdr->setUniform("uStep2", body.fluidStep4 / 2.0f);
        const size_t frame = textureFrameIndex(body.surfaceFrameDurations, body.surfaceFrames.size(), seconds);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, body.surfaceFrames[frame]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        waterShdr->setUniform("uBaseTexture", (int32_t)0);
        waterShdr->setUniform("uUseEnvMap", (int32_t)(body.envFrames.empty() ? 0 : 1));
        if (!body.envFrames.empty()) {
            const size_t envFrame = textureFrameIndex(body.envFrameDurations, body.envFrames.size(), seconds);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, body.envFrames[envFrame]);
            waterShdr->setUniform("uEnvMap", (int32_t)1);
            waterShdr->setUniform("uEnvIntensity", std::clamp(body.envIntensity, 0.0f, 1.0f));
            glActiveTexture(GL_TEXTURE0);
        }
        // fluid::RunQuadTree: the nine reps around the eye's rep.
        const int repI = fluidRepIndex(eye.x), repJ = fluidRepIndex(eye.y);
        glBindVertexArray(body.fluidVao);
        for (int j = repJ - 1; j <= repJ + 1; ++j)
            for (int i = repI - 1; i <= repI + 1; ++i) {
                waterShdr->setUniform("uRepOffset", Point2F{i * 2048.0f, j * 2048.0f});
                glDrawArrays(GL_TRIANGLES, 0, body.fluidVertexCount);
            }
        glBindVertexArray(0);
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
Game::~Game() {
    clearVoicePlaybacks();
    clearProjectileAudio();
    if (pl) { pl->modelShape.destroy(); pl->weaponShape.destroy(); }
    testShape.destroy();
    shapeViewerShape.destroy();
    for (auto& [key, shape] : demoShapeCache) shape.destroy();
    delete pl; delete w; delete hud;
}

void Game::clearProjectileAudio() {
    auto& audio = Engine::instance().audio();
    for (auto& [index, source] : projectileSoundSources) audio.releaseSource(source);
    projectileSoundSources.clear();
}

void Game::clearVoicePlaybacks() {
    auto& audio = Engine::instance().audio();
    for (auto& [key, playback] : voicePlaybacks)
        if (playback && playback->source) audio.releasePcmStream(playback->source);
    voicePlaybacks.clear();
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

    con.addCommand("watchServer", [this](int32_t argc, const char* const* argv) {
        std::string host;
        uint16_t port = 0;
        if (argc < 2 || !parseConsoleHostPort(argv[1], host, port)) {
            Console::instance().printf(LogLevel::Warn, "Usage: watchServer <host:port>");
            return;
        }
        connectToServer(host.c_str(), port);
    }, "watchServer <host:port> - connect as an anonymous observer");

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
    con.addCommand("pauseDemo", [this](int32_t, const char* const*) { pauseDemo(); },
        "pauseDemo - Pause demo playback");
    con.addCommand("resumeDemo", [this](int32_t, const char* const*) { resumeDemo(); },
        "resumeDemo - Resume demo playback");
    con.addCommand("toggleDemoPause", [this](int32_t, const char* const*) { toggleDemoPause(); },
        "toggleDemoPause - Toggle demo playback pause");
    con.addCommand("mapperCamera", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) return;
        selectMapperObserverCamera(atoi(argv[1]));
    }, "mapperCamera <n> - select authored observer camera n (mapper mode)");
    con.addCommand("setFreeCamera", [this](int32_t argc, const char* const* argv) {
        if (argc < 7) {
            Console::instance().printf(LogLevel::Warn,
                "Usage: setFreeCamera px py pz tx ty tz (Torch world, Y up; Torque space during demos)");
            return;
        }
        const Point3F pos{(float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3])};
        const Point3F target{(float)atof(argv[4]), (float)atof(argv[5]), (float)atof(argv[6])};
        setFreeCamActive(true);
        setFreeCamPos(pos);
        setFreeCamTarget(target);
    }, "setFreeCamera px py pz tx ty tz - place the free camera (Torch world, Y up; Torque space during demos)");
    con.addCommand("demoObserve", [this](int32_t argc, const char* const* argv) {
        demoObserveGhost = argc > 1 ? atoi(argv[1]) : -1;
        if (argc > 2) demoObserveDistance = (float)atof(argv[2]);
        if (argc > 3) demoObserveHeight = (float)atof(argv[3]);
        if (demoObserveGhost >= 0) { freeCamActive = false; demoOrbitCam = false; demoFirstPersonCam = false; }
    }, "demoObserve ghost [distance] [height] - chase camera behind a demo ghost (-1 turns it off)");
    con.addCommand("demoSpectate", [this](int32_t argc, const char* const* argv) {
        if (argc > 1) selectSpectateTarget(atoi(argv[1]));
    }, "demoSpectate ghost - first-person view from a demo ghost");
    con.addCommand("toggleDemoOrbit", [this](int32_t, const char* const*) {
        demoFirstPersonCam = false;
        demoOrbitCam = !demoOrbitCam;
        freeCamActive = false;
    }, "toggleDemoOrbit - Toggle the demo orbit camera (F2)");
    con.addCommand("stepDemo", [this](int32_t argc, const char* const* argv) {
        const int blocks = argc > 1 ? std::max(1, atoi(argv[1])) : 1;
        requestDemoStep(blocks);
    }, "stepDemo [blocks] - Process demo blocks while paused");
    con.addCommand("setDemoSpeed", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) {
            Console::instance().printf(LogLevel::Warn, "Usage: setDemoSpeed <0.1..8>");
            return;
        }
        setDemoPlaybackSpeed((float)atof(argv[1]));
    }, "setDemoSpeed <rate> - Set demo playback speed");
    con.addCommand("resetDemoEvents", [this](int32_t, const char* const*) { resetDemoEvents(); },
        "resetDemoEvents - Clear the demo event log");
    con.addCommand("resetDemoHud", [this](int32_t, const char* const*) { resetDemoHud(); },
        "resetDemoHud - Clear demo HUD state");
    con.addCommand("resetDemoCamera", [this](int32_t, const char* const*) { resetDemoCamera(); },
        "resetDemoCamera - Return demo camera control to the recording");
    con.addCommand("resetDemoEffects", [this](int32_t, const char* const*) { resetDemoEffects(); },
        "resetDemoEffects - Clear demo audio, trails, flashes, and effects");
    con.addCommand("resetDemo", [this](int32_t, const char* const*) { resetDemoPresentation(); },
        "resetDemo - Reset demo events, HUD, camera, and effects");
    con.addCommand("seekDemoBlock", [this](int32_t argc, const char* const* argv) {
        if (!demoParser || argc < 2) return;
        const int target = std::max(0, atoi(argv[1]));
        // Clear presentation state before restoring parser state. Resetting
        // after restore would erase the HUD/timeline state captured in the
        // selected snapshot.
        resetDemoPresentation();
        if (demoParser) demoParser->consumeExplosions();
        auto snapshot = demoSnapshots.upper_bound(target);
        if (snapshot != demoSnapshots.begin()) {
            --snapshot;
            if (!demoParser->restoreSnapshot(snapshot->second)) {
                Console::instance().printf(LogLevel::Warn, "seekDemoBlock: snapshot restore failed");
                return;
            }
            auto view = demoViewSnapshots.find(snapshot->first);
            if (view != demoViewSnapshots.end()) restoreDemoView(view->second);
        } else {
            demoParser->reset();
        }
        // Replay to the target the way playback reads blocks, so the
        // recorder's control object, view and camera are those of the
        // target block rather than of the snapshot.
        if (target < demoParser->getBlockCursor()) {
            demoParser->reset();
            resetDemoCamera();
        }
        while (demoParser->getBlockCursor() < target) {
            std::unique_ptr<DemoBlock> block(demoParser->nextBlock());
            if (!block) {
                Console::instance().printf(LogLevel::Warn,
                    "seekDemoBlock: unable to seek to block %d", target);
                return;
            }
            applyDemoMoveView(*block);
            applyDemoInfoBlock(*block);
            if (block->type == T2Demo::BlockTypeSendPacket) {
                demoParser->onSendPacketTrigger();
            } else if (block->type == T2Demo::BlockTypePacket) {
                applyDemoPacketView(demoParser->parsePacket(block->data.data(), block->data.size(), block->index));
            }
        }
        demoParser->setCurrentBlock(target);
        demoParser->consumeExplosions();
        // resetDemoPresentation cleared the effects: re-add the scene's.
        applyDemoSceneEffects();
        demoBlocksDone = target;
        demoTime = T2Demo::playbackBlockTime(target, demoParser->getMoveTicksBefore());
        // A seek may cross a mission change in either direction.
        if (!T2Demo::MissionReplacementState::sameMission(
                demoParser->currentMission(), demoMissionState.loadedMission))
            tryLoadDemoMission(demoParser->currentMission(), false);
        demoInterpolationDt = 0.0f;
        demoMoveBlend = 1.0f;
        demoPathCount = 0;
        Console::instance().printf(LogLevel::Info,
            "Demo seeked to block %d", target);
    }, "seekDemoBlock <index> - seek parser state to a demo block");
    con.addCommand("seekDemoTime", [this](int32_t argc, const char* const* argv) {
        if (!demoParser || argc < 2) return;
        // The block that completes the target 32 ms move tick.
        const int block = T2Demo::playbackTargetBlock((float)std::max(0.0, atof(argv[1])),
                                                      demoParser->getMoveTicksBefore());
        Console::instance().execute(("seekDemoBlock(" + std::to_string(block) + ")").c_str());
    }, "seekDemoTime <seconds> - seek to a demo time");

    con.addCommand("listdemos", [this](int32_t argc, const char* const* argv) {
        auto& fs = Engine::instance().fs();
        std::vector<std::string> files;
        // Tribes 2 stores recorded demos as .rec files.  Looking for the
        // obsolete .demo suffix made the native recordings directory appear
        // empty even though playdemo could open those files.
        fs.listFiles("*.rec", files);
        if (files.empty()) {
            Console::instance().printf(LogLevel::Info, "No demo files found (*.rec)");
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
        testShape.destroy();
        testShape = DTSShape{};
        testShape.name = argv[1];
        if (!testShape.load(data.data(), data.size())) {
            Console::instance().printf(LogLevel::Warn, "testshape: failed to load shape");
            testShape.destroy();
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
    if (activeConn) {
        activeConn->disconnect();
        Engine::instance().network().destroyConnection(activeConn);
        activeConn = nullptr;
    }
    if (w) w->cleanupMission();
    if (pl) { pl->modelShape.destroy(); pl->weaponShape.destroy(); }
    testShape.destroy();
    shapeViewerShape.destroy();
    for (auto& [key, shape] : demoShapeCache) shape.destroy();
    demoShapeCache.clear();
    delete mMenu;
    mMenu = nullptr;
    clearMissionAudio();
}

void Game::clearMissionAudio() {
    // Mission teardown: shapes are reloaded, so cached hulls go too.
    staticShapeHulls.clear();
    auto& audio = Engine::instance().audio();
    for (auto& [key, source] : shapeBaseSoundSources)
        if (source) audio.releaseSource(source);
    shapeBaseSoundSources.clear();
    for (auto& [ghost, source] : demoJetSoundSources) audio.releaseSource(source);
    demoJetSoundSources.clear();
    audio.setUnderwater(false);
    audio.clearEnvironmentState();
    for (auto* source : emitterSources) audio.releaseSource(source);
    emitterSources.clear();
}

static SoundSource* playNativeAudioProfile(AudioSystem& audio,
    const std::map<uint32_t, ParsedDataBlock>& blocks, uint32_t profileId,
    const Point3F& position, bool forceLoop = false, bool persistent = false) {
    if (!profileId || !audio.config().enabled || audio.config().sfxVolume <= 0.0f)
        return nullptr;
    auto it = blocks.find(profileId);
    if (it == blocks.end() || it->second.decoded.audioFilename.empty()) return nullptr;
    const auto& profile = it->second.decoded;
    std::string path = profile.audioFilename;
    if (path.rfind("audio/", 0) != 0) path = "audio/" + path;
    auto* buffer = audio.loadSound(path.c_str());
    auto* source = buffer ? audio.createSource(persistent) : nullptr;
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

bool Game::startVoiceCapture(bool local) {
    stopVoiceCapture();
    auto& audio = Engine::instance().audio();
    if (!voiceCapture.encoder.reset() || !audio.initCapture(8000, 1024)) return false;
    if (!audio.startCapture()) {
        audio.destroyCapture();
        return false;
    }
    voiceCapture.local = local;
    voiceCapture.stream = (uint8_t)((voiceCapture.stream + 1) & 3);
    voiceCapture.sequence = 0;
    voiceCapture.pcm.clear();
    voiceCapture.samplesCaptured = 0;
    return true;
}

void Game::stopVoiceCapture() {
    auto& audio = Engine::instance().audio();
    if (!audio.isCapturing()) return;
    std::array<int16_t, 2048> samples{};
    while (const size_t count = audio.captureSamples(samples.data(), samples.size())) {
        const float gain = audio.captureGainScale();
        const size_t accepted = VoiceStream::acceptedCaptureSamples(voiceCapture.samplesCaptured, count);
        for (size_t i = 0; i < accepted; ++i) {
            const int value = (int)std::trunc(samples[i] * gain);
            samples[i] = (int16_t)std::clamp(value, -32768, 32767);
        }
        voiceCapture.pcm.insert(voiceCapture.pcm.end(), samples.begin(), samples.begin() + accepted);
        voiceCapture.samplesCaptured += accepted;
    }
    audio.stopCapture();
    if (voiceCapture.local) {
        if (!voiceCapture.pcm.empty())
            audio.playPcm16(voiceCapture.pcm.data(), voiceCapture.pcm.size(), 8000,
                            audio.config().masterVolume);
    } else if (activeConn && activeConn->isConnected()) {
        activeConn->sendVoiceEvent(voiceCapture.sequence++ & 0x7f, 3,
                                   voiceCapture.stream, true, {});
    }
    voiceCapture.pcm.clear();
}

void Game::processVoiceCapture() {
    auto& capture = voiceCapture;
    if (capture.local) return;
    auto* connection = activeConnection();
    if (!connection || !connection->isConnected()) return;
    while (capture.pcm.size() >= TorchGsm::SamplesPerFrame) {
        std::array<uint8_t, TorchGsm::EncodedBytesPerFrame> frame{};
        if (!capture.encoder.encode(capture.pcm.data(), frame)) break;
        connection->sendVoiceEvent(capture.sequence++ & 0x7f, 3, capture.stream,
                                   false, {frame});
        capture.pcm.erase(capture.pcm.begin(),
                          capture.pcm.begin() + TorchGsm::SamplesPerFrame);
    }
}

void Game::update(float dt) {
    auto& audio = Engine::instance().audio();
    audio.advance(dt);
    const auto voiceNow = std::chrono::steady_clock::now();
    for (auto it = voicePlaybacks.begin(); it != voicePlaybacks.end();) {
        auto& playback = *it->second;
        if (!playback.finished && voiceNow - playback.lastReceived > std::chrono::seconds(2)) {
            playback.finished = true;
            if (playback.source) audio.finishPcmStream(playback.source);
        }
        if (playback.finished && (!playback.source || audio.pcmStreamDrained(playback.source))) {
            if (playback.source) audio.releasePcmStream(playback.source);
            it = voicePlaybacks.erase(it);
        } else {
            ++it;
        }
    }
    if (audio.isCapturing()) {
        auto& capture = voiceCapture;
        std::array<int16_t, 2048> samples{};
        while (const size_t count = audio.captureSamples(samples.data(), samples.size())) {
            const float gain = audio.captureGainScale();
            const size_t accepted = VoiceStream::acceptedCaptureSamples(capture.samplesCaptured, count);
            for (size_t i = 0; i < accepted; ++i) {
                const int value = (int)std::trunc(samples[i] * gain);
                samples[i] = (int16_t)std::clamp(value, -32768, 32767);
            }
            voiceCapture.pcm.insert(voiceCapture.pcm.end(), samples.begin(), samples.begin() + accepted);
            capture.samplesCaptured += accepted;
        }
        processVoiceCapture();
        if (capture.samplesCaptured >= VoiceStream::MaxCaptureSamples)
            stopVoiceCapture();
    }
    // Pause is a simulation boundary, not just a presentation flag.  Keep
    // audio/UI processing alive, but do not advance clocks, damage flashes,
    // physics, projectiles, or respawn timers while the local game is paused.
    if (gamePaused) return;
    demoStepFeedbackTime = std::max(0.0f, demoStepFeedbackTime - std::max(0.0f, dt));
    const float simulationDt = GameTime::scaledDelta(dt, timeScale);
    time += demoPlaying ? std::max(0.0f, dt) : simulationDt;
    const float feedbackDt = demoPlaying ? std::max(0.0f, dt) : simulationDt;
    // ShapeBase::processTick: SB::DFDec and SB::WODec (0.007) per 32 ms tick.
    if (damageFlash > 0.0f) damageFlash = std::max(0.0f, damageFlash - feedbackDt * (0.007f / 0.032f));
    if (whiteOut > 0.0f) whiteOut = std::max(0.0f, whiteOut - feedbackDt * (0.007f / 0.032f));

    if (gameState == Playing || LiveMovePolicy::shouldAdvanceLivePlaybackInShell(
            gameState == MenuScreen, demoLive, demoPlaying)) {
        // ─── Demo playback ──────────────────────────────────────
        if (demoPlaying) {
            // A replacement can arrive before its mission asset is mounted.
            // Retry it without touching the currently rendered presentation.
            if (!demoMissionState.pendingMission.empty() && demoParser &&
                demoParser->currentMission() == demoMissionState.pendingMission)
                tryLoadDemoMission(demoMissionState.pendingMission);
            if (demoPaused && !demoStepRequest) {
                demoJetHeld = false;
                demoInterpolationDt = 0.0f;
                return;
            }
            const bool stepDemo = demoStepRequest;
            const int stepBlocks = stepDemo ? std::max(1, demoStepBlocks) : 0;
            // A live connection runs in real time.
            const float playbackRate = demoLive ? 1.0f
                : (demoFastForward || currentInput.jet) ? std::max(demoPlaybackRate, 4.0f) : demoPlaybackRate;
            Engine::instance().audio().setPlaybackRate(playbackRate);
            const std::vector<int>& demoTicks = demoParser->getMoveTicksBefore();
            if (demoLive) {
                // The recording ends at what the connection has delivered.
                demoBlocksTotal = demoParser->getBlockCount();
                demoTotalTime = T2Demo::playbackBlockTime(demoBlocksTotal, demoTicks);
            }
            const float playbackDt = stepDemo
                ? std::max(0.0f, T2Demo::playbackBlockTime(
                      std::min(demoBlocksDone + std::min(stepBlocks, 500), demoBlocksTotal),
                      demoTicks) - demoTime)
                : dt * playbackRate;
            demoInterpolationDt = playbackDt;
            demoTime = std::min(demoTime + playbackDt, demoTotalTime);
            int blocksThisFrame;
            if (demoStepRequest) {
                blocksThisFrame = std::min(stepBlocks, 500);
                demoStepRequest = false;
                demoStepBlocks = 1;
            } else if (!demoLive && (demoFastForward || currentInput.jet)) {
                demoJetHeld = currentInput.jet;
                // Use the same playhead-to-block conversion as normal
                // playback. Integer truncation here used to stall 4x playback
                // whenever a frame represented less than one block.
                const int targetDone = T2Demo::playbackTargetBlock(demoTime, demoTicks);
                blocksThisFrame = targetDone - demoBlocksDone;
            } else {
                demoJetHeld = false;
                // Match real-time: catch up to target position
                int targetDone = T2Demo::playbackTargetBlock(demoTime, demoTicks);
                blocksThisFrame = targetDone - demoBlocksDone;
            }
            // Blocks after the final Move tick belong to no later tick; drain
            // them once the clock reaches the end so playback completes.
            if (!stepDemo && demoTime >= demoTotalTime)
                blocksThisFrame = std::max(blocksThisFrame, 500);
            if (blocksThisFrame < 0) blocksThisFrame = 0;
            if (blocksThisFrame > 500) blocksThisFrame = 500;

            for (int i = 0; i < blocksThisFrame; i++) {
                DemoBlock* block = demoParser->nextBlock();
                if (!block && demoLive) break;
                if (!block) {
                    Console::instance().printf(LogLevel::Info,
                        "Demo playback complete: %d blocks in %.1f seconds",
                        demoBlocksDone, demoTime);
                    stopDemoPlayback();
                    return;
                }
                demoBlocksDone++;
                const float blockTime = T2Demo::playbackBlockTime(demoBlocksDone - 1, demoTicks);
                const std::string previousMission = demoParser->currentMission();
                demoParser->setCurrentBlock(demoBlocksDone - 1);
                const bool missionChanged = demoParser->currentMission() != previousMission;
                const std::string requestedMission = demoParser->currentMission();

                // Extract position data from move blocks
                applyDemoMoveView(*block);
                applyDemoInfoBlock(*block);

                if (block->type == T2Demo::BlockTypeMove && !demoMatchEnded)
                    updateDemoPlayerAnimation(T2Demo::playbackBlockTime(demoBlocksDone, demoTicks));
                if (block->type == T2Demo::BlockTypeMove) tickDemoPlayers(*block);

                // Parse packet blocks (GameState, ghost updates, events)
                if (block->type == T2Demo::BlockTypeSendPacket) {
                    demoParser->onSendPacketTrigger();
                } else if (block->type == T2Demo::BlockTypePacket) {
                    PacketData pd = demoParser->parsePacket(block->data.data(), block->data.size(), demoBlocksDone - 1);
                    if (demoLive) forwardLiveEvents(pd);
                    // Collect chat/server events for the event pane
                    for (size_t eventIndex = 0; eventIndex < pd.events.size(); ++eventIndex) {
                        const auto& ev = pd.events[eventIndex];
                        // Handle audio events
                        if (ev.directAudioProfile && ev.audioProfileId >= 0) {
                            auto& audio = Engine::instance().audio();
                            const uint64_t eventKey = demoAudioEventKey(
                                demoBlocksDone - 1, static_cast<int>(eventIndex), ev);
                            if (!demoAudioEventsPlayed.insert(eventKey).second) continue;
                            // The AudioProfile datablock streamed in the demo
                            // names the sound and its description.
                            const Point3F position = ev.hasAudioPosition
                                ? Math::torquePointToYUp({ev.audioPosition.x, ev.audioPosition.y,
                                                          ev.audioPosition.z})
                                : Point3F{};
                            playNativeAudioProfile(audio, demoParser->getInitialBlock().dataBlocks,
                                                   static_cast<uint32_t>(ev.audioProfileId), position);
                            continue;
                        }
                        // MissionEnd (GameConnection::endMission) ends the match;
                        // the next mission's MsgClientReady drops back in. The
                        // debrief messages only fill DebriefGui and can arrive
                        // during play.
                        if (ev.classId == T2Demo::NetEventClassFirst + 9 && !ev.arguments.empty()) {
                            const std::string& command = ev.arguments[0];
                            const std::string type = ev.arguments.size() > 1 ? ev.arguments[1] : "";
                            if (command == "MissionEnd")
                                setDemoMatchEnded(true);
                            else if (command == "ServerMessage" && type == "MsgClientReady")
                                setDemoMatchEnded(false);
                            if (command != "ServerMessage")
                                demoParser->handleHudRemoteCommand(command, ev.arguments);
                            dispatchClientCommand(ev.rawArguments, ev.taggedArguments);
                        }
                        if (ev.message.empty()) continue;
                        std::string displayText = ev.message;
                        if (ev.classId == T2Demo::NetEventClassFirst + 9 &&
                            !ev.arguments.empty()) {
                            const std::string& command = ev.arguments[0];
                            // clientCmdServerMessage(%type, %msg, ...) and
                            // clientCmdChatMessage(%sender, %voice, %pitch, %msg, ...).
                            if ((command == "ServerMessage" || command == "ChatMessage") &&
                                ev.arguments.size() >= (command == "ServerMessage" ? 3u : 5u)) {
                                const size_t templateIndex = command == "ServerMessage" ? 2 : 4;
                                // The event pane formats the template as sent.
                                const auto& raw = ev.rawArguments.size() == ev.arguments.size()
                                    ? ev.rawArguments : ev.arguments;
                                std::vector<std::string> values(raw.begin() + templateIndex + 1, raw.end());
                                displayText = formatDemoRemoteText(raw[templateIndex], values);
                            } else if (command != "ServerMessage" && command != "ChatMessage") {
                                // HUD-only remote commands are state changes, not chat entries.
                                continue;
                            }
                        }
                        // Score and state messages carry no text.
                        if (displayText.empty()) continue;
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
                    applyDemoPacketView(pd);
                    // GameState is sparse; an omitted effect must not erase the
                    // previous effect before its normal client-side decay.
                    if (pd.gameState.hasDamageFlash)
                        damageFlash = pd.gameState.damageFlash;
                    if (pd.gameState.hasWhiteOut)
                        whiteOut = pd.gameState.whiteOut;

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
                        // The move tick the explosion lands on seeds its cosmetic randomness.
                         w->spawnExplosionEffect(expPos, projectileData, explosionData, &dataBlocks, expNormal,
                                                (int)std::floor(demoTime / 0.032f), exp.endedWithDecal);
                    }
                    for (const auto& blowUp : demoParser->consumeBlowUps()) {
                        const GhostEntry* g = demoParser->getGhostTracker().getGhost(blowUp.ghostIndex);
                        if (!g || !g->hasDatablock || !g->hasRenderModel) continue;
                        const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                        auto block = dataBlocks.find((uint32_t)g->datablockId);
                        if (block == dataBlocks.end()) continue;
                        w->blowUpShape(block->second.decoded, g->shape, g->renderModel,
                                       Math::torquePointToYUp({blowUp.normal.x, blowUp.normal.y, blowUp.normal.z}),
                                       dataBlocks, (int)std::floor(demoTime / 0.032f));
                    }

                }
                // Parse and apply this block's events before replacing the
                // world. A missing mission must not drop its CRC, chat, or
                // ghost updates; a successful replacement then clears the
                // mission-owned presentation transactionally.
                if (missionChanged)
                    tryLoadDemoMission(requestedMission);
                delete block;
                if (demoPlaying && demoParser && (demoBlocksDone % 500) == 0) {
                    demoSnapshots[demoBlocksDone] = demoParser->captureSnapshot();
                    demoViewSnapshots[demoBlocksDone] = captureDemoView();
                }
            }

            if (demoPlaying && demoParser) {
                auto applySlots = [](GuiControl* control, const auto& state, int active,
                                     const auto& bitmaps, const std::string& background,
                                     const std::string& highlight, const std::string& infinite,
                                     const std::vector<int>* order,
                                     const std::map<int, std::string>* names) {
                    if (!control) return;
                    if (control->hudSlots.size() < 32) control->hudSlots.resize(32);
                    for (auto& slot : control->hudSlots) {
                        slot.visible = false;
                        slot.active = false;
                        slot.bitmap.clear();
                    }
                    control->activeHudSlot = -1;
                    size_t displaySlot = 0;
                    auto setSlot = [&](int index, int amount) {
                        if (displaySlot >= control->hudSlots.size()) return;
                        auto& hudSlot = control->hudSlots[displaySlot];
                        hudSlot.amount = amount;
                        auto bitmap = bitmaps.find(index);
                        if (bitmap != bitmaps.end()) hudSlot.bitmap = bitmap->second;
                        if (names) {
                            auto name = names->find(index);
                            hudSlot.name = name != names->end() ? name->second : std::string();
                        }
                        hudSlot.visible = true;
                        hudSlot.active = index == active;
                        if (hudSlot.active) control->activeHudSlot = (int)displaySlot;
                        ++displaySlot;
                    };
                    std::unordered_set<int> assigned;
                    if (order) {
                        for (int index : *order) {
                            auto slot = state.find(index);
                            if (slot == state.end() || !assigned.insert(index).second) continue;
                            setSlot(index, slot->second);
                        }
                    }
                    for (const auto& [index, amount] : state) {
                        if (!assigned.insert(index).second) continue;
                        setSlot(index, amount);
                    }
                    control->fields["backgroundBitmap"] = background;
                    control->fields["highlightBitmap"] = highlight;
                    control->fields["infiniteAmmoBitmap"] = infinite;
                };
                auto& gui = Engine::instance().guiRenderer();
                const auto& weaponHud = demoParser->getWeaponsHud();
                applySlots(gui.findControl("weaponsHud"), weaponHud.slots,
                            weaponHud.activeIndex, weaponHud.bitmaps,
                            weaponHud.backgroundBitmap, weaponHud.highlightBitmap,
                            weaponHud.infiniteAmmoBitmap, &weaponHud.slotOrder,
                            &weaponHud.itemNames);
                const auto& inventoryHud = demoParser->getInventoryHud();
                applySlots(gui.findControl("inventoryHud"), demoParser->getInventoryHud().slots, -1,
                            inventoryHud.bitmaps, inventoryHud.backgroundBitmap, "", "",
                            nullptr, nullptr);
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
                            if (ObserverParity::isPositionReady(g->hasPosition)) {
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
            if (demoLive) {
                const char* mission = liveMissionName.empty() ? "Connecting" : liveMissionName.c_str();
                snprintf(title, sizeof(title), "Torch - %s (LAN)", mission);
            } else {
                snprintf(title, sizeof(title), "Torch - Demo [%d%%] %d/%d blocks %d packets",
                    pct, demoBlocksDone, demoBlocksTotal, demoPacketsParsed);
            }
            Engine::instance().platform().setTitle(title);

            // Advance interpolation blend (smooth over ~150ms)
            if (demoMoveBlend < 1.0f) {
                demoMoveBlend = std::min(demoMoveBlend + demoInterpolationDt * 6.0f, 1.0f);
            }

            // Free camera toggle during demos (F1)
            static bool prevDemoFreeCam = false;
            if (currentInput.freeCam && !prevDemoFreeCam && !demoLive) {
                freeCamActive = !freeCamActive;
                if (freeCamActive) {
                    freeCamPos = demoHasPos ? demoCameraPos : pl->cameraPos();
                    freeCamTarget = demoHasPos ? demoCameraTarget : pl->cameraTarget();
                }
            }
            prevDemoFreeCam = currentInput.freeCam;
            if (freeCamActive) {
                 float camSpeed = 50.0f * dt * freeCameraMoveScale(
                     currentInput.forward, currentInput.backward,
                     currentInput.left, currentInput.right,
                     currentInput.jump, currentInput.jet);
                float yaw = freeCamRot.z;
                float pitch = freeCamRot.x;
                yaw -= currentInput.lookDelta.y; // mouse right turns toward screen right
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                freeCamRot = {pitch, 0, yaw};
                Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                Point3F right = {-std::cos(yaw), 0, std::sin(yaw)}; // fwd x up: screen right
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
            // The world's effects (particles, explosions, debris, weather)
            // run on the demo clock and hold still once the match ends.
            if (w) w->update(demoMatchEnded ? 0.0f : demoInterpolationDt);
            return; // skip normal game logic during demo playback
        }

        // Demo playback has its own clock; normal simulation honors the
        // TorqueScript time scale from this point onward.
        dt = simulationDt;

        // Mapper mode: free-fly camera only, no player gameplay
        if (mapperMode) {
            if (freeCamActive) {
                 float camSpeed = 50.0f * dt * freeCameraMoveScale(
                     currentInput.forward, currentInput.backward,
                     currentInput.left, currentInput.right,
                     currentInput.jump, currentInput.jet);
                // Speed multipliers: Shift = 3x, Ctrl = 0.25x
                auto& plat = Engine::instance().platform();
                auto& keys = plat.input().keysDown;
                if (keys[SCANCODE_LSHIFT] || keys[SCANCODE_RSHIFT]) camSpeed *= 3.0f;
                if (keys[SCANCODE_LCTRL] || keys[SCANCODE_RCTRL]) camSpeed *= 0.25f;
                float yaw = freeCamRot.z;
                float pitch = freeCamRot.x;
                yaw -= currentInput.lookDelta.y;        // mouse right turns toward screen right
                pitch -= currentInput.lookDelta.x;      // mouse Y (vert) → pitch
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                freeCamRot = {pitch, 0, yaw};
                Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                Point3F right = {-std::cos(yaw), 0, std::sin(yaw)}; // fwd x up: screen right
                if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
                if (currentInput.left) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
                if (currentInput.right) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
            }
            // World simulation belongs to the same scaled clock as movement.
            // Using the raw frame delta here made projectiles, item respawns,
            // and fog transitions ignore local time dilation.
            w->update(simulationDt);
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

        auto& plat = Engine::instance().platform();
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

        // Tribes 2 maps the mouse wheel to weapon cycling during normal play.
        // Keep free-camera scrolling local to that tool instead of
        // changing the player's loadout while inspecting a mission.
        if (!freeCamActive && !pl->isDead() &&
            plat.input().mouseWheel != 0) {
            const int direction = plat.input().mouseWheel > 0 ? 1 : -1;
            // A coalesced SDL event is normally only a few notches; cap a
            // malformed accumulated value so input cannot stall the frame.
            const int steps = std::min(mouseWheelSteps(plat.input().mouseWheel), 32);
            for (int step = 0; step < steps; ++step)
                pl->weaponCycle(direction);
        }

        if (freeCamActive) {
            // Move free camera using WASD + mouse
             float camSpeed = 50.0f * dt * freeCameraMoveScale(
                 currentInput.forward, currentInput.backward,
                 currentInput.left, currentInput.right,
                 currentInput.jump, currentInput.jet);
            float yaw = freeCamRot.z;
            float pitch = freeCamRot.x;

            // Mouse look
            yaw -= currentInput.lookDelta.y; // mouse right turns toward screen right
            pitch -= currentInput.lookDelta.x;
            if (pitch > 1.5f) pitch = 1.5f;
            if (pitch < -1.5f) pitch = -1.5f;
            freeCamRot = {pitch, 0, yaw};

            // Direction vectors
            Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
            Point3F right = {-std::cos(yaw), 0, std::sin(yaw)}; // fwd x up: screen right

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
            physics.update(pl, simulationDt, currentInput);
            // ShapeBase repairRate is processed once per simulation tick. Keep
            // this separate from movement so scripted repair works while the
            // player is standing still as well as while moving.
            pl->update(simulationDt);

            // Update player animation state
            // Match movement's jetting state.  A held jet key is not enough:
            // once energy is depleted the native player falls back to the
            // jump animation instead of continuing to show the jet effect.
            pl->updateAnimation(simulationDt, currentInput.jet && pl->energy() > 0.0f);

            // Update weapon timers
            for (int i = 0; i < pl->weaponCount(); i++) {
                const_cast<Weapon&>(pl->weapon(i)).updateTimers(simulationDt);
            }
            pl->updateWeaponHud();

            // Fire weapon
            const int32_t currentWeapon = pl->currentWeapon();
            const bool validWeapon = currentWeapon >= 0 &&
                currentWeapon < pl->weaponCount() &&
                pl->weapon(currentWeapon).type >= 0 &&
                pl->weapon(currentWeapon).type < gWeaponCount;
            const WeaponData* currentWeaponData = validWeapon
                ? &gWeaponTable[pl->weapon(currentWeapon).type] : nullptr;
            const int triggerMode = currentWeaponData ? weaponTriggerMode(
                *currentWeaponData, currentInput.fire, currentInput.altFire,
                previousFire, previousAltFire) : 0;
            if (triggerMode == 1) {
                pl->fireWeapon(false);
            } else if (triggerMode == 2) {
                pl->fireWeapon(true);
            }
            // Consume the trigger sample after the simulation tick. Input can
            // be sampled less often than update() (for example during a
            // catch-up frame); leaving this edge state untouched would make a
            // held semi-automatic trigger fire once per catch-up tick.
            previousFire = currentInput.fire;
            previousAltFire = currentInput.altFire;

            // Send the move to the server
            if (cfg.online && activeConn && activeConn->state() >= Connection::Connected) {
                uint32_t thisSeq = ++moveSeq;

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
            if (buttonPressed(currentInput.reload, previousReload)) {
                int32_t cw = pl->currentWeapon();
                if (cw >= 0 && cw < pl->weaponCount()) {
                    auto& weapon = const_cast<Weapon&>(pl->weapon(cw));
                    if (weapon.type >= 0 && weapon.type < gWeaponCount &&
                        weaponCanStartReload(gWeaponTable[weapon.type], weapon.ammo) &&
                        !weapon.reloading) {
                        beginWeaponReload(weapon.reloading, weapon.reloadTimer,
                                          weapon.firing, weapon.fireTimer,
                                          gWeaponTable[weapon.type].reloadTime);
                    }
                }
            }
        }

        // Death check (deaths tracked in Player::applyDamage)
        if (pl->health() <= 0 && gameState == Playing) {
            deathTimer = 0.0f;
            currentInput = {};
            w->projectiles().clear();
            setState(Dead);
        }

        // Update world (projectiles, etc.)
        w->update(simulationDt);

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
                  const bool sensorVisible = isClientTargetVisible(ghost->targetId);
                  return ObserverParity::isSpectatableTarget(
                      ghost->className, ghost->damageState, sensorVisible);
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
                const int control = ObserverParity::controlGhostIndex(
                    activeConn->observerSnapshot().controlGhost);
                if (std::find(spectatableIndices.begin(), spectatableIndices.end(),
                              control) != spectatableIndices.end())
                    initial = control;
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
                    // Enter free camera from the current view. Resetting to
                    // world origin makes a live observer visibly teleport
                    // across the map when toggling the camera.
                    freeCamPos = pl->cameraPos();
                    freeCamTarget = pl->cameraTarget();
                    freeCamRot = pl->rotation();
                }
            }
            prevFree = currentInput.freeCam;

            // Free cam movement
            if (freeCamActive) {
                float pitch = freeCamRot.x, yaw = freeCamRot.z;
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                yaw = freeCameraYaw(yaw, currentInput.lookDelta.y);
                freeCamRot = {pitch, 0, yaw};
                 float camSpeed = 30.0f * dt * freeCameraMoveScale(
                     currentInput.forward, currentInput.backward,
                     currentInput.left, currentInput.right,
                     currentInput.jump, currentInput.jet);
                Point3F fwd = {sinf(yaw)*cosf(pitch), sinf(pitch), cosf(yaw)*cosf(pitch)};
                Point3F right = {-cosf(yaw), 0, sinf(yaw)}; // fwd x up: screen right
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
            // Respawn is simulation state too.  Using wall-clock dt here made
            // offline deaths ignore the active Torque time scale while all
            // other gameplay timers honored it.
            deathTimer += simulationDt;
            if (missionRespawn && deathTimer >= DeathRespawn::RespawnDelay) {
                deathTimer = 0.0f;
                 liveSpectateInit = false;
                liveFollowGhostIndex = -1;
                liveFollowCenterInit = false;
                pl->respawn();
                // A new life starts with fresh weapon trigger edges. Keep the
                // current input held so a trigger can fire on the next tick,
                // but do not let the previous life suppress semi-auto fire.
                previousFire = false;
                previousAltFire = false;
                previousReload = false;
                setState(Playing);
            }
        }
    }
}

void Game::render(float dt) {
    if (gameState != Playing && gameState != Dead && !testShapeLoaded && !shapeViewerActive) return;

    auto& eng = Engine::instance();
    auto& r = eng.renderer();
    auto applyShapeBaseAudio = [&](GhostEntry& ghost, int ghostIndex, const Vec3& position,
                                   const std::map<uint32_t, ParsedDataBlock>& blocks) {
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
            // ShapeBase::updateAudioState stops the slot's sound and plays the
            // written profile on every SoundMask write. A slot whose source
            // was dropped (restore, remount) without a newer write resumes
            // only a looping profile; a one-shot already played.
            auto started = shapeBaseSoundRevisions.find(key);
            const bool newWrite = started == shapeBaseSoundRevisions.end() || state.revision > started->second;
            if (!newWrite) {
                if (it != shapeBaseSoundSources.end()) {
                    it->second->setPosition(Math::torquePointToYUp({position.x, position.y, position.z}));
                    continue;
                }
                auto profile = blocks.find((uint32_t)state.profileId);
                if (profile == blocks.end() || !profile->second.decoded.audioLooping) continue;
            } else {
                shapeBaseSoundRevisions[key] = state.revision;
            }
            if (it != shapeBaseSoundSources.end()) {
                audio.releaseSource(it->second);
                shapeBaseSoundSources.erase(it);
            }
            {
                auto* source = playNativeAudioProfile(audio, blocks, (uint32_t)state.profileId,
                    Math::torquePointToYUp({position.x, position.y, position.z}), false, true);
                if (!source) continue;
                it = shapeBaseSoundSources.emplace(key, source).first;
            }
            it->second->setPosition(Math::torquePointToYUp({position.x, position.y, position.z}));
        }
    };
    r.beginFrame({0.3f, 0.5f, 0.8f, 1.0f});
    DTSShape::advanceCloakShift();

    Point3F camPos, camTarget;
    bool cameraCoordinatesConverted = false;
    const GhostEntry* observed = demoPlaying && demoParser && demoObserveGhost >= 0
        ? demoParser->getGhostTracker().getGhost(demoObserveGhost) : nullptr;
    if (observed && ObserverParity::isPositionReady(observed->hasPosition)) {
        // Torque space, converted below with the other demo cameras.
        const Vec3& p = observed->renderPos;
        const float yaw = observed->bodyYaw;
        camPos = {p.x - sinf(yaw) * demoObserveDistance, p.y - cosf(yaw) * demoObserveDistance,
                  p.z + demoObserveHeight};
        camTarget = {p.x, p.y, p.z + 1.2f};
    } else if (freeCamActive) {
        camPos = freeCamPos;
        camTarget = freeCamTarget;
    } else if (demoPlaying && (demoHasPos || demoFirstPersonCam)) {
        // Orbit camera for spectator mode
        if (demoFirstPersonCam) {
            if (demoHasPos && demoAuthoredCamera) {
                camPos = demoCameraPos;
                camTarget = demoCameraTarget;
                // Camera::interpolateTick: orbit modes centre on the orbit
                // object's render world box (or the orbit point), turn by
                // mRot and pull back per validateEyePoint.
                const Vec3 forwardT = T2Demo::cameraDirectionFromYawPitch(demoViewYaw, demoViewPitch);
                const Point3F forward = Math::torquePointToYUp({forwardT.x, forwardT.y, forwardT.z});
                const Point3F* center = nullptr;
                Point3F orbitCenter;
                if (demoCameraMode == T2Demo::CameraOrbitObject) {
                    auto box = demoBoxCenters.find(demoOrbitGhost);
                    if (box != demoBoxCenters.end()) center = &box->second;
                } else if (demoCameraMode == T2Demo::CameraOrbitPoint) {
                    orbitCenter = Math::torquePointToYUp(demoOrbitPoint);
                    center = &orbitCenter;
                }
                if (center && w) {
                    const float distance = demoOrbitMaxDist - demoOrbitMinDist;
                    const Point3F rayEnd{center->x - forward.x * 2.5f * distance,
                                         center->y - forward.y * 2.5f * distance,
                                         center->z - forward.z * 2.5f * distance};
                    ProjectilePhysics::RayHit hit;
                    const bool blocked = distance > 0.0f && castStaticRay(*w, *center, rayEnd, hit);
                    float along = 0.0f, dot = 0.0f;
                    if (blocked) {
                        along = (center->x - hit.point.x) * forward.x + (center->y - hit.point.y) * forward.y +
                                (center->z - hit.point.z) * forward.z;
                        dot = forward.x * hit.normal.x + forward.y * hit.normal.y + forward.z * hit.normal.z;
                    }
                    const float eyeDistance = T2Demo::orbitEyeDistance(
                        demoOrbitMinDist, demoOrbitMaxDist, blocked, along, dot);
                    camPos = {center->x - forward.x * eyeDistance, center->y - forward.y * eyeDistance,
                              center->z - forward.z * eyeDistance};
                } else {
                    camPos = Math::torquePointToYUp(demoCameraPos);
                }
                camTarget = {camPos.x + forward.x * 10.0f, camPos.y + forward.y * 10.0f,
                             camPos.z + forward.z * 10.0f};
                cameraCoordinatesConverted = true;
            } else {
                camPos = pl ? pl->cameraPos() : Point3F{0, 6, 0};
                camTarget = pl ? pl->cameraTarget() : Point3F{0, 6, 1};
                bool cameraGhostUsed = false;
            // First-person camera: position at spectated ghost's eye level, look in facing direction
            int fpIdx = (spectateGhostIndex >= 0) ? spectateGhostIndex : controlGhostIndex;
            if (fpIdx >= 0 && demoParser) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(fpIdx);
                if (g && ObserverParity::isPositionReady(g->hasPosition)) {
                    const Vec3& position = g->hasRendered ? g->renderPos : g->position;
                    if (ghostClassIs(g->className, "Camera") && g->hasCameraEuler) {
                        const float pitch = g->cameraEuler.x;
                        const float yaw = g->cameraEuler.z;
                        // Torque space (z up): forward from yaw about z, pitch down.
                        camPos = {position.x, position.y, position.z};
                        camTarget = {position.x + std::sin(yaw) * std::cos(pitch) * 10.0f,
                                     position.y + std::cos(yaw) * std::cos(pitch) * 10.0f,
                                     position.z - std::sin(pitch) * 10.0f};
                        cameraGhostUsed = true;
                    } else {
                        // The player's eye (t2-mapper DEFAULT_EYE_HEIGHT) looking along
                        // body yaw + head yaw and head pitch.
                        float maxLookAngle = 0.0f;
                        if (g->hasDatablock) {
                            const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                            auto data = blocks.find((uint32_t)g->datablockId);
                            if (data != blocks.end()) maxLookAngle = data->second.decoded.playerMaxLookAngle;
                        }
                        Point3F aim = playerAimDirection(g->bodyYaw, g->headYaw, g->headPitch, maxLookAngle);
                        // The recorder's own view comes from its moves, not ghost updates.
                        if (fpIdx == controlGhostIndex && spectateGhostIndex < 0 && demoHasOrientation) {
                            // Player::getRenderEyeTransform: the render
                            // transform (the seat's mount frame when mounted)
                            // turned by mHead.z then mHead.x.
                            const auto frame = demoShapeFrames.find(fpIdx);
                            if (g->mountObject >= 0 && frame != demoShapeFrames.end()) {
                                const Vec3 local = T2Demo::cameraDirectionFromYawPitch(demoHeadZ, demoViewPitch);
                                // The DTS loader's local frame is Torque's with y and z swapped.
                                const Point3F world = frame->second.transformNormal({local.x, local.z, local.y});
                                const float len = std::sqrt(world.x * world.x + world.y * world.y + world.z * world.z);
                                if (len > 1e-6f) aim = {world.x / len, -world.z / len, world.y / len};
                            } else {
                                const Vec3 view = T2Demo::cameraDirectionFromYawPitch(demoViewYaw + demoHeadZ,
                                                                                      demoViewPitch);
                                aim = {view.x, view.y, view.z};
                            }
                        }
                        // The animated eye node (as last drawn), else the
                        // default eye height.
                        camPos = {position.x, position.y, position.z + 2.1f};
                        if (auto eyeIt = demoEyePositions.find(fpIdx); eyeIt != demoEyePositions.end())
                            camPos = {eyeIt->second.x, -eyeIt->second.z, eyeIt->second.y};
                        camTarget = {camPos.x + aim.x * 10.0f, camPos.y + aim.y * 10.0f, camPos.z + aim.z * 10.0f};
                        cameraGhostUsed = true;
                        // ShapeBase::getCameraTransform at camera position 1
                        // (third person): (cameraMaxDist - cameraMinDist)
                        // behind the camera node along the eye's forward, cut
                        // short by a ray (collision.t less a tenth of the
                        // approach), all the way back to the eye at zero.
                        const auto camNode = demoCamPositions.find(fpIdx);
                        if (!demoRecordedFirstPerson && fpIdx == controlGhostIndex && spectateGhostIndex < 0 &&
                            g->mountObject < 0 && camNode != demoCamPositions.end() && w) {
                            float minDist = 0.2f, maxDist = 0.0f;
                            if (g->hasDatablock) {
                                const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                                auto data = blocks.find((uint32_t)g->datablockId);
                                if (data != blocks.end()) {
                                    minDist = data->second.decoded.cameraMinDist;
                                    maxDist = data->second.decoded.cameraMaxDist;
                                }
                            }
                            // ShapeBaseData::preload: at least half the shape's y extent.
                            if (g->shape && g->shape->hasHeaderBounds)
                                maxDist = std::max(maxDist, (g->shape->headerBoundsMax.y - g->shape->headerBoundsMin.y) * 0.5f);
                            const Point3F eyeYUp = Math::torquePointToYUp(camPos);
                            const Point3F forward = Math::torquePointToYUp(aim);
                            const float back = maxDist - minDist;
                            const Point3F vec{-forward.x * back, -forward.y * back, -forward.z * back};
                            const Point3F sp = camNode->second;
                            Point3F ep{sp.x + vec.x, sp.y + vec.y, sp.z + vec.z};
                            ProjectilePhysics::RayHit hit;
                            if (back > 0.0f && castStaticRay(*w, sp, ep, hit)) {
                                const float adj = (-(vec.x * hit.normal.x + vec.y * hit.normal.y + vec.z * hit.normal.z) /
                                                   back) * 0.1f;
                                const float newPos = std::max(0.0f, hit.t - adj);
                                ep = newPos == 0.0f ? eyeYUp
                                     : Point3F{sp.x + vec.x * newPos, sp.y + vec.y * newPos, sp.z + vec.z * newPos};
                            }
                            camPos = ep;
                            camTarget = {ep.x + forward.x * 10.0f, ep.y + forward.y * 10.0f, ep.z + forward.z * 10.0f};
                            cameraCoordinatesConverted = true;
                        }
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
                if (g && ObserverParity::isPositionReady(g->hasPosition))
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
             const auto observer = activeConn->observerSnapshot();
             auto isSpectatable = [&](const GhostEntry* ghost) {
                 if (!ghost) return false;
                 return ObserverParity::isSpectatableTarget(
                     ghost->className, ghost->damageState,
                     isSensorGroupTargetVisible(observer.playerSensorGroup,
                                                ghost->sensorGroup));
             };
            if (!isSpectatable(liveGhosts.getGhost(spectateGhostIndex))) {
                auto idxs = liveGhosts.getAllIndices();
                auto first = std::find_if(idxs.begin(), idxs.end(),
                    [&](int index) { return isSpectatable(liveGhosts.getGhost(index)); });
                spectateGhostIndex = first == idxs.end() ? -1 : *first;
            }
             const GhostEntry* g = liveGhosts.getGhost(spectateGhostIndex);
              if (g && ObserverParity::isPositionReady(g->hasPosition)) {
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
    Point3F finalCam = {camPos.x + nativeShake.x,
                        camPos.y + nativeShake.y,
                        camPos.z + nativeShake.z};
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
    }
    r.setCamera(finalCam, camTarget, {0, 1, 0});
    r.config().fov = savedFov; // restore for HUD rendering

    // Shadow pass and scene rendering — skip in shape viewer / test shape
    // mode, and when no GameTSCtrl is on the canvas (the commander screen
    // replaces PlayGui).
    const bool worldShown = mapperMode || !eng.hasGuiRenderer() || eng.guiRenderer().contentShowsWorld();
    if (!shapeViewerActive && !testShapeLoaded && worldShown) {
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

            // Interiors cast (same transform as World::render). DTS shapes
            // never enter the sun shadow map: players and vehicles cast
            // projected shadows instead.
            for (auto& obj : w->objects()) {
                if (!obj.shape || !obj.shape->loaded || !obj.shape->isInterior) continue;
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
                MatrixF mvp = r.lightViewProj() * model * obj.shape->upOrientation();
                shadowShader->setUniform("uLightMVP", mvp);
                for (auto& mesh : obj.shape->meshes)
                    mesh.render();
            }
        }

        r.endShadowPass();

        // Bind shadow map to texture unit 5 and set uniforms for main pass
        float shadowStrength = 1.0f;
        {
            auto* defShader = ShaderManager::getDefaultShader();
            defShader->bind();
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, r.shadowDepthTex);
            defShader->setUniform("uShadowMap", (int32_t)5);
            defShader->setUniform("uShadowStrength", shadowStrength);
            defShader->setUniform("uShadowMatrix", r.shadowMatrix());

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
    // Demo ghost lights, as gathered by the last ghost pass.
    if (demoPlaying && !demoLights.empty()) {
        auto lights = r.dynamicLights;
        lights.insert(lights.end(), demoLights.begin(), demoLights.end());
        r.setDynamicLights(lights);
    }
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
                              color[0], color[1], color[2], data.projectileLightRadius});
        }
        r.setDynamicLights(lights);
    }
    w->render(finalCam, dt);
    if (pl && !freeCamActive && !demoPlaying && !testShapeLoaded) pl->render();
    } // end if (!shapeViewerActive && !testShapeLoaded)

    if (worldShown && (mapperMode || gameState == Playing)) {
        auto* font = r.getFont();
        if (font) {
            for (const auto& obj : w->objects()) {
                if (obj.label.empty()) continue;
                 if ((missionClassIs(obj.className, "Marker") ||
                      missionClassIs(obj.className, "MissionMarker") ||
                      missionClassIs(obj.className, "SpawnSphere") ||
                      missionClassIs(obj.className, "AIObjective")) && !mapperMode)
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
                      labelColor = objectiveMarkerColor(obj.teamId, pl ? pl->team() : 0, mapperMode);
                 ScreenSpace2D screenSpace(r);
                 font->render(obj.label.c_str(), screen.x - 35, screen.y - 12,
                     labelColor, 1.0f);
            }
        }
    }

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
                    if (v.pos.x < mn.x) mn.x = v.pos.x;
                    if (v.pos.y < mn.y) mn.y = v.pos.y;
                    if (v.pos.z < mn.z) mn.z = v.pos.z;
                    if (v.pos.x > mx.x) mx.x = v.pos.x;
                    if (v.pos.y > mx.y) mx.y = v.pos.y;
                    if (v.pos.z > mx.z) mx.z = v.pos.z;
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
        MatrixF model = ry * rx * (testShape.nativeDTS ? Math::nativeDtsFrame() : MatrixF{}) *
                        testShape.upOrientation() * sc * tr;
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
                    if (wp.x < mn.x) mn.x = wp.x;
                    if (wp.y < mn.y) mn.y = wp.y;
                    if (wp.z < mn.z) mn.z = wp.z;
                    if (wp.x > mx.x) mx.x = wp.x;
                    if (wp.y > mx.y) mx.y = wp.y;
                    if (wp.z > mx.z) mx.z = wp.z;
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
        // SV_YAW/SV_PITCH (radians) fix the orbit for scripted captures.
        if (const char* svYaw = getenv("SV_YAW")) shapeViewerYaw = (float)atof(svYaw);
        if (const char* svPitch = getenv("SV_PITCH")) shapeViewerPitch = (float)atof(svPitch);
        MatrixF ry; ry.setRotationY(shapeViewerYaw);
        MatrixF rx; rx.setRotationX(shapeViewerPitch);
        MatrixF sc; sc.setScale({shapeViewerFitScale, shapeViewerFitScale, shapeViewerFitScale});
        MatrixF tr; tr.setTranslation({-shapeViewerCenter.x, -shapeViewerCenter.y, -shapeViewerCenter.z});
        // Orbit in Y-up: ry*rx rotate in Y-up, then C converts Z-up->Y-up (if needed), then sc*tr center/scale in Z-up
        MatrixF model = ry * rx * (shapeViewerShape.nativeDTS ? Math::nativeDtsFrame() : MatrixF{}) *
                        shapeViewerShape.upOrientation() * sc * tr;
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
            // SV_BLEND="name:position,..." layers blend sequences at a
            // normalized position, e.g. look:0 for an upward aim.
            std::vector<DTSShape::BlendThread> svBlends;
            if (const char* svBlend = getenv("SV_BLEND")) {
                std::stringstream list(svBlend);
                std::string item;
                while (std::getline(list, item, ',')) {
                    const size_t colon = item.find(':');
                    const std::string name = item.substr(0, colon);
                    const float position = colon == std::string::npos ? 0.5f
                        : (float)atof(item.c_str() + colon + 1);
                    if (const auto* clip = findAnimation(shapeViewerShape, name.c_str()))
                        svBlends.push_back({(int)(clip - shapeViewerShape.animations.data()),
                                            std::clamp(position, 0.0f, 1.0f) * clip->duration});
                }
            }
            shapeViewerShape.renderAnimationIndex((int)(&anim - shapeViewerShape.animations.data()),
                animationSampleTime(animTime, anim.duration, anim.looping), nullptr, 0,
                svBlends.data(), (int)svBlends.size());
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

        const GhostTracker& gt = demoMatchEnded ? demoEndedGhosts : demoParser->getGhostTracker();
        std::vector<int> indices = gt.getAllIndices();
        w->beginProjectileTrailSync();
        demoLights.clear();
        for (int idx : indices) {
            const GhostEntry* g = gt.getGhost(idx);
            r.shadowCapture = nullptr;
            if (!g) continue;
            if (ghostClassIs(g->className, "ForceFieldBare")) {
                w->syncForceFieldGhost(idx, g->forceFieldState, g->forceFieldPosition,
                                       g->forceFieldStateUpdates);
                continue;
            }
            if (ghostClassIs(g->className, "InteriorInstance"))
                w->syncInteriorAlarmGhost(idx, g->interiorAlarm);

            // Resolve player name from skin name if not already set
            if (g->playerName.empty() && !g->skinName.empty()) {
                const std::string& pn = demoParser->getPlayerNameForSkin(g->skinName);
                if (!pn.empty()) {
                    GhostEntry* mg = const_cast<GhostEntry*>(g);
                    mg->playerName = pn;
                }
            }
            Vec3 p = g->position;
            if (!ObserverParity::isPositionReady(g->hasPosition)) continue;

            // Skip world-level objects already rendered by World
            if (!isRenderableGhostClass(g->className)) continue;

            // Interpolate position for smooth rendering (applies to both shape and fallback paths)
            GhostEntry* mg = const_cast<GhostEntry*>(g);
            Vec3 rp = p;
            if (!mg->hasRendered) {
                mg->renderPos = p;
                mg->renderRotation = g->rotation;
                mg->prevPosition = p;
                mg->spawnTime = demoTime;
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
            // Player::interpolateTick: a predicted player is drawn between its
            // last two simulation ticks (backDelta of a tick behind).
            if (ObserverParity::isPlayerClass(g->className) && mg->prediction.initialized &&
                g->mountObject < 0) {
                const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                const float backDelta = std::clamp(1.0f - (now - demoLastPlayerTick) / 0.032f, 0.0f, 1.0f);
                const Point3F p = PlayerPrediction::renderPosition(mg->prediction, backDelta);
                rp = {p.x, p.y, p.z};
                mg->renderPos = rp;
                mg->velocity = {mg->prediction.velocity.x, mg->prediction.velocity.y, mg->prediction.velocity.z};
                if (idx != controlGhostIndex) {
                    const float half = PlayerPrediction::renderYaw(mg->prediction, backDelta) * 0.5f;
                    mg->renderRotation = {0.0f, 0.0f, -std::sin(half), std::cos(half)};
                }
            }
            // LinearProjectile::createSegments / interpolateTick: the ghost
            // carries its initial position and direction; the client flies it
            // at the dry (or, fired underwater, wet) muzzle speed plus the
            // shooter's excess velocity, from currTick, along one segment cut
            // at the first world hit or the lifetime.
            if (mg->hasLinearFlight && g->hasDatablock && !g->exploded) {
                const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                auto block = blocks.find((uint32_t)g->datablockId);
                if (block != blocks.end()) {
                    const auto& data = block->second.decoded;
                    if (!mg->linearSegmentValid) {
                        const Point3F startYUp = Math::torquePointToYUp(
                            {mg->linearStart.x, mg->linearStart.y, mg->linearStart.z});
                        const bool wetStart = w && w->isUnderwater(startYUp);
                        const float speed = wetStart && data.projectileWetVelocity > 0.0f
                            ? data.projectileWetVelocity : data.projectileDryVelocity;
                        mg->linearVelocity = {mg->linearDir.x * speed + mg->linearExcess.x,
                                              mg->linearDir.y * speed + mg->linearExcess.y,
                                              mg->linearDir.z * speed + mg->linearExcess.z};
                        const float lifetime = std::max(0.0f, data.projectileLifetimeMS / 1000.0f);
                        const Point3F velYUp = Math::torquePointToYUp(
                            {mg->linearVelocity.x, mg->linearVelocity.y, mg->linearVelocity.z});
                        const float vlen = std::sqrt(velYUp.x * velYUp.x + velYUp.y * velYUp.y + velYUp.z * velYUp.z);
                        float endTime = lifetime;
                        if (vlen > 1e-4f && w) {
                            const Point3F endYUp{startYUp.x + velYUp.x * lifetime, startYUp.y + velYUp.y * lifetime,
                                                 startYUp.z + velYUp.z * lifetime};
                            ProjectilePhysics::RayHit hit;
                            if (castStaticRay(*w, startYUp, endYUp, hit)) endTime = hit.t * lifetime;
                        }
                        mg->linearEndTime = endTime;
                        mg->linearSegmentValid = true;
                        // A fresh initial update (possibly a reused ghost index).
                        mg->spawnTime = demoMatchEnded ? demoMatchEndedAt : demoTime;
                        mg->flareSpikes.clear();
                    }
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    const float age = mg->linearCurrTick * 0.032f + std::max(0.0f, now - mg->spawnTime);
                    const float t = std::min(age, mg->linearEndTime);
                    rp = {mg->linearStart.x + mg->linearVelocity.x * t,
                          mg->linearStart.y + mg->linearVelocity.y * t,
                          mg->linearStart.z + mg->linearVelocity.z * t};
                    mg->renderPos = rp;
                    mg->velocity = mg->linearVelocity;
                }
            }
            // GrenadeProjectile::processTick for grenades, mortar shells,
            // energy bolts and flares (seekers coast without gravity): fly the last transmitted state per
            // 32 ms tick with gravity x gravityMod, bouncing off the static
            // world until armed; the first armed contact stops it (the server
            // sends the explosion and bounce corrections).
            if (mg->hasBallistic && g->hasDatablock && !g->exploded && w) {
                const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                auto block = blocks.find((uint32_t)g->datablockId);
                if (block != blocks.end()) {
                    const auto& data = block->second.decoded;
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    if (mg->ballisticFresh) {
                        if (!mg->ballisticStopped && mg->ballisticTime == 0.0f)
                            mg->ballisticAgeTicks = mg->ballisticCurrTick;
                        mg->ballisticPos = Math::torquePointToYUp(
                            {mg->ballisticSentPos.x, mg->ballisticSentPos.y, mg->ballisticSentPos.z});
                        mg->ballisticVel = Math::torquePointToYUp(
                            {mg->ballisticSentVel.x, mg->ballisticSentVel.y, mg->ballisticSentVel.z});
                        mg->ballisticTime = now;
                        mg->ballisticStopped = false;
                        mg->ballisticFresh = false;
                    }
                    // The engine floors arming at 250 ms, in whole ticks.
                    const int armedTick = (int)std::ceil(std::max(250, data.grenadeArmingDelayMS) / 32.0f);
                    const ProjectilePhysics::CastRay cast = [&](const Point3F& a, const Point3F& b,
                                                                 ProjectilePhysics::RayHit& hit) {
                        return castStaticRay(*w, a, b, hit);
                    };
                    int guard = 0;
                    while (!mg->ballisticStopped && now - mg->ballisticTime >= ProjectilePhysics::TickSeconds &&
                           guard++ < 64) {
                        mg->ballisticStopped = !ProjectilePhysics::stepBallistic(
                            mg->ballisticPos, mg->ballisticVel,
                            mg->ballisticCoast ? 0.0f : getGravity() * data.grenadeGravityMod,
                            data.grenadeElasticity, data.grenadeFriction,
                            mg->ballisticCoast || mg->ballisticAgeTicks > armedTick, cast);
                        mg->ballisticTime += ProjectilePhysics::TickSeconds;
                        ++mg->ballisticAgeTicks;
                    }
                    if (guard >= 64) mg->ballisticTime = now; // long pause: resync the clock
                    const Point3F torque{mg->ballisticPos.x, -mg->ballisticPos.z, mg->ballisticPos.y};
                    rp = {torque.x, torque.y, torque.z};
                    mg->renderPos = rp;
                    mg->velocity = {mg->ballisticVel.x, -mg->ballisticVel.z, mg->ballisticVel.y};
                }
            }

            // Item::processTick: a thrown or dropped item falls and bounces
            // on the client from each position update until it rests;
            // interpolateTick draws it between its last two ticks.
            if (ghostClassIs(g->className, "Item") && g->hasDatablock && g->mountObject < 0 && w) {
                const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                auto block = blocks.find((uint32_t)g->datablockId);
                const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                if (mg->itemSimUpdate != g->itemPositionUpdates) {
                    mg->itemSimUpdate = g->itemPositionUpdates;
                    mg->itemSimPos = mg->itemSimPrevPos = g->position;
                    mg->itemSimVel = g->itemVelocity;
                    mg->itemSimAtRest = g->itemAtRest || g->itemStatic;
                    mg->itemSimTime = now;
                }
                if (block != blocks.end() && g->itemPositionUpdates > 0) {
                    const auto& data = block->second.decoded;
                    if (!mg->itemSimAtRest && !g->itemStatic && mg->fadeVal > 0.0f) {
                        ItemPhysics::Params params;
                        params.gravityMod = data.itemGravityMod;
                        params.maxVelocity = data.itemMaxVelocity;
                        params.friction = data.itemFriction;
                        params.elasticity = data.itemElasticity;
                        params.sticky = data.itemSticky;
                        if (g->shape && g->shape->hasHeaderBounds) {
                            const Point3F& lo = g->shape->headerBoundsMin;
                            const Point3F& hi = g->shape->headerBoundsMax;
                            const float cx = (lo.x + hi.x) * 0.5f, cy = (lo.y + hi.y) * 0.5f;
                            params.topCentre = Math::torquePointToYUp({cx, cy, hi.z});
                            params.bottomCentre = Math::torquePointToYUp({cx, cy, lo.z});
                        }
                        const ProjectilePhysics::CastRay cast = [&](const Point3F& a, const Point3F& b,
                                                                     ProjectilePhysics::RayHit& hit) {
                            return castStaticRay(*w, a, b, hit);
                        };
                        Point3F pos = Math::torquePointToYUp(
                            {mg->itemSimPos.x, mg->itemSimPos.y, mg->itemSimPos.z});
                        Point3F vel = Math::torquePointToYUp(
                            {mg->itemSimVel.x, mg->itemSimVel.y, mg->itemSimVel.z});
                        int guard = 0;
                        while (!mg->itemSimAtRest && now - mg->itemSimTime >= ItemPhysics::TickSeconds &&
                               guard++ < 64) {
                            mg->itemSimPrevPos = mg->itemSimPos;
                            mg->itemSimAtRest = ItemPhysics::step(pos, vel, params, cast);
                            mg->itemSimPos = {pos.x, -pos.z, pos.y};
                            mg->itemSimVel = {vel.x, -vel.z, vel.y};
                            mg->itemSimTime += ItemPhysics::TickSeconds;
                        }
                        if (guard >= 64) mg->itemSimTime = now; // long pause: resync the clock
                    }
                    const float t = mg->itemSimAtRest ? 1.0f
                        : std::clamp((now - mg->itemSimTime) / ItemPhysics::TickSeconds, 0.0f, 1.0f);
                    rp = {mg->itemSimPrevPos.x + (mg->itemSimPos.x - mg->itemSimPrevPos.x) * t,
                          mg->itemSimPrevPos.y + (mg->itemSimPos.y - mg->itemSimPrevPos.y) * t,
                          mg->itemSimPrevPos.z + (mg->itemSimPos.z - mg->itemSimPrevPos.z) * t};
                    mg->renderPos = rp;
                }
            }

              const bool isProjectile = isProjectileGhostClass(g->className) ||
                  ghostClassIs(g->className, "TracerProjectile");
              // LinearProjectile::processTick hides a spent projectile until
              // its ghost is deleted.
              if (isProjectile && g->exploded) continue;
             const V12::DecodedDataBlock* visualData = nullptr;
             if (isProjectile && g->hasDatablock) {
                 const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                 auto projectileIt = dataBlocks.find((uint32_t)g->datablockId);
                  if (projectileIt != dataBlocks.end()) {
                      const auto& projectileData = projectileIt->second.decoded;
                      visualData = &projectileData;
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
                     // Projectile::registerLights: a hasLight datablock lights
                     // lightRadius around the projectile. SeekerProjectile
                     // withholds it for flechetteDelayMs >> 5 ticks.
                     if (projectileData.projectileHasLight && projectileData.projectileLightRadius > 0.0f) {
                         const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                         const bool flechette = ghostClassIs(g->className, "SeekerProjectile") &&
                             projectileData.seekerUseFlechette &&
                             (now - mg->spawnTime) * 1000.0f <
                                 (float)((projectileData.seekerFlechetteDelayMS >> 5) * 32);
                         if (!flechette) {
                             const Point3F at = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                             const auto& c = projectileData.projectileLightColor;
                             demoLights.push_back({at.x, at.y, at.z, c[0], c[1], c[2],
                                                   projectileData.projectileLightRadius});
                         }
                     }
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
            if (!demoMatchEnded) mg->threadAnimTime += demoInterpolationDt;
             mg->prevPosition = rp;
             mg->hasRendered = true;
              if (!demoMatchEnded)
                  applyShapeBaseAudio(*mg, idx, p, demoParser->getInitialBlock().dataBlocks);

               if (visualData && visualData->projectileMaterial != V12::DecodedDataBlock::ProjectileMaterial::None) {
                    const auto& data = *visualData;
                    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
                    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
                    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
                    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
                    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
                    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
                    // Projectile material sprites are transparent/additive even
                    // though the opaque scene pass has blending disabled.
                    glEnable(GL_BLEND);
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
                   // TracerProjectile / EnergyProjectile (tracer and bolt renderers,
                   // t2-mapper tracer.ts): an additive camera-facing quad centred on
                   // the projectile along its flight, plus an end-on cross seen
                   // within crossViewAng of the axis; bolts add a motion-blur tail.
                   // Unfogged.
                    const bool crossStyle = data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Cross;
                    const bool tracerAdditive = !data.projectileTracerAlpha;
                    if (crossStyle) {
                       const float length = data.projectileIsEnergyBolt
                           ? (data.hasProjectileScale ? data.projectileScale.y : 20.0f) : data.projectileTracerLength;
                       const float halfWidth = data.projectileIsEnergyBolt
                           ? (data.hasProjectileScale ? data.projectileScale.x : 0.25f) : data.projectileTracerWidth;
                       Point3F dir = velocity;
                       const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                       const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                       if (data.projectileIsEnergyBolt && data.projectileBlurLifetime > 0.0f) {
                           auto& tail = mg->blurTail;
                           if (!tail.empty() && now < tail.back().second) tail.clear();
                           if (tail.empty() || now > tail.back().second) tail.push_back({materialPos, now});
                           if (tail.size() > 32) tail.erase(tail.begin());
                           while (!tail.empty() && now - tail.front().second > data.projectileBlurLifetime)
                               tail.erase(tail.begin());
                           const ColorF blurColor{data.projectileBlurColor[0], data.projectileBlurColor[1],
                                                  data.projectileBlurColor[2], 1.0f};
                           for (size_t i = 0; i + 1 < tail.size(); ++i) {
                               const Point3F a = tail[i].first, b = tail[i + 1].first;
                               const Point3F seg{b.x - a.x, b.y - a.y, b.z - a.z};
                               const Point3F toCam{a.x - r.cameraPos.x, a.y - r.cameraPos.y, a.z - r.cameraPos.z};
                               Point3F c{toCam.y * seg.z - toCam.z * seg.y, toCam.z * seg.x - toCam.x * seg.z,
                                         toCam.x * seg.y - toCam.y * seg.x};
                               const float cl = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                               if (cl < 1e-5f) continue;
                               const float h = data.projectileBlurWidth * 0.5f / cl;
                               c = {c.x * h, c.y * h, c.z * h};
                               // Alpha fades with each end's age (quad tint averages them).
                               const float alpha = std::max(0.0f, 1.0f - (now - 0.5f * (tail[i].second + tail[i + 1].second)) /
                                                                        data.projectileBlurLifetime);
                               r.drawTexturedQuad({a.x + c.x, a.y + c.y, a.z + c.z}, {b.x + c.x, b.y + c.y, b.z + c.z},
                                                  {b.x - c.x, b.y - c.y, b.z - c.z}, {a.x - c.x, a.y - c.y, a.z - c.z},
                                                   0, {blurColor.r, blurColor.g, blurColor.b, alpha}, 0, 0, 1, 1,
                                                   tracerAdditive);
                           }
                       }
                       if (dl > 1e-4f) {
                           dir = {dir.x / dl, dir.y / dl, dir.z / dl};
                           Point3F fromCam{materialPos.x - r.cameraPos.x, materialPos.y - r.cameraPos.y,
                                           materialPos.z - r.cameraPos.z};
                           Point3F c{fromCam.y * dir.z - fromCam.z * dir.y, fromCam.z * dir.x - fromCam.x * dir.z,
                                     fromCam.x * dir.y - fromCam.y * dir.x};
                           float cl = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                           if (cl < 1e-5f) { c = {dir.z, 0.0f, -dir.x}; cl = std::sqrt(c.x * c.x + c.z * c.z); }
                           if (cl < 1e-5f) { c = {1, 0, 0}; cl = 1.0f; }
                           c = {c.x / cl * halfWidth, c.y / cl * halfWidth, c.z / cl * halfWidth};
                           const Point3F a{materialPos.x - dir.x * length * 0.5f, materialPos.y - dir.y * length * 0.5f,
                                           materialPos.z - dir.z * length * 0.5f};
                           const Point3F b{materialPos.x + dir.x * length * 0.5f, materialPos.y + dir.y * length * 0.5f,
                                           materialPos.z + dir.z * length * 0.5f};
                           const uint32_t mainTexture = texture(0);
                           if (mainTexture != UINT32_MAX)
                               r.drawTexturedQuad({a.x + c.x, a.y + c.y, a.z + c.z}, {b.x + c.x, b.y + c.y, b.z + c.z},
                                                  {b.x - c.x, b.y - c.y, b.z - c.z}, {a.x - c.x, a.y - c.y, a.z - c.z},
                                                   mainTexture, {1, 1, 1, 1}, 0, 0, 1, 1, tracerAdditive);
                           const float fl = std::sqrt(fromCam.x * fromCam.x + fromCam.y * fromCam.y + fromCam.z * fromCam.z);
                           const float along = fl > 1e-5f
                               ? (dir.x * fromCam.x + dir.y * fromCam.y + dir.z * fromCam.z) / fl : 0.0f;
                           const uint32_t crossTexture = texture(1);
                           if (data.projectileRenderCross && crossTexture != UINT32_MAX &&
                               !(along > -data.projectileCrossViewAngle && along < data.projectileCrossViewAngle)) {
                               // A square of crossSize facing along the flight.
                               Point3F u = std::fabs(dir.y) < 0.9f ? Point3F{dir.z, 0.0f, -dir.x} : Point3F{1, 0, 0};
                               const float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
                               u = {u.x / ul, u.y / ul, u.z / ul};
                               const Point3F v{dir.y * u.z - dir.z * u.y, dir.z * u.x - dir.x * u.z, dir.x * u.y - dir.y * u.x};
                               const float h = data.projectileCrossSize * 0.5f;
                               const Point3F m = materialPos;
                               r.drawTexturedQuad({m.x + (-u.x - v.x) * h, m.y + (-u.y - v.y) * h, m.z + (-u.z - v.z) * h},
                                                  {m.x + (u.x - v.x) * h, m.y + (u.y - v.y) * h, m.z + (u.z - v.z) * h},
                                                  {m.x + (u.x + v.x) * h, m.y + (u.y + v.y) * h, m.z + (u.z + v.z) * h},
                                                  {m.x + (-u.x + v.x) * h, m.y + (-u.y + v.y) * h, m.z + (-u.z + v.z) * h},
                                                   crossTexture, {1, 1, 1, 1}, 0, 0, 1, 1, tracerAdditive);
                           }
                       }
                   }
                   // LinearFlareProjectile::renderObject (t2-mapper flare.ts): the
                   // bolt's DTS (drawn with the ghost shapes), then numFlares spikes
                   // drawn additively with flareModTexture; a bolt without a DTS
                   // gets two additive flareBaseTexture billboards instead.
                   const bool linearFlare = data.projectileMaterial ==
                       V12::DecodedDataBlock::ProjectileMaterial::LinearFlare;
                   if (linearFlare) {
                       const ColorF flareColor{data.projectileMaterialColor[0], data.projectileMaterialColor[1],
                                               data.projectileMaterialColor[2], 1.0f};
                       const float scaleX = data.hasProjectileScale ? data.projectileScale.x : 1.0f;
                       const uint32_t modTexture = texture(0), baseTexture = texture(1);
                       if (data.shapeFile.empty() && modTexture != UINT32_MAX && baseTexture != UINT32_MAX) {
                           r.drawSprite(materialPos, 1.2f * scaleX,
                                        {std::sqrt(flareColor.r), std::sqrt(flareColor.g), std::sqrt(flareColor.b), 1.0f},
                                        baseTexture, true);
                           r.drawSprite(materialPos, 0.6f * scaleX, {1, 1, 1, 1}, baseTexture, true);
                       }
                       if (baseTexture != UINT32_MAX && modTexture != UINT32_MAX && data.projectileFlareCount > 0) {
                           uint32_t state = (uint32_t)idx * 2654435761u + (uint32_t)(demoTime * 977.0f) + 1u;
                           auto random = [&]() {
                               state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                               return (float)(state & 0xffffff) / 16777216.0f;
                           };
                           const std::array<float, 3> sizes{data.projectileMaterialSizes[0],
                               data.projectileMaterialSizes[1], data.projectileMaterialSizes[2]};
                           auto& spikes = mg->flareSpikes;
                           if (spikes.size() != data.projectileFlareCount) {
                               spikes.clear();
                               for (uint32_t i = 0; i < data.projectileFlareCount && i < 64; ++i)
                                   spikes.push_back(FlareSpikes::spawn(sizes, random));
                           }
                           if (!demoMatchEnded) FlareSpikes::advance(spikes, demoInterpolationDt, sizes, random);
                           const auto tris = FlareSpikes::triangles(spikes);
                           for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                               Point3F p[3];
                               float uv[3][2];
                               ColorF colors[3];
                               for (int k = 0; k < 3; ++k) {
                                   const auto& v = tris[i + k];
                                   p[k] = {materialPos.x + v.x, materialPos.y + v.y, materialPos.z + v.z};
                                   uv[k][0] = v.u; uv[k][1] = v.v;
                                   colors[k] = {flareColor.r * v.shade, flareColor.g * v.shade, flareColor.b * v.shade, 1.0f};
                               }
                               r.drawTexturedTriangle(p, uv, colors, modTexture, true);
                           }
                       }
                   }
                   // FlareProjectile: an additive flareTexture sprite of the
                   // datablock size (t2-mapper tracer.ts createSpriteView).
                    if (data.projectileMaterial == V12::DecodedDataBlock::ProjectileMaterial::Flare) {
                       const uint32_t flareTexture = texture(0);
                       if (flareTexture != UINT32_MAX)
                           r.drawSprite(materialPos, data.projectileMaterialSizes[0], {1.0f, 0.9f, 0.5f, 1.0f},
                                         flareTexture, true);
                    }
                    if (!blendWasOn) glDisable(GL_BLEND);
                    glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB,
                                        (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
               }

                // TargetProjectile is the held targeting laser. Its datablock
                // and ghost endpoints carry the beam independently of the
                // server-side target markers, so render it as a live ribbon.
                if (ghostClassIs(g->className, "TargetProjectile") && g->hasBeam) {
                    const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                    const auto dataIt = g->hasDatablock
                        ? dataBlocks.find((uint32_t)g->datablockId) : dataBlocks.end();
                    const auto* beam = dataIt != dataBlocks.end() &&
                        dataIt->second.decoded.targetBeam.valid
                        ? &dataIt->second.decoded.targetBeam : nullptr;
                    if (!beam) continue;

                    Point3F start = Math::torquePointToYUp(
                        {g->beamStart.x, g->beamStart.y, g->beamStart.z});
                    if (g->linkSourceGhost >= 0) {
                        const GhostEntry* source = demoParser->getGhostTracker().getGhost(
                            g->linkSourceGhost);
                        const int slot = std::clamp(g->linkSourceSlot, 0, 7);
                        if (source && source->hasMuzzle[slot])
                            start = source->muzzlePos[slot];
                        else if (source)
                            start = Math::torquePointToYUp(
                                {source->renderPos.x, source->renderPos.y, source->renderPos.z});
                    }
                    const Point3F end = Math::torquePointToYUp(
                        {g->beamEnd.x, g->beamEnd.y, g->beamEnd.z});
                    const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
                    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (length <= 0.001f) continue;

                    auto texture = [&](size_t index) -> uint32_t {
                        if (index >= beam->textures.size() || beam->textures[index].empty()) return 0;
                        std::vector<uint32_t> frames;
                        std::vector<float> durations;
                        r.loadTextureFrames(beam->textures[index].c_str(), frames, durations);
                        return frames.empty() ? 0u : frames.front();
                    };
                    const ColorF color{beam->color[0], beam->color[1], beam->color[2], 1.0f};
                    auto ribbon = [&](float width, uint32_t tex, const ColorF& tint,
                                      float u0, float u1) {
                        const auto quad = projectileBeamQuad(start, end, r.cameraPos, width);
                        if (quad.size() != 4) return false;
                        r.drawTexturedQuad(quad[0], quad[1], quad[2], quad[3], tex,
                                           tint, u0, 0.0f, u1, 1.0f, true);
                        return true;
                    };
                    const uint32_t gradient = texture(0);
                    if (gradient) {
                        ribbon(std::max(beam->startWidth, 0.02f), gradient,
                               {color.r, color.g, color.b, 0.9f}, 0.0f, 1.0f);
                    } else {
                        r.drawLine(start, end, color);
                    }
                    const uint32_t pulse = texture(2);
                    if (pulse) {
                        const float u0 = -beam->pulseSpeed * demoTime * 0.5f;
                        const float u1 = u0 + length * std::max(beam->pulseLength, 0.01f);
                        ribbon(std::max(beam->pulseWidth, 0.02f), pulse,
                               {color.r, color.g, color.b, 0.8f}, u0, u1);
                    }
                    if (g->beamTruncated) {
                        if (const uint32_t flare = texture(1))
                            r.drawSprite(end, 0.35f, color, flare, true);
                        const Point3F screen = worldToScreen(end, r.viewMatrix(),
                            r.projectionMatrix(), r.config().width, r.config().height);
                        if (screen.z >= -1.0f && screen.z <= 1.0f &&
                            screen.x >= 0.0f && screen.x <= r.config().width &&
                            screen.y >= 0.0f && screen.y <= r.config().height) {
                            ScreenSpace2D screenSpace(r);
                            constexpr float markerRadius = 6.0f;
                            r.drawLine({screen.x, screen.y - markerRadius, 0},
                                       {screen.x + markerRadius, screen.y, 0}, color);
                            r.drawLine({screen.x + markerRadius, screen.y, 0},
                                       {screen.x, screen.y + markerRadius, 0}, color);
                            r.drawLine({screen.x, screen.y + markerRadius, 0},
                                       {screen.x - markerRadius, screen.y, 0}, color);
                            r.drawLine({screen.x - markerRadius, screen.y, 0},
                                       {screen.x, screen.y - markerRadius, 0}, color);
                            if (Font* font = r.getFont()) {
                                const std::string distance =
                                    std::to_string((int)std::lround(length)) + "m";
                                font->render(distance.c_str(), screen.x + markerRadius + 3.0f,
                                             screen.y - 7.0f, color, 1.0f);
                            }
                        }
                    }
                    continue;
                }

                // ELF / repair link beams (ELFProjectile, RepairProjectile
                // renderObject): an additive ribbon bowing from the source's
                // muzzle through muzzle + aim x length to the end point, its
                // texture scrolling with age, and an impact flare. ELF adds
                // three jittered lightning ribbons re-seeded at 15 Hz; the
                // repair end chases hits on the target along the aim ray.
                if (demoParser && (ghostClassIs(g->className, "ELFProjectile") ||
                                   ghostClassIs(g->className, "RepairProjectile"))) {
                    const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                    auto dataIt = g->hasDatablock ? dataBlocks.find((uint32_t)g->datablockId)
                                                  : dataBlocks.end();
                    const auto* link = dataIt != dataBlocks.end() && dataIt->second.decoded.linkBeam.valid
                        ? &dataIt->second.decoded.linkBeam : nullptr;
                    const GhostEntry* source = demoParser->getGhostTracker().getGhost(g->linkSourceGhost);
                    const GhostEntry* target = demoParser->getGhostTracker().getGhost(g->linkTargetGhost);
                    if (!link || !source || !target) continue;
                    const int slot = std::clamp(g->linkSourceSlot, 0, 7);
                    const Point3F start = source->hasMuzzle[slot] ? source->muzzlePos[slot]
                        : Math::torquePointToYUp({source->renderPos.x, source->renderPos.y, source->renderPos.z});
                    Point3F aim;
                    if (ObserverParity::isPlayerClass(source->className)) {
                        float maxLookAngle = 0.0f;
                        auto sourceData = source->hasDatablock
                            ? dataBlocks.find((uint32_t)source->datablockId) : dataBlocks.end();
                        if (sourceData != dataBlocks.end()) maxLookAngle = sourceData->second.decoded.playerMaxLookAngle;
                        aim = Math::torquePointToYUp(playerAimDirection(
                            source->bodyYaw, source->headYaw, source->headPitch, maxLookAngle));
                    } else if (source->hasMuzzle[slot]) {
                        aim = source->muzzleDir[slot];
                    } else {
                        const QuatF q(source->rotation.x, source->rotation.y, source->rotation.z, source->rotation.w);
                        aim = Math::torquePointToYUp(q.toMatrix().transformNormal({0, 1, 0}));
                    }
                    {
                        const float l = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
                        aim = l > 1e-6f ? Point3F{aim.x / l, aim.y / l, aim.z / l} : Point3F{0, 0, -1};
                    }
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    Point3F end;
                    if (link->elf) {
                        end = Math::torquePointToYUp({target->renderPos.x, target->renderPos.y, target->renderPos.z});
                        end.y += 1.0f;
                    } else {
                        // Cast the aim ray against the target's render shape.
                        const Point3F rayEnd{start.x + aim.x * link->beamRange,
                                             start.y + aim.y * link->beamRange,
                                             start.z + aim.z * link->beamRange};
                        float hitT = -1.0f;
                        if (target->shape && target->hasRenderModel) {
                            const float* box = nullptr;
                            if (ObserverParity::isPlayerClass(target->className) && target->hasDatablock) {
                                auto targetData = dataBlocks.find((uint32_t)target->datablockId);
                                if (targetData != dataBlocks.end() && targetData->second.decoded.isPlayerData)
                                    box = targetData->second.decoded.playerBoxSize;
                            }
                            // Player::castRay only hits an enabled (alive) player.
                            if (!box || target->damageState < 1)
                                hitT = raycastShape(*target->shape, target->renderModel, start, rayEnd, box);
                        }
                        if (mg->repairTarget != g->linkTargetGhost || now < mg->repairLastTime) {
                            mg->repairTarget = g->linkTargetGhost;
                            mg->repairHasHit = false;
                            mg->repairLastTime = -1.0f;
                        }
                        RepairEndpoint endpoint{mg->repairCurrent, mg->repairDesired, mg->repairHasHit};
                        const Point3F hit{start.x + (rayEnd.x - start.x) * hitT,
                                          start.y + (rayEnd.y - start.y) * hitT,
                                          start.z + (rayEnd.z - start.z) * hitT};
                        endpoint.step(hitT >= 0.0f, hit,
                                      mg->repairLastTime < 0.0f ? 0.0f : now - mg->repairLastTime);
                        mg->repairCurrent = endpoint.current;
                        mg->repairDesired = endpoint.desired;
                        mg->repairHasHit = endpoint.hasHit;
                        mg->repairLastTime = now;
                        if (!endpoint.hasHit ||
                            !repairWithinCutoff(start, endpoint.current, aim, link->cutoffAngle)) continue;
                        end = endpoint.current;
                    }
                    const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
                    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (length < 1e-6f) continue;
                    const Point3F control = linkBeamControl(start, aim, length);
                    auto textureFor = [&](const std::string& name) -> uint32_t {
                        if (name.empty()) return 0;
                        std::vector<uint32_t> frames;
                        std::vector<float> durations;
                        r.loadTextureFrames(name.c_str(), frames, durations);
                        return frames.empty() ? 0u : frames.front();
                    };
                    // Camera-facing ribbon through `count` samples.
                    auto ribbon = [&](auto&& sample, int count, float halfWidth, uint32_t texture,
                                      const ColorF& tint, float u0, float uLength) {
                        Point3F prevPos{}, prevCross{};
                        float prevU = 0.0f;
                        for (int i = 0; i < count; ++i) {
                            const float t = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
                            const Point3F p = sample(t);
                            const Point3F q = i == count - 1 ? sample(t - 0.5f / count) : sample(t + 0.5f / count);
                            const Point3F tangent = i == count - 1
                                ? Point3F{p.x - q.x, p.y - q.y, p.z - q.z} : Point3F{q.x - p.x, q.y - p.y, q.z - p.z};
                            const Point3F v{p.x - r.cameraPos.x, p.y - r.cameraPos.y, p.z - r.cameraPos.z};
                            Point3F c{v.y * tangent.z - v.z * tangent.y, v.z * tangent.x - v.x * tangent.z,
                                      v.x * tangent.y - v.y * tangent.x};
                            float cl = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                            if (cl < 1e-5f) { c = {0, 1, 0}; cl = 1.0f; }
                            c = {c.x / cl * halfWidth, c.y / cl * halfWidth, c.z / cl * halfWidth};
                            const float u = u0 + uLength * t;
                            if (i > 0)
                                r.drawTexturedQuad({prevPos.x + prevCross.x, prevPos.y + prevCross.y, prevPos.z + prevCross.z},
                                                   {p.x + c.x, p.y + c.y, p.z + c.z},
                                                   {p.x - c.x, p.y - c.y, p.z - c.z},
                                                   {prevPos.x - prevCross.x, prevPos.y - prevCross.y, prevPos.z - prevCross.z},
                                                   texture, tint, prevU, 0.0f, u, 1.0f, true);
                            prevPos = p; prevCross = c; prevU = u;
                        }
                    };
                    auto beamSample = [&](float t) { return linkBeamSample(start, control, end, t); };
                    const float age = std::max(0.0f, now - mg->spawnTime);
                    if (const uint32_t texture = textureFor(link->texture))
                        ribbon(beamSample, link->elf ? 16 : 20, link->width * 0.5f, texture,
                               {1, 1, 1, link->elf ? 1.0f : 0.75f}, -age * link->scrollSpeed,
                               length * link->texRepeat);
                    if (link->elf) {
                        if (const uint32_t lightning = textureFor(link->lightningTexture)) {
                            constexpr int points = 16;
                            const uint32_t seed = (uint32_t)(now * 15.0f) * 2654435761u + (uint32_t)idx * 40503u;
                            for (int strand = 0; strand < 3; ++strand) {
                                Point3F offsets[points]{};
                                uint32_t state = seed + (uint32_t)strand * 97u + 1u;
                                auto random = [&]() {
                                    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                                    return (float)(state & 0xffffff) / 8388608.0f - 1.0f;
                                };
                                for (int i = 1; i + 1 < points; ++i) {
                                    Point3F o{random(), random(), random()};
                                    const float ol = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z);
                                    const float k = ol > 1e-5f ? link->lightningDist / ol : 0.0f;
                                    offsets[i] = {o.x * k, o.y * k, o.z * k};
                                }
                                ribbon([&](float t) {
                                    Point3F p = beamSample(t);
                                    const int i = std::clamp((int)std::lround(t * (points - 1)), 0, points - 1);
                                    return Point3F{p.x + offsets[i].x, p.y + offsets[i].y, p.z + offsets[i].z};
                                }, points, link->lightningWidth * 0.5f, lightning, {1, 1, 1, 1}, 0.0f, 1.0f);
                            }
                        }
                    }
                    // Impact flare; the repair flare hides when viewed from behind.
                    if (const uint32_t flare = textureFor(link->flareTexture)) {
                        bool flareVisible = true;
                        if (!link->elf) {
                            Point3F toEnd{end.x - r.cameraPos.x, end.y - r.cameraPos.y, end.z - r.cameraPos.z};
                            const float te = std::sqrt(toEnd.x * toEnd.x + toEnd.y * toEnd.y + toEnd.z * toEnd.z);
                            const float dotBack = te > 1e-5f
                                ? (dx * toEnd.x + dy * toEnd.y + dz * toEnd.z) / (length * te) : 1.0f;
                            flareVisible = dotBack >= -0.75f;
                        }
                        if (flareVisible) r.drawSprite(end, link->elf ? 0.5f : 0.6f, {1, 1, 1, 1}, flare, true);
                    }
                    continue;
                }

                // SniperProjectile::renderObject: a camera-facing ribbon from
                // muzzle to impact for fadeTime seconds, alpha 1 - t. The core
                // (last texture) widens startWidth -> endWidth; the beamColor
                // overlay is 25% wider, its texture steps through the rip
                // sequence and its U scrolls the pulse toward the target.
                // Beams are drawn without fog.
                if (ghostClassIs(g->className, "SniperProjectile")) {
                    const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                    auto dataIt = g->hasDatablock ? dataBlocks.find((uint32_t)g->datablockId)
                                                  : dataBlocks.end();
                    const auto* beam = dataIt != dataBlocks.end() &&
                        dataIt->second.decoded.sniperBeam.valid &&
                        dataIt->second.decoded.sniperBeam.textures.size() >= 12
                        ? &dataIt->second.decoded.sniperBeam : nullptr;
                    if (!beam || !g->hasBeam) continue;
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    const float elapsed = now - mg->spawnTime;
                    const float t = elapsed / std::max(0.001f, beam->fadeTime);
                    if (t < 0.0f || t >= 1.0f) continue;
                    const Point3F a = Math::torquePointToYUp({g->beamStart.x, g->beamStart.y, g->beamStart.z});
                    const Point3F b = Math::torquePointToYUp({g->beamEnd.x, g->beamEnd.y, g->beamEnd.z});
                    Point3F dir{b.x - a.x, b.y - a.y, b.z - a.z};
                    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                    if (length < 1e-3f) continue;
                    dir = {dir.x / length, dir.y / length, dir.z / length};
                    const Point3F fromCam{a.x - r.cameraPos.x, a.y - r.cameraPos.y, a.z - r.cameraPos.z};
                    Point3F cross{fromCam.y * dir.z - fromCam.z * dir.y,
                                  fromCam.z * dir.x - fromCam.x * dir.z,
                                  fromCam.x * dir.y - fromCam.y * dir.x};
                    float crossLength = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
                    if (crossLength < 1e-4f) { cross = {dir.z, 0.0f, -dir.x}; crossLength = std::sqrt(cross.x * cross.x + cross.z * cross.z); }
                    if (crossLength < 1e-4f) { cross = {1, 0, 0}; crossLength = 1.0f; }
                    cross = {cross.x / crossLength, cross.y / crossLength, cross.z / crossLength};
                    auto texture = [&](size_t index) -> uint32_t {
                        std::vector<uint32_t> frames;
                        std::vector<float> durations;
                        r.loadTextureFrames(beam->textures[index].c_str(), frames, durations);
                        return frames.empty() ? 0u : frames.front();
                    };
                    auto quad = [&](float width, uint32_t tex, const ColorF& tint, float u0, float u1) {
                        const Point3F h{cross.x * width * 0.5f, cross.y * width * 0.5f, cross.z * width * 0.5f};
                        r.drawTexturedQuad({a.x + h.x, a.y + h.y, a.z + h.z}, {b.x + h.x, b.y + h.y, b.z + h.z},
                                           {b.x - h.x, b.y - h.y, b.z - h.z}, {a.x - h.x, a.y - h.y, a.z - h.z},
                                           tex, tint, u0, 0.0f, u1, 1.0f, false);
                    };
                    const float width = beam->startWidth + (beam->endWidth - beam->startWidth) * t;
                    const float alpha = 1.0f - t;
                    if (const uint32_t core = texture(11))
                        quad(width, core, {1, 1, 1, alpha}, 0.0f, 1.0f);
                    const size_t rip = 1 + (size_t)std::clamp((int)std::lround(t * 10.0f + 1.0f) - 1, 0, 10);
                    const float u0 = -beam->pulseSpeed * elapsed * 0.5f;
                    if (const uint32_t pulse = texture(rip))
                        quad(width * 1.25f, pulse, {beam->color[0], beam->color[1], beam->color[2], alpha},
                             u0, u0 + length * beam->pulseLength);
                    continue;
                }

                // ShockLanceProjectile (renderObject / advanceTime, t2-mapper
                // shockLance.ts), additive, unfogged. A pinned bolt keeps the
                // ghost's start/end and draws two textured strips widening
                // startWidth -> endWidth over zapDuration (alpha 0 at the muzzle,
                // 1 - age/zapDuration at the target, U scrolling boltSpeed x age
                // with texWrap repeats) and two lightning ribbons regenerated at
                // lightningFreq; a miss only sparks 0.2 m from the live muzzle.
                if (ghostClassIs(g->className, "ShockLanceProjectile") && g->hasBeam) {
                    const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                    auto dataIt = g->hasDatablock ? dataBlocks.find((uint32_t)g->datablockId)
                                                  : dataBlocks.end();
                    const auto* lance = dataIt != dataBlocks.end() && dataIt->second.decoded.shockLance.valid &&
                        !dataIt->second.decoded.shockLance.textures.empty()
                        ? &dataIt->second.decoded.shockLance : nullptr;
                    if (!lance) continue;
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    if (mg->shockFresh) {
                        mg->shockFresh = false;
                        mg->spawnTime = now;
                        mg->shockRegenTimer = 0.0f;
                        mg->shockLastTime = -1.0f;
                        mg->shockBolts[0].clear(); mg->shockBolts[1].clear();
                    }
                    const float zapDuration = std::max(1e-3f, lance->zapDuration);
                    const float age = now - mg->spawnTime;
                    if (age < 0.0f || age >= zapDuration) continue;
                    const float fade = 1.0f - age / zapDuration;
                    Point3F start = Math::torquePointToYUp({g->beamStart.x, g->beamStart.y, g->beamStart.z});
                    Point3F end = Math::torquePointToYUp({g->beamEnd.x, g->beamEnd.y, g->beamEnd.z});
                    float density = lance->lightningDensity, amp = lance->lightningAmp;
                    if (!g->beamHit) {
                        density = 20.0f; amp = 0.1f;
                        const GhostEntry* source = demoParser->getGhostTracker().getGhost(g->linkSourceGhost);
                        if (source) {
                            const int slot = std::clamp(g->linkSourceSlot, 0, 7);
                            if (source->hasMuzzle[slot]) start = source->muzzlePos[slot];
                            Point3F aim;
                            if (ObserverParity::isPlayerClass(source->className)) {
                                float maxLookAngle = 0.0f;
                                auto sourceData = source->hasDatablock
                                    ? dataBlocks.find((uint32_t)source->datablockId) : dataBlocks.end();
                                if (sourceData != dataBlocks.end()) maxLookAngle = sourceData->second.decoded.playerMaxLookAngle;
                                aim = Math::torquePointToYUp(playerAimDirection(
                                    source->bodyYaw, source->headYaw, source->headPitch, maxLookAngle));
                            } else {
                                aim = source->hasMuzzle[slot] ? source->muzzleDir[slot] : Point3F{0, 0, -1};
                            }
                            const float al = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
                            if (al > 1e-6f) aim = {aim.x / al, aim.y / al, aim.z / al};
                            end = {start.x + aim.x * 0.2f, start.y + aim.y * 0.2f, start.z + aim.z * 0.2f};
                        }
                    }
                    Point3F dir{end.x - start.x, end.y - start.y, end.z - start.z};
                    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                    if (length < 1e-4f) continue;
                    dir = {dir.x / length, dir.y / length, dir.z / length};
                    // Bolt frame: +X along the bolt.
                    Point3F axisY = std::fabs(dir.y) < 0.9f ? Point3F{0, 1, 0} : Point3F{1, 0, 0};
                    Point3F axisZ{dir.y * axisY.z - dir.z * axisY.y, dir.z * axisY.x - dir.x * axisY.z,
                                  dir.x * axisY.y - dir.y * axisY.x};
                    const float zl = std::sqrt(axisZ.x * axisZ.x + axisZ.y * axisZ.y + axisZ.z * axisZ.z);
                    axisZ = {axisZ.x / zl, axisZ.y / zl, axisZ.z / zl};
                    axisY = {axisZ.y * dir.z - axisZ.z * dir.y, axisZ.z * dir.x - axisZ.x * dir.z,
                             axisZ.x * dir.y - axisZ.y * dir.x};
                    auto toWorld = [&](const Point3F& p) {
                        return Point3F{start.x + dir.x * p.x + axisY.x * p.y + axisZ.x * p.z,
                                       start.y + dir.y * p.x + axisY.y * p.y + axisZ.y * p.z,
                                       start.z + dir.z * p.x + axisY.z * p.y + axisZ.z * p.z};
                    };
                    // Regenerate at lightningFreq once each period has elapsed.
                    float regenDt = mg->shockLastTime < 0.0f ? age : now - mg->shockLastTime;
                    if (regenDt < 0.0f || regenDt > 1.0f) regenDt = 0.0f;
                    mg->shockLastTime = now;
                    mg->shockRegenTimer += regenDt;
                    const float period = 1.0f / std::max(lance->lightningFreq, 1e-3f);
                    if (mg->shockRegenTimer >= period) {
                        mg->shockRegenTimer -= period;
                        uint32_t state = (uint32_t)(now * 1000.0f) * 2654435761u + (uint32_t)idx * 97u + 1u;
                        auto random = [&]() {
                            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                            return (float)(state & 0xffffff) / 16777216.0f;
                        };
                        for (auto& bolt : mg->shockBolts) bolt = shockLightningPoints(length, density, amp, random);
                    }
                    auto textureFor = [&](const std::string& name) -> uint32_t {
                        std::vector<uint32_t> frames;
                        std::vector<float> durations;
                        r.loadTextureFrames(name.c_str(), frames, durations);
                        return frames.empty() ? 0u : frames.front();
                    };
                    const uint32_t beamTexture = textureFor(lance->textures.back());
                    // A pinned bolt zaps the struck object (drawn with it).
                    if (g->beamHit && g->linkTargetGhost >= 0) {
                        ShockZapRequest& zap = shockZapRequests()[g->linkTargetGhost];
                        zap.spawnTime = mg->spawnTime;
                        zap.zapDuration = zapDuration;
                        if (zap.textures.empty())
                            for (const std::string& name : lance->textures) zap.textures.push_back(textureFor(name));
                    }
                    // Lightning ribbons; the engine sides each local point against
                    // the world camera position.
                    const float halfLightning = lance->lightningWidth * 0.5f;
                    for (const auto& bolt : mg->shockBolts) {
                        if (bolt.size() < 2) continue;
                        Point3F prevA{}, prevB{};
                        for (size_t i = 0; i < bolt.size(); ++i) {
                            const Point3F& p = bolt[i];
                            Point3F seg = i + 1 == bolt.size()
                                ? Point3F{p.x - bolt[i - 1].x, p.y - bolt[i - 1].y, p.z - bolt[i - 1].z}
                                : Point3F{bolt[i + 1].x - p.x, bolt[i + 1].y - p.y, bolt[i + 1].z - p.z};
                            const float sl = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
                            if (sl * sl > 1e-4f) seg = {seg.x / sl, seg.y / sl, seg.z / sl};
                            const Point3F toCam{p.x - r.cameraPos.x, p.y - r.cameraPos.y, p.z - r.cameraPos.z};
                            Point3F side{toCam.y * seg.z - toCam.z * seg.y, toCam.z * seg.x - toCam.x * seg.z,
                                         toCam.x * seg.y - toCam.y * seg.x};
                            const float sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
                            if (sideLength * sideLength > 1e-4f)
                                side = {side.x / sideLength, side.y / sideLength, side.z / sideLength};
                            side = {side.x * halfLightning, side.y * halfLightning, side.z * halfLightning};
                            const Point3F a = toWorld({p.x - side.x, p.y - side.y, p.z - side.z});
                            const Point3F b = toWorld({p.x + side.x, p.y + side.y, p.z + side.z});
                            if (i > 0) {
                                const float u0 = (float)((i - 1) & 1), u1 = (float)(i & 1);
                                r.drawTexturedQuad(prevB, b, a, prevA, beamTexture, {1, 1, 1, fade},
                                                   u0, 0.0f, u1, 1.0f, true);
                            }
                            prevA = a; prevB = b;
                        }
                    }
                    if (g->beamHit) {
                        const Point3F fromCam{start.x - r.cameraPos.x, start.y - r.cameraPos.y, start.z - r.cameraPos.z};
                        Point3F right{fromCam.y * dir.z - fromCam.z * dir.y, fromCam.z * dir.x - fromCam.x * dir.z,
                                      fromCam.x * dir.y - fromCam.y * dir.x};
                        const float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
                        if (rl > 1e-4f) right = {right.x / rl, right.y / rl, right.z / rl};
                        for (int i = 0; i < 2; ++i) {
                            const float halfWidth = 0.5f * (lance->startWidth[i] +
                                (lance->endWidth[i] - lance->startWidth[i]) / zapDuration * age);
                            const Point3F h{right.x * halfWidth, right.y * halfWidth, right.z * halfWidth};
                            const float u0 = lance->boltSpeed[i] * age;
                            const ColorF colors[4] = {{1, 1, 1, 0}, {1, 1, 1, fade}, {1, 1, 1, fade}, {1, 1, 1, 0}};
                            r.drawTexturedQuadColors({start.x + h.x, start.y + h.y, start.z + h.z},
                                                     {end.x + h.x, end.y + h.y, end.z + h.z},
                                                     {end.x - h.x, end.y - h.y, end.z - h.z},
                                                     {start.x - h.x, start.y - h.y, start.z - h.z},
                                                     beamTexture, colors, u0, 0.0f, u0 - lance->texWrap[i], 1.0f, true);
                        }
                    }
                    continue;
                }

            // Resolve the shape against the ghost's current datablock. Player
            // armor switches replace PlayerData on the ghost while preserving
            // its id, so a one-time shape load leaves the old armor visible.
            DTSShape* shape = const_cast<DTSShape*>(g->shape);
            if (!isEffectOnlyGhostClass(g->className) && !g->className.empty()) {
                GhostEntry* mutableG = const_cast<GhostEntry*>(g);
                std::string shapeRef = g->shapeName;
                if (g->hasDatablock && demoParser) {
                    const auto& dataBlocks = demoParser->getInitialBlock().dataBlocks;
                    auto dataBlock = dataBlocks.find((uint32_t)g->datablockId);
                    if (dataBlock != dataBlocks.end() &&
                        !dataBlock->second.decoded.shapeFile.empty())
                        shapeRef = normalizeShapePath(dataBlock->second.decoded.shapeFile);
                }
                const int datablockId = g->hasDatablock ? g->datablockId : -1;
                const bool identityChanged = mutableG->renderedDatablockId != datablockId ||
                    mutableG->renderedSkinName != g->skinName ||
                    mutableG->renderedShapeRef != shapeRef;
                if (!shape || identityChanged) {
                    if (mutableG->renderedDatablockId >= 0 &&
                        mutableG->renderedDatablockId != datablockId)
                        Console::instance().printf(LogLevel::Debug,
                            "Demo ghost[%d] armor datablock %d -> %d, shape '%s'",
                            idx, mutableG->renderedDatablockId, datablockId,
                            shapeRef.c_str());
                    mutableG->shape = getOrLoadDemoShape(g->className, g->skinName, shapeRef);
                    mutableG->renderedDatablockId = datablockId;
                    mutableG->renderedSkinName = g->skinName;
                    mutableG->renderedShapeRef = shapeRef;
                    mutableG->skinApplied = false;
                    mutableG->footClip = nullptr;
                    mutableG->footPhase = 0.0f;
                    mutableG->footState = 0;
                    mutableG->threadAnimTime = 0.0f;
                    shape = mutableG->shape;
                }
            }

            // Players and vehicles cast projected shadows; a mounted object's
            // shadow belongs to its vehicle.
            static std::vector<ShadowCaptureDraw> shadowDraws;
            shadowDraws.clear();
            bool shadowCaster = false;
            Point3F shadowCenter{};
            float shadowRadius = 0.0f;
            // A shocklance zap redraws the struck object's own meshes (not its
            // mounted images): the draws captured before the images.
            bool zapTarget = false;
            Point3F zapOrigin{};
            size_t zapDrawCount = 0;
            if (shape && shape->loaded) {
                // Apply skin textures on first render (player ghosts only)
                if (!mg->skinApplied && !g->skinName.empty() &&
                    ObserverParity::isPlayerClass(g->className)) {
                    if (shape->applySkin(g->skinName))
                        Console::instance().printf(LogLevel::Debug,
                            "Ghost[%d]: applied skin '%s' to shape '%s'",
                            idx, g->skinName.c_str(), shape->name.c_str());
                    mg->skinApplied = true;
                }

                // Build model matrix
                bool isPlayer = ObserverParity::isPlayerClass(g->className);
                // The recorder's own player turns with its moves (the server
                // sends its view in the control packet, not ghost updates).
                const bool recorderView = isPlayer && idx == controlGhostIndex && demoHasOrientation &&
                                          g->mountObject < 0;
                if (recorderView) {
                    const float half = demoViewYaw * 0.5f;
                    mg->renderRotation = {0.0f, 0.0f, -std::sin(half), std::cos(half)};
                    mg->headPitch = std::clamp(demoViewPitch / (Math::PI * 0.494f), -1.0f, 1.0f);
                }
                MatrixF model;
                if (g->hasRotation || recorderView) {
                    QuatF q(mg->renderRotation.x, mg->renderRotation.y, mg->renderRotation.z, mg->renderRotation.w);
                    model = Math::torqueQuaternionToYUp(q);
                } else if (mg->isMoving || isPlayer) {
                    float yaw = mg->moveYaw;
                    model.setRotationAxis({0, 1, 0}, -yaw);
                } else {
                    model.identity();
                }
                 if (shape->nativeDTS) {
                     model = model * Math::nativeDtsFrame();
                 }
                 if (isProjectile && mg->hasProjectileScale)
                     model.setScale({mg->projectileScale.x, mg->projectileScale.y,
                                     mg->projectileScale.z});
                Point3F renderPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                model.setTranslation(renderPosition);
                // ShapeBase::getMountTransform: a mounted object sits in its
                // parent's mountN node frame (the parent's last drawn pose).
                if (g->mountObject >= 0) {
                    auto parent = demoMountFrames.find(g->mountObject);
                    if (parent != demoMountFrames.end() && (parent->second.valid >> g->mountNode) & 1u) {
                        model = parent->second.frame[g->mountNode];
                        renderPosition = {model.m[0][3], model.m[1][3], model.m[2][3]};
                    }
                }
                r.setModel(model * shape->upOrientation());
                {
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = g->hasDatablock ? blocks.find((uint32_t)g->datablockId) : blocks.end();
                    shape->emapEnabled = block != blocks.end() && block->second.decoded.shapeEmap;
                }
                {
                    // The world box centre (the shape bounds' centre, placed).
                    Point3F localCenter = shape->boundsCenter();
                    if (shape->hasHeaderBounds) {
                        const Point3F& lo = shape->headerBoundsMin;
                        const Point3F& hi = shape->headerBoundsMax;
                        localCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                    }
                    demoBoxCenters[idx] = (model * shape->upOrientation()).transform(localCenter);
                }
                shadowCaster = (isPlayer || ObserverParity::isVehicleClass(g->className)) &&
                               g->mountObject < 0;
                zapTarget = shockZapRequests().count(idx) != 0;
                zapOrigin = renderPosition;
                if (zapTarget) r.shadowCapture = &shadowDraws;
                if (shadowCaster) {
                    // TSShape::bounds: centre and half diagonal.
                    const MatrixF shapeModel = model * shape->upOrientation();
                    Point3F localCenter = shape->boundsCenter();
                    shadowRadius = shape->boundsRadius();
                    if (shape->hasHeaderBounds) {
                        const Point3F& lo = shape->headerBoundsMin;
                        const Point3F& hi = shape->headerBoundsMax;
                        localCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                        shadowRadius = 0.5f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) +
                                                        (hi.y - lo.y) * (hi.y - lo.y) +
                                                        (hi.z - lo.z) * (hi.z - lo.z));
                    }
                    shadowCenter = shapeModel.transform(localCenter);
                    r.shadowCapture = &shadowDraws;
                }

                // Appearance comes from native material and skin data only.
                if (defShader) defShader->setUniform("uTint", ColorF{1, 1, 1, 1});

                // ShapeBase replaces the normal material map with the engine's
                // special/cloakTexture.
                Texture* cloakTexture = nullptr;
                // ShapeBase::updateCloak: mCloakLevel ramps 0 -> 1 over 0.5 s.
                if (!demoMatchEnded)
                    mg->cloakLevel = std::clamp(mg->cloakLevel + (g->cloaked ? 1.0f : -1.0f) *
                                                demoInterpolationDt / 0.5f, 0.0f, 1.0f);
                if (mg->cloakLevel > 0.0f) cloakTexture = r.loadTexture("textures/special/cloakTexture.png");
                shape->cloakTextureOverride = cloakTexture && cloakTexture->loaded ? cloakTexture : nullptr;

                // ShapeBase::renderObject: vertex alpha 0.125 + (1 - level) x 0.875.
                shape->alphaScale = mg->cloakLevel > 0.0f ? 0.125f + (1.0f - mg->cloakLevel) * 0.875f : 1.0f;
                // mFadeVal: a timed fade runs from its receipt; the shape's
                // alpha scales by it (0 draws nothing).
                {
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    if (mg->fading) {
                        if (mg->fadeFresh) { mg->fadeStart = now; mg->fadeFresh = false; }
                        const float t = mg->fadeTime > 0.0f ? (now - mg->fadeStart) / mg->fadeTime : 1.0f;
                        if (t >= 1.0f || t < 0.0f) {
                            mg->fadeVal = mg->fadeOut ? 0.0f : 1.0f;
                            mg->fading = false;
                        } else {
                            mg->fadeVal = mg->fadeOut ? 1.0f - t : t;
                        }
                    }
                    if (!mg->cloaked) shape->alphaScale *= mg->fadeVal;
                    // ShapeBase::renderObject: a Destroyed shape whose datablock
                    // clears renderWhenDestroyed, or a blown-apart player,
                    // is not drawn.
                    if (g->hasDatablock) {
                        const auto& shapeBlocks = demoParser->getInitialBlock().dataBlocks;
                        auto shapeBlock = shapeBlocks.find((uint32_t)g->datablockId);
                        if (shapeBlock != shapeBlocks.end() &&
                            ((g->damageState == 2 && !shapeBlock->second.decoded.shapeRenderWhenDestroyed) ||
                             (g->blowApart && ObserverParity::isPlayerClass(g->className))))
                            shape->alphaScale = 0.0f;
                    }
                    // Item::registerLights: the ItemData light at the world
                    // box centre, skipped for a lightOnlyStatic item that is
                    // not static; pulsing lights follow sin(pi t / lightTime).
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = g->hasDatablock ? blocks.find((uint32_t)g->datablockId) : blocks.end();
                    if (block != blocks.end() && ghostClassIs(g->className, "Item")) {
                        const auto& data = block->second.decoded;
                        if (data.shapeLightType > 0 && data.shapeLightRadius > 0.0f &&
                            !(data.shapeLightOnlyStatic && !g->itemStatic)) {
                            float intensity = mg->fadeVal;
                            if (data.shapeLightType == 2 && data.shapeLightTimeMS > 0) {
                                const float pulse = 0.5f + 0.5f * std::sin(Math::PI * now * 1000.0f /
                                                                           (float)data.shapeLightTimeMS);
                                intensity *= 0.15f + pulse * 0.85f;
                            }
                            Point3F localCenter = shape->boundsCenter();
                            if (shape->hasHeaderBounds) {
                                const Point3F& lo = shape->headerBoundsMin;
                                const Point3F& hi = shape->headerBoundsMax;
                                localCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                            }
                            const Point3F at = (model * shape->upOrientation()).transform(localCenter);
                            const auto& c = data.shapeLightColor;
                            if (intensity > 0.0f)
                                demoLights.push_back({at.x, at.y, at.z, c[0] * intensity, c[1] * intensity,
                                                      c[2] * intensity, data.shapeLightRadius});
                        }
                    }
                }

                // Select the sequence by the index transmitted in the
                // ShapeBase thread state. The DTS owns its sequence names.
                const DTSShape::Animation* animation = nullptr;
                float animationPosition = 0.0f;
                bool isTurret = isTurretGhostClass(g->className);
                // Players animate from the PlayerData action table: the
                // server's wired action (deaths, taunts) or the client-picked
                // movement action.
                const bool playerAnimated = isPlayer && !shape->actionTable.empty();
                const float animationNow = demoMatchEnded ? demoMatchEndedAt : demoTime;
                float animationPhase = -1.0f; // unwrapped for cyclic clips
                if (playerAnimated) {
                    const auto& table = shape->actionTable;
                    auto tableAnimation = [&](int action) -> int {
                        return action >= 0 && action < (int)table.size() ? table[action] : -1;
                    };
                    float actionEnd = -1.0f;
                    if (g->actionAnim >= 0 &&
                        (g->actionAnim >= PlayerAnimation::NumTableActions || g->damageState >= 1)) {
                        const int index = tableAnimation(g->actionAnim);
                        if (index >= 0 && index < (int)shape->animations.size()) {
                            const auto& clip = shape->animations[index];
                            const float position = PlayerAnimation::sampleActionPosition(
                                g->actionAnimPos, g->actionAtEnd, g->actionTime, animationNow, clip.duration);
                            actionEnd = g->actionTime + clip.duration *
                                (1.0f - (g->actionAtEnd ? 1.0f : g->actionAnimPos));
                            const bool holds = g->actionHoldAtEnd || g->mountObject >= 0 ||
                                g->damageState >= 1;
                            if (position < 1.0f || holds) {
                                animation = &clip;
                                animationPosition = position;
                                animationPhase = position;
                            }
                        }
                    }
                    if (!animation) {
                        const int action = g->moveAnimValid ? g->moveAction
                                                            : (int)PlayerAnimation::Root;
                        const int index = tableAnimation(action);
                        if (index >= 0 && index < (int)shape->animations.size()) {
                            const auto& clip = shape->animations[index];
                            const float start = std::max(g->moveStartTime, actionEnd);
                            const float cycles = clip.duration > 0.0f
                                ? std::max(0.0f, animationNow - start) * g->moveTimeScale / clip.duration
                                : 0.0f;
                            animation = &clip;
                            animationPosition = clip.looping
                                ? cycles - std::floor(cycles)
                                : std::clamp(cycles, 0.0f, 1.0f);
                            animationPhase = clip.looping ? cycles : animationPosition;
                        }
                    }
                }
                // Player::updateActionThread: a foot trigger (1 left, 2 right)
                // drops a foot puff and footprint where the foot meets terrain.
                if (playerAnimated && animation && !demoMatchEnded && w && g->hasDatablock) {
                    if (animation != mg->footClip) {
                        mg->footState = DTSTriggers::advance(animation->triggers, 0.0f, animationPhase,
                                                             animation->looping, mg->footState);
                        mg->footClip = animation;
                    } else {
                        mg->footState = DTSTriggers::advance(animation->triggers, mg->footPhase, animationPhase,
                                                             animation->looping, mg->footState);
                    }
                    mg->footPhase = animationPhase;
                                        const uint32_t foot = (mg->footState & 1) ? 1u : (mg->footState & 2) ? 2u : 0u;
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = blocks.find((uint32_t)g->datablockId);
                    const MatrixF shapeFrame = model * shape->upOrientation();
                    if (foot && block != blocks.end() && block->second.decoded.isPlayerData) {
                        mg->footState &= ~foot;
                        const auto& data = block->second.decoded;
                        const bool left = foot == 1;
                        const float offset = (left ? -1.0f : 1.0f) * data.playerDecalOffset;
                        // The DTS loader's local frame swaps Torque's y and z.
                        const Point3F pos = shapeFrame.transform({offset, 0.0f, 0.0f});
                        Point3F forward = shapeFrame.transformNormal({0.0f, 0.0f, 1.0f});
                        const float fl = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
                        if (fl > 1e-6f) forward = {forward.x / fl, forward.y / fl, forward.z / fl};
                        // A ray from 0.01 above the foot to 2 below, against
                        // terrain and interiors (the nearer hit wins).
                        const Point3F start{pos.x, pos.y + 0.01f, pos.z};
                        constexpr float rayLength = 2.01f;
                        const auto* terrain = w->terrain();
                        float terrainT = -1.0f, height = 0.0f;
                        if (terrain && terrain->loaded && !terrain->isEmptySquare(pos.x, pos.z)) {
                            height = w->getHeight(pos.x, pos.z);
                            const float drop = start.y - height;
                            if (drop >= 0.0f && drop <= rayLength) terrainT = drop / rayLength;
                        }
                        float interiorDistance; Point3F hitPos, hitNormal;
                        const bool interiorHit = w->collision().raycast(start, {0, -1, 0}, rayLength,
                                                                        interiorDistance, hitPos, hitNormal);
                        const bool terrainFirst = terrainT >= 0.0f &&
                            (!interiorHit || terrainT * rayLength <= interiorDistance);
                        // mWaterCoverage: the share of the player box (bottom at
                        // the foot's height) under liquid. Torque (x, y) is (pos.x, -pos.z).
                        const float waterLevel = w->waterSurfaceAt(pos.x, -pos.z);
                        const float boxHeight = std::max(0.001f, data.playerBoxSize[2]);
                        const float waterCoverage = std::isfinite(waterLevel)
                            ? std::clamp((waterLevel - pos.y) / boxHeight, 0.0f, 1.0f) : 0.0f;
                        int sound = -1;
                        if (terrainFirst) {
                            const MaterialProperties* material = !terrain->textureNames.empty()
                                ? MaterialPropertyMap::instance().terrain(terrain->textureNames[0]) : nullptr;
                            if (material) {
                                sound = material->sound;
                                if (terrainT <= 0.5f && waterCoverage == 0.0f) {
                                    const Point3F normal = terrainNormalFromHeights(
                                        w->getHeight(pos.x - 1.0f, pos.z), w->getHeight(pos.x + 1.0f, pos.z),
                                        w->getHeight(pos.x, pos.z - 1.0f), w->getHeight(pos.x, pos.z + 1.0f));
                                    if (data.playerFootPuffEmitter)
                                        w->addFootPuff(pos, data.playerFootPuffEmitter, data.playerFootPuffRadius,
                                                       data.playerFootPuffNumParts, material->puffColor, blocks);
                                    if (data.playerDecalData)
                                        w->addFootprint({pos.x, height, pos.z}, normal, forward,
                                                        data.playerDecalData, blocks);
                                }
                            }
                        }
                        // Player::playFootstepSound. Retail PlayerData sounds
                        // lead with jetSound and wetJetSound, so the foot
                        // sounds (soft, hard, metal, snow, then shallow,
                        // wading, underwater, bubbles; left then right) start at 2.
                        auto& audio = Engine::instance().audio();
                        if ((terrainFirst || interiorHit) && audio.isInitialized()) {
                            const Point3F soundPos = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                            auto play = [&](int slot) {
                                const int index = 2 + slot * 2 + (left ? 0 : 1);
                                if (index < (int)data.playerSounds.size())
                                    playNativeAudioProfile(audio, blocks, data.playerSounds[index], soundPos);
                            };
                            if (waterCoverage == 0.0f) play(sound >= 0 && sound <= 3 ? sound : 1);
                            else if (waterCoverage < data.playerFootSplashHeight) play(4);
                            else if (waterCoverage < 1.0f) play(5);
                            else { play(6); play(7); }
                        }
                    }
                    // Player::updateJetting: jet dust on the terrain below
                    // (0.3 behind, a 2 m terrain-only ray, 0.3 above the hit).
                    const bool jetting = mg->prediction.initialized ? mg->prediction.jetting : g->jetting;
                    if (jetting && block != blocks.end() && block->second.decoded.playerDustEmitter &&
                        w->terrain() && w->terrain()->loaded) {
                        Point3F p = shapeFrame.transform({0.0f, 0.0f, -0.3f});
                        p.y = Math::torquePointToYUp({g->position.x, g->position.y, g->position.z}).y;
                        const float height = w->getHeight(p.x, p.z);
                        if (!w->terrain()->isEmptySquare(p.x, p.z) && p.y >= height && p.y - height <= 2.0f) {
                            const Point3F normal = terrainNormalFromHeights(
                                w->getHeight(p.x - 1.0f, p.z), w->getHeight(p.x + 1.0f, p.z),
                                w->getHeight(p.x, p.z - 1.0f), w->getHeight(p.x, p.z + 1.0f));
                            w->syncNodeEmitter((int64_t)idx * 16 + 15, block->second.decoded.playerDustEmitter,
                                               {p.x, height + 0.3f, p.z}, {0, 0, 0}, normal, blocks);
                        }
                    }
                }
                // Every live shape thread plays, each on its own clock: the
                // first as the primary sequence, the others as layers.
                DTSShape::BlendThread threadLayers[4];
                int numThreadLayers = 0;
                for (int ti = 0; ti < 4 && !playerAnimated; ++ti) {
                    const auto& thread = g->threads[ti];
                    if (!thread.valid || thread.sequence < 0 ||
                        thread.sequence >= (int)shape->animations.size()) continue;
                    if (thread.state == ShapeThreads::Destroy) continue;
                    const auto& clip = shape->animations[thread.sequence];
                    const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                    const float time = ShapeThreads::timeAt(mg->threadClocks[ti],
                        {thread.sequence, thread.state, thread.forward, thread.atEnd},
                        now, clip.duration, clip.looping);
                    if (!animation) {
                        animation = &clip;
                        animationPosition = clip.duration > 0.0f ? time / clip.duration : 0.0f;
                    } else {
                        threadLayers[numThreadLayers++] = {thread.sequence, time};
                    }
                }

                // Node overrides for turret barrel and player head
                 DTSShape::NodeOverride overrides[8];
                 int numOverrides = 0;
                 appendWheelNodeOverrides(*g, *shape, dt, overrides, numOverrides,
                                          8, mg->wheelRotation);
                 // Player head aim direction
                // Absolute head overrides would detach the head from an
                // animated body; animated players need the head/look blend
                // sequences instead.
                if (isPlayer && shape && !playerAnimated &&
                    (mg->headPitch != 0.0f || mg->headYaw != 0.0f)) {
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

                // Arm and head aim ride on blend threads: the arm action
                // (default "look") and "head" follow head pitch, "headside"
                // follows head yaw. Dead players drop them.
                DTSShape::BlendThread blends[12];
                int numBlends = 0;
                if (playerAnimated && g->damageState < 1) {
                    auto animationIndex = [&](const char* name) -> int {
                        const DTSShape::Animation* found = findAnimation(*shape, name);
                        return found ? (int)(found - shape->animations.data()) : -1;
                    };
                    auto addBlend = [&](int index, float position) {
                        if (index < 0 || index >= (int)shape->animations.size()) return;
                        const auto& clip = shape->animations[index];
                        if (!clip.blend) return;
                        blends[numBlends++] = {index,
                            std::clamp(position, 0.0f, 1.0f) * clip.duration};
                    };
                    const float pitchPosition = (mg->headPitch + 1.0f) * 0.5f;
                    const float yawPosition = (mg->headYaw + 1.0f) * 0.5f;
                    int arm = -1;
                    if (g->armAction >= 0 && g->armAction < (int)shape->actionTable.size())
                        arm = shape->actionTable[g->armAction];
                    if (arm < 0) arm = animationIndex("look");
                    addBlend(arm, pitchPosition);
                    addBlend(animationIndex("head"), pitchPosition);
                    addBlend(animationIndex("headside"), yawPosition);
                }
                // Jet flare (Player::processTick): the non-cyclic "jetflare"
                // sequence runs forward while jetting and back otherwise, so
                // its visibility fades the flare meshes in and out.
                if (playerAnimated && numBlends < 4) {
                    const DTSShape::Animation* flare = findAnimation(*shape, "jetflare");
                    if (flare && flare->duration > 0.0f) {
                        const bool jetOn = g->jetting && g->damageState < 1;
                        if (!demoMatchEnded) {
                            const float step = demoInterpolationDt / flare->duration;
                            mg->jetFlarePosition = std::clamp(
                                mg->jetFlarePosition + (jetOn ? step : -step), 0.0f, 1.0f);
                        }
                        blends[numBlends++] = {(int)(flare - shape->animations.data()),
                                               mg->jetFlarePosition * flare->duration};
                    }
                }
                // FlyingVehicle/HoverVehicle::updateJet: the back jets' and the
                // bottom jets' Activate/Maintain threads.
                const bool isJetVehicle = ghostClassIs(g->className, "FlyingVehicle") ||
                                          ghostClassIs(g->className, "HoverVehicle");
                if (isJetVehicle) {
                    const float jetDt = demoMatchEnded ? 0.0f : demoInterpolationDt;
                    auto driveDirection = [&](VehicleJets::Direction& d, bool active,
                                              const char* activateName, const char* maintainName) {
                        const DTSShape::Animation* activate = findAnimation(*shape, activateName);
                        const DTSShape::Animation* maintain = findAnimation(*shape, maintainName);
                        if (!activate && !maintain) return;
                        VehicleJets::step(d, active, jetDt, activate ? activate->duration : 0.0f,
                                          maintain != nullptr, animationNow);
                        if (activate && numBlends < 4)
                            blends[numBlends++] = {(int)(activate - shape->animations.data()),
                                                   d.activatePosition * activate->duration};
                        if (d.maintaining && maintain && numBlends < 4)
                            blends[numBlends++] = {(int)(maintain - shape->animations.data()),
                                                   std::max(0.0f, animationNow - d.maintainStart)};
                    };
                    driveDirection(mg->jetBack, VehicleJets::backActive(g->thrustDirection),
                                   "activateback", "maintainback");
                    driveDirection(mg->jetBottom,
                                   VehicleJets::bottomActive(g->thrustDirection, g->vehicleJetting),
                                   "activatebot", "maintainbot");
                }
                // ShapeBase's position-controlled Damage and Visibility (hulk)
                // threads (t2-mapper dtsDamage.ts): damage at the damage level
                // 1 - health, which ShapeBase clears on destruction and Player
                // keeps on corpses; visibility at 1 once destroyed.
                {
                    const float level = std::clamp(1.0f - HudParity::resourceFraction(g->health, g->maxHealth),
                                                   0.0f, 1.0f);
                    auto addPositioned = [&](const char* name, float position) {
                        const DTSShape::Animation* clip = findAnimation(*shape, name);
                        if (!clip || numBlends >= 12) return;
                        blends[numBlends++] = {(int)(clip - shape->animations.data()),
                                               std::clamp(position, 0.0f, 1.0f) * clip->duration};
                    };
                    addPositioned("damage", !isPlayer && level >= 1.0f && g->damageState == 2 ? 0.0f : level);
                    addPositioned("visibility", g->damageState == 2 ? 1.0f : 0.0f);
                    // Turret::processTick: activate while activation is non-zero;
                    // turn (phi / 360) and elevate (theta / 180) once fully
                    // active (t2-mapper turretAim.ts).
                    if (isTurret && g->hasTurretAim) {
                        float thetaMin = 45.0f, thetaMax = 135.0f;
                        if (g->hasDatablock) {
                            const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                            auto block = blocks.find((uint32_t)g->datablockId);
                            if (block != blocks.end() && block->second.decoded.hasTurretTheta) {
                                thetaMin = block->second.decoded.turretThetaMin;
                                thetaMax = block->second.decoded.turretThetaMax;
                            }
                        }
                        if (g->turretActivation != 0.0f) addPositioned("activate", g->turretActivation);
                        if (g->turretActivation >= 1.0f) {
                            const float theta = thetaMin + (thetaMax - thetaMin) * g->turretTheta;
                            addPositioned("elevate", theta / 180.0f);
                            addPositioned("turn", g->turretPhi - std::floor(g->turretPhi));
                        }
                    }
                }
                for (int i = 0; i < numThreadLayers && numBlends < 12; ++i)
                    blends[numBlends++] = threadLayers[i];
                // A shape with no running sequence of its own renders its
                // first layered thread as the primary.
                int primaryBlend = 0;
                if (!animation && numBlends > 0) {
                    animation = &shape->animations[blends[0].animationIndex];
                    animationPosition = animation->duration > 0.0f
                        ? blends[0].time / animation->duration : 0.0f;
                    primaryBlend = 1;
                }
                mg->renderModel = model * shape->upOrientation();
                mg->hasRenderModel = true;
                w->applyShapeLighting(*shape, mg->shapeLight, model * shape->upOrientation(),
                                      dt * 1000.0f);
                if (animation) {
                    const int animationIndexNow = (int)(animation - shape->animations.data());
                    const float animationTimeNow = animationPosition * animation->duration;
                    // Player::setActionThread transitions to a new action over 0.25 s
                    // from the frozen outgoing pose.
                    if (isPlayer) {
                        const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                        if (mg->animLastIndex >= 0 && mg->animLastIndex != animationIndexNow &&
                            now >= mg->animChangedAt) {
                            mg->animPrevIndex = mg->animLastIndex;
                            mg->animPrevTime = mg->animLastTime;
                            mg->animChangedAt = now;
                        }
                        if (now < mg->animChangedAt) mg->animPrevIndex = -1; // seek backwards
                        mg->animLastIndex = animationIndexNow;
                        mg->animLastTime = animationTimeNow;
                        const float weight = std::clamp((now - mg->animChangedAt) / 0.25f, 0.0f, 1.0f);
                        if (mg->animPrevIndex >= 0 && weight < 1.0f)
                            shape->transition = {mg->animPrevIndex, mg->animPrevTime, weight};
                    }
                    shape->renderAnimationIndex(animationIndexNow, animationTimeNow,
                                                numOverrides > 0 ? overrides : nullptr,
                                                numOverrides, blends + primaryBlend,
                                                numBlends - primaryBlend, DTSShape::SelectDetail);
                    shape->transition = {};
                } else {
                    shape->render(DTSShape::SelectDetail, numOverrides > 0 ? overrides : nullptr,
                                  numOverrides);
                }
                shape->cloakTextureOverride = nullptr;
                shape->alphaScale = 1.0f;
                zapDrawCount = shadowDraws.size();
                // This pose's mount nodes, for objects mounted on this ghost.
                {
                    MountFrames& frames = demoMountFrames[idx];
                    frames.valid = 0;
                    const MatrixF shapeModel = model * shape->upOrientation();
                    const auto& nodes = shape->animatedNodeWorld.size() == shape->defaultTransforms.size()
                        ? shape->animatedNodeWorld : shape->defaultTransforms;
                    static std::unordered_map<const DTSShape*, std::array<int, 32>> mountNodeCache;
                    auto cached = mountNodeCache.find(shape);
                    if (cached == mountNodeCache.end()) {
                        std::array<int, 32> found{};
                        for (int point = 0; point < 32; ++point)
                            found[point] = shape->findNode(("mount" + std::to_string(point)).c_str());
                        cached = mountNodeCache.emplace(shape, found).first;
                    }
                    for (int point = 0; point < 32; ++point) {
                        const int node = cached->second[point];
                        if (node < 0 || node >= (int)nodes.size()) continue;
                        frames.frame[point] = shapeModel * nodes[node];
                        frames.valid |= 1u << point;
                    }
                    static std::unordered_map<const DTSShape*, int> eyeNodeCache;
                    auto eye = eyeNodeCache.find(shape);
                    if (eye == eyeNodeCache.end()) eye = eyeNodeCache.emplace(shape, shape->findNode("eye")).first;
                    if (eye->second >= 0 && eye->second < (int)nodes.size()) {
                        const MatrixF eyeWorld = shapeModel * nodes[eye->second];
                        demoEyePositions[idx] = {eyeWorld.m[0][3], eyeWorld.m[1][3], eyeWorld.m[2][3]};
                        demoShapeFrames[idx] = shapeModel;
                    } else {
                        demoEyePositions.erase(idx);
                    }
                    // ShapeBaseData::preload: cameraNode is "cam", else the eye.
                    static std::unordered_map<const DTSShape*, int> camNodeCache;
                    auto cam = camNodeCache.find(shape);
                    if (cam == camNodeCache.end()) {
                        int node = shape->findNode("cam");
                        if (node < 0) node = eye->second;
                        cam = camNodeCache.emplace(shape, node).first;
                    }
                    if (cam->second >= 0 && cam->second < (int)nodes.size()) {
                        const MatrixF camWorld = shapeModel * nodes[cam->second];
                        demoCamPositions[idx] = {camWorld.m[0][3], camWorld.m[1][3], camWorld.m[2][3]};
                    } else {
                        demoCamPositions.erase(idx);
                    }
                }

                // Player::updateJet loops PlayerData jetSound while jetting.
                if (isPlayer) {
                    auto& audio = Engine::instance().audio();
                    const bool jetOn = g->jetting && g->damageState < 1 && !demoMatchEnded &&
                                       g->hasDatablock && audio.isInitialized();
                    const Point3F soundPos = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                    auto sound = demoJetSoundSources.find(idx);
                    if (jetOn && sound == demoJetSoundSources.end()) {
                        const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                        auto block = blocks.find((uint32_t)g->datablockId);
                        const uint32_t jetSound = block != blocks.end() &&
                            !block->second.decoded.playerSounds.empty()
                            ? block->second.decoded.playerSounds[0] : 0;
                        if (SoundSource* source = playNativeAudioProfile(audio, blocks, jetSound,
                                                                         soundPos, true, true))
                            demoJetSoundSources.emplace(idx, source);
                    } else if (!jetOn && sound != demoJetSoundSources.end()) {
                        audio.releaseSource(sound->second);
                        demoJetSoundSources.erase(sound);
                    } else if (sound != demoJetSoundSources.end()) {
                        sound->second->setPosition(soundPos);
                    }
                }

                // Player::updateJet: the PlayerData jetEmitter runs at the
                // jetNozzle nodes while jetting (light armours have only the
                // first), ejecting along the nozzle axis the jetflare mesh
                // extends along (node column 2 in the Y-up frame).
                if (isPlayer && g->jetting && g->damageState < 1 && g->hasDatablock &&
                    !demoMatchEnded && shape->animatedNodeWorld.size() == shape->nodes.size()) {
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = blocks.find((uint32_t)g->datablockId);
                    const uint32_t emitterRef = block != blocks.end()
                        ? block->second.decoded.playerJetEmitterRef : 0;
                    if (emitterRef) {
                        const MatrixF world = model * shape->upOrientation();
                        const Point3F velocity = Math::torquePointToYUp(
                            {g->torqueVelocity.x, g->torqueVelocity.y, g->torqueVelocity.z});
                        int nozzle = 0;
                        for (const char* name : {"jetnozzle0", "jetnozzle1"}) {
                            const int node = shape->findNode(name);
                            if (node >= 0) {
                                const MatrixF nodeWorld = world * shape->animatedNodeWorld[node];
                                const Point3F pos{nodeWorld.m[0][3], nodeWorld.m[1][3], nodeWorld.m[2][3]};
                                const Point3F axis{nodeWorld.m[0][2], nodeWorld.m[1][2], nodeWorld.m[2][2]};
                                w->syncNodeEmitter(((int64_t)idx << 8) | nozzle, emitterRef, pos, velocity,
                                                   axis, blocks);
                            }
                            ++nozzle;
                        }
                    }
                }

                // Vehicle jets: each thrust direction's nozzle emitters run
                // while jetting; flying vehicles' contrails run above
                // minTrailSpeed with a speed-ramped share of the frame; the
                // flying jet sound loops while jetting.
                if (isJetVehicle && g->hasDatablock && !demoMatchEnded &&
                    shape->animatedNodeWorld.size() == shape->nodes.size()) {
                    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
                    auto block = blocks.find((uint32_t)g->datablockId);
                    const V12::DecodedDataBlock* data =
                        block != blocks.end() ? &block->second.decoded : nullptr;
                    if (data && !data->vehicleJetEmitters.empty()) {
                        const MatrixF world = model * shape->upOrientation();
                        const float mass = data->shapeMass > 0.0f ? data->shapeMass : 1.0f;
                        const Vec3 torqueVelocity = g->hasLinearMomentum
                            ? Vec3{g->linearMomentum.x / mass, g->linearMomentum.y / mass,
                                   g->linearMomentum.z / mass}
                            : Vec3{0, 0, 0};
                        const Point3F velocity = Math::torquePointToYUp(
                            {torqueVelocity.x, torqueVelocity.y, torqueVelocity.z});
                        auto syncAt = [&](const char* nodeName, uint32_t emitterRef, int code,
                                          float emitScale) {
                            const int node = shape->findNode(nodeName);
                            if (node < 0 || !emitterRef) return;
                            const MatrixF nodeWorld = world * shape->animatedNodeWorld[node];
                            w->syncNodeEmitter(((int64_t)idx << 8) | code, emitterRef,
                                {nodeWorld.m[0][3], nodeWorld.m[1][3], nodeWorld.m[2][3]}, velocity,
                                {nodeWorld.m[0][2], nodeWorld.m[1][2], nodeWorld.m[2][2]}, blocks,
                                emitScale);
                        };
                        for (int direction = 0; direction < 3; ++direction) {
                            if (!g->vehicleJetting || g->thrustDirection != direction ||
                                direction >= (int)data->vehicleJetEmitters.size()) continue;
                            for (int k = 0; k < 2; ++k)
                                syncAt(VehicleJets::NozzleNodes[direction][k],
                                       data->vehicleJetEmitters[direction], 2 + direction * 2 + k, 1.0f);
                        }
                        if (data->isFlyingVehicleData && data->vehicleJetEmitters.size() > 3) {
                            const QuatF q(g->rotation.x, g->rotation.y, g->rotation.z, g->rotation.w);
                            const Point3F forward = q.toMatrix().transformNormal({0, 1, 0});
                            const float speed = std::fabs(torqueVelocity.x * forward.x +
                                torqueVelocity.y * forward.y + torqueVelocity.z * forward.z);
                            const float scale = VehicleJets::contrailScale(speed,
                                data->vehicleMinTrailSpeed, data->vehicleManeuveringForce / mass);
                            if (scale > 0.0f)
                                for (int k = 0; k < 4; ++k)
                                    syncAt(VehicleJets::ContrailNodes[k], data->vehicleJetEmitters[3],
                                           8 + k, scale);
                        }
                    }
                    auto& audio = Engine::instance().audio();
                    const bool soundOn = g->vehicleJetting && data && data->vehicleJetSound &&
                                         audio.isInitialized() && g->damageState < 2;
                    const Point3F soundPos = Math::torquePointToYUp({rp.x, rp.y, rp.z});
                    auto sound = demoJetSoundSources.find(idx);
                    if (soundOn && sound == demoJetSoundSources.end()) {
                        if (SoundSource* source = playNativeAudioProfile(audio, blocks,
                                data->vehicleJetSound, soundPos, true, true))
                            demoJetSoundSources.emplace(idx, source);
                    } else if (!soundOn && sound != demoJetSoundSources.end()) {
                        audio.releaseSource(sound->second);
                        demoJetSoundSources.erase(sound);
                    } else if (sound != demoJetSoundSources.end()) {
                        sound->second->setPosition(soundPos);
                    }
                }

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
                                 // ShapeBaseImageData shapeFile is relative to shapes/.
                                 dynamicPath = normalizeShapePath(wit->second);
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
                        // Mount to the node as posed by this frame's animation.
                        const auto& mountNodes = shape->animatedNodeWorld.size() ==
                            shape->defaultTransforms.size() ? shape->animatedNodeWorld
                                                            : shape->defaultTransforms;
                        if (mountNode >= 0 && mountNode < (int)mountNodes.size())
                            mountedModel = mountedModel * mountNodes[mountNode];
                        const int imageMount = wShape->findNode("Mountpoint");
                        if (imageMount >= 0 && imageMount < (int)wShape->defaultTransforms.size())
                            mountedModel = mountedModel * wShape->defaultTransforms[imageMount].inverse();
                         MatrixF imageModel = mountedModel * wShape->upOrientation();
                         r.setModel(imageModel);
                         // Mounted images keep their textures; cloakable ones fade to
                         // 0.15 + (1 - level) x 0.85 (ShapeBase::renderMountedImage).
                         wShape->cloakTextureOverride = nullptr;
                         {
                             const auto& imageBlocks = demoParser->getInitialBlock().dataBlocks;
                             auto imageData = imageBlocks.find((uint32_t)dbId);
                             const bool cloakable = imageData != imageBlocks.end() &&
                                                    imageData->second.decoded.imageCloakable;
                             wShape->alphaScale = mg->cloakLevel > 0.0f && cloakable
                                 ? 0.15f + (1.0f - mg->cloakLevel) * 0.85f : 1.0f;
                         }
                         wShape->lighting = shape->lighting;
                         wShape->emapEnabled = false; // image datablocks carry no emap
                         renderMountedImage(*wShape, mg->mountedImages[img].animation,
                                            demoMatchEnded ? demoMatchEndedAt : demoTime);
                         wShape->cloakTextureOverride = nullptr;
                         wShape->alphaScale = 1.0f;
                         // The image datablock's light at the image's mount:
                         // 1 constant, 2 pulsing, 3 WeaponFireLight (full on
                         // a shot, fading out over lightTime).
                         {
                             const auto& imageBlocks = demoParser->getInitialBlock().dataBlocks;
                             auto imageData = imageBlocks.find((uint32_t)dbId);
                             const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                             if (mg->imageLightDatablock[img] != dbId) {
                                 mg->imageLightDatablock[img] = dbId;
                                 mg->imageLightMountAt[img] = now;
                                 mg->imageLightFireCount[img] = -1;
                                 mg->imageLightFireAt[img] = -1.0f;
                             }
                             const int fireCount = g->mountedImages[img].fireCount;
                             if (mg->imageLightFireCount[img] >= 0 && fireCount != mg->imageLightFireCount[img])
                                 mg->imageLightFireAt[img] = now;
                             mg->imageLightFireCount[img] = fireCount;
                             if (imageData != imageBlocks.end()) {
                                 const auto& data = imageData->second.decoded;
                                 const float timeMS = (float)std::max(1, data.shapeLightTimeMS);
                                 float intensity = 0.0f;
                                 if (data.shapeLightType == 1) {
                                     intensity = 1.0f;
                                 } else if (data.shapeLightType == 2) {
                                     const float elapsedMS = (now - mg->imageLightMountAt[img]) * 1000.0f;
                                     intensity = 0.15f + 0.85f * (0.5f + 0.5f * std::sin(Math::PI * elapsedMS / timeMS));
                                 } else if (data.shapeLightType == 3 && mg->imageLightFireAt[img] >= 0.0f) {
                                     const float elapsedMS = (now - mg->imageLightFireAt[img]) * 1000.0f;
                                     intensity = elapsedMS >= 0.0f && elapsedMS <= timeMS ? 1.0f - elapsedMS / timeMS : 0.0f;
                                 }
                                 intensity = std::clamp(intensity, 0.0f, 1.0f);
                                 if (intensity > 0.0f && data.shapeLightRadius > 0.0f) {
                                     const auto& c = data.shapeLightColor;
                                     demoLights.push_back({mountedModel.m[0][3], mountedModel.m[1][3], mountedModel.m[2][3],
                                                           c[0] * intensity, c[1] * intensity, c[2] * intensity,
                                                           data.shapeLightRadius});
                                 }
                             }
                         }
                         // The animated Muzzlepoint (or the image itself).
                         {
                             const int muzzle = wShape->findNode("Muzzlepoint");
                             MatrixF muzzleWorld = imageModel;
                             if (muzzle >= 0 && muzzle < (int)wShape->animatedNodeWorld.size())
                                 muzzleWorld = imageModel * wShape->animatedNodeWorld[muzzle];
                             else if (muzzle >= 0 && muzzle < (int)wShape->defaultTransforms.size())
                                 muzzleWorld = imageModel * wShape->defaultTransforms[muzzle];
                             mg->muzzlePos[img] = {muzzleWorld.m[0][3], muzzleWorld.m[1][3], muzzleWorld.m[2][3]};
                             mg->muzzleDir[img] = {muzzleWorld.m[0][2], muzzleWorld.m[1][2], muzzleWorld.m[2][2]};
                             mg->hasMuzzle[img] = true;
                         }
                    }
                }
            }

            r.shadowCapture = nullptr;
            if (zapTarget) {
                auto zapIt = shockZapRequests().find(idx);
                const float now = demoMatchEnded ? demoMatchEndedAt : demoTime;
                const float age = now - zapIt->second.spawnTime;
                if (age < 0.0f || age >= zapIt->second.zapDuration)
                    shockZapRequests().erase(zapIt);
                else
                    drawShockZap(r, shadowDraws, zapDrawCount, zapOrigin, age, zapIt->second);
            }
            // Shadow::render: cloaked objects cast none.
            if (shadowCaster && !(mg->cloakLevel > 0.0f)) {
                const float dist = std::sqrt((r.cameraPos.x - shadowCenter.x) * (r.cameraPos.x - shadowCenter.x) +
                                             (r.cameraPos.y - shadowCenter.y) * (r.cameraPos.y - shadowCenter.y) +
                                             (r.cameraPos.z - shadowCenter.z) * (r.cameraPos.z - shadowCenter.z));
                ProjectedShadows::Caster caster;
                caster.key = idx;
                caster.draws = &shadowDraws;
                caster.center = shadowCenter;
                caster.radius = shadowRadius;
                caster.alpha = mg->fadeVal;
                projectedShadows().submit(r, caster, Timer::now() * 1000.0,
                    Engine::instance().game().isMapperMode() ? 0.0f : hazeAt(*w, dist),
                    [&](const Point3F& lo, const Point3F& hi, const Point3F& dir, std::vector<Point3F>& out) {
                        w->shadowReceiversInBox(lo, hi, dir, out);
                    });
                projectedShadows().draw(r);
            }

        }
        r.shadowCapture = nullptr;
        projectedShadows().endFrame(Timer::now() * 1000.0);
        for (auto it = shockZapRequests().begin(); it != shockZapRequests().end();)
            it = gt.hasGhost(it->first) ? std::next(it) : shockZapRequests().erase(it);
        w->endProjectileTrailSync();
        for (auto it = demoJetSoundSources.begin(); it != demoJetSoundSources.end();) {
            if (gt.hasGhost(it->first)) { ++it; continue; }
            Engine::instance().audio().releaseSource(it->second);
            it = demoJetSoundSources.erase(it);
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
             if (!isSensorGroupTargetVisible(activeConn->observerSnapshot().playerSensorGroup,
                                             g->sensorGroup)) continue;
            Vec3 p = g->position;
             // Origin is a valid mission position. Use the decoded presence
             // bit rather than treating (0,0,0) as an uninitialized ghost.
             if (!ObserverParity::isPositionReady(g->hasPosition)) continue;
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
             applyShapeBaseAudio(*g, idx, p, nativeDatablocks);

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
                    model = model * Math::nativeDtsFrame();
              }
              Point3F renderPosition = Math::torquePointToYUp({rp.x, rp.y, rp.z});
             model.setTranslation(renderPosition);
             r.setModel(model * g->shape->upOrientation());
             {
                 auto block = g->hasDatablock ? nativeDatablocks.find((uint32_t)g->datablockId)
                                              : nativeDatablocks.end();
                 g->shape->emapEnabled = block != nativeDatablocks.end() && block->second.decoded.shapeEmap;
             }
             Texture* cloakTexture = nullptr;
             if (g->cloaked || g->cloakLevel > 0.0f)
                 cloakTexture = r.loadTexture("textures/special/cloakTexture.png");
             g->shape->cloakTextureOverride = cloakTexture && cloakTexture->loaded ? cloakTexture : nullptr;
             g->cloakLevel = std::clamp(g->cloakLevel + (g->cloaked ? 1.0f : -1.0f) * dt / 0.5f, 0.0f, 1.0f);
             g->shape->alphaScale = g->cloakLevel > 0.0f ? 0.125f + (1.0f - g->cloakLevel) * 0.875f : 1.0f;
              DTSShape::NodeOverride overrides[8]{};
              int overrideCount = 0;
              if (isTurretGhostClass(g->className) && g->hasTurretAim) {
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
              w->applyShapeLighting(*g->shape, g->shapeLight, model * g->shape->upOrientation(),
                                    dt * 1000.0f);
               if (animation)
                   g->shape->renderAnimation(animation->name.c_str(),
                                             animationPosition * animation->duration,
                                             overrideCount ? overrides : nullptr, overrideCount,
                                             DTSShape::SelectDetail);
               else
                   g->shape->render(DTSShape::SelectDetail, overrideCount ? overrides : nullptr,
                                    overrideCount);
               g->shape->cloakTextureOverride = nullptr;
               g->shape->alphaScale = 1.0f;

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
                  imageShape->lighting = g->shape->lighting;
                  renderMountedImage(*imageShape, mounted.animation, demoTime);
              }
          }
        }

    }

    // 3D demo path trail
    if (demoPlaying && demoPathCount > 1) {
        // Full path in dim green
        r.drawLineStrip(demoPath, {0.2f, 0.8f, 0.2f, 0.6f});
    }

    // The HUD switches to its 2D projection, so it draws after every 3D
    // pass (world, demo and live ghosts).
    // Several world renderers change the blend function for additive passes;
    // establish normal source-alpha compositing for the in-game HUD and GUI.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (hud && !mapperMode && (gameState == Playing ||
                               (gameState == Dead && !demoPlaying)))
        hud->render(this);

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

void Game::startLocalGame(const char* map, std::vector<MisObject>* sceneObjects) {
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

    if (sceneObjects && demoParser) w->sceneDataBlocks = &demoParser->getInitialBlock().dataBlocks;
    const bool worldLoaded = sceneObjects
        ? w->loadObjects(missionPath.c_str(), "demo scene ghosts", std::move(*sceneObjects))
        : w->load(missionPath.c_str());
    w->sceneDataBlocks = nullptr;
    if (worldLoaded) {
        // Training missions end on death; stock multiplayer missions respawn.
        // Update this only after a successful load so a rejected replacement
        // cannot change the active mission's lifecycle rules.
        missionRespawn = stockMissionRules(missionPath).respawn;
        // Assign the local player's mission team before respawn selects a
        // team-specific SpawnSphere. Otherwise a previous online team's
        // replicated value can choose the wrong side during mission start.
        pl->setTeam(1);
        pl->respawn();
        previousFire = false;
        previousAltFire = false;
        previousReload = false;
        // Player::respawn already selected the team-specific SpawnSphere (or
        // the world's fallback spawn). Do not replace it with the generic
        // mission spawn here: that made local games ignore their authored
        // Storm/Inferno starting locations after the respawn selection.
        setState(Playing);
        Console::instance().printf(LogLevel::Info, "Game started on '%s'", missionPath.c_str());

        // Clear TS GUI dialogs and switch to in-game view
        auto& gui = Engine::instance().guiRenderer();
         gui.clearDialogs();
         gui.setContent("PlayGui");
         if (auto* weaponsHud = gui.findControl("weaponsHud")) {
            weaponsHud->visible = true;
            weaponsHud->fields["backgroundBitmap"] = "gui/hud_new_panel";
            weaponsHud->fields["highlightBitmap"] = "gui/hud_new_weaponselect";
            weaponsHud->fields["infiniteAmmoBitmap"] = "gui/hud_infinity";
            weaponsHud->hudSlots.resize(18);
            for (auto& slot : weaponsHud->hudSlots) {
                slot.visible = false;
                slot.active = false;
            }
            weaponsHud->activeHudSlot = -1;
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
                  auto* source = audio.createSource(true);
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

    } else {
        Console::instance().printf(LogLevel::Error, "Failed to load map '%s'", missionPath.c_str());
        failLocalGame();
    }
}

void Game::dispatchClientCommand(const std::vector<std::string>& raw, const std::vector<bool>& tagged) {
    // RemoteCommandEvent::process: every server command runs as the script
    // function clientCmd<name> with its arguments, tags expanded.
    const std::vector<std::string> args = RemoteCommand::scriptArguments(raw, tagged);
    if (args.empty()) return;
    if (auto* ts = Engine::instance().script().ts()) {
        if (!ts->dispatchClientCommand(args))
            Console::instance().printf(LogLevel::Warn,
                "Client: ignored unknown clientCmd '%s'", args[0].c_str());
    }
}

void Game::connectToServer(const char* host, uint16_t port) {
    if (!host || !host[0] || port == 0) {
        Console::instance().printf(LogLevel::Warn, "Invalid server address");
        return;
    }
    clearVoicePlaybacks();
    if (!allowDemoConnection(isDemoBuildMode(Engine::instance().demoMode,
                                             config().dedicated), true,
                             Console::instance().getBoolVariable("demoAllowConnect", false),
                             Console::instance().getBoolVariable("demoAllowWatch", true))) {
        Console::instance().printf(LogLevel::Warn,
            "Demo mode policy rejected observer connection; set demoAllowWatch=1 to allow it");
        return;
    }
    if (demoPlaying) stopDemoPlayback();
    if (auto* ts = Engine::instance().script().ts()) ts->clearPackages();
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

    auto& net = Engine::instance().network();
    activeConn = net.createConnection();
    {
        activeConn->setConnectCallback([this](bool success) {
            if (success) {
                Console::instance().printf(LogLevel::Info, "Connected!");
                activeConn->sendCommandPacket("setPlayerTeam 0");
                activeConn->sendCommandPacket("ScopeCommanderMap 1");
                activeConn->sendCommandPacket("WatchOnly ImaWatcher");
                liveSpectateInit = false;
                spectateGhostIndex = -1;
                liveFollowGhostIndex = -1;
                liveFollowCenterInit = false;
                setState(Dead);
                auto& gui = Engine::instance().guiRenderer();
                gui.clearDialogs();
                gui.setContent("PlayGui");
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
        activeConn->setCommandCallback([](const std::string& command) {
            if (!command.empty()) {
                Console::instance().printf(LogLevel::Warn,
                    "Ignored untrusted server command: %s", command.c_str());
            }
        });
        activeConn->setClientCommandCallback([this](const std::vector<std::string>& raw,
                                                    const std::vector<bool>& tagged) {
            dispatchClientCommand(raw, tagged);
        });
         activeConn->setStateCallback([this](const V12::ServerGameState& state) {
             for (const auto& [index, mask] : state.targetVisibleToggles)
                 liveTargetVisible[index & 15] ^= mask;
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
            liveMatchStarted_ = false;
            liveMatchEnded_ = false;
            liveMissionDisplayName_.clear();
            liveMissionType_.clear();
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
                   for (int ghostIndex : liveGhosts.getAllIndices()) {
                       GhostEntry* ghost = liveGhosts.getMutableGhost(ghostIndex);
                       if (ghost && ghost->targetId == (int)targetId)
                           applyLiveTargetInfoToGhost(*ghost, nullptr);
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
                  for (int ghostIndex : liveGhosts.getAllIndices()) {
                      GhostEntry* ghost = liveGhosts.getMutableGhost(ghostIndex);
                      if (!ghost || ghost->targetId != (int)targetId) continue;
                      applyLiveTargetInfoToGhost(*ghost, &target);
                  }
             });
        activeConn->setMissionCallback([this](uint32_t crc) {
            if (liveMissionCrc != 0 && liveMissionCrc != crc) {
                resetLiveMissionState();
                if (activeConn && w)
                    w->cleanupMission();
            }
            liveMissionCrc = crc;
        });
        activeConn->setServerMessageCallback([this](const std::vector<std::string>& argv) {
            if (argv.empty()) return;
            if (argv[0] == "ChatMessage") {
                // Remote clientCmdChatMessage runs through the client command
                // dispatch; only the plain chat event (text alone) lands here.
                if (argv.size() == 2) {
                     Console::instance().printf(LogLevel::Info, "[CHAT] %s", argv[1].c_str());
                     playChatBeep();
                    if (auto* ts = Engine::instance().script().ts();
                        ts && ts->hasFunction("addMessageHudLine"))
                        ts->callFunction("addMessageHudLine", {VMValue(argv[1])});
                }
                return;
            }
            if (argv[0] == "MissionEnd") {
                // GameConnection::endMission. The debrief messages that follow
                // only fill DebriefGui (and can arrive during play); the
                // stock DebriefGui is a mouse-driven screen even though the
                // simulation remains Playing while the summary is open.
                liveMatchEnded_ = true;
                Engine::instance().platform().setRelativeMouse(false);
                Engine::instance().platform().showMouse(true);
                clearProjectileAudio();
                clearMissionAudio();
                if (w) w->clearEffects();
                return;
            }
            if (argv[0] != "ServerMessage") return;
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
            if (argv.size() >= 5 && argv[1] == "MsgMissionDropInfo") {
                if ((!liveMissionDisplayName_.empty() || !liveMissionType_.empty()) &&
                    (liveMissionDisplayName_ != argv[2] || liveMissionType_ != argv[3]))
                    resetLiveMissionState();
                liveMissionDisplayName_ = argv[2];
                liveMissionType_ = argv[3];
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
              if (state->hasInteriorAlarm) ghost->interiorAlarm = state->interiorAlarm;
              if (state->hasTargetId) ghost->targetId = state->targetId;
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
              if (ghost->targetId >= 0) {
                  auto target = liveTargets.find((uint16_t)ghost->targetId);
                  applyLiveTargetInfoToGhost(*ghost, target != liveTargets.end() ? &target->second : nullptr);
              } else {
                  applyLiveTargetInfoToGhost(*ghost, nullptr);
              }
              if (state->hasPosition) {
                  ghost->position = {state->position.x, state->position.y, state->position.z};
                  ghost->hasPosition = true;
              }
            ghost->rotation = {state->rotation.x, state->rotation.y,
                               state->rotation.z, state->rotationW};
            ghost->hasRotation = state->hasRotation;
            ghost->health = state->health;
             if (state->damageRevision != ghost->damageRevision) {
                 // ShapeBase::unpackUpdate: an object already in the scene
                 // blows up when it becomes Destroyed or is blown apart.
                 const int previous = ghost->damageState;
                 const bool known = ghost->damageRevision != 0;
                 ghost->damageRevision = state->damageRevision;
                 ghost->blowApart = state->blowApart;
                 ghost->damageDir = {state->damageDir.x, state->damageDir.y, state->damageDir.z};
                 if (known && ghost->hasRenderModel && ghost->hasDatablock &&
                     ((previous != 2 && state->damageState == 2) || state->blowApart)) {
                     auto block = nativeDatablocks.find((uint32_t)ghost->datablockId);
                     if (block != nativeDatablocks.end())
                         w->blowUpShape(block->second.decoded, ghost->shape, ghost->renderModel,
                                        Math::torquePointToYUp({ghost->damageDir.x, ghost->damageDir.y,
                                                                ghost->damageDir.z}),
                                        nativeDatablocks, 0);
                 }
             }
             if (state->hasDamageState)
                 ghost->damageState = state->damageState;
             ghost->energy = state->energy;
              if (state->hasSteering) {
                  ghost->steeringYaw = state->steeringYaw;
                  ghost->hasSteering = true;
              }
              if (state->hasFrozen) ghost->frozen = state->frozen;
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
                                             state->soundThreads[i].valid,
                                             state->soundThreads[i].revision};
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
                                           &nativeDatablocks, normal, 0, impact.endedWithDecal);
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
         activeConn->setTargetControlCallback(
             [this](bool hasTarget, uint16_t targetId, bool hasPosition,
                    const V12Vec3& position, bool assign) {
                 if (!hasTarget || !assign) {
                     if (!hasTarget) {
                         serverPlayerGhostIndex = 0;
                         serverPlayerGhostSynced = false;
                         spectateGhostIndex = -1;
                         liveFollowGhostIndex = -1;
                         liveFollowCenterInit = false;
                     }
                     return;
                 }
                 serverPlayerGhostIndex = targetId;
                 serverPlayerGhostSynced = true;
                 if (activeConn) {
                     spectateGhostIndex = targetId;
                     if (hasPosition) {
                         liveFollowCenter = {position.x, position.y, position.z};
                         liveFollowCenterInit = true;
                     }
                 }
             });
           activeConn->setAudioCallback([this](const V12::ServerEvent& event) {
              auto& audio = Engine::instance().audio();
              if (event.hasVoiceStream) {
                  if (event.voiceCodec != 3) return;
                  const uint64_t key = (static_cast<uint64_t>(event.voiceClientId) << 16) |
                                       (static_cast<uint64_t>(event.voiceStream) << 8) |
                                       event.voiceCodec;
                  auto it = voicePlaybacks.find(key);
                  if (it != voicePlaybacks.end() && it->second->finished &&
                      event.voiceSequence != 0)
                      return;
                  // Sequence numbers restart at zero for each talk burst and
                  // wrap every 128 packets within a long burst. Preserve the
                  // decoder across a normal wrap, but reset it for a new burst.
                  if (VoiceStream::startsNewBurst(
                          it != voicePlaybacks.end(), event.voiceSequence,
                          it != voicePlaybacks.end() ? it->second->lastSequence : -1,
                          it != voicePlaybacks.end() && it->second->finished)) {
                      if (it != voicePlaybacks.end() && it->second->source)
                          audio.releasePcmStream(it->second->source);
                      auto playback = std::make_unique<VoicePlayback>();
                      if (audio.config().enabled && audio.config().masterVolume > 0.0f)
                          playback->source = audio.createPcmStream(audio.config().masterVolume);
                      voicePlaybacks[key] = std::move(playback);
                  }
                  it = voicePlaybacks.find(key);
                  if (it == voicePlaybacks.end()) return;
                  std::array<int16_t, TorchGsm::SamplesPerFrame> decoded{};
                  for (const auto& frame : event.voiceFrames) {
                      if (!it->second->decoder.decode(frame.data(), decoded)) {
                          if (it->second->source)
                              audio.releasePcmStream(it->second->source);
                          voicePlaybacks.erase(it);
                          return;
                      }
                      if (!it->second->source && audio.config().enabled &&
                          audio.config().masterVolume > 0.0f)
                          it->second->source = audio.createPcmStream(audio.config().masterVolume);
                      if (it->second->source)
                          audio.queuePcm16(it->second->source, decoded.data(), decoded.size(), 8000);
                  }
                  it->second->lastSequence = event.voiceSequence;
                  it->second->lastReceived = std::chrono::steady_clock::now();
                  if (event.voiceEndOfStream) {
                      if (it->second->source) audio.finishPcmStream(it->second->source);
                      it->second->finished = true;
                  }
                  return;
              }
              if (!audio.config().enabled || audio.config().sfxVolume <= 0)
                  return;
              // Audio profiles are the server's AudioProfile datablocks.
              const Point3F position = event.audioHasPosition
                  ? Math::torquePointToYUp({event.audioPosition.x,
                                            event.audioPosition.y,
                                            event.audioPosition.z})
                  : Point3F{};
              playNativeAudioProfile(audio, nativeDatablocks,
                                     static_cast<uint32_t>(event.audioProfileId), position);
          });
        if (!activeConn->connect(host, port)) {
            Console::instance().printf(LogLevel::Error, "Unable to connect to %s:%d", host, port);
            disconnectedCleanup();
        }
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
    return !isWorldLevelGhostClass(className);
}

static bool isEffectOnlyGhostClass(const std::string& className) {
    return isProjectileGhostClass(className);
}

static void appendWheelNodeOverrides(const GhostEntry& ghost, DTSShape& shape,
                                     float dt, DTSShape::NodeOverride* overrides,
                                     int& overrideCount, int maxOverrides,
                                     float* wheelRotation) {
    if (!isWheeledVehicleGhostClass(ghost.className) || !shape.nativeDTS || !overrides ||
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

        if (!ghost.frozen)
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


// Script-defined TSShapeConstructor datablocks (scripts/*.cs) name a shape's
// sequences as sequence0..sequenceN; the engine stops at the first empty one.
static std::vector<std::string> scriptShapeSequences(const std::string& shapePath) {
    std::vector<std::string> sequences;
    const std::string pathLower = missionLower(shapePath);
    auto fieldValue = [](const ScriptObject& object, const std::string& wanted) -> std::string {
        for (const auto& [key, value] : object.fields)
            if (missionLower(key) == wanted) return value.toString();
        return {};
    };
    for (const auto& [name, object] : ScriptEngine::instance().objects) {
        if (!object || missionLower(object->className) != "tsshapeconstructor") continue;
        const std::string base = missionLower(fieldValue(*object, "baseshape"));
        if (base.empty() || !(pathLower == base || pathLower.ends_with("/" + base))) continue;
        for (int i = 0; i < 127; ++i) {
            std::string entry = fieldValue(*object, "sequence" + std::to_string(i));
            if (entry.empty()) break;
            sequences.push_back(std::move(entry));
        }
        break;
    }
    return sequences;
}


void Game::updateDemoPlayerAnimation(float tickTime) {
    if (!demoParser) return;
    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
    GhostTracker& tracker = demoParser->getMutableGhostTracker();
    auto& audio = Engine::instance().audio();
    for (int index : tracker.getAllIndices()) {
        GhostEntry* g = tracker.getMutableGhost(index);
        if (!g) continue;
        // ShapeBase::updateImageState for each mounted image.
        for (int slot = 0; slot < 8; ++slot) {
            auto& image = g->mountedImages[slot];
            if (image.datablockId < 0) continue;
            if (image.animationDatablock != image.datablockId) {
                auto block = blocks.find((uint32_t)image.datablockId);
                std::vector<WeaponImage::StateData> states;
                if (block != blocks.end()) states = block->second.decoded.imageStates;
                image.animation = WeaponImage::Animation(std::move(states), tickTime,
                    (uint32_t)(index * 8 + slot + 1) * 2654435761u);
                image.animationDatablock = image.datablockId;
            }
            if (!image.animation.valid()) continue;
            WeaponImage::Flags flags;
            flags.triggerDown = image.triggerDown;
            flags.loaded = image.loaded;
            flags.ammo = image.ammo;
            flags.wet = image.wet;
            flags.target = image.target;
            flags.fireCount = image.fireCount;
            image.animation.advance(tickTime, flags, image.forceFire);
            image.forceFire = false;
            // State-entry sounds (setImageState), one-shot at the owner.
            for (int profile : image.animation.takeSounds()) {
                if (!audio.isInitialized() || !g->hasPosition || profile <= 0) continue;
                playNativeAudioProfile(audio, demoParser->getInitialBlock().dataBlocks,
                    (uint32_t)profile,
                    Math::torquePointToYUp({g->position.x, g->position.y, g->position.z}));
            }
        }
        if (!g->hasPosition || !ObserverParity::isPlayerClass(g->className)) continue;
        float runSurfaceAngle = 0.0f;
        float halfX = 0.0f, halfY = 0.0f;
        if (g->hasDatablock) {
            auto block = blocks.find((uint32_t)g->datablockId);
            if (block != blocks.end() && block->second.decoded.isPlayerData) {
                runSurfaceAngle = block->second.decoded.playerRunSurfaceAngle;
                halfX = std::max(0.0f, block->second.decoded.playerBoxSize[0] * 0.5f);
                halfY = std::max(0.0f, block->second.decoded.playerBoxSize[1] * 0.5f);
            }
        }
        // Player::updateMove contact: run-surface contact resets the timer.
        bool contact = false;
        if (w) {
            const Point3F feet = Math::torquePointToYUp({g->position.x, g->position.y, g->position.z});
            // Player::findContact queries the box footprint extended 0.03 m
            // above and below the feet; on a slope its uphill edge touches
            // first. Sample the footprint corners (Torque x/y map to Torch
            // x/-z) and take the highest surface.
            const float probe = feet.y + 0.5f;
            const float spacing = std::max(0.25f, std::max(halfX, halfY));
            float floor = w->getFloorHeight(feet.x, probe, feet.z);
            for (float sx : {-halfX, halfX})
                for (float sz : {-halfY, halfY})
                    floor = std::max(floor, w->getFloorHeight(feet.x + sx, probe, feet.z + sz));
            const Point3F normal = terrainNormalFromHeights(
                w->getFloorHeight(feet.x - spacing, probe, feet.z),
                w->getFloorHeight(feet.x + spacing, probe, feet.z),
                w->getFloorHeight(feet.x, probe, feet.z - spacing),
                w->getFloorHeight(feet.x, probe, feet.z + spacing), spacing);
            contact = PlayerAnimation::hasRunContact(feet.y, floor, normal.y, runSurfaceAngle);
        }
        g->contactTimer = contact ? 0 : std::min(g->contactTimer + 1, 1 << 20);
        PlayerAnimation::MoveAnimation picked;
        if (g->mountObject < 0)
            picked = PlayerAnimation::pickMoveAnimation(g->torqueVelocity.x, g->torqueVelocity.y,
                g->bodyYaw, g->contactTimer, g->falling, g->jetting);
        // Player::setActionThread ignores an unchanged action, including a
        // direction change for the same Side action.
        if (!g->moveAnimValid || g->moveAction != picked.action) {
            g->moveAction = picked.action;
            g->moveTimeScale = picked.timeScale;
            g->moveStartTime = tickTime;
            g->moveAnimValid = true;
        }
    }
}

void Game::importShapeSequences(DTSShape& shape, const std::string& shapePath,
                                const std::vector<std::string>& sequences) {
    auto& fs = Engine::instance().fs();
    size_t imported = 0;
    for (const std::string& entry : sequences) {
        // "file.dsq alias": whitespace ends the file name; the rest renames
        // the last imported sequence.
        const size_t split = entry.find_first_of(" \t");
        const std::string file = entry.substr(0, split);
        std::string alias;
        if (split != std::string::npos) {
            const size_t start = entry.find_first_not_of(" \t", split);
            if (start != std::string::npos) alias = entry.substr(start);
            while (!alias.empty() && std::isspace((unsigned char)alias.back())) alias.pop_back();
        }
        if (file.empty()) continue;
        const std::string path = "shapes/" + file;
        if (!TorchPath::isSafeLogicalPath(path.c_str())) continue;
        std::vector<uint8_t> data = fs.read(path.c_str());
        if (data.empty()) {
            Console::instance().printf(LogLevel::Warn, "Missing sequence %s for %s",
                entry.c_str(), shapePath.c_str());
            continue;
        }
        if (importDSQ(data.data(), data.size(), shape.nodes, alias, shape.animations) < 0) {
            Console::instance().printf(LogLevel::Error, "Load sequence %s failed for %s",
                entry.c_str(), shapePath.c_str());
            break;
        }
        ++imported;
    }
    std::vector<std::string> names;
    names.reserve(shape.animations.size());
    for (const auto& animation : shape.animations) names.push_back(animation.name);
    shape.actionTable = PlayerAnimation::buildActionTable(names);
    Console::instance().printf(LogLevel::Debug, "Imported %zu/%zu sequences for %s",
        imported, sequences.size(), shapePath.c_str());
}

DTSShape* Game::getOrLoadDemoShape(const std::string& className, const std::string& skinName,
                                   const std::string& datablockInstance) {
    if (className == "Camera" || className == "StationFXPersonal")
        return nullptr;
    // Only shapes render one; a MissionMarker (AIObjective, SpawnSphere,
    // WayPoint) joins the client scene only in the mission editor.
    if (EngineClasses::isEngineClass(className) &&
        (!EngineClasses::isA(className, "ShapeBase") || EngineClasses::isA(className, "MissionMarker")))
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

    // Streamed TSShapeConstructor datablocks name this shape's sequences.
    if (demoParser) {
        const std::string pathLower = missionLower(dbShapePath);
        for (const auto& [id, block] : demoParser->getInitialBlock().dataBlocks) {
            const std::string& base = block.decoded.constructorShape;
            if (base.empty()) continue;
            const std::string baseLower = missionLower(base);
            if (pathLower == baseLower || (pathLower.size() > baseLower.size() &&
                    pathLower.ends_with("/" + baseLower))) {
                importShapeSequences(shape, dbShapePath, block.decoded.constructorSequences);
                break;
            }
        }
    }

    auto inserted = demoShapeCache.emplace(cacheKey, std::move(shape));
    return inserted.first->second.loaded ? &inserted.first->second : nullptr;
}

bool Game::playDemo(const char* path) {
    if (!path || !path[0]) {
        Console::instance().printf(LogLevel::Warn, "Usage: playdemo <path>");
        return false;
    }
    // Do not carry menu/console key edges into the first demo frame.
    resetInputState();
    Console::instance().printf(LogLevel::Info, "Loading demo: %s", path);
    demoPlaying = false;
    demoPaused = false;
    clearMissionAudio();
    Engine::instance().audio().stopAll();
    clearProjectileAudio();
    auto failDemoLoad = [&]() {
        demoPlaying = false;
        demoMissionState = {};
        demoPaused = false;
        demoStepRequest = false;
        demoStepBlocks = 1;
        demoPlaybackRate = 1.0f;
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

    // The recording's saved client state (saveDemoSettings' $DemoValue[n])
    // is available to the scripts as soon as the demo is loaded.
    if (auto* ts = Engine::instance().script().ts())
        for (size_t i = 0; i < ib.demoValues.size(); ++i)
            ts->setGlobal("$DemoValue[" + std::to_string(i) + "]", VMValue(ib.demoValues[i]));

    // A recording starts from a mid-match snapshot, so it may contain player
    // score messages without replaying the original ClientJoin events. Seed
    // the stock message.cs roster from that snapshot before playback invokes
    // scoreList.cs callbacks.
    if (auto* ts = Engine::instance().script().ts(); ts && ts->hasFunction("handleClientJoin")) {
        const auto ghostIndices = demoParser->getGhostTracker().getAllIndices();
        for (const auto& player : demoParser->getPlayerInfo()) {
            if (player.clientId < 0) continue;
            const bool hasPlayerGhost = std::any_of(
                ghostIndices.begin(), ghostIndices.end(),
                [&](int index) {
                    const GhostEntry* ghost = demoParser->getGhostTracker().getGhost(index);
                    return ghost && ObserverParity::isPlayerClass(ghost->className) &&
                        ghost->targetId == player.targetId;
                });
            if (!hasPlayerGhost) continue;
            const std::string playerObject = "$PlayerList[" +
                std::to_string(player.clientId) + "]";
            const std::string existing = ts->getGlobal(playerObject).toString();
            if (!existing.empty()) continue;
            const std::vector<VMValue> joinArgs{
                VMValue("MsgClientJoin"), VMValue(""), VMValue(player.name),
                VMValue(player.clientId), VMValue(player.targetId),
                // Skip getPlayerPrefs: playback has no server connection from
                // which to request per-client preferences.
                VMValue(1), VMValue(0), VMValue(0), VMValue(0), VMValue(0)};
            ts->callFunction("handleClientJoin", joinArgs);
            const std::string playerHandle = ts->getGlobal(playerObject).toString();
            if (auto* object = ScriptEngine::instance().findObject(playerHandle.c_str()))
                ScriptEngine::instance().setObjectField(object, "isBot", VMValue(0));
        }
    }

    std::string loadMap = extractMapName(ib.missionName);
    if (loadMap.empty()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: initial block did not provide a mission name");
        failDemoLoad();
        return false;
    }
    // The client's world is what the server ghosts: the initial block's
    // SceneObject ghosts.
    std::vector<MisObject> scene = demoSceneObjects();
    if (scene.empty()) {
        Console::instance().printf(LogLevel::Error,
            "Demo: the recording has no TerrainBlock scene ghost for '%s'", loadMap.c_str());
        failDemoLoad();
        return false;
    }
    Console::instance().printf(LogLevel::Info, "Loading mission map from scene ghosts: %s (%zu objects)",
                               loadMap.c_str(), scene.size());
    State prevState = gameState;
    startLocalGame(loadMap.c_str(), &scene);
    if (gameState != Playing) {
        gameState = prevState;
        Console::instance().printf(LogLevel::Error,
            "Demo: the scene of '%s' failed to load", loadMap.c_str());
        failDemoLoad();
        return false;
    }
    demoWorldGhostResets = demoParser->getGhostResets();
    applyDemoSceneEffects();
    Engine::instance().guiRenderer().popDialog("ConsoleDlg");
    if (auto* console = Engine::instance().guiRenderer().findControl("ConsoleDlg"))
        console->visible = false;
    if (auto* overlay = Engine::instance().guiRenderer().findControl("FrameOverlayGui"))
        overlay->visible = false;

    // Reset demo path history
    demoPath.clear();
    demoPathCount = 0;

    // Reset stats
    demoPacketsParsed = 0;
    demoTime = 0;
    demoInterpolationDt = 0;
    demoStepFeedbackTime = 0.0f;
        demoPaused = false;
        demoStepRequest = false;
        demoStepBlocks = 1;
        demoPlaybackRate = 1.0f;
        demoEventLog.clear();
    demoAudioEventsPlayed.clear();
    int totalBlocks = demoParser->getBlockCount();
    // Playback advances one 32 ms tick per Move block. The header length is
    // the displayed duration, but never cut playback short of the last tick.
    const float tickTime = T2Demo::playbackBlockTime(totalBlocks,
                                                     demoParser->getMoveTicksBefore());
    demoTotalTime = std::max(hdr.demoLengthMs / 1000.0f, tickTime);
    if (demoTotalTime <= 0.0f) demoTotalTime = 1.0f;
    demoBlocksTotal = totalBlocks;
    demoBlocksDone = 0;
    demoSnapshots.clear();
    demoViewSnapshots.clear();
    demoSnapshots[0] = demoParser->captureSnapshot();
    demoMissionState = {loadMap, {}};
    demoFastForward = false; // real-time when invoked from console
    // Follow the recorder's view; its $firstPerson picks eye or third person.
    demoFirstPersonCam = true;
    demoRecordedFirstPerson = demoParser->getInitialBlock().firstPerson;
    controlGhostIndex = demoParser->getInitialBlock().controlObjectGhostIndex;
    demoAuthoredCamera = false;
    demoCameraFov = -1.0f;
    demoOrbitCam = false;
    demoHasOrientation = demoParser->getInitialBlock().hasControlRotation;
    demoViewYaw = demoParser->getInitialBlock().controlYaw;
    demoViewPitch = demoParser->getInitialBlock().controlPitch;

    Console::instance().printf(LogLevel::Info,
        "  Total blocks: %d, move ticks: %d (%.1f seconds)",
        totalBlocks, demoParser->getMoveTicksBefore().back(), demoTotalTime);
    Console::instance().printf(LogLevel::Info, "Demo loaded, starting playback...");
    demoPlaying = true;
    setState(Playing);
    return true;
}

void Game::stopDemoPlayback() {
    const bool wasPlayingRecording = demoParser != nullptr && !demoLive;
    demoLive = false;
    liveConnection = nullptr;
    if (auto* ts = Engine::instance().script().ts()) ts->clearPackages();
    resetInputState();
    demoPlaying = false;
    demoMissionState = {};
    demoPaused = false;
    liveLookDelta = {};
    demoStepRequest = false;
    demoStepBlocks = 1;
    demoFastForward = false;
    demoPlaybackRate = 1.0f;
    demoStepFeedbackTime = 0.0f;
    Engine::instance().audio().setPlaybackRate(1.0f);
    Engine::instance().audio().stopAll();
    clearMissionAudio();
    demoAudioEventsPlayed.clear();
    demoPath.clear();
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
    // Removing the playback connection runs the script's
    // demoPlaybackComplete(), which returns the shell to its recordings.
    if (wasPlayingRecording) {
        auto* ts = Engine::instance().script().ts();
        if (ts && ts->isFunction("demoPlaybackComplete"))
            ts->callFunction("demoPlaybackComplete", {});
        else
            Console::instance().printf(LogLevel::Warn, "Demo: demoPlaybackComplete() is not defined");
    }
}

// The recording's Move block: the engine Move struct as the client
// produced it (px/py/pz, pyaw/ppitch/proll, the unclamped floats, id,
// sendCount, freeLook, triggers).
static std::vector<uint8_t> rawMoveBlock(const ClientMoveIn& move) {
    std::vector<uint8_t> block(64, 0);
    auto put = [&](int offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) block[offset + i] = (uint8_t)(value >> (8 * i));
    };
    auto putF = [&](int offset, float value) { uint32_t u; memcpy(&u, &value, 4); put(offset, u); };
    put(0, (uint32_t)move.x); put(4, (uint32_t)move.y); put(8, (uint32_t)move.z);
    put(12, (uint16_t)move.yaw); put(16, (uint16_t)move.pitch); put(20, (uint16_t)move.roll);
    putF(24, (move.x - 16) / 16.0f); putF(28, (move.y - 16) / 16.0f); putF(32, (move.z - 16) / 16.0f);
    putF(36, LiveMovePolicy::angleRadians(move.yaw));
    putF(40, LiveMovePolicy::angleRadians(move.pitch));
    putF(44, LiveMovePolicy::angleRadians(move.roll));
    put(48, move.id); put(52, (uint32_t)move.sendCount);
    block[56] = move.freeLook ? 1 : 0;
    for (int i = 0; i < 6; ++i) block[57 + i] = move.trigger[i] ? 1 : 0;
    return block;
}

void Game::startLiveClient(GameConnection& connection) {
    if (demoParser) delete demoParser;
    demoParser = new DemoParser;
    demoParser->beginLiveStream(connection.getConnectSequence());
    demoLive = true;
    demoPlaying = false;
    demoPaused = false;
    liveConnection = &connection;
    liveMissionName.clear();
    liveMoveClock = 0.0f;
    liveLookDelta = {};
    for (int& triggerCount : livePrevTriggerCount) triggerCount = 0;
    demoMissionState = {};
    demoWorldGhostResets = -1;
    demoBlocksDone = 0;
    demoBlocksTotal = 0;
    demoTime = 0.0f;
    demoTotalTime = 0.0f;
    demoSnapshots.clear();
    demoViewSnapshots.clear();
    demoAudioEventsPlayed.clear();
    connection.onServerPacket = [this](const std::vector<uint8_t>& packet) {
        if (demoParser && demoLive)
            demoParser->appendLiveBlock(T2Demo::BlockTypePacket, packet.data(), packet.size());
    };
    auto send = connection.deliver;
    connection.deliver = [this, send](const std::vector<uint8_t>& packet) {
        if (demoParser && demoLive) demoParser->appendLiveBlock(T2Demo::BlockTypeSendPacket, nullptr, 0);
        if (send) send(packet);
    };
}

// GameConnection::getNextMove: merge Torch's input state with the stock
// MoveManager variables and clamp to the packet's 0..32 axes.
ClientMoveIn Game::nextLiveMove() {
    LiveMovePolicy::Input input{
        .forward = currentInput.forward,
        .backward = currentInput.backward,
        .left = currentInput.left,
        .right = currentInput.right,
        .jump = currentInput.jump,
        .jet = currentInput.jet,
        .fire = currentInput.fire,
        .altFire = currentInput.altFire,
        .freeLook = currentInput.freeCam,
        .yaw = liveLookDelta.y,
        .pitch = liveLookDelta.x,
    };
    auto* ts = Engine::instance().script().ts();
    if (!ts) return LiveMovePolicy::makeMove(input);
    auto value = [&](const char* name) { return ts->getGlobal(name).toFloat(); };
    input.forward = input.forward || ts->getGlobal("$mvForwardAction").toBool();
    input.backward = input.backward || ts->getGlobal("$mvBackwardAction").toBool();
    input.left = input.left || ts->getGlobal("$mvLeftAction").toBool();
    input.right = input.right || ts->getGlobal("$mvRightAction").toBool();
    input.jump = input.jump || ts->getGlobal("$mvUpAction").toBool();
    input.jet = input.jet || ts->getGlobal("$mvDownAction").toBool();
    input.jump = input.jump || (ts->getGlobal("$mvTriggerCount2").toInt() & 1) != 0;
    input.jet = input.jet || (ts->getGlobal("$mvTriggerCount3").toInt() & 1) != 0;
    input.fire = input.fire || (ts->getGlobal("$mvTriggerCount0").toInt() & 1) != 0;
    input.altFire = input.altFire || (ts->getGlobal("$mvTriggerCount1").toInt() & 1) != 0;
    input.freeLook = input.freeLook || ts->getGlobal("$mvFreeLook").toBool();
    const float scriptYaw = value("$mvYaw") + value("$mvYawLeftSpeed") - value("$mvYawRightSpeed");
    const float scriptPitch = value("$mvPitch") + value("$mvPitchUpSpeed") - value("$mvPitchDownSpeed");
    const float scriptRoll = value("$mvRoll") + value("$mvRollRightSpeed") - value("$mvRollLeftSpeed");
    if (scriptYaw != 0.0f) input.yaw = scriptYaw;
    if (scriptPitch != 0.0f) input.pitch = scriptPitch;
    input.roll = scriptRoll;
    ClientMoveIn move = LiveMovePolicy::makeMove(input);
    for (int i = 0; i < 6; ++i) {
        const int count = ts->getGlobal("$mvTriggerCount" + std::to_string(i)).toInt();
        move.trigger[i] = move.trigger[i] || (count & 1) ||
            (!(livePrevTriggerCount[i] & 1) && livePrevTriggerCount[i] != count);
        livePrevTriggerCount[i] = count;
    }
    ts->setGlobal("$mvYaw", VMValue(0.0f));
    ts->setGlobal("$mvPitch", VMValue(0.0f));
    ts->setGlobal("$mvRoll", VMValue(0.0f));
    liveLookDelta = {};
    return move;
}

// The client's process list: one move per 32 ms tick (getNextMove), which
// the recording keeps as a Move block.
void Game::tickLiveClient(float dt) {
    if (!demoLive || !liveConnection || !demoParser) return;
    liveMoveClock += std::max(0.0f, dt);
    while (liveMoveClock >= 0.032f) {
        liveMoveClock -= 0.032f;
        ClientMoveIn move = nextLiveMove();
        if (liveConnection->pushMove(move)) {
            move = liveConnection->moves.back();
            const auto block = rawMoveBlock(move);
            demoParser->appendLiveBlock(T2Demo::BlockTypeMove, block.data(), block.size());
        }
    }
    if (!demoPlaying) pumpLiveClient();
    // GameProcess::advanceTime: the HUD targets follow their objects.
    HUDTargetList::update((uint32_t)(SimState::simTime() * 1000.0), [this](int targetId, float out[3]) {
        return isClientTargetVisible(targetId) && targetBoxCenter(targetId, out);
    });
}

// NetConnection::handleGhostMessage and GhostAlwaysObjectEvent::process on
// the client.
bool Game::targetBoxCenter(int targetId, float out[3]) const {
    if (!demoParser || targetId < 0) return false;
    const GhostTracker& tracker = demoParser->getGhostTracker();
    for (int index : tracker.getAllIndices()) {
        const GhostEntry* ghost = tracker.getGhost(index);
        if (!ghost || ghost->targetId != targetId) continue;
        return ghostBoxCenter(index, out);
    }
    return false;
}

bool Game::ghostBoxCenter(int ghostIndex, float out[3]) const {
    if (!demoParser) return false;
    const GhostEntry* ghost = demoParser->getGhostTracker().getGhost(ghostIndex);
    if (!ghost) return false;
    auto drawn = demoBoxCenters.find(ghostIndex);
    if (drawn != demoBoxCenters.end()) {
        const Point3F p = Math::czUpToYUp().inverse().transform(drawn->second);
        out[0] = p.x, out[1] = p.y, out[2] = p.z;
    } else {
        out[0] = ghost->position.x, out[1] = ghost->position.y, out[2] = ghost->position.z;
    }
    return true;
}

void Game::forwardLiveEvents(const PacketData& pd) {
    if (!liveConnection) return;
    for (const auto& ev : pd.events) {
        // The client targets (targetManager.cc): tasks, removals, resets.
        if (ev.hasTargetTo) {
            float pos[3] = {ev.targetToPosition.x, ev.targetToPosition.y, ev.targetToPosition.z};
            // TargetToEvent::unpack: without a position, the target object's
            // world box centre.
            if (!ev.targetToHasPosition && !targetBoxCenter(ev.targetToId, pos)) pos[0] = pos[1] = pos[2] = 0;
            ClientTargets::targetTo(ev.targetToId, pos, ev.targetToAssign);
        }
        if (ev.removeClientTargetType >= 0) ClientTargets::removeTargetsOfType((uint32_t)ev.removeClientTargetType);
        if (ev.resetClientTargets >= 0) ClientTargets::reset(ev.resetClientTargets == 1);
        if (ev.hasTargetFree) HUDTargetList::targetRemoved((uint32_t)ev.targetId);
        if (ev.ghostMessage >= 0)
            liveConnection->clientGhostMessage(ev.ghostMessage, ev.ghostSequence, (uint32_t)ev.ghostCount);
        if (ev.classId == T2Demo::NetEventClassFirst + 3)
            if (auto* ts = Engine::instance().script().ts())
                ts->callFunction("ghostAlwaysObjectReceived", {});
    }
}

// Before the world: read the stream as it arrives, running its commands.
void Game::pumpLiveClient() {
    while (demoLive && !demoPlaying && demoParser) {
        std::unique_ptr<DemoBlock> block(demoParser->nextBlock());
        if (!block) break;
        ++demoBlocksDone;
        if (block->type == T2Demo::BlockTypeSendPacket) {
            demoParser->onSendPacketTrigger();
            continue;
        }
        if (block->type != T2Demo::BlockTypePacket) continue;
        PacketData pd = demoParser->parsePacket(block->data.data(), block->data.size(), demoBlocksDone - 1);
        bool ghostAlwaysDone = false;
        for (const auto& ev : pd.events) {
            if (ev.classId != T2Demo::NetEventClassFirst + 9 || ev.arguments.empty()) continue;
            // clientCmdMissionStartPhase1(%seq, %missionName, %musicTrack).
            if (ev.arguments[0] == "MissionStartPhase1" && ev.arguments.size() > 2)
                liveMissionName = extractMapName(ev.arguments[2]);
            dispatchClientCommand(ev.rawArguments, ev.taggedArguments);
        }
        for (const auto& ev : pd.events) ghostAlwaysDone = ghostAlwaysDone || ev.ghostMessage == 0;
        forwardLiveEvents(pd);
        if (ghostAlwaysDone && startLiveWorld()) break;
    }
}

bool Game::startLiveWorld() {
    std::vector<MisObject> scene = demoSceneObjects();
    if (scene.empty() || liveMissionName.empty()) {
        Console::instance().printf(LogLevel::Error,
            "Live client: the ghosted scene has no TerrainBlock (mission '%s')", liveMissionName.c_str());
        return false;
    }
    Console::instance().printf(LogLevel::Info, "Live client: loading %s from %zu scene ghosts",
                               liveMissionName.c_str(), scene.size());
    startLocalGame(liveMissionName.c_str(), &scene);
    if (gameState != Playing) return false;
    demoWorldGhostResets = demoParser->getGhostResets();
    applyDemoSceneEffects();
    demoMissionState = {liveMissionName, {}};
    demoPlaybackRate = 1.0f;
    demoFastForward = false;
    demoFirstPersonCam = true;
    demoRecordedFirstPerson = true;
    controlGhostIndex = -1;
    demoTime = T2Demo::playbackBlockTime(demoBlocksDone, demoParser->getMoveTicksBefore());
    demoBlocksTotal = demoParser->getBlockCount();
    demoTotalTime = demoTime;
    demoPlaying = true;
    setState(Playing);
    if (liveConnection) {
        liveConnection->sendRemoteCommand({NetStrings::literal("ClientJoinGame")});
    }
    return true;
}

void Game::toggleDemoPause() {
    if (demoPaused) resumeDemo();
    else pauseDemo();
}

void Game::pauseDemo() {
    // A live connection cannot be paused from the client.
    if (!demoPlaying || demoPaused || demoLive) return;
    demoPaused = true;
    Engine::instance().audio().pauseAll();
}

void Game::resumeDemo() {
    if (!demoPlaying || !demoPaused) return;
    demoPaused = false;
    Engine::instance().audio().setPlaybackRate(
        (demoFastForward || currentInput.jet) ? std::max(demoPlaybackRate, 4.0f) : demoPlaybackRate);
    Engine::instance().audio().resumeAll();
}

void Game::setDemoPlaybackSpeed(float speed) {
    if (!std::isfinite(speed)) return;
    demoPlaybackRate = std::clamp(speed, 0.1f, 8.0f);
    if (demoPlaying && !demoPaused)
        Engine::instance().audio().setPlaybackRate(
            (demoFastForward || currentInput.jet) ? std::max(demoPlaybackRate, 4.0f) : demoPlaybackRate);
}

void Game::resetDemoEvents() {
    demoEventLog.clear();
    if (demoParser) demoParser->clearEventLog();
}

void Game::resetDemoHud() {
    if (demoParser) demoParser->resetHudState();
    if (hud) hud->resetState();
    resetGameplayGui(Engine::instance().guiRenderer());
}

void Game::resetDemoCamera() {
    demoAuthoredCamera = false;
    demoHasPos = false;
    demoHasOrientation = demoParser && demoParser->getInitialBlock().hasControlRotation;
    if (demoHasOrientation) {
        demoViewYaw = demoParser->getInitialBlock().controlYaw;
        demoViewPitch = demoParser->getInitialBlock().controlPitch;
    }
    demoOrbitCam = false;
    demoFirstPersonCam = demoParser != nullptr;
    demoRecordedFirstPerson = demoParser ? demoParser->getInitialBlock().firstPerson : true;
    spectateGhostIndex = -1;
    controlGhostIndex = demoParser ? demoParser->getInitialBlock().controlObjectGhostIndex : -1;
    demoCameraFov = -1.0f;
    demoPath.clear();
    demoPathCount = 0;
    orbitCenterInit = false;
}

Game::DemoViewSnapshot Game::captureDemoView() const {
    DemoViewSnapshot view;
    view.controlGhostIndex = controlGhostIndex;
    view.viewYaw = demoViewYaw;
    view.viewPitch = demoViewPitch;
    view.hasOrientation = demoHasOrientation;
    view.hasPos = demoHasPos;
    view.authoredCamera = demoAuthoredCamera;
    view.cameraPos = demoCameraPos;
    view.cameraTarget = demoCameraTarget;
    view.cameraFov = demoCameraFov;
    view.cameraMode = demoCameraMode;
    view.orbitGhost = demoOrbitGhost;
    view.orbitMinDist = demoOrbitMinDist;
    view.orbitMaxDist = demoOrbitMaxDist;
    view.orbitPoint = demoOrbitPoint;
    view.recordedFirstPerson = demoRecordedFirstPerson;
    view.headZ = demoHeadZ;
    view.piloting = demoPiloting;
    return view;
}

void Game::restoreDemoView(const DemoViewSnapshot& view) {
    controlGhostIndex = view.controlGhostIndex;
    demoViewYaw = view.viewYaw;
    demoViewPitch = view.viewPitch;
    demoHasOrientation = view.hasOrientation;
    demoHasPos = view.hasPos;
    demoAuthoredCamera = view.authoredCamera;
    demoCameraPos = demoPrevCameraPos = view.cameraPos;
    demoCameraTarget = demoPrevCameraTarget = view.cameraTarget;
    demoCameraFov = view.cameraFov;
    demoCameraMode = view.cameraMode;
    demoOrbitGhost = view.orbitGhost;
    demoOrbitMinDist = view.orbitMinDist;
    demoOrbitMaxDist = view.orbitMaxDist;
    demoOrbitPoint = view.orbitPoint;
    demoRecordedFirstPerson = view.recordedFirstPerson;
    demoHeadZ = view.headZ;
    demoPiloting = view.piloting;
}

void Game::tickDemoPlayers(const DemoBlock& moveBlock) {
    if (!demoParser || !w || demoMatchEnded) return;
    PlayerPrediction::Move recorderMove;
    bool haveRecorderMove = false;
    // A recording's move queue follows collectMove's admission limit; a live
    // connection's moves are the local client's own (GameConnection queues
    // and acknowledges them).
    if (moveBlock.size >= 64 && (demoLive || demoParser->moveQueue().admit())) {
        const DemoMove move = demoParser->readRawMove(moveBlock.data.data(), moveBlock.data.size());
        recorderMove.x = move.x; recorderMove.y = move.y; recorderMove.z = move.z;
        recorderMove.yaw = move.yaw; recorderMove.pitch = move.pitch; recorderMove.roll = move.roll;
        recorderMove.freeLook = move.freeLook;
        for (int i = 0; i < 6; ++i) recorderMove.trigger[i] = move.trigger[i];
        haveRecorderMove = true;
    }
    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
    auto& tracker = demoParser->getGhostTracker();
    // Static shapes are in the player's collision mask (StaticShapeObjectType
    // | StaticTSObjectType). Their hulls are rebuilt only when a ghost's shape
    // or placement changes.
    std::vector<const StaticShapeHull*> hulls;
    for (auto it = staticShapeHulls.begin(); it != staticShapeHulls.end();)
        it = tracker.getGhost(it->first) ? std::next(it) : staticShapeHulls.erase(it);
    for (int index : tracker.getAllIndices()) {
        const GhostEntry* g = tracker.getGhost(index);
        if (!g || !g->shape || !g->hasRenderModel) continue;
        if (!ghostClassIs(g->className, "StaticShape") && !ghostClassIs(g->className, "ScopeAlwaysShape") &&
            !ghostClassIs(g->className, "TSStatic") && !isTurretGhostClass(g->className)) continue;
        StaticShapeHull& hull = staticShapeHulls[index];
        if (hull.shape != g->shape || std::memcmp(hull.model.m, g->renderModel.m, sizeof(hull.model.m)) != 0) {
            hull.shape = g->shape;
            hull.model = g->renderModel;
            hull.tris.clear();
            appendShapeCollisionTriangles(*g->shape, g->renderModel, hull.tris);
            hull.lo = {1e30f, 1e30f, 1e30f};
            hull.hi = {-1e30f, -1e30f, -1e30f};
            for (const auto& t : hull.tris)
                for (const Point3F& p : {t.a, t.b, t.c}) {
                    hull.lo = {std::min(hull.lo.x, p.x), std::min(hull.lo.y, p.y), std::min(hull.lo.z, p.z)};
                    hull.hi = {std::max(hull.hi.x, p.x), std::max(hull.hi.y, p.y), std::max(hull.hi.z, p.z)};
                }
        }
        if (!hull.tris.empty()) hulls.push_back(&hull);
    }
    const PlayerPrediction::GatherTriangles gather = [&](const Point3F& lo, const Point3F& hi,
                                                         std::vector<PlayerPrediction::Triangle>& out) {
        w->playerTrianglesInBox(lo, hi, out);
        for (const StaticShapeHull* h : hulls) {
            const StaticShapeHull& hull = *h;
            if (hull.hi.x < lo.x || hull.lo.x > hi.x || hull.hi.y < lo.y || hull.lo.y > hi.y ||
                hull.hi.z < lo.z || hull.lo.z > hi.z) continue;
            out.insert(out.end(), hull.tris.begin(), hull.tris.end());
        }
    };
    const PlayerPrediction::WaterLevel water = [&](float x, float y) { return w->waterSurfaceAt(x, y); };
    for (int index : tracker.getAllIndices()) {
        GhostEntry* g = const_cast<GhostEntry*>(tracker.getGhost(index));
        if (!g || !ObserverParity::isPlayerClass(g->className) || !g->hasDatablock) continue;
        auto block = blocks.find((uint32_t)g->datablockId);
        if (block == blocks.end() || !block->second.decoded.isPlayerData) continue;
        const auto& data = block->second.decoded.playerPhysics;
        auto& state = g->prediction;
        if (g->predictionUpdate != g->playerUpdates) {
            const auto update = index == controlGhostIndex
                ? PlayerPrediction::interpolatedAnchor(g->playerUpdate) : g->playerUpdate;
            PlayerPrediction::unpackUpdate(state, data, update, g->predictionUpdate < 0,
                                           update.headInRadians);
            if (g->predictionUpdate < 0 && !g->playerUpdate.hasEnergy)
                state.energy = data.maxEnergy;
            g->predictionUpdate = g->playerUpdates;
        }
        state.mounted = g->mountObject >= 0;
        state.damageState = g->damageState;
        const bool recorder = index == controlGhostIndex;
        // Once the recorded queue overflows its input history is unusable;
        // the recorder is predicted without its retained move.
        if (recorder && !demoLive && !demoParser->moveQueue().available) state.move = {};
        state.allowFreelook = recorder && ((state.mounted && g->mountNode == 0) || !demoRecordedFirstPerson);
        PlayerPrediction::processTick(state, data, getGravity(), recorder && haveRecorderMove ? &recorderMove : nullptr,
                                      0.0f, demoPlayerCollision, gather, water);
    }
    demoLastPlayerTick = T2Demo::playbackBlockTime(demoParser->getBlockCursor(), demoParser->getMoveTicksBefore());
}

// GameConnection::handleRecordedBlock info block: $firstPerson (byte 0) and
// the camera FOV (F32 at +4).
void Game::applyDemoInfoBlock(const DemoBlock& block) {
    if (block.type != T2Demo::BlockTypeInfo || block.data.size() < 8) return;
    demoRecordedFirstPerson = block.data[0] != 0;
    float fov;
    std::memcpy(&fov, block.data.data() + 4, sizeof(fov));
    if (std::isfinite(fov) && fov > 0.0f) demoCameraFov = fov;
}

// The recorder's view from a Move block: moves carry view deltas
// (Player::updateMove).
void Game::applyDemoMoveView(const DemoBlock& block) {
    if (block.type != T2Demo::BlockTypeMove || block.size < 64) return;
    DemoMove move = demoParser->readRawMove(block.data.data(), block.data.size());
    if (!demoMoveOrientationValid(move.yaw, move.pitch)) return;
    if (demoAuthoredCamera) {
        T2Demo::accumulateViewMove(demoViewYaw, demoViewPitch, move.yaw, move.pitch,
                                   T2Demo::CameraMaxViewPitch);
    } else {
        // Player::updateMove: pitch always turns the head; yaw turns the
        // head (within maxFreelookAngle) under freelook when seated at mount
        // node 0 or in third person, else the body, while the head's yaw (and
        // its pitch when controlling a vehicle) halves back.
        const GhostEntry* control = demoParser->getGhostTracker().getGhost(controlGhostIndex);
        float maxFreelook = 0.0f;
        if (control && control->hasDatablock) {
            const auto& blocks = demoParser->getInitialBlock().dataBlocks;
            auto data = blocks.find((uint32_t)control->datablockId);
            if (data != blocks.end()) maxFreelook = data->second.decoded.playerMaxFreelookAngle;
        }
        const bool seated = control && control->mountObject >= 0 && control->mountNode == 0;
        float yaw = move.yaw;
        if (yaw > Math::PI) yaw -= 2.0f * Math::PI;
        if (move.freeLook && (seated || !demoRecordedFirstPerson)) {
            T2Demo::accumulateViewMove(demoViewYaw, demoViewPitch, 0.0f, move.pitch);
            demoHeadZ = std::clamp(demoHeadZ + yaw, -maxFreelook, maxFreelook);
        } else {
            T2Demo::accumulateViewMove(demoViewYaw, demoViewPitch, move.yaw, move.pitch);
            demoHeadZ *= 0.5f;
            if (demoPiloting) demoViewPitch *= 0.5f;
        }
    }
    demoHasOrientation = true;
    // Camera::processTick FlyMode: the move's x/y/z along the camera's
    // right/forward/up at Camera::movementSpeed (40), doubled by a trigger.
    if (demoAuthoredCamera && demoCameraMode == T2Demo::CameraFly &&
        std::isfinite(move.x) && std::isfinite(move.y) && std::isfinite(move.z)) {
        const float scale = 40.0f * ((move.trigger[0] || move.trigger[1]) ? 2.0f : 1.0f) * 0.032f;
        const float sy = std::sin(demoViewYaw), cy = std::cos(demoViewYaw);
        const float sp = std::sin(demoViewPitch), cp = std::cos(demoViewPitch);
        const Point3F right{cy, -sy, 0.0f}, forward{sy * cp, cy * cp, -sp}, up{sy * sp, cy * sp, cp};
        demoPrevCameraPos = demoCameraPos;
        demoCameraPos.x += (right.x * move.x + forward.x * move.y + up.x * move.z) * scale;
        demoCameraPos.y += (right.y * move.x + forward.y * move.y + up.y * move.z) * scale;
        demoCameraPos.z += (right.z * move.x + forward.z * move.y + up.z * move.z) * scale;
    }
    if (demoHasPos) {
        demoPrevCameraTarget = demoCameraTarget;
        const Vec3 direction = T2Demo::cameraDirectionFromYawPitch(demoViewYaw, demoViewPitch);
        demoCameraTarget = {demoCameraPos.x + direction.x,
                            demoCameraPos.y + direction.y,
                            demoCameraPos.z + direction.z};
        demoMoveBlend = 0.0f;
    }
}

// The recorder's control object, view and camera from a packet's GameState.
void Game::applyDemoPacketView(const PacketData& pd) {
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
        // Camera::readPacketData sets mRot and the mode; moves then turn
        // it from there (Camera::processTick).
        demoViewYaw = pd.gameState.cameraYaw;
        demoViewPitch = pd.gameState.cameraPitch;
        demoHasOrientation = true;
        demoCameraMode = pd.gameState.cameraMode;
        demoOrbitGhost = pd.gameState.orbitObjectGhostIndex;
        demoOrbitMinDist = pd.gameState.orbitMinDistance;
        demoOrbitMaxDist = pd.gameState.orbitMaxDistance;
        demoOrbitPoint = {pd.gameState.orbitPoint.x, pd.gameState.orbitPoint.y,
                          pd.gameState.orbitPoint.z};
    }
    // The control player's own view re-anchors the move-
    // accumulated angles (t2-mapper getAbsoluteRotation).
    if (pd.gameState.hasControlRotation && pd.gameState.controlObjectGhostIndex >= 0 && demoParser) {
        // Player::readPacketData drives the recorder's own simulation.
        GhostEntry* control = const_cast<GhostEntry*>(
            demoParser->getGhostTracker().getGhost(pd.gameState.controlObjectGhostIndex));
        if (control && (pd.gameState.controlPlayer.hasPosition ||
                        pd.gameState.controlPlayer.hasEnergy)) {
            control->playerUpdate = pd.gameState.controlPlayer;
            ++control->playerUpdates;
            control->prediction.jumpDelay = pd.gameState.controlJumpDelay;
            control->prediction.jumpSurfaceLastContact = pd.gameState.controlJumpSurfaceLastContact;
            control->prediction.disableMove = pd.gameState.controlDisableMove;
        }
    }
    if (pd.gameState.hasControlRotation) {
        demoViewYaw = pd.gameState.controlRotZ;
        demoViewPitch = pd.gameState.controlHeadX;
        demoHeadZ = pd.gameState.controlHeadZ;
        demoPiloting = pd.gameState.controlPilotedGhostIndex >= 0;
        demoHasOrientation = true;
        // Control passed to a Player: the view follows it, not a
        // Camera transform from earlier in the recording.
        demoAuthoredCamera = false;
        if (pd.gameState.controlObjectGhostIndex >= 0)
            controlGhostIndex = pd.gameState.controlObjectGhostIndex;
    }
    if (pd.gameState.controlObjectDirty) {
        // Full control object update with new ghost index
        // (position comes from move blocks, not GameState)
    } else if (pd.gameState.compressionPointUpdated && !pd.gameState.hasCameraTransform) {
          // Update compression point from partial control update
          Vec3 cp = pd.gameState.compressionPoint;
          const int controlIndex = pd.gameState.controlObjectGhostIndex >= 0
              ? pd.gameState.controlObjectGhostIndex : controlGhostIndex;
          if (demoParser) {
              const auto* control = demoParser->getGhostTracker().getGhost(controlIndex);
              if (control && ObserverParity::isPlayerClass(control->className)) cp.z += 1.5f;
          }
          demoPrevCameraPos = demoCameraPos;
          demoPrevCameraTarget = demoCameraTarget;
           demoCameraPos = {cp.x, cp.y, cp.z};
           if (demoHasOrientation) {
               const Vec3 direction = T2Demo::cameraDirectionFromYawPitch(
                   demoViewYaw, demoViewPitch);
               demoCameraTarget = {cp.x + direction.x, cp.y + direction.y,
                                   cp.z + direction.z};
           } else {
               demoCameraTarget = {cp.x, cp.y + 2.0f, cp.z};
           }
          demoMoveBlend = 0.0f;
          demoHasPos = true;
     }
     if (pd.gameState.cameraFov > 0) demoCameraFov = pd.gameState.cameraFov;
    // Store control object ghost index for highlight
     if (pd.gameState.controlObjectGhostIndex >= 0)
         controlGhostIndex = pd.gameState.controlObjectGhostIndex;
}

void Game::resetDemoEffects() {
    Engine::instance().audio().stopAll();
    clearMissionAudio();
    clearProjectileAudio();
    demoAudioEventsPlayed.clear();
    damageFlash = -1.0f;
    whiteOut = -1.0f;
    if (w) w->clearEffects();
}


// AudioEmitter::update: a profile supplies the file; its description (with
// useProfileDescription) or the emitter's own description datablock
// replaces the emitter's volume, looping and 3D distance fields.
static void resolveDemoAudioEmitter(MisObject& object, const std::map<uint32_t, ParsedDataBlock>& blocks) {
    auto prop = [&](const char* name) { return getProp(object.props, name); };
    auto set = [&](const char* name, const std::string& value) {
        for (auto& p : object.props) if (p.name == name) { p.value = value; return; }
        object.props.push_back({name, value});
    };
    auto block = [&](const std::string& id) -> const V12::DecodedDataBlock* {
        if (id.empty() || std::atoi(id.c_str()) <= 0) return nullptr;
        auto it = blocks.find((uint32_t)std::atoi(id.c_str()));
        return it == blocks.end() ? nullptr : &it->second.decoded;
    };
    // AudioEmitter::packUpdate sends only fields that differ from
    // smDefaultDescription (volume 1, looping, 3D, distances 1..100).
    const std::pair<const char*, const char*> defaults[] = {
        {"volume", "1"}, {"islooping", "1"}, {"is3d", "1"}, {"mindistance", "1"}, {"maxdistance", "100"}};
    for (const auto& [name, value] : defaults)
        if (prop(name).empty()) set(name, value);
    const V12::DecodedDataBlock* profile = block(prop("audioprofileid"));
    if (profile && !profile->audioFilename.empty()) set("filename", profile->audioFilename);
    const V12::DecodedDataBlock* description = block(prop("audiodescriptionid"));
    if (profile && std::atoi(prop("useprofiledescription").c_str()) != 0)
        description = block(std::to_string(profile->audioDescriptionRef));
    if (description) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", description->audioVolume); set("volume", buf);
        set("islooping", description->audioLooping ? "1" : "0");
        set("is3d", description->audioIs3D ? "1" : "0");
        snprintf(buf, sizeof(buf), "%g", description->audioMinDistance); set("mindistance", buf);
        snprintf(buf, sizeof(buf), "%g", description->audioMaxDistance); set("maxdistance", buf);
    }
}

std::vector<MisObject> Game::demoSceneObjects() const {
    std::vector<MisObject> objects;
    if (!demoParser) return objects;
    bool terrain = false;
    const auto& tracker = demoParser->getGhostTracker();
    for (int index : tracker.getAllIndices()) {
        const GhostEntry* g = tracker.getGhost(index);
        if (!g || g->sceneProps.empty()) continue;
        // Effects with recorded datablocks are added by applyDemoSceneEffects.
        if (ghostClassIs(g->className, "Lightning") || ghostClassIs(g->className, "ParticleEmissionDummy") ||
            ghostClassIs(g->className, "Precipitation"))
            continue;
        MisObject object;
        object.className = g->className;
        for (const auto& [name, value] : g->sceneProps) {
            std::string lower = name;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            object.props.push_back({lower, value});
        }
        if (missionClassEquals(object.className, "ForceFieldBare")) {
            object.props.push_back({"ghostindex", std::to_string(index)});
            if (g->hasDatablock) object.props.push_back({"datablockid", std::to_string(g->datablockId)});
        }
        if (missionClassEquals(object.className, "InteriorInstance"))
            object.props.push_back({"ghostindex", std::to_string(index)});
        terrain = terrain || missionClassEquals(object.className, "TerrainBlock");
        if (missionClassEquals(object.className, "AudioEmitter"))
            resolveDemoAudioEmitter(object, demoParser->getInitialBlock().dataBlocks);
        objects.push_back(std::move(object));
    }
    if (!terrain) objects.clear();
    return objects;
}

void Game::applyDemoSceneEffects() {
    if (!demoParser || !w) return;
    const auto& blocks = demoParser->getInitialBlock().dataBlocks;
    auto find = [&](const std::string& id) -> const V12::DecodedDataBlock* {
        if (id.empty() || std::atoi(id.c_str()) <= 0) return nullptr;
        auto it = blocks.find((uint32_t)std::atoi(id.c_str()));
        return it == blocks.end() ? nullptr : &it->second.decoded;
    };
    const auto& tracker = demoParser->getGhostTracker();
    for (int index : tracker.getAllIndices()) {
        const GhostEntry* g = tracker.getGhost(index);
        if (!g || g->sceneProps.empty()) continue;
        auto prop = [&](const char* name) -> std::string {
            for (const auto& [key, value] : g->sceneProps) if (key == name) return value;
            return {};
        };
        if (ghostClassIs(g->className, "Lightning")) {
            w->addSceneLightning([&](const char* field) { return prop(field); });
            continue;
        }
        if (ghostClassIs(g->className, "Precipitation")) {
            const auto* data = g->hasDatablock ? find(std::to_string(g->datablockId)) : nullptr;
            w->setScenePrecipitation([&](const char* field) { return prop(field); },
                [&](const char* field) -> std::string {
                    if (!data || !data->hasPrecipitation) return {};
                    const std::string name = field;
                    if (name == "sizex") return std::to_string(data->precipitationSizeX);
                    if (name == "sizey") return std::to_string(data->precipitationSizeY);
                    if (name == "type") return std::to_string(data->precipitationType);
                    if (name == "materiallist") return data->precipitationMaterialList;
                    return {};
                });
            continue;
        }
        if (ghostClassIs(g->className, "ParticleEmissionDummy")) {
            // ParticleEmissionDummy: its emitter datablock, emitting along
            // the dummy's z axis.
            const auto* emitter = find(prop("emitterId"));
            if (!emitter || !emitter->hasEmitter || emitter->emitter.particleRefs.empty()) continue;
            const auto* particle = find(std::to_string(emitter->emitter.particleRefs.front()));
            if (!particle || !particle->hasParticle) continue;
            Point3F axis{0, 0, 1};
            sscanf(prop("emitterAxis").c_str(), "%f %f %f", &axis.x, &axis.y, &axis.z);
            Point3F up = Math::torquePointToYUp(axis);
            const float length = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
            if (length > 0.0001f) up = {up.x / length, up.y / length, up.z / length};
            w->addSceneEmitter(Math::torquePointToYUp({g->position.x, g->position.y, g->position.z}), up,
                               emitter->emitter, particle->particle);
        }
    }
}

bool Game::tryLoadDemoMission(const std::string& mission, bool resetParserState) {
    if (mission.empty() || mission == demoMissionState.loadedMission) {
        if (mission == demoMissionState.loadedMission)
            demoMissionState.pendingMission.clear();
        return true;
    }
    if (!w) {
        Console::instance().printf(LogLevel::Warn,
            "Demo mission deferred: no world is available for '%s'", mission.c_str());
        demoMissionState.defer(mission);
        return false;
    }

    // The new mission's world is its scene ghosts, once EndGhosting has
    // cleared the old ones and its TerrainBlock is in. Until then the current
    // scene, camera, ghosts, HUD, audio and effects stay for the next retry.
    std::vector<MisObject> scene;
    if (demoParser && demoParser->getGhostResets() != demoWorldGhostResets)
        scene = demoSceneObjects();
    w->sceneDataBlocks = demoParser ? &demoParser->getInitialBlock().dataBlocks : nullptr;
    const bool sceneLoaded = !scene.empty() &&
        w->loadObjects(missionLoadPath(mission).c_str(), "demo scene ghosts", std::move(scene));
    w->sceneDataBlocks = nullptr;
    if (!sceneLoaded) {
        if (demoMissionState.pendingMission != mission)
            Console::instance().printf(LogLevel::Warn,
                "Demo mission unavailable; deferring replacement: %s", mission.c_str());
        demoMissionState.defer(mission);
        return false;
    }

    // commit() only accepts the pending replacement; a first-attempt load
    // has not been deferred yet.
    demoMissionState.defer(mission);
    demoMissionState.commit(mission);
    demoWorldGhostResets = demoParser ? demoParser->getGhostResets() : -1;
    // A seek has already rebuilt parser state for the new mission.
    if (resetParserState) demoParser->resetMissionState();
    clearMissionAudio();
    Engine::instance().audio().stopAll();
    clearProjectileAudio();
    w->clearEffects();
    targetFinderShown = false;
    damageFlash = -1.0f;
    whiteOut = -1.0f;
    auto& missionGui = Engine::instance().guiRenderer();
    missionGui.clearDialogs();
    missionGui.setContent("PlayGui");
    resetGameplayGui(missionGui);
    if (hud) hud->resetState();
    applyDemoSceneEffects();
    Console::instance().printf(LogLevel::Info,
        "Demo mission replacement loaded: %s", mission.c_str());
    return true;
}

void Game::setDemoMatchEnded(bool ended) {
    if (ended == demoMatchEnded) return;
    demoMatchEnded = ended;
    if (!ended) {
        demoEndedGhosts.clear();
        return;
    }
    demoMatchEndedAt = demoTime;
    // Keep the final world while the parser keeps receiving the debrief.
    if (demoParser) demoEndedGhosts = demoParser->getGhostTracker();
    // Looping shape sounds and projectile audio stop with the world.
    auto& audio = Engine::instance().audio();
    for (auto& [key, source] : shapeBaseSoundSources) audio.releaseSource(source);
    shapeBaseSoundSources.clear();
    for (auto& [ghost, source] : demoJetSoundSources) audio.releaseSource(source);
    demoJetSoundSources.clear();
    clearProjectileAudio();
    Console::instance().printf(LogLevel::Info, "Demo: match ended at %.1f s", demoTime);
}

void Game::resetDemoPresentation() {
    setDemoMatchEnded(false);
    resetDemoEvents();
    resetDemoHud();
    resetDemoCamera();
    resetDemoEffects();
}

void Game::disconnectedCleanup() {
    if (auto* ts = Engine::instance().script().ts(); ts && ts->hasFunction("DisconnectedCleanup"))
        ts->callFunction("DisconnectedCleanup", {});
    ScriptEngine::instance().cancelMissionEvents();
    if (activeConn) activeConn->disconnect();
    auto& audio = Engine::instance().audio();
    clearVoicePlaybacks();
    if (w) w->cleanupMission();
    audio.stopAll();
    clearProjectileAudio();
    clearMissionAudio();
    liveGhosts.clear();
    nativeDatablockShapes.clear();
    nativeDatablocks.clear();
    liveTargets.clear();
    liveTargetVisible.fill(0);
    liveMissionCrc = 0;
    liveTeamScores.clear();
    liveMatchStarted_ = false;
    liveMatchEnded_ = false;
    liveMissionDisplayName_.clear();
    liveMissionType_.clear();
    liveLoadInfoLines_.clear();
    liveSpectateInit = false;
    spectateGhostIndex = -1;
    liveFollowGhostIndex = -1;
    liveFollowCenterInit = false;
    targetFinderShown = false;
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
    ScriptEngine::instance().cancelMissionEvents();
    resetInputState();
    auto& audio = Engine::instance().audio();
    audio.stopAll();
    clearProjectileAudio();
    clearMissionAudio();
    liveGhosts.clear();
    nativeDatablockShapes.clear();
    nativeDatablocks.clear();
    liveTargets.clear();
    liveTargetVisible.fill(0);
    liveMissionCrc = 0;
    liveTeamScores.clear();
    liveMatchStarted_ = false;
    liveMatchEnded_ = false;
    liveMissionDisplayName_.clear();
    liveMissionType_.clear();
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
    gamePaused = false;
    targetFinderShown = false;
    serverPlayerGhostIndex = 0;
    serverPlayerGhostSynced = false;
    damageFlash = -1.0f;
    whiteOut = -1.0f;
    if (w) w->clearEffects();
    if (w) w->resetTriggerTracking();
    if (hud) hud->resetState();
    auto& gui = Engine::instance().guiRenderer();
    gui.clearDialogs();
    gui.setContent("PlayGui");
    resetGameplayGui(gui);
}

void Game::toggleTargetFinder() {
    const bool available = demoPlaying || (activeConn && activeConn->isConnected());
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
    } else if (activeConn && liveGhosts.getGhost(ghostIndex)) {
        spectateGhostIndex = ghostIndex;
        liveFollowGhostIndex = -1;
        liveFollowCenterInit = false;
    }
}

void Game::applyInput(const InputMove& input) {
    if (demoLive)
        LiveMovePolicy::accumulateLook(liveLookDelta.x, liveLookDelta.y,
                                       input.lookDelta.x, input.lookDelta.y);
    previousFire = currentInput.fire;
    previousAltFire = currentInput.altFire;
    previousReload = currentInput.reload;
    currentInput = input;

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
    const bool replayObserver = ObserverParity::shouldCycleReplayTargets(demoPlaying, demoLive);
    const bool liveObserver = ObserverParity::shouldCycleLiveTargets(
        demoLive, gameState == Dead, activeConn != nullptr);
    if (replayObserver && observerCycle && !previousObserverCycle) {
        if (demoParser) {
            auto indices = demoParser->getGhostTracker().getAllIndices();
            // Match the dead-state target filter so vehicles are reachable by
            // the cycle control as well as the observer camera fallback.
            std::vector<int> targets;
            for (int i : indices) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(i);
                 if (g && ObserverParity::isReadySpectatableTarget(
                     g->className, g->damageState, g->hasPosition))
                    targets.push_back(i);
            }
            if (!targets.empty()) {
                // Find current spectate index (or control index) in target list
                int current = (spectateGhostIndex >= 0) ? spectateGhostIndex : controlGhostIndex;
                auto it = std::find(targets.begin(), targets.end(), current);
                if (it != targets.end() && ++it != targets.end())
                    spectateGhostIndex = *it;
                else
                    spectateGhostIndex = targets[0];
                Console::instance().printf(LogLevel::Info, "Spectating ghost %d", spectateGhostIndex);
            }
        }
    } else if (liveObserver && observerCycle && !previousObserverCycle) {
        const auto observer = activeConn->observerSnapshot();
        const auto indices = liveGhosts.getAllIndices();
        std::vector<int> targets;
        for (int index : indices) {
            const GhostEntry* g = liveGhosts.getGhost(index);
             if (!g || !ObserverParity::isReadySpectatableTarget(
                 g->className, g->damageState, g->hasPosition,
                 isSensorGroupTargetVisible(observer.playerSensorGroup, g->sensorGroup))) continue;
            targets.push_back(index);
        }
        if (!targets.empty()) {
            auto it = std::find(targets.begin(), targets.end(), spectateGhostIndex);
            spectateGhostIndex = it != targets.end() && ++it != targets.end() ? *it : targets.front();
            liveFollowGhostIndex = -1;
            liveFollowCenterInit = false;
        }
    }
    previousObserverCycle = observerCycle;
}

void Game::resetInputState() {
    currentInput = {};
    previousFire = false;
    previousAltFire = false;
    previousReload = false;
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

    shapeViewerShape.destroy();
    shapeViewerShape = DTSShape{};
    shapeViewerShape.name = path;
    if (!shapeViewerShape.load(data.data(), data.size())) {
        Console::instance().printf(LogLevel::Warn, "Shape Viewer: failed to load '%s'", path.c_str());
        return;
    }

    {
        const auto sequences = scriptShapeSequences(path);
        if (!sequences.empty()) importShapeSequences(shapeViewerShape, path, sequences);
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
