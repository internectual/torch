// Engine console functions the retail server scripts call that belong to
// the simulation/server layer rather than the client.
#include "sim/sim_natives.h"
#include "sim/camera.h"
#include "sim/player.h"
#include "sim/torque_math.h"
#include "sim/engine_crc.h"
#include "sim/sim_state.h"
#include "core/engine.h"
#include "script/torquescript.h"
#include "script/script_engine.h"
#include <map>
#include <cstdio>
#include <cmath>
#include "sim/shape_base.h"
#include "sim/game_connection.h"
#include "sim/net_object.h"
#include "sim/net_string_table.h"

void registerSimNatives(TorqueScript& ts) {
    registerShapeBaseNatives(ts);
    registerCameraNatives(ts);
    registerPlayerNatives(ts);
    registerGameConnectionNatives(ts);
    registerSceneObjectClasses();

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
}
