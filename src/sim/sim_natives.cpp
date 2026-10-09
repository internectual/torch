// Engine console functions the retail server scripts call that belong to
// the simulation/server layer rather than the client.
#include "sim/sim_natives.h"
#include "sim/camera.h"
#include "sim/player.h"
#include "sim/static_shapes.h"
#include "sim/weather.h"
#include "sim/client_targets.h"
#include "sim/trigger.h"
#include "sim/vehicle.h"
#include "sim/force_field.h"
#include "sim/nav_graph.h"
#include "ai/ai_connection.h"
#include "sim/path_manager.h"
#include "sim/projectiles.h"
#include "sim/target_manager.h"
#include "sim/server_container.h"
#include "sim/engine_classes.h"
#include "sim/torque_math.h"
#include "sim/engine_crc.h"
#include "sim/sim_state.h"
#include "core/engine.h"
#include "core/timer.h"
#include "script/torquescript.h"
#include "script/script_engine.h"
#include <map>
#include <strings.h>
#include <cstdio>
#include <cmath>
#include "sim/shape_base.h"
#include "sim/game_connection.h"
#include "sim/net_interface.h"
#include "sim/net_object.h"
#include "sim/net_string_table.h"

void registerSimNatives(TorqueScript& ts) {
    registerShapeBaseNatives(ts);
    registerCameraNatives(ts);
    registerPlayerNatives(ts);
    registerStaticShapeNatives(ts);
    registerLightningNatives(ts);
    registerPrecipitationNatives(ts);
    registerClientTargetNatives(ts);
    registerTriggerNatives(ts);
    registerVehicleNatives(ts);
    registerForceFieldNatives(ts);
    registerNavGraphNatives(ts);
    registerAINatives(ts);
    registerPathNatives(ts);
    registerTargetManagerNatives(ts);
    // The server's own static geometry and water (gServerContainer).
    serverCollision().triangles = ServerContainer::gatherTriangles;
    serverCollision().geometry = [](const Point3F& min, const Point3F& max, bool terrain, bool interiors,
                                    std::vector<PlayerPrediction::Triangle>& out,
                                    std::vector<const ScriptObject*>* owners) {
        ServerContainer::gatherGeometry(min, max, terrain, interiors, out, owners);
    };
    serverCollision().water = ServerContainer::waterSurfaceAt;
    registerContainerNatives(ts);
    registerProjectileNatives(ts);
    // ai/aiConsole.cc AISystemEnabled([bool]): no argument enables it.
    ts.registerNative("AISystemEnabled", [](const std::vector<VMValue>& args) -> VMValue {
        bool status = true;
        if (!args.empty()) {
            const std::string v = args[0].toString();
            status = strcasecmp(v.c_str(), "true") == 0 || std::atoi(v.c_str()) != 0;
        }
        SimState::server().aiSystemEnabled = status;
        return VMValue("");
    });

    // simBase.cc consoleInit: the object type masks (game/objectTypes.h).
    static const std::pair<const char*, int> typeMasks[] = {
        {"StaticObjectType", 1 << 0}, {"EnvironmentObjectType", 1 << 1}, {"TerrainObjectType", 1 << 2},
        {"InteriorObjectType", 1 << 3}, {"WaterObjectType", 1 << 4}, {"TriggerObjectType", 1 << 5},
        {"MarkerObjectType", 1 << 6}, {"ForceFieldObjectType", 1 << 8}, {"GameBaseObjectType", 1 << 10},
        {"ShapeBaseObjectType", 1 << 11}, {"CameraObjectType", 1 << 12}, {"StaticShapeObjectType", 1 << 13},
        {"PlayerObjectType", 1 << 14}, {"ItemObjectType", 1 << 15}, {"VehicleObjectType", 1 << 16},
        {"VehicleBlockerObjectType", 1 << 17}, {"ProjectileObjectType", 1 << 18},
        {"ExplosionObjectType", 1 << 19}, {"CorpseObjectType", 1 << 20}, {"TurretObjectType", 1 << 21},
        {"DebrisObjectType", 1 << 22}, {"PhysicalZoneObjectType", 1 << 23}, {"StaticTSObjectType", 1 << 24},
        {"GuiControlObjectType", 1 << 25}, {"StaticRenderedObjectType", 1 << 26},
        {"DamagableItemObjectType", 1 << 27}, {"SensorObjectType", 1 << 28}, {"StationObjectType", 1 << 29},
        {"GeneratorObjectType", 1 << 30}};
    for (const auto& [name, value] : typeMasks) ts.setGlobal(std::string("$TypeMasks::") + name, VMValue(value));
    // SimObject::mTypeMask as each class's constructor sets it.
    static auto typeMaskOf = [](const std::string& cls) {
        int mask = 0;
        auto is = [&](const char* base) { return EngineClasses::isA(cls, base); };
        if (is("GameBase")) mask |= 1 << 10;
        if (is("ShapeBase")) mask |= 1 << 11;
        if (is("Camera")) mask |= 1 << 12;
        if (is("StaticShape")) mask |= 1 << 13;
        if (is("Player")) mask |= 1 << 14;
        if (is("Item")) mask |= 1 << 15;
        if (is("Vehicle")) mask |= 1 << 16;
        if (is("Turret")) mask |= 1 << 21;
        if (is("TerrainBlock")) mask |= (1 << 2) | 1;
        if (is("InteriorInstance")) mask |= (1 << 3) | 1;
        if (is("WaterBlock")) mask |= 1 << 4;
        if (is("Trigger")) mask |= 1 << 5;
        if (is("ForceFieldBare")) mask |= 1 << 8;
        if (is("TSStatic")) mask |= (1 << 24) | 1;
        if (is("MissionMarker")) mask |= 1 << 6;
        return mask;
    };
    ts.registerNative("SceneObject::getType", [](const std::vector<VMValue>& args) -> VMValue {
        ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
        return VMValue(object ? typeMaskOf(object->className) : 0);
    });

    // math/mathTypes.cc getRandom / setRandomSeed / getRandomSeed on gRandGen.
    ts.registerNative("getRandom", [](const std::vector<VMValue>& args) -> VMValue {
        auto& rng = Nav::gRandGen();
        if (args.size() == 1) return VMValue((float)rng.randI(0, args[0].toInt()));
        if (args.size() == 2) {
            int32_t lo = args[0].toInt(), hi = args[1].toInt();
            if (lo > hi) std::swap(lo, hi);
            return VMValue((float)rng.randI(lo, hi));
        }
        return VMValue(rng.randF());
    });
    ts.registerNative("setRandomSeed", [](const std::vector<VMValue>& args) -> VMValue {
        Nav::setGlobalRandSeed(args.empty() ? (uint32_t)(Timer::now() * 1000.0) : (uint32_t)args[0].toInt());
        return VMValue("");
    });
    ts.registerNative("getRandomSeed", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(Nav::gRandGen().getSeed());
    });

    // gServerContainer.castRay over the server's collision geometry
    // (terrain, interiors, force fields).
    Nav::setRayCaster([](const Nav::Point3& a, const Nav::Point3& b, uint32_t, Nav::RayHit& hit) {
        const auto& world = serverCollision();
        if (!world.triangles) return false;
        std::vector<PlayerPrediction::Triangle> tris;
        const Point3F lo{std::min(a.x, b.x) - 0.1f, std::min(a.y, b.y) - 0.1f, std::min(a.z, b.z) - 0.1f};
        const Point3F hi{std::max(a.x, b.x) + 0.1f, std::max(a.y, b.y) + 0.1f, std::max(a.z, b.z) + 0.1f};
        world.triangles(lo, hi, tris);
        ForceFields::gather(nullptr, lo, hi, tris);
        const Point3F d{b.x - a.x, b.y - a.y, b.z - a.z};
        float best = 2.0f;
        for (const auto& t : tris) {
            // Moller-Trumbore.
            const Point3F e1 = PlayerPrediction::sub(t.b, t.a), e2 = PlayerPrediction::sub(t.c, t.a);
            const Point3F p = PlayerPrediction::cross(d, e2);
            const float det = PlayerPrediction::dot(e1, p);
            if (std::fabs(det) < 1e-9f) continue;
            const Point3F s{a.x - t.a.x, a.y - t.a.y, a.z - t.a.z};
            const float u = PlayerPrediction::dot(s, p) / det;
            if (u < 0 || u > 1) continue;
            const Point3F q = PlayerPrediction::cross(s, e1);
            const float v = PlayerPrediction::dot(d, q) / det;
            if (v < 0 || u + v > 1) continue;
            const float tt = PlayerPrediction::dot(e2, q) / det;
            if (tt < 0 || tt > 1 || tt >= best) continue;
            best = tt;
            hit.point = {a.x + d.x * tt, a.y + d.y * tt, a.z + d.z * tt};
            hit.normal = {t.n.x, t.n.y, t.n.z};
        }
        return best <= 1.0f;
    });

    // ContainerBoxEmpty(mask, center, xRad [, yRad, zRad]): nothing of the
    // masked types in the box (geometry by its triangles; objects by their
    // box around the origin: a player's PlayerData boxSize, else a point).
    ts.registerNative("ContainerBoxEmpty", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.size() < 3) return VMValue(1);
        const int mask = args[0].toInt();
        float c[3] = {0, 0, 0};
        std::sscanf(args[1].toString().c_str(), "%f %f %f", &c[0], &c[1], &c[2]);
        const float rx = args[2].toFloat();
        const float ry = args.size() > 3 ? args[3].toFloat() : rx;
        const float rz = args.size() > 4 ? args[4].toFloat() : rx;
        const Point3F lo{c[0] - rx, c[1] - ry, c[2] - rz}, hi{c[0] + rx, c[1] + ry, c[2] + rz};
        constexpr int geometry = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 8);
        if ((mask & geometry) && serverCollision().triangles) {
            std::vector<PlayerPrediction::Triangle> tris;
            serverCollision().triangles(lo, hi, tris);
            if (mask & (1 << 8)) ForceFields::gather(nullptr, lo, hi, tris);
            const PlayerPrediction::Box box{lo, hi};
            for (const auto& t : tris)
                if (PlayerPrediction::boxIntersectsTriangle(box, t)) return VMValue(0);
        }
        for (auto& [name, object] : ScriptEngine::instance().objects) {
            auto* scene = object ? dynamic_cast<SceneObject*>(object->engine.get()) : nullptr;
            if (!scene || !(typeMaskOf(object->className) & mask & ~geometry)) continue;
            float half[3] = {0, 0, 0}, height = 0;
            if (auto* player = dynamic_cast<PlayerObject*>(scene)) {
                (void)player;
                const auto size = Fields::point(ScriptEngine::instance().findObject(
                    ScriptEngine::instance().objectDataBlock(object).c_str()), "boxSize", {1, 1, 2});
                half[0] = size[0] * 0.5f; half[1] = size[1] * 0.5f; height = size[2];
            }
            const float x = scene->transform[3], y = scene->transform[7], z = scene->transform[11];
            if (x + half[0] >= lo.x && x - half[0] <= hi.x && y + half[1] >= lo.y && y - half[1] <= hi.y &&
                z + height >= lo.z && z <= hi.z)
                return VMValue(0);
        }
        return VMValue(1);
    });
    registerGameConnectionNatives(ts);
    registerNetInterfaceNatives(ts);
    registerSceneObjectClasses();
    registerSceneObjectNatives(ts);

    // math/mathTypes.cc console functions.
    {
        using namespace TorqueMath;
        auto text = [](const std::vector<VMValue>& args, size_t i) {
            return i < args.size() ? args[i].toString() : std::string();
        };
        // "x y z ax ay az angle" as dSscanf leaves it: missing values are 0.
        auto transform = [](const std::string& t, float pos[3], AngAxis& aa) {
            pos[0] = pos[1] = pos[2] = 0;
            aa = {0, 0, 0, 0};
            std::sscanf(t.c_str(), "%g %g %g %g %g %g %g", &pos[0], &pos[1], &pos[2], &aa.x, &aa.y, &aa.z, &aa.angle);
        };
        auto format = [](std::initializer_list<float> values) {
            std::string out;
            char buffer[32];
            for (float v : values) {
                std::snprintf(buffer, sizeof(buffer), "%g", v);
                if (!out.empty()) out += ' ';
                out += buffer;
            }
            return VMValue(out);
        };
        ts.registerNative("VectorLen", [text, format](const std::vector<VMValue>& args) -> VMValue {
            float v[3] = {0, 0, 0};
            std::sscanf(text(args, 0).c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
            return format({std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])});
        });
        ts.registerNative("VectorOrthoBasis", [text, format](const std::vector<VMValue>& args) -> VMValue {
            AngAxis aa{0, 0, 0, 0};
            std::sscanf(text(args, 0).c_str(), "%f %f %f %f", &aa.x, &aa.y, &aa.z, &aa.angle);
            const Matrix m = matrix(quat(aa));
            return format({m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]});
        });
        ts.registerNative("MatrixCreate", [text, format](const std::vector<VMValue>& args) -> VMValue {
            float pos[3] = {0, 0, 0};
            AngAxis aa{0, 0, 0, 0};
            std::sscanf(text(args, 0).c_str(), "%g %g %g", &pos[0], &pos[1], &pos[2]);
            std::sscanf(text(args, 1).c_str(), "%g %g %g %g", &aa.x, &aa.y, &aa.z, &aa.angle);
            return format({pos[0], pos[1], pos[2], aa.x, aa.y, aa.z, aa.angle * 3.14159265358979323846f / 180.0f});
        });
        ts.registerNative("MatrixCreateFromEuler", [text](const std::vector<VMValue>& args) -> VMValue {
            float e[3] = {0, 0, 0};
            std::sscanf(text(args, 0).c_str(), "%g %g %g", &e[0], &e[1], &e[2]);
            const AngAxis aa = angAxis(quat(e[0], e[1], e[2]));
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "0 0 0 %g %g %g %g", aa.x, aa.y, aa.z, aa.angle);
            return VMValue(buffer);
        });
        ts.registerNative("MatrixMultiply", [text, transform, format](const std::vector<VMValue>& args) -> VMValue {
            float p1[3], p2[3];
            AngAxis a1, a2;
            transform(text(args, 0), p1, a1);
            transform(text(args, 1), p2, a2);
            const Matrix m = mul(matrix(p1, a1), matrix(p2, a2));
            AngAxis aa = angAxis(quat(m));
            const float len = std::sqrt(aa.x * aa.x + aa.y * aa.y + aa.z * aa.z);
            if (len > 0) { aa.x /= len; aa.y /= len; aa.z /= len; }
            return format({m[3], m[7], m[11], aa.x, aa.y, aa.z, aa.angle});
        });
        ts.registerNative("MatrixMulVector", [text, transform, format](const std::vector<VMValue>& args) -> VMValue {
            float p[3], v[3] = {0, 0, 0}, out[3];
            AngAxis a;
            transform(text(args, 0), p, a);
            std::sscanf(text(args, 1).c_str(), "%g %g %g", &v[0], &v[1], &v[2]);
            mulV(matrix(p, a), v, out);
            return format({out[0], out[1], out[2]});
        });
        ts.registerNative("MatrixMulPoint", [text, transform, format](const std::vector<VMValue>& args) -> VMValue {
            float p[3], v[3] = {0, 0, 0}, out[3];
            AngAxis a;
            transform(text(args, 0), p, a);
            std::sscanf(text(args, 1).c_str(), "%g %g %g", &v[0], &v[1], &v[2]);
            mulP(matrix(p, a), v, out);
            return format({out[0], out[1], out[2]});
        });
    }

    // game/net.cc and consoleFunctions.cc tagged strings.
    ts.registerNative("addTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        return VMValue(NetStrings::tag(NetStrings::add(args.empty() ? "" : args[0].toString())));
    });
    ts.registerNative("removeTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        if (!args.empty()) NetStrings::remove(NetStrings::tagId(args[0].toString()));
        return VMValue("");
    });
    // getTag('tag'): the tagged string's id (commanderMap.cs keys its
    // entry types by it).
    ts.registerNative("getTag", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty() || !NetStrings::isTag(args[0].toString())) return VMValue(0);
        return VMValue((int32_t)NetStrings::tagId(args[0].toString()));
    });
    ts.registerNative("getTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        const std::string* text = args.empty() ? nullptr : NetStrings::lookup(NetStrings::tagId(args[0].toString()));
        return VMValue(text ? *text : std::string());
    });
    ts.registerNative("detag", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string value = args[0].toString();
        if (!NetStrings::isTag(value)) return VMValue(value);
        const size_t space = value.find(' ');
        return VMValue(space == std::string::npos ? std::string() : value.substr(space + 1));
    });
    ts.registerNative("buildTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string* format = NetStrings::lookup(NetStrings::tagId(args[0].toString()));
        if (!format) return VMValue("");
        std::string out;
        for (size_t i = 0; i < format->size(); ++i) {
            if ((*format)[i] == '%' && i + 1 < format->size() && (*format)[i + 1] >= '1' && (*format)[i + 1] <= '9') {
                const size_t index = (size_t)((*format)[i + 1] - '0');
                if (index >= args.size()) break;
                out += args[index].toString();
                ++i;
                continue;
            }
            out += (*format)[i];
        }
        return VMValue(out.substr(0, 511));
    });
    // consoleFunctions.cc getFileCRC: -1 when the file is not found.
    ts.registerNative("getFileCRC", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue(-1);
        std::vector<uint8_t> data;
        if (!Engine::instance().fs().readFile(args[0].toString().c_str(), data)) return VMValue(-1);
        return VMValue((int32_t)EngineCrc::calculate(data.data(), data.size()));
    });
    ts.registerNative("isPackage", [&ts](const std::vector<VMValue>& args) -> VMValue {
        return VMValue(!args.empty() && ts.isPackage(args[0].toString()) ? 1 : 0);
    });
    ts.registerNative("allowConnections", [](const std::vector<VMValue>& args) -> VMValue {
        SimState::server().allowConnections = !args.empty() && args[0].toBool();
        return VMValue(1);
    });
    ts.registerNative("disableCyclingConnections", [](const std::vector<VMValue>& args) -> VMValue {
        SimState::server().cyclingConnectionsDisabled = !args.empty() && args[0].toBool();
        return VMValue(1);
    });
    ts.registerNative("startHeartbeat", [](const std::vector<VMValue>&) -> VMValue {
        SimState::server().heartbeat = true;
        return VMValue(1);
    });
    ts.registerNative("stopHeartbeat", [](const std::vector<VMValue>&) -> VMValue {
        SimState::server().heartbeat = false;
        return VMValue(1);
    });
    ts.registerNative("getGravity", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(SimState::server().gravity);
    });
    ts.registerNative("setGravity", [](const std::vector<VMValue>& args) -> VMValue {
        if (!args.empty()) SimState::server().gravity = args[0].toFloat();
        return VMValue(1);
    });
    // ResManager::purge frees unreferenced cached resources; Torch's
    // resource caches hold nothing the scripts can observe.
    ts.registerNative("deleteDataBlocks", [](const std::vector<VMValue>&) -> VMValue {
        ScriptEngine::instance().deleteDataBlocks();
        return VMValue("");
    });
    ts.registerNative("purgeResources", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(1);
    });
    // TextureManager holds keep a mission's textures resident between
    // missions; Torch's texture cache keeps none to release.
    ts.registerNative("clearTextureHolds", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(1);
    });
}
